/*
 * The module set of an XQuery compilation: library modules loaded through
 * the resource resolver (src/xquery_module_internal.h; D106, D134, D148;
 * packet 3.11).
 *
 * Golden file: src/xquery_static.c (prolog walks, errors through
 * sxml_xpath_report_static_error, arrays from the allocator).
 *
 * Loading is a work list over the set itself: module n is scanned after
 * modules 0 to n - 1, and every module its imports deliver is appended, so
 * no function calls itself (D71) and a module URI met again ends a cycle.
 *
 * Section numbers are of XQuery 4.0 (W3C QT4 CG draft, 13 October 2026)
 * unless another specification is named.
 */
#include "xquery_module_internal.h"

#include "processor_internal.h"
#include "xdm_internal.h"
#include "xpath_static_internal.h"
#include "xquery_static_internal.h"

#include <stdalign.h>
#include <string.h>

/* ----------------------------------------------------------- AST access */

static const sxml_ast_node *node_of(const sxml_ast *ast, sxml_ast_node_index node) {
    return sxml_ast_get_node(ast, node);
}

static sxml_ast_node_index child_of(const sxml_ast *ast, sxml_ast_node_index parent,
                                    sxml_ast_field field) {
    return sxml_ast_find_child(ast, (sxml_ast_child_query){.parent = parent, .field = field});
}

static sxml_ast_node_index next_of(const sxml_ast *ast, sxml_ast_node_index node) {
    return sxml_ast_find_next_with_same_field(ast, node);
}

static sxml_byte_view text_of(const sxml_ast *ast, sxml_ast_node_index node) {
    return sxml_ast_get_text(ast, node_of(ast, node)->payload.text);
}

/* The MAIN_MODULE or LIBRARY_MODULE of a MODULE. */
static sxml_ast_node_index module_of(const sxml_ast *ast) {
    return child_of(ast, ast->root, SXML_AST_FIELD_MODULE);
}

static sxml_ast_node_index first_declaration(const sxml_ast *ast) {
    const sxml_ast_node_index prolog = child_of(ast, module_of(ast), SXML_AST_FIELD_PROLOG);
    if (prolog.value == 0) {
        return prolog;
    }
    return child_of(ast, prolog, SXML_AST_FIELD_DECLARATION);
}

/* ------------------------------------------------------------- Loading */

/* What loading works with. */
typedef struct module_loader {
    const sxml_xquery_module_loading *request;
    sxml_name_pool *name_pool;
    sxml_xquery_module_set *set;
} module_loader;

/* code_and_description is "CODE description" (an eight-character code, a
   space, static text). */
typedef struct module_error {
    const sxml_ast *ast;
    sxml_ast_node_index node;
    sxml_byte_view code_and_description;
} module_error;

static sxml_status report(const module_loader *loader, module_error error) {
    return sxml_xpath_report_static_error(
        &(sxml_xpath_static_error){
            .ast = error.ast,
            .node = error.node,
            .code_local_name = {.bytes = error.code_and_description.bytes, .byte_count = 8},
            .description = {.bytes = error.code_and_description.bytes + 9,
                            .byte_count = error.code_and_description.byte_count - 9}},
        loader->request->error_record);
}

/* An array of the set that grows to hold one more element. */
typedef struct array_growth {
    void **elements;
    size_t *capacity;
    size_t element_size;
    size_t element_alignment;
} array_growth;

static sxml_status grow_for(const module_loader *loader, array_growth growth, size_t count) {
    if (count < *growth.capacity) {
        return SXML_STATUS_OK;
    }
    size_t new_capacity = 0;
    size_t byte_count = 0;
    sxml_status status = sxml_size_add(*growth.capacity, *growth.capacity + 4, &new_capacity);
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(new_capacity, growth.element_size, &byte_count);
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            loader->set->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = growth.element_alignment},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(memory, 0, byte_count);
    if (*growth.elements != NULL) {
        memcpy(memory, *growth.elements, count * growth.element_size);
    }
    sxml_allocator_release(loader->set->allocator, *growth.elements);
    *growth.elements = memory;
    *growth.capacity = new_capacity;
    return SXML_STATUS_OK;
}

