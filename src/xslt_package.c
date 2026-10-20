/*
 * XSLT 4.0 packages (src/xslt_package_internal.h; XSLT 4.0, section 3.4;
 * docs/DECISIONS.md D160; packet 4.16).
 *
 * Golden files: src/xquery_module.c (a set of modules found on a work
 * list, each module's declarations seen by the others only when public,
 * D148) and src/xslt_lower_declaration.c (tables of the components of one
 * lowering in the lowerer's scratch memory, made before anything is
 * lowered and released after).
 *
 * The table is made in four steps, none recursive (D71): the package of
 * every module (a module's package is its parent's unless it is the
 * principal module or a USED_PACKAGE module), of every static context
 * and the overriding declaration around each; then the instances of each
 * package, from the last package to the first so a used package is done
 * before its users: its own winning declarations with their visibility
 * (3.4.5.1), then for each xsl:use-package the overriding and accepted
 * instances (3.4.5.2, 3.4.5.3), each instance of the used package
 * pointing at its successor in the user; then the checks across
 * instances. Binding walks the successors.
 *
 * Section numbers are of XSLT 4.0 (W3C QT4 CG draft, October 2026).
 */
#include "xslt_package_internal.h"

#include "sxml/name.h"
#include "sxml/types.h"
#include "xslt_lower_internal.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------- State */

/* One package. */
typedef struct package_entry {
    sxml_xslt_ast_module_index top_module;
    /* PACKAGE, or STYLESHEET for an implicit package. */
    sxml_xslt_ast_node_index root;
    /* The xsl:use-package that read it; 0 for the top-level package. */
    sxml_xslt_ast_node_index use_node;
    uint32_t parent_package;
    /* Its instances are first_instance to first_instance + count - 1. */
    uint32_t first_instance;
    uint32_t instance_count;
} package_entry;

/* One component instance (glossary). */
typedef struct component_instance {
    sxml_xslt_component_kind component_kind;
    sxml_name_identifier component_name;
    /* The arity range of a function; 0 for the other kinds. */
    uint32_t minimum_arity;
    uint32_t maximum_arity;
    /* The declaration whose body it has. */
    sxml_xslt_ast_node_index declaration;
    /* What made it: the declaration (own or overriding) or, for an
       accepted copy, the xsl:use-package. */
    sxml_xslt_ast_node_index origin;
    uint32_t package;
    sxml_xslt_visibility visibility;
    uint32_t import_precedence;
    bool is_accepted;
    bool is_overriding;
    /* An overriding instance: the instance of the used package it
       overrides (3.4.5.4). */
    uint32_t overridden_instance;
    /* The instance of the using package that replaces it: the overriding
       instance or the accepted copy; 0 for none. */
    uint32_t successor;
} component_instance;

struct sxml_xslt_package_table {
    package_entry *packages;
    uint32_t package_count;
    /* module_packages[m]: the package of module m. */
    uint32_t *module_packages;
    uint32_t module_count;
    /* By AST static context: its package and overriding declaration. */
    uint32_t *context_packages;
    sxml_xslt_ast_node_index *context_overrides;
    uint32_t context_count;
    /* Entry 0 unused. */
    component_instance *instances;
    uint32_t instance_count;
    uint32_t instance_capacity;
    /* Scratch memory of other modules, released with the table. */
    void **kept_memory;
    uint32_t kept_count;
    uint32_t kept_capacity;
};

/* ------------------------------------------------------------ AST reads */

static const sxml_xslt_ast_node *node_of(const sxml_xslt_lowering *lowering,
                                         sxml_xslt_ast_node_index node) {
    return sxml_xslt_ast_get_node(lowering->ast, node);
}

static const sxml_xslt_ast_attribute *find_attribute(const sxml_xslt_lowering *lowering,
                                                     sxml_xslt_ast_node_index node,
                                                     sxml_xslt_ast_attribute_kind kind) {
    return sxml_xslt_ast_get_attribute(
        lowering->ast,
        sxml_xslt_ast_find_attribute(lowering->ast,
                                     (sxml_xslt_ast_attribute_query){.node = node, .kind = kind}));
}

static sxml_name_identifier name_of(const sxml_xslt_lowering *lowering,
                                    sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_attribute *attribute =
        find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_NAME);
    if (attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_NAME) {
        return (sxml_name_identifier){.index = 0};
    }
    return attribute->value.payload.name;
}

static sxml_xslt_ast_keyword keyword_of(const sxml_xslt_lowering *lowering,
                                        sxml_xslt_ast_node_index node,
                                        sxml_xslt_ast_attribute_kind kind) {
    const sxml_xslt_ast_attribute *attribute = find_attribute(lowering, node, kind);
    if (attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_KEYWORD) {
        return SXML_XSLT_AST_KEYWORD_NONE;
    }
    return attribute->value.payload.keyword;
}

/* A TEXT attribute's value, trimmed; empty without. */
static sxml_byte_view text_of(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node,
                              sxml_xslt_ast_attribute_kind kind) {
    const sxml_xslt_ast_attribute *attribute = find_attribute(lowering, node, kind);
    if (attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_TEXT) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return sxml_xml_text_trim_whitespace(sxml_xslt_ast_get_text(lowering->ast, attribute->value.payload.text));
}

static bool has_attribute(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node,
                          sxml_xslt_ast_attribute_kind kind) {
    return find_attribute(lowering, node, kind)->kind != SXML_XSLT_AST_ATTRIBUTE_NONE;
}

static uint32_t precedence_of(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node) {
    return sxml_xslt_ast_get_module(lowering->ast, node_of(lowering, node)->module)
        ->import_precedence;
}

static bool is_overriding_node(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node) {
    return (node_of(lowering, node)->flags & SXML_XSLT_AST_NODE_FLAG_DECLARATION) != 0 &&
           node_of(lowering, node_of(lowering, node)->parent)->kind == SXML_XSLT_AST_KIND_OVERRIDE;
}

static sxml_status report(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node,
                          sxml_byte_view code_and_description) {
    return sxml_xslt_lowering_report_static_error(
        lowering,
        (sxml_xslt_static_error){.node = node, .code_and_description = code_and_description});
}

/* ----------------------------------------------------------- Memory */

typedef struct array_request {
    size_t element_count;
    size_t element_size;
    size_t element_alignment;
} array_request;

/* Zeroed scratch memory for element_count elements (at least one). */
static sxml_status allocate_zeroed(const sxml_xslt_lowering *lowering, array_request request,
                                   void **memory) {
    const size_t count = request.element_count == 0 ? 1U : request.element_count;
    void *allocated = NULL;
    const sxml_status status = sxml_xpath_lowerer_allocate_scratch(
        lowering->lowerer,
        (sxml_xpath_lowering_scratch_request){.element_count = count,
                                              .element_size = request.element_size,
                                              .element_alignment = request.element_alignment},
        &allocated);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(allocated, 0, count * request.element_size);
    *memory = allocated;
    return SXML_STATUS_OK;
}

