/*
 * Running one QT4 test case (test_run.h). Imitates apps/sxml/command_xpath.c
 * (processor, resolver, external variables, evaluation through the public
 * API); the test's allocator is the shared test budget (budget.h).
 */
#include "test_run.h"

#include "assertion.h"
#include "budget.h"
#include "reason.h"
#include "resource_table.h"
#include "sandpit.h"
#include "suite_path.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/schema.h"
#include "sxml/serialize.h"
#include "sxml/string.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"
#include "sxml/xquery.h"

#include <stdalign.h>
#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_qt4_empty_text = {.bytes = NULL,
                                                   .byte_count = 0};

/* The URI scheme and authority of suite files: file:///SUITE-PATH. */
#define SXML_QT4_SUITE_URI_PREFIX "file:///"

/* The most optional features a test's processor names: every feature of
   sxml_all_optional_features and EXPath File for a sandpit (D200). */
#define SXML_QT4_FEATURE_CAPACITY ((size_t)16)

/* As sxml check and sxml xpath: entity expansion is bounded. */
enum { SXML_QT4_MAXIMUM_EXPANSION_BYTE_COUNT = 4 * 1024 * 1024 };

/* Most evaluation steps of one evaluation of a test case (D89): four times
   SXML_QT4_TEST_MAXIMUM_STEP_COUNT, as the XSLT driver allows (D166), since
   a query may read a whole large source quadratically (app-Demos itunes
   compares the keys of 6,597 tracks of a 10 MB library once per artist,
   about 287 million steps; D178). */
#define SXML_QT4_QUERY_MAXIMUM_STEP_COUNT ((uint64_t)4 * SXML_QT4_TEST_MAXIMUM_STEP_COUNT)

/* The scratch arena of one test: environment arrays and URIs. */
enum { SXML_QT4_SCRATCH_BLOCK_BYTE_COUNT = 8 * 1024 };

sxml_byte_view sxml_qt4_test_outcome_name(sxml_qt4_test_outcome outcome) {
    switch (outcome) {
    case SXML_QT4_TEST_OUTCOME_PASS:
        return SXML_TEXT("pass");
    case SXML_QT4_TEST_OUTCOME_WRONG_ERROR:
        return SXML_TEXT("wrong-error");
    case SXML_QT4_TEST_OUTCOME_FAIL:
        return SXML_TEXT("fail");
    case SXML_QT4_TEST_OUTCOME_ERROR:
        return SXML_TEXT("error");
    case SXML_QT4_TEST_OUTCOME_NOT_RUN:
        break;
    }
    return SXML_TEXT("not-run");
}

/* ------------------------------------------------------------ The context */

/* Everything one test holds while it runs. */
typedef struct sxml_qt4_test_context {
    const sxml_qt4_test_run_request *request;
    const sxml_qt4_environment *environment;
    sxml_allocator allocator;
    sxml_processor *processor;
    sxml_arena *scratch;
    sxml_namespace_binding *namespace_bindings;
    size_t namespace_binding_count;
    sxml_byte_view static_base_uri;
    /* The host's declarations: the variables without declared="true". */
    sxml_variable_declaration *declarations;
    size_t declaration_count;
    sxml_variable_binding *bindings;
    size_t variable_count;
    sxml_available_document *available_documents;
    size_t available_document_count;
    sxml_decimal_format *decimal_formats;
    size_t decimal_format_count;
    /* Empty for the codepoint collation. */
    sxml_byte_view default_collation_uri;
    /* What the resolver serves; its arrays are filled here. */
    sxml_qt4_resource_table *resource_table;
    /* The environment's schemas compiled into one schema, the in-scope
       schema of the static context and of source validation; NULL when
       the environment has none, or when they do not compile (a query that
       imports them then meets the error itself). */
    sxml_schema *schema;
    bool has_context_value;
    sxml_sequence context_value;
    /* Owned values: one per source (of the environment and of its
       collections) and parameter, two per collection, one per collection
       query and one more for a collection with queries, and the context
       item's; released at the end. */
    sxml_sequence *owned_values;
    size_t owned_value_count;
    /* The owned value of the first source bound to a role, from 1; 0 for
       none. The queries after the first of an XQuery Update test case
       read it as their context value when the environment has none
       (D187). */
    size_t first_source_number;
    /* The available documents fn:put stored so far (D205). */
    size_t put_document_count;
    /* What was being done when something failed, for the reason. */
    sxml_byte_view stage;
    sxml_byte_view stage_subject;
    sxml_error_record error_record;
} sxml_qt4_test_context;

/* Allocates count elements of element_byte_count bytes in the scratch
   arena; count 0 gives NULL. */
typedef struct sxml_qt4_array_request {
    size_t element_count;
    size_t element_byte_count;
    size_t alignment;
} sxml_qt4_array_request;