/* A URILiteral node of a module's AST. */
typedef struct literal_node {
    const sxml_ast *ast;
    sxml_ast_node_index node;
} literal_node;

/* Interns a URILiteral node's text, whitespace normalized (A.3.1). */
static sxml_status intern_literal(const module_loader *loader, literal_node literal,
                                  sxml_namespace_uri_identifier *uri) {
    return sxml_xquery_intern_uri_literal(&(sxml_xquery_uri_literal){
                                              .name_pool = loader->name_pool,
                                              .allocator = loader->request->allocator,
                                              .text = text_of(literal.ast, literal.node)},
                                          uri);
}

/* A module's static base URI (5.5): its base URI declaration resolved
   against the module URI (the host's base for the main module). */
static sxml_status base_uri_of(const module_loader *loader, size_t module_index,
                               sxml_uri_resolution *resolution) {
    const sxml_xquery_module *module = &loader->set->modules[module_index];
    const sxml_byte_view initial =
        module_index == 0 ? loader->request->static_base_uri : module->module_uri;
    *resolution = (sxml_uri_resolution){.uri = initial, .memory = NULL};
    for (sxml_ast_node_index declaration = first_declaration(module->ast); declaration.value != 0;
         declaration = next_of(module->ast, declaration)) {
        if (node_of(module->ast, declaration)->kind != SXML_AST_KIND_BASE_URI_DECLARATION) {
            continue;
        }
        sxml_namespace_uri_identifier normalized = {0};
        sxml_status status = intern_literal(
            loader,
            (literal_node){.ast = module->ast,
                           .node = child_of(module->ast, declaration, SXML_AST_FIELD_URI)},
            &normalized);
        sxml_byte_view reference = {.bytes = NULL, .byte_count = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_namespace_uri(loader->name_pool, normalized, &reference);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_uri_resolve(
                &(sxml_uri_resolution_request){.reference = reference,
                                               .base_uri = initial,
                                               .allocator = loader->request->allocator},
                resolution);
        }
        return status;
    }
    return SXML_STATUS_OK;
}

/* The module of the set with module_uri; SIZE_MAX for none. */
static size_t find_module(const sxml_xquery_module_set *set, sxml_byte_view module_uri) {
    for (size_t module_index = 1; module_index < set->module_count; module_index += 1) {
        if (sxml_byte_view_equals(set->modules[module_index].module_uri, module_uri)) {
            return module_index;
        }
    }
    return SIZE_MAX;
}

/* True when the set has a module of namespace. */
static bool has_namespace(const sxml_xquery_module_set *set,
                          sxml_namespace_uri_identifier namespace_uri) {
    for (size_t module_index = 1; module_index < set->module_count; module_index += 1) {
        if (set->modules[module_index].target_namespace.index == namespace_uri.index) {
            return true;
        }
    }
    return false;
}

/* One resolver request of a module import. */
typedef struct import_request {
    /* The importing module and its MODULE_IMPORT. */
    const sxml_ast *ast;
    sxml_ast_node_index declaration;
    sxml_namespace_uri_identifier namespace_uri;
    /* The hint as written (empty for none) and resolved. */
    sxml_byte_view hint;
    sxml_byte_view absolute_hint;
} import_request;

/* Text without a UTF-8 byte order mark. */
static sxml_byte_view without_byte_order_mark(sxml_byte_view text) {
    if (text.byte_count >= 3 && text.bytes[0] == 0xEF && text.bytes[1] == 0xBB &&
        text.bytes[2] == 0xBF) {
        return (sxml_byte_view){.bytes = text.bytes + 3, .byte_count = text.byte_count - 3};
    }
    return text;
}

