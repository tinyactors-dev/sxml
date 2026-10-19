/*
 * XSLT 4.0 named types (src/xslt_lower_internal.h; XSLT 4.0, section 5.4;
 * docs/DECISIONS.md D98; packet 4.19): xsl:item-type gives an item type a
 * name (5.4.1), xsl:record-type declares a nominative record type (5.4.2).
 *
 * Golden file: src/xquery_static.c (declare_records, read_item_types,
 * define_records: XQuery's "declare type" and "declare record" are the same
 * two mechanisms). A named item type is an entry of the XPath static
 * context's named item types (an alias of a registered item type, D98),
 * found by name before the type registry's named types (XPath 4.0,
 * 3.2.3); a record type is a nominative record type of the type registry,
 * declared first so that every type may name it (records may be
 * recursive), its fields defined last.
 *
 * Scope (5.4.1): a package. Both declarations are components of kind
 * item type (src/xslt_package_internal.h), so a package sees its own and
 * the public ones of the packages it uses that it accepts, by the binding
 * rules of 3.4.5.5 (xsl:expose, xsl:accept, visibility); each package's
 * entries are kept by the package table and every static context of the
 * package reads them. A type is resolved in its own package's view, in
 * rounds, so declarations may come in any order: a round resolves every
 * declaration whose type names only resolved ones; when a round makes no
 * progress, the first one left reports its error (XPST0051 for a name
 * never declared, XTSE4035 for a cycle, 5.4.1).
 *
 * Static errors: XTSE4030 (two declarations of one name in one package
 * with the same import precedence and none higher), XTSE4035, XTSE4050
 * (two fields of a record of one name), and those of resolving the types.
 *
 * The constructor function of constructor="yes" (5.4.2.2) is the implicit
 * xsl:function the reader synthesizes beside the declaration
 * (src/xslt_read.c, add_record_constructor). Not done (delayed, D162):
 * the initial values of fields in coercions and casts.
 */
#include "xslt_lower_internal.h"

#include "memory_internal.h"

#include "sxml/name.h"
#include "sxml/types.h"
#include "xslt_package_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>

/* One xsl:item-type or xsl:record-type. */
typedef struct type_declaration {
    sxml_xslt_ast_node_index node;
    sxml_name_identifier type_name;
    uint32_t package;
    sxml_type_identifier item_type;
    bool is_resolved;
    /* An xsl:record-type whose fields this lowering defines (a record
       declared by an earlier compilation for the item heap keeps its
       fields: the registry cannot redefine a type, as in XQuery). */
    bool defines_fields;
    /* Not resolved after the rounds (for the error). */
    bool is_pending;
} type_declaration;

/* What reading the named types works on. */
typedef struct named_type_reading {
    const sxml_xslt_lowering *lowering;
    type_declaration *declarations;
    size_t declaration_count;
    /* Row p (package p, from 1) of package_count + 1 rows of
       declaration_count entries each, kept by the package table. */
    sxml_xpath_named_item_type *entries;
} named_type_reading;

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
    return attribute->value.kind == SXML_XSLT_AST_VALUE_KIND_NAME
               ? attribute->value.payload.name
               : (sxml_name_identifier){.index = 0};
}

static sxml_status report(const sxml_xslt_lowering *lowering, sxml_xslt_ast_node_index node,
                          sxml_byte_view code_and_description) {
    return sxml_xslt_lowering_report_static_error(
        lowering,
        (sxml_xslt_static_error){.node = node, .code_and_description = code_and_description});
}

static bool is_type_declaration(const sxml_xslt_lowering *lowering,
                                sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_kind kind = node_of(lowering, node)->kind;
    return (kind == SXML_XSLT_AST_KIND_ITEM_TYPE || kind == SXML_XSLT_AST_KIND_RECORD_TYPE) &&
           name_of(lowering, node).index != 0;
}

/* ------------------------------------------------------------ Views */

/* The entries package sees (row package): each resolved declaration its
   references to the declaration's name bind to (3.4.5.5). */
static size_t build_view(const named_type_reading *reading, uint32_t package) {
    sxml_xpath_named_item_type *row = reading->entries + (size_t)package * reading->declaration_count;
    size_t entry_count = 0;
    for (size_t declaration_index = 0; declaration_index < reading->declaration_count;
         declaration_index += 1) {
        const type_declaration *declaration = &reading->declarations[declaration_index];
        if (!declaration->is_resolved) {
            continue;
        }
        const sxml_xslt_component_binding binding = sxml_xslt_package_bind(
            reading->lowering,
            &(sxml_xslt_component_reference){.component_kind = SXML_XSLT_COMPONENT_KIND_ITEM_TYPE,
                                             .component_name = declaration->type_name,
                                             .arity = 0,
                                             .package = package});
        if (binding.declaration.value == declaration->node.value) {
            row[entry_count] = (sxml_xpath_named_item_type){.type_name = declaration->type_name,
                                                            .item_type = declaration->item_type};
            entry_count += 1;
        }
    }
    return entry_count;
}