static sxml_status sxml_qt4_allocate_array(sxml_qt4_test_context *context,
                                           sxml_qt4_array_request request,
                                           void **memory) {
    *memory = NULL;
    if (request.element_count == 0) {
        return SXML_STATUS_OK;
    }
    size_t byte_count = 0;
    const sxml_status status = sxml_size_multiply(
        request.element_count, request.element_byte_count, &byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_arena_allocate(
        context->scratch,
        (sxml_allocation_request){.byte_count = byte_count,
                                  .alignment = request.alignment},
        memory);
}

/* file:///SUITE-PATH, in the scratch arena. */
static sxml_status sxml_qt4_suite_uri(sxml_qt4_test_context *context,
                                      sxml_byte_view suite_path,
                                      sxml_byte_view *uri) {
    return sxml_qt4_concatenate(
        context->scratch,
        (sxml_qt4_concatenation){.first = SXML_TEXT(SXML_QT4_SUITE_URI_PREFIX),
                                 .second = suite_path},
        uri);
}

/* True when reference starts with a URI scheme (RFC 3986, section 3.1). */
static bool sxml_qt4_has_scheme(sxml_byte_view reference) {
    for (size_t byte_index = 0; byte_index < reference.byte_count;
         byte_index += 1) {
        const uint8_t byte = reference.bytes[byte_index];
        if (byte == ':') {
            return byte_index > 0;
        }
        const bool is_letter =
            (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
        const bool is_other = (byte >= '0' && byte <= '9') || byte == '+' ||
                              byte == '-' || byte == '.';
        if (!is_letter && (byte_index == 0 || !is_other)) {
            return false;
        }
    }
    return false;
}

/* A URI of the environment as an absolute URI: a relative one is resolved
   against the test-set file (RFC 3986, section 5.2). */
static sxml_status sxml_qt4_absolute_uri(sxml_qt4_test_context *context,
                                         sxml_byte_view reference,
                                         sxml_byte_view *uri) {
    if (sxml_qt4_has_scheme(reference)) {
        *uri = reference;
        return SXML_STATUS_OK;
    }
    sxml_byte_view suite_path = sxml_qt4_empty_text;
    const sxml_status status = sxml_qt4_suite_path_resolve(
        &(sxml_qt4_suite_path_request){
            .containing_file = context->request->test_case->test_set_file_path,
            .reference = reference,
            .arena = context->scratch},
        &suite_path);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_qt4_suite_uri(context, suite_path, uri);
}

/* What is being done, for the reason when it fails. */
typedef struct sxml_qt4_stage {
    sxml_byte_view description;
    sxml_byte_view subject;
} sxml_qt4_stage;

static void sxml_qt4_set_stage(sxml_qt4_test_context *context,
                               sxml_qt4_stage stage) {
    context->stage = stage.description;
    context->stage_subject = stage.subject;
}

/* True when the run targets XQuery: the query is a main module, which
   can declare variables itself. */
static bool sxml_qt4_runs_xquery(const sxml_qt4_test_context *context) {
    return context->request->target.language == SXML_QT4_LANGUAGE_XQUERY;
}

/* Keeps value until the end of the test. */
static void sxml_qt4_own_value(sxml_qt4_test_context *context,
                               sxml_sequence value) {
    context->owned_values[context->owned_value_count] = value;
    context->owned_value_count += 1;
}

/* ------------------------------------------------------------ Environment */

static sxml_status sxml_qt4_bind_namespaces(sxml_qt4_test_context *context) {
    const sxml_qt4_namespace_declaration_view declarations =
        context->environment->namespace_declarations;
    void *memory = NULL;
    sxml_status status = sxml_qt4_allocate_array(
        context,
        (sxml_qt4_array_request){
            .element_count = declarations.namespace_declaration_count,
            .element_byte_count = sizeof(sxml_namespace_binding),
            .alignment = alignof(sxml_namespace_binding)},
        &memory);
    context->namespace_bindings = memory;
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(context->processor);
    for (size_t declaration_index = 0;
         status == SXML_STATUS_OK &&
         declaration_index < declarations.namespace_declaration_count;
         declaration_index += 1) {
        const sxml_qt4_namespace_declaration *declaration =
            &declarations.namespace_declarations[declaration_index];
        sxml_namespace_binding binding = {.namespace_prefix = {.index = 0},
                                          .namespace_uri = {.index = 0}};
        status = sxml_name_pool_intern_namespace_prefix(
            name_pool, declaration->namespace_prefix, &binding.namespace_prefix);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_intern_namespace_uri(
                name_pool, declaration->namespace_uri, &binding.namespace_uri);
        }
        if (status == SXML_STATUS_OK) {
            context->namespace_bindings[context->namespace_binding_count] =
                binding;
            context->namespace_binding_count += 1;
        }
    }
    return status;
}

static sxml_status sxml_qt4_set_static_base_uri(sxml_qt4_test_context *context) {
    switch (context->environment->static_base_uri_kind) {
    case SXML_QT4_STATIC_BASE_URI_ABSENT:
        context->static_base_uri = sxml_qt4_empty_text;
        return SXML_STATUS_OK;
    case SXML_QT4_STATIC_BASE_URI_GIVEN:
        context->static_base_uri = context->environment->static_base_uri;
        return SXML_STATUS_OK;
    case SXML_QT4_STATIC_BASE_URI_TEST_SET_FILE:
        break;
    }
    return sxml_qt4_suite_uri(context,
                              context->request->test_case->test_set_file_path,
                              &context->static_base_uri);
}

/* True when an XPath test asks for XPath 1.0 compatibility mode (the
   dependency feature xpath-1.0-compatibility, satisfied; D197):
   "if this dependency is not present then the test must NOT be run in
   1.0 compatibility mode" (dependency_meanings.xml). */
static bool sxml_qt4_asks_xpath_1_compatibility(const sxml_qt4_test_context *context) {
    if (sxml_qt4_runs_xquery(context)) {
        return false;
    }
    const sxml_qt4_dependency_view lists[2] = {
        context->request->test_case->test_set_dependencies,
        context->request->test_case->dependencies};
    for (size_t list_index = 0; list_index < 2; list_index += 1) {
        for (size_t dependency_index = 0;
             dependency_index < lists[list_index].dependency_count;
             dependency_index += 1) {
            const sxml_qt4_dependency *dependency =
                &lists[list_index].dependencies[dependency_index];
            if (dependency->requires_satisfaction &&
                sxml_byte_view_equals(dependency->dependency_type,
                                      SXML_TEXT("feature")) &&
                sxml_byte_view_equals(dependency->value,
                                      SXML_TEXT("xpath-1.0-compatibility"))) {
                return true;
            }
        }
    }
    return false;
}

/* The static context of the test, with the variables declared so far. */
static sxml_static_context_configuration
sxml_qt4_static_context(const sxml_qt4_test_context *context) {
    return (sxml_static_context_configuration){
        .has_xpath_1_compatibility_mode =
            sxml_qt4_asks_xpath_1_compatibility(context),
        .namespace_bindings = {.namespace_bindings = context->namespace_bindings,
                               .namespace_binding_count =
                                   context->namespace_binding_count},
        .static_base_uri = context->static_base_uri,
        .variable_declarations = {.variable_declarations = context->declarations,
                                  .variable_declaration_count =
                                      context->declaration_count},
        .decimal_formats = {.decimal_formats = context->decimal_formats,
                            .decimal_format_count =
                                context->decimal_format_count},
        .default_collation_uri = context->default_collation_uri,
        .collation_bindings = {.collation_bindings = NULL,
                               .collation_binding_count = 0},
        .in_scope_schemas = {.schemas = context->schema == NULL
                                            ? NULL
                                            : (const sxml_schema *const *)&context
                                                  ->schema,
                             .schema_count = context->schema == NULL ? 0 : 1}};
}

/* The dynamic context of the test, with what is bound so far. */
static sxml_dynamic_context_configuration
sxml_qt4_dynamic_context(const sxml_qt4_test_context *context) {
    sxml_dynamic_context_configuration dynamic_context;
    memset(&dynamic_context, 0, sizeof dynamic_context);
    dynamic_context.has_context_value = context->has_context_value;
    dynamic_context.context_value = context->context_value;
    dynamic_context.variable_bindings = (sxml_variable_binding_view){
        .variable_bindings = context->bindings,
        .variable_binding_count = context->variable_count};
    dynamic_context.available_documents = (sxml_available_document_view){
        .available_documents = context->available_documents,
        .available_document_count = context->available_document_count};
    return dynamic_context;
}

/* The expanded name of a variable written as a QName, its prefix bound by
   the environment's namespaces. Returns SXML_STATUS_MALFORMED_INPUT for an
   unknown prefix. */
static sxml_status sxml_qt4_variable_name(sxml_qt4_test_context *context,
                                          sxml_byte_view qualified_name,
                                          sxml_name_identifier *name) {
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(context->processor);
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    sxml_byte_view local_name = qualified_name;
    const uint8_t *colon =
        qualified_name.byte_count == 0
            ? NULL
            : memchr(qualified_name.bytes, ':', qualified_name.byte_count);
    if (colon != NULL) {
        const sxml_byte_view prefix = {
            .bytes = qualified_name.bytes,
            .byte_count = (size_t)(colon - qualified_name.bytes)};
        local_name = (sxml_byte_view){
            .bytes = colon + 1,
            .byte_count = qualified_name.byte_count - prefix.byte_count - 1};
        const sxml_qt4_namespace_declaration_view declarations =
            context->environment->namespace_declarations;
        bool is_found = false;
        for (size_t declaration_index = 0;
             declaration_index < declarations.namespace_declaration_count;
             declaration_index += 1) {
            const sxml_qt4_namespace_declaration *declaration =
                &declarations.namespace_declarations[declaration_index];
            if (sxml_byte_view_equals(declaration->namespace_prefix, prefix)) {
                const sxml_status status = sxml_name_pool_intern_namespace_uri(
                    name_pool, declaration->namespace_uri, &namespace_uri);
                if (status != SXML_STATUS_OK) {
                    return status;
                }
                is_found = true;
            }
        }
        if (!is_found) {
            return SXML_STATUS_MALFORMED_INPUT;
        }
    }
    return sxml_name_pool_intern_name(
        name_pool,
        &(sxml_name_request){.namespace_uri = namespace_uri,
                             .local_name = local_name},
        name);
}

/* Declares and binds one external variable. */
typedef struct sxml_qt4_variable {
    sxml_name_identifier variable_name;
    sxml_sequence_type declared_type;
    bool is_bound;
    sxml_sequence value;
    /* declared="true": the query's prolog declares it, so the host only
       binds it (XQuery 4.0, "Variable Declaration"; a host declaration of
       the same name would be XQST0049, D106). */
    bool is_declared_by_query;
} sxml_qt4_variable;

static void sxml_qt4_add_variable(sxml_qt4_test_context *context,
                                  sxml_qt4_variable variable) {
    const size_t variable_index = context->variable_count;
    if (!variable.is_declared_by_query) {
        context->declarations[context->declaration_count] =
            (sxml_variable_declaration){.variable_name = variable.variable_name,
                                        .declared_type = variable.declared_type};
        context->declaration_count += 1;
    }
    /* An unbound variable is bound to () so the arrays stay parallel; the
       XPath API raises XPDY0002 only for a missing binding, and QT4 never
       uses a param without a value in XPath. */
    context->bindings[variable_index] = (sxml_variable_binding){
        .variable_name = variable.variable_name,
        .value = variable.is_bound ? variable.value : sxml_sequence_empty()};
    context->variable_count += 1;
}

static const sxml_sequence_type sxml_qt4_any_sequence_type = {
    .item_type = {.index = SXML_TYPE_ITEM},
    .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};

/* A parsed source: its document node, owned by the test, and its
   document URI. */
typedef struct sxml_qt4_parsed_source {
    sxml_item document_node;
    sxml_byte_view document_uri;
} sxml_qt4_parsed_source;

/* Replaces *document_node, owned, by its validated copy when the source
   asks for strict or lax validation (catalog-schema.xsd, "validation";
   D174) against the environment's schema. */
static sxml_status sxml_qt4_validate_source(sxml_qt4_test_context *context,
                                            const sxml_qt4_source *source,
                                            sxml_item *document_node) {
    const bool is_strict =
        sxml_byte_view_equals(source->validation, SXML_TEXT("strict"));
    const bool is_lax =
        sxml_byte_view_equals(source->validation, SXML_TEXT("lax"));
    if (!is_strict && !is_lax) {
        return SXML_STATUS_OK;
    }
    if (context->schema == NULL) {
        sxml_item_release(*document_node);
        return SXML_STATUS_UNSUPPORTED_FEATURE;
    }
    sxml_item validated;
    memset(&validated, 0, sizeof validated);
    const sxml_status status = sxml_schema_validate_node(
        context->schema,
        &(sxml_schema_validation_request){
            .node = *document_node,
            .validation_mode = is_strict ? SXML_VALIDATION_MODE_STRICT
                                         : SXML_VALIDATION_MODE_LAX,
            .governing_type = {.index = SXML_TYPE_NONE},
            .report_sink = {.state = NULL, .operations = NULL},
            .error_record = &context->error_record},
        &validated);
    sxml_item_release(*document_node);
    if (status == SXML_STATUS_OK) {
        *document_node = validated;
    }
    return status;
}

/* Parses one source (document URI and base URI: its uri made absolute, or
   file:///SUITE-PATH), keeps its document node until the end of the test
   and makes it available by its uri, when it has one. */
static sxml_status sxml_qt4_parse_source(sxml_qt4_test_context *context,
                                         const sxml_qt4_source *source,
                                         sxml_qt4_parsed_source *parsed) {
    sxml_qt4_set_stage(context,
                       (sxml_qt4_stage){.description = SXML_TEXT("source"),
                                        .subject = source->file_path});
    sxml_byte_view document_uri = sxml_qt4_empty_text;
    sxml_status status =
        source->uri.byte_count != 0
            ? sxml_qt4_absolute_uri(context, source->uri, &document_uri)
            : sxml_qt4_suite_uri(context, source->file_path, &document_uri);
    sxml_byte_view contents = sxml_qt4_empty_text;
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_catalog_read_file(
            context->request->catalog,
            &(sxml_qt4_file_request){.suite_path = source->file_path,
                                     .allocator = context->allocator},
            &contents);
    }
    sxml_item document_node;
    memset(&document_node, 0, sizeof document_node);
    if (status == SXML_STATUS_OK) {
        context->error_record.diagnostic.description = sxml_qt4_empty_text;
        status = sxml_processor_parse_document(
            context->processor,
            &(sxml_xml_parse_request){.input = contents,
                                      .source_name = source->file_path,
                                      .document_uri = document_uri,
                                      .base_uri = document_uri,
                                      .encoding_name = sxml_qt4_empty_text,
                                      .diagnostic =
                                          &context->error_record.diagnostic,
                                      /* External DTDs and entities are
                                         suite files (D199, D210). */
                                      .entity_resolver = sxml_processor_entity_resolver(
                                          context->processor)},
            &document_node);
        sxml_allocator_release(context->allocator, (void *)contents.bytes);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_validate_source(context, source, &document_node);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_qt4_own_value(context, sxml_sequence_from_item(document_node));
    if (source->uri.byte_count != 0) {
        context->available_documents[context->available_document_count] =
            (sxml_available_document){.document_uri = document_uri,
                                      .document_node = document_node};
        context->available_document_count += 1;
    }
    *parsed = (sxml_qt4_parsed_source){.document_node = document_node,
                                       .document_uri = document_uri};
    return SXML_STATUS_OK;
}

/* Parses one source of the environment and binds it as its role says. */
static sxml_status sxml_qt4_load_source(sxml_qt4_test_context *context,
                                        const sxml_qt4_source *source) {
    sxml_qt4_parsed_source parsed;
    memset(&parsed, 0, sizeof parsed);
    sxml_status status = sxml_qt4_parse_source(context, source, &parsed);
    if ((status == SXML_STATUS_MALFORMED_INPUT || status == SXML_STATUS_INVALID_UTF8) &&
        source->role.byte_count == 0) {
        /* A source that is not well-formed and only has a uri is what the
           test reads with fn:doc, which then raises FODC0002 (fn-doc-27):
           it stays unavailable instead of failing the environment. */
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_sequence value = sxml_sequence_from_item(parsed.document_node);
    if (context->first_source_number == 0 && source->role.byte_count != 0) {
        /* sxml_qt4_parse_source owned it last. */
        context->first_source_number = context->owned_value_count;
    }
    if (sxml_byte_view_equals(source->role, SXML_TEXT("."))) {
        context->has_context_value = true;
        context->context_value = value;
    } else if (source->role.byte_count > 1 && source->role.bytes[0] == '$') {
        sxml_name_identifier name = {.index = 0};
        status = sxml_qt4_variable_name(
            context,
            (sxml_byte_view){.bytes = source->role.bytes + 1,
                             .byte_count = source->role.byte_count - 1},
            &name);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_qt4_add_variable(
            context, (sxml_qt4_variable){.variable_name = name,
                                         .declared_type =
                                             sxml_qt4_any_sequence_type,
                                         .is_bound = true,
                                         .value = value,
                                         .is_declared_by_query =
                                             source->is_declared &&
                                             sxml_qt4_runs_xquery(context)});
    }
    return SXML_STATUS_OK;
}

/* Adds the served form of each resource to served, from *served_count
   on. */
typedef struct sxml_qt4_served_resources {
    sxml_qt4_served_resource *served_resources;
    size_t served_resource_count;
} sxml_qt4_served_resources;

static sxml_status sxml_qt4_add_served_resources(
    sxml_qt4_test_context *context, sxml_qt4_resource_view resources,
    sxml_qt4_served_resources *served) {
    sxml_status status = SXML_STATUS_OK;
    /* The array is NULL only when there are no resources at all. */
    for (size_t resource_index = 0;
         status == SXML_STATUS_OK && served->served_resources != NULL &&
         resource_index < resources.resource_count;
         resource_index += 1) {
        const sxml_qt4_resource *resource =
            &resources.resources[resource_index];
        sxml_qt4_served_resource *entry =
            &served->served_resources[served->served_resource_count];
        *entry = (sxml_qt4_served_resource){.uri = sxml_qt4_empty_text,
                                            .file_path = resource->file_path,
                                            .encoding = resource->encoding};
        status = sxml_qt4_absolute_uri(context, resource->uri, &entry->uri);
        if (status == SXML_STATUS_OK) {
            served->served_resource_count += 1;
        }
    }
    return status;
}

/* Makes the served resources: each <resource> of the environment and of
   its collections under its absolute URI. */
static sxml_status sxml_qt4_serve_resources(sxml_qt4_test_context *context) {
    const sxml_qt4_environment *environment = context->environment;
    size_t resource_count = environment->resources.resource_count;
    for (size_t collection_index = 0;
         collection_index < environment->collections.collection_count;
         collection_index += 1) {
        resource_count += environment->collections.collections[collection_index]
                              .resources.resource_count;
    }
    void *memory = NULL;
    sxml_status status = sxml_qt4_allocate_array(
        context,
        (sxml_qt4_array_request){
            .element_count = resource_count,
            .element_byte_count = sizeof(sxml_qt4_served_resource),
            .alignment = alignof(sxml_qt4_served_resource)},
        &memory);
    sxml_qt4_served_resources served = {.served_resources = memory,
                                        .served_resource_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_add_served_resources(context, environment->resources,
                                               &served);
    }
    for (size_t collection_index = 0;
         status == SXML_STATUS_OK &&
         collection_index < environment->collections.collection_count;
         collection_index += 1) {
        status = sxml_qt4_add_served_resources(
            context,
            environment->collections.collections[collection_index].resources,
            &served);
    }
    if (status == SXML_STATUS_OK) {
        context->resource_table->served_resources =
            (sxml_qt4_served_resource_view){
                .served_resources = served.served_resources,
                .served_resource_count = served.served_resource_count};
    }
    return status;
}

/* An xs:anyURI item of uri, owned (F&O 4.0, "fn:uri-collection"). */
static sxml_status sxml_qt4_make_uri_item(sxml_qt4_test_context *context,
                                          sxml_byte_view uri,
                                          sxml_item *item) {
    sxml_string string = {.utf8 = sxml_qt4_empty_text,
                          .codepoint_count = 0,
                          .is_ascii = true};
    const sxml_status status = sxml_string_from_utf8(uri, &string);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_atomic_item_make(
        sxml_processor_get_item_heap(context->processor),
        &(sxml_atomic_item_request){
            .type = {.index = SXML_TYPE_XS_ANY_URI},
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = string}}},
        item);
}