/* Parses a delivered module as module module_uri of the set and checks it
   is a library module of request->namespace_uri (XQST0059). */
typedef struct delivered_module {
    sxml_byte_view content;
    sxml_byte_view module_uri;
} delivered_module;

static sxml_status add_module(const module_loader *loader, const import_request *request,
                              delivered_module delivered) {
    sxml_xquery_module_set *set = loader->set;
    sxml_status status = grow_for(loader,
                                  (array_growth){.elements = (void **)&set->modules,
                                                 .capacity = &set->module_capacity,
                                                 .element_size = sizeof(sxml_xquery_module),
                                                 .element_alignment = alignof(sxml_xquery_module)},
                                  set->module_count);
    /* The module URI is interned (as a namespace URI), so the name pool
       keeps it as long as error records may name it as their source. */
    sxml_namespace_uri_identifier interned = {0};
    sxml_byte_view module_uri = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_uri(loader->name_pool, delivered.module_uri,
                                                     &interned);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_namespace_uri(loader->name_pool, interned, &module_uri);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ast *ast = NULL;
    status = sxml_xquery_build_ast(
        &(sxml_ast_build_request){.expression_text = without_byte_order_mark(delivered.content),
                                  .source_name = module_uri,
                                  .allocator = loader->request->allocator,
                                  .maximum_expression_depth =
                                      loader->request->maximum_expression_depth,
                                  .error_record = loader->request->error_record,
                                  .reads_xml_1_1 = sxml_processor_reads_query_text_as_xml_1_1(
                                      loader->request->processor)},
        &ast);
    sxml_namespace_uri_identifier namespace_uri = {0};
    if (status == SXML_STATUS_OK && node_of(ast, module_of(ast))->kind == SXML_AST_KIND_LIBRARY_MODULE) {
        const sxml_ast_node_index declaration =
            child_of(ast, module_of(ast), SXML_AST_FIELD_DECLARATION);
        status = intern_literal(
            loader,
            (literal_node){.ast = ast, .node = child_of(ast, declaration, SXML_AST_FIELD_URI)},
            &namespace_uri);
        if (status == SXML_STATUS_OK && namespace_uri.index == SXML_NAMESPACE_NONE) {
            /* 5.2: the module declaration's URI is never empty. */
            status = report(loader, (module_error){.ast = ast,
                                                    .node = declaration,
                                                    .code_and_description =
                                                        SXML_BYTE_VIEW_FROM_LITERAL(
                                                            "XQST0088 a target namespace is "
                                                            "empty")});
        }
    }
    if (status == SXML_STATUS_OK && namespace_uri.index != request->namespace_uri.index) {
        /* 5.12: a main module, or a library module of another namespace. */
        status = report(loader, (module_error){
                                    .ast = request->ast,
                                    .node = request->declaration,
                                    .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                        "XQST0059 the module found is not a library module of "
                                        "the imported namespace")});
    }
    if (status != SXML_STATUS_OK) {
        sxml_ast_destroy(ast);
        return status;
    }
    set->modules[set->module_count] = (sxml_xquery_module){.ast = ast,
                                                           .owned_ast = ast,
                                                           .target_namespace = namespace_uri,
                                                           .module_uri = module_uri};
    set->module_count += 1;
    return SXML_STATUS_OK;
}

/* Asks the resolver for one hint (or none) of an import and adds what it
   delivers, unless its module URI is loaded already. */