static sxml_xpath_named_item_type_view view_of(const named_type_reading *reading,
                                               uint32_t package) {
    const size_t entry_count = build_view(reading, package);
    return (sxml_xpath_named_item_type_view){
        .named_item_types =
            entry_count == 0 ? NULL : reading->entries + (size_t)package * reading->declaration_count,
        .named_item_type_count = entry_count};
}

/* -------------------------------------------------------- Declarations */

static sxml_status collect_declarations(named_type_reading *reading) {
    const sxml_xslt_lowering *lowering = reading->lowering;
    const sxml_xslt_ast *ast = lowering->ast;
    size_t declaration_count = 0;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        declaration_count +=
            is_type_declaration(lowering, sxml_xslt_ast_get_declaration(ast, order)) ? 1U : 0U;
    }
    if (declaration_count == 0) {
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        lowering->lowerer->allocator,
        (sxml_array_allocation_request){.element_count = declaration_count,
                                        .element_size = sizeof(type_declaration),
                                        .element_alignment = alignof(type_declaration)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    reading->declarations = memory;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index node = sxml_xslt_ast_get_declaration(ast, order);
        if (!is_type_declaration(lowering, node)) {
            continue;
        }
        reading->declarations[reading->declaration_count] =
            (type_declaration){.node = node,
                               .type_name = name_of(lowering, node),
                               .package = sxml_xslt_package_of_node(lowering, node),
                               .item_type = {.index = 0},
                               .is_resolved = false,
                               .defines_fields = false,
                               .is_pending = false};
        reading->declaration_count += 1;
    }
    /* Package rows 0 to package_count, kept until the packages go. */
    const size_t row_count = (size_t)sxml_xslt_package_count(lowering) + 1U;
    size_t entry_count = 0;
    status = sxml_size_multiply(row_count, reading->declaration_count, &entry_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_package_allocate(
            lowering,
            (sxml_xslt_package_memory_request){.element_count = entry_count,
                                               .element_size = sizeof(sxml_xpath_named_item_type),
                                               .element_alignment =
                                                   alignof(sxml_xpath_named_item_type)},
            &memory);
        reading->entries = memory;
    }
    return status;
}

/* XTSE4030 (5.4.1): two declarations of one name in one package with one
   import precedence, when no declaration of the name has a higher one. */
static sxml_status check_duplicates(const named_type_reading *reading) {
    const sxml_xslt_lowering *lowering = reading->lowering;
    for (size_t later = 1; later < reading->declaration_count; later += 1) {
        const type_declaration *declaration = &reading->declarations[later];
        const uint32_t precedence =
            sxml_xslt_ast_get_module(lowering->ast, node_of(lowering, declaration->node)->module)
                ->import_precedence;
        bool has_twin = false;
        bool has_higher = false;
        for (size_t other_index = 0; other_index < reading->declaration_count;
             other_index += 1) {
            const type_declaration *other = &reading->declarations[other_index];
            if (other_index == later || other->package != declaration->package ||
                other->type_name.index != declaration->type_name.index) {
                continue;
            }
            const uint32_t other_precedence =
                sxml_xslt_ast_get_module(lowering->ast, node_of(lowering, other->node)->module)
                    ->import_precedence;
            has_twin = has_twin || (other_index < later && other_precedence == precedence);
            has_higher = has_higher || other_precedence > precedence;
        }
        if (has_twin && !has_higher) {
            return report(lowering, declaration->node,
                          SXML_BYTE_VIEW_FROM_LITERAL(
                              "XTSE4030 two named item or record types of one name and import "
                              "precedence (XSLT 4.0, 5.4.1)"));
        }
    }
    return SXML_STATUS_OK;
}

/* Declares each record type in the type registry, so every type may name
   it (5.4.2.1, XPath 4.0, 3.2.10.2.1). */
static sxml_status declare_records(const named_type_reading *reading) {
    sxml_type_registry *registry = reading->lowering->lowerer->type_registry;
    sxml_status status = SXML_STATUS_OK;
    for (size_t declaration_index = 0;
         status == SXML_STATUS_OK && declaration_index < reading->declaration_count;
         declaration_index += 1) {
        type_declaration *declaration = &reading->declarations[declaration_index];
        if (node_of(reading->lowering, declaration->node)->kind != SXML_XSLT_AST_KIND_RECORD_TYPE) {
            continue;
        }
        declaration->is_resolved = true;
        if (!sxml_type_registry_find_type(registry, declaration->type_name,
                                          &declaration->item_type)) {
            declaration->defines_fields = true;
            status = sxml_type_registry_declare_nominative_record_type(
                registry, declaration->type_name, &declaration->item_type);
        }
    }
    return status;
}

