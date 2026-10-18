/*
 * The text form of schema components (src/schema_internal.h, "Text form";
 * docs/DECISIONS.md D167, D170): one line per component, in table order
 * and index order, the built-in components left out, for golden tests of
 * every compile stage.
 *
 * Golden file: src/xslt_ast.c (sxml_xslt_ast_write_text: a writer that
 * counts the bytes it needs and writes what fits).
 *
 * The lines (properties a stage has not written print as for a zero
 * entry, so each stage's text shows what it adds):
 *
 *   document N "URI" target-namespace URI|none
 *   context N namespaces (PREFIX = URI, ...) default-namespace URI|none
 *     base-uri "URI"
 *   type N NAME|anonymous VARIETY [abstract] base T [NAME] METHOD
 *     [final (...)] [block (...)] [mixed] [simple-content]
 *     [explicit-content particle P] content VARIETY [particle P]
 *     [simple-type T] [open-content MODE wildcard W] [attribute-uses (...)]
 *     [attribute-groups (reference R, ...)] [attribute-wildcard W]
 *     [default-attributes G] [item T] [members (...)] [primitive T NAME]
 *     [facets (...)] [assertions (...)] [registered] at POSITION
 *   element N NAME global|local [in type T|in group G] [abstract]
 *     [nillable] type T [NAME] [default|fixed "V" context C]
 *     [substitution-group (...)] [members (...)] [alternatives (...)]
 *     [identity-constraints (...)] [final (...)] [block (...)] at POSITION
 *   attribute N NAME global|local [in type T|in attribute-group G] type T
 *     [NAME] [default|fixed "V" context C] [inheritable] at POSITION
 *   attribute-use N optional|required|prohibited attribute D
 *     [NAME (reference R)] [default|fixed "V" context C] [inheritable]
 *   attribute-group N NAME [attribute-uses (...)] [attribute-groups (...)]
 *     [attribute-wildcard W]
 *   group N NAME model-group M
 *   model-group N sequence|choice|all (particle P, ...)
 *   particle N MIN..MAX|unbounded element D|model-group M|wildcard W
 *     [NAME (reference R)]
 *   wildcard N any|(URI, absent, ...)|not (...) [not-names (...)]
 *     [not-defined] [not-sibling] strict|lax|skip
 *   identity-constraint N NAME|anonymous unique|key|keyref
 *     [ref (reference R)] [selector xpath X] [fields (xpath X, ...)]
 *     [refer K (reference R)]
 *   alternative N [test xpath X] type T [NAME]
 *   assertion N test xpath X
 *   notation N NAME [public "P"] [system "S"]
 *   facet N KIND "VALUE" [fixed] context C
 *   xpath N context C "EXPRESSION" [compiled]
 *   reference N SYMBOL-SPACE NAME -> INDEX|none
 *
 * each ending "at LINE:COLUMN" (DOCUMENT:LINE:COLUMN when the schema has
 * more than one document). Names print with the prefix some namespace
 * context binds to their namespace, else as Q{URI}LOCAL.
 */
#include "schema_internal.h"

#include "processor_internal.h"

#include <string.h>

typedef struct text_writer {
    sxml_memory_view output;
    /* Bytes the whole text needs; may exceed the output's byte count. */
    size_t byte_count;
    const sxml_schema_components *components;
    sxml_name_pool *name_pool;
    bool has_several_documents;
} text_writer;

static void write_bytes(text_writer *writer, sxml_byte_view bytes) {
    for (size_t byte_index = 0; byte_index < bytes.byte_count; byte_index += 1) {
        if (writer->byte_count < writer->output.byte_count) {
            writer->output.bytes[writer->byte_count] = bytes.bytes[byte_index];
        }
        writer->byte_count += 1;
    }
}

// ast-grep-ignore: pointer-parameter-without-length
static void write_text(text_writer *writer, const char *text) {
    write_bytes(writer, sxml_byte_view_from_c_string(text));
}

static void write_unsigned(text_writer *writer, uint64_t number) {
    uint8_t digits[20];
    size_t digit_count = 0;
    do {
        digits[sizeof(digits) - 1 - digit_count] = (uint8_t)('0' + (uint8_t)(number % 10U));
        number /= 10U;
        digit_count += 1;
    } while (number != 0);
    write_bytes(writer, (sxml_byte_view){.bytes = digits + sizeof(digits) - digit_count,
                                         .byte_count = digit_count});
}

/* " WORD N". */
typedef struct labelled_index {
    const char *label;
    uint32_t index;
} labelled_index;

static void write_labelled(text_writer *writer, labelled_index item) {
    write_text(writer, " ");
    write_text(writer, item.label);
    write_text(writer, " ");
    write_unsigned(writer, item.index);
}

/* Text in double quotes, quotes and line ends escaped. */
static void write_quoted(text_writer *writer, sxml_byte_view text) {
    write_text(writer, "\"");
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        if (byte == '"') {
            write_text(writer, "\\\"");
        } else if (byte == '\n') {
            write_text(writer, "\\n");
        } else if (byte == '\t') {
            write_text(writer, "\\t");
        } else if (byte == '\r') {
            write_text(writer, "\\r");
        } else {
            write_bytes(writer,
                        (sxml_byte_view){.bytes = text.bytes + byte_index, .byte_count = 1});
        }
    }
    write_text(writer, "\"");
}