static sxml_status request_module(const module_loader *loader, const import_request *request) {
    const sxml_resource_resolver resolver = loader->request->resource_resolver;
    sxml_resource resource;
    memset(&resource, 0, sizeof(resource));
    sxml_status status = SXML_STATUS_OK;
    if (resolver.operations != NULL && resolver.operations->resolve != NULL) {
        sxml_byte_view namespace_text = {.bytes = NULL, .byte_count = 0};
        status = sxml_name_pool_get_namespace_uri(loader->name_pool, request->namespace_uri,
                                                  &namespace_text);
        if (status == SXML_STATUS_OK) {
            status = resolver.operations->resolve(
                resolver.state,
                &(sxml_resource_request){.resource_kind = SXML_RESOURCE_KIND_XQUERY_MODULE,
                                         .uri_reference = request->hint,
                                         .absolute_uri = request->absolute_hint,
                                         .processor = loader->request->processor,
                                         .target_namespace_uri = namespace_text},
                &resource);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (resource.resource_form == SXML_RESOURCE_FORM_NONE) {
        status = report(loader, (module_error){.ast = request->ast,
                                                .node = request->declaration,
                                                .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                                    "XQST0059 no module is found for an "
                                                    "imported namespace")});
    } else if (resource.resource_form != SXML_RESOURCE_FORM_BYTES) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_byte_view module_uri = resource.resource_uri;
    if (module_uri.byte_count == 0) {
        module_uri = request->absolute_hint.byte_count > 0 ? request->absolute_hint : request->hint;
    }
    if (status == SXML_STATUS_OK && module_uri.byte_count == 0) {
        status = sxml_name_pool_get_namespace_uri(loader->name_pool, request->namespace_uri,
                                                  &module_uri);
    }
    if (status == SXML_STATUS_OK) {
        const size_t known = find_module(loader->set, module_uri);
        if (known == SIZE_MAX) {
            status = add_module(loader, request,
                                (delivered_module){.content = resource.content,
                                                   .module_uri = module_uri});
        } else if (loader->set->modules[known].target_namespace.index !=
                   request->namespace_uri.index) {
            status = report(loader, (module_error){
                                        .ast = request->ast,
                                        .node = request->declaration,
                                        .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                            "XQST0059 the module found is not a library module "
                                            "of the imported namespace")});
        }
    }
    if (resolver.operations != NULL && resolver.operations->release != NULL) {
        resolver.operations->release(resolver.state, &resource);
    }
    return status;
}

/* The location hints of one import, each resolved against base_uri. */
typedef struct hinted_import {
    const sxml_ast *ast;
    sxml_ast_node_index declaration;
    sxml_namespace_uri_identifier namespace_uri;
    sxml_byte_view base_uri;
} hinted_import;

static sxml_status request_hints(const module_loader *loader, const hinted_import *import) {
    const sxml_ast *ast = import->ast;
    sxml_status status = SXML_STATUS_OK;
    for (sxml_ast_node_index location = child_of(ast, import->declaration, SXML_AST_FIELD_LOCATION);
         status == SXML_STATUS_OK && location.value != 0; location = next_of(ast, location)) {
        sxml_namespace_uri_identifier normalized = {0};
        sxml_byte_view hint = {.bytes = NULL, .byte_count = 0};
        status = intern_literal(loader, (literal_node){.ast = ast, .node = location}, &normalized);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_namespace_uri(loader->name_pool, normalized, &hint);
        }
        sxml_uri_resolution resolution = {.uri = {.bytes = NULL, .byte_count = 0}, .memory = NULL};
        if (status == SXML_STATUS_OK) {
            /* 5.12.3: hints are made absolute against the importing
               module's static base URI. */
            status = sxml_uri_resolve(
                &(sxml_uri_resolution_request){.reference = hint,
                                               .base_uri = import->base_uri,
                                               .allocator = loader->request->allocator},
                &resolution);
        }
        if (status == SXML_STATUS_OK) {
            status = request_module(
                loader, &(import_request){.ast = ast,
                                          .declaration = import->declaration,
                                          .namespace_uri = import->namespace_uri,
                                          .hint = hint,
                                          .absolute_hint = sxml_uri_has_scheme(resolution.uri)
                                                               ? resolution.uri
                                                               : (sxml_byte_view){.bytes = NULL,
                                                                                  .byte_count = 0}});
            sxml_allocator_release(loader->request->allocator, resolution.memory);
        }
    }
    return status;
}