/* Resolves the as attribute of an xsl:item-type in its package's view
   (item() without one, 5.4.1). error_record NULL tries quietly. */
typedef struct alias_resolution {
    const named_type_reading *reading;
    type_declaration *declaration;
    sxml_error_record *error_record;
} alias_resolution;

static sxml_status resolve_alias(alias_resolution resolution) {
    const sxml_xslt_lowering *lowering = resolution.reading->lowering;
    type_declaration *declaration = resolution.declaration;
    const sxml_xslt_ast_attribute *as_attribute =
        find_attribute(lowering, declaration->node, SXML_XSLT_AST_ATTRIBUTE_AS);
    if (as_attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_ITEM_TYPE) {
        declaration->item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM);
        return SXML_STATUS_OK;
    }
    const sxml_xslt_ast_expression *expression =
        sxml_xslt_ast_get_expression(lowering->ast, as_attribute->value.payload.expression);
    const sxml_xpath_static_context *context =
        sxml_xslt_lowering_expression_context(lowering, as_attribute->value.payload.expression);
    if (expression->ast == NULL || context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_xpath_static_context in_package = *context;
    in_package.named_item_types = view_of(resolution.reading, declaration->package);
    /* The reader checked the occurrence to be one (D139). */
    sxml_sequence_type sequence_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                                        .occurrence = SXML_OCCURRENCE_EXACTLY_ONE};
    const sxml_status status = sxml_xpath_resolve_sequence_type(
        &(sxml_xpath_type_request){.ast = expression->ast,
                                   .type_node = expression->ast->root,
                                   .static_context = &in_package,
                                   .error_record = resolution.error_record},
        &sequence_type);
    declaration->item_type = sequence_type.item_type;
    return status;
}

/* The named item types in rounds (see the file comment). */
static sxml_status resolve_aliases(const named_type_reading *reading) {
    bool has_progress = true;
    bool has_pending = true;
    while (has_progress && has_pending) {
        has_progress = false;
        has_pending = false;
        for (size_t declaration_index = 0; declaration_index < reading->declaration_count;
             declaration_index += 1) {
            type_declaration *declaration = &reading->declarations[declaration_index];
            if (declaration->is_resolved) {
                continue;
            }
            const sxml_status status = resolve_alias((alias_resolution){
                .reading = reading, .declaration = declaration, .error_record = NULL});
            if (status == SXML_STATUS_OK) {
                declaration->is_resolved = true;
                has_progress = true;
            } else if (status == SXML_STATUS_STATIC_ERROR) {
                has_pending = true;
            } else {
                return status;
            }
        }
    }
    if (!has_pending) {
        return SXML_STATUS_OK;
    }
    /* What is left either names a type nobody declares (some declaration
       then fails even when every one left counts as item()) or is in a
       cycle (XTSE4035): the first that fails so reports its XPST0051. */
    for (size_t declaration_index = 0; declaration_index < reading->declaration_count;
         declaration_index += 1) {
        type_declaration *declaration = &reading->declarations[declaration_index];
        declaration->is_pending = !declaration->is_resolved;
        if (declaration->is_pending) {
            declaration->is_resolved = true;
            declaration->item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM);
        }
    }
    type_declaration *first_pending = NULL;
    type_declaration *undeclared = NULL;
    for (size_t declaration_index = 0;
         undeclared == NULL && declaration_index < reading->declaration_count;
         declaration_index += 1) {
        type_declaration *declaration = &reading->declarations[declaration_index];
        if (!declaration->is_pending) {
            continue;
        }
        first_pending = first_pending == NULL ? declaration : first_pending;
        const sxml_status status = resolve_alias(
            (alias_resolution){.reading = reading, .declaration = declaration, .error_record = NULL});
        if (status == SXML_STATUS_STATIC_ERROR) {
            undeclared = declaration;
        } else if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    for (size_t declaration_index = 0; declaration_index < reading->declaration_count;
         declaration_index += 1) {
        type_declaration *declaration = &reading->declarations[declaration_index];
        declaration->is_resolved = declaration->is_resolved && !declaration->is_pending;
    }
    if (undeclared == NULL) {
        return report(reading->lowering, first_pending->node,
                      SXML_BYTE_VIEW_FROM_LITERAL(
                          "XTSE4035 a named item type refers to itself (XSLT 4.0, 5.4.1)"));
    }
    const sxml_status status =
        resolve_alias((alias_resolution){.reading = reading,
                                         .declaration = undeclared,
                                         .error_record = reading->lowering->error_record});
    return status == SXML_STATUS_OK ? SXML_STATUS_INVALID_ARGUMENT : status;
}