static sxml_status sxml_qt4_evaluate_text(sxml_qt4_test_context *context,
                                          sxml_byte_view text,
                                          sxml_sequence *value);

/* Evaluates the <query> elements of a collection and writes, owned by the
   test, the concatenation of *items and their values to *items
   (catalog-schema.xsd, element "collection": "the collection contains the
   result of evaluating the query"). The queries are XPath expressions in
   the test's contexts as they are so far. */
static sxml_status
sxml_qt4_add_query_items(sxml_qt4_test_context *context,
                         const sxml_qt4_collection *collection,
                         sxml_sequence *items) {
    const size_t query_count = collection->queries.text_count;
    void *memory = NULL;
    sxml_status status = sxml_qt4_allocate_array(
        context,
        (sxml_qt4_array_request){.element_count = query_count + 1,
                                 .element_byte_count = sizeof(sxml_sequence),
                                 .alignment = alignof(sxml_sequence)},
        &memory);
    sxml_sequence *parts = memory;
    /* query_count + 1 elements, so parts is NULL only after a failure. */
    if (status == SXML_STATUS_OK && parts == NULL) {
        status = SXML_STATUS_OUT_OF_MEMORY;
    }
    if (status == SXML_STATUS_OK) {
        parts[0] = *items;
    }
    sxml_qt4_set_stage(context,
                       (sxml_qt4_stage){.description =
                                            SXML_TEXT("collection query"),
                                        .subject = collection->uri});
    for (size_t query_index = 0;
         status == SXML_STATUS_OK && query_index < query_count;
         query_index += 1) {
        parts[query_index + 1] = sxml_sequence_empty();
        status = sxml_qt4_evaluate_text(
            context, collection->queries.texts[query_index],
            &parts[query_index + 1]);
        if (status == SXML_STATUS_OK) {
            sxml_qt4_own_value(context, parts[query_index + 1]);
        }
    }
    sxml_sequence concatenation = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_concatenate(
            sxml_processor_get_item_heap(context->processor),
            (sxml_sequence_view){.sequences = parts,
                                 .sequence_count = query_count + 1},
            &concatenation);
    }
    if (status == SXML_STATUS_OK) {
        sxml_qt4_own_value(context, concatenation);
        *items = concatenation;
    }
    return status;
}