/* Loads what one MODULE_IMPORT of module module_index names and records
   its namespace among the module's imports. */
typedef struct module_import {
    size_t module_index;
    sxml_ast_node_index declaration;
    sxml_byte_view base_uri;
} module_import;

static sxml_status load_import(const module_loader *loader, module_import import) {
    sxml_xquery_module_set *set = loader->set;
    const sxml_ast *ast = set->modules[import.module_index].ast;
    sxml_namespace_uri_identifier namespace_uri = {0};
    sxml_status status = intern_literal(
        loader,
        (literal_node){.ast = ast, .node = child_of(ast, import.declaration, SXML_AST_FIELD_URI)},
        &namespace_uri);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (namespace_uri.index == SXML_NAMESPACE_NONE) {
        return report(loader, (module_error){.ast = ast,
                                              .node = import.declaration,
                                              .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                                  "XQST0088 a target namespace is empty")});
    }
    const sxml_ast_node_index prefix = child_of(ast, import.declaration, SXML_AST_FIELD_PREFIX);
    if (prefix.value != 0) {
        const sxml_byte_view prefix_text =
            sxml_ast_get_text(ast, node_of(ast, prefix)->payload.name.local_name);
        if (sxml_byte_view_equals(prefix_text, SXML_BYTE_VIEW_FROM_LITERAL("xml")) ||
            sxml_byte_view_equals(prefix_text, SXML_BYTE_VIEW_FROM_LITERAL("xmlns"))) {
            /* 5.12, checked before anything is loaded. */
            return report(loader, (module_error){
                                      .ast = ast,
                                      .node = prefix,
                                      .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                          "XQST0070 the prefixes xml and xmlns cannot be "
                                          "declared")});
        }
    }
    const sxml_xquery_module *module = &set->modules[import.module_index];
    for (size_t import_index = module->first_import;
         import_index < module->first_import + module->import_count; import_index += 1) {
        if (set->imported_namespaces[import_index].index == namespace_uri.index) {
            return report(loader, (module_error){
                                      .ast = ast,
                                      .node = import.declaration,
                                      .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                          "XQST0047 a prolog imports a namespace twice")});
        }
    }
    status = grow_for(loader,
                      (array_growth){.elements = (void **)&set->imported_namespaces,
                                     .capacity = &set->imported_namespace_capacity,
                                     .element_size = sizeof(sxml_namespace_uri_identifier),
                                     .element_alignment = alignof(sxml_namespace_uri_identifier)},
                      set->imported_namespace_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    set->imported_namespaces[set->imported_namespace_count] = namespace_uri;
    set->imported_namespace_count += 1;
    set->modules[import.module_index].import_count += 1;
    if (child_of(ast, import.declaration, SXML_AST_FIELD_LOCATION).value != 0) {
        status = request_hints(loader, &(hinted_import){.ast = ast,
                                                        .declaration = import.declaration,
                                                        .namespace_uri = namespace_uri,
                                                        .base_uri = import.base_uri});
    } else if (!has_namespace(set, namespace_uri) &&
               set->modules[import.module_index].target_namespace.index != namespace_uri.index) {
        status = request_module(loader, &(import_request){.ast = ast,
                                                          .declaration = import.declaration,
                                                          .namespace_uri = namespace_uri,
                                                          .hint = {.bytes = NULL, .byte_count = 0},
                                                          .absolute_hint = {.bytes = NULL,
                                                                            .byte_count = 0}});
    }
    if (status == SXML_STATUS_OK && !has_namespace(set, namespace_uri) &&
        set->modules[import.module_index].target_namespace.index != namespace_uri.index) {
        status = report(loader, (module_error){.ast = ast,
                                                .node = import.declaration,
                                                .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                                    "XQST0059 no module is found for an "
                                                    "imported namespace")});
    }
    return status;
}

