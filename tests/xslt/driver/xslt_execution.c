/*
 * Executing one planned XSLT 4.0 test case (xslt_execution.h). Imitates
 * tests/qt4/driver/test_run.c: its budget, its stages and reasons, its
 * treatment of a raised error; the stylesheet and the transformation go
 * through include/sxml/xslt.h only.
 */
#include "xslt_execution.h"
#include "xslt_package_version.h"

#include "resource_table.h"
#include "suite_path.h"
#include "sxml/name.h"
#include "sxml/serialize.h"
#include "sxml/string.h"
#include "sxml/types.h"
#include "sxml/xpath.h"

#include <stdalign.h>
#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

#define SXML_XSLT_TEST_SUITE_URI_PREFIX "file:///"

static const sxml_byte_view sxml_xslt_test_empty_text = {.bytes = NULL,
                                                         .byte_count = 0};

/* As the QT4 driver: entity expansion bounded, nesting not. */
enum { SXML_XSLT_TEST_MAXIMUM_EXPANSION_BYTE_COUNT = 4 * 1024 * 1024 };

/* The scratch arena's blocks. */
enum { SXML_XSLT_TEST_SCRATCH_BLOCK_BYTE_COUNT = 8 * 1024 };

/* Most bytes of a serialized principal result. */
enum { SXML_XSLT_TEST_MAXIMUM_OUTPUT_BYTE_COUNT = 64 * 1024 * 1024 };

/* Most messages kept for assert-message; later ones are dropped. */
enum { SXML_XSLT_TEST_MAXIMUM_MESSAGE_COUNT = 256 };

/* ---------------------------------------------------------------- Budget */

/*
 * The allocator of one test, as the QT4 driver's (test_run.c): passes
 * allocations on, counts live allocations and bytes, and refuses any past
 * SXML_QT4_TEST_MAXIMUM_BYTE_COUNT live bytes. Each block carries a header
 * with its size.
 */
typedef struct sxml_xslt_test_budget {
    sxml_allocator backing_allocator;
    size_t live_byte_count;
    uint64_t live_allocation_count;
    bool was_exceeded;
} sxml_xslt_test_budget;

typedef struct sxml_xslt_test_budget_header {
    size_t total_byte_count;
    size_t header_byte_count;
} sxml_xslt_test_budget_header;

enum { SXML_XSLT_TEST_BUDGET_MINIMUM_ALIGNMENT = 16 };