static sxml_byte_view namespace_text(const text_writer *writer, sxml_namespace_uri_identifier uri) {
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    if (sxml_name_pool_get_namespace_uri(writer->name_pool, uri, &text) != SXML_STATUS_OK) {
        text = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return text;
}

static void write_namespace(text_writer *writer, sxml_namespace_uri_identifier uri) {
    if (uri.index == SXML_NAMESPACE_NONE) {
        write_text(writer, "none");
        return;
    }
    write_bytes(writer, namespace_text(writer, uri));
}

/* A prefix some namespace context binds to uri: a prefixed binding
   first; found_default when only the default namespace is. */
typedef struct prefix_choice {
    bool is_found;
    sxml_namespace_prefix_identifier prefix;
} prefix_choice;

static prefix_choice choose_prefix(const text_writer *writer, sxml_namespace_uri_identifier uri) {
    prefix_choice choice = {.is_found = false, .prefix = {.index = 0}};
    const sxml_schema_table_storage *storage =
        &writer->components->tables[SXML_SCHEMA_TABLE_NAMESPACE_BINDING];
    const sxml_namespace_binding *bindings = storage->entries;
    for (uint32_t binding_index = 1; binding_index < storage->entry_count; binding_index += 1) {
        const sxml_namespace_binding binding = bindings[binding_index];
        if (binding.namespace_uri.index != uri.index) {
            continue;
        }
        if (binding.namespace_prefix.index != 0) {
            choice = (prefix_choice){.is_found = true, .prefix = binding.namespace_prefix};
            return choice;
        }
        choice.is_found = true;
    }
    return choice;
}

static void write_name(text_writer *writer, sxml_name_identifier name) {
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    if (name.index == 0 ||
        sxml_name_pool_get_name_parts(writer->name_pool, name, &parts) != SXML_STATUS_OK) {
        write_text(writer, "anonymous");
        return;
    }
    if (parts.namespace_uri.index != SXML_NAMESPACE_NONE) {
        const prefix_choice choice = choose_prefix(writer, parts.namespace_uri);
        sxml_byte_view prefix = {.bytes = NULL, .byte_count = 0};
        if (choice.prefix.index != 0 &&
            sxml_name_pool_get_namespace_prefix(writer->name_pool, choice.prefix, &prefix) ==
                SXML_STATUS_OK) {
            write_bytes(writer, prefix);
            write_text(writer, ":");
        } else if (!choice.is_found) {
            write_text(writer, "Q{");
            write_bytes(writer, namespace_text(writer, parts.namespace_uri));
            write_text(writer, "}");
        }
    }
    write_bytes(writer, parts.local_name);
}

static void write_position(text_writer *writer, sxml_schema_position position) {
    write_text(writer, " at ");
    if (writer->has_several_documents) {
        write_unsigned(writer, position.document.value);
        write_text(writer, ":");
    }
    write_unsigned(writer, position.line_number);
    write_text(writer, ":");
    write_unsigned(writer, position.column_number);
    write_text(writer, "\n");
}

/* " LABEL (A, B, ...)", with each entry prefixed by entry_label when it
   is not NULL; nothing for an empty list. */
typedef struct list_text {
    const char *label;
    const char *entry_label;
    sxml_schema_list list;
} list_text;

static void write_list(text_writer *writer, list_text request) {
    const sxml_schema_index_view entries = sxml_schema_get_list(writer->components, request.list);
    if (entries.index_count == 0) {
        return;
    }
    if (request.label[0] != '\0') {
        write_text(writer, " ");
        write_text(writer, request.label);
    }
    write_text(writer, " (");
    for (size_t entry_index = 0; entry_index < entries.index_count; entry_index += 1) {
        if (entry_index > 0) {
            write_text(writer, ", ");
        }
        if (request.entry_label != NULL) {
            write_text(writer, request.entry_label);
            write_text(writer, " ");
        }
        write_unsigned(writer, entries.indexes[entry_index]);
    }
    write_text(writer, ")");
}

/* A type definition's index and, when it has one, its name. */
static void write_type(text_writer *writer, uint32_t type) {
    write_unsigned(writer, type);
    const sxml_schema_type_definition *definition = sxml_schema_get_type_definition(
        writer->components, (sxml_schema_type_definition_index){.value = type});
    if (definition->type_name.index != 0) {
        write_text(writer, " ");
        write_name(writer, definition->type_name);
    }
}

/* " LABEL (extension, ...)" for a derivation set that is not empty. */
typedef struct derivation_set_text {
    const char *label;
    sxml_schema_derivation_set set;
} derivation_set_text;

static void write_derivation_set(text_writer *writer, derivation_set_text request) {
    const char *label = request.label;
    const sxml_schema_derivation_set set = request.set;
    static const struct {
        sxml_schema_derivation derivation;
        const char *name;
    } names[] = {{SXML_SCHEMA_DERIVATION_EXTENSION, "extension"},
                 {SXML_SCHEMA_DERIVATION_RESTRICTION, "restriction"},
                 {SXML_SCHEMA_DERIVATION_LIST, "list"},
                 {SXML_SCHEMA_DERIVATION_UNION, "union"},
                 {SXML_SCHEMA_DERIVATION_SUBSTITUTION, "substitution"}};
    if (set == 0) {
        return;
    }
    write_text(writer, " ");
    write_text(writer, label);
    write_text(writer, " (");
    bool is_first = true;
    for (size_t name_index = 0; name_index < sizeof(names) / sizeof(names[0]); name_index += 1) {
        if ((set & (uint32_t)names[name_index].derivation) != 0) {
            write_text(writer, is_first ? "" : ", ");
            write_text(writer, names[name_index].name);
            is_first = false;
        }
    }
    write_text(writer, ")");
}

static void write_value_constraint(text_writer *writer, sxml_schema_value_constraint constraint) {
    if (constraint.variety == SXML_SCHEMA_VALUE_CONSTRAINT_NONE) {
        return;
    }
    write_text(writer, constraint.variety == SXML_SCHEMA_VALUE_CONSTRAINT_DEFAULT ? " default "
                                                                                  : " fixed ");
    write_quoted(writer, sxml_schema_get_text(writer->components, constraint.lexical_form));
    write_labelled(
        writer, (labelled_index){.label = "context", .index = constraint.namespace_context.value});
}

/* A referenced component's name and reference: " NAME (reference R)". */
static void write_reference_suffix(text_writer *writer, sxml_schema_reference_index reference) {
    if (reference.value == 0) {
        return;
    }
    write_text(writer, " ");
    write_name(writer, sxml_schema_get_reference(writer->components, reference)->name);
    write_text(writer, " (reference ");
    write_unsigned(writer, reference.value);
    write_text(writer, ")");
}

/* -------------------------------------------------------------- Lines */

/* The first index of each table that is not built in. */
static uint32_t first_own_index(sxml_schema_table table) {
    if (table == SXML_SCHEMA_TABLE_TYPE_DEFINITION) {
        return 56;
    }
    if (table == SXML_SCHEMA_TABLE_PARTICLE || table == SXML_SCHEMA_TABLE_WILDCARD) {
        return 3;
    }
    if (table == SXML_SCHEMA_TABLE_MODEL_GROUP) {
        return 2;
    }
    if (table == SXML_SCHEMA_TABLE_ATTRIBUTE_DECLARATION) {
        return 5;
    }
    return 1;
}

static void write_document(text_writer *writer, uint32_t index) {
    const sxml_schema_document_entry *document =
        sxml_schema_get_document(writer->components, (sxml_schema_document_index){.value = index});
    write_text(writer, "document ");
    write_unsigned(writer, index);
    write_text(writer, " ");
    write_quoted(writer, sxml_schema_get_text(writer->components, document->document_uri));
    write_text(writer, " target-namespace ");
    write_namespace(writer, document->target_namespace);
    write_text(writer, "\n");
}

static void write_context(text_writer *writer, uint32_t index) {
    const sxml_schema_namespace_context *context = sxml_schema_get_namespace_context(
        writer->components, (sxml_schema_namespace_context_index){.value = index});
    write_text(writer, "context ");
    write_unsigned(writer, index);
    write_text(writer, " namespaces (");
    const sxml_namespace_binding_view bindings =
        sxml_schema_get_namespace_bindings(writer->components, context->namespace_bindings);
    bool is_first = true;
    for (size_t binding_index = 0; binding_index < bindings.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding binding = bindings.namespace_bindings[binding_index];
        if (binding.namespace_uri.index == (uint32_t)SXML_NAMESPACE_XML) {
            continue;
        }
        write_text(writer, is_first ? "" : ", ");
        is_first = false;
        sxml_byte_view prefix = {.bytes = NULL, .byte_count = 0};
        if (binding.namespace_prefix.index == 0 ||
            sxml_name_pool_get_namespace_prefix(writer->name_pool, binding.namespace_prefix,
                                                &prefix) != SXML_STATUS_OK) {
            write_text(writer, "(default)");
        } else {
            write_bytes(writer, prefix);
        }
        write_text(writer, " = ");
        write_namespace(writer, binding.namespace_uri);
    }
    write_text(writer, ") default-namespace ");
    write_namespace(writer, context->default_namespace);
    write_text(writer, " base-uri ");
    write_quoted(writer, sxml_schema_get_text(writer->components, context->base_uri));
    write_text(writer, "\n");
}

static const char *variety_name(sxml_type_variety variety) {
    if (variety == SXML_TYPE_VARIETY_COMPLEX) {
        return "complex";
    }
    if (variety == SXML_TYPE_VARIETY_ANY_SIMPLE) {
        return "any-simple";
    }
    if (variety == SXML_TYPE_VARIETY_ATOMIC) {
        return "atomic";
    }
    if (variety == SXML_TYPE_VARIETY_LIST) {
        return "list";
    }
    return variety == SXML_TYPE_VARIETY_UNION ? "union" : "other";
}

static const char *derivation_name(sxml_schema_derivation derivation) {
    if (derivation == SXML_SCHEMA_DERIVATION_EXTENSION) {
        return "extension";
    }
    if (derivation == SXML_SCHEMA_DERIVATION_RESTRICTION) {
        return "restriction";
    }
    if (derivation == SXML_SCHEMA_DERIVATION_LIST) {
        return "list";
    }
    return derivation == SXML_SCHEMA_DERIVATION_UNION ? "union" : "none";
}

static const char *content_variety_name(sxml_content_type_variety variety) {
    static const char *const names[] = {
        [SXML_CONTENT_TYPE_VARIETY_EMPTY] = "empty",
        [SXML_CONTENT_TYPE_VARIETY_SIMPLE] = "simple",
        [SXML_CONTENT_TYPE_VARIETY_ELEMENT_ONLY] = "element-only",
        [SXML_CONTENT_TYPE_VARIETY_MIXED] = "mixed",
    };
    return (size_t)variety < sizeof(names) / sizeof(names[0]) ? names[variety] : "empty";
}

static const char *open_content_name(sxml_schema_open_content_mode mode) {
    return mode == SXML_SCHEMA_OPEN_CONTENT_SUFFIX ? "suffix" : "interleave";
}

static void write_complex_parts(text_writer *writer,
                                const sxml_schema_type_definition *definition) {
    write_derivation_set(
        writer,
        (derivation_set_text){.label = "block", .set = definition->prohibited_substitutions});
    if (definition->is_mixed) {
        write_text(writer, " mixed");
    }
    if (definition->has_simple_content) {
        write_text(writer, " simple-content");
    }
    if (definition->explicit_content.value != definition->particle.value) {
        write_labelled(writer, (labelled_index){.label = "explicit-content particle",
                                                .index = definition->explicit_content.value});
    }
    write_text(writer, " content ");
    write_text(writer, content_variety_name(definition->content_type_variety));
    if (definition->particle.value != 0) {
        write_labelled(writer,
                       (labelled_index){.label = "particle", .index = definition->particle.value});
    }
    if (definition->simple_content_type.value != 0) {
        write_text(writer, " simple-type ");
        write_type(writer, definition->simple_content_type.value);
    }
    if (definition->open_content_mode != SXML_SCHEMA_OPEN_CONTENT_NONE) {
        write_text(writer, " open-content ");
        write_text(writer, open_content_name(definition->open_content_mode));
        write_labelled(writer, (labelled_index){.label = "wildcard",
                                                .index = definition->open_content_wildcard.value});
    }
    write_list(writer, (list_text){.label = "attribute-uses",
                                   .entry_label = NULL,
                                   .list = definition->attribute_uses});
    write_list(writer, (list_text){.label = "attribute-groups",
                                   .entry_label = "reference",
                                   .list = definition->attribute_group_references});
    if (definition->attribute_wildcard.value != 0) {
        write_labelled(writer, (labelled_index){.label = "attribute-wildcard",
                                                .index = definition->attribute_wildcard.value});
    }
    if (definition->default_attribute_group.value != 0) {
        write_labelled(writer,
                       (labelled_index){.label = "default-attributes",
                                        .index = definition->default_attribute_group.value});
    }
}

static void write_simple_parts(text_writer *writer, const sxml_schema_type_definition *definition) {
    if (definition->item_type.value != 0) {
        write_text(writer, " item ");
        write_type(writer, definition->item_type.value);
    }
    write_list(
        writer,
        (list_text){.label = "members", .entry_label = NULL, .list = definition->member_types});
    if (definition->primitive_type.value != 0) {
        write_text(writer, " primitive ");
        write_type(writer, definition->primitive_type.value);
    }
    write_list(writer,
               (list_text){.label = "facets", .entry_label = NULL, .list = definition->facets});
}

static void write_type_definition(text_writer *writer, uint32_t index) {
    const sxml_schema_type_definition *definition = sxml_schema_get_type_definition(
        writer->components, (sxml_schema_type_definition_index){.value = index});
    write_text(writer, "type ");
    write_unsigned(writer, index);
    write_text(writer, " ");
    write_name(writer, definition->type_name);
    write_text(writer, " ");
    write_text(writer, variety_name(definition->variety));
    if (definition->is_abstract) {
        write_text(writer, " abstract");
    }
    write_text(writer, " base ");
    write_type(writer, definition->base_type.value);
    write_text(writer, " ");
    write_text(writer, derivation_name(definition->derivation_method));
    write_derivation_set(
        writer, (derivation_set_text){.label = "final", .set = definition->final_derivations});
    if (definition->variety == SXML_TYPE_VARIETY_COMPLEX) {
        write_complex_parts(writer, definition);
    } else {
        write_simple_parts(writer, definition);
    }
    write_list(
        writer,
        (list_text){.label = "assertions", .entry_label = NULL, .list = definition->assertions});
    if (definition->registered_type.index != (uint32_t)SXML_TYPE_NONE) {
        write_text(writer, " registered");
    }
    write_position(writer, definition->position);
}

static void write_scope(text_writer *writer, labelled_index parent_type, labelled_index group) {
    if (parent_type.index != 0) {
        write_labelled(writer, parent_type);
    } else if (group.index != 0) {
        write_labelled(writer, group);
    }
}

static void write_element_declaration(text_writer *writer, uint32_t index) {
    const sxml_schema_element_declaration *declaration = sxml_schema_get_element_declaration(
        writer->components, (sxml_schema_element_declaration_index){.value = index});
    write_text(writer, "element ");
    write_unsigned(writer, index);
    write_text(writer, " ");
    write_name(writer, declaration->element_name);
    if (declaration->scope_variety == SXML_SCHEMA_SCOPE_GLOBAL) {
        write_text(writer, " global");
    } else {
        write_text(writer, " local");
        write_scope(
            writer, (labelled_index){.label = "in type", .index = declaration->parent_type.value},
            (labelled_index){.label = "in group", .index = declaration->parent_group.value});
    }
    if (declaration->is_abstract) {
        write_text(writer, " abstract");
    }
    if (declaration->is_nillable) {
        write_text(writer, " nillable");
    }
    write_text(writer, " type ");
    write_type(writer, declaration->type_definition.value);
    write_value_constraint(writer, declaration->value_constraint);
    write_list(writer, (list_text){.label = "substitution-group",
                                   .entry_label = NULL,
                                   .list = declaration->substitution_group_affiliations});
    write_list(writer, (list_text){.label = "members",
                                   .entry_label = NULL,
                                   .list = declaration->substitution_group_members});
    write_list(writer, (list_text){.label = "alternatives",
                                   .entry_label = NULL,
                                   .list = declaration->type_alternatives});
    write_list(writer, (list_text){.label = "identity-constraints",
                                   .entry_label = NULL,
                                   .list = declaration->identity_constraints});
    write_derivation_set(
        writer,
        (derivation_set_text){.label = "final", .set = declaration->substitution_group_exclusions});
    write_derivation_set(
        writer,
        (derivation_set_text){.label = "block", .set = declaration->disallowed_substitutions});
    write_position(writer, declaration->position);
}

static void write_attribute_declaration(text_writer *writer, uint32_t index) {
    const sxml_schema_attribute_declaration *declaration = sxml_schema_get_attribute_declaration(
        writer->components, (sxml_schema_attribute_declaration_index){.value = index});
    write_text(writer, "attribute ");
    write_unsigned(writer, index);
    write_text(writer, " ");
    write_name(writer, declaration->attribute_name);
    if (declaration->scope_variety == SXML_SCHEMA_SCOPE_GLOBAL) {
        write_text(writer, " global");
    } else {
        write_text(writer, " local");
        write_scope(writer,
                    (labelled_index){.label = "in type", .index = declaration->parent_type.value},
                    (labelled_index){.label = "in attribute-group",
                                     .index = declaration->parent_attribute_group.value});
    }
    write_text(writer, " type ");
    write_type(writer, declaration->type_definition.value);
    write_value_constraint(writer, declaration->value_constraint);
    if (declaration->is_inheritable) {
        write_text(writer, " inheritable");
    }
    write_position(writer, declaration->position);
}

static void write_attribute_use(text_writer *writer, uint32_t index) {
    const sxml_schema_attribute_use *use = sxml_schema_get_attribute_use(
        writer->components, (sxml_schema_attribute_use_index){.value = index});
    static const char *const kinds[] = {"optional", "required", "prohibited"};
    write_text(writer, "attribute-use ");
    write_unsigned(writer, index);
    write_text(writer, " ");
    write_text(writer, kinds[use->use_kind]);
    write_labelled(
        writer, (labelled_index){.label = "attribute", .index = use->attribute_declaration.value});
    write_reference_suffix(writer, use->attribute_declaration_reference);
    write_value_constraint(writer, use->value_constraint);
    if (use->is_inheritable) {
        write_text(writer, " inheritable");
    }
    write_position(writer, use->position);
}

static void write_attribute_group(text_writer *writer, uint32_t index) {
    const sxml_schema_attribute_group *group = sxml_schema_get_attribute_group(
        writer->components, (sxml_schema_attribute_group_index){.value = index});
    write_text(writer, "attribute-group ");
    write_unsigned(writer, index);
    write_text(writer, " ");
    write_name(writer, group->group_name);
    write_list(
        writer,
        (list_text){.label = "attribute-uses", .entry_label = NULL, .list = group->attribute_uses});
    write_list(writer, (list_text){.label = "attribute-groups",
                                   .entry_label = "reference",
                                   .list = group->attribute_group_references});
    if (group->attribute_wildcard.value != 0) {
        write_labelled(writer, (labelled_index){.label = "attribute-wildcard",
                                                .index = group->attribute_wildcard.value});
    }
    write_position(writer, group->position);
}

static void write_group_definition(text_writer *writer, uint32_t index) {
    const sxml_schema_model_group_definition *definition = sxml_schema_get_model_group_definition(
        writer->components, (sxml_schema_model_group_definition_index){.value = index});
    write_text(writer, "group ");
    write_unsigned(writer, index);
    write_text(writer, " ");
    write_name(writer, definition->group_name);
    write_labelled(
        writer, (labelled_index){.label = "model-group", .index = definition->model_group.value});
    write_position(writer, definition->position);
}

static void write_model_group(text_writer *writer, uint32_t index) {
    const sxml_schema_model_group *group = sxml_schema_get_model_group(
        writer->components, (sxml_schema_model_group_index){.value = index});
    static const char *const compositors[] = {"sequence", "choice", "all"};
    write_text(writer, "model-group ");
    write_unsigned(writer, index);
    write_text(writer, " ");
    write_text(writer, compositors[group->compositor]);
    if (group->particles.entry_count == 0) {
        write_text(writer, " ()");
    } else {
        write_list(writer,
                   (list_text){.label = "", .entry_label = "particle", .list = group->particles});
    }
    write_position(writer, group->position);
}

static void write_particle(text_writer *writer, uint32_t index) {
    const sxml_schema_particle *particle =
        sxml_schema_get_particle(writer->components, (sxml_schema_particle_index){.value = index});
    write_text(writer, "particle ");
    write_unsigned(writer, index);
    write_text(writer, " ");
    write_unsigned(writer, particle->minimum_occurrences);
    write_text(writer, "..");
    if (particle->maximum_occurrences == SXML_SCHEMA_UNBOUNDED) {
        write_text(writer, "unbounded");
    } else {
        write_unsigned(writer, particle->maximum_occurrences);
    }
    static const char *const terms[] = {"none", "element", "model-group", "wildcard"};
    write_labelled(writer,
                   (labelled_index){.label = terms[particle->term_kind], .index = particle->term});
    write_reference_suffix(writer, particle->term_reference);
    write_position(writer, particle->position);
}

static void write_namespace_entries(text_writer *writer, sxml_schema_list list) {
    const sxml_schema_index_view entries = sxml_schema_get_list(writer->components, list);
    write_text(writer, "(");
    for (size_t entry_index = 0; entry_index < entries.index_count; entry_index += 1) {
        write_text(writer, entry_index > 0 ? ", " : "");
        if (entries.indexes[entry_index] == SXML_NAMESPACE_NONE) {
            write_text(writer, "absent");
        } else {
            write_bytes(writer, namespace_text(writer, (sxml_namespace_uri_identifier){
                                                           .index = entries.indexes[entry_index]}));
        }
    }
    write_text(writer, ")");
}

static void write_wildcard(text_writer *writer, uint32_t index) {
    const sxml_schema_wildcard *wildcard =
        sxml_schema_get_wildcard(writer->components, (sxml_schema_wildcard_index){.value = index});
    write_text(writer, "wildcard ");
    write_unsigned(writer, index);
    write_text(writer, " ");
    if (wildcard->namespace_constraint_variety == SXML_SCHEMA_NAMESPACE_CONSTRAINT_ANY) {
        write_text(writer, "any");
    } else {
        if (wildcard->namespace_constraint_variety == SXML_SCHEMA_NAMESPACE_CONSTRAINT_NOT) {
            write_text(writer, "not ");
        }
        write_namespace_entries(writer, wildcard->namespaces);
    }
    const sxml_schema_index_view names =
        sxml_schema_get_list(writer->components, wildcard->disallowed_names);
    if (names.index_count != 0) {
        write_text(writer, " not-names (");
        for (size_t name_index = 0; name_index < names.index_count; name_index += 1) {
            write_text(writer, name_index > 0 ? ", " : "");
            write_name(writer, (sxml_name_identifier){.index = names.indexes[name_index]});
        }
        write_text(writer, ")");
    }
    if (wildcard->disallows_defined) {
        write_text(writer, " not-defined");
    }
    if (wildcard->disallows_sibling) {
        write_text(writer, " not-sibling");
    }
    static const char *const processes[] = {" strict", " lax", " skip"};
    write_text(writer, processes[wildcard->process_contents]);
    write_position(writer, wildcard->position);
}

static void write_identity_constraint(text_writer *writer, uint32_t index) {
    const sxml_schema_identity_constraint *constraint = sxml_schema_get_identity_constraint(
        writer->components, (sxml_schema_identity_constraint_index){.value = index});
    static const char *const categories[] = {"unique", "key", "keyref"};
    write_text(writer, "identity-constraint ");
    write_unsigned(writer, index);
    write_text(writer, " ");
    write_name(writer, constraint->constraint_name);
    write_text(writer, " ");
    write_text(writer, categories[constraint->category]);
    if (constraint->definition_reference.value != 0) {
        write_text(writer, " ref");
        write_reference_suffix(writer, constraint->definition_reference);
    }
    if (constraint->selector.value != 0) {
        write_labelled(writer, (labelled_index){.label = "selector xpath",
                                                .index = constraint->selector.value});
    }
    write_list(writer,
               (list_text){.label = "fields", .entry_label = "xpath", .list = constraint->fields});
    if (constraint->referenced_key_reference.value != 0) {
        write_labelled(
            writer, (labelled_index){.label = "refer", .index = constraint->referenced_key.value});
        write_reference_suffix(writer, constraint->referenced_key_reference);
    }
    write_position(writer, constraint->position);
}

static void write_type_alternative(text_writer *writer, uint32_t index) {
    const sxml_schema_type_alternative *alternative = sxml_schema_get_type_alternative(
        writer->components, (sxml_schema_type_alternative_index){.value = index});
    write_text(writer, "alternative ");
    write_unsigned(writer, index);
    if (alternative->test.value != 0) {
        write_labelled(writer,
                       (labelled_index){.label = "test xpath", .index = alternative->test.value});
    }
    write_text(writer, " type ");
    write_type(writer, alternative->type_definition.value);
    write_position(writer, alternative->position);
}

static void write_assertion(text_writer *writer, uint32_t index) {
    const sxml_schema_assertion *assertion = sxml_schema_get_assertion(
        writer->components, (sxml_schema_assertion_index){.value = index});
    write_text(writer, "assertion ");
    write_unsigned(writer, index);
    write_labelled(writer, (labelled_index){.label = "test xpath", .index = assertion->test.value});
    write_position(writer, assertion->position);
}

static void write_notation(text_writer *writer, uint32_t index) {
    const sxml_schema_notation_declaration *notation = sxml_schema_get_notation_declaration(
        writer->components, (sxml_schema_notation_declaration_index){.value = index});
    write_text(writer, "notation ");
    write_unsigned(writer, index);
    write_text(writer, " ");
    write_name(writer, notation->notation_name);
    if (notation->has_public_identifier) {
        write_text(writer, " public ");
        write_quoted(writer, sxml_schema_get_text(writer->components, notation->public_identifier));
    }
    if (notation->has_system_identifier) {
        write_text(writer, " system ");
        write_quoted(writer, sxml_schema_get_text(writer->components, notation->system_identifier));
    }
    write_position(writer, notation->position);
}

static void write_facet(text_writer *writer, uint32_t index) {
    const sxml_schema_facet *facet =
        sxml_schema_get_facet(writer->components, (sxml_schema_facet_index){.value = index});
    static const char *const kinds[] = {
        [SXML_FACET_KIND_LENGTH] = "length",
        [SXML_FACET_KIND_MIN_LENGTH] = "min-length",
        [SXML_FACET_KIND_MAX_LENGTH] = "max-length",
        [SXML_FACET_KIND_PATTERN] = "pattern",
        [SXML_FACET_KIND_ENUMERATION] = "enumeration",
        [SXML_FACET_KIND_WHITESPACE] = "white-space",
        [SXML_FACET_KIND_MAX_INCLUSIVE] = "max-inclusive",
        [SXML_FACET_KIND_MAX_EXCLUSIVE] = "max-exclusive",
        [SXML_FACET_KIND_MIN_EXCLUSIVE] = "min-exclusive",
        [SXML_FACET_KIND_MIN_INCLUSIVE] = "min-inclusive",
        [SXML_FACET_KIND_TOTAL_DIGITS] = "total-digits",
        [SXML_FACET_KIND_FRACTION_DIGITS] = "fraction-digits",
        [SXML_FACET_KIND_EXPLICIT_TIMEZONE] = "explicit-timezone",
    };
    write_text(writer, "facet ");
    write_unsigned(writer, index);
    write_text(writer, " ");
    write_text(writer, kinds[facet->facet_kind]);
    write_text(writer, " ");
    write_quoted(writer, sxml_schema_get_text(writer->components, facet->value));
    if (facet->is_fixed) {
        write_text(writer, " fixed");
    }
    write_labelled(writer,
                   (labelled_index){.label = "context", .index = facet->namespace_context.value});
    write_position(writer, facet->position);
}

static void write_xpath_record(text_writer *writer, uint32_t index) {
    const sxml_schema_xpath_record *record = sxml_schema_get_xpath_record(
        writer->components, (sxml_schema_xpath_record_index){.value = index});
    write_text(writer, "xpath ");
    write_unsigned(writer, index);
    write_labelled(writer,
                   (labelled_index){.label = "context", .index = record->namespace_context.value});
    write_text(writer, " ");
    write_quoted(writer, sxml_schema_get_text(writer->components, record->expression));
    if (record->compiled_expression != NULL) {
        write_text(writer, " compiled");
    }
    write_position(writer, record->position);
}

static void write_reference(text_writer *writer, uint32_t index) {
    const sxml_schema_reference *reference = sxml_schema_get_reference(
        writer->components, (sxml_schema_reference_index){.value = index});
    static const char *const spaces[] = {
        [SXML_SCHEMA_SYMBOL_SPACE_NONE] = "none",
        [SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION] = "type",
        [SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION] = "element",
        [SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_DECLARATION] = "attribute",
        [SXML_SCHEMA_SYMBOL_SPACE_MODEL_GROUP_DEFINITION] = "group",
        [SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_GROUP] = "attribute-group",
        [SXML_SCHEMA_SYMBOL_SPACE_IDENTITY_CONSTRAINT] = "identity-constraint",
        [SXML_SCHEMA_SYMBOL_SPACE_NOTATION_DECLARATION] = "notation",
    };
    write_text(writer, "reference ");
    write_unsigned(writer, index);
    write_text(writer, " ");
    write_text(writer, spaces[reference->symbol_space]);
    write_text(writer, " ");
    write_name(writer, reference->name);
    write_text(writer, " -> ");
    if (reference->component == 0) {
        write_text(writer, "none");
    } else {
        write_unsigned(writer, reference->component);
    }
    write_position(writer, reference->position);
}

typedef void (*line_writer)(text_writer *writer, uint32_t index);

/* The tables in the order they are written, each with its line. */
static const struct {
    sxml_schema_table table;
    line_writer write_line;
} table_lines[] = {
    {SXML_SCHEMA_TABLE_DOCUMENT, write_document},
    {SXML_SCHEMA_TABLE_NAMESPACE_CONTEXT, write_context},
    {SXML_SCHEMA_TABLE_TYPE_DEFINITION, write_type_definition},
    {SXML_SCHEMA_TABLE_ELEMENT_DECLARATION, write_element_declaration},
    {SXML_SCHEMA_TABLE_ATTRIBUTE_DECLARATION, write_attribute_declaration},
    {SXML_SCHEMA_TABLE_ATTRIBUTE_USE, write_attribute_use},
    {SXML_SCHEMA_TABLE_ATTRIBUTE_GROUP, write_attribute_group},
    {SXML_SCHEMA_TABLE_MODEL_GROUP_DEFINITION, write_group_definition},
    {SXML_SCHEMA_TABLE_MODEL_GROUP, write_model_group},
    {SXML_SCHEMA_TABLE_PARTICLE, write_particle},
    {SXML_SCHEMA_TABLE_WILDCARD, write_wildcard},
    {SXML_SCHEMA_TABLE_IDENTITY_CONSTRAINT, write_identity_constraint},
    {SXML_SCHEMA_TABLE_TYPE_ALTERNATIVE, write_type_alternative},
    {SXML_SCHEMA_TABLE_ASSERTION, write_assertion},
    {SXML_SCHEMA_TABLE_NOTATION_DECLARATION, write_notation},
    {SXML_SCHEMA_TABLE_FACET, write_facet},
    {SXML_SCHEMA_TABLE_XPATH_RECORD, write_xpath_record},
    {SXML_SCHEMA_TABLE_REFERENCE, write_reference},
};

sxml_status sxml_schema_components_write_text(const sxml_schema_components *components,
                                              sxml_memory_view output, size_t *byte_count) {
    if (components == NULL || byte_count == NULL ||
        (output.byte_count > 0 && output.bytes == NULL)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    text_writer writer = {.output = output,
                          .byte_count = 0,
                          .components = components,
                          .name_pool = components->processor->name_pool,
                          .has_several_documents =
                              components->tables[SXML_SCHEMA_TABLE_DOCUMENT].entry_count > 2U};
    for (size_t line_index = 0; line_index < sizeof(table_lines) / sizeof(table_lines[0]);
         line_index += 1) {
        const sxml_schema_table table = table_lines[line_index].table;
        const uint32_t count = components->tables[table].entry_count;
        for (uint32_t index = first_own_index(table); index < count; index += 1) {
            table_lines[line_index].write_line(&writer, index);
        }
    }
    /* The content models COMPLEX_TYPES compiled (src/schema_content_model.c
       writes them), after every component. */
    const uint32_t type_count = components->tables[SXML_SCHEMA_TABLE_TYPE_DEFINITION].entry_count;
    for (uint32_t index = first_own_index(SXML_SCHEMA_TABLE_TYPE_DEFINITION); index < type_count;
         index += 1) {
        const sxml_schema_content_model *content_model =
            sxml_schema_get_type_definition(components,
                                            (sxml_schema_type_definition_index){.value = index})
                ->content_model;
        if (content_model == NULL) {
            continue;
        }
        const size_t offset = writer.byte_count;
        size_t model_byte_count = 0;
        const sxml_status status = sxml_schema_content_model_write_text(
            components,
            &(sxml_schema_content_model_text_request){
                .content_model = content_model,
                .output = {.bytes = offset < output.byte_count ? output.bytes + offset : NULL,
                           .byte_count =
                               offset < output.byte_count ? output.byte_count - offset : 0}},
            &model_byte_count);
        if (status != SXML_STATUS_OK && status != SXML_STATUS_CAPACITY_EXCEEDED) {
            return status;
        }
        writer.byte_count += model_byte_count;
    }
    *byte_count = writer.byte_count;
    return writer.byte_count > output.byte_count ? SXML_STATUS_CAPACITY_EXCEEDED : SXML_STATUS_OK;
}