/* Parses the sources of one collection and makes its two sequences:
   the document nodes, and the URIs of the documents and then of the
   collection's resources (which the resource table serves by URI). */
static sxml_status
sxml_qt4_serve_collection(sxml_qt4_test_context *context,
                          const sxml_qt4_collection *collection,
                          sxml_qt4_served_collection *served) {
    const size_t source_count = collection->sources.source_count;
    const size_t resource_count = collection->resources.resource_count;
    const size_t uri_count = source_count + resource_count;
    void *memory = NULL;
    sxml_status status = sxml_qt4_allocate_array(
        context,
        (sxml_qt4_array_request){.element_count = source_count + uri_count,
                                 .element_byte_count = sizeof(sxml_item),
                                 .alignment = alignof(sxml_item)},
        &memory);
    sxml_item *documents = memory;
    sxml_item *uris = documents == NULL ? NULL : documents + source_count;
    size_t made_uri_count = 0;
    /* The array is NULL only when it has no elements. */
    for (size_t source_index = 0;
         status == SXML_STATUS_OK && documents != NULL &&
         source_index < source_count;
         source_index += 1) {
        sxml_qt4_parsed_source parsed;
        memset(&parsed, 0, sizeof parsed);
        status = sxml_qt4_parse_source(
            context, &collection->sources.sources[source_index], &parsed);
        if (status == SXML_STATUS_OK) {
            documents[source_index] = parsed.document_node;
            status = sxml_qt4_make_uri_item(context, parsed.document_uri,
                                            &uris[source_index]);
        }
        if (status == SXML_STATUS_OK) {
            made_uri_count += 1;
        }
    }
    sxml_qt4_set_stage(context,
                       (sxml_qt4_stage){.description = SXML_TEXT("collection"),
                                        .subject = collection->uri});
    for (size_t resource_index = 0;
         status == SXML_STATUS_OK && uris != NULL &&
         resource_index < resource_count;
         resource_index += 1) {
        sxml_byte_view resource_uri = sxml_qt4_empty_text;
        status = sxml_qt4_absolute_uri(
            context, collection->resources.resources[resource_index].uri,
            &resource_uri);
        if (status == SXML_STATUS_OK) {
            status = sxml_qt4_make_uri_item(context, resource_uri,
                                            &uris[made_uri_count]);
        }
        if (status == SXML_STATUS_OK) {
            made_uri_count += 1;
        }
    }
    sxml_qt4_set_stage(context,
                       (sxml_qt4_stage){.description = SXML_TEXT("collection"),
                                        .subject = collection->uri});
    sxml_item_heap *item_heap =
        sxml_processor_get_item_heap(context->processor);
    sxml_sequence items = sxml_sequence_empty();
    sxml_sequence uri_items = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_make_vector(
            item_heap,
            (sxml_item_view){.items = documents, .item_count = source_count},
            &items);
    }
    if (status == SXML_STATUS_OK) {
        sxml_qt4_own_value(context, items);
        status = sxml_sequence_make_vector(
            item_heap, (sxml_item_view){.items = uris, .item_count = uri_count},
            &uri_items);
    }
    if (status == SXML_STATUS_OK) {
        sxml_qt4_own_value(context, uri_items);
    }
    if (status == SXML_STATUS_OK && collection->queries.text_count != 0) {
        status = sxml_qt4_add_query_items(context, collection, &items);
    }
    /* The vector retains its items; the URI items made here are released
       so that the vector holds the only reference. */
    for (size_t uri_index = 0; uri_index < made_uri_count; uri_index += 1) {
        sxml_item_release(uris[uri_index]);
    }
    *served = (sxml_qt4_served_collection){.is_default =
                                               collection->uri.byte_count == 0,
                                           .uri = sxml_qt4_empty_text,
                                           .items = items,
                                           .uris = uri_items};
    if (status == SXML_STATUS_OK && !served->is_default) {
        status = sxml_qt4_absolute_uri(context, collection->uri, &served->uri);
    }
    return status;
}

/* Makes the served modules: each <module> of the test case with the URI
   of its file (resource_table.h). */
static sxml_status sxml_qt4_serve_modules(sxml_qt4_test_context *context) {
    const sxml_qt4_module_view modules = context->request->test_case->modules;
    void *memory = NULL;
    sxml_status status = sxml_qt4_allocate_array(
        context,
        (sxml_qt4_array_request){
            .element_count = modules.module_count,
            .element_byte_count = sizeof(sxml_qt4_served_module),
            .alignment = alignof(sxml_qt4_served_module)},
        &memory);
    sxml_qt4_served_module *served = memory;
    size_t served_count = 0;
    for (size_t module_index = 0;
         status == SXML_STATUS_OK && module_index < modules.module_count;
         module_index += 1) {
        const sxml_qt4_module *module = &modules.modules[module_index];
        served[module_index] =
            (sxml_qt4_served_module){.namespace_uri = module->uri,
                                     .location = module->location,
                                     .file_path = module->file_path,
                                     .file_uri = sxml_qt4_empty_text};
        status = sxml_qt4_suite_uri(context, module->file_path,
                                    &served[module_index].file_uri);
        if (status == SXML_STATUS_OK) {
            served_count += 1;
        }
    }
    context->resource_table->served_modules = (sxml_qt4_served_module_view){
        .served_modules = served, .served_module_count = served_count};
    return status;
}

static sxml_status sxml_qt4_serve_collections(sxml_qt4_test_context *context) {
    const sxml_qt4_collection_view collections =
        context->environment->collections;
    void *memory = NULL;
    sxml_status status = sxml_qt4_allocate_array(
        context,
        (sxml_qt4_array_request){
            .element_count = collections.collection_count,
            .element_byte_count = sizeof(sxml_qt4_served_collection),
            .alignment = alignof(sxml_qt4_served_collection)},
        &memory);
    sxml_qt4_served_collection *served = memory;
    size_t served_count = 0;
    for (size_t collection_index = 0;
         status == SXML_STATUS_OK &&
         collection_index < collections.collection_count;
         collection_index += 1) {
        status = sxml_qt4_serve_collection(
            context, &collections.collections[collection_index],
            &served[collection_index]);
        served_count += 1;
    }
    /* What was made is owned by the test, so a failure leaves nothing to
       release here. */
    context->resource_table->served_collections =
        (sxml_qt4_served_collection_view){.served_collections = served,
                                          .served_collection_count =
                                              status == SXML_STATUS_OK
                                                  ? served_count
                                                  : 0};
    return status;
}

/* Makes the served schemas and compiles them into context->schema: one
   RESOLVED document per <schema> with a file, the resolver serving it by
   its suite URI (resource_table.h). A compilation error leaves the schema
   out (see sxml_qt4_test_context). */
static sxml_status sxml_qt4_compile_schemas(sxml_qt4_test_context *context) {
    const sxml_qt4_schema_view schemas = context->environment->schemas;
    void *memory = NULL;
    sxml_status status = sxml_qt4_allocate_array(
        context,
        (sxml_qt4_array_request){
            .element_count = schemas.schema_count,
            .element_byte_count = sizeof(sxml_qt4_served_schema),
            .alignment = alignof(sxml_qt4_served_schema)},
        &memory);
    sxml_qt4_served_schema *served = memory;
    void *document_memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_allocate_array(
            context,
            (sxml_qt4_array_request){
                .element_count = schemas.schema_count,
                .element_byte_count = sizeof(sxml_schema_document),
                .alignment = alignof(sxml_schema_document)},
            &document_memory);
    }
    sxml_schema_document *documents = document_memory;
    size_t served_count = 0;
    for (size_t schema_index = 0;
         status == SXML_STATUS_OK && schema_index < schemas.schema_count;
         schema_index += 1) {
        const sxml_qt4_schema *schema = &schemas.schemas[schema_index];
        if (schema->file_path.byte_count == 0) {
            continue;
        }
        sxml_qt4_served_schema *entry = &served[served_count];
        *entry = (sxml_qt4_served_schema){.namespace_uri = schema->uri,
                                          .file_path = schema->file_path,
                                          .file_uri = sxml_qt4_empty_text};
        status = sxml_qt4_suite_uri(context, schema->file_path, &entry->file_uri);
        if (status == SXML_STATUS_OK) {
            documents[served_count] = (sxml_schema_document){
                .document_source = SXML_SCHEMA_DOCUMENT_SOURCE_RESOLVED,
                .document_uri = entry->file_uri,
                .checks_target_namespace = false};
            served_count += 1;
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    context->resource_table->served_schemas = (sxml_qt4_served_schema_view){
        .served_schemas = served, .served_schema_count = served_count};
    if (served_count == 0) {
        return SXML_STATUS_OK;
    }
    sxml_qt4_set_stage(context,
                       (sxml_qt4_stage){.description = SXML_TEXT("schema"),
                                        .subject = served[0].file_path});
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof error_record);
    status = sxml_schema_create(
        context->allocator,
        &(sxml_schema_configuration){
            .processor = context->processor,
            .schema_documents = {.schema_documents = documents,
                                 .schema_document_count = served_count},
            .base_uri = sxml_qt4_empty_text,
            .report_sink = {.state = NULL, .operations = NULL},
            .maximum_content_model_state_count = 0,
            .error_record = &error_record},
        &context->schema);
    sxml_item_heap_clear_raised_error(
        sxml_processor_get_item_heap(context->processor));
    if (status == SXML_STATUS_STATIC_ERROR) {
        context->schema = NULL;
        return SXML_STATUS_OK;
    }
    return status;
}