static sxml_status sxml_xslt_test_budget_allocate(void *state,
                                                  sxml_allocation_request request,
                                                  void **memory) {
    sxml_xslt_test_budget *budget = state;
    size_t alignment = request.alignment;
    if (alignment < SXML_XSLT_TEST_BUDGET_MINIMUM_ALIGNMENT) {
        alignment = SXML_XSLT_TEST_BUDGET_MINIMUM_ALIGNMENT;
    }
    _Static_assert(sizeof(sxml_xslt_test_budget_header) <=
                       SXML_XSLT_TEST_BUDGET_MINIMUM_ALIGNMENT,
                   "the budget header fits before the block");
    size_t total_byte_count = 0;
    if (sxml_size_add(request.byte_count, alignment, &total_byte_count) !=
            SXML_STATUS_OK ||
        total_byte_count >
            SXML_QT4_TEST_MAXIMUM_BYTE_COUNT - budget->live_byte_count) {
        budget->was_exceeded = true;
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    void *block = NULL;
    const sxml_status status = sxml_allocator_allocate(
        budget->backing_allocator,
        (sxml_allocation_request){.byte_count = total_byte_count,
                                  .alignment = alignment},
        &block);
    if (status != SXML_STATUS_OK) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    uint8_t *start = (uint8_t *)block + alignment;
    const sxml_xslt_test_budget_header header = {
        .total_byte_count = total_byte_count, .header_byte_count = alignment};
    memcpy(start - sizeof header, &header, sizeof header);
    budget->live_byte_count += total_byte_count;
    budget->live_allocation_count += 1;
    *memory = start;
    return SXML_STATUS_OK;
}

static void sxml_xslt_test_budget_release(void *state, void *memory) {
    sxml_xslt_test_budget *budget = state;
    uint8_t *start = memory;
    sxml_xslt_test_budget_header header;
    memcpy(&header, start - sizeof header, sizeof header);
    budget->live_byte_count -= header.total_byte_count;
    budget->live_allocation_count -= 1;
    sxml_allocator_release(budget->backing_allocator,
                           start - header.header_byte_count);
}

static const sxml_allocator_operations sxml_xslt_test_budget_operations = {
    .allocate = sxml_xslt_test_budget_allocate,
    .release = sxml_xslt_test_budget_release,
};

/* ------------------------------------------------------------ The context */

/* A list of bindings being filled, in the scratch arena. */
typedef struct sxml_xslt_test_binding_list {
    sxml_variable_binding *bindings;
    size_t binding_count;
} sxml_xslt_test_binding_list;

/* Everything one test holds while it runs. */
typedef struct sxml_xslt_test_context {
    const sxml_xslt_test_execution_request *request;
    const sxml_xslt_test_case *test_case;
    sxml_allocator allocator;
    sxml_processor *processor;
    sxml_arena *scratch;
    sxml_qt4_resource_table resource_table;
    /* The result element's in-scope namespaces, in the processor's pool:
       for params' select and the assertions. */
    sxml_namespace_binding *namespace_bindings;
    size_t namespace_binding_count;
    sxml_byte_view test_set_uri;
    sxml_available_document *available_documents;
    size_t available_document_count;
    bool has_global_context_item;
    sxml_sequence global_context_item;
    sxml_xslt_test_binding_list static_parameters;
    sxml_xslt_test_binding_list stylesheet_parameters;
    sxml_xslt_test_binding_list template_parameters;
    sxml_xslt_test_binding_list tunnel_parameters;
    sxml_sequence *function_arguments;
    size_t function_argument_count;
    bool has_initial_match_selection;
    sxml_sequence initial_match_selection;
    /* Owned values (sources, params, the selection), released at the
       end. */
    sxml_sequence *owned_values;
    size_t owned_value_count;
    size_t owned_value_capacity;
    /* What xsl:message sent, retained. */
    sxml_sequence messages[SXML_XSLT_TEST_MAXIMUM_MESSAGE_COUNT];
    size_t message_count;
    /* What was being done when something failed, for the reason. */
    sxml_byte_view stage;
    sxml_byte_view stage_subject;
    sxml_error_record error_record;
} sxml_xslt_test_context;

typedef struct sxml_xslt_test_stage {
    sxml_byte_view description;
    sxml_byte_view subject;
} sxml_xslt_test_stage;

static void sxml_xslt_test_set_stage(sxml_xslt_test_context *context,
                                     sxml_xslt_test_stage stage) {
    context->stage = stage.description;
    context->stage_subject = stage.subject;
}

/* Keeps value until the end of the test; the capacity covers every value
   the environment and the invocation can own. */
static void sxml_xslt_test_own_value(sxml_xslt_test_context *context,
                                     sxml_sequence value) {
    if (context->owned_value_count < context->owned_value_capacity) {
        context->owned_values[context->owned_value_count] = value;
        context->owned_value_count += 1;
    } else {
        sxml_sequence_release(value);
    }
}

/* Allocates element_count elements in the scratch arena; 0 gives NULL. */
typedef struct sxml_xslt_test_scratch_request {
    size_t element_count;
    size_t element_byte_count;
    size_t alignment;
} sxml_xslt_test_scratch_request;

static sxml_status
sxml_xslt_test_scratch_array(sxml_xslt_test_context *context,
                              sxml_xslt_test_scratch_request request,
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
static sxml_status sxml_xslt_test_suite_uri(sxml_xslt_test_context *context,
                                            sxml_byte_view suite_path,
                                            sxml_byte_view *uri) {
    return sxml_qt4_concatenate(
        context->scratch,
        (sxml_qt4_concatenation){.first = SXML_TEXT(SXML_XSLT_TEST_SUITE_URI_PREFIX),
                                 .second = suite_path},
        uri);
}

/* True when reference starts with a URI scheme (RFC 3986, section 3.1). */
static bool sxml_xslt_test_has_scheme(sxml_byte_view reference) {
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

/* A URI of the test case as an absolute URI: a relative one is resolved
   against the test-set file. */
static sxml_status sxml_xslt_test_absolute_uri(sxml_xslt_test_context *context,
                                               sxml_byte_view reference,
                                               sxml_byte_view *uri) {
    if (sxml_xslt_test_has_scheme(reference)) {
        *uri = reference;
        return SXML_STATUS_OK;
    }
    sxml_byte_view suite_path = sxml_xslt_test_empty_text;
    const sxml_status status = sxml_qt4_suite_path_resolve(
        &(sxml_qt4_suite_path_request){.containing_file =
                                           context->test_case->test_set_file_path,
                                       .reference = reference,
                                       .arena = context->scratch},
        &suite_path);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xslt_test_suite_uri(context, suite_path, uri);
}

/* The name a catalog name stands for, interned in the processor's pool. */
static sxml_status sxml_xslt_test_intern(sxml_xslt_test_context *context,
                                         const sxml_xslt_test_name *name,
                                         sxml_name_identifier *identifier) {
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(context->processor);
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    sxml_status status = sxml_name_pool_intern_namespace_uri(
        name_pool, name->namespace_uri, &namespace_uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){.namespace_uri = namespace_uri,
                                 .local_name = name->local_name},
            identifier);
    }
    return status;
}

/* ------------------------------------------------------------- Resolver */

/* The name and version of a package (3.4); the texts read from a file
   are copied into the buffers. */
typedef struct sxml_package_identity {
    sxml_byte_view package_name;
    sxml_byte_view package_version;
    uint8_t name_bytes[256];
    uint8_t version_bytes[64];
} sxml_package_identity;

static sxml_byte_view sxml_copy_into(sxml_byte_view text, uint8_t *buffer, size_t capacity) {
    if (text.byte_count == 0 || text.byte_count > capacity) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    memcpy(buffer, text.bytes, text.byte_count);
    return (sxml_byte_view){.bytes = buffer, .byte_count = text.byte_count};
}

/* For a <package> without uri= or package-version=: the name and version
   its file gives. */
static sxml_status sxml_xslt_test_read_package_identity(sxml_xslt_test_context *context,
                                                        const sxml_xslt_test_module *module,
                                                        sxml_package_identity *identity) {
    if (identity->package_name.byte_count > 0 && identity->package_version.byte_count > 0) {
        return SXML_STATUS_OK;
    }
    const sxml_resource_resolver table =
        sxml_qt4_resource_table_resolver(&context->resource_table);
    sxml_byte_view file_uri = sxml_xslt_test_empty_text;
    sxml_status status = sxml_xslt_test_suite_uri(context, module->file_path, &file_uri);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_resource resource;
    memset(&resource, 0, sizeof(resource));
    status = table.operations->resolve(
        table.state,
        &(sxml_resource_request){.resource_kind = SXML_RESOURCE_KIND_XML_DOCUMENT,
                                 .uri_reference = file_uri,
                                 .absolute_uri = file_uri,
                                 .processor = NULL,
                                 .encoding_name = sxml_xslt_test_empty_text,
                                 .is_default_collection = false,
                                 .target_namespace_uri = sxml_xslt_test_empty_text,
                                 .package_version = sxml_xslt_test_empty_text},
        &resource);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (resource.resource_form == SXML_RESOURCE_FORM_BYTES) {
        if (identity->package_name.byte_count == 0) {
            identity->package_name = sxml_copy_into(
                sxml_xslt_test_package_attribute((sxml_xslt_test_package_attribute_query){
                    .module_text = resource.content, .attribute_name = SXML_TEXT("name")}),
                identity->name_bytes, sizeof(identity->name_bytes));
        }
        if (identity->package_version.byte_count == 0) {
            identity->package_version = sxml_copy_into(
                sxml_xslt_test_package_attribute((sxml_xslt_test_package_attribute_query){
                    .module_text = resource.content,
                    .attribute_name = SXML_TEXT("package-version")}),
                identity->version_bytes, sizeof(identity->version_bytes));
        }
    }
    if (table.operations->release != NULL) {
        table.operations->release(table.state, &resource);
    }
    return SXML_STATUS_OK;
}

/* The test's resolver: modules and packages as suite files, the rest as
   the QT4 resource table serves it. */
static sxml_status sxml_xslt_test_resolve(void *state,
                                          const sxml_resource_request *request,
                                          sxml_resource *resource) {
    sxml_xslt_test_context *context = state;
    const sxml_resource_resolver table =
        sxml_qt4_resource_table_resolver(&context->resource_table);
    sxml_resource_request forwarded = *request;
    if (request->resource_kind == SXML_RESOURCE_KIND_XSLT_MODULE) {
        forwarded.resource_kind = SXML_RESOURCE_KIND_XML_DOCUMENT;
        return table.operations->resolve(table.state, &forwarded, resource);
    }
    if (request->resource_kind != SXML_RESOURCE_KIND_XSLT_PACKAGE) {
        return table.operations->resolve(table.state, request, resource);
    }
    /* 3.4.3: the package of that name among the test's and the
       environment's modules whose version the range matches, the highest
       (package_version_resolution highest_version, D117, D160); a
       <package> without uri= is known by the name in its file. */
    const sxml_xslt_test_module_view lists[2] = {
        context->test_case->modules, context->test_case->environment.modules};
    const sxml_xslt_test_module *best = NULL;
    uint8_t best_version_bytes[64];
    sxml_byte_view best_version = sxml_xslt_test_empty_text;
    for (size_t list_index = 0; list_index < 2; list_index += 1) {
        for (size_t module_index = 0; module_index < lists[list_index].module_count;
             module_index += 1) {
            const sxml_xslt_test_module *module =
                &lists[list_index].modules[module_index];
            if (module->module_kind != SXML_XSLT_TEST_MODULE_KIND_PACKAGE) {
                continue;
            }
            const bool is_unnamed =
                module->uri.byte_count == 0 && module->package_name.byte_count == 0;
            if (request->uri_reference.byte_count == 0) {
                /* The principal package without a name: the first such. */
                if (is_unnamed && best == NULL) {
                    best = module;
                }
                continue;
            }
            const bool is_named = sxml_byte_view_equals(module->uri, request->uri_reference) ||
                                  sxml_byte_view_equals(module->package_name,
                                                        request->uri_reference);
            if (!is_named && !is_unnamed) {
                continue;
            }
            sxml_package_identity identity_storage;
            sxml_package_identity *identity = &identity_storage;
            memset(identity, 0, sizeof(*identity));
            identity->package_name = is_named ? request->uri_reference : sxml_xslt_test_empty_text;
            identity->package_version = module->package_version;
            const sxml_status read_status =
                sxml_xslt_test_read_package_identity(context, module, identity);
            if (read_status != SXML_STATUS_OK) {
                return read_status;
            }
            if (!is_named &&
                !sxml_byte_view_equals(identity->package_name, request->uri_reference)) {
                continue;
            }
            const sxml_byte_view version = identity->package_version.byte_count == 0
                                               ? SXML_TEXT("1")
                                               : identity->package_version;
            sxml_xslt_test_package_version_match match = {.matches = true};
            if (sxml_xslt_test_package_version_matches(
                    (sxml_xslt_test_package_version_query){
                        .package_version = version,
                        .version_range = request->package_version.byte_count == 0
                                             ? SXML_TEXT("*")
                                             : request->package_version},
                    &match) != SXML_STATUS_OK) {
                /* A range the package reader rejects (XTSE0020): any. */
                match.matches = true;
            }
            if (match.matches &&
                (best == NULL ||
                 sxml_xslt_test_compare_package_versions((sxml_xslt_test_package_version_pair){
                     .first = version, .second = best_version}) > 0)) {
                best = module;
                best_version = sxml_copy_into(version, best_version_bytes,
                                              sizeof(best_version_bytes));
            }
        }
    }
    if (best != NULL) {
        sxml_byte_view file_uri = sxml_xslt_test_empty_text;
        sxml_status status = sxml_xslt_test_suite_uri(context, best->file_path, &file_uri);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        forwarded.resource_kind = SXML_RESOURCE_KIND_XML_DOCUMENT;
        forwarded.uri_reference = file_uri;
        forwarded.absolute_uri = file_uri;
        status = table.operations->resolve(table.state, &forwarded, resource);
        if (status == SXML_STATUS_OK && resource->resource_form == SXML_RESOURCE_FORM_BYTES &&
            resource->resource_uri.byte_count == 0) {
            resource->resource_uri = file_uri;
        }
        return status;
    }
    resource->resource_form = SXML_RESOURCE_FORM_NONE;
    resource->description = SXML_TEXT("No package of that name in the test case");
    return SXML_STATUS_OK;
}

static void sxml_xslt_test_release_resource(void *state,
                                            const sxml_resource *resource) {
    sxml_xslt_test_context *context = state;
    const sxml_resource_resolver table =
        sxml_qt4_resource_table_resolver(&context->resource_table);
    if (table.operations->release != NULL) {
        table.operations->release(table.state, resource);
    }
}

static const sxml_resource_resolver_operations sxml_xslt_test_resolver_operations = {
    .resolve = sxml_xslt_test_resolve,
    .release = sxml_xslt_test_release_resource,
};

/* ------------------------------------------------------------- Messages */

/* Keeps every xsl:message (and failed xsl:assert) value for
   assert-message. */
static sxml_status sxml_xslt_test_receive_trace(void *state,
                                                const sxml_trace_event *event) {
    sxml_xslt_test_context *context = state;
    if (event->trace_event_kind == SXML_TRACE_EVENT_KIND_XSLT_MESSAGE &&
        context->message_count < SXML_XSLT_TEST_MAXIMUM_MESSAGE_COUNT) {
        sxml_sequence_retain(event->value);
        context->messages[context->message_count] = event->value;
        context->message_count += 1;
    }
    return SXML_STATUS_OK;
}

static const sxml_trace_sink_operations sxml_xslt_test_trace_operations = {
    .receive = sxml_xslt_test_receive_trace,
};

/* ----------------------------------------------------------- Environment */

/* The in-scope namespaces of the result element, prefixed ones only (the
   catalog's default namespace is not the expressions'), moved from the
   catalog's name pool to the processor's. */
static sxml_status sxml_xslt_test_bind_namespaces(sxml_xslt_test_context *context) {
    const sxml_qt4_document *document = sxml_xslt_test_catalog_get_test_set_document(
        context->request->run->catalog);
    sxml_namespace_binding_view in_scope = {.namespace_bindings = NULL,
                                            .namespace_binding_count = 0};
    sxml_status status = sxml_node_get_in_scope_namespaces(
        sxml_qt4_document_reference(document, context->test_case->result), &in_scope);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_scratch_array(
            context,
            (sxml_xslt_test_scratch_request){
                .element_count = in_scope.namespace_binding_count,
                .element_byte_count = sizeof(sxml_namespace_binding),
                .alignment = alignof(sxml_namespace_binding)},
            &memory);
    }
    context->namespace_bindings = memory;
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(context->processor);
    for (size_t binding_index = 0;
         status == SXML_STATUS_OK && binding_index < in_scope.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding *binding = &in_scope.namespace_bindings[binding_index];
        if (binding->namespace_prefix.index == 0) {
            continue;
        }
        sxml_byte_view prefix = sxml_xslt_test_empty_text;
        sxml_byte_view uri = sxml_xslt_test_empty_text;
        status = sxml_name_pool_get_namespace_prefix(document->name_pool,
                                                     binding->namespace_prefix, &prefix);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_namespace_uri(document->name_pool,
                                                      binding->namespace_uri, &uri);
        }
        sxml_namespace_binding moved = {.namespace_prefix = {.index = 0},
                                        .namespace_uri = {.index = 0}};
        if (status == SXML_STATUS_OK && sxml_byte_view_equals(prefix, SXML_TEXT("xml"))) {
            /* Always in scope; a static context does not bind it. */
            continue;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_intern_namespace_prefix(name_pool, prefix,
                                                            &moved.namespace_prefix);
        }
        if (status == SXML_STATUS_OK) {
            status =
                sxml_name_pool_intern_namespace_uri(name_pool, uri, &moved.namespace_uri);
        }
        if (status == SXML_STATUS_OK) {
            context->namespace_bindings[context->namespace_binding_count] = moved;
            context->namespace_binding_count += 1;
        }
    }
    return status;
}

