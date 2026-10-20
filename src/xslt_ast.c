/*
 * The stylesheet AST of XSLT 4.0: lifecycle, builder and text form
 * (src/xslt_ast_internal.h; docs/DECISIONS.md D24, D52, D102).
 *
 * Golden file: src/ir.c (the fixed part placed in the caller's memory, the
 * creating allocator after it; arrays that grow geometrically through the
 * configuration's allocator with entry 0 reserved; sealing; the text
 * writer that counts the bytes it needs).
 */
#include "xslt_ast_internal.h"

#include "text_form_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------ Lifecycle */

/* The default bound on modules (sxml_xslt_ast_configuration). */
#define XSLT_AST_DEFAULT_MAXIMUM_MODULE_COUNT 1000U

/* Where the parts of a created AST's block go: the AST, then the
   allocator that made the block. */
typedef struct ast_layout {
    sxml_memory_requirements requirements;
    size_t creating_allocator_offset;
} ast_layout;

typedef struct ast_layout_request {
    bool is_created;
} ast_layout_request;

static sxml_status compute_ast_layout(ast_layout_request request, ast_layout *layout) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    size_t ast_offset = 0;
    sxml_status status = sxml_memory_requirements_append(
        &requirements,
        (sxml_memory_requirements){.byte_count = sizeof(sxml_xslt_ast),
                                   .alignment = alignof(sxml_xslt_ast)},
        &ast_offset);
    size_t creating_allocator_offset = 0;
    if (status == SXML_STATUS_OK && request.is_created) {
        status = sxml_memory_requirements_append(
            &requirements,
            (sxml_memory_requirements){.byte_count = sizeof(sxml_allocator),
                                       .alignment = alignof(sxml_allocator)},
            &creating_allocator_offset);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *layout = (ast_layout){.requirements = requirements,
                           .creating_allocator_offset = creating_allocator_offset};
    return SXML_STATUS_OK;
}

static sxml_status check_ast_configuration(const sxml_xslt_ast_configuration *configuration) {
    if (configuration == NULL || sxml_allocator_is_null(configuration->allocator) ||
        configuration->item_heap == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_xslt_ast_memory_requirements(const sxml_xslt_ast_configuration *configuration,
                                              sxml_memory_requirements *requirements) {
    sxml_status status = check_ast_configuration(configuration);
    ast_layout layout = {.requirements = {.byte_count = 0, .alignment = 1},
                         .creating_allocator_offset = 0};
    if (status == SXML_STATUS_OK) {
        status = compute_ast_layout((ast_layout_request){.is_created = false}, &layout);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *requirements = layout.requirements;
    return SXML_STATUS_OK;
}

sxml_status sxml_xslt_ast_initialize(sxml_memory_view memory,
                                     const sxml_xslt_ast_configuration *configuration,
                                     sxml_xslt_ast **ast) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_xslt_ast_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_view_check_requirements(memory, requirements);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* The memory was checked for the AST's size and alignment. */
    void *ast_memory = memory.bytes;
    sxml_xslt_ast *new_ast = ast_memory;
    memset(new_ast, 0, sizeof(*new_ast));
    new_ast->allocator = configuration->allocator;
    new_ast->item_heap = configuration->item_heap;
    new_ast->maximum_module_count = configuration->maximum_module_count == 0
                                        ? XSLT_AST_DEFAULT_MAXIMUM_MODULE_COUNT
                                        : configuration->maximum_module_count;
    new_ast->maximum_node_count = configuration->maximum_node_count;
    *ast = new_ast;
    return SXML_STATUS_OK;
}

void sxml_xslt_ast_finalize(sxml_xslt_ast *ast) {
    if (ast == NULL) {
        return;
    }
    const sxml_allocator allocator = ast->allocator;
    for (uint32_t expression_index = 1; expression_index < ast->expression_count;
         expression_index += 1) {
        sxml_ast_destroy(ast->expressions[expression_index].ast);
    }
    for (uint32_t module_index = 1; module_index < ast->module_count; module_index += 1) {
        sxml_item_release(ast->modules[module_index].document);
        /* module_uri.bytes is the module's own allocation (add_module). */
        void *uri_memory = (void *)(uintptr_t)ast->modules[module_index].module_uri.bytes;
        sxml_allocator_release(allocator, uri_memory);
    }
    sxml_allocator_release(allocator, ast->modules);
    sxml_allocator_release(allocator, ast->nodes);
    sxml_allocator_release(allocator, ast->attributes);
    sxml_allocator_release(allocator, ast->expressions);
    sxml_allocator_release(allocator, ast->static_contexts);
    sxml_allocator_release(allocator, ast->declarations);
    sxml_allocator_release(allocator, ast->name_tokens);
    sxml_allocator_release(allocator, ast->namespace_bindings);
    sxml_allocator_release(allocator, ast->namespace_uris);
    sxml_allocator_release(allocator, ast->text_bytes);
    sxml_allocator_release(allocator, (void *)ast->in_scope_schemas);
    const bool is_created = ast->is_created;
    memset(ast, 0, sizeof(*ast));
    ast->is_created = is_created;
}

sxml_status sxml_xslt_ast_create(sxml_allocator allocator,
                                 const sxml_xslt_ast_configuration *configuration,
                                 sxml_xslt_ast **ast) {
    sxml_status status = check_ast_configuration(configuration);
    ast_layout layout = {.requirements = {.byte_count = 0, .alignment = 1},
                         .creating_allocator_offset = 0};
    if (status == SXML_STATUS_OK) {
        status = compute_ast_layout((ast_layout_request){.is_created = true}, &layout);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *block = NULL;
    status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = layout.requirements.byte_count,
                                  .alignment = layout.requirements.alignment},
        &block);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xslt_ast *new_ast = NULL;
    status = sxml_xslt_ast_initialize(
        (sxml_memory_view){.bytes = block, .byte_count = layout.requirements.byte_count},
        configuration, &new_ast);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, block);
        return status;
    }
    memcpy((uint8_t *)block + layout.creating_allocator_offset, &allocator, sizeof(allocator));
    new_ast->is_created = true;
    *ast = new_ast;
    return SXML_STATUS_OK;
}

void sxml_xslt_ast_destroy(sxml_xslt_ast *ast) {
    if (ast == NULL) {
        return;
    }
    ast_layout layout = {.requirements = {.byte_count = 0, .alignment = 1},
                         .creating_allocator_offset = 0};
    /* create computed the same layout, so this cannot fail. */
    if (compute_ast_layout((ast_layout_request){.is_created = true}, &layout) != SXML_STATUS_OK) {
        return;
    }
    const void *ast_memory = ast;
    const uint8_t *block_bytes = ast_memory;
    sxml_allocator creating_allocator = {.state = NULL, .operations = NULL};
    memcpy(&creating_allocator, block_bytes + layout.creating_allocator_offset,
           sizeof(creating_allocator));
    sxml_xslt_ast_finalize(ast);
    sxml_allocator_release(creating_allocator, ast);
}

/* ------------------------------------------------------ In-scope schemas */

sxml_status sxml_xslt_ast_set_in_scope_schemas(sxml_xslt_ast *ast, sxml_schema_view schemas) {
    void *memory = NULL;
    if (schemas.schema_count > 0) {
        size_t byte_count = 0;
        sxml_status status =
            sxml_size_multiply(schemas.schema_count, sizeof(const sxml_schema *), &byte_count);
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                ast->allocator,
                (sxml_allocation_request){.byte_count = byte_count,
                                          .alignment = alignof(const sxml_schema *)},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        memcpy(memory, (const void *)schemas.schemas, byte_count);
    }
    sxml_allocator_release(ast->allocator, (void *)ast->in_scope_schemas);
    ast->in_scope_schemas = (const sxml_schema **)memory;
    ast->in_scope_schema_count = schemas.schema_count;
    return SXML_STATUS_OK;
}

sxml_schema_view sxml_xslt_ast_get_in_scope_schemas(const sxml_xslt_ast *ast) {
    return (sxml_schema_view){.schemas = ast->in_scope_schemas,
                              .schema_count = ast->in_scope_schema_count};
}

/* ---------------------------------------------------------------- Arrays */

/* One array of the AST to make room in (as src/ir.c). */
typedef struct array_room_request {
    void *elements;
    uint32_t element_count;
    uint32_t element_capacity;
    size_t element_size;
    size_t element_alignment;
    /* Entries to add after the count; the reserved entry 0 is added too
       when the array is still empty. */
    uint32_t added_count;
} array_room_request;

/* The array after making room: elements is unchanged when it had room. */
typedef struct array_room {
    void *elements;
    uint32_t element_count;
    uint32_t element_capacity;
} array_room;

/* UINT32_MAX - 1 entries plus the reserved entry 0. */
#define XSLT_AST_ARRAY_MAXIMUM_COUNT ((uint32_t)UINT32_MAX)