/* Makes the statically known decimal formats: names in the processor's
   name pool, properties as the environment wrote them (D104). */
static sxml_status
sxml_qt4_declare_decimal_formats(sxml_qt4_test_context *context) {
    const sxml_qt4_decimal_format_view formats =
        context->environment->decimal_formats;
    void *memory = NULL;
    sxml_status status = sxml_qt4_allocate_array(
        context,
        (sxml_qt4_array_request){.element_count = formats.decimal_format_count,
                                 .element_byte_count =
                                     sizeof(sxml_decimal_format),
                                 .alignment = alignof(sxml_decimal_format)},
        &memory);
    context->decimal_formats = memory;
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(context->processor);
    for (size_t format_index = 0;
         status == SXML_STATUS_OK && format_index < formats.decimal_format_count;
         format_index += 1) {
        const sxml_qt4_decimal_format *format =
            &formats.decimal_formats[format_index];
        sxml_qt4_set_stage(
            context, (sxml_qt4_stage){.description = SXML_TEXT("decimal-format"),
                                      .subject = format->name});
        sxml_name_identifier format_name = {.index = 0};
        if (format->local_name.byte_count != 0) {
            sxml_namespace_uri_identifier namespace_uri = {.index = 0};
            status = sxml_name_pool_intern_namespace_uri(
                name_pool, format->namespace_uri, &namespace_uri);
            if (status == SXML_STATUS_OK) {
                status = sxml_name_pool_intern_name(
                    name_pool,
                    &(sxml_name_request){.namespace_uri = namespace_uri,
                                         .local_name = format->local_name},
                    &format_name);
            }
        }
        if (status == SXML_STATUS_OK) {
            context->decimal_formats[format_index] = (sxml_decimal_format){
                .format_name = format_name,
                .decimal_format_properties = format->properties};
            context->decimal_format_count += 1;
        }
    }
    return status;
}

/* The default collation: the collation marked default="true", if any. */
static void sxml_qt4_set_default_collation(sxml_qt4_test_context *context) {
    const sxml_qt4_collation_view collations =
        context->environment->collations;
    for (size_t collation_index = 0;
         collation_index < collations.collation_count; collation_index += 1) {
        if (collations.collations[collation_index].is_default) {
            context->default_collation_uri =
                collations.collations[collation_index].uri;
        }
    }
}

/* Compiles and evaluates text in the test's contexts, as they are so far,
   writing the value, owned, to *value. */
static sxml_status sxml_qt4_evaluate_text(sxml_qt4_test_context *context,
                                          sxml_byte_view text,
                                          sxml_sequence *value) {
    sxml_xpath_expression *expression = NULL;
    sxml_status status = sxml_xpath_expression_create(
        context->allocator,
        &(sxml_xpath_expression_configuration){
            .processor = context->processor,
            .expression_text = text,
            .source_name = context->stage,
            .static_context = sxml_qt4_static_context(context),
            .error_record = &context->error_record},
        &expression);
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_expression_evaluate(
            expression,
            &(sxml_xpath_evaluation_request){
                .dynamic_context = sxml_qt4_dynamic_context(context),
                .error_record = &context->error_record},
            value);
    }
    sxml_xpath_expression_destroy(expression);
    return status;
}

/* The test's query compiled and evaluated: as sxml_qt4_evaluate_text for
   XPath; for XQuery, a main module through include/sxml/xquery.h, whose
   serialization parameters the serialization assertions use. */
typedef struct sxml_qt4_query_answer {
    sxml_sequence value;
    /* Its views belong to query: valid until it is destroyed. */
    sxml_serialization_parameters serialization_parameters;
    /* The compiled XQuery query, destroyed after the assertions; NULL for
       XPath. */
    sxml_xquery_query *query;
} sxml_qt4_query_answer;

/* ------------------------------------------------------------- Updates */

/* Whether value is the one node item at the root of tree. */
static bool sxml_qt4_is_root_of(sxml_sequence value, const sxml_tree *tree) {
    sxml_item item;
    memset(&item, 0, sizeof item);
    sxml_node_reference node;
    return sxml_sequence_get_item(value,
                                  (sxml_item_request){.item_index = 0,
                                                      .error_record = NULL},
                                  &item) == SXML_STATUS_OK &&
           sxml_item_get_node(item, &node) == SXML_STATUS_OK &&
           node.tree == tree && node.node_index.index == 0 &&
           node.namespace_number == 0;
}

/*
 * The update sink of the test's queries (include/sxml/xquery.h, D187): a
 * tree an updating query changed replaces the source it came from, in the
 * owned values, the variables, the context value and the available
 * documents, so the next query of the test case reads the new tree
 * (catalog-schema.xsd, mutable sources). A tree the test did not bind
 * (one the query built) is dropped.
 */
static sxml_status
sxml_qt4_receive_updated_tree(void *state,
                              const sxml_xquery_updated_tree *updated_tree) {
    sxml_qt4_test_context *context = state;
    sxml_node_reference original;
    if (sxml_item_get_node(updated_tree->original_root, &original) !=
        SXML_STATUS_OK) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    for (size_t value_index = 0; value_index < context->owned_value_count;
         value_index += 1) {
        sxml_sequence *owned = &context->owned_values[value_index];
        if (!sxml_qt4_is_root_of(*owned, original.tree)) {
            continue;
        }
        const sxml_sequence old_value = *owned;
        sxml_item_retain(updated_tree->updated_root);
        *owned = sxml_sequence_from_item(updated_tree->updated_root);
        for (size_t variable_index = 0;
             variable_index < context->variable_count; variable_index += 1) {
            if (sxml_qt4_is_root_of(context->bindings[variable_index].value,
                                    original.tree)) {
                context->bindings[variable_index].value = *owned;
            }
        }
        if (context->has_context_value &&
            sxml_qt4_is_root_of(context->context_value, original.tree)) {
            context->context_value = *owned;
        }
        for (size_t document_index = 0;
             document_index < context->available_document_count;
             document_index += 1) {
            sxml_available_document *document =
                &context->available_documents[document_index];
            if (sxml_qt4_is_root_of(sxml_sequence_from_item(document->document_node),
                                    original.tree)) {
                document->document_node = updated_tree->updated_root;
            }
        }
        sxml_sequence_release(old_value);
    }
    return SXML_STATUS_OK;
}

/* The most documents one test case may store with fn:put. */
#define SXML_QT4_PUT_DOCUMENT_CAPACITY ((size_t)8)

/* The document a put stores, as a file would hold it: node serialized
   (an element as a document's one child) and parsed again, with uri as
   its document URI. */
static sxml_status sxml_qt4_store_put_document(sxml_qt4_test_context *context,
                                               const sxml_xquery_put_document *put_document,
                                               sxml_item *document_node) {
    sxml_output_buffer *output_buffer = NULL;
    sxml_status status = sxml_output_buffer_create(
        context->allocator,
        &(sxml_output_buffer_configuration){.allocator = context->allocator,
                                            .maximum_byte_count = 0},
        &output_buffer);
    sxml_serialization_parameters parameters;
    memset(&parameters, 0, sizeof parameters);
    parameters.serialization_method = SXML_SERIALIZATION_METHOD_XML;
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(
            sxml_processor_get_item_heap(context->processor),
            &(sxml_serialization_request){.value = sxml_sequence_from_item(put_document->node),
                                          .serialization_parameters = parameters,
                                          .error_record = &context->error_record},
            sxml_output_buffer_as_output_stream(output_buffer));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_processor_parse_document(
            context->processor,
            &(sxml_xml_parse_request){.input = sxml_output_buffer_get_bytes(output_buffer),
                                      .source_name = put_document->uri,
                                      .document_uri = put_document->uri,
                                      .base_uri = put_document->uri,
                                      .encoding_name = sxml_qt4_empty_text,
                                      .diagnostic = NULL},
            document_node);
    }
    sxml_output_buffer_destroy(output_buffer);
    return status;
}

/*
 * The put half of the update sink (include/sxml/xquery.h, D205): what
 * fn:put stores becomes an available document at its URI for the rest of
 * the test case, so that a later fn:doc, in a query or an assertion,
 * reads it back (the catalog's fn:put tests). Nothing is written to a
 * file.
 */