/* The static context of params' select and of the assertions. */
static sxml_static_context_configuration
sxml_xslt_test_static_context(const sxml_xslt_test_context *context) {
    sxml_static_context_configuration static_context;
    memset(&static_context, 0, sizeof static_context);
    static_context.namespace_bindings = (sxml_namespace_binding_view){
        .namespace_bindings = context->namespace_bindings,
        .namespace_binding_count = context->namespace_binding_count};
    static_context.static_base_uri = context->test_set_uri;
    return static_context;
}

/* Evaluates an XPath expression of the catalog (a param's or the initial
   mode's select), with focus when has_focus, into *value (owned). */
typedef struct sxml_xslt_test_expression {
    sxml_byte_view expression_text;
    bool has_focus;
    sxml_sequence focus;
} sxml_xslt_test_expression;

static sxml_status sxml_xslt_test_evaluate(sxml_xslt_test_context *context,
                                           sxml_xslt_test_expression expression_text,
                                           sxml_sequence *value) {
    sxml_xpath_expression *expression = NULL;
    sxml_status status = sxml_xpath_expression_create(
        context->allocator,
        &(sxml_xpath_expression_configuration){
            .processor = context->processor,
            .expression_text = expression_text.expression_text,
            .source_name = SXML_TEXT("select"),
            .static_context = sxml_xslt_test_static_context(context),
            .error_record = &context->error_record},
        &expression);
    if (status == SXML_STATUS_OK) {
        sxml_dynamic_context_configuration dynamic_context;
        memset(&dynamic_context, 0, sizeof dynamic_context);
        dynamic_context.has_context_value = expression_text.has_focus;
        dynamic_context.context_value = expression_text.focus;
        dynamic_context.available_documents = (sxml_available_document_view){
            .available_documents = context->available_documents,
            .available_document_count = context->available_document_count};
        status = sxml_xpath_expression_evaluate(
            expression,
            &(sxml_xpath_evaluation_request){.dynamic_context = dynamic_context,
                                             .error_record = &context->error_record},
            value);
    }
    sxml_xpath_expression_destroy(expression);
    return status;
}

