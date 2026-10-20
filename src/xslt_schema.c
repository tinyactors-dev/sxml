/*
 * The schema imports of a stylesheet (src/xslt_schema_internal.h; XSLT
 * 4.0, section 3.13; D179). Golden: src/xquery_static.c (XQuery's import
 * schema, D174).
 */
#include "xslt_schema_internal.h"

#include "processor_internal.h"
#include "schema_scope_internal.h"
#include "xdm_internal.h"
#include "xslt_package_internal.h"

#include "sxml/schema.h"

#include <stdalign.h>
#include <string.h>

static const sxml_byte_view no_text = {.bytes = NULL, .byte_count = 0};

/* One xsl:import-schema as 3.13 reads it. */
typedef struct schema_import {
    sxml_xslt_ast_node_index node;
    /* The inline xs:schema element; tree NULL for none. */
    sxml_node_reference inline_schema;
    /* The effective namespace: the namespace attribute, else the inline
       schema's targetNamespace; has_namespace false for none. */
    bool has_namespace;
    sxml_byte_view namespace_text;
    /* The schema-location attribute as written; empty for none. */
    sxml_byte_view location;
    sxml_byte_view base_uri;
    uint32_t import_precedence;
    /* Taken from a host schema, or a lower import precedence's. */
    bool is_skipped;
    /* The location resolved, owned, while the schema compiles. */
    void *location_memory;
} schema_import;

typedef struct import_work {
    const sxml_xslt_schema_request *request;
    schema_import *imports;
    size_t import_count;
} import_work;

static sxml_source_location location_of(const import_work *work, sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast *ast = work->request->ast;
    const sxml_xslt_ast_node *declaration = sxml_xslt_ast_get_node(ast, node);
    return (sxml_source_location){
        .source_name = sxml_xslt_ast_get_module(ast, declaration->module)->module_uri,
        .line_number = declaration->source_span.line_number,
        .column_number = declaration->source_span.column_number,
        .byte_offset = declaration->source_span.byte_offset};
}

typedef struct import_error {
    sxml_xslt_ast_node_index node;
    const char *error_code;
    const char *description;
} import_error;

static sxml_status raise_import_error(const import_work *work, import_error error) {
    sxml_error_record *record = work->request->error_record;
    if (record != NULL) {
        *record = (sxml_error_record){
            .code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(SXML_ERROR_NAMESPACE_URI),
                     .local_name = sxml_byte_view_from_c_string(error.error_code),
                     .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err")},
            .value = NULL,
            .diagnostic = {.description = sxml_byte_view_from_c_string(error.description),
                           .location = location_of(work, error.node)}};
    }
    return SXML_STATUS_STATIC_ERROR;
}

/* The text of a TEXT attribute of node; *is_present false without one. */
typedef struct attribute_text {
    sxml_byte_view text;
    bool is_present;
} attribute_text;

static attribute_text read_attribute_text(const sxml_xslt_ast *ast,
                                          sxml_xslt_ast_attribute_query query) {
    const sxml_xslt_ast_attribute_index index = sxml_xslt_ast_find_attribute(ast, query);
    const sxml_xslt_ast_attribute *attribute = sxml_xslt_ast_get_attribute(ast, index);
    if (index.value == 0 || attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_TEXT) {
        return (attribute_text){.text = no_text, .is_present = false};
    }
    return (attribute_text){.text = sxml_xslt_ast_get_text(ast, attribute->value.payload.text),
                            .is_present = true};
}

/* True when name is local_name in namespace (an XSD element or an
   unprefixed attribute). */
typedef struct name_query {
    sxml_name_identifier name;
    sxml_namespace_uri_identifier namespace_uri;
    const char *local_name;
} name_query;

static bool is_named(const sxml_processor *processor, name_query query) {
    sxml_name_parts parts;
    return sxml_name_pool_get_name_parts(processor->name_pool, query.name, &parts) ==
               SXML_STATUS_OK &&
           parts.namespace_uri.index == query.namespace_uri.index &&
           sxml_byte_view_equals(parts.local_name, sxml_byte_view_from_c_string(query.local_name));
}

/* The xs:schema child of declaration's element in its module's tree, and
   its targetNamespace. */