static sxml_status
sxml_qt4_receive_put_document(void *state, const sxml_xquery_put_document *put_document) {
    sxml_qt4_test_context *context = state;
    if (context->put_document_count == SXML_QT4_PUT_DOCUMENT_CAPACITY) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    /* An absolute URI is never empty (include/sxml/xquery.h). */
    if (put_document->uri.byte_count == 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    void *memory = NULL;
    sxml_status status = sxml_qt4_allocate_array(
        context,
        (sxml_qt4_array_request){.element_count = put_document->uri.byte_count,
                                 .element_byte_count = 1,
                                 .alignment = 1},
        &memory);
    sxml_item document_node;
    memset(&document_node, 0, sizeof document_node);
    if (status == SXML_STATUS_OK && memory != NULL) {
        memcpy(memory, put_document->uri.bytes, put_document->uri.byte_count);
        status = sxml_qt4_store_put_document(context, put_document, &document_node);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_qt4_own_value(context, sxml_sequence_from_item(document_node));
    context->available_documents[context->available_document_count] = (sxml_available_document){
        .document_uri = {.bytes = memory, .byte_count = put_document->uri.byte_count},
        .document_node = document_node};
    context->available_document_count += 1;
    context->put_document_count += 1;
    return SXML_STATUS_OK;
}

static const sxml_xquery_update_sink_operations sxml_qt4_update_sink_operations = {
    .receive_updated_tree = sxml_qt4_receive_updated_tree,
    .receive_put_document = sxml_qt4_receive_put_document};

static sxml_status sxml_qt4_evaluate_main_query(sxml_qt4_test_context *context,
                                                sxml_byte_view text,
                                                sxml_qt4_query_answer *answer) {
    memset(&answer->serialization_parameters, 0,
           sizeof answer->serialization_parameters);
    answer->serialization_parameters.serialization_method =
        SXML_SERIALIZATION_METHOD_XML;
    if (!sxml_qt4_runs_xquery(context)) {
        return sxml_qt4_evaluate_text(context, text, &answer->value);
    }
    sxml_xquery_query *query = NULL;
    sxml_status status = sxml_xquery_query_create(
        context->allocator,
        &(sxml_xquery_query_configuration){
            .processor = context->processor,
            .expression_text = text,
            .source_name = context->stage,
            .static_context = sxml_qt4_static_context(context),
            .error_record = &context->error_record},
        &query);
    if (status == SXML_STATUS_OK) {
        answer->serialization_parameters =
            sxml_xquery_query_get_serialization_parameters(query);
        status = sxml_xquery_query_evaluate(
            query,
            &(sxml_xquery_evaluation_request){
                .dynamic_context = sxml_qt4_dynamic_context(context),
                .error_record = &context->error_record,
                .update_sink = {.state = context,
                                .operations = &sxml_qt4_update_sink_operations}},
            &answer->value);
    }
    answer->query = query;
    return status;
}

static sxml_status sxml_qt4_bind_parameter(sxml_qt4_test_context *context,
                                           const sxml_qt4_parameter *parameter) {
    sxml_qt4_set_stage(context,
                       (sxml_qt4_stage){.description = SXML_TEXT("param"),
                                        .subject = parameter->name});
    if (parameter->source.byte_count != 0) {
        return SXML_STATUS_UNSUPPORTED_FEATURE;
    }
    sxml_qt4_variable variable = {.variable_name = {.index = 0},
                                  .declared_type = sxml_qt4_any_sequence_type,
                                  .is_bound = false,
                                  .value = sxml_sequence_empty(),
                                  .is_declared_by_query =
                                      parameter->is_declared &&
                                      sxml_qt4_runs_xquery(context)};
    sxml_status status = SXML_STATUS_OK;
    if (parameter->has_name_namespace) {
        /* The prefix is the param element's (prod-VarDecl.external,
           extvardeclwithtype-24). */
        sxml_name_pool *name_pool = sxml_processor_get_name_pool(context->processor);
        const uint8_t *colon = memchr(parameter->name.bytes, ':', parameter->name.byte_count);
        const size_t prefix_byte_count = (size_t)(colon - parameter->name.bytes);
        sxml_namespace_uri_identifier namespace_uri = {.index = 0};
        status = sxml_name_pool_intern_namespace_uri(name_pool, parameter->name_namespace_uri,
                                                     &namespace_uri);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_intern_name(
                name_pool,
                &(sxml_name_request){
                    .namespace_uri = namespace_uri,
                    .local_name = {.bytes = colon + 1,
                                   .byte_count = parameter->name.byte_count - prefix_byte_count - 1}},
                &variable.variable_name);
        }
    } else {
        status = sxml_qt4_variable_name(context, parameter->name, &variable.variable_name);
    }
    if (status == SXML_STATUS_OK && parameter->declared_type.byte_count != 0) {
        status = sxml_xpath_parse_sequence_type(
            &(sxml_sequence_type_parse_request){
                .processor = context->processor,
                .sequence_type_text = parameter->declared_type,
                .source_name = SXML_TEXT("as"),
                .static_context = sxml_qt4_static_context(context),
                .error_record = &context->error_record},
            &variable.declared_type);
    }
    if (status == SXML_STATUS_OK && parameter->select.byte_count != 0) {
        status = sxml_qt4_evaluate_text(context, parameter->select,
                                        &variable.value);
        if (status == SXML_STATUS_OK) {
            sxml_qt4_own_value(context, variable.value);
            variable.is_bound = true;
        }
    }
    if (status == SXML_STATUS_OK) {
        sxml_qt4_add_variable(context, variable);
    }
    return status;
}

/* Sets up the environment of the test case. */
static sxml_status sxml_qt4_apply_environment(sxml_qt4_test_context *context) {
    const sxml_qt4_environment *environment = context->environment;
    const size_t source_count = environment->sources.source_count;
    const size_t parameter_count = environment->parameters.parameter_count;
    const size_t variable_capacity = source_count + parameter_count;
    size_t collection_source_count = 0;
    /* A collection with queries owns their values and the concatenation. */
    size_t collection_query_value_count = 0;
    for (size_t collection_index = 0;
         collection_index < environment->collections.collection_count;
         collection_index += 1) {
        const sxml_qt4_collection *collection =
            &environment->collections.collections[collection_index];
        collection_source_count += collection->sources.source_count;
        if (collection->queries.text_count != 0) {
            collection_query_value_count += collection->queries.text_count + 1;
        }
    }
    const size_t document_capacity =
        source_count + collection_source_count + SXML_QT4_PUT_DOCUMENT_CAPACITY;
    const size_t owned_capacity =
        variable_capacity + 1 + collection_source_count + SXML_QT4_PUT_DOCUMENT_CAPACITY +
        2 * environment->collections.collection_count +
        collection_query_value_count;
    sxml_qt4_set_stage(context,
                       (sxml_qt4_stage){.description = SXML_TEXT("environment"),
                                        .subject = environment->name});
    void *memory = NULL;
    sxml_status status = sxml_qt4_allocate_array(
        context,
        (sxml_qt4_array_request){.element_count = variable_capacity,
                                 .element_byte_count =
                                     sizeof(sxml_variable_declaration),
                                 .alignment = alignof(sxml_variable_declaration)},
        &memory);
    context->declarations = memory;
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_allocate_array(
            context,
            (sxml_qt4_array_request){
                .element_count = variable_capacity,
                .element_byte_count = sizeof(sxml_variable_binding),
                .alignment = alignof(sxml_variable_binding)},
            &memory);
        context->bindings = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_allocate_array(
            context,
            (sxml_qt4_array_request){
                .element_count = document_capacity,
                .element_byte_count = sizeof(sxml_available_document),
                .alignment = alignof(sxml_available_document)},
            &memory);
        context->available_documents = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_allocate_array(
            context,
            (sxml_qt4_array_request){.element_count = owned_capacity,
                                     .element_byte_count = sizeof(sxml_sequence),
                                     .alignment = alignof(sxml_sequence)},
            &memory);
        context->owned_values = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_bind_namespaces(context);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_set_static_base_uri(context);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_declare_decimal_formats(context);
    }
    sxml_qt4_set_default_collation(context);
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_serve_resources(context);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_serve_modules(context);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_compile_schemas(context);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_serve_collections(context);
    }
    for (size_t source_index = 0;
         status == SXML_STATUS_OK && source_index < source_count;
         source_index += 1) {
        status = sxml_qt4_load_source(
            context, &environment->sources.sources[source_index]);
    }
    for (size_t parameter_index = 0;
         status == SXML_STATUS_OK && parameter_index < parameter_count;
         parameter_index += 1) {
        status = sxml_qt4_bind_parameter(
            context, &environment->parameters.parameters[parameter_index]);
    }
    if (status == SXML_STATUS_OK && environment->has_context_item &&
        !context->has_context_value) {
        sxml_qt4_set_stage(
            context, (sxml_qt4_stage){.description = SXML_TEXT("context-item"),
                                      .subject = sxml_qt4_empty_text});
        sxml_sequence value = sxml_sequence_empty();
        status = sxml_qt4_evaluate_text(
            context, environment->context_item_select, &value);
        if (status == SXML_STATUS_OK) {
            sxml_qt4_own_value(context, value);
            context->has_context_value = true;
            context->context_value = value;
        }
    }
    return status;
}