/* Parses one source: its inline content or its file. */
static sxml_status sxml_xslt_test_parse_source(sxml_xslt_test_context *context,
                                               size_t source_index) {
    const sxml_xslt_test_environment *environment = &context->test_case->environment;
    const sxml_qt4_source *source = &environment->shared.sources.sources[source_index];
    const sxml_xslt_test_source_detail *detail =
        source_index < environment->source_details.source_detail_count
            ? &environment->source_details.source_details[source_index]
            : NULL;
    const bool has_content = detail != NULL && detail->has_content;
    sxml_xslt_test_set_stage(context,
                             (sxml_xslt_test_stage){.description = SXML_TEXT("source"),
                                                    .subject = source->file_path});
    sxml_byte_view document_uri = sxml_xslt_test_empty_text;
    sxml_status status = SXML_STATUS_OK;
    if (source->uri.byte_count != 0) {
        status = sxml_xslt_test_absolute_uri(context, source->uri, &document_uri);
    } else if (source->file_path.byte_count != 0) {
        status = sxml_xslt_test_suite_uri(context, source->file_path, &document_uri);
    } else {
        document_uri = context->test_set_uri;
    }
    sxml_byte_view contents = has_content ? detail->content : sxml_xslt_test_empty_text;
    if (status == SXML_STATUS_OK && !has_content) {
        status = sxml_xslt_test_catalog_read_file(
            context->request->run->catalog,
            &(sxml_qt4_file_request){.suite_path = source->file_path,
                                     .allocator = context->allocator},
            &contents);
    }
    sxml_item document_node = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_processor_parse_document(
            context->processor,
            &(sxml_xml_parse_request){.input = contents,
                                      .source_name = source->file_path,
                                      .document_uri = document_uri,
                                      .base_uri = document_uri,
                                      .encoding_name = sxml_xslt_test_empty_text,
                                      .diagnostic = &context->error_record.diagnostic},
            &document_node);
        if (!has_content) {
            sxml_allocator_release(context->allocator, (void *)contents.bytes);
        }
    }
    if (status != SXML_STATUS_OK) {
        if ((status == SXML_STATUS_MALFORMED_INPUT || status == SXML_STATUS_INVALID_UTF8) &&
            source->role.byte_count == 0) {
            /* Read only through fn:doc, which then fails (as QT4). */
            return SXML_STATUS_OK;
        }
        return status;
    }
    const sxml_sequence value = sxml_sequence_from_item(document_node);
    sxml_xslt_test_own_value(context, value);
    if (source->uri.byte_count != 0) {
        context->available_documents[context->available_document_count] =
            (sxml_available_document){.document_uri = document_uri,
                                      .document_node = document_node};
        context->available_document_count += 1;
    }
    if (!sxml_byte_view_equals(source->role, SXML_TEXT("."))) {
        return SXML_STATUS_OK;
    }
    if (detail == NULL || detail->select.byte_count == 0) {
        context->has_global_context_item = true;
        context->global_context_item = value;
        return SXML_STATUS_OK;
    }
    /* select=: the global context item within the document. */
    sxml_sequence selected = sxml_sequence_empty();
    status = sxml_xslt_test_evaluate(
        context,
        (sxml_xslt_test_expression){.expression_text = detail->select,
                                    .has_focus = true,
                                    .focus = value},
        &selected);
    if (status == SXML_STATUS_OK) {
        sxml_xslt_test_own_value(context, selected);
        context->has_global_context_item = true;
        context->global_context_item = selected;
    }
    return status;
}

