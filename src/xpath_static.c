/*
 * Static analysis of XPath 4.0: the static context, names and types
 * (src/xpath_static_internal.h; docs/DECISIONS.md, D51, D55, D63).
 *
 * Golden file: src/memory.c. Nothing here keeps state between calls: the
 * statically known namespaces are searched where they are written (the
 * AST's namespace declarations, then the configuration, then the
 * predeclared prefixes), so resolving a name allocates nothing beyond what
 * the name pool interns.
 */
#include "xpath_static_internal.h"

#include <stdalign.h>
#include <string.h>

/* ---------------------------------------------------------------- Errors */

sxml_status sxml_xpath_report_static_error(const sxml_xpath_static_error *error,
                                           sxml_error_record *error_record) {
    if (error_record != NULL) {
        *error_record = (sxml_error_record){
            .code = {.namespace_uri =
                         SXML_BYTE_VIEW_FROM_LITERAL(SXML_ERROR_NAMESPACE_URI),
                     .local_name = error->code_local_name,
                     .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err")},
            .value = NULL,
            .diagnostic = {.description = error->description,
                           .location = sxml_ast_get_source_location(
                               error->ast, error->node)},
        };
    }
    return SXML_STATUS_STATIC_ERROR;
}

typedef struct static_error_site {
    const sxml_ast *ast;
    sxml_ast_node_index node;
    sxml_error_record *error_record;
} static_error_site;

static sxml_status report_at(static_error_site site, sxml_byte_view code,
                             sxml_byte_view description) {
    return sxml_xpath_report_static_error(
        &(sxml_xpath_static_error){.ast = site.ast,
                                   .node = site.node,
                                   .code_local_name = code,
                                   .description = description},
        site.error_record);
}

/* Fills only the diagnostic of the record: for SXML_STATUS_UNSUPPORTED_
   FEATURE, which is not a specification error. */
static sxml_status report_unsupported(static_error_site site,
                                      sxml_byte_view description) {
    if (site.error_record != NULL) {
        site.error_record->diagnostic = (sxml_diagnostic){
            .description = description,
            .location = sxml_ast_get_source_location(site.ast, site.node)};
    }
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}

/* ------------------------------------------------------------ Namespaces */

/* The predeclared prefixes (sxml_static_context_configuration; XPath 4.0,
   section 2.2.1, "statically known namespaces"). */
typedef struct predeclared_prefix {
    sxml_byte_view namespace_prefix;
    sxml_well_known_namespace namespace_uri;
} predeclared_prefix;

static const predeclared_prefix predeclared_prefixes[] = {
    {SXML_BYTE_VIEW_INITIALIZER("xml"), SXML_NAMESPACE_XML},
    {SXML_BYTE_VIEW_INITIALIZER("xs"), SXML_NAMESPACE_XML_SCHEMA},
    {SXML_BYTE_VIEW_INITIALIZER("xsi"), SXML_NAMESPACE_XML_SCHEMA_INSTANCE},
    {SXML_BYTE_VIEW_INITIALIZER("fn"), SXML_NAMESPACE_FUNCTIONS},
    {SXML_BYTE_VIEW_INITIALIZER("map"), SXML_NAMESPACE_MAP_FUNCTIONS},
    {SXML_BYTE_VIEW_INITIALIZER("array"), SXML_NAMESPACE_ARRAY_FUNCTIONS},
    {SXML_BYTE_VIEW_INITIALIZER("math"), SXML_NAMESPACE_MATH_FUNCTIONS},
    {SXML_BYTE_VIEW_INITIALIZER("err"), SXML_NAMESPACE_ERRORS},
};

static bool is_xml_whitespace(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/*
 * Interns a namespace URI written in the expression. Whitespace is
 * normalized as fn:normalize-space does (XPath 4.0, A.3.1, BracedURILiteral
 * and URILiteral: "whitespace normalized as described for xs:anyURI").
 */
typedef struct uri_text {
    sxml_byte_view text;
    const sxml_xpath_static_context *static_context;
} uri_text;

static sxml_status intern_uri_text(uri_text request,
                                   sxml_namespace_uri_identifier *uri) {
    sxml_name_pool *name_pool =
        sxml_item_heap_get_name_pool(request.static_context->item_heap);
    const sxml_byte_view text = request.text;
    bool needs_normalizing = false;
    for (size_t byte_index = 0; byte_index < text.byte_count;
         byte_index += 1) {
        if (is_xml_whitespace(text.bytes[byte_index])) {
            needs_normalizing = true;
            break;
        }
    }
    if (!needs_normalizing) {
        return sxml_name_pool_intern_namespace_uri(name_pool, text, uri);
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        request.static_context->allocator,
        (sxml_allocation_request){.byte_count = text.byte_count,
                                  .alignment = 1},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *normalized = memory;
    size_t normalized_count = 0;
    bool is_after_space = true;
    for (size_t byte_index = 0; byte_index < text.byte_count;
         byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        if (is_xml_whitespace(byte)) {
            if (!is_after_space) {
                normalized[normalized_count] = ' ';
                normalized_count += 1;
                is_after_space = true;
            }
            continue;
        }
        normalized[normalized_count] = byte;
        normalized_count += 1;
        is_after_space = false;
    }
    if (normalized_count > 0 && normalized[normalized_count - 1] == ' ') {
        normalized_count -= 1;
    }
    status = sxml_name_pool_intern_namespace_uri(
        name_pool,
        (sxml_byte_view){.bytes = normalized, .byte_count = normalized_count},
        uri);
    sxml_allocator_release(request.static_context->allocator, memory);
    return status;
}

/* A prefix to look up in the statically known namespaces. */
typedef struct prefix_lookup {
    sxml_byte_view namespace_prefix;
    const sxml_ast *ast;
    const sxml_xpath_static_context *static_context;
} prefix_lookup;

/* What a prefix lookup found. */
typedef struct prefix_binding {
    bool is_bound;
    sxml_namespace_uri_identifier namespace_uri;
} prefix_binding;

static sxml_status look_up_prefix(prefix_lookup lookup,
                                  prefix_binding *binding) {
    const sxml_ast *ast = lookup.ast;
    /* "xml" is bound to its namespace and cannot be rebound (Namespaces in
       XML 1.0, section 3). */
    if (sxml_byte_view_equals(lookup.namespace_prefix,
                              SXML_BYTE_VIEW_FROM_LITERAL("xml"))) {
        *binding = (prefix_binding){
            .is_bound = true,
            .namespace_uri =
                sxml_well_known_namespace_identifier(SXML_NAMESPACE_XML)};
        return SXML_STATUS_OK;
    }
    /* Declarations in the expression come first (XPath 4.0, A.1, the
       NamespaceDecl of the XPath production). */
    for (sxml_ast_node_index declaration = sxml_ast_find_child(
             ast, (sxml_ast_child_query){.parent = ast->root,
                                         .field = SXML_AST_FIELD_DECLARATION});
         declaration.value != 0;
         declaration = sxml_ast_find_next_with_same_field(ast, declaration)) {
        if (sxml_ast_get_node(ast, declaration)->kind !=
            SXML_AST_KIND_NAMESPACE_DECLARATION) {
            continue;
        }
        const sxml_ast_node_index prefix_node = sxml_ast_find_child(
            ast, (sxml_ast_child_query){.parent = declaration,
                                        .field = SXML_AST_FIELD_PREFIX});
        const sxml_byte_view prefix_text = sxml_ast_get_text(
            ast, sxml_ast_get_node(ast, prefix_node)->payload.name.local_name);
        if (!sxml_byte_view_equals(prefix_text, lookup.namespace_prefix)) {
            continue;
        }
        const sxml_ast_node_index uri_node = sxml_ast_find_child(
            ast, (sxml_ast_child_query){.parent = declaration,
                                        .field = SXML_AST_FIELD_URI});
        sxml_namespace_uri_identifier uri = {0};
        sxml_status status = intern_uri_text(
            (uri_text){.text = sxml_ast_get_text(
                           ast, sxml_ast_get_node(ast, uri_node)->payload.text),
                       .static_context = lookup.static_context},
            &uri);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        /* A declaration with an empty URI undeclares the prefix. */
        *binding = (prefix_binding){.is_bound = uri.index != SXML_NAMESPACE_NONE,
                                    .namespace_uri = uri};
        return SXML_STATUS_OK;
    }
    sxml_name_pool *name_pool =
        sxml_item_heap_get_name_pool(lookup.static_context->item_heap);
    const sxml_namespace_binding_view bindings =
        lookup.static_context->namespace_bindings;
    for (size_t binding_index = 0;
         binding_index < bindings.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding *candidate =
            &bindings.namespace_bindings[binding_index];
        if (candidate->namespace_prefix.index == 0) {
            continue;
        }
        sxml_byte_view prefix_text = {0};
        sxml_status status = sxml_name_pool_get_namespace_prefix(
            name_pool, candidate->namespace_prefix, &prefix_text);
        if (status != SXML_STATUS_OK) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        if (sxml_byte_view_equals(prefix_text, lookup.namespace_prefix)) {
            *binding = (prefix_binding){
                .is_bound = candidate->namespace_uri.index != 0,
                .namespace_uri = candidate->namespace_uri};
            return SXML_STATUS_OK;
        }
    }
    for (size_t prefix_index = 0;
         prefix_index <
         sizeof(predeclared_prefixes) / sizeof(predeclared_prefixes[0]);
         prefix_index += 1) {
        if (sxml_byte_view_equals(
                predeclared_prefixes[prefix_index].namespace_prefix,
                lookup.namespace_prefix)) {
            *binding = (prefix_binding){
                .is_bound = true,
                .namespace_uri = sxml_well_known_namespace_identifier(
                    predeclared_prefixes[prefix_index].namespace_uri)};
            return SXML_STATUS_OK;
        }
    }
    *binding = (prefix_binding){.is_bound = false,
                                .namespace_uri = {.index = 0}};
    return SXML_STATUS_OK;
}

/* The default namespace for elements and types: a declaration in the
   expression, else the configuration's binding of prefix 0, else none. */
typedef struct default_namespace_lookup {
    const sxml_ast *ast;
    const sxml_xpath_static_context *static_context;
} default_namespace_lookup;

static sxml_status look_up_default_element_namespace(
    default_namespace_lookup lookup, sxml_namespace_uri_identifier *uri) {
    const sxml_ast *ast = lookup.ast;
    for (sxml_ast_node_index declaration = sxml_ast_find_child(
             ast, (sxml_ast_child_query){.parent = ast->root,
                                         .field = SXML_AST_FIELD_DECLARATION});
         declaration.value != 0;
         declaration = sxml_ast_find_next_with_same_field(ast, declaration)) {
        if (sxml_ast_get_node(ast, declaration)->kind !=
            SXML_AST_KIND_DEFAULT_ELEMENT_NAMESPACE_DECLARATION) {
            continue;
        }
        const sxml_ast_node_index uri_node = sxml_ast_find_child(
            ast, (sxml_ast_child_query){.parent = declaration,
                                        .field = SXML_AST_FIELD_URI});
        return intern_uri_text(
            (uri_text){.text = sxml_ast_get_text(
                           ast, sxml_ast_get_node(ast, uri_node)->payload.text),
                       .static_context = lookup.static_context},
            uri);
    }
    const sxml_namespace_binding_view bindings =
        lookup.static_context->namespace_bindings;
    for (size_t binding_index = 0;
         binding_index < bindings.namespace_binding_count;
         binding_index += 1) {
        if (bindings.namespace_bindings[binding_index].namespace_prefix.index ==
            0) {
            *uri = bindings.namespace_bindings[binding_index].namespace_uri;
            return SXML_STATUS_OK;
        }
    }
    *uri = (sxml_namespace_uri_identifier){.index = SXML_NAMESPACE_NONE};
    return SXML_STATUS_OK;
}

sxml_status sxml_xpath_resolve_name(const sxml_xpath_name_request *request,
                                    sxml_xpath_resolved_name *resolved_name) {
    const sxml_ast *ast = request->ast;
    const sxml_ast_node *node = sxml_ast_get_node(ast, request->name_node);
    const sxml_ast_name name = node->payload.name;
    const sxml_byte_view local_name = sxml_ast_get_text(ast, name.local_name);
    const bool is_braced =
        node->kind == SXML_AST_KIND_URI_QUALIFIED_NAME ||
        (node->kind == SXML_AST_KIND_WILDCARD &&
         (node->flags & SXML_AST_FLAG_BRACED_URI) != 0);
    sxml_namespace_uri_identifier uri = {0};
    sxml_status status = SXML_STATUS_OK;
    bool is_any_namespace = false;
    if (is_braced) {
        status = intern_uri_text(
            (uri_text){.text = sxml_ast_get_text(ast, name.namespace_uri),
                       .static_context = request->static_context},
            &uri);
    } else if (name.namespace_prefix.byte_count > 0) {
        prefix_binding binding = {0};
        status = look_up_prefix(
            (prefix_lookup){
                .namespace_prefix = sxml_ast_get_text(ast, name.namespace_prefix),
                .ast = ast,
                .static_context = request->static_context},
            &binding);
        if (status == SXML_STATUS_OK && !binding.is_bound) {
            /* XPath 4.0, section 2.2.1: a prefix must be statically known. */
            return report_at(
                (static_error_site){.ast = ast,
                                    .node = request->name_node,
                                    .error_record = request->error_record},
                SXML_BYTE_VIEW_FROM_LITERAL("XPST0081"), SXML_BYTE_VIEW_FROM_LITERAL("the namespace prefix is not declared"));
        }
        uri = binding.namespace_uri;
    } else {
        switch (request->name_role) {
        case SXML_XPATH_NAME_ROLE_ELEMENT_OR_TYPE:
            status = look_up_default_element_namespace(
                (default_namespace_lookup){
                    .ast = ast, .static_context = request->static_context},
                &uri);
            is_any_namespace = request->static_context->has_any_default_element_namespace;
            break;
        case SXML_XPATH_NAME_ROLE_FUNCTION:
            switch (request->static_context->function_namespace_rule) {
            case SXML_XPATH_FUNCTION_NAMESPACE_RULE_STANDARD:
                uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_FUNCTIONS);
                break;
            case SXML_XPATH_FUNCTION_NAMESPACE_RULE_DECLARED:
                uri = request->static_context->default_function_namespace;
                break;
            case SXML_XPATH_FUNCTION_NAMESPACE_RULE_NO_NAMESPACE_FIRST:
                uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_NONE);
                break;
            }
            break;
        case SXML_XPATH_NAME_ROLE_NO_NAMESPACE:
            uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_NONE);
            break;
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *resolved_name = (sxml_xpath_resolved_name){.namespace_uri = uri,
                                                .local_name = local_name,
                                                .is_any_namespace = is_any_namespace};
    return SXML_STATUS_OK;
}

/* True when collection has a binding for prefix. */
static bool collection_binds(const sxml_xpath_namespace_collection *collection,
                             sxml_namespace_prefix_identifier prefix) {
    for (size_t binding_index = 0; binding_index < collection->binding_count;
         binding_index += 1) {
        if (collection->bindings[binding_index].namespace_prefix.index == prefix.index) {
            return true;
        }
    }
    return false;
}

sxml_status sxml_xpath_collect_namespaces(
    const sxml_ast *ast, const sxml_xpath_static_context *static_context,
    sxml_xpath_namespace_collection *collection) {
    const sxml_namespace_binding_view configured = static_context->namespace_bindings;
    size_t capacity = configured.namespace_binding_count;
    for (sxml_ast_node_index declaration = sxml_ast_find_child(
             ast, (sxml_ast_child_query){.parent = ast->root,
                                         .field = SXML_AST_FIELD_DECLARATION});
         declaration.value != 0;
         declaration = sxml_ast_find_next_with_same_field(ast, declaration)) {
        capacity += 1;
    }
    sxml_xpath_namespace_collection collected = {.bindings = NULL, .binding_count = 0};
    if (capacity == 0) {
        *collection = collected;
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        static_context->allocator,
        (sxml_allocation_request){.byte_count = capacity * sizeof(sxml_namespace_binding),
                                  .alignment = alignof(sxml_namespace_binding)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    collected.bindings = memory;
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(static_context->item_heap);
    /* The expression's declarations first (XPath 4.0, A.1), as
       look_up_prefix and look_up_default_element_namespace read them. */
    for (sxml_ast_node_index declaration = sxml_ast_find_child(
             ast, (sxml_ast_child_query){.parent = ast->root,
                                         .field = SXML_AST_FIELD_DECLARATION});
         status == SXML_STATUS_OK && declaration.value != 0;
         declaration = sxml_ast_find_next_with_same_field(ast, declaration)) {
        const sxml_ast_kind kind = sxml_ast_get_node(ast, declaration)->kind;
        if (kind != SXML_AST_KIND_NAMESPACE_DECLARATION &&
            kind != SXML_AST_KIND_DEFAULT_ELEMENT_NAMESPACE_DECLARATION) {
            continue;
        }
        sxml_namespace_prefix_identifier prefix = {.index = 0};
        if (kind == SXML_AST_KIND_NAMESPACE_DECLARATION) {
            const sxml_ast_node_index prefix_node = sxml_ast_find_child(
                ast, (sxml_ast_child_query){.parent = declaration,
                                            .field = SXML_AST_FIELD_PREFIX});
            status = sxml_name_pool_intern_namespace_prefix(
                name_pool,
                sxml_ast_get_text(ast,
                                  sxml_ast_get_node(ast, prefix_node)->payload.name.local_name),
                &prefix);
        }
        const sxml_ast_node_index uri_node = sxml_ast_find_child(
            ast, (sxml_ast_child_query){.parent = declaration, .field = SXML_AST_FIELD_URI});
        sxml_namespace_uri_identifier uri = {.index = 0};
        if (status == SXML_STATUS_OK) {
            status = intern_uri_text(
                (uri_text){.text = sxml_ast_get_text(
                               ast, sxml_ast_get_node(ast, uri_node)->payload.text),
                           .static_context = static_context},
                &uri);
        }
        if (status == SXML_STATUS_OK && !collection_binds(&collected, prefix)) {
            collected.bindings[collected.binding_count] =
                (sxml_namespace_binding){.namespace_prefix = prefix, .namespace_uri = uri};
            collected.binding_count += 1;
        }
    }
    for (size_t binding_index = 0;
         status == SXML_STATUS_OK && binding_index < configured.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding binding = configured.namespace_bindings[binding_index];
        /* An undeclared prefix (D135) is not statically known. */
        if (binding.namespace_uri.index == SXML_NAMESPACE_NONE &&
            binding.namespace_prefix.index != 0) {
            continue;
        }
        if (!collection_binds(&collected, binding.namespace_prefix)) {
            collected.bindings[collected.binding_count] = binding;
            collected.binding_count += 1;
        }
    }
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(static_context->allocator, memory);
        return status;
    }
    *collection = collected;
    return SXML_STATUS_OK;
}

/* -------------------------------------------------------------- Functions */

sxml_status sxml_xpath_find_function(sxml_function_table table,
                                     const sxml_function_query *query,
                                     sxml_function_entry *entry) {
    if (table.operations == NULL || table.operations->find_function == NULL) {
        *entry = (sxml_function_entry){
            .entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
        return SXML_STATUS_OK;
    }
    return table.operations->find_function(table.state, query, entry);
}

/* ------------------------------------------------------------------ Types */

/* Everything resolving one type needs. */
typedef struct type_resolver {
    const sxml_ast *ast;
    const sxml_xpath_static_context *static_context;
    sxml_error_record *error_record;
    sxml_type_registry *type_registry;
    sxml_name_pool *name_pool;
} type_resolver;

static static_error_site site_of(const type_resolver *resolver,
                                 sxml_ast_node_index node) {
    return (static_error_site){.ast = resolver->ast,
                               .node = node,
                               .error_record = resolver->error_record};
}

static sxml_ast_node_index find_field(const type_resolver *resolver,
                                      sxml_ast_node_index parent,
                                      sxml_ast_field field) {
    return sxml_ast_find_child(
        resolver->ast, (sxml_ast_child_query){.parent = parent, .field = field});
}

static sxml_ast_kind kind_of(const type_resolver *resolver,
                             sxml_ast_node_index node) {
    return sxml_ast_get_node(resolver->ast, node)->kind;
}

static sxml_status register_type(type_resolver *resolver,
                                 const sxml_type_description *description,
                                 sxml_type_identifier *type) {
    return sxml_type_registry_register_item_type(resolver->type_registry,
                                                 description, type);
}

/* Scratch memory for an array of a type description, released after
   registration (the registry copies views). */
typedef struct scratch_request {
    size_t element_count;
    size_t element_size;
    size_t element_alignment;
} scratch_request;

static sxml_status allocate_scratch(const type_resolver *resolver,
                                    scratch_request request, void **memory) {
    /* At least one element, so that a successful call never gives NULL
       and code indexing the array needs no special case for none. */
    const size_t element_count =
        request.element_count == 0 ? 1 : request.element_count;
    size_t byte_count = 0;
    sxml_status status =
        sxml_size_multiply(element_count, request.element_size, &byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_allocator_allocate(
        resolver->static_context->allocator,
        (sxml_allocation_request){.byte_count = byte_count,
                                  .alignment = request.element_alignment},
        memory);
}

static void release_scratch(const type_resolver *resolver, void *memory) {
    sxml_allocator_release(resolver->static_context->allocator, memory);
}

static size_t count_field(const type_resolver *resolver,
                          sxml_ast_node_index parent, sxml_ast_field field) {
    size_t count = 0;
    for (sxml_ast_node_index child = find_field(resolver, parent, field);
         child.value != 0;
         child = sxml_ast_find_next_with_same_field(resolver->ast, child)) {
        count += 1;
    }
    return count;
}

static bool is_name_kind(sxml_ast_kind kind) {
    return kind == SXML_AST_KIND_NCNAME ||
           kind == SXML_AST_KIND_PREFIXED_NAME ||
           kind == SXML_AST_KIND_URI_QUALIFIED_NAME;
}

/* Resolves and interns a name of a type or node test. */
typedef struct name_to_intern {
    sxml_ast_node_index node;
    sxml_xpath_name_role name_role;
} name_to_intern;

static sxml_status intern_resolved_name(const type_resolver *resolver,
                                        name_to_intern name,
                                        sxml_name_identifier *identifier) {
    sxml_xpath_resolved_name resolved = {0};
    sxml_status status = sxml_xpath_resolve_name(
        &(sxml_xpath_name_request){.ast = resolver->ast,
                                   .name_node = name.node,
                                   .name_role = name.name_role,
                                   .static_context = resolver->static_context,
                                   .error_record = resolver->error_record},
        &resolved);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_name_pool_intern_name(
        resolver->name_pool,
        &(sxml_name_request){.namespace_uri = resolved.namespace_uri,
                             .local_name = resolved.local_name},
        identifier);
}

uint32_t sxml_xpath_global_number(const sxml_xpath_static_context *static_context,
                                  size_t declaration_index) {
    if (static_context->global_numbers != NULL) {
        return static_context->global_numbers[declaration_index];
    }
    return (uint32_t)declaration_index + 1U;
}

bool sxml_xpath_find_named_type(const sxml_xpath_static_context *static_context,
                                sxml_name_identifier type_name, sxml_type_identifier *type) {
    /* Named item types of the static context first (D135). */
    const sxml_xpath_named_item_type_view aliases = static_context->named_item_types;
    for (size_t alias_index = 0; alias_index < aliases.named_item_type_count; alias_index += 1) {
        if (aliases.named_item_types[alias_index].type_name.index == type_name.index) {
            *type = aliases.named_item_types[alias_index].item_type;
            return true;
        }
    }
    return sxml_type_registry_find_type(sxml_item_heap_get_type_registry(static_context->item_heap),
                                        type_name, type);
}

/* What a type name names: found without interning, so an unknown name
   from untrusted text does not grow the pool. */
static sxml_status find_named_type(const type_resolver *resolver,
                                   sxml_ast_node_index name_node,
                                   sxml_type_identifier *type) {
    sxml_xpath_resolved_name resolved = {0};
    sxml_status status = sxml_xpath_resolve_name(
        &(sxml_xpath_name_request){
            .ast = resolver->ast,
            .name_node = name_node,
            .name_role = SXML_XPATH_NAME_ROLE_ELEMENT_OR_TYPE,
            .static_context = resolver->static_context,
            .error_record = resolver->error_record},
        &resolved);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (resolved.is_any_namespace) {
        /* "##any": the default type namespace rule gives xs (XPath 4.0,
           2.2.1). */
        resolved.namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_XML_SCHEMA);
    }
    sxml_name_identifier name = {0};
    sxml_type_identifier found = {0};
    const bool is_interned = sxml_name_pool_find_name(
        resolver->name_pool,
        &(sxml_name_request){.namespace_uri = resolved.namespace_uri,
                             .local_name = resolved.local_name},
        &name);
    if (!is_interned ||
        !sxml_xpath_find_named_type(resolver->static_context, name, &found)) {
        *type = (sxml_type_identifier){.index = SXML_TYPE_NONE};
        return SXML_STATUS_OK;
    }
    *type = found;
    return SXML_STATUS_OK;
}

/* A type name used as an item type (XPath 4.0, section 3.2.3 and 3.2.10):
   a generalized atomic type or a named item type; XPST0051 otherwise. */
static sxml_status resolve_type_name(const type_resolver *resolver,
                                     sxml_ast_node_index name_node,
                                     sxml_type_identifier *item_type) {
    sxml_type_identifier type = {0};
    sxml_status status = find_named_type(resolver, name_node, &type);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_type_description description = {0};
    if (type.index == SXML_TYPE_NONE ||
        sxml_type_registry_describe_type(resolver->type_registry, type,
                                         &description) != SXML_STATUS_OK) {
        return report_at(site_of(resolver, name_node), SXML_BYTE_VIEW_FROM_LITERAL("XPST0051"),
                         SXML_BYTE_VIEW_FROM_LITERAL("the type name is not defined"));
    }
    switch (description.variety) {
    case SXML_TYPE_VARIETY_ATOMIC:
    case SXML_TYPE_VARIETY_UNION:
    case SXML_TYPE_VARIETY_ANY_ITEM:
    case SXML_TYPE_VARIETY_NODE_TEST:
    case SXML_TYPE_VARIETY_JSON_NODE_TEST:
    case SXML_TYPE_VARIETY_FUNCTION:
    case SXML_TYPE_VARIETY_MAP:
    case SXML_TYPE_VARIETY_ARRAY:
    case SXML_TYPE_VARIETY_RECORD:
    case SXML_TYPE_VARIETY_ENUMERATION:
    case SXML_TYPE_VARIETY_CHOICE:
        *item_type = type;
        return SXML_STATUS_OK;
    case SXML_TYPE_VARIETY_COMPLEX:
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_LIST:
        break;
    }
    /* XPath 4.0, section 3.2.3: a name in an ItemType must be a
       generalized atomic type or a named item type. */
    return report_at(site_of(resolver, name_node), SXML_BYTE_VIEW_FROM_LITERAL("XPST0051"),
                     SXML_BYTE_VIEW_FROM_LITERAL("the type name is not an atomic or item type"));
}

/* The type annotation of element(N, T) or attribute(N, T): any schema
   type known to the registry; XPST0008 otherwise (XPath 4.0, 3.2.7.2). */
static sxml_status resolve_annotation_name(const type_resolver *resolver,
                                           sxml_ast_node_index name_node,
                                           sxml_type_identifier *annotation) {
    sxml_type_identifier type = {0};
    sxml_status status = find_named_type(resolver, name_node, &type);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (type.index == SXML_TYPE_NONE) {
        return report_at(site_of(resolver, name_node), SXML_BYTE_VIEW_FROM_LITERAL("XPST0008"),
                         SXML_BYTE_VIEW_FROM_LITERAL("the type name of a node test is not defined"));
    }
    *annotation = type;
    return SXML_STATUS_OK;
}

/* A name test of an element or attribute test (XPath 4.0, 3.2.7.2). */
typedef struct name_test_request {
    sxml_ast_node_index node;
    sxml_xpath_name_role name_role;
} name_test_request;

static sxml_status resolve_name_test(const type_resolver *resolver,
                                     name_test_request request,
                                     sxml_name_test *name_test) {
    const sxml_ast_node *node = sxml_ast_get_node(resolver->ast, request.node);
    if (is_name_kind(node->kind) && request.name_role == SXML_XPATH_NAME_ROLE_ELEMENT_OR_TYPE &&
        resolver->static_context->has_any_default_element_namespace &&
        node->kind == SXML_AST_KIND_NCNAME) {
        /* "##any": the element name matching rule reads local as *:local
           (XPath 4.0, 2.2.1). */
        *name_test = (sxml_name_test){
            .name_test_kind = SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD,
            .local_name = sxml_ast_get_text(resolver->ast, node->payload.name.local_name)};
        return SXML_STATUS_OK;
    }
    if (is_name_kind(node->kind)) {
        sxml_name_identifier name = {0};
        sxml_status status = intern_resolved_name(
            resolver,
            (name_to_intern){.node = request.node,
                             .name_role = request.name_role},
            &name);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        *name_test = (sxml_name_test){
            .name_test_kind = SXML_NAME_TEST_KIND_EXPANDED_NAME,
            .expanded_name = name};
        return SXML_STATUS_OK;
    }
    if (node->kind != SXML_AST_KIND_WILDCARD) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    switch (node->detail.name_test_kind) {
    case SXML_NAME_TEST_KIND_ANY_NAME:
        *name_test =
            (sxml_name_test){.name_test_kind = SXML_NAME_TEST_KIND_ANY_NAME};
        return SXML_STATUS_OK;
    case SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD:
        *name_test = (sxml_name_test){
            .name_test_kind = SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD,
            .local_name =
                sxml_ast_get_text(resolver->ast, node->payload.name.local_name)};
        return SXML_STATUS_OK;
    case SXML_NAME_TEST_KIND_NAMESPACE_WILDCARD: {
        sxml_xpath_resolved_name resolved = {0};
        sxml_status status = sxml_xpath_resolve_name(
            &(sxml_xpath_name_request){
                .ast = resolver->ast,
                .name_node = request.node,
                .name_role = request.name_role,
                .static_context = resolver->static_context,
                .error_record = resolver->error_record},
            &resolved);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        *name_test = (sxml_name_test){
            .name_test_kind = SXML_NAME_TEST_KIND_NAMESPACE_WILDCARD,
            .namespace_uri = resolved.namespace_uri};
        return SXML_STATUS_OK;
    }
    case SXML_NAME_TEST_KIND_EXPANDED_NAME:
        break;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* element(...) or attribute(...): one node test per name of the name test
   union, a choice of them for more than one. */
typedef struct element_test_request {
    sxml_ast_node_index node;
    sxml_node_kind node_kind;
} element_test_request;

static sxml_status register_choice(type_resolver *resolver,
                                   sxml_type_identifier_view members,
                                   sxml_type_identifier *type) {
    if (members.type_identifier_count == 1) {
        *type = members.type_identifiers[0];
        return SXML_STATUS_OK;
    }
    return register_type(
        resolver,
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_CHOICE,
            .variety_details = {.member_types = members}},
        type);
}

static sxml_status resolve_element_or_attribute_test(
    type_resolver *resolver, element_test_request request,
    sxml_type_identifier *item_type) {
    const sxml_xpath_name_role name_role =
        request.node_kind == SXML_NODE_KIND_ELEMENT
            ? SXML_XPATH_NAME_ROLE_ELEMENT_OR_TYPE
            : SXML_XPATH_NAME_ROLE_NO_NAMESPACE;
    sxml_node_test node_test = {
        .node_kind = request.node_kind,
        .name_test = {.name_test_kind = SXML_NAME_TEST_KIND_ANY_NAME},
        .allows_nilled = (sxml_ast_get_node(resolver->ast, request.node)->flags &
                          SXML_AST_FLAG_NILLABLE) != 0,
    };
    const sxml_ast_node_index type_name =
        find_field(resolver, request.node, SXML_AST_FIELD_TYPE);
    sxml_status status = SXML_STATUS_OK;
    if (type_name.value != 0) {
        status = resolve_annotation_name(resolver, type_name,
                                         &node_test.type_annotation);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    const sxml_ast_node_index union_node =
        find_field(resolver, request.node, SXML_AST_FIELD_NAME);
    const size_t test_count =
        union_node.value == 0
            ? 0
            : count_field(resolver, union_node, SXML_AST_FIELD_TEST);
    if (test_count == 0) {
        return register_type(
            resolver,
            &(sxml_type_description){
                .variety = SXML_TYPE_VARIETY_NODE_TEST,
                .variety_details = {.node_test = node_test}},
            item_type);
    }
    void *memory = NULL;
    status = allocate_scratch(
        resolver,
        (scratch_request){.element_count = test_count,
                          .element_size = sizeof(sxml_type_identifier),
                          .element_alignment = alignof(sxml_type_identifier)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_type_identifier *members = memory;
    size_t member_count = 0;
    for (sxml_ast_node_index union_member =
             find_field(resolver, union_node, SXML_AST_FIELD_TEST);
         status == SXML_STATUS_OK && union_member.value != 0;
         union_member = sxml_ast_find_next_with_same_field(resolver->ast, union_member)) {
        status = resolve_name_test(
            resolver,
            (name_test_request){.node = union_member, .name_role = name_role},
            &node_test.name_test);
        if (status == SXML_STATUS_OK) {
            status = register_type(
                resolver,
                &(sxml_type_description){
                    .variety = SXML_TYPE_VARIETY_NODE_TEST,
                    .variety_details = {.node_test = node_test}},
                &members[member_count]);
        }
        member_count += 1;
    }
    if (status == SXML_STATUS_OK) {
        status = register_choice(
            resolver,
            (sxml_type_identifier_view){.type_identifiers = members,
                                        .type_identifier_count = member_count},
            item_type);
    }
    release_scratch(resolver, memory);
    return status;
}

static sxml_status resolve_processing_instruction_test(
    type_resolver *resolver, sxml_ast_node_index node,
    sxml_type_identifier *item_type) {
    const sxml_ast_node_index name_node =
        find_field(resolver, node, SXML_AST_FIELD_NAME);
    if (name_node.value == 0) {
        *item_type = sxml_builtin_type_identifier(
            SXML_TYPE_PROCESSING_INSTRUCTION_NODE);
        return SXML_STATUS_OK;
    }
    const sxml_ast_node *name = sxml_ast_get_node(resolver->ast, name_node);
    sxml_byte_view target = sxml_ast_get_text(
        resolver->ast, name->kind == SXML_AST_KIND_STRING_LITERAL
                           ? name->payload.text
                           : name->payload.name.local_name);
    /* A string literal target is whitespace-normalized (XPath 4.0,
       3.2.7.4); only surrounding whitespace can remain in an NCName. */
    while (target.byte_count > 0 && is_xml_whitespace(target.bytes[0])) {
        target.bytes += 1;
        target.byte_count -= 1;
    }
    while (target.byte_count > 0 &&
           is_xml_whitespace(target.bytes[target.byte_count - 1])) {
        target.byte_count -= 1;
    }
    if (target.byte_count == 0) {
        return report_at(site_of(resolver, name_node), SXML_BYTE_VIEW_FROM_LITERAL("XPTY0004"),
                         SXML_BYTE_VIEW_FROM_LITERAL("a processing-instruction target is not an NCName"));
    }
    sxml_name_identifier target_name = {0};
    sxml_status status = sxml_name_pool_intern_name(
        resolver->name_pool,
        &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                             .local_name = target},
        &target_name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return register_type(
        resolver,
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_NODE_TEST,
            .variety_details = {
                .node_test = {.node_kind = SXML_NODE_KIND_PROCESSING_INSTRUCTION,
                              .name_test = {.name_test_kind =
                                                SXML_NAME_TEST_KIND_EXPANDED_NAME,
                                            .expanded_name = target_name}}}},
        item_type);
}

static sxml_status resolve_enumeration_type(type_resolver *resolver,
                                            sxml_ast_node_index node,
                                            sxml_type_identifier *item_type) {
    const size_t value_count = count_field(resolver, node, SXML_AST_FIELD_VALUE);
    void *memory = NULL;
    sxml_status status = allocate_scratch(
        resolver,
        (scratch_request){.element_count = value_count,
                          .element_size = sizeof(sxml_byte_view),
                          .element_alignment = alignof(sxml_byte_view)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_byte_view *values = memory;
    size_t distinct_count = 0;
    for (sxml_ast_node_index value =
             find_field(resolver, node, SXML_AST_FIELD_VALUE);
         value.value != 0;
         value = sxml_ast_find_next_with_same_field(resolver->ast, value)) {
        const sxml_byte_view text = sxml_ast_get_text(
            resolver->ast, sxml_ast_get_node(resolver->ast, value)->payload.text);
        /* Duplicates are allowed and ignored (XPath 4.0, 3.2.11); the
           registry takes distinct values (D58). */
        bool is_duplicate = false;
        for (size_t earlier_index = 0; earlier_index < distinct_count;
             earlier_index += 1) {
            if (sxml_byte_view_equals(values[earlier_index], text)) {
                is_duplicate = true;
                break;
            }
        }
        if (!is_duplicate) {
            values[distinct_count] = text;
            distinct_count += 1;
        }
    }
    status = register_type(
        resolver,
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_ENUMERATION,
            .variety_details = {.enumeration_values = {.byte_views = values,
                                                       .byte_view_count =
                                                           distinct_count}}},
        item_type);
    release_scratch(resolver, memory);
    return status;
}

/* The lexical form of a jnode() selector that is not text of the AST:
   pieces joined in scratch memory, which *text points into until the
   caller releases *request->memory. */
typedef struct joined_text {
    sxml_byte_view pieces[4];
    size_t piece_count;
    /* Where the scratch memory goes; the caller releases it. */
    void **memory;
} joined_text;

static sxml_status join_text(const type_resolver *resolver,
                             const joined_text *request, sxml_byte_view *text) {
    void **memory = request->memory;
    size_t byte_count = 0;
    for (size_t piece_index = 0; piece_index < request->piece_count;
         piece_index += 1) {
        sxml_status status = sxml_size_add(
            byte_count, request->pieces[piece_index].byte_count, &byte_count);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_status status = allocate_scratch(
        resolver,
        (scratch_request){.element_count = byte_count,
                          .element_size = 1,
                          .element_alignment = 1},
        memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *bytes = *memory;
    size_t offset = 0;
    for (size_t piece_index = 0; piece_index < request->piece_count;
         piece_index += 1) {
        const sxml_byte_view piece = request->pieces[piece_index];
        if (piece.byte_count > 0) {
            memcpy(bytes + offset, piece.bytes, piece.byte_count);
        }
        offset += piece.byte_count;
    }
    *text = (sxml_byte_view){.bytes = bytes, .byte_count = byte_count};
    return SXML_STATUS_OK;
}

/* The type of a numeric literal kind as a jnode() key; false for other
   kinds. */
static bool numeric_key_type(sxml_ast_kind kind, sxml_type_identifier *type) {
    switch ((uint32_t)kind) {
    case SXML_AST_KIND_INTEGER_LITERAL:
        *type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER);
        return true;
    case SXML_AST_KIND_DECIMAL_LITERAL:
        *type = sxml_builtin_type_identifier(SXML_TYPE_XS_DECIMAL);
        return true;
    case SXML_AST_KIND_DOUBLE_LITERAL:
        *type = sxml_builtin_type_identifier(SXML_TYPE_XS_DOUBLE);
        return true;
    default:
        return false;
    }
}

/* A jnode() selector whose lexical form is made, not read: a negative
   number ("-" and the digits) or a QName literal (its EQName, as
   sxml_json_node_test wants). *memory holds the text until the type is
   registered. */
typedef struct made_selector {
    sxml_ast_node_index selector;
    void **memory;
} made_selector;

static sxml_status resolve_made_selector(const type_resolver *resolver,
                                         made_selector request,
                                         sxml_json_node_test *test) {
    const sxml_ast_node *selector_node =
        sxml_ast_get_node(resolver->ast, request.selector);
    if (selector_node->kind == SXML_AST_KIND_NEGATIVE_NUMERIC_LITERAL) {
        const sxml_ast_node_index value =
            find_field(resolver, request.selector, SXML_AST_FIELD_VALUE);
        const sxml_ast_node *value_node = sxml_ast_get_node(resolver->ast, value);
        if (!numeric_key_type(value_node->kind, &test->key_type)) {
            return report_unsupported(site_of(resolver, request.selector),
                                      SXML_BYTE_VIEW_FROM_LITERAL(
                                          "this jnode() selector is not supported"));
        }
        test->key_constraint = SXML_JSON_KEY_CONSTRAINT_CONSTANT;
        return join_text(
            resolver,
            &(joined_text){
                .pieces = {SXML_BYTE_VIEW_FROM_LITERAL("-"),
                           sxml_ast_get_text(resolver->ast, value_node->payload.text)},
                .piece_count = 2,
                .memory = request.memory},
            &test->key_lexical_form);
    }
    /* A QName literal (XPath 4.0, section 4.3.1.3): unprefixed, in no
       namespace. */
    sxml_xpath_resolved_name resolved = {0};
    sxml_status status = sxml_xpath_resolve_name(
        &(sxml_xpath_name_request){
            .ast = resolver->ast,
            .name_node = find_field(resolver, request.selector, SXML_AST_FIELD_NAME),
            .name_role = SXML_XPATH_NAME_ROLE_NO_NAMESPACE,
            .static_context = resolver->static_context,
            .error_record = resolver->error_record},
        &resolved);
    sxml_byte_view namespace_uri = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_namespace_uri(
            resolver->name_pool, resolved.namespace_uri, &namespace_uri);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    test->key_constraint = SXML_JSON_KEY_CONSTRAINT_CONSTANT;
    test->key_type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME);
    if (namespace_uri.byte_count == 0) {
        /* In no namespace the local name is the EQName's meaning and the
           form a cast to xs:QName without namespaces reads (D113). */
        return join_text(resolver,
                         &(joined_text){.pieces = {resolved.local_name},
                                        .piece_count = 1,
                                        .memory = request.memory},
                         &test->key_lexical_form);
    }
    return join_text(resolver,
                     &(joined_text){.pieces = {SXML_BYTE_VIEW_FROM_LITERAL("Q{"),
                                               namespace_uri,
                                               SXML_BYTE_VIEW_FROM_LITERAL("}"),
                                               resolved.local_name},
                                    .piece_count = 4,
                                    .memory = request.memory},
                     &test->key_lexical_form);
}

/* jnode(selector, type) (XPath 4.0, 3.2.12): the selector: *, (), an
   NCName, or a constant (a string, numeric, boolean or QName literal, or
   a negative number). A made lexical form is in *memory (released with
   the frame). */
typedef struct json_node_test_request {
    /* The JNODE_TYPE node. */
    sxml_ast_node_index node;
    void **memory;
} json_node_test_request;

static sxml_status resolve_json_node_selector(const type_resolver *resolver,
                                              json_node_test_request request,
                                              sxml_json_node_test *test) {
    *test = (sxml_json_node_test){.key_constraint = SXML_JSON_KEY_CONSTRAINT_ANY};
    const sxml_ast_node_index selector =
        find_field(resolver, request.node, SXML_AST_FIELD_SELECTOR);
    const sxml_ast_node *selector_node =
        sxml_ast_get_node(resolver->ast, selector);
    /* The AST kinds form an open set that grows with XQuery (D52), so
       switches over them name the kinds they handle and have a default. */
    switch ((uint32_t)selector_node->kind) {
    case SXML_AST_KIND_NONE:
    case SXML_AST_KIND_WILDCARD:
        break;
    case SXML_AST_KIND_JROOT_SELECTOR:
        test->key_constraint = SXML_JSON_KEY_CONSTRAINT_ROOT;
        break;
    case SXML_AST_KIND_NCNAME:
        test->key_constraint = SXML_JSON_KEY_CONSTRAINT_CONSTANT;
        test->key_type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING);
        test->key_lexical_form = sxml_ast_get_text(
            resolver->ast, selector_node->payload.name.local_name);
        break;
    case SXML_AST_KIND_STRING_LITERAL:
        test->key_constraint = SXML_JSON_KEY_CONSTRAINT_CONSTANT;
        test->key_type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING);
        test->key_lexical_form =
            sxml_ast_get_text(resolver->ast, selector_node->payload.text);
        break;
    case SXML_AST_KIND_BOOLEAN_CONSTANT:
        test->key_constraint = SXML_JSON_KEY_CONSTRAINT_CONSTANT;
        test->key_type = sxml_builtin_type_identifier(SXML_TYPE_XS_BOOLEAN);
        test->key_lexical_form =
            (selector_node->flags & SXML_AST_FLAG_TRUE) != 0
                ? SXML_BYTE_VIEW_FROM_LITERAL("true")
                : SXML_BYTE_VIEW_FROM_LITERAL("false");
        break;
    case SXML_AST_KIND_INTEGER_LITERAL: {
        /* The canonical form has no leading zeros (XSD 1.1 Part 2,
           3.4.13.2). */
        sxml_byte_view digits =
            sxml_ast_get_text(resolver->ast, selector_node->payload.text);
        while (digits.byte_count > 1 && digits.bytes[0] == '0') {
            digits.bytes += 1;
            digits.byte_count -= 1;
        }
        test->key_constraint = SXML_JSON_KEY_CONSTRAINT_CONSTANT;
        test->key_type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER);
        test->key_lexical_form = digits;
        break;
    }
    case SXML_AST_KIND_DECIMAL_LITERAL:
    case SXML_AST_KIND_DOUBLE_LITERAL:
        /* The key is compared by value (atomic-equal), so the literal's
           own lexical form serves. */
        test->key_constraint = SXML_JSON_KEY_CONSTRAINT_CONSTANT;
        (void)numeric_key_type(selector_node->kind, &test->key_type);
        test->key_lexical_form =
            sxml_ast_get_text(resolver->ast, selector_node->payload.text);
        break;
    case SXML_AST_KIND_NEGATIVE_NUMERIC_LITERAL:
    case SXML_AST_KIND_QNAME_LITERAL:
        return resolve_made_selector(
            resolver, (made_selector){.selector = selector, .memory = request.memory},
            test);
    default:
        return report_unsupported(site_of(resolver, selector),
                                  SXML_BYTE_VIEW_FROM_LITERAL("this jnode() selector is not supported"));
    }
    return SXML_STATUS_OK;
}

/*
 * Item types nest (map(K, array(map(...)))), and resolving one registers
 * its parts first. Without C recursion (D71) the nesting is a stack of
 * frames, one per item type being resolved whose parts are not all
 * resolved yet, at most SXML_XPATH_MAXIMUM_TYPE_DEPTH of them. A frame
 * continues in steps: each step resolves a part or asks for one (a new
 * frame on top), and the last registers the type and writes it to its
 * destination, which is in the frame below or the caller's.
 *
 * Item types without parts that are item types or sequence types
 * (generic types, names, element, attribute, processing-instruction and
 * enumeration types) are resolved where they are asked for, without a
 * frame.
 */
typedef struct type_frame {
    sxml_ast_node_index node;
    sxml_ast_kind kind;
    /* Which step comes next; 0 for the first. */
    uint32_t step;
    bool is_finished;
    sxml_type_identifier *destination;
    /* The next parameter, field or alternative; parts_resolved counts the
       ones before it. */
    sxml_ast_node_index next_part;
    size_t part_count;
    size_t parts_resolved;
    /* Parameter types, record fields or choice members, released when the
       frame is popped. */
    void *scratch;
    /* The key type of a map type, the element test of a document test. */
    sxml_type_identifier item_part;
    /* The value type of a map type, member type of an array type, result
       type of a function type. */
    sxml_sequence_type sequence_part;
    sxml_json_node_test json_node_test;
} type_frame;

typedef struct type_machine {
    type_resolver *resolver;
    /* SXML_XPATH_MAXIMUM_TYPE_DEPTH frames, allocated for the first
       frame; never moved, so destinations in frames stay valid. */
    type_frame *frames;
    uint32_t frame_count;
} type_machine;

static sxml_status report_type_depth(const type_resolver *resolver,
                                     sxml_ast_node_index node) {
    if (resolver->error_record != NULL) {
        resolver->error_record->diagnostic = (sxml_diagnostic){
            .description =
                SXML_BYTE_VIEW_FROM_LITERAL("item types are nested too deep"),
            .location = sxml_ast_get_source_location(resolver->ast, node)};
    }
    return SXML_STATUS_LIMIT_EXCEEDED;
}

/* Resolves an item type without parts; false when it has parts. */
typedef struct leaf_type_request {
    sxml_ast_node_index node;
    sxml_type_identifier *item_type;
} leaf_type_request;

static bool resolve_leaf_type(type_resolver *resolver,
                              leaf_type_request request, sxml_status *status) {
    static const sxml_builtin_type generic_types[] = {
        [SXML_AST_KIND_ANY_ITEM_TYPE] = SXML_TYPE_ITEM,
        [SXML_AST_KIND_GNODE_TYPE] = SXML_TYPE_GENERIC_NODE,
        [SXML_AST_KIND_ANY_XNODE_TYPE] = SXML_TYPE_NODE,
        [SXML_AST_KIND_COMMENT_NODE_TYPE] = SXML_TYPE_COMMENT_NODE,
        [SXML_AST_KIND_TEXT_NODE_TYPE] = SXML_TYPE_TEXT_NODE,
        [SXML_AST_KIND_NAMESPACE_NODE_TYPE] = SXML_TYPE_NAMESPACE_NODE,
        [SXML_AST_KIND_ANY_MAP_TYPE] = SXML_TYPE_MAP,
        [SXML_AST_KIND_ANY_ARRAY_TYPE] = SXML_TYPE_ARRAY,
        [SXML_AST_KIND_ANY_RECORD_TYPE] = SXML_TYPE_RECORD,
        [SXML_AST_KIND_ANY_FUNCTION_TYPE] = SXML_TYPE_FUNCTION,
    };
    const sxml_ast_node_index node = request.node;
    sxml_type_identifier *item_type = request.item_type;
    const uint32_t kind = (uint32_t)kind_of(resolver, node);
    switch (kind) {
    case SXML_AST_KIND_ANY_ITEM_TYPE:
    case SXML_AST_KIND_GNODE_TYPE:
    case SXML_AST_KIND_ANY_XNODE_TYPE:
    case SXML_AST_KIND_COMMENT_NODE_TYPE:
    case SXML_AST_KIND_TEXT_NODE_TYPE:
    case SXML_AST_KIND_NAMESPACE_NODE_TYPE:
    case SXML_AST_KIND_ANY_MAP_TYPE:
    case SXML_AST_KIND_ANY_ARRAY_TYPE:
    case SXML_AST_KIND_ANY_RECORD_TYPE:
    case SXML_AST_KIND_ANY_FUNCTION_TYPE:
        *item_type = sxml_builtin_type_identifier(generic_types[kind]);
        *status = SXML_STATUS_OK;
        return true;
    case SXML_AST_KIND_ELEMENT_NODE_TYPE:
    case SXML_AST_KIND_ATTRIBUTE_NODE_TYPE:
        *status = resolve_element_or_attribute_test(
            resolver,
            (element_test_request){.node = node,
                                   .node_kind =
                                       kind == SXML_AST_KIND_ELEMENT_NODE_TYPE
                                           ? SXML_NODE_KIND_ELEMENT
                                           : SXML_NODE_KIND_ATTRIBUTE},
            item_type);
        return true;
    case SXML_AST_KIND_SCHEMA_ELEMENT_NODE_TYPE:
    case SXML_AST_KIND_SCHEMA_ATTRIBUTE_NODE_TYPE:
        /* No schema is imported in Phase 1, so no declaration is in scope
           (XPath 4.0, 3.2.7.3). */
        *status = report_at(site_of(resolver, node), SXML_BYTE_VIEW_FROM_LITERAL("XPST0008"),
                            SXML_BYTE_VIEW_FROM_LITERAL("no schema declaration of this name is in scope"));
        return true;
    case SXML_AST_KIND_PROCESSING_INSTRUCTION_NODE_TYPE:
        *status = resolve_processing_instruction_test(resolver, node, item_type);
        return true;
    case SXML_AST_KIND_ENUMERATION_TYPE:
        *status = resolve_enumeration_type(resolver, node, item_type);
        return true;
    case SXML_AST_KIND_NCNAME:
    case SXML_AST_KIND_PREFIXED_NAME:
    case SXML_AST_KIND_URI_QUALIFIED_NAME:
        *status = resolve_type_name(resolver, node, item_type);
        return true;
    case SXML_AST_KIND_DOCUMENT_NODE_TYPE:
    case SXML_AST_KIND_JNODE_TYPE:
    case SXML_AST_KIND_TYPED_MAP_TYPE:
    case SXML_AST_KIND_TYPED_ARRAY_TYPE:
    case SXML_AST_KIND_TYPED_RECORD_TYPE:
    case SXML_AST_KIND_TYPED_FUNCTION_TYPE:
    case SXML_AST_KIND_CHOICE_ITEM_TYPE:
        return false;
    default:
        *status = SXML_STATUS_INVALID_ARGUMENT;
        return true;
    }
}

/* An item type to resolve into destination. */
typedef struct type_part {
    sxml_ast_node_index node;
    sxml_type_identifier *destination;
} type_part;

/* Resolves part now when it has no parts, else pushes a frame for it.
   Item types nest at most SXML_XPATH_MAXIMUM_TYPE_DEPTH deep, counting
   the one asked for. */
static sxml_status begin_item_type(type_machine *machine, type_part part) {
    type_resolver *resolver = machine->resolver;
    if (machine->frame_count >= SXML_XPATH_MAXIMUM_TYPE_DEPTH) {
        return report_type_depth(resolver, part.node);
    }
    sxml_status status = SXML_STATUS_OK;
    if (resolve_leaf_type(
            resolver,
            (leaf_type_request){.node = part.node,
                                .item_type = part.destination},
            &status)) {
        return status;
    }
    if (machine->frames == NULL) {
        void *memory = NULL;
        status = allocate_scratch(
            resolver,
            (scratch_request){.element_count = SXML_XPATH_MAXIMUM_TYPE_DEPTH,
                              .element_size = sizeof(type_frame),
                              .element_alignment = alignof(type_frame)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        machine->frames = memory;
    }
    machine->frames[machine->frame_count] =
        (type_frame){.node = part.node,
                     .kind = kind_of(resolver, part.node),
                     .destination = part.destination};
    machine->frame_count += 1;
    return SXML_STATUS_OK;
}

/* A sequence type to resolve into destination. */
typedef struct sequence_type_part {
    sxml_ast_node_index node;
    sxml_sequence_type *destination;
} sequence_type_part;

/* A sequence type (XPath 4.0, section 3.1): the occurrence now, the item
   type through begin_item_type. */
static sxml_status begin_sequence_type(type_machine *machine,
                                       sequence_type_part part) {
    const type_resolver *resolver = machine->resolver;
    const sxml_ast_node *type_node = sxml_ast_get_node(resolver->ast, part.node);
    if (type_node->kind != SXML_AST_KIND_SEQUENCE_TYPE) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_occurrence occurrence = type_node->detail.occurrence;
    if (occurrence == SXML_OCCURRENCE_ZERO) {
        /* empty-sequence(): the item type is ignored (types.h). */
        *part.destination = (sxml_sequence_type){
            .item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
            .occurrence = SXML_OCCURRENCE_ZERO};
        return SXML_STATUS_OK;
    }
    part.destination->occurrence = occurrence;
    return begin_item_type(
        machine,
        (type_part){.node = find_field(resolver, part.node,
                                       SXML_AST_FIELD_ITEM_TYPE),
                    .destination = &part.destination->item_type});
}

/* Registers the frame's type into its destination and finishes it. */
static sxml_status finish_frame(type_machine *machine, type_frame *frame,
                                const sxml_type_description *description) {
    frame->is_finished = true;
    return register_type(machine->resolver, description, frame->destination);
}

/* Counts the parts of field and allocates their scratch array. */
typedef struct frame_parts {
    sxml_ast_field field;
    size_t element_size;
    size_t element_alignment;
} frame_parts;

static sxml_status begin_frame_parts(type_machine *machine, type_frame *frame,
                                     frame_parts parts) {
    const type_resolver *resolver = machine->resolver;
    frame->part_count = count_field(resolver, frame->node, parts.field);
    frame->next_part = find_field(resolver, frame->node, parts.field);
    return allocate_scratch(
        resolver,
        (scratch_request){.element_count = frame->part_count,
                          .element_size = parts.element_size,
                          .element_alignment = parts.element_alignment},
        &frame->scratch);
}

/* document-node(...) (XPath 4.0, 3.2.7.1). */
static sxml_status continue_document_test(type_machine *machine,
                                          type_frame *frame) {
    type_resolver *resolver = machine->resolver;
    if (frame->step == 0) {
        frame->step = 1;
        const sxml_ast_node_index content =
            find_field(resolver, frame->node, SXML_AST_FIELD_CONTENT);
        if (content.value == 0) {
            frame->is_finished = true;
            *frame->destination =
                sxml_builtin_type_identifier(SXML_TYPE_DOCUMENT_NODE);
            return SXML_STATUS_OK;
        }
        if (kind_of(resolver, content) == SXML_AST_KIND_NAME_TEST_UNION) {
            /* document-node(N) abbreviates document-node(element(N))
               (XPath 4.0, 3.2.7.1). Resolve the union as an element
               test's names. */
            return resolve_element_or_attribute_test(
                resolver,
                (element_test_request){.node = frame->node,
                                       .node_kind = SXML_NODE_KIND_ELEMENT},
                &frame->item_part);
        }
        return begin_item_type(machine,
                               (type_part){.node = content,
                                           .destination = &frame->item_part});
    }
    return finish_frame(
        machine, frame,
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_NODE_TEST,
            .variety_details = {.node_test = {
                                    .node_kind = SXML_NODE_KIND_DOCUMENT,
                                    .name_test = {.name_test_kind =
                                                      SXML_NAME_TEST_KIND_ANY_NAME},
                                    .document_element_test = frame->item_part}}});
}

/* function(T1, T2) as R (XPath 4.0, 3.2.8.1). */
static sxml_status continue_function_type(type_machine *machine,
                                          type_frame *frame) {
    const type_resolver *resolver = machine->resolver;
    if (frame->step == 0) {
        frame->step = 1;
        return begin_frame_parts(
            machine, frame,
            (frame_parts){.field = SXML_AST_FIELD_PARAMETER,
                          .element_size = sizeof(sxml_sequence_type),
                          .element_alignment = alignof(sxml_sequence_type)});
    }
    sxml_sequence_type *parameter_types = frame->scratch;
    if (frame->step == 1) {
        if (frame->next_part.value != 0) {
            const sxml_ast_node_index parameter = frame->next_part;
            frame->next_part =
                sxml_ast_find_next_with_same_field(resolver->ast, parameter);
            frame->parts_resolved += 1;
            return begin_sequence_type(
                machine,
                (sequence_type_part){
                    .node = find_field(resolver, parameter, SXML_AST_FIELD_TYPE),
                    .destination = &parameter_types[frame->parts_resolved - 1]});
        }
        frame->step = 2;
        return begin_sequence_type(
            machine,
            (sequence_type_part){
                .node = find_field(resolver, frame->node,
                                   SXML_AST_FIELD_RETURN_TYPE),
                .destination = &frame->sequence_part});
    }
    return finish_frame(
        machine, frame,
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_FUNCTION,
            .variety_details = {
                .function_type = {
                    .is_any_function = false,
                    .parameter_types = {.sequence_types = parameter_types,
                                        .sequence_type_count = frame->part_count},
                    .result_type = frame->sequence_part}}});
}

/* map(K, V) (XPath 4.0, 3.2.9.2). */
static sxml_status continue_map_type(type_machine *machine, type_frame *frame) {
    const type_resolver *resolver = machine->resolver;
    const sxml_ast_node_index key_node =
        find_field(resolver, frame->node, SXML_AST_FIELD_KEY);
    switch (frame->step) {
    case 0:
        frame->step = 1;
        return begin_item_type(
            machine, (type_part){.node = key_node, .destination = &frame->item_part});
    case 1:
        frame->step = 2;
        /* The key type must be a generalized atomic type (XPath 4.0,
           3.2.9.2): an atomic, union or enumeration type, or a choice of
           them. */
        if (!sxml_type_registry_is_subtype(
                resolver->type_registry,
                (sxml_type_pair){
                    .derived_type = frame->item_part,
                    .base_type = sxml_builtin_type_identifier(
                        SXML_TYPE_XS_ANY_ATOMIC_TYPE)})) {
            return report_at(site_of(resolver, key_node), SXML_BYTE_VIEW_FROM_LITERAL("XPST0152"),
                             SXML_BYTE_VIEW_FROM_LITERAL("the key type of a map type is not atomic"));
        }
        return begin_sequence_type(
            machine,
            (sequence_type_part){
                .node = find_field(resolver, frame->node, SXML_AST_FIELD_VALUE),
                .destination = &frame->sequence_part});
    default:
        return finish_frame(
            machine, frame,
            &(sxml_type_description){
                .variety = SXML_TYPE_VARIETY_MAP,
                .variety_details = {.map_type = {.is_any_map = false,
                                                 .key_type = frame->item_part,
                                                 .value_type =
                                                     frame->sequence_part}}});
    }
}

/* array(T) (XPath 4.0, 3.2.9.3). */
static sxml_status continue_array_type(type_machine *machine,
                                       type_frame *frame) {
    if (frame->step == 0) {
        frame->step = 1;
        return begin_sequence_type(
            machine,
            (sequence_type_part){
                .node = find_field(machine->resolver, frame->node,
                                   SXML_AST_FIELD_MEMBER),
                .destination = &frame->sequence_part});
    }
    return finish_frame(
        machine, frame,
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_ARRAY,
            .variety_details = {.array_type = {.is_any_array = false,
                                               .member_type =
                                                   frame->sequence_part}}});
}

/* record(N as T, ...) (XPath 4.0, 3.2.10.2). */
static sxml_status continue_record_type(type_machine *machine,
                                        type_frame *frame) {
    const type_resolver *resolver = machine->resolver;
    if (frame->step == 0) {
        frame->step = 1;
        return begin_frame_parts(
            machine, frame,
            (frame_parts){.field = SXML_AST_FIELD_FIELD,
                          .element_size = sizeof(sxml_record_field),
                          .element_alignment = alignof(sxml_record_field)});
    }
    sxml_record_field *fields = frame->scratch;
    if (frame->next_part.value != 0) {
        const sxml_ast_node_index field = frame->next_part;
        frame->next_part =
            sxml_ast_find_next_with_same_field(resolver->ast, field);
        const size_t field_index = frame->parts_resolved;
        frame->parts_resolved += 1;
        const sxml_ast_node *name = sxml_ast_get_node(
            resolver->ast, find_field(resolver, field, SXML_AST_FIELD_NAME));
        const sxml_byte_view field_name = sxml_ast_get_text(
            resolver->ast, name->kind == SXML_AST_KIND_STRING_LITERAL
                               ? name->payload.text
                               : name->payload.name.local_name);
        for (size_t earlier_index = 0; earlier_index < field_index;
             earlier_index += 1) {
            if (sxml_byte_view_equals(fields[earlier_index].field_name,
                                      field_name)) {
                /* XPath 4.0, 3.2.10.2: field names must be distinct. */
                return report_at(site_of(resolver, field), SXML_BYTE_VIEW_FROM_LITERAL("XPST0021"),
                                 SXML_BYTE_VIEW_FROM_LITERAL("two fields of a record type have the "
                                 "same name"));
            }
        }
        fields[field_index] = (sxml_record_field){
            .field_name = field_name,
            .field_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                           .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}};
        const sxml_ast_node_index type_node =
            find_field(resolver, field, SXML_AST_FIELD_TYPE);
        if (type_node.value == 0) {
            return SXML_STATUS_OK;
        }
        return begin_sequence_type(
            machine,
            (sequence_type_part){.node = type_node,
                                 .destination = &fields[field_index].field_type});
    }
    return finish_frame(
        machine, frame,
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_RECORD,
            .variety_details = {
                .record_type = {.is_any_record = false,
                                .is_nominative = false,
                                .record_fields = {.record_fields = fields,
                                                  .record_field_count =
                                                      frame->part_count}}}});
}

/* (A | B | ...) (XPath 4.0, 3.2.4). */
static sxml_status continue_choice_type(type_machine *machine,
                                        type_frame *frame) {
    type_resolver *resolver = machine->resolver;
    if (frame->step == 0) {
        frame->step = 1;
        return begin_frame_parts(
            machine, frame,
            (frame_parts){.field = SXML_AST_FIELD_ALTERNATIVE,
                          .element_size = sizeof(sxml_type_identifier),
                          .element_alignment = alignof(sxml_type_identifier)});
    }
    sxml_type_identifier *members = frame->scratch;
    if (frame->next_part.value != 0) {
        const sxml_ast_node_index alternative = frame->next_part;
        frame->next_part =
            sxml_ast_find_next_with_same_field(resolver->ast, alternative);
        frame->parts_resolved += 1;
        return begin_item_type(
            machine,
            (type_part){.node = alternative,
                        .destination = &members[frame->parts_resolved - 1]});
    }
    frame->is_finished = true;
    return register_choice(
        resolver,
        (sxml_type_identifier_view){.type_identifiers = members,
                                    .type_identifier_count = frame->part_count},
        frame->destination);
}

/* jnode(selector, type) (XPath 4.0, 3.2.12). */
static sxml_status continue_json_node_test(type_machine *machine,
                                           type_frame *frame) {
    const type_resolver *resolver = machine->resolver;
    if (frame->step == 0) {
        frame->step = 1;
        sxml_status status = resolve_json_node_selector(
            resolver,
            (json_node_test_request){.node = frame->node, .memory = &frame->scratch},
            &frame->json_node_test);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const sxml_ast_node_index type_node =
            find_field(resolver, frame->node, SXML_AST_FIELD_TYPE);
        if (type_node.value == 0) {
            return SXML_STATUS_OK;
        }
        frame->json_node_test.has_value_type = true;
        return begin_sequence_type(
            machine,
            (sequence_type_part){.node = type_node,
                                 .destination =
                                     &frame->json_node_test.value_type});
    }
    return finish_frame(
        machine, frame,
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_JSON_NODE_TEST,
            .variety_details = {.json_node_test = frame->json_node_test}});
}

static sxml_status continue_frame(type_machine *machine, type_frame *frame) {
    switch ((uint32_t)frame->kind) {
    case SXML_AST_KIND_DOCUMENT_NODE_TYPE:
        return continue_document_test(machine, frame);
    case SXML_AST_KIND_JNODE_TYPE:
        return continue_json_node_test(machine, frame);
    case SXML_AST_KIND_TYPED_MAP_TYPE:
        return continue_map_type(machine, frame);
    case SXML_AST_KIND_TYPED_ARRAY_TYPE:
        return continue_array_type(machine, frame);
    case SXML_AST_KIND_TYPED_RECORD_TYPE:
        return continue_record_type(machine, frame);
    case SXML_AST_KIND_TYPED_FUNCTION_TYPE:
        return continue_function_type(machine, frame);
    case SXML_AST_KIND_CHOICE_ITEM_TYPE:
        return continue_choice_type(machine, frame);
    default:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
}

/* Runs frames until none is left or one fails, then releases them all. */
static sxml_status run_type_machine(type_machine *machine, sxml_status status) {
    while (status == SXML_STATUS_OK && machine->frame_count > 0) {
        type_frame *frame = &machine->frames[machine->frame_count - 1];
        if (frame->is_finished) {
            release_scratch(machine->resolver, frame->scratch);
            machine->frame_count -= 1;
            continue;
        }
        status = continue_frame(machine, frame);
    }
    while (machine->frame_count > 0) {
        machine->frame_count -= 1;
        release_scratch(machine->resolver,
                        machine->frames[machine->frame_count].scratch);
    }
    if (machine->frames != NULL) {
        release_scratch(machine->resolver, machine->frames);
    }
    return status;
}

static type_resolver make_type_resolver(const sxml_xpath_type_request *request) {
    return (type_resolver){
        .ast = request->ast,
        .static_context = request->static_context,
        .error_record = request->error_record,
        .type_registry =
            sxml_item_heap_get_type_registry(request->static_context->item_heap),
        .name_pool =
            sxml_item_heap_get_name_pool(request->static_context->item_heap),
    };
}

sxml_status sxml_xpath_resolve_sequence_type(
    const sxml_xpath_type_request *request, sxml_sequence_type *sequence_type) {
    type_resolver resolver = make_type_resolver(request);
    type_machine machine = {.resolver = &resolver};
    sxml_sequence_type resolved = {0};
    sxml_status status = run_type_machine(
        &machine, begin_sequence_type(
                      &machine, (sequence_type_part){.node = request->type_node,
                                                     .destination = &resolved}));
    if (status == SXML_STATUS_OK) {
        *sequence_type = resolved;
    }
    return status;
}

sxml_status sxml_xpath_resolve_item_type(const sxml_xpath_type_request *request,
                                         sxml_type_identifier *item_type) {
    type_resolver resolver = make_type_resolver(request);
    type_machine machine = {.resolver = &resolver};
    sxml_type_identifier resolved = {0};
    sxml_status status = run_type_machine(
        &machine,
        begin_item_type(&machine, (type_part){.node = request->type_node,
                                               .destination = &resolved}));
    if (status == SXML_STATUS_OK) {
        *item_type = resolved;
    }
    return status;
}