/* Scans module module_index's imports (the set may grow meanwhile). */
static sxml_status load_imports_of(const module_loader *loader, size_t module_index) {
    sxml_uri_resolution base = {.uri = {.bytes = NULL, .byte_count = 0}, .memory = NULL};
    sxml_status status = base_uri_of(loader, module_index, &base);
    loader->set->modules[module_index].first_import = loader->set->imported_namespace_count;
    const sxml_ast *ast = loader->set->modules[module_index].ast;
    for (sxml_ast_node_index declaration = first_declaration(ast);
         status == SXML_STATUS_OK && declaration.value != 0;
         declaration = next_of(ast, declaration)) {
        if (node_of(ast, declaration)->kind == SXML_AST_KIND_MODULE_IMPORT) {
            status = load_import(loader, (module_import){.module_index = module_index,
                                                         .declaration = declaration,
                                                         .base_uri = base.uri});
        }
    }
    sxml_allocator_release(loader->request->allocator, base.memory);
    return status;
}

/* ------------------------------------------------------------- Linking */

/* Every module's imported modules: the modules of its imported
   namespaces but itself; counted (the set's imported_modules NULL) or
   written. */
static void list_imported_modules(sxml_xquery_module_set *set) {
    size_t listed = 0;
    for (size_t module_index = 0; module_index < set->module_count; module_index += 1) {
        sxml_xquery_module *module = &set->modules[module_index];
        module->first_imported_module = listed;
        module->imported_module_count = 0;
        for (size_t import_index = module->first_import;
             import_index < module->first_import + module->import_count; import_index += 1) {
            for (size_t imported_index = 1; imported_index < set->module_count;
                 imported_index += 1) {
                if (imported_index == module_index ||
                    set->modules[imported_index].target_namespace.index !=
                        set->imported_namespaces[import_index].index) {
                    continue;
                }
                if (set->imported_modules != NULL) {
                    set->imported_modules[listed] = imported_index;
                }
                listed += 1;
                module->imported_module_count += 1;
            }
        }
    }
    set->imported_module_count = listed;
}

/* The reading order: a depth-first walk from the main module that puts a
   module after the modules it imports; the path holds each module on the
   walk with its next import (D71). */
typedef struct order_walk {
    uint8_t *colors;
    size_t *path_modules;
    size_t *path_imports;
} order_walk;

static void order_modules(sxml_xquery_module_set *set, order_walk walk) {
    size_t order_count = 0;
    for (size_t root_index = 0; root_index < set->module_count; root_index += 1) {
        /* Every module is reached from the main module; the other roots
           only keep the order complete. */
        if (walk.colors[root_index] != 0) {
            continue;
        }
        size_t path_count = 1;
        walk.path_modules[0] = root_index;
        walk.path_imports[0] = 0;
        walk.colors[root_index] = 1;
        while (path_count > 0) {
            const size_t top = walk.path_modules[path_count - 1];
            const sxml_xquery_module *module = &set->modules[top];
            if (walk.path_imports[path_count - 1] == module->imported_module_count) {
                walk.colors[top] = 2;
                set->reading_order[order_count] = top;
                order_count += 1;
                path_count -= 1;
                continue;
            }
            const size_t imported = set->imported_modules[module->first_imported_module +
                                                          walk.path_imports[path_count - 1]];
            walk.path_imports[path_count - 1] += 1;
            if (walk.colors[imported] == 0) {
                /* A module on the path (color 1) is a cycle, broken here. */
                walk.colors[imported] = 1;
                walk.path_modules[path_count] = imported;
                walk.path_imports[path_count] = 0;
                path_count += 1;
            }
        }
    }
}

/* Zeroed scratch of element_count elements of element_size bytes. */
typedef struct scratch_request {
    size_t element_count;
    size_t element_size;
} scratch_request;