/* The value of one <param>: its select evaluated, or the empty sequence. */
static sxml_status sxml_xslt_test_parameter_value(sxml_xslt_test_context *context,
                                                  const sxml_xslt_test_parameter *parameter,
                                                  sxml_sequence *value) {
    sxml_xslt_test_set_stage(
        context, (sxml_xslt_test_stage){.description = SXML_TEXT("param"),
                                        .subject = parameter->name.lexical_name});
    *value = sxml_sequence_empty();
    if (parameter->select.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_status status = sxml_xslt_test_evaluate(
        context,
        (sxml_xslt_test_expression){.expression_text = parameter->select,
                                    .has_focus = false,
                                    .focus = sxml_sequence_empty()},
        value);
    if (status == SXML_STATUS_OK) {
        sxml_xslt_test_own_value(context, *value);
    }
    return status;
}

/* Adds one named param to list (allocated for every param of its kind). */
static sxml_status sxml_xslt_test_add_parameter(sxml_xslt_test_context *context,
                                                const sxml_xslt_test_parameter *parameter,
                                                sxml_xslt_test_binding_list *list) {
    sxml_variable_binding binding = {.variable_name = {.index = 0},
                                     .value = sxml_sequence_empty()};
    sxml_status status = sxml_xslt_test_parameter_value(context, parameter, &binding.value);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_intern(context, &parameter->name, &binding.variable_name);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* A later param of a name replaces an earlier one (the test's over the
       environment's). */
    for (size_t binding_index = 0; binding_index < list->binding_count;
         binding_index += 1) {
        if (list->bindings[binding_index].variable_name.index ==
            binding.variable_name.index) {
            list->bindings[binding_index] = binding;
            return SXML_STATUS_OK;
        }
    }
    list->bindings[list->binding_count] = binding;
    list->binding_count += 1;
    return SXML_STATUS_OK;
}

/* Allocates a binding list for count bindings. */
static sxml_status sxml_xslt_test_start_list(sxml_xslt_test_context *context,
                                             size_t binding_count,
                                             sxml_xslt_test_binding_list *list) {
    void *memory = NULL;
    const sxml_status status = sxml_xslt_test_scratch_array(
        context,
        (sxml_xslt_test_scratch_request){.element_count = binding_count,
                                       .element_byte_count = sizeof(sxml_variable_binding),
                                       .alignment = alignof(sxml_variable_binding)},
        &memory);
    *list = (sxml_xslt_test_binding_list){.bindings = memory, .binding_count = 0};
    return status;
}

/* The params of the environment, then of <test>: static or stylesheet
   parameters. */
static sxml_status sxml_xslt_test_bind_parameters(sxml_xslt_test_context *context) {
    const sxml_xslt_test_execution_request *request = context->request;
    sxml_status status = sxml_xslt_test_start_list(
        context, request->plan->static_parameter_count, &context->static_parameters);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_start_list(context, request->plan->stylesheet_parameter_count,
                                           &context->stylesheet_parameters);
    }
    const sxml_xslt_test_parameter_view lists[2] = {
        context->test_case->environment.parameters, context->test_case->parameters};
    for (size_t list_index = 0; status == SXML_STATUS_OK && list_index < 2; list_index += 1) {
        for (size_t parameter_index = 0;
             status == SXML_STATUS_OK && parameter_index < lists[list_index].parameter_count;
             parameter_index += 1) {
            const sxml_xslt_test_parameter *parameter =
                &lists[list_index].parameters[parameter_index];
            status = sxml_xslt_test_add_parameter(context, parameter,
                                                  parameter->is_static
                                                      ? &context->static_parameters
                                                      : &context->stylesheet_parameters);
        }
    }
    return status;
}

/* The invocation's params and the initial match selection. */
static sxml_status sxml_xslt_test_bind_invocation(sxml_xslt_test_context *context) {
    const sxml_xslt_test_invocation *invocation = &context->test_case->invocation;
    const sxml_xslt_test_parameter_view parameters = invocation->parameters;
    sxml_status status = SXML_STATUS_OK;
    if (invocation->invocation_kind == SXML_XSLT_INVOCATION_KIND_CALL_FUNCTION) {
        void *memory = NULL;
        status = sxml_xslt_test_scratch_array(
            context,
            (sxml_xslt_test_scratch_request){.element_count = parameters.parameter_count,
                                           .element_byte_count = sizeof(sxml_sequence),
                                           .alignment = alignof(sxml_sequence)},
            &memory);
        context->function_arguments = memory;
        for (size_t parameter_index = 0;
             status == SXML_STATUS_OK && parameter_index < parameters.parameter_count;
             parameter_index += 1) {
            status = sxml_xslt_test_parameter_value(
                context, &parameters.parameters[parameter_index],
                &context->function_arguments[parameter_index]);
            context->function_argument_count += status == SXML_STATUS_OK ? 1 : 0;
        }
        return status;
    }
    status = sxml_xslt_test_start_list(context, parameters.parameter_count,
                                       &context->template_parameters);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_start_list(context, parameters.parameter_count,
                                           &context->tunnel_parameters);
    }
    for (size_t parameter_index = 0;
         status == SXML_STATUS_OK && parameter_index < parameters.parameter_count;
         parameter_index += 1) {
        const sxml_xslt_test_parameter *parameter = &parameters.parameters[parameter_index];
        status = sxml_xslt_test_add_parameter(context, parameter,
                                              parameter->is_tunnel
                                                  ? &context->tunnel_parameters
                                                  : &context->template_parameters);
    }
    if (status == SXML_STATUS_OK &&
        invocation->invocation_kind == SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES &&
        invocation->initial_match_selection.byte_count != 0) {
        sxml_xslt_test_set_stage(
            context, (sxml_xslt_test_stage){.description = SXML_TEXT("initial-mode select"),
                                            .subject = sxml_xslt_test_empty_text});
        status = sxml_xslt_test_evaluate(
            context,
            (sxml_xslt_test_expression){.expression_text = invocation->initial_match_selection,
                                        .has_focus = context->has_global_context_item,
                                        .focus = context->global_context_item},
            &context->initial_match_selection);
        if (status == SXML_STATUS_OK) {
            sxml_xslt_test_own_value(context, context->initial_match_selection);
            context->has_initial_match_selection = true;
        }
    }
    return status;
}

/* Sets up the sources, the params and the invocation's values. */
static sxml_status sxml_xslt_test_apply_environment(sxml_xslt_test_context *context) {
    const sxml_xslt_test_case *test_case = context->test_case;
    const sxml_qt4_source_view sources = test_case->environment.shared.sources;
    sxml_xslt_test_set_stage(context,
                             (sxml_xslt_test_stage){.description = SXML_TEXT("environment"),
                                                    .subject = sxml_xslt_test_empty_text});
    context->owned_value_capacity = 2 * sources.source_count +
                                    test_case->environment.parameters.parameter_count +
                                    test_case->parameters.parameter_count +
                                    test_case->invocation.parameters.parameter_count + 1;
    void *memory = NULL;
    sxml_status status = sxml_xslt_test_scratch_array(
        context,
        (sxml_xslt_test_scratch_request){.element_count = context->owned_value_capacity,
                                       .element_byte_count = sizeof(sxml_sequence),
                                       .alignment = alignof(sxml_sequence)},
        &memory);
    context->owned_values = memory;
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_scratch_array(
            context,
            (sxml_xslt_test_scratch_request){
                .element_count = sources.source_count,
                .element_byte_count = sizeof(sxml_available_document),
                .alignment = alignof(sxml_available_document)},
            &memory);
        context->available_documents = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_suite_uri(context, test_case->test_set_file_path,
                                          &context->test_set_uri);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_bind_namespaces(context);
    }
    for (size_t source_index = 0; status == SXML_STATUS_OK && source_index < sources.source_count;
         source_index += 1) {
        status = sxml_xslt_test_parse_source(context, source_index);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_bind_parameters(context);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_bind_invocation(context);
    }
    return status;
}

/* ---------------------------------------------------------- Transforming */

/* What the transformation gave, for the assertions. */
typedef struct sxml_xslt_test_transformation_answer {
    sxml_status status;
    /* The principal result (owned), or for a serialized one what it parses
       to. */
    sxml_sequence value;
    sxml_serialization_parameters serialization_parameters;
    sxml_byte_view serialized_bytes;
    bool is_serialized;
    sxml_byte_view base_output_uri;
} sxml_xslt_test_transformation_answer;

/* What the transformation needs that outlives the call. */
typedef struct sxml_xslt_test_outputs {
    sxml_xslt_stylesheet *stylesheet;
    sxml_xslt_result_document_collector *collector;
    sxml_output_buffer *output_buffer;
} sxml_xslt_test_outputs;

static void sxml_xslt_test_release_outputs(sxml_xslt_test_outputs *outputs) {
    sxml_output_buffer_destroy(outputs->output_buffer);
    sxml_xslt_result_document_collector_destroy(outputs->collector);
    sxml_xslt_stylesheet_destroy(outputs->stylesheet);
    outputs->output_buffer = NULL;
    outputs->collector = NULL;
    outputs->stylesheet = NULL;
}

/* True when the test has an assertion on the serialization. */
static bool
sxml_xslt_test_needs_serialization(const sxml_xslt_test_assertion_survey *survey) {
    return survey->kind_counts[SXML_XSLT_TEST_ASSERTION_KIND_SERIALIZATION_MATCHES] > 0 ||
           survey->kind_counts[SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_SERIALIZATION] > 0 ||
           survey->kind_counts[SXML_XSLT_TEST_ASSERTION_KIND_ASSERT_SERIALIZATION_ERROR] > 0;
}