static sxml_status find_inline_schema(const import_work *work, schema_import *import) {
    const sxml_xslt_ast *ast = work->request->ast;
    const sxml_xslt_ast_node *declaration = sxml_xslt_ast_get_node(ast, import->node);
    const sxml_xslt_ast_module *module = sxml_xslt_ast_get_module(ast, declaration->module);
    sxml_node_description description;
    sxml_status status = sxml_node_describe((sxml_node_reference){.tree = module->tree,
                                                                  .node_index =
                                                                      declaration->source_node,
                                                                  .namespace_number = 0},
                                            &description);
    bool has_child = status == SXML_STATUS_OK && description.has_children;
    sxml_node_reference child = {
        .tree = module->tree, .node_index = description.first_child, .namespace_number = 0};
    while (has_child) {
        sxml_node_description child_description;
        status = sxml_node_describe(child, &child_description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (child_description.node_kind == SXML_NODE_KIND_ELEMENT &&
            is_named(work->request->processor,
                     (name_query){.name = child_description.node_name,
                                  .namespace_uri = {.index = SXML_NAMESPACE_XML_SCHEMA},
                                  .local_name = "schema"})) {
            import->inline_schema = child;
            /* Its attributes follow it (include/sxml/tree.h). */
            for (uint32_t offset = 1; offset <= child_description.attribute_count; offset += 1) {
                const sxml_node_reference attribute = {
                    .tree = module->tree,
                    .node_index = {.index = child.node_index.index + offset},
                    .namespace_number = 0};
                sxml_node_description attribute_description;
                status = sxml_node_describe(attribute, &attribute_description);
                if (status == SXML_STATUS_OK &&
                    is_named(work->request->processor,
                             (name_query){.name = attribute_description.node_name,
                                          .namespace_uri = {.index = SXML_NAMESPACE_NONE},
                                          .local_name = "targetNamespace"})) {
                    status = sxml_node_get_content(attribute, &import->namespace_text);
                    import->has_namespace = true;
                }
                if (status != SXML_STATUS_OK) {
                    return status;
                }
            }
            return SXML_STATUS_OK;
        }
        has_child = child_description.has_next_sibling;
        child.node_index = child_description.next_sibling;
    }
    return status;
}

/* Reads one declaration (XTSE0215). */
static sxml_status read_import(const import_work *work, schema_import *import) {
    const sxml_xslt_ast *ast = work->request->ast;
    const sxml_xslt_ast_node *declaration = sxml_xslt_ast_get_node(ast, import->node);
    const attribute_text namespace_attribute = read_attribute_text(
        ast, (sxml_xslt_ast_attribute_query){.node = import->node,
                                             .kind = SXML_XSLT_AST_ATTRIBUTE_NAMESPACE});
    const attribute_text location = read_attribute_text(
        ast, (sxml_xslt_ast_attribute_query){.node = import->node,
                                             .kind = SXML_XSLT_AST_ATTRIBUTE_SCHEMA_LOCATION});
    import->location = location.text;
    import->base_uri = sxml_xslt_ast_get_text(
        ast, sxml_xslt_ast_get_static_context(ast, declaration->static_context)->static_base_uri);
    import->import_precedence =
        sxml_xslt_ast_get_module(ast, declaration->module)->import_precedence;
    sxml_status status = find_inline_schema(work, import);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (import->inline_schema.tree == NULL) {
        import->has_namespace = namespace_attribute.is_present;
        import->namespace_text = namespace_attribute.text;
        return SXML_STATUS_OK;
    }
    /* 3.13: an inline schema has no schema-location, and a namespace
       attribute only when it is its targetNamespace. */
    if (location.is_present ||
        (namespace_attribute.is_present &&
         (!import->has_namespace ||
          !sxml_byte_view_equals(namespace_attribute.text, import->namespace_text)))) {
        return raise_import_error(
            work, (import_error){.node = import->node,
                                 .error_code = "XTSE0215",
                                 .description = "xsl:import-schema with an inline xs:schema has a "
                                                "schema-location, or a namespace other than the "
                                                "schema's targetNamespace (XSLT 4.0, 3.13)"});
    }
    return SXML_STATUS_OK;
}

static bool is_same_namespace(const schema_import *first, const schema_import *second) {
    return first->has_namespace == second->has_namespace &&
           (!first->has_namespace ||
            sxml_byte_view_equals(first->namespace_text, second->namespace_text));
}

/* 3.13: of the declarations of one namespace only those of the highest
   import precedence; none of a namespace a host schema defines (nor of
   XSD's, whose components are built in). */
static sxml_status skip_imports(const import_work *work) {
    const sxml_schema_view hosts = work->request->host_schemas;
    for (size_t import_index = 0; import_index < work->import_count; import_index += 1) {
        schema_import *import = &work->imports[import_index];
        for (size_t other_index = 0; other_index < work->import_count; other_index += 1) {
            const schema_import *other = &work->imports[other_index];
            if (is_same_namespace(import, other) &&
                other->import_precedence > import->import_precedence) {
                import->is_skipped = true;
            }
        }
        sxml_namespace_uri_identifier namespace_uri = {.index = SXML_NAMESPACE_NONE};
        if (import->has_namespace) {
            const sxml_status status = sxml_name_pool_intern_namespace_uri(
                work->request->processor->name_pool, import->namespace_text, &namespace_uri);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        if (namespace_uri.index == SXML_NAMESPACE_XML_SCHEMA) {
            import->is_skipped = true;
        }
        for (size_t host_index = 0; host_index < hosts.schema_count; host_index += 1) {
            if (sxml_schema_scope_defines_namespace(hosts.schemas[host_index], namespace_uri)) {
                import->is_skipped = true;
            }
        }
    }
    return SXML_STATUS_OK;
}

/* The document of one import, or none (*has_document false) when the
   resolver delivers neither its location nor its namespace. */
typedef struct import_document {
    sxml_schema_document *document;
    bool *has_document;
} import_document;

static sxml_status make_document(const import_work *work, schema_import *import,
                                 import_document answer) {
    sxml_schema_document *document = answer.document;
    *answer.has_document = false;
    if (import->inline_schema.tree != NULL) {
        *document = (sxml_schema_document){.document_source = SXML_SCHEMA_DOCUMENT_SOURCE_NODE,
                                           .document_node =
                                               sxml_node_item_make(import->inline_schema),
                                           /* Its errors' source name and the base
                                              URI of its references (nodes are never
                                              taken for one another by URI). */
                                           .document_uri = import->base_uri,
                                           .checks_target_namespace = false};
        *answer.has_document = true;
        return SXML_STATUS_OK;
    }
    sxml_processor *processor = work->request->processor;
    *document = (sxml_schema_document){.document_source = SXML_SCHEMA_DOCUMENT_SOURCE_RESOLVED,
                                       .document_uri = no_text,
                                       .checks_target_namespace = true,
                                       .target_namespace_uri = import->namespace_text};
    sxml_status status = SXML_STATUS_OK;
    if (import->location.byte_count > 0) {
        /* The hint first, made absolute: the schema's base URI is not the
           declaration's. */
        sxml_uri_resolution resolution = {.uri = no_text, .memory = NULL};
        status = sxml_uri_resolve(&(sxml_uri_resolution_request){.reference = import->location,
                                                                 .base_uri = import->base_uri,
                                                                 .allocator = processor->allocator},
                                  &resolution);
        sxml_schema_document hinted = *document;
        hinted.document_uri = resolution.uri;
        import->location_memory = resolution.memory;
        sxml_schema_scope_delivery delivery = {.is_delivered = false};
        if (status == SXML_STATUS_OK) {
            status = sxml_schema_scope_probe_document(
                &(sxml_schema_scope_probe){
                    .processor = processor, .document = hinted, .base_uri = no_text},
                &delivery);
        }
        *answer.has_document = delivery.is_delivered;
        if (status == SXML_STATUS_OK && *answer.has_document) {
            *document = hinted;
            return SXML_STATUS_OK;
        }
    }
    sxml_schema_scope_delivery delivery = {.is_delivered = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_scope_probe_document(
            &(sxml_schema_scope_probe){
                .processor = processor, .document = *document, .base_uri = no_text},
            &delivery);
    }
    *answer.has_document = delivery.is_delivered;
    return status;
}

static void release_documents(const import_work *work, sxml_schema_document *documents,
                              size_t document_count) {
    for (size_t document_index = 0; document_index < document_count; document_index += 1) {
        sxml_schema_document *document = &documents[document_index];
        if (document->document_source == SXML_SCHEMA_DOCUMENT_SOURCE_NODE) {
            sxml_item_release(document->document_node);
        }
    }
    sxml_allocator_release(work->request->processor->allocator, documents);
}

/* Allocates count elements of size and alignment from the processor's
   allocator. */
typedef struct array_request {
    size_t element_count;
    size_t element_size;
    size_t element_alignment;
} array_request;

static sxml_status allocate_array(const import_work *work, array_request request, void **memory) {
    size_t byte_count = 0;
    sxml_status status =
        sxml_size_multiply(request.element_count, request.element_size, &byte_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            work->request->processor->allocator,
            (sxml_allocation_request){.byte_count = byte_count == 0 ? 1 : byte_count,
                                      .alignment = request.element_alignment},
            memory);
    }
    if (status == SXML_STATUS_OK) {
        memset(*memory, 0, byte_count);
    }
    return status;
}

/* Compiles the imports not skipped into *schema (NULL when none has a
   document); XTSE0220 for an invalid synthetic schema document. */
static sxml_status compile_imports(const import_work *work, sxml_schema **schema) {
    void *memory = NULL;
    sxml_status status = allocate_array(
        work, (array_request){.element_count = work->import_count,
                              .element_size = sizeof(sxml_schema_document),
                              .element_alignment = alignof(sxml_schema_document)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_schema_document *documents = memory;
    size_t document_count = 0;
    sxml_xslt_ast_node_index first_node = {.value = 0};
    for (size_t import_index = 0; status == SXML_STATUS_OK && import_index < work->import_count;
         import_index += 1) {
        schema_import *import = &work->imports[import_index];
        if (import->is_skipped) {
            continue;
        }
        bool has_document = false;
        status = make_document(work, import,
                               (import_document){.document = &documents[document_count],
                                                 .has_document = &has_document});
        if (status == SXML_STATUS_OK && has_document) {
            document_count += 1;
            first_node = first_node.value == 0 ? import->node : first_node;
        }
    }
    sxml_processor *processor = work->request->processor;
    sxml_error_record schema_error;
    memset(&schema_error, 0, sizeof schema_error);
    if (status == SXML_STATUS_OK && document_count > 0) {
        status = sxml_schema_create(
            processor->allocator,
            &(sxml_schema_configuration){
                .processor = processor,
                .schema_documents = {.schema_documents = documents,
                                     .schema_document_count = document_count},
                .base_uri = no_text,
                .report_sink = {.state = NULL, .operations = NULL},
                .maximum_content_model_state_count = 0,
                .error_record = &schema_error},
            schema);
        if (status == SXML_STATUS_STATIC_ERROR && schema_error.code.local_name.byte_count != 0) {
            /* 3.13: the synthetic schema document breaks XSD's constraints. */
            status = sxml_schema_scope_raise(&(sxml_schema_scope_error){
                .item_heap = processor->item_heap,
                .allocator = processor->allocator,
                .error_status = SXML_STATUS_STATIC_ERROR,
                .error_code = SXML_BYTE_VIEW_FROM_LITERAL("XTSE0220"),
                .schema_error = &schema_error,
                .error_record = work->request->error_record,
                .location = location_of(work, first_node)});
        }
    }
    release_documents(work, documents, document_count);
    return status;
}

/* The host's schemas, then schema (or the built-in components' schema
   when there is nothing else), as the AST's in-scope schemas. */
static sxml_status set_in_scope_schemas(const import_work *work, const sxml_schema *schema) {
    const sxml_schema_view hosts = work->request->host_schemas;
    const sxml_schema *builtin_schema = NULL;
    sxml_status status = SXML_STATUS_OK;
    if (schema == NULL && hosts.schema_count == 0) {
        status = sxml_processor_get_builtin_schema(work->request->processor, &builtin_schema);
        schema = builtin_schema;
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_array(
            work, (array_request){.element_count = hosts.schema_count + 1,
                                  .element_size = sizeof(const sxml_schema *),
                                  .element_alignment = alignof(const sxml_schema *)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_schema **schemas = (const sxml_schema **)memory;
    for (size_t host_index = 0; host_index < hosts.schema_count; host_index += 1) {
        schemas[host_index] = hosts.schemas[host_index];
    }
    size_t schema_count = hosts.schema_count;
    if (schema != NULL) {
        schemas[schema_count] = schema;
        schema_count += 1;
    }
    status = sxml_xslt_ast_set_in_scope_schemas(
        work->request->ast, (sxml_schema_view){.schemas = schemas, .schema_count = schema_count});
    sxml_allocator_release(work->request->processor->allocator, memory);
    return status;
}

/* 3.13.1, XTSE4045: every [xsl:]schema-role names the role of an
   xsl:import-schema of its package. (Only the check: the roles' schemas
   are not kept apart yet, every import is in every static context.) */
static sxml_status check_schema_roles(const import_work *work) {
    const sxml_xslt_ast *ast = work->request->ast;
    for (uint32_t node_index = 1; node_index < ast->node_count; node_index += 1) {
        const sxml_xslt_ast_node_index node = {.value = node_index};
        const attribute_text role = read_attribute_text(
            ast, (sxml_xslt_ast_attribute_query){.node = node,
                                                 .kind = SXML_XSLT_AST_ATTRIBUTE_SCHEMA_ROLE});
        if (!role.is_present) {
            continue;
        }
        const sxml_xslt_ast_module_index package = sxml_xslt_package_top_module(ast, node);
        bool is_declared = false;
        for (size_t import_index = 0; !is_declared && import_index < work->import_count;
             import_index += 1) {
            const sxml_xslt_ast_node_index import = work->imports[import_index].node;
            /* role is an NCName, read as a name in no namespace. */
            const sxml_xslt_ast_attribute *declared = sxml_xslt_ast_get_attribute(
                ast, sxml_xslt_ast_find_attribute(
                         ast, (sxml_xslt_ast_attribute_query){
                                  .node = import, .kind = SXML_XSLT_AST_ATTRIBUTE_ROLE}));
            sxml_name_parts parts = {.local_name = no_text};
            if (declared->value.kind == SXML_XSLT_AST_VALUE_KIND_NAME) {
                const sxml_status status = sxml_name_pool_get_name_parts(
                    work->request->processor->name_pool, declared->value.payload.name, &parts);
                if (status != SXML_STATUS_OK) {
                    return status;
                }
            }
            is_declared = declared->value.kind == SXML_XSLT_AST_VALUE_KIND_NAME &&
                          sxml_byte_view_equals(parts.local_name, role.text) &&
                          sxml_xslt_package_top_module(ast, import).value == package.value;
        }
        if (!is_declared) {
            return raise_import_error(
                work, (import_error){.node = node,
                                     .error_code = "XTSE4045",
                                     .description = "a schema-role names no role of an "
                                                    "xsl:import-schema of the package (XSLT "
                                                    "4.0, 3.13.1)"});
        }
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_xslt_import_schemas(const sxml_xslt_schema_request *request) {
    if (request == NULL || request->ast == NULL || request->processor == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_xslt_ast *ast = request->ast;
    size_t import_count = 0;
    for (uint32_t node_index = 1; node_index < ast->node_count; node_index += 1) {
        import_count += ast->nodes[node_index].kind == SXML_XSLT_AST_KIND_IMPORT_SCHEMA ? 1 : 0;
    }
    import_work work = {.request = request, .imports = NULL, .import_count = 0};
    void *memory = NULL;
    sxml_status status = allocate_array(
        &work, (array_request){.element_count = import_count,
                               .element_size = sizeof(schema_import),
                               .element_alignment = alignof(schema_import)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    work.imports = memory;
    for (uint32_t node_index = 1; status == SXML_STATUS_OK && node_index < ast->node_count;
         node_index += 1) {
        if (ast->nodes[node_index].kind != SXML_XSLT_AST_KIND_IMPORT_SCHEMA) {
            continue;
        }
        schema_import *import = &work.imports[work.import_count];
        *import = (schema_import){.node = {.value = node_index},
                                  .inline_schema = {.tree = NULL},
                                  .has_namespace = false,
                                  .namespace_text = no_text,
                                  .location = no_text,
                                  .base_uri = no_text,
                                  .import_precedence = 0,
                                  .is_skipped = false,
                                  .location_memory = NULL};
        work.import_count += 1;
        status = read_import(&work, import);
    }
    if (status == SXML_STATUS_OK) {
        status = check_schema_roles(&work);
    }
    if (status == SXML_STATUS_OK) {
        status = skip_imports(&work);
    }
    sxml_schema *schema = NULL;
    if (status == SXML_STATUS_OK) {
        status = compile_imports(&work, &schema);
    }
    if (status == SXML_STATUS_OK && schema != NULL) {
        status = sxml_processor_keep_schema(request->processor, schema);
        if (status != SXML_STATUS_OK) {
            sxml_schema_destroy(schema);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = set_in_scope_schemas(&work, schema);
    }
    for (size_t import_index = 0; import_index < work.import_count; import_index += 1) {
        sxml_allocator_release(request->processor->allocator,
                               work.imports[import_index].location_memory);
    }
    sxml_allocator_release(request->processor->allocator, memory);
    return status;
}