/* ------------------------------------------------------------- The test */

/* What the test's own evaluation and its assertions gave. */
typedef struct sxml_qt4_test_answer {
    sxml_qt4_test_outcome outcome;
    sxml_qt4_reason reason;
    /* The query failed for something sxml does not have: an unknown
       function (XPST0017) or SXML_STATUS_UNSUPPORTED_FEATURE. */
    bool needs_missing_feature;
} sxml_qt4_test_answer;

/* Ends the reason with "STAGE[ SUBJECT]: ERROR". */
static void sxml_qt4_describe_failure(const sxml_qt4_test_context *context,
                                      sxml_status status,
                                      sxml_qt4_test_answer *answer) {
    answer->outcome = SXML_QT4_TEST_OUTCOME_ERROR;
    sxml_qt4_reason_append(&answer->reason, context->stage);
    if (context->stage_subject.byte_count != 0) {
        sxml_qt4_reason_append(&answer->reason, SXML_TEXT(" "));
        sxml_qt4_reason_append(&answer->reason, context->stage_subject);
    }
    sxml_qt4_reason_append(&answer->reason, SXML_TEXT(": "));
    sxml_qt4_reason_append_error(
        &answer->reason,
        (sxml_qt4_raised_error){.status = status,
                                .error_record = &context->error_record});
}

/* Checks the assertions against what the evaluation gave. */
static void sxml_qt4_judge(sxml_qt4_test_context *context,
                           sxml_qt4_evaluation_outcome evaluation,
                           sxml_qt4_test_answer *answer) {
    const sxml_qt4_test_run_request *request = context->request;
    sxml_qt4_assertion_verdict verdict = {
        .verdict_kind = SXML_QT4_VERDICT_FAIL, .reason = sxml_qt4_empty_text};
    sxml_qt4_set_stage(context,
                       (sxml_qt4_stage){.description = SXML_TEXT("assertion"),
                                        .subject = sxml_qt4_empty_text});
    if (!request->test_case->has_result) {
        answer->outcome = SXML_QT4_TEST_OUTCOME_FAIL;
        sxml_qt4_reason_append(&answer->reason, SXML_TEXT("no result element"));
        return;
    }
    memset(&context->error_record, 0, sizeof context->error_record);
    const sxml_status status = sxml_qt4_check_assertion(
        &(sxml_qt4_assertion_request){
            .processor = context->processor,
            .static_context = sxml_qt4_static_context(context),
            .document = sxml_qt4_catalog_get_test_set_document(request->catalog),
            .result_element = request->test_case->result,
            .evaluation = evaluation,
            .catalog = request->catalog,
            .test_set_file_path = request->test_case->test_set_file_path,
            .allocator = context->allocator,
            .arena = request->arena,
            .available_documents = {.available_documents = context->available_documents,
                                    .available_document_count =
                                        context->available_document_count}},
        &verdict);
    if (status != SXML_STATUS_OK) {
        sxml_qt4_describe_failure(context, status, answer);
        return;
    }
    switch (verdict.verdict_kind) {
    case SXML_QT4_VERDICT_PASS:
        answer->outcome = SXML_QT4_TEST_OUTCOME_PASS;
        break;
    case SXML_QT4_VERDICT_WRONG_ERROR:
        answer->outcome = SXML_QT4_TEST_OUTCOME_WRONG_ERROR;
        break;
    case SXML_QT4_VERDICT_UNSUPPORTED:
        answer->outcome = SXML_QT4_TEST_OUTCOME_NOT_RUN;
        break;
    case SXML_QT4_VERDICT_FAIL:
        answer->outcome = SXML_QT4_TEST_OUTCOME_FAIL;
        break;
    }
    sxml_qt4_reason_append(&answer->reason, verdict.reason);
}

/* Reads one query of the test case into *query_text; *query_contents is
   the memory to release (NULL for an inline query). */
static sxml_status sxml_qt4_read_query_text(sxml_qt4_test_context *context,
                                            const sxml_qt4_query *query,
                                            sxml_byte_view *query_text) {
    *query_text = query->query_text;
    if (query->query_file_path.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    sxml_qt4_set_stage(context,
                       (sxml_qt4_stage){.description = SXML_TEXT("query file"),
                                        .subject = query->query_file_path});
    return sxml_qt4_catalog_read_file(
        context->request->catalog,
        &(sxml_qt4_file_request){.suite_path = query->query_file_path,
                                 .allocator = context->allocator},
        query_text);
}

/* Runs the queries before the last of an XQuery Update test case, in
   order: each updating one's changed trees replace their sources (D187).
   Their values are dropped; the first failure is the test's outcome,
   left in *query_answer. Afterwards the first source is the context value
   when the environment gives none. */
static sxml_status
sxml_qt4_run_earlier_queries(sxml_qt4_test_context *context,
                             sxml_qt4_query_answer *query_answer) {
    const sxml_qt4_query_view queries =
        context->request->test_case->earlier_queries;
    sxml_status status = SXML_STATUS_OK;
    for (size_t query_index = 0;
         status == SXML_STATUS_OK && query_index < queries.query_count;
         query_index += 1) {
        sxml_byte_view query_text = sxml_qt4_empty_text;
        status = sxml_qt4_read_query_text(context, &queries.queries[query_index],
                                          &query_text);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_qt4_set_stage(context,
                           (sxml_qt4_stage){.description = SXML_TEXT("update query"),
                                            .subject = sxml_qt4_empty_text});
        memset(&context->error_record, 0, sizeof context->error_record);
        status = sxml_qt4_evaluate_main_query(context, query_text, query_answer);
        if (queries.queries[query_index].query_file_path.byte_count != 0) {
            sxml_allocator_release(context->allocator, (void *)query_text.bytes);
        }
        if (status == SXML_STATUS_OK) {
            sxml_sequence_release(query_answer->value);
            sxml_xquery_query_destroy(query_answer->query);
            *query_answer = (sxml_qt4_query_answer){.value = sxml_sequence_empty(),
                                                    .query = NULL};
        }
        if (status == SXML_STATUS_OK && !context->has_context_value &&
            context->first_source_number != 0) {
            context->has_context_value = true;
            context->context_value =
                context->owned_values[context->first_source_number - 1];
        }
    }
    return status;
}

/* Reads the query, evaluates it and judges the outcome. */
static void sxml_qt4_evaluate_query(sxml_qt4_test_context *context,
                                    sxml_qt4_test_answer *answer) {
    const sxml_qt4_test_case *test_case = context->request->test_case;
    sxml_qt4_query_answer query_answer = {.value = sxml_sequence_empty(), .query = NULL};
    sxml_status status = sxml_qt4_run_earlier_queries(context, &query_answer);
    const bool earlier_query_failed = status != SXML_STATUS_OK;
    sxml_byte_view query_text = sxml_qt4_empty_text;
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_read_query_text(
            context,
            &(sxml_qt4_query){.query_text = test_case->query_text,
                              .query_file_path = test_case->query_file_path},
            &query_text);
    }
    void *query_contents =
        test_case->query_file_path.byte_count != 0 && !earlier_query_failed
            ? (void *)query_text.bytes
            : NULL;
    if (status == SXML_STATUS_OK) {
        sxml_qt4_set_stage(context,
                           (sxml_qt4_stage){.description = SXML_TEXT("query"),
                                            .subject = sxml_qt4_empty_text});
        memset(&context->error_record, 0, sizeof context->error_record);
        status = sxml_qt4_evaluate_main_query(context, query_text, &query_answer);
    }
    const sxml_sequence value = query_answer.value;
    sxml_allocator_release(context->allocator, query_contents);
    answer->needs_missing_feature =
        status == SXML_STATUS_UNSUPPORTED_FEATURE ||
        (sxml_status_has_error_record(status) &&
         sxml_byte_view_equals(context->error_record.code.local_name, SXML_TEXT("XPST0017")));
    if (status != SXML_STATUS_OK && !sxml_status_has_error_record(status)) {
        sxml_xquery_query_destroy(query_answer.query);
        sxml_qt4_describe_failure(context, status, answer);
        return;
    }
    /* The error record of the query stays valid through the assertions:
       they raise errors on the same heap, so it is copied. */
    sxml_error_record raised = context->error_record;
    if (status != SXML_STATUS_OK) {
        sxml_byte_view *const kept_views[] = {
            &raised.code.namespace_uri, &raised.code.local_name,
            &raised.code.namespace_prefix, &raised.diagnostic.description};
        for (size_t view_index = 0;
             view_index < sizeof kept_views / sizeof kept_views[0];
             view_index += 1) {
            sxml_byte_view copy = sxml_qt4_empty_text;
            if (sxml_qt4_concatenate(
                    context->request->arena,
                    (sxml_qt4_concatenation){.first = *kept_views[view_index],
                                             .second = sxml_qt4_empty_text},
                    &copy) != SXML_STATUS_OK) {
                copy = sxml_qt4_empty_text;
            }
            *kept_views[view_index] = copy;
        }
        raised.value = NULL;
    }
    sxml_qt4_judge(context,
                   (sxml_qt4_evaluation_outcome){
                       .status = status,
                       .value = value,
                       .error_record = &raised,
                       .serialization_parameters =
                           query_answer.serialization_parameters},
                   answer);
    sxml_sequence_release(value);
    sxml_xquery_query_destroy(query_answer.query);
}