/* Compiles the principal module. */
static sxml_status sxml_xslt_test_compile(sxml_xslt_test_context *context,
                                          sxml_xslt_test_outputs *outputs) {
    const sxml_xslt_test_plan *plan = context->request->plan;
    const sxml_xslt_test_module *module = plan->principal_module;
    sxml_xslt_test_set_stage(context,
                             (sxml_xslt_test_stage){.description = SXML_TEXT("stylesheet"),
                                                    .subject = module->file_path});
    sxml_xslt_stylesheet_configuration configuration;
    memset(&configuration, 0, sizeof configuration);
    configuration.processor = context->processor;
    configuration.principal_module_source = plan->principal_module_source;
    configuration.static_parameters = (sxml_variable_binding_view){
        .variable_bindings = context->static_parameters.bindings,
        .variable_binding_count = context->static_parameters.binding_count};
    configuration.error_record = &context->error_record;
    sxml_byte_view text = sxml_xslt_test_empty_text;
    sxml_status status = SXML_STATUS_OK;
    if (plan->principal_module_source == SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT) {
        status = sxml_xslt_test_suite_uri(context, module->file_path,
                                          &configuration.static_context.static_base_uri);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_test_catalog_read_file(
                context->request->run->catalog,
                &(sxml_qt4_file_request){.suite_path = module->file_path,
                                         .allocator = context->allocator},
                &text);
        }
        configuration.principal_module_text = text;
        configuration.source_name = module->file_path;
    } else {
        configuration.principal_module_uri_reference =
            module->uri.byte_count != 0 ? module->uri : module->package_name;
        configuration.package_version = module->package_version;
    }
    if (status == SXML_STATUS_OK) {
        memset(&context->error_record, 0, sizeof context->error_record);
        status = sxml_xslt_stylesheet_create(context->allocator, &configuration,
                                             &outputs->stylesheet);
    }
    sxml_allocator_release(context->allocator, (void *)text.bytes);
    return status;
}

/* The base output URI: output file=, else a directory of the test case's
   own under the test set's. */
static sxml_status sxml_xslt_test_base_output_uri(sxml_xslt_test_context *context,
                                                  sxml_byte_view *uri) {
    const sxml_xslt_test_case *test_case = context->test_case;
    if (test_case->output.file_path.byte_count != 0) {
        return sxml_xslt_test_suite_uri(context, test_case->output.file_path, uri);
    }
    sxml_byte_view directory = test_case->test_set_file_path;
    while (directory.byte_count > 0 && directory.bytes[directory.byte_count - 1] != '/') {
        directory.byte_count -= 1;
    }
    sxml_byte_view path = sxml_xslt_test_empty_text;
    sxml_status status = sxml_qt4_concatenate(
        context->scratch,
        (sxml_qt4_concatenation){.first = directory, .second = SXML_TEXT("output/")},
        &path);
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_concatenate(
            context->scratch,
            (sxml_qt4_concatenation){.first = path, .second = test_case->name}, &path);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_concatenate(
            context->scratch, (sxml_qt4_concatenation){.first = path, .second = SXML_TEXT("/")},
            &path);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_suite_uri(context, path, uri);
    }
    return status;
}

/* The names of the invocation, interned. */
typedef struct sxml_xslt_test_invocation_names {
    sxml_name_identifier initial_template;
    sxml_name_identifier initial_mode;
    sxml_name_identifier initial_function;
    bool selects_unnamed_mode;
} sxml_xslt_test_invocation_names;

static sxml_status sxml_xslt_test_invocation_names_of(sxml_xslt_test_context *context,
                                                      sxml_xslt_test_invocation_names *names) {
    const sxml_xslt_test_invocation *invocation = &context->test_case->invocation;
    sxml_status status = SXML_STATUS_OK;
    switch (invocation->invocation_kind) {
    case SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE:
        if (invocation->initial_template.local_name.byte_count != 0) {
            status = sxml_xslt_test_intern(context, &invocation->initial_template,
                                           &names->initial_template);
        }
        break;
    case SXML_XSLT_INVOCATION_KIND_CALL_FUNCTION:
        status = sxml_xslt_test_intern(context, &invocation->initial_function,
                                       &names->initial_function);
        break;
    case SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES:
        if (sxml_byte_view_equals(invocation->initial_mode.lexical_name,
                                  SXML_TEXT("#unnamed"))) {
            names->selects_unnamed_mode = true;
        } else if (invocation->initial_mode.local_name.byte_count != 0) {
            status = sxml_xslt_test_intern(context, &invocation->initial_mode,
                                           &names->initial_mode);
        }
        break;
    }
    return status;
}

/* The invocation: as the test says, except that a test that names
   neither an initial template, mode or function nor a global context item
   calls xsl:initial-template (as the suite expects: XSLT 4.0, 2.4.4). */
static sxml_xslt_invocation_kind
sxml_xslt_test_invocation_kind_of(const sxml_xslt_test_context *context) {
    const sxml_xslt_test_invocation *invocation = &context->test_case->invocation;
    if (invocation->invocation_kind == SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES &&
        !invocation->has_initial_mode && !context->has_global_context_item &&
        !context->has_initial_match_selection) {
        return SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE;
    }
    return invocation->invocation_kind;
}

/* Parses a serialized result back for the other assertions: a document
   when it is well-formed XML, else an xs:string. */
static sxml_status sxml_xslt_test_parse_back(sxml_xslt_test_context *context,
                                             sxml_byte_view bytes, sxml_sequence *value) {
    sxml_diagnostic diagnostic;
    memset(&diagnostic, 0, sizeof diagnostic);
    sxml_item document = {.representation = 0};
    if (sxml_processor_parse_document(
            context->processor,
            &(sxml_xml_parse_request){.input = bytes,
                                      .source_name = SXML_TEXT("serialized result"),
                                      .document_uri = sxml_xslt_test_empty_text,
                                      .base_uri = sxml_xslt_test_empty_text,
                                      .encoding_name = sxml_xslt_test_empty_text,
                                      .diagnostic = &diagnostic},
            &document) == SXML_STATUS_OK) {
        *value = sxml_sequence_from_item(document);
        return SXML_STATUS_OK;
    }
    sxml_string string = {.utf8 = sxml_xslt_test_empty_text, .codepoint_count = 0, .is_ascii = true};
    sxml_status status = sxml_string_from_utf8(bytes, &string);
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            sxml_processor_get_item_heap(context->processor),
            &(sxml_atomic_item_request){.type = {.index = SXML_TYPE_XS_STRING},
                                        .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                                                  .datum_details = {.string = string}}},
            &item);
    }
    if (status == SXML_STATUS_OK) {
        *value = sxml_sequence_from_item(item);
    } else {
        /* Output that is not UTF-8 (an encoding parameter) is no string. */
        *value = sxml_sequence_empty();
    }
    return SXML_STATUS_OK;
}