/* Appends a zeroed instance and writes its index. */
static sxml_status add_instance(const sxml_xslt_lowering *lowering, component_instance instance,
                                uint32_t *index) {
    sxml_xslt_package_table *table = lowering->packages;
    if (table->instance_count + 1U >= table->instance_capacity) {
        const uint32_t capacity =
            table->instance_capacity < 16U ? 16U : table->instance_capacity * 2U;
        if (capacity <= table->instance_capacity) {
            return SXML_STATUS_LIMIT_EXCEEDED;
        }
        void *memory = NULL;
        const sxml_status status =
            allocate_zeroed(lowering,
                            (array_request){.element_count = capacity,
                                            .element_size = sizeof(component_instance),
                                            .element_alignment = alignof(component_instance)},
                            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (table->instances != NULL) {
            memcpy(memory, table->instances,
                   (size_t)table->instance_count * sizeof(component_instance));
            sxml_xpath_lowerer_release_scratch(lowering->lowerer, table->instances);
        }
        table->instances = memory;
        table->instance_capacity = capacity;
        if (table->instance_count == 0) {
            table->instance_count = 1;
        }
    }
    *index = table->instance_count;
    table->instances[table->instance_count] = instance;
    table->instance_count += 1;
    return SXML_STATUS_OK;
}

sxml_status sxml_xslt_package_allocate(const sxml_xslt_lowering *lowering,
                                       sxml_xslt_package_memory_request request, void **memory) {
    sxml_xslt_package_table *table = lowering->packages;
    if (table == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (table->kept_count == table->kept_capacity) {
        const uint32_t capacity = table->kept_capacity < 8U ? 8U : table->kept_capacity * 2U;
        void *list = NULL;
        const sxml_status status =
            allocate_zeroed(lowering,
                            (array_request){.element_count = capacity,
                                            .element_size = sizeof(void *),
                                            .element_alignment = alignof(void *)},
                            &list);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (table->kept_memory != NULL) {
            memcpy(list, (void *)table->kept_memory, (size_t)table->kept_count * sizeof(void *));
            sxml_xpath_lowerer_release_scratch(lowering->lowerer, (void *)table->kept_memory);
        }
        table->kept_memory = (void **)list;
        table->kept_capacity = capacity;
    }
    void *allocated = NULL;
    const sxml_status status =
        allocate_zeroed(lowering,
                        (array_request){.element_count = request.element_count,
                                        .element_size = request.element_size,
                                        .element_alignment = request.element_alignment},
                        &allocated);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    table->kept_memory[table->kept_count] = allocated;
    table->kept_count += 1;
    *memory = allocated;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------ Package lookup */

uint32_t sxml_xslt_package_count(const sxml_xslt_lowering *lowering) {
    const sxml_xslt_package_table *table = lowering->packages;
    return table == NULL || table->package_count < 2U ? 1U : table->package_count - 1U;
}

sxml_xslt_ast_module_index sxml_xslt_package_top_module(const sxml_xslt_ast *ast,
                                                        sxml_xslt_ast_node_index node) {
    sxml_xslt_ast_module_index module =
        node.value == 0 ? (sxml_xslt_ast_module_index){.value = 1}
                        : sxml_xslt_ast_get_node(ast, node)->module;
    /* Up the module tree (each module's parent comes first, so the walk
       ends) to the top of the package, as the table's module_packages. */
    while (module.value != 0) {
        const sxml_xslt_ast_module *entry = sxml_xslt_ast_get_module(ast, module);
        if (entry->relation == SXML_XSLT_AST_MODULE_RELATION_PRINCIPAL ||
            entry->relation == SXML_XSLT_AST_MODULE_RELATION_USED_PACKAGE) {
            return module;
        }
        module = entry->parent_module;
    }
    return (sxml_xslt_ast_module_index){.value = 1};
}

uint32_t sxml_xslt_package_of_node(const sxml_xslt_lowering *lowering,
                                   sxml_xslt_ast_node_index node) {
    const sxml_xslt_package_table *table = lowering->packages;
    const uint32_t module = node_of(lowering, node)->module.value;
    if (table == NULL || module == 0 || module >= table->module_count) {
        return 1;
    }
    return table->module_packages[module];
}

uint32_t sxml_xslt_package_of_static_context(const sxml_xslt_lowering *lowering,
                                             sxml_xslt_ast_static_context_index static_context) {
    const sxml_xslt_package_table *table = lowering->packages;
    if (table == NULL || static_context.value == 0 ||
        static_context.value >= table->context_count ||
        table->context_packages[static_context.value] == 0) {
        return 1;
    }
    return table->context_packages[static_context.value];
}

sxml_xslt_ast_node_index sxml_xslt_package_get_root(const sxml_xslt_lowering *lowering,
                                                    uint32_t package) {
    const sxml_xslt_package_table *table = lowering->packages;
    if (table == NULL || package == 0 || package >= table->package_count) {
        return sxml_xslt_ast_get_module(lowering->ast, (sxml_xslt_ast_module_index){.value = 1})
            ->root;
    }
    return table->packages[package].root;
}

bool sxml_xslt_package_is_same(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index first,
                               sxml_xslt_ast_node_index second) {
    return sxml_xslt_package_of_node(lowering, first) ==
           sxml_xslt_package_of_node(lowering, second);
}

sxml_xslt_ast_node_index
sxml_xslt_package_overriding_declaration_of(const sxml_xslt_lowering *lowering,
                                            sxml_xslt_ast_static_context_index static_context) {
    const sxml_xslt_package_table *table = lowering->packages;
    if (table == NULL || static_context.value == 0 ||
        static_context.value >= table->context_count) {
        return (sxml_xslt_ast_node_index){.value = 0};
    }
    return table->context_overrides[static_context.value];
}

sxml_xslt_ast_node_index
sxml_xslt_package_overriding_ancestor_of(const sxml_xslt_lowering *lowering,
                                         sxml_xslt_ast_node_index node) {
    sxml_xslt_ast_node_index current = node;
    while (current.value != 0 &&
           (node_of(lowering, current)->flags & SXML_XSLT_AST_NODE_FLAG_DECLARATION) == 0) {
        current = node_of(lowering, current)->parent;
    }
    if (current.value != 0 && is_overriding_node(lowering, current)) {
        return current;
    }
    return (sxml_xslt_ast_node_index){.value = 0};
}

/* -------------------------------------------------- Packages, contexts */

static sxml_status find_packages(const sxml_xslt_lowering *lowering) {
    sxml_xslt_package_table *table = lowering->packages;
    const sxml_xslt_ast *ast = lowering->ast;
    const uint32_t module_count = ast->module_count < 2U ? 2U : ast->module_count;
    void *memory = NULL;
    sxml_status status = allocate_zeroed(lowering,
                                         (array_request){.element_count = module_count,
                                                         .element_size = sizeof(uint32_t),
                                                         .element_alignment = alignof(uint32_t)},
                                         &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    table->module_packages = memory;
    table->module_count = module_count;
    uint32_t package_count = 1;
    for (uint32_t module = 1; module < ast->module_count; module += 1) {
        const sxml_xslt_ast_module_relation relation =
            sxml_xslt_ast_get_module(ast, (sxml_xslt_ast_module_index){.value = module})->relation;
        package_count += relation == SXML_XSLT_AST_MODULE_RELATION_PRINCIPAL ||
                                 relation == SXML_XSLT_AST_MODULE_RELATION_USED_PACKAGE
                             ? 1U
                             : 0U;
    }
    status = allocate_zeroed(lowering,
                             (array_request){.element_count = package_count,
                                             .element_size = sizeof(package_entry),
                                             .element_alignment = alignof(package_entry)},
                             &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    table->packages = memory;
    table->package_count = 1;
    for (uint32_t module = 1; module < ast->module_count; module += 1) {
        const sxml_xslt_ast_module *entry =
            sxml_xslt_ast_get_module(ast, (sxml_xslt_ast_module_index){.value = module});
        if (entry->relation == SXML_XSLT_AST_MODULE_RELATION_PRINCIPAL ||
            entry->relation == SXML_XSLT_AST_MODULE_RELATION_USED_PACKAGE) {
            const uint32_t parent = entry->parent_module.value;
            table->packages[table->package_count] =
                (package_entry){.top_module = {.value = module},
                                .root = entry->root,
                                .use_node = entry->referencing_node,
                                .parent_package = parent == 0 ? 0U : table->module_packages[parent],
                                .first_instance = 0,
                                .instance_count = 0};
            table->module_packages[module] = table->package_count;
            table->package_count += 1;
        } else {
            table->module_packages[module] = table->module_packages[entry->parent_module.value];
        }
    }
    return SXML_STATUS_OK;
}

/* The package and overriding declaration of every static context: those
   of the nodes that use it, inherited by derived contexts (a context is
   made after the one it derives from). */
static sxml_status map_static_contexts(const sxml_xslt_lowering *lowering) {
    sxml_xslt_package_table *table = lowering->packages;
    const sxml_xslt_ast *ast = lowering->ast;
    const uint32_t context_count = ast->static_context_count < 2U ? 2U : ast->static_context_count;
    void *memory = NULL;
    sxml_status status = allocate_zeroed(lowering,
                                         (array_request){.element_count = context_count,
                                                         .element_size = sizeof(uint32_t),
                                                         .element_alignment = alignof(uint32_t)},
                                         &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    table->context_packages = memory;
    status =
        allocate_zeroed(lowering,
                        (array_request){.element_count = context_count,
                                        .element_size = sizeof(sxml_xslt_ast_node_index),
                                        .element_alignment = alignof(sxml_xslt_ast_node_index)},
                        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    table->context_overrides = memory;
    table->context_count = context_count;
    for (uint32_t node_index = 1; node_index < ast->node_count; node_index += 1) {
        const sxml_xslt_ast_node_index node = {.value = node_index};
        const uint32_t context = node_of(lowering, node)->static_context.value;
        if (context == 0 || context >= context_count) {
            continue;
        }
        if (table->context_packages[context] == 0) {
            table->context_packages[context] = sxml_xslt_package_of_node(lowering, node);
        }
        if (is_overriding_node(lowering, node)) {
            table->context_overrides[context] = node;
        }
    }
    for (uint32_t context = 1; context < ast->static_context_count; context += 1) {
        const uint32_t parent = sxml_xslt_ast_get_static_context(
                                    ast, (sxml_xslt_ast_static_context_index){.value = context})
                                    ->parent.value;
        if (parent == 0 || parent >= context) {
            continue;
        }
        if (table->context_packages[context] == 0) {
            table->context_packages[context] = table->context_packages[parent];
        }
        if (table->context_overrides[context].value == 0) {
            table->context_overrides[context] = table->context_overrides[parent];
        }
    }
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------- Symbolic names */

/* The component a declaration declares; kind NONE for none. */
typedef struct declared_component {
    sxml_xslt_component_kind component_kind;
    sxml_name_identifier component_name;
    uint32_t minimum_arity;
    uint32_t maximum_arity;
} declared_component;

static declared_component component_of(const sxml_xslt_lowering *lowering,
                                       sxml_xslt_ast_node_index declaration) {
    declared_component component = {.component_kind = SXML_XSLT_COMPONENT_KIND_NONE,
                                    .component_name = {.index = 0},
                                    .minimum_arity = 0,
                                    .maximum_arity = 0};
    const sxml_name_identifier name = name_of(lowering, declaration);
    if (name.index == 0) {
        return component;
    }
    component.component_name = name;
    switch ((uint32_t)node_of(lowering, declaration)->kind) {
    case SXML_XSLT_AST_KIND_FUNCTION:
        component.component_kind = SXML_XSLT_COMPONENT_KIND_FUNCTION;
        for (sxml_xslt_ast_node_index child = node_of(lowering, declaration)->first_child;
             child.value != 0; child = node_of(lowering, child)->next_sibling) {
            if (node_of(lowering, child)->kind != SXML_XSLT_AST_KIND_PARAM) {
                continue;
            }
            const sxml_xslt_ast_attribute *required =
                find_attribute(lowering, child, SXML_XSLT_AST_ATTRIBUTE_REQUIRED);
            const bool is_required = required->value.kind != SXML_XSLT_AST_VALUE_KIND_BOOLEAN ||
                                     required->value.payload.is_true;
            component.minimum_arity += is_required ? 1U : 0U;
            component.maximum_arity += 1;
        }
        break;
    case SXML_XSLT_AST_KIND_TEMPLATE:
        component.component_kind = SXML_XSLT_COMPONENT_KIND_TEMPLATE;
        break;
    case SXML_XSLT_AST_KIND_VARIABLE:
    case SXML_XSLT_AST_KIND_PARAM:
        component.component_kind = SXML_XSLT_COMPONENT_KIND_VARIABLE;
        break;
    case SXML_XSLT_AST_KIND_ATTRIBUTE_SET:
        component.component_kind = SXML_XSLT_COMPONENT_KIND_ATTRIBUTE_SET;
        break;
    case SXML_XSLT_AST_KIND_MODE:
        component.component_kind = SXML_XSLT_COMPONENT_KIND_MODE;
        break;
    case SXML_XSLT_AST_KIND_ITEM_TYPE:
    case SXML_XSLT_AST_KIND_RECORD_TYPE:
        component.component_kind = SXML_XSLT_COMPONENT_KIND_ITEM_TYPE;
        break;
    default:
        component.component_name = (sxml_name_identifier){.index = 0};
        break;
    }
    return component;
}

static bool ranges_overlap(const component_instance *instance, declared_component component) {
    return instance->minimum_arity <= component.maximum_arity &&
           component.minimum_arity <= instance->maximum_arity;
}

/* Same kind and name and, for functions, overlapping arity ranges: the
   instances a reference could not tell apart. */
static bool is_homonymous(const component_instance *instance, declared_component component) {
    return instance->component_kind == component.component_kind &&
           instance->component_name.index == component.component_name.index &&
           (component.component_kind != SXML_XSLT_COMPONENT_KIND_FUNCTION ||
            ranges_overlap(instance, component));
}

static declared_component component_of_instance(const component_instance *instance) {
    return (declared_component){.component_kind = instance->component_kind,
                                .component_name = instance->component_name,
                                .minimum_arity = instance->minimum_arity,
                                .maximum_arity = instance->maximum_arity};
}

/* ------------------------------------------------- Expose and accept */

/* How a component test matches (3.4.5.1): by name, by a wildcard with a
   namespace or local name, by "*". */
typedef enum match_strength {
    SXML_MATCH_STRENGTH_NONE = 0,
    SXML_MATCH_STRENGTH_ANY_NAME,
    SXML_MATCH_STRENGTH_PARTIAL_WILDCARD,
    SXML_MATCH_STRENGTH_EXPLICIT,
} match_strength;

static sxml_xslt_component_kind kind_of_keyword(sxml_xslt_ast_keyword keyword) {
    switch ((uint32_t)keyword) {
    case SXML_XSLT_AST_KEYWORD_TEMPLATE:
        return SXML_XSLT_COMPONENT_KIND_TEMPLATE;
    case SXML_XSLT_AST_KEYWORD_FUNCTION:
        return SXML_XSLT_COMPONENT_KIND_FUNCTION;
    case SXML_XSLT_AST_KEYWORD_VARIABLE:
        return SXML_XSLT_COMPONENT_KIND_VARIABLE;
    case SXML_XSLT_AST_KEYWORD_ATTRIBUTE_SET:
        return SXML_XSLT_COMPONENT_KIND_ATTRIBUTE_SET;
    case SXML_XSLT_AST_KEYWORD_MODE:
        return SXML_XSLT_COMPONENT_KIND_MODE;
    case SXML_XSLT_AST_KEYWORD_ITEM_TYPE:
    case SXML_XSLT_AST_KEYWORD_RECORD_TYPE:
        return SXML_XSLT_COMPONENT_KIND_ITEM_TYPE;
    default:
        return SXML_XSLT_COMPONENT_KIND_NONE;
    }
}

static sxml_xslt_visibility visibility_of_keyword(sxml_xslt_ast_keyword keyword) {
    switch ((uint32_t)keyword) {
    case SXML_XSLT_AST_KEYWORD_PUBLIC:
        return SXML_XSLT_VISIBILITY_PUBLIC;
    case SXML_XSLT_AST_KEYWORD_PRIVATE:
        return SXML_XSLT_VISIBILITY_PRIVATE;
    case SXML_XSLT_AST_KEYWORD_FINAL:
        return SXML_XSLT_VISIBILITY_FINAL;
    case SXML_XSLT_AST_KEYWORD_ABSTRACT:
        return SXML_XSLT_VISIBILITY_ABSTRACT;
    case SXML_XSLT_AST_KEYWORD_HIDDEN:
        return SXML_XSLT_VISIBILITY_HIDDEN;
    default:
        return SXML_XSLT_VISIBILITY_NONE;
    }
}

static sxml_xslt_ast_name_token_view tokens_of(const sxml_xslt_lowering *lowering,
                                               sxml_xslt_ast_node_index element) {
    const sxml_xslt_ast_attribute *names =
        find_attribute(lowering, element, SXML_XSLT_AST_ATTRIBUTE_NAMES);
    if (names->value.kind != SXML_XSLT_AST_VALUE_KIND_NAME_LIST) {
        return (sxml_xslt_ast_name_token_view){.name_tokens = NULL, .name_token_count = 0};
    }
    return sxml_xslt_ast_get_name_tokens(lowering->ast, names->value.payload.name_tokens);
}

/* How one token matches a component. */
typedef struct token_test {
    const sxml_xslt_ast_name_token *token;
    declared_component component;
} token_test;

static match_strength match_token(const sxml_xslt_lowering *lowering, token_test test) {
    const sxml_xslt_ast_name_token *token = test.token;
    const declared_component component = test.component;
    const bool is_function = component.component_kind == SXML_XSLT_COMPONENT_KIND_FUNCTION;
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    switch ((uint32_t)token->kind) {
    case SXML_XSLT_AST_NAME_TOKEN_KIND_NAME:
        /* A function is named with its arity, others without (3.4.5.1). */
        if (token->name.index != component.component_name.index ||
            token->has_arity != is_function ||
            (is_function &&
             (token->arity < component.minimum_arity || token->arity > component.maximum_arity))) {
            return SXML_MATCH_STRENGTH_NONE;
        }
        return SXML_MATCH_STRENGTH_EXPLICIT;
    case SXML_XSLT_AST_NAME_TOKEN_KIND_ANY_NAME:
        return SXML_MATCH_STRENGTH_ANY_NAME;
    case SXML_XSLT_AST_NAME_TOKEN_KIND_NAMESPACE_WILDCARD:
        if (sxml_name_pool_get_name_parts(lowering->lowerer->name_pool, component.component_name,
                                          &parts) != SXML_STATUS_OK ||
            parts.namespace_uri.index != token->namespace_uri.index) {
            return SXML_MATCH_STRENGTH_NONE;
        }
        return SXML_MATCH_STRENGTH_PARTIAL_WILDCARD;
    case SXML_XSLT_AST_NAME_TOKEN_KIND_LOCAL_NAME_WILDCARD: {
        sxml_name_parts token_parts = parts;
        if (sxml_name_pool_get_name_parts(lowering->lowerer->name_pool, component.component_name,
                                          &parts) != SXML_STATUS_OK ||
            sxml_name_pool_get_name_parts(lowering->lowerer->name_pool, token->name,
                                          &token_parts) != SXML_STATUS_OK ||
            !sxml_byte_view_equals(parts.local_name, token_parts.local_name)) {
            return SXML_MATCH_STRENGTH_NONE;
        }
        return SXML_MATCH_STRENGTH_PARTIAL_WILDCARD;
    }
    default:
        return SXML_MATCH_STRENGTH_NONE;
    }
}

/* The strongest token of element (xsl:expose or xsl:accept) that matches
   component; NONE when its component attribute names another kind. */
static match_strength match_element(const sxml_xslt_lowering *lowering,
                                    sxml_xslt_ast_node_index element,
                                    declared_component component) {
    const sxml_xslt_ast_keyword kind =
        keyword_of(lowering, element, SXML_XSLT_AST_ATTRIBUTE_COMPONENT);
    if (kind != SXML_XSLT_AST_KEYWORD_ANY_COMPONENT &&
        kind_of_keyword(kind) != component.component_kind) {
        return SXML_MATCH_STRENGTH_NONE;
    }
    const sxml_xslt_ast_name_token_view tokens = tokens_of(lowering, element);
    match_strength strongest = SXML_MATCH_STRENGTH_NONE;
    for (size_t token_index = 0; token_index < tokens.name_token_count; token_index += 1) {
        const match_strength strength =
            match_token(lowering, (token_test){.token = &tokens.name_tokens[token_index],
                                               .component = component});
        strongest = strength > strongest ? strength : strongest;
    }
    return strongest;
}

/* The elements to search: the children of parent of one kind. */
typedef struct element_search {
    sxml_xslt_ast_node_index parent;
    sxml_xslt_ast_kind element_kind;
    declared_component component;
    /* xsl:accept: the visibility in the used package, for 3.4.5.2's
       table; NONE for xsl:expose. */
    sxml_xslt_visibility used_visibility;
} element_search;

/* What the best element gives. */
typedef struct element_choice {
    sxml_xslt_ast_node_index element;
    match_strength strength;
    sxml_xslt_visibility visibility;
} element_choice;

/* 3.4.5.2: the visibility an xsl:accept may give to a component of the
   used package's visibility. */
static bool is_acceptable(sxml_xslt_visibility accepted, sxml_xslt_visibility used) {
    const bool is_public_or_final =
        used == SXML_XSLT_VISIBILITY_PUBLIC || used == SXML_XSLT_VISIBILITY_FINAL;
    switch ((uint32_t)accepted) {
    case SXML_XSLT_VISIBILITY_PUBLIC:
        return used == SXML_XSLT_VISIBILITY_PUBLIC;
    case SXML_XSLT_VISIBILITY_PRIVATE:
    case SXML_XSLT_VISIBILITY_FINAL:
        return is_public_or_final;
    case SXML_XSLT_VISIBILITY_ABSTRACT:
        return used == SXML_XSLT_VISIBILITY_ABSTRACT;
    case SXML_XSLT_VISIBILITY_HIDDEN:
        return is_public_or_final || used == SXML_XSLT_VISIBILITY_ABSTRACT;
    default:
        return false;
    }
}

/* The strongest matching element, the last of equal strength (3.4.5.1);
   for xsl:accept a wildcard match the table forbids does not count
   (3.4.5.2), an explicit one is *conflict. */
static element_choice choose_element(const sxml_xslt_lowering *lowering,
                                     const element_search *search,
                                     sxml_xslt_ast_node_index *conflict) {
    element_choice choice = {.element = {.value = 0},
                             .strength = SXML_MATCH_STRENGTH_NONE,
                             .visibility = SXML_XSLT_VISIBILITY_NONE};
    for (sxml_xslt_ast_node_index child = node_of(lowering, search->parent)->first_child;
         child.value != 0; child = node_of(lowering, child)->next_sibling) {
        if (node_of(lowering, child)->kind != search->element_kind) {
            continue;
        }
        const match_strength strength = match_element(lowering, child, search->component);
        if (strength == SXML_MATCH_STRENGTH_NONE || strength < choice.strength) {
            continue;
        }
        const sxml_xslt_visibility visibility =
            visibility_of_keyword(keyword_of(lowering, child, SXML_XSLT_AST_ATTRIBUTE_VISIBILITY));
        if (search->used_visibility != SXML_XSLT_VISIBILITY_NONE &&
            !is_acceptable(visibility, search->used_visibility)) {
            if (strength == SXML_MATCH_STRENGTH_EXPLICIT && conflict->value == 0) {
                *conflict = child;
            }
            continue;
        }
        choice = (element_choice){.element = child, .strength = strength, .visibility = visibility};
    }
    return choice;
}

/* 3.4.5.1's table: an explicit exposure a declared visibility allows. */
static bool is_exposure_consistent(sxml_xslt_visibility exposed, sxml_xslt_visibility declared) {
    switch ((uint32_t)exposed) {
    case SXML_XSLT_VISIBILITY_PUBLIC:
        return declared == SXML_XSLT_VISIBILITY_PUBLIC;
    case SXML_XSLT_VISIBILITY_PRIVATE:
        return declared != SXML_XSLT_VISIBILITY_ABSTRACT;
    case SXML_XSLT_VISIBILITY_FINAL:
        return declared == SXML_XSLT_VISIBILITY_PUBLIC || declared == SXML_XSLT_VISIBILITY_FINAL;
    case SXML_XSLT_VISIBILITY_ABSTRACT:
        return declared == SXML_XSLT_VISIBILITY_ABSTRACT;
    default:
        return true;
    }
}

/* The visibility of an own instance in its package (3.4.5.1). */
static sxml_status decide_visibility(const sxml_xslt_lowering *lowering,
                                     component_instance *instance) {
    const sxml_xslt_package_table *table = lowering->packages;
    const package_entry *package = &table->packages[instance->package];
    const sxml_xslt_ast_node_index declaration = instance->declaration;
    if (node_of(lowering, declaration)->kind == SXML_XSLT_AST_KIND_PARAM) {
        /* 3.4.5.2: a stylesheet parameter becomes public in a using
           package, and xsl:expose never matches one; as XSLT 3.0 and the
           test suite have it, it is overridable (4.0's 3.4.5.1 calls it
           private: D160). */
        instance->visibility = SXML_XSLT_VISIBILITY_PUBLIC;
        return SXML_STATUS_OK;
    }
    const sxml_xslt_visibility declared = visibility_of_keyword(
        keyword_of(lowering, declaration, SXML_XSLT_AST_ATTRIBUTE_VISIBILITY));
    const bool is_implicit = node_of(lowering, package->root)->kind != SXML_XSLT_AST_KIND_PACKAGE;
    sxml_xslt_ast_node_index unused = {.value = 0};
    const element_choice exposed =
        is_implicit
            ? (element_choice){.element = {.value = 0},
                               .strength = SXML_MATCH_STRENGTH_NONE,
                               .visibility = SXML_XSLT_VISIBILITY_NONE}
            : choose_element(lowering,
                             &(element_search){.parent = package->root,
                                               .element_kind = SXML_XSLT_AST_KIND_EXPOSE,
                                               .component = component_of_instance(instance),
                                               .used_visibility = SXML_XSLT_VISIBILITY_NONE},
                             &unused);
    sxml_xslt_visibility visibility = SXML_XSLT_VISIBILITY_PRIVATE;
    if (exposed.strength == SXML_MATCH_STRENGTH_EXPLICIT) {
        if (declared != SXML_XSLT_VISIBILITY_NONE &&
            !is_exposure_consistent(exposed.visibility, declared)) {
            return report(lowering, exposed.element,
                          SXML_BYTE_VIEW_FROM_LITERAL(
                              "XTSE3010 xsl:expose gives a component a visibility its "
                              "declaration does not allow (XSLT 4.0, 3.4.5.1)"));
        }
        visibility = exposed.visibility;
    } else if (declared != SXML_XSLT_VISIBILITY_NONE) {
        visibility = declared;
    } else if (exposed.strength != SXML_MATCH_STRENGTH_NONE) {
        visibility = exposed.visibility;
    } else if (is_implicit && (instance->component_kind == SXML_XSLT_COMPONENT_KIND_TEMPLATE ||
                               instance->component_kind == SXML_XSLT_COMPONENT_KIND_MODE)) {
        /* 3.4: an xsl:stylesheet exposes its named templates and modes. */
        visibility = SXML_XSLT_VISIBILITY_PUBLIC;
    }
    if (visibility == SXML_XSLT_VISIBILITY_ABSTRACT && declared != SXML_XSLT_VISIBILITY_ABSTRACT) {
        return report(lowering, exposed.element.value != 0 ? exposed.element : declaration,
                      SXML_BYTE_VIEW_FROM_LITERAL(
                          "XTSE3025 xsl:expose makes a component abstract that is not declared "
                          "abstract (XSLT 4.0, 3.4.5.1)"));
    }
    instance->visibility = visibility;
    return SXML_STATUS_OK;
}

/* XTSE3022/XTSE3032: component="*" with a token that is no wildcard;
   XTSE3020/XTSE3030: an explicit token that matches no component among
   instances first to first + count - 1. */
typedef struct test_check {
    sxml_xslt_ast_node_index element;
    uint32_t first_instance;
    uint32_t instance_count;
} test_check;

/* XTSE3022/XTSE3032 for element, an xsl:expose or xsl:accept. */
static sxml_status check_any_component(const sxml_xslt_lowering *lowering,
                                       sxml_xslt_ast_node_index element) {
    const bool is_expose = node_of(lowering, element)->kind == SXML_XSLT_AST_KIND_EXPOSE;
    if (keyword_of(lowering, element, SXML_XSLT_AST_ATTRIBUTE_COMPONENT) !=
        SXML_XSLT_AST_KEYWORD_ANY_COMPONENT) {
        return SXML_STATUS_OK;
    }
    const sxml_xslt_ast_name_token_view tokens = tokens_of(lowering, element);
    for (size_t token_index = 0; token_index < tokens.name_token_count; token_index += 1) {
        if (tokens.name_tokens[token_index].kind == SXML_XSLT_AST_NAME_TOKEN_KIND_NAME) {
            return report(lowering, element,
                          is_expose ? SXML_BYTE_VIEW_FROM_LITERAL(
                                          "XTSE3022 xsl:expose with component=\"*\" names a "
                                          "component that is no wildcard (XSLT 4.0, 3.4.5.1)")
                                    : SXML_BYTE_VIEW_FROM_LITERAL(
                                          "XTSE3032 xsl:accept with component=\"*\" names a "
                                          "component that is no wildcard (XSLT 4.0, 3.4.5.2)"));
        }
    }
    return SXML_STATUS_OK;
}

static sxml_status check_tests(const sxml_xslt_lowering *lowering, test_check check) {
    const sxml_xslt_package_table *table = lowering->packages;
    const bool is_expose = node_of(lowering, check.element)->kind == SXML_XSLT_AST_KIND_EXPOSE;
    const sxml_xslt_ast_keyword kind =
        keyword_of(lowering, check.element, SXML_XSLT_AST_ATTRIBUTE_COMPONENT);
    const sxml_status status = check_any_component(lowering, check.element);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_xslt_ast_name_token_view tokens = tokens_of(lowering, check.element);
    for (size_t token_index = 0; token_index < tokens.name_token_count; token_index += 1) {
        const sxml_xslt_ast_name_token *token = &tokens.name_tokens[token_index];
        if (token->kind != SXML_XSLT_AST_NAME_TOKEN_KIND_NAME) {
            continue;
        }
        bool is_matched = false;
        for (uint32_t offset = 0; offset < check.instance_count && !is_matched; offset += 1) {
            const component_instance *instance = &table->instances[check.first_instance + offset];
            /* xsl:expose is about the package's own declarations; no
               xsl:accept matches an xsl:param (3.4.5.1, 3.4.5.2). */
            const bool is_candidate = is_expose ? !instance->is_accepted && !instance->is_overriding
                                                : node_of(lowering, instance->declaration)->kind !=
                                                      SXML_XSLT_AST_KIND_PARAM;
            is_matched =
                is_candidate && kind_of_keyword(kind) == instance->component_kind &&
                match_token(lowering, (token_test){.token = token,
                                                   .component = component_of_instance(instance)}) ==
                    SXML_MATCH_STRENGTH_EXPLICIT;
        }
        if (!is_matched) {
            return report(lowering, check.element,
                          is_expose ? SXML_BYTE_VIEW_FROM_LITERAL(
                                          "XTSE3020 xsl:expose names a component the package "
                                          "does not have (XSLT 4.0, 3.4.5.1)")
                                    : SXML_BYTE_VIEW_FROM_LITERAL(
                                          "XTSE3030 xsl:accept names a component the used "
                                          "package does not have (XSLT 4.0, 3.4.5.2)"));
        }
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- Own instances */

/* The own declarations of package that win by import precedence: one
   instance per symbolic identifier (for functions, per arity range not
   eclipsed, 10.3.4.1), the highest precedence first met. */
static sxml_status add_own_instances(const sxml_xslt_lowering *lowering, uint32_t package) {
    sxml_xslt_package_table *table = lowering->packages;
    const sxml_xslt_ast *ast = lowering->ast;
    const uint32_t first = table->instance_count == 0 ? 1U : table->instance_count;
    table->packages[package].first_instance = first;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t order = 1;
         status == SXML_STATUS_OK && sxml_xslt_ast_get_declaration(ast, order).value != 0;
         order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        if (sxml_xslt_package_of_node(lowering, declaration) != package ||
            is_overriding_node(lowering, declaration)) {
            continue;
        }
        const declared_component component = component_of(lowering, declaration);
        if (component.component_kind == SXML_XSLT_COMPONENT_KIND_NONE) {
            continue;
        }
        const uint32_t precedence = precedence_of(lowering, declaration);
        bool is_added = false;
        const uint32_t end = table->instance_count == 0 ? 1U : table->instance_count;
        for (uint32_t index = first; index < end && !is_added; index += 1) {
            component_instance *other = &table->instances[index];
            if (other->component_kind != component.component_kind ||
                other->component_name.index != component.component_name.index) {
                continue;
            }
            const bool is_same_range =
                component.component_kind != SXML_XSLT_COMPONENT_KIND_FUNCTION ||
                (other->minimum_arity == component.minimum_arity &&
                 other->maximum_arity == component.maximum_arity);
            const bool is_contained =
                component.component_kind != SXML_XSLT_COMPONENT_KIND_FUNCTION ||
                (other->minimum_arity <= component.minimum_arity &&
                 component.maximum_arity <= other->maximum_arity);
            if (is_contained && other->import_precedence >= precedence) {
                /* Eclipsed or overridden by import precedence. */
                is_added = true;
            } else if (is_same_range && precedence > other->import_precedence) {
                other->declaration = declaration;
                other->origin = declaration;
                other->import_precedence = precedence;
                is_added = true;
            }
        }
        if (!is_added) {
            uint32_t index = 0;
            status = add_instance(lowering,
                                  (component_instance){.component_kind = component.component_kind,
                                                       .component_name = component.component_name,
                                                       .minimum_arity = component.minimum_arity,
                                                       .maximum_arity = component.maximum_arity,
                                                       .declaration = declaration,
                                                       .origin = declaration,
                                                       .package = package,
                                                       .visibility = SXML_XSLT_VISIBILITY_NONE,
                                                       .import_precedence = precedence,
                                                       .is_accepted = false,
                                                       .is_overriding = false,
                                                       .overridden_instance = 0,
                                                       .successor = 0},
                                  &index);
        }
    }
    /* An xsl:expose that is invalid in itself (XTSE3022) is reported
       before the visibilities it would give (XTSE3010, XTSE3025). */
    const sxml_xslt_ast_node_index root = table->packages[package].root;
    for (sxml_xslt_ast_node_index child = node_of(lowering, root)->first_child;
         status == SXML_STATUS_OK && child.value != 0;
         child = node_of(lowering, child)->next_sibling) {
        if (node_of(lowering, child)->kind == SXML_XSLT_AST_KIND_EXPOSE) {
            status = check_any_component(lowering, child);
        }
    }
    const uint32_t end = table->instance_count == 0 ? 1U : table->instance_count;
    for (uint32_t index = first; status == SXML_STATUS_OK && index < end; index += 1) {
        status = decide_visibility(lowering, &table->instances[index]);
    }
    for (sxml_xslt_ast_node_index child = node_of(lowering, root)->first_child;
         status == SXML_STATUS_OK && child.value != 0;
         child = node_of(lowering, child)->next_sibling) {
        if (node_of(lowering, child)->kind == SXML_XSLT_AST_KIND_EXPOSE) {
            status = check_tests(lowering, (test_check){.element = child,
                                                        .first_instance = first,
                                                        .instance_count = end - first});
        }
    }
    return status;
}

/* ------------------------------------------------------------ Versions */

/* One portion of a version (3.4.1). */
typedef struct version_portion {
    bool is_integer;
    uint64_t integer;
    sxml_byte_view name;
} version_portion;

#define MAXIMUM_VERSION_PORTIONS 16U

typedef struct parsed_version {
    version_portion portions[MAXIMUM_VERSION_PORTIONS];
    size_t portion_count;
} parsed_version;

static bool is_name_start_byte(uint8_t byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || byte == '_' ||
           byte >= 0x80U;
}

static bool is_name_byte(uint8_t byte) {
    return is_name_start_byte(byte) || (byte >= '0' && byte <= '9') || byte == '-' || byte == '.';
}

/* PackageVersion ::= NumericPart ( "-" NamePart )?, NumericPart ::=
   IntegerLiteral ( "." IntegerLiteral )*. */
static bool parse_version(sxml_byte_view text, parsed_version *version) {
    version->portion_count = 0;
    size_t position = 0;
    while (true) {
        if (position >= text.byte_count || text.bytes[position] < '0' ||
            text.bytes[position] > '9' || version->portion_count == MAXIMUM_VERSION_PORTIONS) {
            return false;
        }
        uint64_t integer = 0;
        while (position < text.byte_count && text.bytes[position] >= '0' &&
               text.bytes[position] <= '9') {
            if (integer > UINT64_MAX / 10U - 10U) {
                return false;
            }
            integer = integer * 10U + (uint64_t)(text.bytes[position] - '0');
            position += 1;
        }
        version->portions[version->portion_count] = (version_portion){
            .is_integer = true, .integer = integer, .name = {.bytes = NULL, .byte_count = 0}};
        version->portion_count += 1;
        if (position < text.byte_count && text.bytes[position] == '.') {
            position += 1;
            continue;
        }
        break;
    }
    if (position == text.byte_count) {
        return true;
    }
    if (text.bytes[position] != '-' || position + 1U >= text.byte_count ||
        !is_name_start_byte(text.bytes[position + 1U]) ||
        version->portion_count == MAXIMUM_VERSION_PORTIONS) {
        return false;
    }
    const size_t start = position + 1U;
    for (position = start; position < text.byte_count; position += 1) {
        if (!is_name_byte(text.bytes[position])) {
            return false;
        }
    }
    version->portions[version->portion_count] = (version_portion){
        .is_integer = false,
        .integer = 0,
        .name = {.bytes = text.bytes + start, .byte_count = text.byte_count - start}};
    version->portion_count += 1;
    return true;
}

/* Trailing zero portions dropped: zero integers not followed by an
   integer (3.4.1). */
static parsed_version normalized(parsed_version version) {
    size_t integer_end = 0;
    while (integer_end < version.portion_count && version.portions[integer_end].is_integer) {
        integer_end += 1;
    }
    size_t kept = integer_end;
    while (kept > 0 && version.portions[kept - 1U].integer == 0) {
        kept -= 1;
    }
    if (kept < integer_end) {
        const size_t tail = version.portion_count - integer_end;
        memmove(&version.portions[kept], &version.portions[integer_end],
                tail * sizeof(version_portion));
        version.portion_count = kept + tail;
    }
    return version;
}

static int compare_portions(const version_portion *first, const version_portion *second) {
    if (first->is_integer && second->is_integer) {
        return first->integer < second->integer ? -1 : first->integer > second->integer ? 1 : 0;
    }
    if (first->is_integer != second->is_integer) {
        /* An NCName comes first. */
        return first->is_integer ? 1 : -1;
    }
    const size_t common = first->name.byte_count < second->name.byte_count
                              ? first->name.byte_count
                              : second->name.byte_count;
    const int order = common == 0 ? 0 : memcmp(first->name.bytes, second->name.bytes, common);
    if (order != 0) {
        return order < 0 ? -1 : 1;
    }
    return first->name.byte_count < second->name.byte_count   ? -1
           : first->name.byte_count > second->name.byte_count ? 1
                                                              : 0;
}

/* The order of two versions (3.4.1): -1, 0 or 1. */
typedef struct version_pair {
    const parsed_version *first;
    const parsed_version *second;
} version_pair;

static int compare_versions(version_pair pair) {
    const parsed_version first = normalized(*pair.first);
    const parsed_version second = normalized(*pair.second);
    const size_t common =
        first.portion_count < second.portion_count ? first.portion_count : second.portion_count;
    for (size_t index = 0; index < common; index += 1) {
        const int order = compare_portions(&first.portions[index], &second.portions[index]);
        if (order != 0) {
            return order;
        }
    }
    if (first.portion_count == second.portion_count) {
        return 0;
    }
    if (first.portion_count < second.portion_count) {
        return second.portions[common].is_integer ? -1 : 1;
    }
    return first.portions[common].is_integer ? 1 : -1;
}

/* The leading portions of version are those of prefix. */
static bool has_prefix(version_pair pair) {
    if (pair.first->portion_count < pair.second->portion_count) {
        return false;
    }
    for (size_t index = 0; index < pair.second->portion_count; index += 1) {
        if (compare_portions(&pair.first->portions[index], &pair.second->portions[index]) != 0) {
            return false;
        }
    }
    return true;
}

static bool ends_with(sxml_byte_view text, sxml_byte_view suffix) {
    return text.byte_count >= suffix.byte_count &&
           memcmp(text.bytes + text.byte_count - suffix.byte_count, suffix.bytes,
                  suffix.byte_count) == 0;
}

/* The end of a range "to X" (X a version or a prefix V.*). */
typedef struct range_end {
    parsed_version version;
    bool is_prefix;
} range_end;

static bool parse_range_end(sxml_byte_view text, range_end *end) {
    end->is_prefix = ends_with(text, SXML_BYTE_VIEW_FROM_LITERAL(".*"));
    if (end->is_prefix) {
        text.byte_count -= 2;
    }
    return parse_version(text, &end->version);
}

static bool is_at_most(const parsed_version *version, const range_end *end) {
    return compare_versions((version_pair){.first = version, .second = &end->version}) <= 0 ||
           (end->is_prefix &&
            has_prefix((version_pair){.first = version, .second = &end->version}));
}

/* One VersionRange matching version. */
typedef struct range_test {
    sxml_byte_view range;
    const parsed_version *version;
} range_test;

/* Whether the range has the grammar's syntax, and then whether it
   matches. */
typedef struct range_outcome {
    bool is_valid;
    bool matches;
} range_outcome;

static range_outcome matches_range(range_test test) {
    const range_outcome invalid = {.is_valid = false, .matches = false};
    sxml_byte_view range = sxml_xml_text_trim_whitespace(test.range);
    /* VersionTo: "to" S (PackageVersion | VersionPrefix). */
    if (range.byte_count > 3 && memcmp(range.bytes, "to", 2) == 0 && sxml_xml_byte_is_whitespace(range.bytes[2])) {
        range_end end;
        if (!parse_range_end(sxml_xml_text_trim_whitespace((sxml_byte_view){.bytes = range.bytes + 2,
                                                   .byte_count = range.byte_count - 2}),
                             &end)) {
            return invalid;
        }
        return (range_outcome){.is_valid = true, .matches = is_at_most(test.version, &end)};
    }
    /* VersionFromTo: PackageVersion S "to" S (...). */
    for (size_t position = 1; position + 3U < range.byte_count; position += 1) {
        if (sxml_xml_byte_is_whitespace(range.bytes[position - 1U]) && range.bytes[position] == 't' &&
            range.bytes[position + 1U] == 'o' && sxml_xml_byte_is_whitespace(range.bytes[position + 2U])) {
            parsed_version from;
            range_end end;
            if (!parse_version(sxml_xml_text_trim_whitespace((sxml_byte_view){.bytes = range.bytes, .byte_count = position}),
                               &from) ||
                !parse_range_end(
                    sxml_xml_text_trim_whitespace((sxml_byte_view){.bytes = range.bytes + position + 2U,
                                          .byte_count = range.byte_count - position - 2U}),
                    &end)) {
                return invalid;
            }
            return (range_outcome){.is_valid = true,
                                   .matches = compare_versions((version_pair){
                                                  .first = test.version, .second = &from}) >= 0 &&
                                              is_at_most(test.version, &end)};
        }
    }
    parsed_version bound;
    if (ends_with(range, SXML_BYTE_VIEW_FROM_LITERAL("+"))) {
        if (!parse_version(
                (sxml_byte_view){.bytes = range.bytes, .byte_count = range.byte_count - 1},
                &bound)) {
            return invalid;
        }
        return (range_outcome){.is_valid = true,
                               .matches = compare_versions((version_pair){.first = test.version,
                                                                          .second = &bound}) >= 0};
    }
    if (ends_with(range, SXML_BYTE_VIEW_FROM_LITERAL(".*"))) {
        if (!parse_version(
                (sxml_byte_view){.bytes = range.bytes, .byte_count = range.byte_count - 2},
                &bound)) {
            return invalid;
        }
        return (range_outcome){
            .is_valid = true,
            .matches = has_prefix((version_pair){.first = test.version, .second = &bound})};
    }
    if (!parse_version(range, &bound)) {
        return invalid;
    }
    return (range_outcome){
        .is_valid = true,
        .matches = compare_versions((version_pair){.first = test.version, .second = &bound}) == 0};
}

sxml_status sxml_xslt_package_version_matches(sxml_xslt_package_version_query query,
                                              sxml_xslt_package_version_match *match) {
    parsed_version version;
    if (!parse_version(sxml_xml_text_trim_whitespace(query.package_version), &version)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_byte_view range = sxml_xml_text_trim_whitespace(query.version_range);
    if (range.byte_count == 1 && range.bytes[0] == '*') {
        match->matches = true;
        return SXML_STATUS_OK;
    }
    bool is_matched = false;
    size_t start = 0;
    for (size_t position = 0; position <= range.byte_count; position += 1) {
        if (position < range.byte_count && range.bytes[position] != ',') {
            continue;
        }
        const range_outcome outcome = matches_range(
            (range_test){.range = {.bytes = range.bytes + start, .byte_count = position - start},
                         .version = &version});
        if (!outcome.is_valid) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        is_matched = is_matched || outcome.matches;
        start = position + 1U;
    }
    match->matches = is_matched;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------ Used packages */

/* XTSE3003 and XTSE0020 (3.4.1, 3.4.3): the used package's name and
   version against the xsl:use-package. */
static sxml_status check_used_package(const sxml_xslt_lowering *lowering, uint32_t used) {
    const package_entry *package = &lowering->packages->packages[used];
    const sxml_xslt_ast_node_index use_node = package->use_node;
    const sxml_xslt_ast_node_index root = package->root;
    const sxml_byte_view version =
        has_attribute(lowering, root, SXML_XSLT_AST_ATTRIBUTE_PACKAGE_VERSION)
            ? text_of(lowering, root, SXML_XSLT_AST_ATTRIBUTE_PACKAGE_VERSION)
            : SXML_BYTE_VIEW_FROM_LITERAL("1");
    const sxml_byte_view range =
        has_attribute(lowering, use_node, SXML_XSLT_AST_ATTRIBUTE_PACKAGE_VERSION)
            ? text_of(lowering, use_node, SXML_XSLT_AST_ATTRIBUTE_PACKAGE_VERSION)
            : SXML_BYTE_VIEW_FROM_LITERAL("*");
    sxml_xslt_package_version_match match = {.matches = false};
    if (sxml_xslt_package_version_matches(
            (sxml_xslt_package_version_query){.package_version = SXML_BYTE_VIEW_FROM_LITERAL("1"),
                                              .version_range = range},
            &match) != SXML_STATUS_OK) {
        return report(lowering, use_node,
                      SXML_BYTE_VIEW_FROM_LITERAL(
                          "XTSE0020 the package-version of an xsl:use-package is no version "
                          "range (XSLT 4.0, 3.4.1)"));
    }
    if (node_of(lowering, root)->kind != SXML_XSLT_AST_KIND_PACKAGE ||
        !sxml_byte_view_equals(text_of(lowering, root, SXML_XSLT_AST_ATTRIBUTE_NAME),
                               text_of(lowering, use_node, SXML_XSLT_AST_ATTRIBUTE_NAME))) {
        return report(lowering, use_node,
                      SXML_BYTE_VIEW_FROM_LITERAL(
                          "XTSE3003 the package found for an xsl:use-package has another name "
                          "(XSLT 4.0, 3.4.3)"));
    }
    if (sxml_xslt_package_version_matches(
            (sxml_xslt_package_version_query){.package_version = version, .version_range = range},
            &match) != SXML_STATUS_OK) {
        return report(lowering, root,
                      SXML_BYTE_VIEW_FROM_LITERAL(
                          "XTSE0020 the package-version of an xsl:package is no version (XSLT "
                          "4.0, 3.4.1)"));
    }
    if (!match.matches) {
        return report(lowering, use_node,
                      SXML_BYTE_VIEW_FROM_LITERAL(
                          "XTSE3003 the package found for an xsl:use-package has a version out of "
                          "its range (XSLT 4.0, 3.4.3)"));
    }
    return SXML_STATUS_OK;
}

/* The xsl:override children of use_node's xsl:override elements in turn:
   the one after previous (0 for the first); 0 after the last. */
static sxml_xslt_ast_node_index next_overriding(const sxml_xslt_lowering *lowering,
                                                sxml_xslt_ast_node_index use_node,
                                                sxml_xslt_ast_node_index previous) {
    sxml_xslt_ast_node_index candidate = {.value = 0};
    sxml_xslt_ast_node_index group = node_of(lowering, use_node)->first_child;
    if (previous.value != 0) {
        candidate = node_of(lowering, previous)->next_sibling;
        group = node_of(lowering, previous)->parent;
        if (candidate.value != 0) {
            return candidate;
        }
        group = node_of(lowering, group)->next_sibling;
    }
    for (; group.value != 0; group = node_of(lowering, group)->next_sibling) {
        if (node_of(lowering, group)->kind == SXML_XSLT_AST_KIND_OVERRIDE &&
            node_of(lowering, group)->first_child.value != 0) {
            return node_of(lowering, group)->first_child;
        }
    }
    return (sxml_xslt_ast_node_index){.value = 0};
}

/* The overriding declaration of use_node homonymous with instance. */
static sxml_xslt_ast_node_index find_overriding(const sxml_xslt_lowering *lowering,
                                                sxml_xslt_ast_node_index use_node,
                                                const component_instance *instance) {
    for (sxml_xslt_ast_node_index overriding =
             next_overriding(lowering, use_node, (sxml_xslt_ast_node_index){.value = 0});
         overriding.value != 0; overriding = next_overriding(lowering, use_node, overriding)) {
        const declared_component component = component_of(lowering, overriding);
        if (component.component_kind == instance->component_kind &&
            component.component_name.index == instance->component_name.index &&
            (component.component_kind != SXML_XSLT_COMPONENT_KIND_FUNCTION ||
             component.maximum_arity == instance->maximum_arity)) {
            return overriding;
        }
    }
    return (sxml_xslt_ast_node_index){.value = 0};
}

/* ------------------------------------------- Signatures (3.4.5.3) */

/* The declared type of node's as attribute; ok false when it has one
   that does not resolve yet (then nothing is compared). */
typedef struct declared_type {
    bool has_type;
    bool is_resolved;
    sxml_sequence_type sequence_type;
} declared_type;

static declared_type declared_type_of(const sxml_xslt_lowering *lowering,
                                      sxml_xslt_ast_node_index node) {
    declared_type type = {
        .has_type = false,
        .is_resolved = true,
        .sequence_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                          .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}};
    const sxml_xslt_ast_attribute *as_attribute =
        find_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_AS);
    /* An item type (xsl:context-item) is read as a SEQUENCE_TYPE of
       occurrence one (D139). */
    if (as_attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_SEQUENCE_TYPE &&
        as_attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_ITEM_TYPE) {
        return type;
    }
    type.has_type = true;
    if (as_attribute->value.kind == SXML_XSLT_AST_VALUE_KIND_ITEM_TYPE) {
        type.sequence_type.occurrence = SXML_OCCURRENCE_EXACTLY_ONE;
    }
    const sxml_xslt_ast_expression *expression =
        sxml_xslt_ast_get_expression(lowering->ast, as_attribute->value.payload.expression);
    const uint32_t context = expression->static_context.value;
    type.is_resolved =
        expression->ast != NULL && context != 0 && context < lowering->static_context_count &&
        sxml_xpath_resolve_sequence_type(
            &(sxml_xpath_type_request){.ast = expression->ast,
                                       .type_node = expression->ast->root,
                                       .static_context = &lowering->static_contexts[context],
                                       .error_record = NULL},
            &type.sequence_type) == SXML_STATUS_OK;
    return type;
}

/* A variable's declared type for 3.4.5.3: as, else item()* with a select,
   document-node() with content, else xs:string. */
static declared_type variable_type_of(const sxml_xslt_lowering *lowering,
                                      sxml_xslt_ast_node_index node) {
    declared_type type = declared_type_of(lowering, node);
    if (type.has_type) {
        return type;
    }
    if (!has_attribute(lowering, node, SXML_XSLT_AST_ATTRIBUTE_SELECT)) {
        type.sequence_type =
            node_of(lowering, node)->first_child.value != 0
                ? (sxml_sequence_type){.item_type =
                                           sxml_builtin_type_identifier(SXML_TYPE_DOCUMENT_NODE),
                                       .occurrence = SXML_OCCURRENCE_EXACTLY_ONE}
                : (sxml_sequence_type){.item_type =
                                           sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                                       .occurrence = SXML_OCCURRENCE_EXACTLY_ONE};
    }
    return type;
}

/* Types identical in the sense of 3.4.5.3: each a subtype of the other;
   true when either is not resolved. */
typedef struct type_comparison {
    declared_type first;
    declared_type second;
} type_comparison;

static bool are_identical(const sxml_xslt_lowering *lowering, type_comparison comparison) {
    if (!comparison.first.is_resolved || !comparison.second.is_resolved) {
        return true;
    }
    const sxml_type_registry *registry = lowering->lowerer->type_registry;
    return sxml_type_registry_is_sequence_subtype(
               registry, (sxml_sequence_type_pair){.derived_type = comparison.first.sequence_type,
                                                   .base_type = comparison.second.sequence_type}) &&
           sxml_type_registry_is_sequence_subtype(
               registry, (sxml_sequence_type_pair){.derived_type = comparison.second.sequence_type,
                                                   .base_type = comparison.first.sequence_type});
}

static bool is_true_attribute(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node,
                              sxml_xslt_ast_attribute_kind kind) {
    const sxml_xslt_ast_attribute *attribute = find_attribute(lowering, node, kind);
    return attribute->value.kind == SXML_XSLT_AST_VALUE_KIND_BOOLEAN &&
           attribute->value.payload.is_true;
}

/* The xsl:param child of node named name; 0 for none. */
typedef struct parameter_query {
    sxml_xslt_ast_node_index node;
    sxml_name_identifier name;
} parameter_query;

static sxml_xslt_ast_node_index find_parameter(const sxml_xslt_lowering *lowering,
                                               parameter_query query) {
    for (sxml_xslt_ast_node_index child = node_of(lowering, query.node)->first_child;
         child.value != 0; child = node_of(lowering, child)->next_sibling) {
        if (node_of(lowering, child)->kind == SXML_XSLT_AST_KIND_PARAM &&
            name_of(lowering, child).index == query.name.index) {
            return child;
        }
    }
    return (sxml_xslt_ast_node_index){.value = 0};
}

/* The signatures of two homonymous declarations (3.4.5.3). */
typedef struct signature_pair {
    sxml_xslt_ast_node_index overriding;
    sxml_xslt_ast_node_index overridden;
} signature_pair;

static bool are_parameters_compatible(const sxml_xslt_lowering *lowering, signature_pair pair) {
    const bool is_function =
        node_of(lowering, pair.overriding)->kind == SXML_XSLT_AST_KIND_FUNCTION;
    /* Every parameter of the overridden one has a counterpart. */
    sxml_xslt_ast_node_index overriding_child = node_of(lowering, pair.overriding)->first_child;
    for (sxml_xslt_ast_node_index child = node_of(lowering, pair.overridden)->first_child;
         child.value != 0; child = node_of(lowering, child)->next_sibling) {
        if (node_of(lowering, child)->kind != SXML_XSLT_AST_KIND_PARAM) {
            continue;
        }
        sxml_xslt_ast_node_index counterpart = {.value = 0};
        if (is_function) {
            /* Functions: pairwise by position. */
            while (overriding_child.value != 0 &&
                   node_of(lowering, overriding_child)->kind != SXML_XSLT_AST_KIND_PARAM) {
                overriding_child = node_of(lowering, overriding_child)->next_sibling;
            }
            counterpart = overriding_child;
            if (overriding_child.value != 0) {
                overriding_child = node_of(lowering, overriding_child)->next_sibling;
            }
        } else {
            counterpart =
                find_parameter(lowering, (parameter_query){.node = pair.overriding,
                                                           .name = name_of(lowering, child)});
        }
        const bool is_tunnel = is_true_attribute(lowering, child, SXML_XSLT_AST_ATTRIBUTE_TUNNEL);
        if (counterpart.value == 0) {
            if (is_function || !is_tunnel) {
                return false;
            }
            continue;
        }
        if (is_true_attribute(lowering, counterpart, SXML_XSLT_AST_ATTRIBUTE_TUNNEL) != is_tunnel ||
            !are_identical(lowering,
                           (type_comparison){.first = declared_type_of(lowering, child),
                                             .second = declared_type_of(lowering, counterpart)})) {
            return false;
        }
        if (!is_function && !is_tunnel &&
            is_true_attribute(lowering, child, SXML_XSLT_AST_ATTRIBUTE_REQUIRED) !=
                is_true_attribute(lowering, counterpart, SXML_XSLT_AST_ATTRIBUTE_REQUIRED)) {
            return false;
        }
    }
    /* A template's extra parameters are optional. */
    for (sxml_xslt_ast_node_index child = node_of(lowering, pair.overriding)->first_child;
         !is_function && child.value != 0; child = node_of(lowering, child)->next_sibling) {
        if (node_of(lowering, child)->kind == SXML_XSLT_AST_KIND_PARAM &&
            find_parameter(lowering, (parameter_query){.node = pair.overridden,
                                                       .name = name_of(lowering, child)})
                    .value == 0 &&
            is_true_attribute(lowering, child, SXML_XSLT_AST_ATTRIBUTE_REQUIRED)) {
            return false;
        }
    }
    return true;
}

/* A template's xsl:context-item: its use and type; optional item()
   without one (3.4.5.3). */
typedef struct context_item_signature {
    sxml_xslt_ast_keyword use;
    declared_type type;
} context_item_signature;

static context_item_signature context_item_of(const sxml_xslt_lowering *lowering,
                                              sxml_xslt_ast_node_index template_node) {
    context_item_signature signature = {
        .use = SXML_XSLT_AST_KEYWORD_OPTIONAL,
        .type = {.has_type = false,
                 .is_resolved = true,
                 .sequence_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                                   .occurrence = SXML_OCCURRENCE_EXACTLY_ONE}}};
    const sxml_xslt_ast_node_index first = node_of(lowering, template_node)->first_child;
    if (node_of(lowering, first)->kind != SXML_XSLT_AST_KIND_CONTEXT_ITEM) {
        return signature;
    }
    const sxml_xslt_ast_keyword use = keyword_of(lowering, first, SXML_XSLT_AST_ATTRIBUTE_USE);
    signature.use = use == SXML_XSLT_AST_KEYWORD_NONE ? SXML_XSLT_AST_KEYWORD_OPTIONAL : use;
    const declared_type type = declared_type_of(lowering, first);
    if (type.has_type) {
        signature.type = type;
    }
    return signature;
}

static bool are_context_items_equivalent(const sxml_xslt_lowering *lowering, signature_pair pair) {
    const context_item_signature first = context_item_of(lowering, pair.overriding);
    const context_item_signature second = context_item_of(lowering, pair.overridden);
    return first.use == second.use &&
           are_identical(lowering, (type_comparison){.first = first.type, .second = second.type});
}

/* XTSE3070 (3.4.5.3): the overriding signature is compatible. */
static bool is_signature_compatible(const sxml_xslt_lowering *lowering, signature_pair pair) {
    switch ((uint32_t)node_of(lowering, pair.overriding)->kind) {
    case SXML_XSLT_AST_KIND_TEMPLATE:
        if (!are_context_items_equivalent(lowering, pair)) {
            return false;
        }
        /* The return type and parameters, as for a function. */
        return are_identical(
                   lowering,
                   (type_comparison){.first = declared_type_of(lowering, pair.overriding),
                                     .second = declared_type_of(lowering, pair.overridden)}) &&
               are_parameters_compatible(lowering, pair);
    case SXML_XSLT_AST_KIND_FUNCTION:
        return are_identical(
                   lowering,
                   (type_comparison){.first = declared_type_of(lowering, pair.overriding),
                                     .second = declared_type_of(lowering, pair.overridden)}) &&
               are_parameters_compatible(lowering, pair);
    case SXML_XSLT_AST_KIND_VARIABLE:
    case SXML_XSLT_AST_KIND_PARAM:
        return are_identical(
            lowering, (type_comparison){.first = variable_type_of(lowering, pair.overriding),
                                        .second = variable_type_of(lowering, pair.overridden)});
    default:
        return true;
    }
}

/* An instance of the used package seen through an xsl:use-package. */
typedef struct use_request {
    uint32_t package;
    sxml_xslt_ast_node_index use_node;
    uint32_t used_instance;
} use_request;

/* The instance of package for used instance: an overriding one, an
   accepted copy, or none (hidden). */
static sxml_status add_used_instance(const sxml_xslt_lowering *lowering, use_request request) {
    sxml_xslt_package_table *table = lowering->packages;
    const component_instance used = table->instances[request.used_instance];
    const sxml_xslt_ast_node_index overriding = find_overriding(lowering, request.use_node, &used);
    uint32_t index = 0;
    if (overriding.value != 0) {
        if (used.visibility != SXML_XSLT_VISIBILITY_PUBLIC &&
            used.visibility != SXML_XSLT_VISIBILITY_ABSTRACT) {
            return report(lowering, overriding,
                          SXML_BYTE_VIEW_FROM_LITERAL(
                              "XTSE3060 an overriding declaration overrides a component that is "
                              "not public or abstract (XSLT 4.0, 3.4.5.3)"));
        }
        const declared_component component = component_of(lowering, overriding);
        if (component.minimum_arity != used.minimum_arity ||
            component.maximum_arity != used.maximum_arity) {
            return report(lowering, overriding,
                          SXML_BYTE_VIEW_FROM_LITERAL(
                              "XTSE3070 an overriding function has another arity range (XSLT "
                              "4.0, 3.4.5.3)"));
        }
        if (!is_signature_compatible(lowering, (signature_pair){.overriding = overriding,
                                                                .overridden = used.declaration})) {
            return report(lowering, overriding,
                          SXML_BYTE_VIEW_FROM_LITERAL(
                              "XTSE3070 an overriding declaration's signature is not compatible "
                              "with the overridden one's (XSLT 4.0, 3.4.5.3)"));
        }
        const sxml_xslt_visibility declared = visibility_of_keyword(
            keyword_of(lowering, overriding, SXML_XSLT_AST_ATTRIBUTE_VISIBILITY));
        const sxml_status status = add_instance(
            lowering,
            (component_instance){.component_kind = component.component_kind,
                                 .component_name = component.component_name,
                                 .minimum_arity = component.minimum_arity,
                                 .maximum_arity = component.maximum_arity,
                                 .declaration = overriding,
                                 .origin = overriding,
                                 .package = request.package,
                                 .visibility = declared == SXML_XSLT_VISIBILITY_NONE
                                                   ? SXML_XSLT_VISIBILITY_PRIVATE
                                                   : declared,
                                 .import_precedence = precedence_of(lowering, overriding),
                                 .is_accepted = false,
                                 .is_overriding = true,
                                 .overridden_instance = request.used_instance,
                                 .successor = 0},
            &index);
        if (status == SXML_STATUS_OK) {
            lowering->packages->instances[request.used_instance].successor = index;
        }
        return status;
    }
    const bool is_parameter = node_of(lowering, used.declaration)->kind == SXML_XSLT_AST_KIND_PARAM;
    sxml_xslt_visibility visibility = is_parameter ? SXML_XSLT_VISIBILITY_PUBLIC
                                      : used.visibility == SXML_XSLT_VISIBILITY_PUBLIC ||
                                              used.visibility == SXML_XSLT_VISIBILITY_FINAL
                                          ? SXML_XSLT_VISIBILITY_PRIVATE
                                          : SXML_XSLT_VISIBILITY_HIDDEN;
    if (!is_parameter) {
        sxml_xslt_ast_node_index conflict = {.value = 0};
        const element_choice accepted =
            choose_element(lowering,
                           &(element_search){.parent = request.use_node,
                                             .element_kind = SXML_XSLT_AST_KIND_ACCEPT,
                                             .component = component_of_instance(&used),
                                             .used_visibility = used.visibility},
                           &conflict);
        if (conflict.value != 0 && accepted.strength != SXML_MATCH_STRENGTH_EXPLICIT) {
            return report(lowering, conflict,
                          SXML_BYTE_VIEW_FROM_LITERAL(
                              "XTSE3040 xsl:accept gives a component a visibility its "
                              "visibility in the used package does not allow (XSLT 4.0, "
                              "3.4.5.2)"));
        }
        if (accepted.strength != SXML_MATCH_STRENGTH_NONE) {
            visibility = accepted.visibility;
        }
    }
    if (visibility == SXML_XSLT_VISIBILITY_HIDDEN) {
        return SXML_STATUS_OK;
    }
    const sxml_status status =
        add_instance(lowering,
                     (component_instance){.component_kind = used.component_kind,
                                          .component_name = used.component_name,
                                          .minimum_arity = used.minimum_arity,
                                          .maximum_arity = used.maximum_arity,
                                          .declaration = used.declaration,
                                          .origin = request.use_node,
                                          .package = request.package,
                                          .visibility = visibility,
                                          .import_precedence = 0,
                                          .is_accepted = true,
                                          .is_overriding = false,
                                          .overridden_instance = 0,
                                          .successor = 0},
                     &index);
    if (status == SXML_STATUS_OK) {
        lowering->packages->instances[request.used_instance].successor = index;
    }
    return status;
}

/* The package whose xsl:use-package is use_node; 0 when none was read. */
static uint32_t package_used_by(const sxml_xslt_package_table *table,
                                sxml_xslt_ast_node_index use_node) {
    for (uint32_t package = 2; package < table->package_count; package += 1) {
        if (table->packages[package].use_node.value == use_node.value) {
            return package;
        }
    }
    return 0;
}

/* A template rule among the overriding declarations of an
   xsl:use-package of package used. */
typedef struct overriding_rule {
    sxml_xslt_ast_node_index rule;
    uint32_t used;
} overriding_rule;

/* XTSE3060 (3.4.5.3, 3.4.6): a template rule in xsl:override adds rules
   to modes of the used package, which must be public there (a final mode
   takes no new rules; a private one is not visible). */
static sxml_status check_overriding_modes(const sxml_xslt_lowering *lowering,
                                          overriding_rule request) {
    const sxml_xslt_package_table *table = lowering->packages;
    const package_entry *package = &table->packages[request.used];
    if (node_of(lowering, request.rule)->kind != SXML_XSLT_AST_KIND_TEMPLATE ||
        find_attribute(lowering, request.rule, SXML_XSLT_AST_ATTRIBUTE_MATCH)->kind ==
            SXML_XSLT_AST_ATTRIBUTE_NONE) {
        return SXML_STATUS_OK;
    }
    const sxml_xslt_ast_attribute *modes =
        find_attribute(lowering, request.rule, SXML_XSLT_AST_ATTRIBUTE_MODE);
    if (modes->value.kind != SXML_XSLT_AST_VALUE_KIND_NAME_LIST) {
        return SXML_STATUS_OK;
    }
    const sxml_xslt_ast_name_token_view tokens =
        sxml_xslt_ast_get_name_tokens(lowering->ast, modes->value.payload.name_tokens);
    for (size_t token_index = 0; token_index < tokens.name_token_count; token_index += 1) {
        const sxml_xslt_ast_name_token *token = &tokens.name_tokens[token_index];
        if (token->kind != SXML_XSLT_AST_NAME_TOKEN_KIND_NAME) {
            continue;
        }
        for (uint32_t offset = 0; offset < package->instance_count; offset += 1) {
            const component_instance *instance =
                &table->instances[package->first_instance + offset];
            if (instance->component_kind == SXML_XSLT_COMPONENT_KIND_MODE &&
                instance->component_name.index == token->name.index &&
                instance->visibility != SXML_XSLT_VISIBILITY_PUBLIC) {
                return report(lowering, request.rule,
                              SXML_BYTE_VIEW_FROM_LITERAL(
                                  "XTSE3060 a template rule in xsl:override adds to a mode the "
                                  "used package does not make public (XSLT 4.0, 3.4.6)"));
            }
        }
    }
    return SXML_STATUS_OK;
}

/* XTSE3051: an explicit xsl:accept token naming an overriding declaration
   of the same xsl:use-package; XTSE3058: an overriding named component
   no instance of the used package matches. */
static sxml_status check_overrides(const sxml_xslt_lowering *lowering,
                                   sxml_xslt_ast_node_index use_node, uint32_t used) {
    const sxml_xslt_package_table *table = lowering->packages;
    const package_entry *package = &table->packages[used];
    for (sxml_xslt_ast_node_index overriding =
             next_overriding(lowering, use_node, (sxml_xslt_ast_node_index){.value = 0});
         overriding.value != 0; overriding = next_overriding(lowering, use_node, overriding)) {
        const sxml_status mode_status =
            check_overriding_modes(lowering, (overriding_rule){.rule = overriding, .used = used});
        if (mode_status != SXML_STATUS_OK) {
            return mode_status;
        }
        const declared_component component = component_of(lowering, overriding);
        if (component.component_kind == SXML_XSLT_COMPONENT_KIND_NONE) {
            continue;
        }
        bool is_matched = false;
        for (uint32_t offset = 0; offset < package->instance_count; offset += 1) {
            const component_instance *instance =
                &table->instances[package->first_instance + offset];
            is_matched = is_matched || (instance->successor != 0 &&
                                        table->instances[instance->successor].declaration.value ==
                                            overriding.value);
        }
        if (!is_matched) {
            return report(lowering, overriding,
                          SXML_BYTE_VIEW_FROM_LITERAL(
                              "XTSE3058 an overriding declaration matches no component of the "
                              "used package (XSLT 4.0, 3.4.5.3)"));
        }
        for (sxml_xslt_ast_node_index child = node_of(lowering, use_node)->first_child;
             child.value != 0; child = node_of(lowering, child)->next_sibling) {
            if (node_of(lowering, child)->kind != SXML_XSLT_AST_KIND_ACCEPT) {
                continue;
            }
            if (match_element(lowering, child, component) == SXML_MATCH_STRENGTH_EXPLICIT) {
                return report(lowering, child,
                              SXML_BYTE_VIEW_FROM_LITERAL(
                                  "XTSE3051 xsl:accept names a component an overriding "
                                  "declaration of the same xsl:use-package declares (XSLT 4.0, "
                                  "3.4.5.2)"));
            }
        }
    }
    return SXML_STATUS_OK;
}

/* XTSE3050 (3.4.6): a template rule of package outside xsl:override that
   names a mode package accepts from a used package (with
   declared-modes="no" it would declare a second mode of that name; new
   rules for such a mode belong in xsl:override). */
static sxml_status check_rules_in_accepted_modes(const sxml_xslt_lowering *lowering,
                                                 uint32_t package) {
    const sxml_xslt_package_table *table = lowering->packages;
    const sxml_xslt_ast *ast = lowering->ast;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index rule = sxml_xslt_ast_get_declaration(ast, order);
        const sxml_xslt_ast_node_index parent = node_of(lowering, rule)->parent;
        if (node_of(lowering, rule)->kind != SXML_XSLT_AST_KIND_TEMPLATE ||
            (parent.value != 0 && node_of(lowering, parent)->kind == SXML_XSLT_AST_KIND_OVERRIDE) ||
            sxml_xslt_package_of_node(lowering, rule) != package ||
            find_attribute(lowering, rule, SXML_XSLT_AST_ATTRIBUTE_MATCH)->kind ==
                SXML_XSLT_AST_ATTRIBUTE_NONE) {
            continue;
        }
        const sxml_xslt_ast_attribute *modes =
            find_attribute(lowering, rule, SXML_XSLT_AST_ATTRIBUTE_MODE);
        if (modes->value.kind != SXML_XSLT_AST_VALUE_KIND_NAME_LIST) {
            continue;
        }
        const sxml_xslt_ast_name_token_view tokens =
            sxml_xslt_ast_get_name_tokens(ast, modes->value.payload.name_tokens);
        for (size_t token_index = 0; token_index < tokens.name_token_count; token_index += 1) {
            const sxml_xslt_ast_name_token *token = &tokens.name_tokens[token_index];
            for (uint32_t index = 1;
                 token->kind == SXML_XSLT_AST_NAME_TOKEN_KIND_NAME && index < table->instance_count;
                 index += 1) {
                const component_instance *instance = &table->instances[index];
                if (instance->package == package && instance->is_accepted &&
                    instance->component_kind == SXML_XSLT_COMPONENT_KIND_MODE &&
                    instance->component_name.index == token->name.index) {
                    return report(lowering, rule,
                                  SXML_BYTE_VIEW_FROM_LITERAL(
                                      "XTSE3050 a template rule outside xsl:override names a mode "
                                      "accepted from a used package (XSLT 4.0, 3.4.6)"));
                }
            }
        }
    }
    return SXML_STATUS_OK;
}

static sxml_status add_used_instances(const sxml_xslt_lowering *lowering, uint32_t package) {
    sxml_xslt_package_table *table = lowering->packages;
    const sxml_xslt_ast *ast = lowering->ast;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t order = 1;
         status == SXML_STATUS_OK && sxml_xslt_ast_get_declaration(ast, order).value != 0;
         order += 1) {
        const sxml_xslt_ast_node_index use_node = sxml_xslt_ast_get_declaration(ast, order);
        if (node_of(lowering, use_node)->kind != SXML_XSLT_AST_KIND_USE_PACKAGE ||
            sxml_xslt_package_of_node(lowering, use_node) != package) {
            continue;
        }
        const uint32_t used = package_used_by(table, use_node);
        if (used == 0) {
            continue;
        }
        status = check_used_package(lowering, used);
        const package_entry used_package = table->packages[used];
        for (sxml_xslt_ast_node_index child = node_of(lowering, use_node)->first_child;
             status == SXML_STATUS_OK && child.value != 0;
             child = node_of(lowering, child)->next_sibling) {
            if (node_of(lowering, child)->kind == SXML_XSLT_AST_KIND_ACCEPT) {
                status = check_tests(lowering,
                                     (test_check){.element = child,
                                                  .first_instance = used_package.first_instance,
                                                  .instance_count = used_package.instance_count});
            }
        }
        for (uint32_t offset = 0; status == SXML_STATUS_OK && offset < used_package.instance_count;
             offset += 1) {
            status = add_used_instance(
                lowering, (use_request){.package = package,
                                        .use_node = use_node,
                                        .used_instance = used_package.first_instance + offset});
        }
        if (status == SXML_STATUS_OK) {
            status = check_overrides(lowering, use_node, used);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = check_rules_in_accepted_modes(lowering, package);
    }
    return status;
}

/* XTSE3050 (3.4.5.2): homonymous instances of one package other than two
   own declarations (import precedence decides those); XTSE3055 (3.4.5.3):
   an overriding declaration homonymous with another declaration. */
static sxml_status check_conflicts(const sxml_xslt_lowering *lowering, uint32_t package) {
    const sxml_xslt_package_table *table = lowering->packages;
    const package_entry *entry = &table->packages[package];
    for (uint32_t first = 0; first < entry->instance_count; first += 1) {
        const component_instance *one = &table->instances[entry->first_instance + first];
        for (uint32_t second = first + 1U; second < entry->instance_count; second += 1) {
            const component_instance *other = &table->instances[entry->first_instance + second];
            if (!is_homonymous(one, component_of_instance(other))) {
                continue;
            }
            const bool is_own_pair = !one->is_accepted && !one->is_overriding &&
                                     !other->is_accepted && !other->is_overriding;
            if (is_own_pair) {
                continue;
            }
            if (one->is_overriding || other->is_overriding) {
                return report(lowering, one->is_overriding ? one->declaration : other->declaration,
                              SXML_BYTE_VIEW_FROM_LITERAL(
                                  "XTSE3055 an overriding declaration has the name of another "
                                  "declaration of the package (XSLT 4.0, 3.4.5.3)"));
            }
            return report(lowering, other->is_accepted ? other->origin : one->origin,
                          SXML_BYTE_VIEW_FROM_LITERAL(
                              "XTSE3050 two components of one name are accepted from used "
                              "packages, or one is accepted and one declared (XSLT 4.0, "
                              "3.4.5.2)"));
        }
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------- Entry */

sxml_status sxml_xslt_declare_packages(sxml_xslt_lowering *lowering) {
    if (lowering->packages != NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    void *memory = NULL;
    sxml_status status =
        allocate_zeroed(lowering,
                        (array_request){.element_count = 1,
                                        .element_size = sizeof(sxml_xslt_package_table),
                                        .element_alignment = alignof(sxml_xslt_package_table)},
                        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    lowering->packages = memory;
    sxml_xslt_package_table *table = lowering->packages;
    status = find_packages(lowering);
    if (status == SXML_STATUS_OK) {
        status = map_static_contexts(lowering);
    }
    /* 3.4.1: every package-version is a version (XTSE0020). */
    for (uint32_t package = 1; status == SXML_STATUS_OK && package < table->package_count;
         package += 1) {
        const sxml_xslt_ast_node_index root = table->packages[package].root;
        sxml_xslt_package_version_match match = {.matches = false};
        if (has_attribute(lowering, root, SXML_XSLT_AST_ATTRIBUTE_PACKAGE_VERSION) &&
            sxml_xslt_package_version_matches(
                (sxml_xslt_package_version_query){
                    .package_version =
                        text_of(lowering, root, SXML_XSLT_AST_ATTRIBUTE_PACKAGE_VERSION),
                    .version_range = SXML_BYTE_VIEW_FROM_LITERAL("*")},
                &match) != SXML_STATUS_OK) {
            status = report(lowering, root,
                            SXML_BYTE_VIEW_FROM_LITERAL(
                                "XTSE0020 the package-version of an xsl:package is no version "
                                "(XSLT 4.0, 3.4.1)"));
        }
    }
    /* Used packages come after their users: the last first. */
    for (uint32_t package = table->package_count - 1U;
         status == SXML_STATUS_OK && package >= 1U && package < table->package_count;
         package -= 1) {
        status = add_own_instances(lowering, package);
        if (status == SXML_STATUS_OK) {
            status = add_used_instances(lowering, package);
        }
        const uint32_t end = table->instance_count == 0 ? 1U : table->instance_count;
        table->packages[package].instance_count = end - table->packages[package].first_instance;
        if (status == SXML_STATUS_OK) {
            status = check_conflicts(lowering, package);
        }
    }
    /* XTSE3080 (3.4.5.5): the top-level package is executable. */
    if (status == SXML_STATUS_OK && table->package_count > 1U) {
        const package_entry *top = &table->packages[1];
        for (uint32_t offset = 0; offset < top->instance_count; offset += 1) {
            const component_instance *instance = &table->instances[top->first_instance + offset];
            if (instance->visibility == SXML_XSLT_VISIBILITY_ABSTRACT) {
                return report(lowering, instance->origin,
                              SXML_BYTE_VIEW_FROM_LITERAL(
                                  "XTSE3080 the top-level package has an abstract component "
                                  "(XSLT 4.0, 3.4.5.5)"));
            }
        }
    }
    return status;
}

void sxml_xslt_release_packages(sxml_xslt_lowering *lowering) {
    sxml_xslt_package_table *table = lowering->packages;
    if (table == NULL) {
        return;
    }
    for (uint32_t kept_index = 0; kept_index < table->kept_count; kept_index += 1) {
        sxml_xpath_lowerer_release_scratch(lowering->lowerer, table->kept_memory[kept_index]);
    }
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, (void *)table->kept_memory);
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, table->instances);
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, table->context_overrides);
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, table->context_packages);
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, table->packages);
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, table->module_packages);
    sxml_xpath_lowerer_release_scratch(lowering->lowerer, table);
    lowering->packages = NULL;
}

/* ------------------------------------------------------------ Binding */

/* The instance a reference from its package names; 0 for none. */
static uint32_t find_instance(const sxml_xslt_package_table *table,
                              const sxml_xslt_component_reference *reference) {
    if (reference->package == 0 || reference->package >= table->package_count) {
        return 0;
    }
    const package_entry *package = &table->packages[reference->package];
    uint32_t found = 0;
    for (uint32_t offset = 0; offset < package->instance_count; offset += 1) {
        const uint32_t index = package->first_instance + offset;
        const component_instance *instance = &table->instances[index];
        if (instance->component_kind != reference->component_kind ||
            instance->component_name.index != reference->component_name.index ||
            (reference->component_kind == SXML_XSLT_COMPONENT_KIND_FUNCTION &&
             (reference->arity < instance->minimum_arity ||
              reference->arity > instance->maximum_arity))) {
            continue;
        }
        if (found == 0 || instance->import_precedence > table->instances[found].import_precedence) {
            found = index;
        }
    }
    return found;
}

static sxml_xslt_component_binding binding_of(const sxml_xslt_lowering *lowering,
                                              sxml_xslt_ast_node_index declaration) {
    return (sxml_xslt_component_binding){
        .declaration = declaration,
        .package = sxml_xslt_package_of_node(lowering, declaration),
        .is_abstract = sxml_xslt_package_is_abstract(lowering, declaration)};
}

sxml_xslt_component_binding sxml_xslt_package_bind(const sxml_xslt_lowering *lowering,
                                                   const sxml_xslt_component_reference *reference) {
    const sxml_xslt_package_table *table = lowering->packages;
    const sxml_xslt_component_binding none = {
        .declaration = {.value = 0}, .package = reference->package, .is_abstract = false};
    if (table == NULL) {
        return none;
    }
    uint32_t index = find_instance(table, reference);
    if (index == 0) {
        return none;
    }
    /* 3.4.5.5: the successors in the using packages, at most one per
       package. */
    for (uint32_t step_count = 0;
         step_count < table->package_count && table->instances[index].successor != 0;
         step_count += 1) {
        index = table->instances[index].successor;
    }
    return binding_of(lowering, table->instances[index].declaration);
}

sxml_xslt_visibility
sxml_xslt_package_get_reference_visibility(const sxml_xslt_lowering *lowering,
                                           const sxml_xslt_component_reference *reference) {
    const sxml_xslt_package_table *table = lowering->packages;
    if (table == NULL) {
        return SXML_XSLT_VISIBILITY_NONE;
    }
    const uint32_t index = find_instance(table, reference);
    return index == 0 ? SXML_XSLT_VISIBILITY_NONE : table->instances[index].visibility;
}

sxml_xslt_component_binding sxml_xslt_package_bind_original(const sxml_xslt_lowering *lowering,
                                                            sxml_xslt_ast_node_index overriding) {
    const sxml_xslt_package_table *table = lowering->packages;
    const sxml_xslt_component_binding none = {
        .declaration = {.value = 0}, .package = 0, .is_abstract = false};
    if (table == NULL || overriding.value == 0) {
        return none;
    }
    for (uint32_t index = 1; index < table->instance_count; index += 1) {
        const component_instance *instance = &table->instances[index];
        if (instance->is_overriding && instance->declaration.value == overriding.value) {
            const component_instance *overridden = &table->instances[instance->overridden_instance];
            sxml_xslt_component_binding binding = binding_of(lowering, overridden->declaration);
            binding.is_abstract = overridden->visibility == SXML_XSLT_VISIBILITY_ABSTRACT;
            return binding;
        }
    }
    return none;
}

sxml_xslt_visibility sxml_xslt_package_get_visibility(const sxml_xslt_lowering *lowering,
                                                      sxml_xslt_ast_node_index declaration) {
    const sxml_xslt_package_table *table = lowering->packages;
    if (table == NULL) {
        return SXML_XSLT_VISIBILITY_NONE;
    }
    const uint32_t package = sxml_xslt_package_of_node(lowering, declaration);
    for (uint32_t index = 1; index < table->instance_count; index += 1) {
        const component_instance *instance = &table->instances[index];
        if (instance->package == package && !instance->is_accepted &&
            instance->declaration.value == declaration.value) {
            return instance->visibility;
        }
    }
    return SXML_XSLT_VISIBILITY_NONE;
}

bool sxml_xslt_package_is_abstract(const sxml_xslt_lowering *lowering,
                                   sxml_xslt_ast_node_index declaration) {
    return keyword_of(lowering, declaration, SXML_XSLT_AST_ATTRIBUTE_VISIBILITY) ==
           SXML_XSLT_AST_KEYWORD_ABSTRACT;
}