/* The local name of a FIELD's name attribute (an NCName). */
static sxml_status field_name_of(const sxml_xslt_lowering *lowering,
                                 sxml_xslt_ast_node_index field, sxml_byte_view *field_name) {
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    const sxml_status status = sxml_name_pool_get_name_parts(lowering->lowerer->name_pool,
                                                             name_of(lowering, field), &parts);
    *field_name = parts.local_name;
    return status;
}

/* Gives each record type this lowering declared its fields (5.4.2.1), in
   the static contexts that see the named types; XTSE4050. */
static sxml_status define_records(const named_type_reading *reading) {
    const sxml_xslt_lowering *lowering = reading->lowering;
    sxml_status status = SXML_STATUS_OK;
    for (size_t declaration_index = 0;
         status == SXML_STATUS_OK && declaration_index < reading->declaration_count;
         declaration_index += 1) {
        const type_declaration *declaration = &reading->declarations[declaration_index];
        if (!declaration->defines_fields) {
            continue;
        }
        size_t field_count = 0;
        for (sxml_xslt_ast_node_index field = node_of(lowering, declaration->node)->first_child;
             field.value != 0; field = node_of(lowering, field)->next_sibling) {
            field_count += node_of(lowering, field)->kind == SXML_XSLT_AST_KIND_FIELD ? 1U : 0U;
        }
        void *memory = NULL;
        status = sxml_allocator_allocate_array(
            lowering->lowerer->allocator,
            (sxml_array_allocation_request){.element_count = field_count,
                                            .element_size = sizeof(sxml_record_field),
                                            .element_alignment = alignof(sxml_record_field)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_record_field *fields = memory;
        size_t field_index = 0;
        for (sxml_xslt_ast_node_index field = node_of(lowering, declaration->node)->first_child;
             status == SXML_STATUS_OK && field.value != 0;
             field = node_of(lowering, field)->next_sibling) {
            if (node_of(lowering, field)->kind != SXML_XSLT_AST_KIND_FIELD) {
                continue;
            }
            status = field_name_of(lowering, field, &fields[field_index].field_name);
            for (size_t earlier = 0; status == SXML_STATUS_OK && earlier < field_index;
                 earlier += 1) {
                if (sxml_byte_view_equals(fields[earlier].field_name,
                                          fields[field_index].field_name)) {
                    status = report(lowering, field,
                                    SXML_BYTE_VIEW_FROM_LITERAL(
                                        "XTSE4050 two fields of an xsl:record-type have one name "
                                        "(XSLT 4.0, 5.4.2)"));
                }
            }
            sxml_xslt_declared_type type = {.has_type = false};
            if (status == SXML_STATUS_OK) {
                /* item()* without as (5.4.2.1). */
                status = sxml_xslt_lowering_resolve_declared_type(lowering, field, &type);
            }
            fields[field_index].field_type = type.sequence_type;
            field_index += 1;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_type_registry_define_record_fields(
                lowering->lowerer->type_registry,
                &(sxml_record_definition){
                    .record_type = declaration->item_type,
                    .record_fields = {.record_fields = fields, .record_field_count = field_count}});
        }
        sxml_allocator_release(lowering->lowerer->allocator, memory);
    }
    return status;
}

sxml_status sxml_xslt_declare_named_types(const sxml_xslt_lowering *lowering,
                                          sxml_xpath_static_context *static_contexts) {
    named_type_reading reading = {
        .lowering = lowering, .declarations = NULL, .declaration_count = 0, .entries = NULL};
    sxml_status status = collect_declarations(&reading);
    if (status == SXML_STATUS_OK && reading.declaration_count > 0) {
        status = check_duplicates(&reading);
        if (status == SXML_STATUS_OK) {
            status = declare_records(&reading);
        }
        if (status == SXML_STATUS_OK) {
            status = resolve_aliases(&reading);
        }
        const uint32_t package_count = sxml_xslt_package_count(lowering);
        for (uint32_t package = 1; status == SXML_STATUS_OK && package <= package_count;
             package += 1) {
            /* Row package now holds the package's final view. */
            const sxml_xpath_named_item_type_view view = view_of(&reading, package);
            for (uint32_t context = 1; context < lowering->static_context_count; context += 1) {
                if (sxml_xslt_package_of_static_context(
                        lowering, (sxml_xslt_ast_static_context_index){.value = context}) ==
                    package) {
                    static_contexts[context].named_item_types = view;
                }
            }
        }
        if (status == SXML_STATUS_OK) {
            status = define_records(&reading);
        }
    }
    if (reading.declarations != NULL) {
        sxml_allocator_release(lowering->lowerer->allocator, reading.declarations);
    }
    return status;
}