/* Compiles and runs the transformation. */
static void sxml_xslt_test_transform(sxml_xslt_test_context *context,
                                     sxml_xslt_test_outputs *outputs,
                                     sxml_xslt_test_transformation_answer *answer) {
    const sxml_xslt_test_execution_request *request = context->request;
    sxml_xslt_result_delivery delivery = request->plan->result_delivery;
    if (!context->test_case->output.is_present &&
        sxml_xslt_test_needs_serialization(request->survey)) {
        delivery = SXML_XSLT_RESULT_DELIVERY_SERIALIZED;
    }
    answer->serialization_parameters.serialization_method = SXML_SERIALIZATION_METHOD_XML;
    sxml_status status = sxml_xslt_test_compile(context, outputs);
    sxml_xslt_output_definition definition;
    memset(&definition, 0, sizeof definition);
    if (status == SXML_STATUS_OK &&
        sxml_xslt_stylesheet_get_output_definition(outputs->stylesheet, (sxml_name_identifier){.index = 0},
                                                   &definition) == SXML_STATUS_OK) {
        answer->serialization_parameters = definition.serialization_parameters;
        if (!definition.has_serialization_method) {
            answer->serialization_parameters.serialization_method = SXML_SERIALIZATION_METHOD_XML;
        }
    }
    sxml_xslt_test_invocation_names names = {.initial_template = {.index = 0},
                                             .initial_mode = {.index = 0},
                                             .initial_function = {.index = 0},
                                             .selects_unnamed_mode = false};
    if (status == SXML_STATUS_OK) {
        sxml_xslt_test_set_stage(context,
                                 (sxml_xslt_test_stage){.description = SXML_TEXT("transformation"),
                                                        .subject = sxml_xslt_test_empty_text});
        status = sxml_xslt_test_invocation_names_of(context, &names);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_base_output_uri(context, &answer->base_output_uri);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_result_document_collector_create(
            context->allocator,
            &(sxml_xslt_result_document_collector_configuration){
                .allocator = context->allocator,
                .maximum_byte_count = 0,
                .result_delivery = SXML_XSLT_RESULT_DELIVERY_DEFAULT},
            &outputs->collector);
    }
    if (status == SXML_STATUS_OK && delivery == SXML_XSLT_RESULT_DELIVERY_SERIALIZED) {
        status = sxml_output_buffer_create(
            context->allocator,
            &(sxml_output_buffer_configuration){
                .allocator = context->allocator,
                .maximum_byte_count = SXML_XSLT_TEST_MAXIMUM_OUTPUT_BYTE_COUNT},
            &outputs->output_buffer);
    }
    if (status == SXML_STATUS_OK) {
        sxml_dynamic_context_configuration dynamic_context;
        memset(&dynamic_context, 0, sizeof dynamic_context);
        dynamic_context.has_context_value = context->has_global_context_item;
        dynamic_context.context_value = context->global_context_item;
        dynamic_context.variable_bindings = (sxml_variable_binding_view){
            .variable_bindings = context->stylesheet_parameters.bindings,
            .variable_binding_count = context->stylesheet_parameters.binding_count};
        dynamic_context.available_documents = (sxml_available_document_view){
            .available_documents = context->available_documents,
            .available_document_count = context->available_document_count};
        dynamic_context.trace_sink = (sxml_trace_sink){
            .state = context, .operations = &sxml_xslt_test_trace_operations};
        memset(&context->error_record, 0, sizeof context->error_record);
        sxml_sequence principal = sxml_sequence_empty();
        status = sxml_xslt_stylesheet_transform(
            outputs->stylesheet,
            &(sxml_xslt_transformation_request){
                .dynamic_context = dynamic_context,
                .initial_match_selection = context->initial_match_selection,
                .template_parameters = {.variable_bindings = context->template_parameters.bindings,
                                        .variable_binding_count =
                                            context->template_parameters.binding_count},
                .tunnel_parameters = {.variable_bindings = context->tunnel_parameters.bindings,
                                      .variable_binding_count =
                                          context->tunnel_parameters.binding_count},
                .function_arguments = {.sequences = context->function_arguments,
                                       .sequence_count = context->function_argument_count},
                .base_output_uri = answer->base_output_uri,
                .output_definition = NULL,
                .output_stream = outputs->output_buffer == NULL
                                     ? (sxml_output_stream){.state = NULL, .operations = NULL}
                                     : sxml_output_buffer_as_output_stream(outputs->output_buffer),
                .result_document_handler =
                    sxml_xslt_result_document_collector_as_handler(outputs->collector),
                .error_record = &context->error_record,
                .initial_mode_name = names.initial_mode,
                .initial_template_name = names.initial_template,
                .initial_function_name = names.initial_function,
                .invocation_kind = sxml_xslt_test_invocation_kind_of(context),
                .result_delivery = delivery,
                .has_initial_match_selection = context->has_initial_match_selection,
                .selects_unnamed_mode = names.selects_unnamed_mode},
            &principal);
        answer->value = principal;
    }
    if (status == SXML_STATUS_OK && delivery == SXML_XSLT_RESULT_DELIVERY_SERIALIZED) {
        answer->is_serialized = true;
        answer->serialized_bytes = sxml_output_buffer_get_bytes(outputs->output_buffer);
        status = sxml_xslt_test_parse_back(context, answer->serialized_bytes, &answer->value);
    }
    answer->status = status;
}

/* ------------------------------------------------------------- The test */

/* Ends the reason with "STAGE[ SUBJECT]: ERROR" as an error outcome. */
static void sxml_xslt_test_describe_failure(const sxml_xslt_test_context *context,
                                            sxml_status status,
                                            sxml_xslt_test_answer *answer) {
    answer->outcome = SXML_QT4_TEST_OUTCOME_ERROR;
    sxml_qt4_reason_append(answer->reason, context->stage);
    if (context->stage_subject.byte_count != 0) {
        sxml_qt4_reason_append(answer->reason, SXML_TEXT(" "));
        sxml_qt4_reason_append(answer->reason, context->stage_subject);
    }
    sxml_qt4_reason_append(answer->reason, SXML_TEXT(": "));
    sxml_qt4_reason_append_error(
        answer->reason,
        (sxml_qt4_raised_error){.status = status, .error_record = &context->error_record});
}

/* The raised error, copied into the arena: the assertions raise errors
   on the same heap (as the QT4 driver does). */
static sxml_error_record sxml_xslt_test_keep_error(const sxml_xslt_test_context *context) {
    sxml_error_record raised = context->error_record;
    sxml_byte_view *const kept_views[] = {&raised.code.namespace_uri, &raised.code.local_name,
                                          &raised.code.namespace_prefix,
                                          &raised.diagnostic.description};
    for (size_t view_index = 0; view_index < sizeof kept_views / sizeof kept_views[0];
         view_index += 1) {
        sxml_byte_view copy = sxml_xslt_test_empty_text;
        if (sxml_qt4_concatenate(context->request->run->arena,
                                 (sxml_qt4_concatenation){.first = *kept_views[view_index],
                                                          .second = sxml_xslt_test_empty_text},
                                 &copy) != SXML_STATUS_OK) {
            copy = sxml_xslt_test_empty_text;
        }
        *kept_views[view_index] = copy;
    }
    raised.value = NULL;
    raised.diagnostic.location.source_name = sxml_xslt_test_empty_text;
    return raised;
}

/* What the assertions are checked against. */
typedef struct sxml_xslt_test_outcome_view {
    const sxml_xslt_test_transformation_answer *transformation;
    const sxml_xslt_test_outputs *outputs;
} sxml_xslt_test_outcome_view;