static sxml_status make_array_room(sxml_xslt_ast *ast, const array_room_request *request,
                                   array_room *room) {
    if (ast->is_sealed) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const uint32_t current_count = request->element_count == 0 ? 1U : request->element_count;
    if (request->added_count > XSLT_AST_ARRAY_MAXIMUM_COUNT - current_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    const uint32_t needed_count = current_count + request->added_count;
    if (needed_count <= request->element_capacity) {
        *room = (array_room){.elements = request->elements,
                             .element_count = current_count,
                             .element_capacity = request->element_capacity};
        return SXML_STATUS_OK;
    }
    /* Grow geometrically so that adding n entries costs O(n) copies. */
    uint32_t new_capacity = request->element_capacity < 8U ? 8U : request->element_capacity;
    while (new_capacity < needed_count) {
        new_capacity = new_capacity > XSLT_AST_ARRAY_MAXIMUM_COUNT / 2U
                           ? XSLT_AST_ARRAY_MAXIMUM_COUNT
                           : new_capacity * 2U;
    }
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(new_capacity, request->element_size, &byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *new_elements = NULL;
    status = sxml_allocator_allocate(
        ast->allocator,
        (sxml_allocation_request){.byte_count = byte_count,
                                  .alignment = request->element_alignment},
        &new_elements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (request->element_count > 0) {
        memcpy(new_elements, request->elements,
               (size_t)request->element_count * request->element_size);
    } else {
        memset(new_elements, 0, request->element_size);
    }
    sxml_allocator_release(ast->allocator, request->elements);
    *room = (array_room){.elements = new_elements,
                         .element_count = current_count,
                         .element_capacity = new_capacity};
    return SXML_STATUS_OK;
}

/* Entries in use of an array, without the reserved entry 0. */
static uint32_t used_entry_count(uint32_t element_count) {
    return element_count == 0 ? 0U : element_count - 1U;
}

/* True for 0 ("none") and for an index in use. */
static bool is_optional_index_valid(uint32_t index, uint32_t count) {
    return index == 0 || index < count;
}

/* True when the nodes and attributes may grow by one more entry. */
static bool has_room_for_node_or_attribute(const sxml_xslt_ast *ast) {
    if (ast->maximum_node_count == 0) {
        return true;
    }
    const uint64_t used_count = (uint64_t)used_entry_count(ast->node_count) +
                                (uint64_t)used_entry_count(ast->attribute_count);
    return used_count < ast->maximum_node_count;
}

/* ---------------------------------------------------------------- Modules */

/* True when module is the module last added or one of its ancestors. */
static bool is_on_last_module_path(const sxml_xslt_ast *ast, sxml_xslt_ast_module_index module) {
    uint32_t candidate = ast->module_count == 0 ? 0U : ast->module_count - 1U;
    while (candidate != 0) {
        if (candidate == module.value) {
            return true;
        }
        candidate = ast->modules[candidate].parent_module.value;
    }
    return false;
}

static bool is_module_request_valid(const sxml_xslt_ast *ast,
                                    const sxml_xslt_ast_module_request *request) {
    const bool is_first = used_entry_count(ast->module_count) == 0;
    if (request->module_uri.byte_count > 0 && request->module_uri.bytes == NULL) {
        return false;
    }
    if (is_first) {
        return request->relation == SXML_XSLT_AST_MODULE_RELATION_PRINCIPAL &&
               request->parent_module.value == 0 && request->referencing_node.value == 0;
    }
    if (request->relation != SXML_XSLT_AST_MODULE_RELATION_INCLUDED &&
        request->relation != SXML_XSLT_AST_MODULE_RELATION_IMPORTED &&
        request->relation != SXML_XSLT_AST_MODULE_RELATION_USED_PACKAGE) {
        return false;
    }
    if (!is_on_last_module_path(ast, request->parent_module)) {
        return false;
    }
    const sxml_xslt_ast_node *referencing = sxml_xslt_ast_get_node(ast, request->referencing_node);
    const sxml_xslt_ast_kind expected_kind =
        request->relation == SXML_XSLT_AST_MODULE_RELATION_INCLUDED ? SXML_XSLT_AST_KIND_INCLUDE
        : request->relation == SXML_XSLT_AST_MODULE_RELATION_IMPORTED
            ? SXML_XSLT_AST_KIND_IMPORT
            : SXML_XSLT_AST_KIND_USE_PACKAGE;
    return referencing->kind == expected_kind &&
           referencing->module.value == request->parent_module.value;
}

sxml_status sxml_xslt_ast_add_module(sxml_xslt_ast *ast,
                                     const sxml_xslt_ast_module_request *request,
                                     sxml_xslt_ast_module_index *module) {
    if (ast->is_sealed || !is_module_request_valid(ast, request)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (used_entry_count(ast->module_count) >= ast->maximum_module_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    array_room room = {.elements = NULL, .element_count = 0, .element_capacity = 0};
    sxml_status status = make_array_room(
        ast,
        &(array_room_request){.elements = ast->modules,
                              .element_count = ast->module_count,
                              .element_capacity = ast->module_capacity,
                              .element_size = sizeof(sxml_xslt_ast_module),
                              .element_alignment = alignof(sxml_xslt_ast_module),
                              .added_count = 1},
        &room);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    ast->modules = room.elements;
    ast->module_capacity = room.element_capacity;
    void *uri_memory = NULL;
    if (request->module_uri.byte_count > 0) {
        status = sxml_allocator_allocate(
            ast->allocator,
            (sxml_allocation_request){.byte_count = request->module_uri.byte_count,
                                      .alignment = 1},
            &uri_memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        memcpy(uri_memory, request->module_uri.bytes, request->module_uri.byte_count);
    }
    sxml_tree *tree = NULL;
    sxml_node_index outermost_element = {.index = 0};
    sxml_node_reference document_node = {.tree = NULL, .node_index = {.index = 0},
                                         .namespace_number = 0};
    if (sxml_item_get_node(request->document, &document_node) == SXML_STATUS_OK) {
        tree = document_node.tree;
        sxml_node_description description;
        memset(&description, 0, sizeof(description));
        /* The document element: the one element child (XML 1.0, 2.1). */
        if (sxml_node_describe(document_node, &description) == SXML_STATUS_OK &&
            description.has_children) {
            sxml_node_index child = description.first_child;
            bool has_child = true;
            while (has_child) {
                sxml_node_description child_description;
                memset(&child_description, 0, sizeof(child_description));
                if (sxml_node_describe((sxml_node_reference){.tree = tree,
                                                             .node_index = child,
                                                             .namespace_number = 0},
                                       &child_description) != SXML_STATUS_OK) {
                    break;
                }
                if (child_description.node_kind == SXML_NODE_KIND_ELEMENT) {
                    outermost_element = child;
                    break;
                }
                has_child = child_description.has_next_sibling;
                child = child_description.next_sibling;
            }
        }
    }
    const uint32_t new_index = room.element_count;
    const sxml_xslt_ast_module *parent = sxml_xslt_ast_get_module(ast, request->parent_module);
    sxml_item_retain(request->document);
    ast->modules[new_index] = (sxml_xslt_ast_module){
        .relation = request->relation,
        .parent_module = request->parent_module,
        .referencing_node = request->referencing_node,
        .root = {.value = 0},
        .is_simplified = false,
        .document = request->document,
        .tree = tree,
        .outermost_element = outermost_element,
        .module_uri = {.bytes = uri_memory, .byte_count = request->module_uri.byte_count},
        .import_precedence = 0,
        .nesting_depth = request->parent_module.value == 0 ? 0U : parent->nesting_depth + 1U,
        .subtree_module_count = 1};
    ast->module_count = new_index + 1U;
    /* Each ancestor's subtree gains the new module. */
    uint32_t ancestor = request->parent_module.value;
    while (ancestor != 0) {
        ast->modules[ancestor].subtree_module_count += 1;
        ancestor = ast->modules[ancestor].parent_module.value;
    }
    if (request->referencing_node.value != 0) {
        ast->nodes[request->referencing_node.value].included_module.value = new_index;
    }
    *module = (sxml_xslt_ast_module_index){.value = new_index};
    return SXML_STATUS_OK;
}

/* The module of its stylesheet level: itself unless it was included, then
   its includer's (XSLT 4.0, 3.10.2). */
static uint32_t level_module_of(const sxml_xslt_ast *ast, uint32_t module) {
    uint32_t current = module;
    while (ast->modules[current].relation == SXML_XSLT_AST_MODULE_RELATION_INCLUDED) {
        current = ast->modules[current].parent_module.value;
    }
    return current;
}

/*
 * Import precedence (XSLT 4.0, 3.10.3): the post-order position of each
 * stylesheet level in the import tree. Modules are numbered in pre-order
 * of the module tree, so the level modules (principal and imported) in
 * that order are a pre-order of the import tree, and a level's import
 * subtree is the levels within its module subtree. A node's post-order
 * position (from 1) is the count of nodes up to the end of its subtree in
 * pre-order minus its number of ancestors.
 */
void sxml_xslt_ast_seal(sxml_xslt_ast *ast) {
    if (ast->is_sealed) {
        return;
    }
    /* First the count of level modules up to each module, in place. */
    uint32_t level_count = 0;
    for (uint32_t module = 1; module < ast->module_count; module += 1) {
        if (ast->modules[module].relation != SXML_XSLT_AST_MODULE_RELATION_INCLUDED) {
            level_count += 1;
        }
        ast->modules[module].import_precedence = level_count;
    }
    /* Each module reads the count at the end of its subtree, which is at
       or after itself and not yet overwritten. */
    for (uint32_t module = 1; module < ast->module_count; module += 1) {
        sxml_xslt_ast_module *entry = &ast->modules[module];
        if (entry->relation == SXML_XSLT_AST_MODULE_RELATION_INCLUDED) {
            entry->import_precedence = ast->modules[level_module_of(ast, module)].import_precedence;
            continue;
        }
        const uint32_t subtree_end = module + entry->subtree_module_count - 1U;
        uint32_t ancestor_count = 0;
        uint32_t ancestor = entry->parent_module.value;
        while (ancestor != 0) {
            ancestor = level_module_of(ast, ancestor);
            ancestor_count += 1;
            ancestor = ast->modules[ancestor].parent_module.value;
        }
        entry->import_precedence = ast->modules[subtree_end].import_precedence - ancestor_count;
    }
    ast->is_sealed = true;
}

/* ------------------------------------------------------------------ Nodes */

sxml_status sxml_xslt_ast_add_node(sxml_xslt_ast *ast, const sxml_xslt_ast_node_request *request,
                                   sxml_xslt_ast_node_index *node) {
    if (ast->is_sealed || request->kind == SXML_XSLT_AST_KIND_NONE ||
        request->kind >= SXML_XSLT_AST_KIND_COUNT || request->module.value == 0 ||
        request->module.value >= ast->module_count ||
        !is_optional_index_valid(request->static_context.value, ast->static_context_count)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (!has_room_for_node_or_attribute(ast)) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    const bool is_declaration = (request->flags & SXML_XSLT_AST_NODE_FLAG_DECLARATION) != 0;
    array_room declaration_room = {.elements = NULL, .element_count = 0, .element_capacity = 0};
    sxml_status status = SXML_STATUS_OK;
    if (is_declaration) {
        status = make_array_room(
            ast,
            &(array_room_request){.elements = ast->declarations,
                                  .element_count = ast->declaration_count,
                                  .element_capacity = ast->declaration_capacity,
                                  .element_size = sizeof(sxml_xslt_ast_node_index),
                                  .element_alignment = alignof(sxml_xslt_ast_node_index),
                                  .added_count = 1},
            &declaration_room);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        ast->declarations = declaration_room.elements;
        ast->declaration_capacity = declaration_room.element_capacity;
    }
    array_room room = {.elements = NULL, .element_count = 0, .element_capacity = 0};
    status = make_array_room(
        ast,
        &(array_room_request){.elements = ast->nodes,
                              .element_count = ast->node_count,
                              .element_capacity = ast->node_capacity,
                              .element_size = sizeof(sxml_xslt_ast_node),
                              .element_alignment = alignof(sxml_xslt_ast_node),
                              .added_count = 1},
        &room);
    if (status != SXML_STATUS_OK) {
        /* The declaration list kept its count: the room made is unused. */
        return status;
    }
    ast->nodes = room.elements;
    ast->node_capacity = room.element_capacity;
    const uint32_t new_index = room.element_count;
    uint32_t declaration_order = 0;
    if (is_declaration) {
        declaration_order = declaration_room.element_count;
        ast->declarations[declaration_order] = (sxml_xslt_ast_node_index){.value = new_index};
        ast->declaration_count = declaration_order + 1U;
    }
    ast->nodes[new_index] = (sxml_xslt_ast_node){
        .kind = request->kind,
        .flags = request->flags,
        .module = request->module,
        .source_node = request->source_node,
        .parent = {.value = 0},
        .first_child = {.value = 0},
        .last_child = {.value = 0},
        .next_sibling = {.value = 0},
        .static_context = request->static_context,
        .attributes = {.first_attribute_index = 0, .attribute_count = 0},
        .element_name = request->element_name,
        .namespace_prefix = request->namespace_prefix,
        .content = request->content,
        .included_module = {.value = 0},
        .declaration_order = declaration_order,
        .source_span = request->source_span};
    ast->node_count = new_index + 1U;
    sxml_xslt_ast_module *module = &ast->modules[request->module.value];
    if (module->root.value == 0) {
        module->root = (sxml_xslt_ast_node_index){.value = new_index};
        /* A synthesized root is that of a simplified module (3.7). */
        module->is_simplified = request->kind == SXML_XSLT_AST_KIND_STYLESHEET &&
                                (request->flags & SXML_XSLT_AST_NODE_FLAG_SYNTHESIZED) != 0;
    }
    *node = (sxml_xslt_ast_node_index){.value = new_index};
    return SXML_STATUS_OK;
}

sxml_status sxml_xslt_ast_append_child(sxml_xslt_ast *ast, sxml_xslt_ast_child_request request) {
    const uint32_t parent = request.parent.value;
    const uint32_t child = request.child.value;
    if (ast->is_sealed || parent == 0 || child == 0 || parent >= ast->node_count ||
        child >= ast->node_count || parent == child || ast->nodes[child].parent.value != 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_xslt_ast_node *parent_node = &ast->nodes[parent];
    if (parent_node->last_child.value == 0) {
        parent_node->first_child = request.child;
    } else {
        ast->nodes[parent_node->last_child.value].next_sibling = request.child;
    }
    parent_node->last_child = request.child;
    ast->nodes[child].parent = request.parent;
    return SXML_STATUS_OK;
}

sxml_status sxml_xslt_ast_add_attribute(sxml_xslt_ast *ast,
                                        const sxml_xslt_ast_attribute_request *request,
                                        sxml_xslt_ast_attribute_index *attribute) {
    if (ast->is_sealed || request->node.value == 0 ||
        request->node.value + 1U != ast->node_count ||
        request->kind == SXML_XSLT_AST_ATTRIBUTE_NONE ||
        request->kind >= SXML_XSLT_AST_ATTRIBUTE_KIND_COUNT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (!has_room_for_node_or_attribute(ast)) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    array_room room = {.elements = NULL, .element_count = 0, .element_capacity = 0};
    const sxml_status status = make_array_room(
        ast,
        &(array_room_request){.elements = ast->attributes,
                              .element_count = ast->attribute_count,
                              .element_capacity = ast->attribute_capacity,
                              .element_size = sizeof(sxml_xslt_ast_attribute),
                              .element_alignment = alignof(sxml_xslt_ast_attribute),
                              .added_count = 1},
        &room);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    ast->attributes = room.elements;
    ast->attribute_capacity = room.element_capacity;
    const uint32_t new_index = room.element_count;
    ast->attributes[new_index] = (sxml_xslt_ast_attribute){
        .kind = request->kind,
        .flags = request->flags,
        .node = request->node,
        .attribute_name = request->attribute_name,
        .namespace_prefix = request->namespace_prefix,
        .value = request->value,
        .source_span = request->source_span};
    ast->attribute_count = new_index + 1U;
    sxml_xslt_ast_attribute_list *list = &ast->nodes[request->node.value].attributes;
    if (list->attribute_count == 0) {
        list->first_attribute_index = new_index;
    }
    list->attribute_count += 1;
    *attribute = (sxml_xslt_ast_attribute_index){.value = new_index};
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Expressions */

static bool is_expression_kind(sxml_xslt_ast_value_kind kind) {
    switch (kind) {
    case SXML_XSLT_AST_VALUE_KIND_EXPRESSION:
    case SXML_XSLT_AST_VALUE_KIND_PATTERN:
    case SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE:
    case SXML_XSLT_AST_VALUE_KIND_SEQUENCE_TYPE:
    case SXML_XSLT_AST_VALUE_KIND_ITEM_TYPE:
        return true;
    case SXML_XSLT_AST_VALUE_KIND_NONE:
    case SXML_XSLT_AST_VALUE_KIND_TEXT:
    case SXML_XSLT_AST_VALUE_KIND_BOOLEAN:
    case SXML_XSLT_AST_VALUE_KIND_NUMBER:
    case SXML_XSLT_AST_VALUE_KIND_KEYWORD:
    case SXML_XSLT_AST_VALUE_KIND_NAME:
    case SXML_XSLT_AST_VALUE_KIND_NAME_LIST:
    case SXML_XSLT_AST_VALUE_KIND_NAMESPACE_BINDING:
        return false;
    }
    return false;
}

sxml_status sxml_xslt_ast_take_expression(sxml_xslt_ast *ast,
                                          const sxml_xslt_ast_expression_request *request,
                                          sxml_xslt_ast_expression_index *expression) {
    if (ast->is_sealed || !is_expression_kind(request->kind) || request->expression_ast == NULL ||
        !is_optional_index_valid(request->static_context.value, ast->static_context_count) ||
        !is_optional_index_valid(request->node.value, ast->node_count)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    array_room room = {.elements = NULL, .element_count = 0, .element_capacity = 0};
    const sxml_status status = make_array_room(
        ast,
        &(array_room_request){.elements = ast->expressions,
                              .element_count = ast->expression_count,
                              .element_capacity = ast->expression_capacity,
                              .element_size = sizeof(sxml_xslt_ast_expression),
                              .element_alignment = alignof(sxml_xslt_ast_expression),
                              .added_count = 1},
        &room);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    ast->expressions = room.elements;
    ast->expression_capacity = room.element_capacity;
    const uint32_t new_index = room.element_count;
    ast->expressions[new_index] = (sxml_xslt_ast_expression){
        .kind = request->kind,
        .ast = request->expression_ast,
        .static_context = request->static_context,
        .node = request->node,
        .source_span = request->source_span};
    ast->expression_count = new_index + 1U;
    *expression = (sxml_xslt_ast_expression_index){.value = new_index};
    return SXML_STATUS_OK;
}

/* ----------------------------------------------- Static contexts and lists */

/* Room for count more namespace bindings; the array's first free entry is
   the old count (or 1). */
static sxml_status make_namespace_binding_room(sxml_xslt_ast *ast, size_t count,
                                               uint32_t *first_free) {
    if (count > XSLT_AST_ARRAY_MAXIMUM_COUNT) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    array_room room = {.elements = NULL, .element_count = 0, .element_capacity = 0};
    const sxml_status status = make_array_room(
        ast,
        &(array_room_request){.elements = ast->namespace_bindings,
                              .element_count = ast->namespace_binding_count,
                              .element_capacity = ast->namespace_binding_capacity,
                              .element_size = sizeof(sxml_namespace_binding),
                              .element_alignment = alignof(sxml_namespace_binding),
                              .added_count = (uint32_t)count},
        &room);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    ast->namespace_bindings = room.elements;
    ast->namespace_binding_capacity = room.element_capacity;
    if (ast->namespace_binding_count == 0) {
        ast->namespace_binding_count = 1;
    }
    *first_free = room.element_count;
    return SXML_STATUS_OK;
}

static sxml_status make_namespace_uri_room(sxml_xslt_ast *ast, size_t count,
                                           uint32_t *first_free) {
    if (count > XSLT_AST_ARRAY_MAXIMUM_COUNT) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    array_room room = {.elements = NULL, .element_count = 0, .element_capacity = 0};
    const sxml_status status = make_array_room(
        ast,
        &(array_room_request){.elements = ast->namespace_uris,
                              .element_count = ast->namespace_uri_count,
                              .element_capacity = ast->namespace_uri_capacity,
                              .element_size = sizeof(sxml_namespace_uri_identifier),
                              .element_alignment = alignof(sxml_namespace_uri_identifier),
                              .added_count = (uint32_t)count},
        &room);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    ast->namespace_uris = room.elements;
    ast->namespace_uri_capacity = room.element_capacity;
    if (ast->namespace_uri_count == 0) {
        ast->namespace_uri_count = 1;
    }
    *first_free = room.element_count;
    return SXML_STATUS_OK;
}

/* The bytes free in the text store, and its first free offset, after
   making room for byte_count more. */
static sxml_status make_text_room(sxml_xslt_ast *ast, size_t byte_count) {
    if (byte_count > XSLT_AST_ARRAY_MAXIMUM_COUNT) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    array_room room = {.elements = NULL, .element_count = 0, .element_capacity = 0};
    const sxml_status status = make_array_room(
        ast,
        &(array_room_request){.elements = ast->text_bytes,
                              .element_count = ast->text_byte_count,
                              .element_capacity = ast->text_byte_capacity,
                              .element_size = 1,
                              .element_alignment = 1,
                              .added_count = (uint32_t)byte_count},
        &room);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    ast->text_bytes = room.elements;
    ast->text_byte_capacity = room.element_capacity;
    if (ast->text_byte_count == 0) {
        ast->text_byte_count = 1;
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_xslt_ast_append_text(sxml_xslt_ast *ast, sxml_byte_view bytes,
                                      sxml_xslt_ast_text *text) {
    const bool extends_text = text->byte_count != 0;
    if (ast->is_sealed || (bytes.byte_count > 0 && bytes.bytes == NULL)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (extends_text && (text->byte_offset > ast->text_byte_count ||
                         text->byte_count != ast->text_byte_count - text->byte_offset)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (bytes.byte_count == 0) {
        if (!extends_text) {
            *text = (sxml_xslt_ast_text){.byte_offset = 0, .byte_count = 0};
        }
        return SXML_STATUS_OK;
    }
    const sxml_status status = make_text_room(ast, bytes.byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint32_t start = ast->text_byte_count;
    memcpy(ast->text_bytes + start, bytes.bytes, bytes.byte_count);
    /* make_text_room bounded byte_count by the array maximum. */
    const uint32_t appended_byte_count = (uint32_t)bytes.byte_count;
    ast->text_byte_count = start + appended_byte_count;
    if (extends_text) {
        text->byte_count += appended_byte_count;
    } else {
        *text = (sxml_xslt_ast_text){.byte_offset = start, .byte_count = appended_byte_count};
    }
    return SXML_STATUS_OK;
}

static bool is_static_context_request_valid(const sxml_xslt_ast *ast,
                                            const sxml_xslt_ast_static_context_request *request) {
    return is_optional_index_valid(request->parent.value, ast->static_context_count) &&
           (request->namespace_bindings.namespace_binding_count == 0 ||
            request->namespace_bindings.namespace_bindings != NULL) &&
           (request->excluded_namespaces.namespace_uri_count == 0 ||
            request->excluded_namespaces.namespace_uris != NULL) &&
           (request->extension_namespaces.namespace_uri_count == 0 ||
            request->extension_namespaces.namespace_uris != NULL) &&
           (request->default_collation.byte_count == 0 ||
            request->default_collation.bytes != NULL) &&
           (request->static_base_uri.byte_count == 0 || request->static_base_uri.bytes != NULL);
}

/* Copies uris into the namespace URI array as one run. */
static sxml_status add_namespace_uris(sxml_xslt_ast *ast, sxml_xslt_ast_namespace_uri_view uris,
                                      sxml_xslt_ast_namespace_uri_list *list) {
    if (uris.namespace_uri_count == 0) {
        *list = (sxml_xslt_ast_namespace_uri_list){.first_namespace_uri_index = 0,
                                                   .namespace_uri_count = 0};
        return SXML_STATUS_OK;
    }
    uint32_t first = 0;
    const sxml_status status = make_namespace_uri_room(ast, uris.namespace_uri_count, &first);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memcpy(ast->namespace_uris + first, uris.namespace_uris,
           uris.namespace_uri_count * sizeof(sxml_namespace_uri_identifier));
    ast->namespace_uri_count = first + (uint32_t)uris.namespace_uri_count;
    *list = (sxml_xslt_ast_namespace_uri_list){
        .first_namespace_uri_index = first,
        .namespace_uri_count = (uint32_t)uris.namespace_uri_count};
    return SXML_STATUS_OK;
}

sxml_status sxml_xslt_ast_add_static_context(
    sxml_xslt_ast *ast, const sxml_xslt_ast_static_context_request *request,
    sxml_xslt_ast_static_context_index *static_context) {
    if (ast->is_sealed || !is_static_context_request_valid(ast, request)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* Room in the context array first; the runs added after it are only
       lost space if a later step fails, never a broken entry. */
    array_room room = {.elements = NULL, .element_count = 0, .element_capacity = 0};
    sxml_status status = make_array_room(
        ast,
        &(array_room_request){.elements = ast->static_contexts,
                              .element_count = ast->static_context_count,
                              .element_capacity = ast->static_context_capacity,
                              .element_size = sizeof(sxml_xslt_ast_static_context),
                              .element_alignment = alignof(sxml_xslt_ast_static_context),
                              .added_count = 1},
        &room);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    ast->static_contexts = room.elements;
    ast->static_context_capacity = room.element_capacity;
    sxml_xslt_ast_namespace_binding_list bindings = {.first_namespace_binding_index = 0,
                                                     .namespace_binding_count = 0};
    const size_t binding_count = request->namespace_bindings.namespace_binding_count;
    if (binding_count > 0) {
        uint32_t first = 0;
        status = make_namespace_binding_room(ast, binding_count, &first);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        memcpy(ast->namespace_bindings + first, request->namespace_bindings.namespace_bindings,
               binding_count * sizeof(sxml_namespace_binding));
        ast->namespace_binding_count = first + (uint32_t)binding_count;
        bindings = (sxml_xslt_ast_namespace_binding_list){
            .first_namespace_binding_index = first,
            .namespace_binding_count = (uint32_t)binding_count};
    }
    sxml_xslt_ast_namespace_uri_list excluded = {.first_namespace_uri_index = 0,
                                                 .namespace_uri_count = 0};
    sxml_xslt_ast_namespace_uri_list extension = excluded;
    status = add_namespace_uris(ast, request->excluded_namespaces, &excluded);
    if (status == SXML_STATUS_OK) {
        status = add_namespace_uris(ast, request->extension_namespaces, &extension);
    }
    sxml_xslt_ast_text collation = {.byte_offset = 0, .byte_count = 0};
    sxml_xslt_ast_text base_uri = {.byte_offset = 0, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_ast_append_text(ast, request->default_collation, &collation);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_ast_append_text(ast, request->static_base_uri, &base_uri);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint32_t new_index = room.element_count;
    ast->static_contexts[new_index] = (sxml_xslt_ast_static_context){
        .parent = request->parent,
        .namespace_bindings = bindings,
        .xpath_default_namespace = request->xpath_default_namespace,
        .is_any_namespace = request->is_any_namespace,
        .default_collation = collation,
        .default_mode = request->default_mode,
        .default_validation = request->default_validation,
        .expands_text = request->expands_text,
        .version = request->version,
        .excluded_namespaces = excluded,
        .extension_namespaces = extension,
        .static_base_uri = base_uri};
    ast->static_context_count = new_index + 1U;
    *static_context = (sxml_xslt_ast_static_context_index){.value = new_index};
    return SXML_STATUS_OK;
}

sxml_status sxml_xslt_ast_add_name_tokens(sxml_xslt_ast *ast,
                                          sxml_xslt_ast_name_token_view name_tokens,
                                          sxml_xslt_ast_name_token_list *list) {
    if (ast->is_sealed || (name_tokens.name_token_count > 0 && name_tokens.name_tokens == NULL)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (name_tokens.name_token_count == 0) {
        *list = (sxml_xslt_ast_name_token_list){.first_name_token_index = 0,
                                                .name_token_count = 0};
        return SXML_STATUS_OK;
    }
    if (name_tokens.name_token_count > XSLT_AST_ARRAY_MAXIMUM_COUNT) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    array_room room = {.elements = NULL, .element_count = 0, .element_capacity = 0};
    const sxml_status status = make_array_room(
        ast,
        &(array_room_request){.elements = ast->name_tokens,
                              .element_count = ast->name_token_count,
                              .element_capacity = ast->name_token_capacity,
                              .element_size = sizeof(sxml_xslt_ast_name_token),
                              .element_alignment = alignof(sxml_xslt_ast_name_token),
                              .added_count = (uint32_t)name_tokens.name_token_count},
        &room);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    ast->name_tokens = room.elements;
    ast->name_token_capacity = room.element_capacity;
    const uint32_t first = room.element_count;
    memcpy(ast->name_tokens + first, name_tokens.name_tokens,
           name_tokens.name_token_count * sizeof(sxml_xslt_ast_name_token));
    ast->name_token_count = first + (uint32_t)name_tokens.name_token_count;
    *list = (sxml_xslt_ast_name_token_list){
        .first_name_token_index = first,
        .name_token_count = (uint32_t)name_tokens.name_token_count};
    return SXML_STATUS_OK;
}

/* -------------------------------------------------------------- Text form */

/* Generated from the enumerations of src/xslt_ast_internal.h. */
static const char *const kind_names[] = {
    [SXML_XSLT_AST_KIND_NONE] = "NONE",
    [SXML_XSLT_AST_KIND_LITERAL_RESULT_ELEMENT] = "LITERAL_RESULT_ELEMENT",
    [SXML_XSLT_AST_KIND_TEXT_NODE] = "TEXT_NODE",
    [SXML_XSLT_AST_KIND_EXTENSION_INSTRUCTION] = "EXTENSION_INSTRUCTION",
    [SXML_XSLT_AST_KIND_UNKNOWN_INSTRUCTION] = "UNKNOWN_INSTRUCTION",
    [SXML_XSLT_AST_KIND_DATA_ELEMENT] = "DATA_ELEMENT",
    [SXML_XSLT_AST_KIND_STYLESHEET] = "STYLESHEET",
    [SXML_XSLT_AST_KIND_PACKAGE] = "PACKAGE",
    [SXML_XSLT_AST_KIND_ACCUMULATOR] = "ACCUMULATOR",
    [SXML_XSLT_AST_KIND_ATTRIBUTE_SET] = "ATTRIBUTE_SET",
    [SXML_XSLT_AST_KIND_CHARACTER_MAP] = "CHARACTER_MAP",
    [SXML_XSLT_AST_KIND_DECIMAL_FORMAT] = "DECIMAL_FORMAT",
    [SXML_XSLT_AST_KIND_FUNCTION] = "FUNCTION",
    [SXML_XSLT_AST_KIND_GLOBAL_CONTEXT_ITEM] = "GLOBAL_CONTEXT_ITEM",
    [SXML_XSLT_AST_KIND_IMPORT] = "IMPORT",
    [SXML_XSLT_AST_KIND_IMPORT_SCHEMA] = "IMPORT_SCHEMA",
    [SXML_XSLT_AST_KIND_INCLUDE] = "INCLUDE",
    [SXML_XSLT_AST_KIND_ITEM_TYPE] = "ITEM_TYPE",
    [SXML_XSLT_AST_KIND_KEY] = "KEY",
    [SXML_XSLT_AST_KIND_MODE] = "MODE",
    [SXML_XSLT_AST_KIND_NAMESPACE_ALIAS] = "NAMESPACE_ALIAS",
    [SXML_XSLT_AST_KIND_OUTPUT] = "OUTPUT",
    [SXML_XSLT_AST_KIND_PARAM] = "PARAM",
    [SXML_XSLT_AST_KIND_PRESERVE_SPACE] = "PRESERVE_SPACE",
    [SXML_XSLT_AST_KIND_RECORD_TYPE] = "RECORD_TYPE",
    [SXML_XSLT_AST_KIND_STRIP_SPACE] = "STRIP_SPACE",
    [SXML_XSLT_AST_KIND_TEMPLATE] = "TEMPLATE",
    [SXML_XSLT_AST_KIND_USE_PACKAGE] = "USE_PACKAGE",
    [SXML_XSLT_AST_KIND_VARIABLE] = "VARIABLE",
    [SXML_XSLT_AST_KIND_ANALYZE_STRING] = "ANALYZE_STRING",
    [SXML_XSLT_AST_KIND_APPLY_IMPORTS] = "APPLY_IMPORTS",
    [SXML_XSLT_AST_KIND_APPLY_TEMPLATES] = "APPLY_TEMPLATES",
    [SXML_XSLT_AST_KIND_ARRAY] = "ARRAY",
    [SXML_XSLT_AST_KIND_ARRAY_MEMBER] = "ARRAY_MEMBER",
    [SXML_XSLT_AST_KIND_ASSERT] = "ASSERT",
    [SXML_XSLT_AST_KIND_ATTRIBUTE] = "ATTRIBUTE",
    [SXML_XSLT_AST_KIND_BREAK] = "BREAK",
    [SXML_XSLT_AST_KIND_CALL_TEMPLATE] = "CALL_TEMPLATE",
    [SXML_XSLT_AST_KIND_CHOOSE] = "CHOOSE",
    [SXML_XSLT_AST_KIND_COMMENT] = "COMMENT",
    [SXML_XSLT_AST_KIND_COPY] = "COPY",
    [SXML_XSLT_AST_KIND_COPY_OF] = "COPY_OF",
    [SXML_XSLT_AST_KIND_DOCUMENT] = "DOCUMENT",
    [SXML_XSLT_AST_KIND_ELEMENT] = "ELEMENT",
    [SXML_XSLT_AST_KIND_EVALUATE] = "EVALUATE",
    [SXML_XSLT_AST_KIND_FALLBACK] = "FALLBACK",
    [SXML_XSLT_AST_KIND_FOR_EACH] = "FOR_EACH",
    [SXML_XSLT_AST_KIND_FOR_EACH_GROUP] = "FOR_EACH_GROUP",
    [SXML_XSLT_AST_KIND_FORK] = "FORK",
    [SXML_XSLT_AST_KIND_IF] = "IF",
    [SXML_XSLT_AST_KIND_ITERATE] = "ITERATE",
    [SXML_XSLT_AST_KIND_MAP] = "MAP",
    [SXML_XSLT_AST_KIND_MAP_ENTRY] = "MAP_ENTRY",
    [SXML_XSLT_AST_KIND_MERGE] = "MERGE",
    [SXML_XSLT_AST_KIND_MESSAGE] = "MESSAGE",
    [SXML_XSLT_AST_KIND_NAMESPACE] = "NAMESPACE",
    [SXML_XSLT_AST_KIND_NEXT_ITERATION] = "NEXT_ITERATION",
    [SXML_XSLT_AST_KIND_NEXT_MATCH] = "NEXT_MATCH",
    [SXML_XSLT_AST_KIND_NUMBER] = "NUMBER",
    [SXML_XSLT_AST_KIND_ON_EMPTY] = "ON_EMPTY",
    [SXML_XSLT_AST_KIND_ON_NON_EMPTY] = "ON_NON_EMPTY",
    [SXML_XSLT_AST_KIND_PERFORM_SORT] = "PERFORM_SORT",
    [SXML_XSLT_AST_KIND_PROCESSING_INSTRUCTION] = "PROCESSING_INSTRUCTION",
    [SXML_XSLT_AST_KIND_RESULT_DOCUMENT] = "RESULT_DOCUMENT",
    [SXML_XSLT_AST_KIND_SELECT] = "SELECT",
    [SXML_XSLT_AST_KIND_SEQUENCE] = "SEQUENCE",
    [SXML_XSLT_AST_KIND_SOURCE_DOCUMENT] = "SOURCE_DOCUMENT",
    [SXML_XSLT_AST_KIND_SWITCH] = "SWITCH",
    [SXML_XSLT_AST_KIND_TEXT] = "TEXT",
    [SXML_XSLT_AST_KIND_TRY] = "TRY",
    [SXML_XSLT_AST_KIND_VALUE_OF] = "VALUE_OF",
    [SXML_XSLT_AST_KIND_WHERE_POPULATED] = "WHERE_POPULATED",
    [SXML_XSLT_AST_KIND_ACCEPT] = "ACCEPT",
    [SXML_XSLT_AST_KIND_ACCUMULATOR_RULE] = "ACCUMULATOR_RULE",
    [SXML_XSLT_AST_KIND_CATCH] = "CATCH",
    [SXML_XSLT_AST_KIND_CONTEXT_ITEM] = "CONTEXT_ITEM",
    [SXML_XSLT_AST_KIND_EXPOSE] = "EXPOSE",
    [SXML_XSLT_AST_KIND_FIELD] = "FIELD",
    [SXML_XSLT_AST_KIND_MATCHING_SUBSTRING] = "MATCHING_SUBSTRING",
    [SXML_XSLT_AST_KIND_MERGE_ACTION] = "MERGE_ACTION",
    [SXML_XSLT_AST_KIND_MERGE_KEY] = "MERGE_KEY",
    [SXML_XSLT_AST_KIND_MERGE_SOURCE] = "MERGE_SOURCE",
    [SXML_XSLT_AST_KIND_NON_MATCHING_SUBSTRING] = "NON_MATCHING_SUBSTRING",
    [SXML_XSLT_AST_KIND_ON_COMPLETION] = "ON_COMPLETION",
    [SXML_XSLT_AST_KIND_OTHERWISE] = "OTHERWISE",
    [SXML_XSLT_AST_KIND_OUTPUT_CHARACTER] = "OUTPUT_CHARACTER",
    [SXML_XSLT_AST_KIND_OVERRIDE] = "OVERRIDE",
    [SXML_XSLT_AST_KIND_PACKAGE_LOCATION] = "PACKAGE_LOCATION",
    [SXML_XSLT_AST_KIND_SORT] = "SORT",
    [SXML_XSLT_AST_KIND_WHEN] = "WHEN",
    [SXML_XSLT_AST_KIND_WITH_PARAM] = "WITH_PARAM",
    [SXML_XSLT_AST_KIND_RECORD] = "RECORD",
};
_Static_assert(sizeof(kind_names) / sizeof(kind_names[0]) == SXML_XSLT_AST_KIND_COUNT,
               "one name per node kind");

static const char *const attribute_kind_names[] = {
    [SXML_XSLT_AST_ATTRIBUTE_NONE] = "NONE",
    [SXML_XSLT_AST_ATTRIBUTE_LITERAL_RESULT] = "LITERAL_RESULT",
    [SXML_XSLT_AST_ATTRIBUTE_FOREIGN] = "FOREIGN",
    [SXML_XSLT_AST_ATTRIBUTE_DEFAULT_COLLATION] = "DEFAULT_COLLATION",
    [SXML_XSLT_AST_ATTRIBUTE_DEFAULT_MODE] = "DEFAULT_MODE",
    [SXML_XSLT_AST_ATTRIBUTE_DEFAULT_VALIDATION] = "DEFAULT_VALIDATION",
    [SXML_XSLT_AST_ATTRIBUTE_EXCLUDE_RESULT_PREFIXES] = "EXCLUDE_RESULT_PREFIXES",
    [SXML_XSLT_AST_ATTRIBUTE_EXPAND_TEXT] = "EXPAND_TEXT",
    [SXML_XSLT_AST_ATTRIBUTE_EXTENSION_ELEMENT_PREFIXES] = "EXTENSION_ELEMENT_PREFIXES",
    [SXML_XSLT_AST_ATTRIBUTE_FIXED_NAMESPACES] = "FIXED_NAMESPACES",
    [SXML_XSLT_AST_ATTRIBUTE_SCHEMA_ROLE] = "SCHEMA_ROLE",
    [SXML_XSLT_AST_ATTRIBUTE_USE_WHEN] = "USE_WHEN",
    [SXML_XSLT_AST_ATTRIBUTE_VERSION] = "VERSION",
    [SXML_XSLT_AST_ATTRIBUTE_XPATH_DEFAULT_NAMESPACE] = "XPATH_DEFAULT_NAMESPACE",
    [SXML_XSLT_AST_ATTRIBUTE_ALLOW_DUPLICATE_NAMES] = "ALLOW_DUPLICATE_NAMES",
    [SXML_XSLT_AST_ATTRIBUTE_ARCHIVE_TYPE] = "ARCHIVE_TYPE",
    [SXML_XSLT_AST_ATTRIBUTE_AS] = "AS",
    [SXML_XSLT_AST_ATTRIBUTE_BASE_URI] = "BASE_URI",
    [SXML_XSLT_AST_ATTRIBUTE_BUILD_TREE] = "BUILD_TREE",
    [SXML_XSLT_AST_ATTRIBUTE_BYTE_ORDER_MARK] = "BYTE_ORDER_MARK",
    [SXML_XSLT_AST_ATTRIBUTE_CACHE] = "CACHE",
    [SXML_XSLT_AST_ATTRIBUTE_CANONICAL] = "CANONICAL",
    [SXML_XSLT_AST_ATTRIBUTE_CAPTURE] = "CAPTURE",
    [SXML_XSLT_AST_ATTRIBUTE_CASE_ORDER] = "CASE_ORDER",
    [SXML_XSLT_AST_ATTRIBUTE_CDATA] = "CDATA",
    [SXML_XSLT_AST_ATTRIBUTE_CDATA_SECTION_ELEMENTS] = "CDATA_SECTION_ELEMENTS",
    [SXML_XSLT_AST_ATTRIBUTE_CHARACTER] = "CHARACTER",
    [SXML_XSLT_AST_ATTRIBUTE_COLLATION] = "COLLATION",
    [SXML_XSLT_AST_ATTRIBUTE_COMPILED_REGEX] = "COMPILED_REGEX",
    [SXML_XSLT_AST_ATTRIBUTE_COMPONENT] = "COMPONENT",
    [SXML_XSLT_AST_ATTRIBUTE_COMPOSITE] = "COMPOSITE",
    [SXML_XSLT_AST_ATTRIBUTE_CONTEXT_ITEM] = "CONTEXT_ITEM",
    [SXML_XSLT_AST_ATTRIBUTE_COPY_ACCUMULATORS] = "COPY_ACCUMULATORS",
    [SXML_XSLT_AST_ATTRIBUTE_COPY_NAMESPACES] = "COPY_NAMESPACES",
    [SXML_XSLT_AST_ATTRIBUTE_COUNT] = "COUNT",
    [SXML_XSLT_AST_ATTRIBUTE_CSV_HEADER] = "CSV_HEADER",
    [SXML_XSLT_AST_ATTRIBUTE_CSV_QUOTE_CHARACTER] = "CSV_QUOTE_CHARACTER",
    [SXML_XSLT_AST_ATTRIBUTE_CSV_SEPARATOR] = "CSV_SEPARATOR",
    [SXML_XSLT_AST_ATTRIBUTE_DATA_TYPE] = "DATA_TYPE",
    [SXML_XSLT_AST_ATTRIBUTE_DECIMAL_SEPARATOR] = "DECIMAL_SEPARATOR",
    [SXML_XSLT_AST_ATTRIBUTE_DECLARED_MODES] = "DECLARED_MODES",
    [SXML_XSLT_AST_ATTRIBUTE_DEFAULT] = "DEFAULT",
    [SXML_XSLT_AST_ATTRIBUTE_DIGIT] = "DIGIT",
    [SXML_XSLT_AST_ATTRIBUTE_DISABLE_OUTPUT_ESCAPING] = "DISABLE_OUTPUT_ESCAPING",
    [SXML_XSLT_AST_ATTRIBUTE_DOCTYPE_PUBLIC] = "DOCTYPE_PUBLIC",
    [SXML_XSLT_AST_ATTRIBUTE_DOCTYPE_SYSTEM] = "DOCTYPE_SYSTEM",
    [SXML_XSLT_AST_ATTRIBUTE_DUPLICATES] = "DUPLICATES",
    [SXML_XSLT_AST_ATTRIBUTE_ELEMENTS] = "ELEMENTS",
    [SXML_XSLT_AST_ATTRIBUTE_ELSE] = "ELSE",
    [SXML_XSLT_AST_ATTRIBUTE_ENCODING] = "ENCODING",
    [SXML_XSLT_AST_ATTRIBUTE_ERRORS] = "ERRORS",
    [SXML_XSLT_AST_ATTRIBUTE_ERROR_CODE] = "ERROR_CODE",
    [SXML_XSLT_AST_ATTRIBUTE_ESCAPE_SOLIDUS] = "ESCAPE_SOLIDUS",
    [SXML_XSLT_AST_ATTRIBUTE_ESCAPE_URI_ATTRIBUTES] = "ESCAPE_URI_ATTRIBUTES",
    [SXML_XSLT_AST_ATTRIBUTE_EXPONENT_SEPARATOR] = "EXPONENT_SEPARATOR",
    [SXML_XSLT_AST_ATTRIBUTE_FLAGS] = "FLAGS",
    [SXML_XSLT_AST_ATTRIBUTE_FORMAT] = "FORMAT",
    [SXML_XSLT_AST_ATTRIBUTE_FOR_EACH] = "FOR_EACH",
    [SXML_XSLT_AST_ATTRIBUTE_FOR_EACH_ITEM] = "FOR_EACH_ITEM",
    [SXML_XSLT_AST_ATTRIBUTE_FOR_EACH_SOURCE] = "FOR_EACH_SOURCE",
    [SXML_XSLT_AST_ATTRIBUTE_FROM] = "FROM",
    [SXML_XSLT_AST_ATTRIBUTE_GROUPING_SEPARATOR] = "GROUPING_SEPARATOR",
    [SXML_XSLT_AST_ATTRIBUTE_GROUPING_SIZE] = "GROUPING_SIZE",
    [SXML_XSLT_AST_ATTRIBUTE_GROUP_ADJACENT] = "GROUP_ADJACENT",
    [SXML_XSLT_AST_ATTRIBUTE_GROUP_BY] = "GROUP_BY",
    [SXML_XSLT_AST_ATTRIBUTE_GROUP_ENDING_WITH] = "GROUP_ENDING_WITH",
    [SXML_XSLT_AST_ATTRIBUTE_GROUP_STARTING_WITH] = "GROUP_STARTING_WITH",
    [SXML_XSLT_AST_ATTRIBUTE_HREF] = "HREF",
    [SXML_XSLT_AST_ATTRIBUTE_HTML_VERSION] = "HTML_VERSION",
    [SXML_XSLT_AST_ATTRIBUTE_ID] = "ID",
    [SXML_XSLT_AST_ATTRIBUTE_INCLUDE_CONTENT_TYPE] = "INCLUDE_CONTENT_TYPE",
    [SXML_XSLT_AST_ATTRIBUTE_INDENT] = "INDENT",
    [SXML_XSLT_AST_ATTRIBUTE_INDENT_ATTRIBUTES] = "INDENT_ATTRIBUTES",
    [SXML_XSLT_AST_ATTRIBUTE_INDENT_UNIT] = "INDENT_UNIT",
    [SXML_XSLT_AST_ATTRIBUTE_INFINITY] = "INFINITY",
    [SXML_XSLT_AST_ATTRIBUTE_INHERIT_NAMESPACES] = "INHERIT_NAMESPACES",
    [SXML_XSLT_AST_ATTRIBUTE_INITIAL_VALUE] = "INITIAL_VALUE",
    [SXML_XSLT_AST_ATTRIBUTE_INPUT_TYPE_ANNOTATIONS] = "INPUT_TYPE_ANNOTATIONS",
    [SXML_XSLT_AST_ATTRIBUTE_IS_PRIORITY] = "IS_PRIORITY",
    [SXML_XSLT_AST_ATTRIBUTE_ITEM_SEPARATOR] = "ITEM_SEPARATOR",
    [SXML_XSLT_AST_ATTRIBUTE_JSON_LINES] = "JSON_LINES",
    [SXML_XSLT_AST_ATTRIBUTE_JSON_NODE_OUTPUT_METHOD] = "JSON_NODE_OUTPUT_METHOD",
    [SXML_XSLT_AST_ATTRIBUTE_KEY] = "KEY",
    [SXML_XSLT_AST_ATTRIBUTE_LANG] = "LANG",
    [SXML_XSLT_AST_ATTRIBUTE_LETTER_VALUE] = "LETTER_VALUE",
    [SXML_XSLT_AST_ATTRIBUTE_LEVEL] = "LEVEL",
    [SXML_XSLT_AST_ATTRIBUTE_LINE_ENDING] = "LINE_ENDING",
    [SXML_XSLT_AST_ATTRIBUTE_MAIN_MODULE] = "MAIN_MODULE",
    [SXML_XSLT_AST_ATTRIBUTE_MATCH] = "MATCH",
    [SXML_XSLT_AST_ATTRIBUTE_MEDIA_TYPE] = "MEDIA_TYPE",
    [SXML_XSLT_AST_ATTRIBUTE_MERGE_WHEN] = "MERGE_WHEN",
    [SXML_XSLT_AST_ATTRIBUTE_METHOD] = "METHOD",
    [SXML_XSLT_AST_ATTRIBUTE_MINUS_SIGN] = "MINUS_SIGN",
    [SXML_XSLT_AST_ATTRIBUTE_MODE] = "MODE",
    [SXML_XSLT_AST_ATTRIBUTE_NAME] = "NAME",
    [SXML_XSLT_AST_ATTRIBUTE_NAMES] = "NAMES",
    [SXML_XSLT_AST_ATTRIBUTE_NAMESPACE] = "NAMESPACE",
    [SXML_XSLT_AST_ATTRIBUTE_NAMESPACE_CONTEXT] = "NAMESPACE_CONTEXT",
    [SXML_XSLT_AST_ATTRIBUTE_NAN] = "NAN",
    [SXML_XSLT_AST_ATTRIBUTE_NEW_EACH_TIME] = "NEW_EACH_TIME",
    [SXML_XSLT_AST_ATTRIBUTE_NORMALIZATION_FORM] = "NORMALIZATION_FORM",
    [SXML_XSLT_AST_ATTRIBUTE_OMIT_XML_DECLARATION] = "OMIT_XML_DECLARATION",
    [SXML_XSLT_AST_ATTRIBUTE_ON_MULTIPLE_MATCH] = "ON_MULTIPLE_MATCH",
    [SXML_XSLT_AST_ATTRIBUTE_ON_NO_MATCH] = "ON_NO_MATCH",
    [SXML_XSLT_AST_ATTRIBUTE_ORDER] = "ORDER",
    [SXML_XSLT_AST_ATTRIBUTE_ORDINAL] = "ORDINAL",
    [SXML_XSLT_AST_ATTRIBUTE_OUTPUT_VERSION] = "OUTPUT_VERSION",
    [SXML_XSLT_AST_ATTRIBUTE_OVERRIDE] = "OVERRIDE",
    [SXML_XSLT_AST_ATTRIBUTE_OVERRIDE_EXTENSION_FUNCTION] = "OVERRIDE_EXTENSION_FUNCTION",
    [SXML_XSLT_AST_ATTRIBUTE_PACKAGE_VERSION] = "PACKAGE_VERSION",
    [SXML_XSLT_AST_ATTRIBUTE_PARAMETER_DOCUMENT] = "PARAMETER_DOCUMENT",
    [SXML_XSLT_AST_ATTRIBUTE_PATH_IN_ARCHIVE] = "PATH_IN_ARCHIVE",
    [SXML_XSLT_AST_ATTRIBUTE_PATTERN_SEPARATOR] = "PATTERN_SEPARATOR",
    [SXML_XSLT_AST_ATTRIBUTE_PERCENT] = "PERCENT",
    [SXML_XSLT_AST_ATTRIBUTE_PER_MILLE] = "PER_MILLE",
    [SXML_XSLT_AST_ATTRIBUTE_PHASE] = "PHASE",
    [SXML_XSLT_AST_ATTRIBUTE_PRIORITY] = "PRIORITY",
    [SXML_XSLT_AST_ATTRIBUTE_REGEX] = "REGEX",
    [SXML_XSLT_AST_ATTRIBUTE_REQUIRED] = "REQUIRED",
    [SXML_XSLT_AST_ATTRIBUTE_RESULT_PREFIX] = "RESULT_PREFIX",
    [SXML_XSLT_AST_ATTRIBUTE_ROLE] = "ROLE",
    [SXML_XSLT_AST_ATTRIBUTE_ROLLBACK_OUTPUT] = "ROLLBACK_OUTPUT",
    [SXML_XSLT_AST_ATTRIBUTE_SCHEMA_AWARE] = "SCHEMA_AWARE",
    [SXML_XSLT_AST_ATTRIBUTE_SCHEMA_LOCATION] = "SCHEMA_LOCATION",
    [SXML_XSLT_AST_ATTRIBUTE_SELECT] = "SELECT",
    [SXML_XSLT_AST_ATTRIBUTE_SEPARATOR] = "SEPARATOR",
    [SXML_XSLT_AST_ATTRIBUTE_SORT_BEFORE_MERGE] = "SORT_BEFORE_MERGE",
    [SXML_XSLT_AST_ATTRIBUTE_SPLIT_WHEN] = "SPLIT_WHEN",
    [SXML_XSLT_AST_ATTRIBUTE_STABLE] = "STABLE",
    [SXML_XSLT_AST_ATTRIBUTE_STANDALONE] = "STANDALONE",
    [SXML_XSLT_AST_ATTRIBUTE_START_AT] = "START_AT",
    [SXML_XSLT_AST_ATTRIBUTE_STATIC] = "STATIC",
    [SXML_XSLT_AST_ATTRIBUTE_STREAMABILITY] = "STREAMABILITY",
    [SXML_XSLT_AST_ATTRIBUTE_STREAMABLE] = "STREAMABLE",
    [SXML_XSLT_AST_ATTRIBUTE_STRING] = "STRING",
    [SXML_XSLT_AST_ATTRIBUTE_STYLESHEET_PREFIX] = "STYLESHEET_PREFIX",
    [SXML_XSLT_AST_ATTRIBUTE_SUPPRESS_INDENTATION] = "SUPPRESS_INDENTATION",
    [SXML_XSLT_AST_ATTRIBUTE_TERMINATE] = "TERMINATE",
    [SXML_XSLT_AST_ATTRIBUTE_TEST] = "TEST",
    [SXML_XSLT_AST_ATTRIBUTE_THEN] = "THEN",
    [SXML_XSLT_AST_ATTRIBUTE_TRUSTED] = "TRUSTED",
    [SXML_XSLT_AST_ATTRIBUTE_TUNNEL] = "TUNNEL",
    [SXML_XSLT_AST_ATTRIBUTE_TYPE] = "TYPE",
    [SXML_XSLT_AST_ATTRIBUTE_TYPED] = "TYPED",
    [SXML_XSLT_AST_ATTRIBUTE_UNDECLARE_PREFIXES] = "UNDECLARE_PREFIXES",
    [SXML_XSLT_AST_ATTRIBUTE_USE] = "USE",
    [SXML_XSLT_AST_ATTRIBUTE_USE_ACCUMULATORS] = "USE_ACCUMULATORS",
    [SXML_XSLT_AST_ATTRIBUTE_USE_ATTRIBUTE_SETS] = "USE_ATTRIBUTE_SETS",
    [SXML_XSLT_AST_ATTRIBUTE_USE_CHARACTER_MAPS] = "USE_CHARACTER_MAPS",
    [SXML_XSLT_AST_ATTRIBUTE_VALIDATION] = "VALIDATION",
    [SXML_XSLT_AST_ATTRIBUTE_VALUE] = "VALUE",
    [SXML_XSLT_AST_ATTRIBUTE_VISIBILITY] = "VISIBILITY",
    [SXML_XSLT_AST_ATTRIBUTE_WARNING_ON_MULTIPLE_MATCH] = "WARNING_ON_MULTIPLE_MATCH",
    [SXML_XSLT_AST_ATTRIBUTE_WARNING_ON_NO_MATCH] = "WARNING_ON_NO_MATCH",
    [SXML_XSLT_AST_ATTRIBUTE_WITH_PARAMS] = "WITH_PARAMS",
    [SXML_XSLT_AST_ATTRIBUTE_XPATH] = "XPATH",
    [SXML_XSLT_AST_ATTRIBUTE_ZERO_DIGIT] = "ZERO_DIGIT",
    [SXML_XSLT_AST_ATTRIBUTE_CONSTRUCTOR] = "CONSTRUCTOR",
    [SXML_XSLT_AST_ATTRIBUTE_RECORD_FIELD] = "RECORD_FIELD",
};
_Static_assert(sizeof(attribute_kind_names) / sizeof(attribute_kind_names[0]) ==
                   SXML_XSLT_AST_ATTRIBUTE_KIND_COUNT,
               "one name per attribute kind");

static const char *const keyword_names[] = {
    [SXML_XSLT_AST_KEYWORD_NONE] = "NONE",
    [SXML_XSLT_AST_KEYWORD_DEEP_COPY] = "DEEP_COPY",
    [SXML_XSLT_AST_KEYWORD_SHALLOW_COPY] = "SHALLOW_COPY",
    [SXML_XSLT_AST_KEYWORD_SHALLOW_COPY_ALL] = "SHALLOW_COPY_ALL",
    [SXML_XSLT_AST_KEYWORD_DEEP_SKIP] = "DEEP_SKIP",
    [SXML_XSLT_AST_KEYWORD_SHALLOW_SKIP] = "SHALLOW_SKIP",
    [SXML_XSLT_AST_KEYWORD_TEXT_ONLY_COPY] = "TEXT_ONLY_COPY",
    [SXML_XSLT_AST_KEYWORD_FAIL] = "FAIL",
    [SXML_XSLT_AST_KEYWORD_USE_LAST] = "USE_LAST",
    [SXML_XSLT_AST_KEYWORD_PUBLIC] = "PUBLIC",
    [SXML_XSLT_AST_KEYWORD_PRIVATE] = "PRIVATE",
    [SXML_XSLT_AST_KEYWORD_FINAL] = "FINAL",
    [SXML_XSLT_AST_KEYWORD_ABSTRACT] = "ABSTRACT",
    [SXML_XSLT_AST_KEYWORD_HIDDEN] = "HIDDEN",
    [SXML_XSLT_AST_KEYWORD_STRICT] = "STRICT",
    [SXML_XSLT_AST_KEYWORD_LAX] = "LAX",
    [SXML_XSLT_AST_KEYWORD_PRESERVE] = "PRESERVE",
    [SXML_XSLT_AST_KEYWORD_STRIP] = "STRIP",
    [SXML_XSLT_AST_KEYWORD_UNSPECIFIED] = "UNSPECIFIED",
    [SXML_XSLT_AST_KEYWORD_YES] = "YES",
    [SXML_XSLT_AST_KEYWORD_NO] = "NO",
    [SXML_XSLT_AST_KEYWORD_MAYBE] = "MAYBE",
    [SXML_XSLT_AST_KEYWORD_OMIT] = "OMIT",
    [SXML_XSLT_AST_KEYWORD_XML] = "XML",
    [SXML_XSLT_AST_KEYWORD_XHTML] = "XHTML",
    [SXML_XSLT_AST_KEYWORD_HTML] = "HTML",
    [SXML_XSLT_AST_KEYWORD_TEXT] = "TEXT",
    [SXML_XSLT_AST_KEYWORD_JSON] = "JSON",
    [SXML_XSLT_AST_KEYWORD_CSV] = "CSV",
    [SXML_XSLT_AST_KEYWORD_ADAPTIVE] = "ADAPTIVE",
    [SXML_XSLT_AST_KEYWORD_SINGLE] = "SINGLE",
    [SXML_XSLT_AST_KEYWORD_MULTIPLE] = "MULTIPLE",
    [SXML_XSLT_AST_KEYWORD_ANY] = "ANY",
    [SXML_XSLT_AST_KEYWORD_REQUIRED] = "REQUIRED",
    [SXML_XSLT_AST_KEYWORD_OPTIONAL] = "OPTIONAL",
    [SXML_XSLT_AST_KEYWORD_ABSENT] = "ABSENT",
    [SXML_XSLT_AST_KEYWORD_START] = "START",
    [SXML_XSLT_AST_KEYWORD_END] = "END",
    [SXML_XSLT_AST_KEYWORD_NFC] = "NFC",
    [SXML_XSLT_AST_KEYWORD_NFD] = "NFD",
    [SXML_XSLT_AST_KEYWORD_NFKC] = "NFKC",
    [SXML_XSLT_AST_KEYWORD_NFKD] = "NFKD",
    [SXML_XSLT_AST_KEYWORD_FULLY_NORMALIZED] = "FULLY_NORMALIZED",
    [SXML_XSLT_AST_KEYWORD_NO_NORMALIZATION] = "NO_NORMALIZATION",
    [SXML_XSLT_AST_KEYWORD_UNCLASSIFIED] = "UNCLASSIFIED",
    [SXML_XSLT_AST_KEYWORD_ABSORBING] = "ABSORBING",
    [SXML_XSLT_AST_KEYWORD_INSPECTION] = "INSPECTION",
    [SXML_XSLT_AST_KEYWORD_FILTER] = "FILTER",
    [SXML_XSLT_AST_KEYWORD_SHALLOW_DESCENT] = "SHALLOW_DESCENT",
    [SXML_XSLT_AST_KEYWORD_DEEP_DESCENT] = "DEEP_DESCENT",
    [SXML_XSLT_AST_KEYWORD_ASCENT] = "ASCENT",
    [SXML_XSLT_AST_KEYWORD_TEMPLATE] = "TEMPLATE",
    [SXML_XSLT_AST_KEYWORD_FUNCTION] = "FUNCTION",
    [SXML_XSLT_AST_KEYWORD_VARIABLE] = "VARIABLE",
    [SXML_XSLT_AST_KEYWORD_ATTRIBUTE_SET] = "ATTRIBUTE_SET",
    [SXML_XSLT_AST_KEYWORD_MODE] = "MODE",
    [SXML_XSLT_AST_KEYWORD_ITEM_TYPE] = "ITEM_TYPE",
    [SXML_XSLT_AST_KEYWORD_RECORD_TYPE] = "RECORD_TYPE",
    [SXML_XSLT_AST_KEYWORD_ANY_COMPONENT] = "ANY_COMPONENT",
};

typedef struct text_writer {
    sxml_text_form_writer form;
    const sxml_xslt_ast *ast;
    sxml_name_pool *name_pool;
} text_writer;

static void write_bytes(text_writer *writer, sxml_byte_view bytes) {
    sxml_text_form_write_bytes(&writer->form, bytes);
}

// ast-grep-ignore: pointer-parameter-without-length
static void write_text(text_writer *writer, const char *text) {
    sxml_text_form_write_text(&writer->form, text);
}

static void write_unsigned(text_writer *writer, uint64_t number) {
    sxml_text_form_write_unsigned(&writer->form, number);
}

/* A number as a decimal: the integer part, then up to six fraction digits
   without trailing zeros; with_fraction keeps ".0" for an integer (a
   version is written 2.0). Precise enough for priorities and versions. */
typedef struct number_text {
    double number;
    bool keeps_fraction;
} number_text;

static void write_number(text_writer *writer, number_text request) {
    double number = request.number;
    if (number != number) {
        write_text(writer, "NaN");
        return;
    }
    if (number < 0) {
        write_text(writer, "-");
        number = -number;
    }
    if (number >= 1e18) {
        write_text(writer, "INF");
        return;
    }
    uint64_t scaled = (uint64_t)(number * 1e6 + 0.5);
    const uint64_t integer_part = scaled / 1000000U;
    uint64_t fraction = scaled % 1000000U;
    write_unsigned(writer, integer_part);
    if (fraction == 0) {
        if (request.keeps_fraction) {
            write_text(writer, ".0");
        }
        return;
    }
    uint8_t digits[6];
    size_t digit_count = 6;
    for (size_t digit_index = 6; digit_index > 0; digit_index -= 1) {
        digits[digit_index - 1] = (uint8_t)('0' + (uint8_t)(fraction % 10U));
        fraction /= 10U;
    }
    while (digit_count > 1 && digits[digit_count - 1] == '0') {
        digit_count -= 1;
    }
    write_text(writer, ".");
    write_bytes(writer, (sxml_byte_view){.bytes = digits, .byte_count = digit_count});
}

static void write_newline(text_writer *writer, size_t indent) {
    write_text(writer, "\n");
    for (size_t space_index = 0; space_index < indent; space_index += 1) {
        write_text(writer, " ");
    }
}

/* Text in double quotes, with quotes, backslashes and line ends escaped
   so that each entry stays on one line. */
static void write_quoted(text_writer *writer, sxml_byte_view text) {
    write_text(writer, "\"");
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        if (byte == '"' || byte == '\\') {
            write_text(writer, "\\");
        } else if (byte == '\n') {
            write_text(writer, "\\n");
            continue;
        } else if (byte == '\t') {
            write_text(writer, "\\t");
            continue;
        } else if (byte == '\r') {
            write_text(writer, "\\r");
            continue;
        }
        write_bytes(writer, (sxml_byte_view){.bytes = text.bytes + byte_index, .byte_count = 1});
    }
    write_text(writer, "\"");
}

static void write_namespace_uri(text_writer *writer, sxml_namespace_uri_identifier uri) {
    sxml_byte_view uri_text = {.bytes = NULL, .byte_count = 0};
    if (sxml_name_pool_get_namespace_uri(writer->name_pool, uri, &uri_text) != SXML_STATUS_OK) {
        uri_text = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    write_bytes(writer, uri_text);
}

/* An expanded name: the local name in no namespace, Q{uri}local
   otherwise. */
static void write_name(text_writer *writer, sxml_name_identifier name) {
    sxml_name_parts parts = {.namespace_uri = {.index = 0}, .local_name = {.bytes = NULL,
                                                                           .byte_count = 0}};
    if (sxml_name_pool_get_name_parts(writer->name_pool, name, &parts) != SXML_STATUS_OK) {
        write_text(writer, "Q{}?");
        return;
    }
    if (parts.namespace_uri.index != SXML_NAMESPACE_NONE) {
        write_text(writer, "Q{");
        write_namespace_uri(writer, parts.namespace_uri);
        write_text(writer, "}");
    }
    write_bytes(writer, parts.local_name);
}

static void write_prefix(text_writer *writer, sxml_namespace_prefix_identifier prefix) {
    sxml_byte_view prefix_text = {.bytes = NULL, .byte_count = 0};
    if (prefix.index == 0 ||
        sxml_name_pool_get_namespace_prefix(writer->name_pool, prefix, &prefix_text) !=
            SXML_STATUS_OK) {
        write_text(writer, "#default");
        return;
    }
    write_bytes(writer, prefix_text);
}

static void write_location(text_writer *writer, sxml_ast_source_span span) {
    if (span.line_number == 0) {
        return;
    }
    write_text(writer, " at ");
    write_unsigned(writer, span.line_number);
    write_text(writer, ":");
    write_unsigned(writer, span.column_number);
}

static void write_name_token(text_writer *writer, const sxml_xslt_ast_name_token *token) {
    switch (token->kind) {
    case SXML_XSLT_AST_NAME_TOKEN_KIND_NONE:
        write_text(writer, "?");
        break;
    case SXML_XSLT_AST_NAME_TOKEN_KIND_NAME:
        write_name(writer, token->name);
        break;
    case SXML_XSLT_AST_NAME_TOKEN_KIND_ANY_NAME:
        write_text(writer, "*");
        break;
    case SXML_XSLT_AST_NAME_TOKEN_KIND_NAMESPACE_WILDCARD:
        write_text(writer, "Q{");
        write_namespace_uri(writer, token->namespace_uri);
        write_text(writer, "}*");
        break;
    case SXML_XSLT_AST_NAME_TOKEN_KIND_LOCAL_NAME_WILDCARD:
        write_text(writer, "*:");
        write_name(writer, token->name);
        break;
    case SXML_XSLT_AST_NAME_TOKEN_KIND_DEFAULT:
        write_text(writer, "#default");
        break;
    case SXML_XSLT_AST_NAME_TOKEN_KIND_UNNAMED:
        write_text(writer, "#unnamed");
        break;
    case SXML_XSLT_AST_NAME_TOKEN_KIND_CURRENT:
        write_text(writer, "#current");
        break;
    case SXML_XSLT_AST_NAME_TOKEN_KIND_ALL:
        write_text(writer, "#all");
        break;
    }
    if (token->has_arity) {
        write_text(writer, "#");
        write_unsigned(writer, token->arity);
    }
}

static const char *expression_kind_word(sxml_xslt_ast_value_kind kind) {
    switch (kind) {
    case SXML_XSLT_AST_VALUE_KIND_EXPRESSION:
        return "expression";
    case SXML_XSLT_AST_VALUE_KIND_PATTERN:
        return "pattern";
    case SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE:
        return "value-template";
    case SXML_XSLT_AST_VALUE_KIND_SEQUENCE_TYPE:
        return "sequence-type";
    case SXML_XSLT_AST_VALUE_KIND_ITEM_TYPE:
        return "item-type";
    case SXML_XSLT_AST_VALUE_KIND_NONE:
    case SXML_XSLT_AST_VALUE_KIND_TEXT:
    case SXML_XSLT_AST_VALUE_KIND_BOOLEAN:
    case SXML_XSLT_AST_VALUE_KIND_NUMBER:
    case SXML_XSLT_AST_VALUE_KIND_KEYWORD:
    case SXML_XSLT_AST_VALUE_KIND_NAME:
    case SXML_XSLT_AST_VALUE_KIND_NAME_LIST:
    case SXML_XSLT_AST_VALUE_KIND_NAMESPACE_BINDING:
        break;
    }
    return "value";
}

/* Where an expression's text is in the module's tree: the attribute of
   the source element with the attribute's name, or the source text node
   (the first text child of an element, for xsl:select). */
typedef struct expression_source {
    const sxml_xslt_ast_node *node;
    /* 0 for the content of a node. */
    sxml_name_identifier attribute_name;
} expression_source;

static sxml_byte_view find_expression_text(const text_writer *writer, expression_source source) {
    const sxml_byte_view none = {.bytes = NULL, .byte_count = 0};
    const sxml_xslt_ast_module *module = sxml_xslt_ast_get_module(writer->ast, source.node->module);
    if (module->tree == NULL || (source.node->flags & SXML_XSLT_AST_NODE_FLAG_SYNTHESIZED) != 0) {
        return none;
    }
    const sxml_node_reference element = {
        .tree = module->tree, .node_index = source.node->source_node, .namespace_number = 0};
    sxml_node_description description;
    memset(&description, 0, sizeof(description));
    if (sxml_node_describe(element, &description) != SXML_STATUS_OK) {
        return none;
    }
    sxml_byte_view content = none;
    if (description.node_kind == SXML_NODE_KIND_TEXT) {
        return sxml_node_get_content(element, &content) == SXML_STATUS_OK ? content : none;
    }
    if (source.attribute_name.index == 0) {
        if (!description.has_children) {
            return none;
        }
        const sxml_node_reference child = {
            .tree = module->tree, .node_index = description.first_child, .namespace_number = 0};
        return sxml_node_get_content(child, &content) == SXML_STATUS_OK ? content : none;
    }
    for (uint32_t offset = 1; offset <= description.attribute_count; offset += 1) {
        const sxml_node_reference attribute = {
            .tree = module->tree,
            .node_index = {.index = source.node->source_node.index + offset},
            .namespace_number = 0};
        sxml_node_description attribute_description;
        memset(&attribute_description, 0, sizeof(attribute_description));
        if (sxml_node_describe(attribute, &attribute_description) == SXML_STATUS_OK &&
            attribute_description.node_name.index == source.attribute_name.index &&
            sxml_node_get_content(attribute, &content) == SXML_STATUS_OK) {
            return content;
        }
    }
    return none;
}

typedef struct value_text_request {
    const sxml_xslt_ast_value *value;
    expression_source source;
} value_text_request;

static void write_value(text_writer *writer, value_text_request request) {
    const sxml_xslt_ast_value *value = request.value;
    const sxml_xslt_ast *ast = writer->ast;
    switch (value->kind) {
    case SXML_XSLT_AST_VALUE_KIND_NONE:
        write_text(writer, "none");
        return;
    case SXML_XSLT_AST_VALUE_KIND_EXPRESSION:
    case SXML_XSLT_AST_VALUE_KIND_PATTERN:
    case SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE:
    case SXML_XSLT_AST_VALUE_KIND_SEQUENCE_TYPE:
    case SXML_XSLT_AST_VALUE_KIND_ITEM_TYPE: {
        write_text(writer, expression_kind_word(value->kind));
        write_text(writer, " ");
        write_unsigned(writer, value->payload.expression.value);
        const sxml_byte_view text = find_expression_text(writer, request.source);
        if (text.bytes != NULL) {
            write_text(writer, " ");
            write_quoted(writer, text);
        }
        return;
    }
    case SXML_XSLT_AST_VALUE_KIND_TEXT:
        write_text(writer, "text ");
        write_quoted(writer, sxml_xslt_ast_get_text(ast, value->payload.text));
        return;
    case SXML_XSLT_AST_VALUE_KIND_BOOLEAN:
        write_text(writer, value->payload.is_true ? "boolean yes" : "boolean no");
        return;
    case SXML_XSLT_AST_VALUE_KIND_NUMBER:
        write_text(writer, "number ");
        write_number(writer,
                     (number_text){.number = value->payload.number, .keeps_fraction = false});
        return;
    case SXML_XSLT_AST_VALUE_KIND_KEYWORD:
        write_text(writer, "keyword ");
        write_text(writer, value->payload.keyword < sizeof(keyword_names) / sizeof(keyword_names[0])
                               ? keyword_names[value->payload.keyword]
                               : "?");
        return;
    case SXML_XSLT_AST_VALUE_KIND_NAME:
        write_text(writer, "name ");
        write_name(writer, value->payload.name);
        return;
    case SXML_XSLT_AST_VALUE_KIND_NAME_LIST: {
        write_text(writer, "names (");
        const sxml_xslt_ast_name_token_view tokens =
            sxml_xslt_ast_get_name_tokens(ast, value->payload.name_tokens);
        for (size_t token_index = 0; token_index < tokens.name_token_count; token_index += 1) {
            if (token_index > 0) {
                write_text(writer, ", ");
            }
            write_name_token(writer, &tokens.name_tokens[token_index]);
        }
        write_text(writer, ")");
        return;
    }
    case SXML_XSLT_AST_VALUE_KIND_NAMESPACE_BINDING:
        write_text(writer, "binding ");
        write_prefix(writer, value->payload.namespace_binding.namespace_prefix);
        write_text(writer, " = ");
        write_namespace_uri(writer, value->payload.namespace_binding.namespace_uri);
        return;
    }
}

static void write_namespace_uri_list(text_writer *writer, sxml_xslt_ast_namespace_uri_list list) {
    const sxml_xslt_ast_namespace_uri_view uris = sxml_xslt_ast_get_namespace_uris(writer->ast,
                                                                                  list);
    write_text(writer, "(");
    for (size_t uri_index = 0; uri_index < uris.namespace_uri_count; uri_index += 1) {
        if (uri_index > 0) {
            write_text(writer, ", ");
        }
        write_namespace_uri(writer, uris.namespace_uris[uri_index]);
    }
    write_text(writer, ")");
}

static void write_module(text_writer *writer, uint32_t module_number) {
    const sxml_xslt_ast_module *module =
        sxml_xslt_ast_get_module(writer->ast, (sxml_xslt_ast_module_index){.value = module_number});
    write_text(writer, "module ");
    write_unsigned(writer, module_number);
    switch (module->relation) {
    case SXML_XSLT_AST_MODULE_RELATION_NONE:
        write_text(writer, " none");
        break;
    case SXML_XSLT_AST_MODULE_RELATION_PRINCIPAL:
        write_text(writer, " principal");
        break;
    case SXML_XSLT_AST_MODULE_RELATION_INCLUDED:
        write_text(writer, " included");
        break;
    case SXML_XSLT_AST_MODULE_RELATION_IMPORTED:
        write_text(writer, " imported");
        break;
    case SXML_XSLT_AST_MODULE_RELATION_USED_PACKAGE:
        write_text(writer, " used-package");
        break;
    }
    if (module->parent_module.value != 0) {
        write_text(writer, " parent ");
        write_unsigned(writer, module->parent_module.value);
        write_text(writer, " by node ");
        write_unsigned(writer, module->referencing_node.value);
    }
    if (module->is_simplified) {
        write_text(writer, " simplified");
    }
    write_text(writer, " precedence ");
    write_unsigned(writer, module->import_precedence);
    write_text(writer, " root ");
    write_unsigned(writer, module->root.value);
    write_newline(writer, 2);
    write_text(writer, "uri ");
    write_quoted(writer, module->module_uri);
    write_newline(writer, 0);
}

static void write_static_context(text_writer *writer, uint32_t context_number) {
    const sxml_xslt_ast *ast = writer->ast;
    const sxml_xslt_ast_static_context_index index = {.value = context_number};
    const sxml_xslt_ast_static_context *context = sxml_xslt_ast_get_static_context(ast, index);
    write_text(writer, "context ");
    write_unsigned(writer, context_number);
    write_text(writer, " parent ");
    write_unsigned(writer, context->parent.value);
    write_text(writer, " version ");
    write_number(writer, (number_text){.number = context->version, .keeps_fraction = true});
    write_text(writer, context->expands_text ? " expand-text yes" : " expand-text no");
    write_newline(writer, 2);
    write_text(writer, "namespaces (");
    const sxml_namespace_binding_view bindings = sxml_xslt_ast_get_namespace_bindings(ast, index);
    for (size_t binding_index = 0; binding_index < bindings.namespace_binding_count;
         binding_index += 1) {
        if (binding_index > 0) {
            write_text(writer, ", ");
        }
        write_prefix(writer, bindings.namespace_bindings[binding_index].namespace_prefix);
        write_text(writer, " = ");
        write_namespace_uri(writer, bindings.namespace_bindings[binding_index].namespace_uri);
    }
    write_text(writer, ")");
    write_newline(writer, 2);
    write_text(writer, "xpath-default-namespace ");
    if (context->is_any_namespace) {
        write_text(writer, "##any");
    } else if (context->xpath_default_namespace.index == SXML_NAMESPACE_NONE) {
        write_text(writer, "none");
    } else {
        write_namespace_uri(writer, context->xpath_default_namespace);
    }
    write_text(writer, " default-collation ");
    const sxml_byte_view collation = sxml_xslt_ast_get_text(ast, context->default_collation);
    if (collation.byte_count == 0) {
        write_text(writer, "codepoint");
    } else {
        write_bytes(writer, collation);
    }
    write_newline(writer, 2);
    write_text(writer, "default-mode ");
    write_name_token(writer, &context->default_mode);
    write_text(writer, context->default_validation == SXML_XSLT_AST_KEYWORD_PRESERVE
                           ? " default-validation preserve"
                           : " default-validation strip");
    write_newline(writer, 2);
    write_text(writer, "excluded ");
    write_namespace_uri_list(writer, context->excluded_namespaces);
    write_text(writer, " extension ");
    write_namespace_uri_list(writer, context->extension_namespaces);
    write_newline(writer, 2);
    write_text(writer, "base-uri ");
    write_quoted(writer, sxml_xslt_ast_get_text(ast, context->static_base_uri));
    write_newline(writer, 0);
}

static void write_attribute(text_writer *writer, uint32_t attribute_number, size_t indent) {
    const sxml_xslt_ast_attribute *attribute = sxml_xslt_ast_get_attribute(
        writer->ast, (sxml_xslt_ast_attribute_index){.value = attribute_number});
    write_newline(writer, indent);
    write_text(writer, "attribute ");
    write_text(writer, attribute->kind < sizeof(attribute_kind_names) /
                                             sizeof(attribute_kind_names[0])
                           ? attribute_kind_names[attribute->kind]
                           : "?");
    if (attribute->kind == SXML_XSLT_AST_ATTRIBUTE_LITERAL_RESULT ||
        attribute->kind == SXML_XSLT_AST_ATTRIBUTE_FOREIGN) {
        write_text(writer, " ");
        write_name(writer, attribute->attribute_name);
    }
    if ((attribute->flags & SXML_XSLT_AST_ATTRIBUTE_FLAG_XSLT_NAMESPACE) != 0) {
        write_text(writer, " xslt-namespace");
    }
    if ((attribute->flags & SXML_XSLT_AST_ATTRIBUTE_FLAG_SHADOW) != 0) {
        write_text(writer, " shadow");
    }
    if ((attribute->flags & SXML_XSLT_AST_ATTRIBUTE_FLAG_SYNTHESIZED) != 0) {
        write_text(writer, " synthesized");
    }
    write_text(writer, " ");
    const sxml_xslt_ast_node *node = sxml_xslt_ast_get_node(writer->ast, attribute->node);
    /* A shadow attribute's text is what its value template gave, which the
       tree does not hold. */
    const bool is_shadow = (attribute->flags & SXML_XSLT_AST_ATTRIBUTE_FLAG_SHADOW) != 0;
    write_value(writer, (value_text_request){
                            .value = &attribute->value,
                            .source = {.node = is_shadow ? &writer->ast->no_node : node,
                                       .attribute_name = attribute->attribute_name}});
    write_location(writer, attribute->source_span);
}

/* The node line, its attributes, then its children in document order,
   by a walk over the parent and sibling links (no stack: the AST is a
   tree linked both ways). */
static void write_module_tree(text_writer *writer, sxml_xslt_ast_node_index root) {
    const sxml_xslt_ast *ast = writer->ast;
    uint32_t current = root.value;
    size_t depth = 0;
    while (current != 0) {
        const sxml_xslt_ast_node *node =
            sxml_xslt_ast_get_node(ast, (sxml_xslt_ast_node_index){.value = current});
        const size_t indent = depth * 2U;
        write_text(writer, "node ");
        write_unsigned(writer, current);
        write_text(writer, " ");
        write_text(writer, node->kind < SXML_XSLT_AST_KIND_COUNT ? kind_names[node->kind] : "?");
        if (node->element_name.index != 0) {
            write_text(writer, " ");
            write_name(writer, node->element_name);
        }
        if (current == root.value) {
            write_text(writer, " module ");
            write_unsigned(writer, node->module.value);
        }
        if ((node->flags & SXML_XSLT_AST_NODE_FLAG_SYNTHESIZED) != 0) {
            write_text(writer, " synthesized");
        }
        if (node->declaration_order != 0) {
            write_text(writer, " declaration ");
            write_unsigned(writer, node->declaration_order);
        }
        if (node->kind == SXML_XSLT_AST_KIND_IMPORT || node->kind == SXML_XSLT_AST_KIND_INCLUDE ||
            node->kind == SXML_XSLT_AST_KIND_USE_PACKAGE) {
            write_text(writer, node->kind == SXML_XSLT_AST_KIND_IMPORT    ? " imports module "
                               : node->kind == SXML_XSLT_AST_KIND_INCLUDE ? " includes module "
                                                                          : " uses module ");
            write_unsigned(writer, node->included_module.value);
        }
        const sxml_xslt_ast_node *parent = sxml_xslt_ast_get_node(ast, node->parent);
        if (current == root.value || parent->static_context.value != node->static_context.value) {
            write_text(writer, " context ");
            write_unsigned(writer, node->static_context.value);
        }
        if (node->content.kind != SXML_XSLT_AST_VALUE_KIND_NONE) {
            write_text(writer, " ");
            write_value(writer,
                        (value_text_request){.value = &node->content,
                                             .source = {.node = node,
                                                        .attribute_name = {.index = 0}}});
        }
        write_location(writer, node->source_span);
        for (uint32_t offset = 0; offset < node->attributes.attribute_count; offset += 1) {
            write_attribute(writer, node->attributes.first_attribute_index + offset, indent + 2U);
        }
        write_newline(writer, 0);
        /* Next in document order: the first child, else the next sibling
           of the nearest node on the way up that has one. */
        if (node->first_child.value != 0) {
            current = node->first_child.value;
            depth += 1;
        } else {
            uint32_t climbing = current;
            current = 0;
            while (climbing != 0 && climbing != root.value) {
                const sxml_xslt_ast_node *climbing_node =
                    sxml_xslt_ast_get_node(ast, (sxml_xslt_ast_node_index){.value = climbing});
                if (climbing_node->next_sibling.value != 0) {
                    current = climbing_node->next_sibling.value;
                    break;
                }
                climbing = climbing_node->parent.value;
                depth -= 1;
            }
        }
        if (current != 0) {
            for (size_t space_index = 0; space_index < depth * 2U; space_index += 1) {
                write_text(writer, " ");
            }
        }
    }
}

sxml_status sxml_xslt_ast_write_text(const sxml_xslt_ast *ast, sxml_memory_view output,
                                     size_t *byte_count) {
    if (ast == NULL || byte_count == NULL || (output.byte_count > 0 && output.bytes == NULL)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    text_writer writer = {.form = {.output = output, .byte_count = 0, .column = 0},
                          .ast = ast,
                          .name_pool = sxml_item_heap_get_name_pool(ast->item_heap)};
    for (uint32_t module = 1; module < ast->module_count; module += 1) {
        write_module(&writer, module);
    }
    for (uint32_t context = 1; context < ast->static_context_count; context += 1) {
        write_static_context(&writer, context);
    }
    write_text(&writer, "declarations (");
    for (uint32_t order = 1; order < ast->declaration_count; order += 1) {
        if (order > 1) {
            write_text(&writer, ", ");
        }
        write_text(&writer, "node ");
        write_unsigned(&writer, ast->declarations[order].value);
    }
    write_text(&writer, ")");
    write_newline(&writer, 0);
    for (uint32_t module = 1; module < ast->module_count; module += 1) {
        const sxml_xslt_ast_node_index root = ast->modules[module].root;
        if (root.value != 0) {
            write_module_tree(&writer, root);
        }
    }
    *byte_count = writer.form.byte_count;
    return writer.form.byte_count > output.byte_count ? SXML_STATUS_CAPACITY_EXCEEDED : SXML_STATUS_OK;
}