/* Releases everything the test holds; the budget then has nothing
   live. */
static void sxml_qt4_release_context(sxml_qt4_test_context *context) {
    for (size_t value_index = 0; value_index < context->owned_value_count;
         value_index += 1) {
        sxml_sequence_release(context->owned_values[value_index]);
    }
    context->owned_value_count = 0;
    if (context->processor != NULL) {
        sxml_item_heap_clear_raised_error(
            sxml_processor_get_item_heap(context->processor));
    }
    sxml_schema_destroy(context->schema);
    context->schema = NULL;
    sxml_processor_destroy(context->processor);
    sxml_arena_destroy(context->scratch);
}

/* Whether a dependency list asks for the feature expath-file. */
static bool sxml_qt4_lists_file_feature(sxml_qt4_dependency_view dependencies) {
    for (size_t dependency_index = 0; dependency_index < dependencies.dependency_count;
         dependency_index += 1) {
        const sxml_qt4_dependency *dependency = &dependencies.dependencies[dependency_index];
        if (dependency->requires_satisfaction &&
            sxml_byte_view_equals(dependency->dependency_type, SXML_TEXT("feature")) &&
            sxml_byte_view_equals(dependency->value, SXML_TEXT("expath-file"))) {
            return true;
        }
    }
    return false;
}

/* Runs an applicable test in its own processor and budget. */
static void sxml_qt4_run_isolated(const sxml_qt4_test_run_request *request,
                                  sxml_qt4_test_answer *answer) {
    sxml_qt4_budget budget = sxml_qt4_budget_start(request->allocator);
    const sxml_allocator allocator = sxml_qt4_budget_allocator(&budget);
    sxml_qt4_resource_table resource_table = {
        .catalog = request->catalog,
        .allocator = allocator,
        .served_resources = {.served_resources = NULL,
                             .served_resource_count = 0},
        .served_collections = {.served_collections = NULL,
                               .served_collection_count = 0},
        .served_modules = {.served_modules = NULL, .served_module_count = 0},
        .served_schemas = {.served_schemas = NULL, .served_schema_count = 0}};
    sxml_qt4_test_context context;
    memset(&context, 0, sizeof context);
    context.request = request;
    context.resource_table = &resource_table;
    context.environment = &request->test_case->environment;
    context.allocator = allocator;
    context.stage = SXML_TEXT("processor");
    /* A sandpit's test gets EXPath File over a copy of the sandpit; the
       others do not name the feature (D200). */
    sxml_qt4_sandpit sandpit;
    memset(&sandpit, 0, sizeof sandpit);
    const sxml_optional_feature_view all_features = sxml_all_optional_features();
    sxml_optional_feature features[SXML_QT4_FEATURE_CAPACITY];
    size_t feature_count = 0;
    for (size_t feature_index = 0;
         feature_index < all_features.optional_feature_count &&
         feature_count + 1 < SXML_QT4_FEATURE_CAPACITY;
         feature_index += 1) {
        features[feature_count] = all_features.optional_features[feature_index];
        feature_count += 1;
    }
    sxml_status status = SXML_STATUS_OK;
    if (context.environment->has_sandpit &&
        request->file_system.operations != NULL) {
        context.stage = SXML_TEXT("sandpit");
        sxml_byte_view source = sxml_qt4_empty_text;
        status = sxml_qt4_concatenate(
            request->arena,
            (sxml_qt4_concatenation){
                .first = sxml_qt4_catalog_get_directory(request->catalog),
                .second = context.environment->sandpit_path},
            &source);
        if (status == SXML_STATUS_OK) {
            status = sxml_qt4_sandpit_create(
                &(sxml_qt4_sandpit_request){.host = request->file_system,
                                            .source_directory = source,
                                            .allocator = allocator},
                &sandpit);
        }
        context.stage = SXML_TEXT("processor");
    }
    /* A test that asks for EXPath File without a sandpit gets the
       feature without files (D200). */
    if (status == SXML_STATUS_OK &&
        (context.environment->has_sandpit ||
         sxml_qt4_lists_file_feature(request->test_case->test_set_dependencies) ||
         sxml_qt4_lists_file_feature(request->test_case->dependencies))) {
        features[feature_count] = sxml_expath_file_feature();
        feature_count += 1;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .limits = {.xml_limits = {.maximum_depth = 0,
                                      .maximum_entity_expansion_byte_count =
                                          SXML_QT4_MAXIMUM_EXPANSION_BYTE_COUNT},
                       .maximum_evaluation_step_count =
                           SXML_QT4_QUERY_MAXIMUM_STEP_COUNT},
            .resource_resolver =
                sxml_qt4_resource_table_resolver(&resource_table),
            .clock = sxml_system_clock(),
            .optional_features = {.optional_features = features,
                                  .optional_feature_count = feature_count},
            .file_system = sandpit.file_system},
        &context.processor);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_create(
            allocator,
            &(sxml_arena_configuration){
                .block_allocator = allocator,
                .block_byte_count = SXML_QT4_SCRATCH_BLOCK_BYTE_COUNT,
                .maximum_block_byte_count = 0,
                .initial_memory = {.bytes = NULL, .byte_count = 0}},
            &context.scratch);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_apply_environment(&context);
    }
    if (status == SXML_STATUS_OK) {
        sxml_qt4_evaluate_query(&context, answer);
    } else {
        sxml_qt4_describe_failure(&context, status, answer);
    }
    sxml_qt4_release_context(&context);
    sxml_qt4_sandpit_destroy(&sandpit);
    if (budget.live_allocation_count != 0) {
        answer->needs_missing_feature = false;
    }
    sxml_qt4_budget_finish(&budget,
                           (sxml_qt4_budget_verdict){.outcome = &answer->outcome,
                                                     .reason = &answer->reason});
}

sxml_status sxml_qt4_run_test_case(const sxml_qt4_test_run_request *request,
                                   sxml_qt4_test_result *result) {
    sxml_qt4_test_answer answer = {.outcome = SXML_QT4_TEST_OUTCOME_FAIL};
    const sxml_status status =
        sxml_qt4_reason_start(request->arena, &answer.reason);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_qt4_applicability applicability = {.is_applicable = false};
    sxml_qt4_test_case_decide_applicability(
        (sxml_qt4_test_case_target){.test_case = request->test_case,
                                    .target = request->target},
        &applicability);
    if (!applicability.is_applicable) {
        answer.outcome = SXML_QT4_TEST_OUTCOME_NOT_RUN;
        sxml_qt4_reason_append(&answer.reason, SXML_TEXT("skip: "));
        sxml_qt4_reason_append(&answer.reason, applicability.reason);
        if (applicability.detail.byte_count != 0) {
            sxml_qt4_reason_append(&answer.reason, SXML_TEXT(" "));
            sxml_qt4_reason_append(&answer.reason, applicability.detail);
        }
        if (applicability.is_inverted) {
            sxml_qt4_reason_append(&answer.reason, SXML_TEXT(" (satisfied)"));
        }
    } else {
        sxml_qt4_run_isolated(request, &answer);
    }
    if (!applicability.is_in_scope && !applicability.is_schema_aware &&
        answer.needs_missing_feature &&
        (answer.outcome == SXML_QT4_TEST_OUTCOME_FAIL ||
         answer.outcome == SXML_QT4_TEST_OUTCOME_ERROR) &&
        sxml_qt4_reason_start(request->arena, &answer.reason) == SXML_STATUS_OK) {
        /* A test of a non-goal (D95, D97) that stops at the missing
           feature is not run, as a dependency would have said; any other
           outcome of such a test (a leak, a crash, a wrong result) stays.
           Schema awareness is no longer missing (D174): its tests count
           as they end. */
        answer.outcome = SXML_QT4_TEST_OUTCOME_NOT_RUN;
        sxml_qt4_reason_append(&answer.reason, SXML_TEXT("skip: needs the non-goal "));
        sxml_qt4_reason_append(&answer.reason, applicability.non_goal_kind);
        sxml_qt4_reason_append(&answer.reason, SXML_TEXT(" "));
        sxml_qt4_reason_append(&answer.reason, applicability.non_goal);
    }
    *result = (sxml_qt4_test_result){
        .outcome = answer.outcome,
        .reason = answer.outcome == SXML_QT4_TEST_OUTCOME_PASS
                      ? sxml_qt4_empty_text
                      : sxml_qt4_reason_text(&answer.reason),
        .is_in_scope = applicability.is_in_scope,
        .is_schema_aware = applicability.is_schema_aware};
    return SXML_STATUS_OK;
}