/* Checks the assertions against what the transformation gave. */
static void sxml_xslt_test_judge(sxml_xslt_test_context *context,
                                 sxml_xslt_test_outcome_view outcome,
                                 sxml_xslt_test_answer *answer) {
    const sxml_xslt_test_transformation_answer *transformation = outcome.transformation;
    const sxml_xslt_test_outputs *outputs = outcome.outputs;
    const sxml_xslt_test_run_request *run = context->request->run;
    const sxml_error_record raised = sxml_xslt_test_keep_error(context);
    sxml_xslt_test_set_stage(context,
                             (sxml_xslt_test_stage){.description = SXML_TEXT("assertion"),
                                                    .subject = sxml_xslt_test_empty_text});
    memset(&context->error_record, 0, sizeof context->error_record);
    sxml_qt4_assertion_verdict verdict = {.verdict_kind = SXML_QT4_VERDICT_FAIL,
                                          .reason = sxml_xslt_test_empty_text};
    const sxml_status status = sxml_xslt_test_check_assertions(
        &(sxml_xslt_test_check_request){
            .base = {.processor = context->processor,
                     .static_context = sxml_xslt_test_static_context(context),
                     .document = sxml_xslt_test_catalog_get_test_set_document(run->catalog),
                     .result_element = context->test_case->result,
                     .evaluation = {.status = transformation->status,
                                    .value = transformation->value,
                                    .error_record = &raised,
                                    .serialization_parameters =
                                        transformation->serialization_parameters},
                     .catalog = sxml_xslt_test_catalog_get_file_catalog(run->catalog),
                     .test_set_file_path = context->test_case->test_set_file_path,
                     .allocator = context->allocator,
                     .arena = run->arena,
                     .normalizes_string_values_by_default = true},
            .serialized_bytes = transformation->serialized_bytes,
            .is_serialized = transformation->is_serialized,
            .messages = {.sequences = context->messages, .sequence_count = context->message_count},
            .collector = outputs->collector,
            .base_output_uri = transformation->base_output_uri},
        &verdict);
    if (status != SXML_STATUS_OK) {
        sxml_xslt_test_describe_failure(context, status, answer);
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
    sxml_qt4_reason_append(answer->reason, verdict.reason);
}

/* Runs the stylesheet and judges, or describes why it could not run. */
static void sxml_xslt_test_run_transformation(sxml_xslt_test_context *context,
                                              sxml_xslt_test_answer *answer) {
    sxml_xslt_test_outputs outputs = {.stylesheet = NULL, .collector = NULL, .output_buffer = NULL};
    sxml_xslt_test_transformation_answer transformation = {
        .status = SXML_STATUS_OK,
        .value = sxml_sequence_empty(),
        .serialized_bytes = sxml_xslt_test_empty_text,
        .is_serialized = false,
        .base_output_uri = sxml_xslt_test_empty_text};
    memset(&transformation.serialization_parameters, 0,
           sizeof transformation.serialization_parameters);
    sxml_xslt_test_transform(context, &outputs, &transformation);
    const sxml_status status = transformation.status;
    if (status == SXML_STATUS_UNSUPPORTED_FEATURE) {
        answer->outcome = SXML_QT4_TEST_OUTCOME_FAIL;
        sxml_qt4_reason_append(answer->reason, SXML_TEXT("unsupported: "));
        sxml_qt4_reason_append(answer->reason, context->error_record.diagnostic.description);
    } else if (status != SXML_STATUS_OK && !sxml_status_has_error_record(status)) {
        sxml_xslt_test_describe_failure(context, status, answer);
    } else {
        sxml_xslt_test_judge(
            context,
            (sxml_xslt_test_outcome_view){.transformation = &transformation, .outputs = &outputs},
            answer);
    }
    sxml_sequence_release(transformation.value);
    sxml_xslt_test_release_outputs(&outputs);
}

/* Releases everything the test holds; the budget then has nothing live. */
static void sxml_xslt_test_release_context(sxml_xslt_test_context *context) {
    for (size_t message_index = 0; message_index < context->message_count; message_index += 1) {
        sxml_sequence_release(context->messages[message_index]);
    }
    context->message_count = 0;
    for (size_t value_index = 0; value_index < context->owned_value_count; value_index += 1) {
        sxml_sequence_release(context->owned_values[value_index]);
    }
    context->owned_value_count = 0;
    if (context->processor != NULL) {
        sxml_item_heap_clear_raised_error(sxml_processor_get_item_heap(context->processor));
    }
    sxml_processor_destroy(context->processor);
    sxml_arena_destroy(context->scratch);
}

void sxml_xslt_test_execute(const sxml_xslt_test_execution_request *request,
                            sxml_xslt_test_answer *answer) {
    const sxml_xslt_test_run_request *run = request->run;
    sxml_xslt_test_budget budget = {.backing_allocator = run->allocator,
                                    .live_byte_count = 0,
                                    .live_allocation_count = 0,
                                    .was_exceeded = false};
    const sxml_allocator allocator = {.state = &budget,
                                      .operations = &sxml_xslt_test_budget_operations};
    /* Large (messages): in allocator memory, not on the stack. */
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = sizeof(sxml_xslt_test_context),
                                  .alignment = alignof(sxml_xslt_test_context)},
        &memory);
    if (status != SXML_STATUS_OK) {
        answer->outcome = SXML_QT4_TEST_OUTCOME_ERROR;
        sxml_qt4_reason_append(answer->reason, SXML_TEXT("out of memory"));
        return;
    }
    sxml_xslt_test_context *context = memory;
    memset(context, 0, sizeof *context);
    context->request = request;
    context->test_case = run->test_case;
    context->allocator = allocator;
    context->stage = SXML_TEXT("processor");
    context->resource_table = (sxml_qt4_resource_table){
        .catalog = sxml_xslt_test_catalog_get_file_catalog(run->catalog),
        .allocator = allocator,
        .served_resources = {.served_resources = NULL, .served_resource_count = 0},
        .served_collections = {.served_collections = NULL, .served_collection_count = 0},
        .served_modules = {.served_modules = NULL, .served_module_count = 0}};
    status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .limits = {.xml_limits = {.maximum_depth = 0,
                                      .maximum_entity_expansion_byte_count =
                                          SXML_XSLT_TEST_MAXIMUM_EXPANSION_BYTE_COUNT},
                       .maximum_evaluation_step_count = SXML_QT4_TEST_MAXIMUM_STEP_COUNT},
            .resource_resolver = {.state = context,
                                  .operations = &sxml_xslt_test_resolver_operations},
            .clock = sxml_system_clock()},
        &context->processor);
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_create(
            allocator,
            &(sxml_arena_configuration){.block_allocator = allocator,
                                        .block_byte_count = SXML_XSLT_TEST_SCRATCH_BLOCK_BYTE_COUNT,
                                        .maximum_block_byte_count = 0,
                                        .initial_memory = {.bytes = NULL, .byte_count = 0}},
            &context->scratch);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_test_apply_environment(context);
    }
    if (status == SXML_STATUS_OK) {
        sxml_xslt_test_run_transformation(context, answer);
    } else {
        sxml_xslt_test_describe_failure(context, status, answer);
    }
    sxml_xslt_test_release_context(context);
    sxml_allocator_release(allocator, context);
    if (budget.was_exceeded) {
        sxml_qt4_reason_append(answer->reason, SXML_TEXT(" (memory limit)"));
    }
    if (budget.live_allocation_count != 0) {
        answer->outcome = SXML_QT4_TEST_OUTCOME_ERROR;
        sxml_qt4_reason_append(answer->reason, SXML_TEXT(" [leaked "));
        sxml_qt4_reason_append_decimal(answer->reason, budget.live_allocation_count);
        sxml_qt4_reason_append(answer->reason, SXML_TEXT(" allocations]"));
    }
}