static sxml_status allocate_zeroed(const sxml_xquery_module_set *set, scratch_request request,
                                   void **memory) {
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(request.element_count == 0 ? 1 : request.element_count,
                                            request.element_size, &byte_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            set->allocator,
            (sxml_allocation_request){.byte_count = byte_count, .alignment = alignof(size_t)},
            memory);
    }
    if (status == SXML_STATUS_OK) {
        memset(*memory, 0, byte_count);
    }
    return status;
}

/* Lists the imported modules and orders the set. */
static sxml_status link_modules(sxml_xquery_module_set *set) {
    list_imported_modules(set);
    const size_t count = set->module_count;
    void *imported = NULL;
    void *order = NULL;
    void *colors = NULL;
    void *path_modules = NULL;
    void *path_imports = NULL;
    sxml_status status = allocate_zeroed(
        set, (scratch_request){.element_count = set->imported_module_count, .element_size = sizeof(size_t)},
        &imported);
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(
            set, (scratch_request){.element_count = count, .element_size = sizeof(size_t)}, &order);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(
            set, (scratch_request){.element_count = count, .element_size = sizeof(uint8_t)},
            &colors);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(
            set, (scratch_request){.element_count = count, .element_size = sizeof(size_t)},
            &path_modules);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(
            set, (scratch_request){.element_count = count, .element_size = sizeof(size_t)},
            &path_imports);
    }
    if (status == SXML_STATUS_OK) {
        set->imported_modules = imported;
        set->reading_order = order;
        imported = NULL;
        order = NULL;
        list_imported_modules(set);
        order_modules(set, (order_walk){.colors = colors,
                                        .path_modules = path_modules,
                                        .path_imports = path_imports});
    }
    sxml_allocator_release(set->allocator, imported);
    sxml_allocator_release(set->allocator, order);
    sxml_allocator_release(set->allocator, colors);
    sxml_allocator_release(set->allocator, path_modules);
    sxml_allocator_release(set->allocator, path_imports);
    return status;
}

/* ========================================================= Entry points */

void sxml_xquery_release_modules(sxml_xquery_module_set *set) {
    if (set == NULL) {
        return;
    }
    for (size_t module_index = 0; module_index < set->module_count; module_index += 1) {
        sxml_ast_destroy(set->modules[module_index].owned_ast);
    }
    sxml_allocator_release(set->allocator, set->modules);
    sxml_allocator_release(set->allocator, set->imported_namespaces);
    sxml_allocator_release(set->allocator, set->imported_modules);
    sxml_allocator_release(set->allocator, set->reading_order);
    *set = (sxml_xquery_module_set){.allocator = set->allocator};
}

sxml_status sxml_xquery_load_modules(const sxml_xquery_module_loading *request,
                                     sxml_xquery_module_set *set) {
    if (request == NULL || request->main_ast == NULL || request->item_heap == NULL ||
        set == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_xquery_module_set loaded = {.allocator = request->allocator};
    const module_loader loader = {.request = request,
                                  .name_pool = sxml_item_heap_get_name_pool(request->item_heap),
                                  .set = &loaded};
    sxml_status status = grow_for(&loader,
                                  (array_growth){.elements = (void **)&loaded.modules,
                                                 .capacity = &loaded.module_capacity,
                                                 .element_size = sizeof(sxml_xquery_module),
                                                 .element_alignment = alignof(sxml_xquery_module)},
                                  0);
    if (status == SXML_STATUS_OK) {
        loaded.modules[0] = (sxml_xquery_module){.ast = request->main_ast};
        loaded.module_count = 1;
    }
    for (size_t module_index = 0; status == SXML_STATUS_OK && module_index < loaded.module_count;
         module_index += 1) {
        status = load_imports_of(&loader, module_index);
    }
    if (status == SXML_STATUS_OK) {
        status = link_modules(&loaded);
    }
    if (status != SXML_STATUS_OK) {
        sxml_xquery_release_modules(&loaded);
        return status;
    }
    *set = loaded;
    return SXML_STATUS_OK;
}
