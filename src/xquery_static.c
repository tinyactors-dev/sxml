/*
 * The static context of an XQuery main module (src/xquery_static_internal.h;
 * D135, packet 3.10): the prolog's namespaces, setters, variable and
 * function declarations applied to the host's static context.
 *
 * Golden file: src/xpath_static.c. The prolog is a flat list of
 * declarations, so every check is a loop over it; duplicates are found by
 * sorting (a heapsort over an array, no recursion, D71), so a prolog of n
 * declarations costs O(n log n).
 *
 * Section numbers are of XQuery 4.0 (W3C QT4 CG draft, 13 October 2026)
 * unless another specification is named.
 */
#include "xquery_static_internal.h"

#include "processor_internal.h"
#include "schema_scope_internal.h"

#include "format_internal.h"
#include "sort_internal.h"
#include "sxml/memory.h"
#include "sxml/xdm.h"
#include "xdm_internal.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <string.h>

/* The namespace of unprefixed annotations and options (5.15, 5.21). */
static const sxml_byte_view xquery_namespace_text =
    SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/2012/xquery");

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

/* The PROLOG of the module's MAIN_MODULE; 0 for none. */
static sxml_ast_node_index prolog_of(const sxml_ast *ast) {
    const sxml_ast_node_index module = child_of(ast, ast->root, SXML_AST_FIELD_MODULE);
    return child_of(ast, module, SXML_AST_FIELD_PROLOG);
}

static sxml_ast_node_index first_declaration(const sxml_ast *ast) {
    const sxml_ast_node_index prolog = prolog_of(ast);
    if (prolog.value == 0) {
        return prolog;
    }
    return child_of(ast, prolog, SXML_AST_FIELD_DECLARATION);
}

/* --------------------------------------------------------- Parameters */

static bool is_static_field(const sxml_ast *ast, sxml_ast_node_index node) {
    return node_of(ast, node)->kind == SXML_AST_KIND_EXTENDED_FIELD_DECLARATION &&
           (node_of(ast, node)->flags & SXML_AST_FLAG_STATIC) != 0;
}

static sxml_ast_node_index skip_static_fields(const sxml_ast *ast, sxml_ast_node_index node) {
    sxml_ast_node_index parameter = node;
    while (parameter.value != 0 && is_static_field(ast, parameter)) {
        parameter = next_of(ast, parameter);
    }
    return parameter;
}

sxml_ast_node_index sxml_xquery_first_parameter(const sxml_ast *ast,
                                                sxml_ast_node_index declaration) {
    if (node_of(ast, declaration)->kind == SXML_AST_KIND_NOMINATIVE_RECORD_TYPE_DECLARATION) {
        return skip_static_fields(ast, child_of(ast, declaration, SXML_AST_FIELD_FIELD));
    }
    return child_of(ast, declaration, SXML_AST_FIELD_PARAMETER);
}

sxml_ast_node_index sxml_xquery_next_parameter(const sxml_ast *ast,
                                               sxml_ast_node_index parameter) {
    return skip_static_fields(ast, next_of(ast, parameter));
}

sxml_ast_node_index sxml_xquery_parameter_name(const sxml_ast *ast,
                                               sxml_ast_node_index parameter) {
    if (node_of(ast, parameter)->kind == SXML_AST_KIND_EXTENDED_FIELD_DECLARATION) {
        return child_of(ast, parameter, SXML_AST_FIELD_NAME);
    }
    return child_of(ast, child_of(ast, parameter, SXML_AST_FIELD_VARIABLE), SXML_AST_FIELD_NAME);
}

sxml_ast_node_index sxml_xquery_parameter_type(const sxml_ast *ast,
                                               sxml_ast_node_index parameter) {
    if (node_of(ast, parameter)->kind == SXML_AST_KIND_EXTENDED_FIELD_DECLARATION) {
        return child_of(ast, parameter, SXML_AST_FIELD_TYPE);
    }
    const sxml_ast_node_index declaration =
        child_of(ast, child_of(ast, parameter, SXML_AST_FIELD_VARIABLE), SXML_AST_FIELD_TYPE);
    return child_of(ast, declaration, SXML_AST_FIELD_TYPE);
}

sxml_byte_view sxml_xquery_field_name(const sxml_ast *ast, sxml_ast_node_index field) {
    const sxml_ast_node_index name = child_of(ast, field, SXML_AST_FIELD_NAME);
    if (node_of(ast, name)->kind == SXML_AST_KIND_STRING_LITERAL) {
        return text_of(ast, name);
    }
    return sxml_ast_get_text(ast, node_of(ast, name)->payload.name.local_name);
}

/* ------------------------------------------------------------ Reading */

/* Everything reading one prolog needs. */
typedef struct static_reader {
    const sxml_xquery_static_request *request;
    const sxml_ast *ast;
    sxml_name_pool *name_pool;
    sxml_type_registry *type_registry;
    sxml_allocator allocator;
    /* The context being read; names and types resolve with its XPath
       static context as far as it is built. */
    const sxml_xquery_static_context *context;
} static_reader;

/* code_and_description is "CODE description" (an eight-character code, a
   space, static text). */
static sxml_status report(const static_reader *reader, sxml_ast_node_index node,
                          sxml_byte_view code_and_description) {
    return sxml_xpath_report_static_error(
        &(sxml_xpath_static_error){
            .ast = reader->ast,
            .node = node,
            .code_local_name = {.bytes = code_and_description.bytes, .byte_count = 8},
            .description = {.bytes = code_and_description.bytes + 9,
                            .byte_count = code_and_description.byte_count - 9}},
        reader->request->error_record);
}

/* An array of element_count zeroed elements (at least one). */
typedef struct array_request {
    size_t element_count;
    size_t element_size;
    size_t element_alignment;
} array_request;

static sxml_status allocate_array(const static_reader *reader, array_request request,
                                  void **memory) {
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(
        request.element_count == 0 ? 1 : request.element_count, request.element_size,
        &byte_count);
    void *allocated = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            reader->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = request.element_alignment},
            &allocated);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(allocated, 0, byte_count);
    *memory = allocated;
    return SXML_STATUS_OK;
}

/* Interns a URILiteral (src/xquery_static_internal.h). */
sxml_status sxml_xquery_intern_uri_literal(const sxml_xquery_uri_literal *literal,
                                           sxml_namespace_uri_identifier *uri) {
    const sxml_byte_view text = literal->text;
    size_t first = 0;
    size_t end = text.byte_count;
    while (first < end && sxml_xml_byte_is_whitespace(text.bytes[first])) {
        first += 1;
    }
    while (end > first && sxml_xml_byte_is_whitespace(text.bytes[end - 1])) {
        end -= 1;
    }
    bool needs_collapsing = false;
    for (size_t byte_index = first; byte_index < end; byte_index += 1) {
        if (sxml_xml_byte_is_whitespace(text.bytes[byte_index])) {
            needs_collapsing = true;
        }
    }
    const sxml_byte_view trimmed = {.bytes = text.bytes + first, .byte_count = end - first};
    if (!needs_collapsing) {
        return sxml_name_pool_intern_namespace_uri(literal->name_pool, trimmed, uri);
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        literal->allocator, (sxml_allocation_request){.byte_count = trimmed.byte_count, .alignment = 1},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *collapsed = memory;
    size_t collapsed_count = 0;
    for (size_t byte_index = 0; byte_index < trimmed.byte_count; byte_index += 1) {
        const uint8_t byte = trimmed.bytes[byte_index];
        if (sxml_xml_byte_is_whitespace(byte)) {
            if (collapsed[collapsed_count - 1] != ' ') {
                collapsed[collapsed_count] = ' ';
                collapsed_count += 1;
            }
            continue;
        }
        collapsed[collapsed_count] = byte;
        collapsed_count += 1;
    }
    status = sxml_name_pool_intern_namespace_uri(
        literal->name_pool, (sxml_byte_view){.bytes = collapsed, .byte_count = collapsed_count},
        uri);
    sxml_allocator_release(literal->allocator, memory);
    return status;
}

/* Interns a URILiteral, its whitespace normalized as for xs:anyURI
   (A.3.1). */
static sxml_status intern_uri(const static_reader *reader, sxml_byte_view text,
                              sxml_namespace_uri_identifier *uri) {
    return sxml_xquery_intern_uri_literal(
        &(sxml_xquery_uri_literal){
            .name_pool = reader->name_pool, .allocator = reader->allocator, .text = text},
        uri);
}

static bool is_well_known(sxml_namespace_uri_identifier uri,
                          sxml_well_known_namespace namespace_uri) {
    return uri.index == sxml_well_known_namespace_identifier(namespace_uri).index;
}

/* XQST0070 for the XML and XMLNS namespace URIs (5.13, 5.14). */
static bool is_xml_namespace(sxml_namespace_uri_identifier uri) {
    return is_well_known(uri, SXML_NAMESPACE_XML) || is_well_known(uri, SXML_NAMESPACE_XMLNS);
}

/* The namespaces no declared function or annotation may be in (5.15,
   5.18); also the XQuery namespace for annotations. */
static bool is_reserved_namespace(sxml_namespace_uri_identifier uri) {
    static const sxml_well_known_namespace reserved[] = {
        SXML_NAMESPACE_XML,           SXML_NAMESPACE_XML_SCHEMA,
        SXML_NAMESPACE_XML_SCHEMA_INSTANCE, SXML_NAMESPACE_FUNCTIONS,
        SXML_NAMESPACE_ARRAY_FUNCTIONS, SXML_NAMESPACE_MAP_FUNCTIONS,
        SXML_NAMESPACE_MATH_FUNCTIONS,
    };
    for (size_t reserved_index = 0; reserved_index < sizeof(reserved) / sizeof(reserved[0]);
         reserved_index += 1) {
        if (is_well_known(uri, reserved[reserved_index])) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------- Sorting */

/* A key and where it came from (a declaration's position), sorted by key,
   then position. */
typedef struct sort_entry {
    uint64_t key;
    size_t position;
} sort_entry;

static bool sorts_before(const void *state, sxml_sort_pair pair) {
    (void)state;
    const sort_entry *left = pair.first;
    const sort_entry *right = pair.second;
    return left->key < right->key || (left->key == right->key && left->position < right->position);
}

static void sort_entries(sort_entry *entries, size_t entry_count) {
    sxml_sort_in_place(&(sxml_sort_request){.elements = entries,
                                            .element_count = entry_count,
                                            .element_size = sizeof(sort_entry),
                                            .is_before = sorts_before,
                                            .state = NULL});
}

/* ------------------------------------------------------- Setters */

/* A declaration a prolog may make at most once, and the error of a second
   one. */
typedef struct single_declaration_rule {
    sxml_ast_kind kind;
    sxml_byte_view code_and_description;
} single_declaration_rule;

static const single_declaration_rule single_declaration_rules[] = {
    {SXML_AST_KIND_BOUNDARY_SPACE_DECLARATION,
     SXML_BYTE_VIEW_INITIALIZER("XQST0068 the prolog declares boundary-space twice")},
    {SXML_AST_KIND_DEFAULT_COLLATION_DECLARATION,
     SXML_BYTE_VIEW_INITIALIZER("XQST0038 the prolog declares the default collation twice")},
    {SXML_AST_KIND_BASE_URI_DECLARATION,
     SXML_BYTE_VIEW_INITIALIZER("XQST0032 the prolog declares the base URI twice")},
    {SXML_AST_KIND_CONSTRUCTION_DECLARATION,
     SXML_BYTE_VIEW_INITIALIZER("XQST0067 the prolog declares construction twice")},
    {SXML_AST_KIND_ORDERING_MODE_DECLARATION,
     SXML_BYTE_VIEW_INITIALIZER("XQST0065 the prolog declares the ordering mode twice")},
    {SXML_AST_KIND_EMPTY_ORDER_DECLARATION,
     SXML_BYTE_VIEW_INITIALIZER("XQST0069 the prolog declares the empty order twice")},
    {SXML_AST_KIND_COPY_NAMESPACES_DECLARATION,
     SXML_BYTE_VIEW_INITIALIZER("XQST0055 the prolog declares copy-namespaces twice")},
    {SXML_AST_KIND_CONTEXT_VALUE_DECLARATION,
     SXML_BYTE_VIEW_INITIALIZER("XQST0099 the prolog declares the context value twice")},
    /* XQuery Update Facility 3.0, 5.2 (D187). */
    {SXML_AST_KIND_REVALIDATION_DECLARATION,
     SXML_BYTE_VIEW_INITIALIZER("XUST0003 the prolog declares revalidation twice")},
};

#define SINGLE_DECLARATION_RULE_COUNT                                          \
    (sizeof(single_declaration_rules) / sizeof(single_declaration_rules[0]))

/* What the first pass over the prolog counts and finds. */
typedef struct prolog_survey {
    size_t namespace_declaration_count;
    size_t module_import_count;
    size_t schema_import_count;
    size_t variable_declaration_count;
    size_t function_declaration_count;
    size_t parameter_count;
    size_t signature_count;
    size_t item_type_declaration_count;
    size_t record_declaration_count;
    size_t decimal_format_declaration_count;
    size_t decimal_format_property_count;
    sxml_ast_node_index default_element_namespace;
    sxml_ast_node_index default_function_namespace;
    sxml_ast_node_index base_uri_declaration;
    sxml_ast_node_index default_collation_declaration;
    sxml_ast_node_index context_value_declaration;
} prolog_survey;

/* Counts the parameters of a function declaration or of a record's
   constructor (5.18.3, 5.20.2): XQST0148 for a required one after an
   optional one. */
static sxml_status survey_parameters(const static_reader *reader,
                                     sxml_ast_node_index declaration, prolog_survey *survey) {
    const sxml_ast *ast = reader->ast;
    size_t parameter_count = 0;
    size_t required_count = 0;
    for (sxml_ast_node_index parameter = sxml_xquery_first_parameter(ast, declaration);
         parameter.value != 0; parameter = sxml_xquery_next_parameter(ast, parameter)) {
        const bool has_default =
            child_of(ast, parameter, SXML_AST_FIELD_DEFAULT).value != 0;
        if (!has_default && required_count != parameter_count) {
            /* 5.18.3: required parameters come first. */
            return report(reader, parameter,
                          SXML_BYTE_VIEW_FROM_LITERAL(
                              "XQST0148 a required parameter follows an optional one"));
        }
        parameter_count += 1;
        if (!has_default) {
            required_count += 1;
        }
    }
    if (parameter_count >= UINT32_MAX / 4) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    survey->function_declaration_count += 1;
    survey->parameter_count += parameter_count;
    survey->signature_count += parameter_count - required_count + 1;
    return SXML_STATUS_OK;
}

static sxml_status survey_declaration(const static_reader *reader,
                                      sxml_ast_node_index declaration,
                                      prolog_survey *survey) {
    const sxml_ast *ast = reader->ast;
    const sxml_ast_node *node = node_of(ast, declaration);
    switch ((uint32_t)node->kind) {
    case SXML_AST_KIND_NAMESPACE_DECLARATION:
        survey->namespace_declaration_count += 1;
        return SXML_STATUS_OK;
    case SXML_AST_KIND_DEFAULT_NAMESPACE_DECLARATION: {
        sxml_ast_node_index *seen = node->detail.keyword == SXML_AST_KEYWORD_FUNCTION
                                        ? &survey->default_function_namespace
                                        : &survey->default_element_namespace;
        if (seen->value != 0) {
            return report(reader, declaration,
                          SXML_BYTE_VIEW_FROM_LITERAL(
                              "XQST0066 the prolog declares a default namespace twice"));
        }
        *seen = declaration;
        return SXML_STATUS_OK;
    }
    case SXML_AST_KIND_BASE_URI_DECLARATION:
        survey->base_uri_declaration = declaration;
        return SXML_STATUS_OK;
    case SXML_AST_KIND_DEFAULT_COLLATION_DECLARATION:
        survey->default_collation_declaration = declaration;
        return SXML_STATUS_OK;
    case SXML_AST_KIND_CONTEXT_VALUE_DECLARATION:
        survey->context_value_declaration = declaration;
        if (reader->request->target_namespace.index != 0 &&
            (child_of(ast, declaration, SXML_AST_FIELD_VALUE).value != 0 ||
             child_of(ast, declaration, SXML_AST_FIELD_DEFAULT).value != 0)) {
            /* 5.17: only the main module sets the initial context value. */
            return report(reader, declaration,
                          SXML_BYTE_VIEW_FROM_LITERAL(
                              "XQST0113 a library module gives the context value a value"));
        }
        return SXML_STATUS_OK;
    case SXML_AST_KIND_VARIABLE_DECLARATION:
        survey->variable_declaration_count += 1;
        return SXML_STATUS_OK;
    case SXML_AST_KIND_NOMINATIVE_RECORD_TYPE_DECLARATION:
        /* Also counted as a constructor function (5.20.2); the space is
           unused without %constructor. */
        survey->record_declaration_count += 1;
        return survey_parameters(reader, declaration, survey);
    case SXML_AST_KIND_FUNCTION_DECLARATION:
        return survey_parameters(reader, declaration, survey);
    case SXML_AST_KIND_SCHEMA_IMPORT: {
        /* 5.11 (D174): "default element namespace" is a default element
           namespace declaration too (5.14.1: XQST0066 twice). */
        survey->schema_import_count += 1;
        const sxml_ast_node_index prefix = child_of(ast, declaration, SXML_AST_FIELD_PREFIX);
        if (prefix.value != 0 && child_of(ast, prefix, SXML_AST_FIELD_PREFIX).value == 0) {
            if (survey->default_element_namespace.value != 0) {
                return report(reader, declaration,
                              SXML_BYTE_VIEW_FROM_LITERAL(
                                  "XQST0066 the prolog declares a default namespace twice"));
            }
            survey->default_element_namespace = declaration;
        }
        return SXML_STATUS_OK;
    }
    case SXML_AST_KIND_MODULE_IMPORT:
        /* The module set loaded the modules (D148); the prefix is bound
           here. */
        survey->module_import_count += 1;
        return SXML_STATUS_OK;
    case SXML_AST_KIND_ITEM_TYPE_DECLARATION:
        survey->item_type_declaration_count += 1;
        return SXML_STATUS_OK;
    case SXML_AST_KIND_REVALIDATION_DECLARATION:
        /* XQuery Update Facility 3.0, 3.1: every mode is supported
           (XUST0026 never), revalidation by upd:revalidate after the
           updates (src/xquery_update_apply.c, D205). */
        return SXML_STATUS_OK;
    case SXML_AST_KIND_DECIMAL_FORMAT_DECLARATION:
        survey->decimal_format_declaration_count += 1;
        for (sxml_ast_node_index property = child_of(ast, declaration, SXML_AST_FIELD_PROPERTY);
             property.value != 0; property = next_of(ast, property)) {
            survey->decimal_format_property_count += 1;
        }
        return SXML_STATUS_OK;
    default:
        return SXML_STATUS_OK;
    }
}

/* Counts the declarations and checks the setters (5.3 to 5.9) and the
   context value declaration (5.17) for repetition. */
static sxml_status survey_prolog(const static_reader *reader, prolog_survey *survey) {
    bool is_seen[SINGLE_DECLARATION_RULE_COUNT] = {false};
    sxml_status status = SXML_STATUS_OK;
    for (sxml_ast_node_index declaration = first_declaration(reader->ast);
         status == SXML_STATUS_OK && declaration.value != 0;
         declaration = next_of(reader->ast, declaration)) {
        const sxml_ast_kind kind = node_of(reader->ast, declaration)->kind;
        for (size_t rule_index = 0; rule_index < SINGLE_DECLARATION_RULE_COUNT;
             rule_index += 1) {
            if (single_declaration_rules[rule_index].kind != kind) {
                continue;
            }
            if (is_seen[rule_index]) {
                return report(reader, declaration,
                              single_declaration_rules[rule_index].code_and_description);
            }
            is_seen[rule_index] = true;
        }
        status = survey_declaration(reader, declaration, survey);
    }
    return status;
}

/* ------------------------------------------------------- Namespaces */

/* Interns the text of a declaration's prefix (an NCNAME node). */
static sxml_status intern_prefix(const static_reader *reader, sxml_ast_node_index prefix_node,
                                 sxml_namespace_prefix_identifier *prefix) {
    return sxml_name_pool_intern_namespace_prefix(
        reader->name_pool,
        sxml_ast_get_text(reader->ast, node_of(reader->ast, prefix_node)->payload.name.local_name),
        prefix);
}

/* The bindings of the namespace declarations (5.13) and the default
   element namespace (5.14.1, prefix 0), in prolog order, in
   bindings[0 .. *binding_count). */
typedef struct binding_list {
    sxml_namespace_binding *bindings;
    size_t binding_count;
} binding_list;

/* The NCNAME a namespace declaration, module import or schema import
   binds (a schema import's through its SCHEMA_PREFIX); 0 for none. */
static sxml_ast_node_index declared_prefix(const sxml_ast *ast, sxml_ast_node_index declaration) {
    const sxml_ast_node_index prefix = child_of(ast, declaration, SXML_AST_FIELD_PREFIX);
    if (prefix.value != 0 && node_of(ast, declaration)->kind == SXML_AST_KIND_SCHEMA_IMPORT) {
        return child_of(ast, prefix, SXML_AST_FIELD_PREFIX);
    }
    return prefix;
}

static sxml_status read_namespace_declaration(const static_reader *reader,
                                              sxml_ast_node_index declaration,
                                              binding_list *list) {
    const sxml_ast *ast = reader->ast;
    const sxml_ast_node_index prefix_node = declared_prefix(ast, declaration);
    const sxml_byte_view prefix_text =
        sxml_ast_get_text(ast, node_of(ast, prefix_node)->payload.name.local_name);
    if (sxml_byte_view_equals(prefix_text, SXML_BYTE_VIEW_FROM_LITERAL("xml")) ||
        sxml_byte_view_equals(prefix_text, SXML_BYTE_VIEW_FROM_LITERAL("xmlns"))) {
        return report(reader, prefix_node,
                      SXML_BYTE_VIEW_FROM_LITERAL(
                          "XQST0070 the prefixes xml and xmlns cannot be declared"));
    }
    sxml_namespace_uri_identifier uri = {0};
    sxml_status status = intern_uri(
        reader, text_of(ast, child_of(ast, declaration, SXML_AST_FIELD_URI)), &uri);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_ast_kind kind = node_of(ast, declaration)->kind;
    if (kind == SXML_AST_KIND_SCHEMA_IMPORT && uri.index == SXML_NAMESPACE_NONE) {
        /* 5.11. */
        return report(reader, declaration,
                      SXML_BYTE_VIEW_FROM_LITERAL(
                          "XQST0057 a schema import binds a prefix to no namespace"));
    }
    if (kind != SXML_AST_KIND_NAMESPACE_DECLARATION && uri.index == SXML_NAMESPACE_NONE) {
        /* 5.2, 5.12: a target namespace is never empty. */
        return report(reader, declaration,
                      SXML_BYTE_VIEW_FROM_LITERAL("XQST0088 a target namespace is empty"));
    }
    if (is_xml_namespace(uri)) {
        return report(reader, declaration,
                      SXML_BYTE_VIEW_FROM_LITERAL(
                          "XQST0070 the XML namespaces cannot be bound to a prefix"));
    }
    sxml_namespace_prefix_identifier prefix = {0};
    status = intern_prefix(reader, prefix_node, &prefix);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (kind == SXML_AST_KIND_MODULE_IMPORT && list->binding_count > 0 &&
        list->bindings[0].namespace_prefix.index == prefix.index &&
        list->bindings[0].namespace_uri.index == uri.index &&
        reader->request->target_namespace.index == uri.index) {
        /* 5.12: the module declaration's prefix may be imported again with
           the same target namespace. */
        return SXML_STATUS_OK;
    }
    /* 5.13: "" removes the binding: a binding to no namespace, which the
       core reads as an undeclared prefix (D135). */
    list->bindings[list->binding_count] =
        (sxml_namespace_binding){.namespace_prefix = prefix, .namespace_uri = uri};
    list->binding_count += 1;
    return SXML_STATUS_OK;
}

/* XQST0033: a prefix declared twice (5.13). positions[n] is the
   declaration of binding n. */
typedef struct prefix_check {
    const sxml_namespace_binding *bindings;
    const sxml_ast_node_index *declarations;
    size_t binding_count;
} prefix_check;

static sxml_status check_prefixes(const static_reader *reader, prefix_check check) {
    if (check.binding_count < 2) {
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    sxml_status status = allocate_array(
        reader,
        (array_request){.element_count = check.binding_count,
                        .element_size = sizeof(sort_entry),
                        .element_alignment = alignof(sort_entry)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sort_entry *entries = memory;
    for (size_t binding_index = 0; binding_index < check.binding_count; binding_index += 1) {
        entries[binding_index] = (sort_entry){
            .key = check.bindings[binding_index].namespace_prefix.index,
            .position = binding_index};
    }
    sort_entries(entries, check.binding_count);
    for (size_t entry_index = 1; status == SXML_STATUS_OK && entry_index < check.binding_count;
         entry_index += 1) {
        if (entries[entry_index].key == entries[entry_index - 1].key) {
            status = report(reader, check.declarations[entries[entry_index].position],
                            SXML_BYTE_VIEW_FROM_LITERAL(
                                "XQST0033 the prolog declares a namespace prefix twice"));
        }
    }
    sxml_allocator_release(reader->allocator, memory);
    return status;
}

/* True when bindings[0 .. binding_count) binds prefix. */
static bool binds_prefix(const sxml_namespace_binding *bindings, size_t binding_count,
                         sxml_namespace_prefix_identifier prefix) {
    for (size_t binding_index = 0; binding_index < binding_count; binding_index += 1) {
        if (bindings[binding_index].namespace_prefix.index == prefix.index) {
            return true;
        }
    }
    return false;
}

/* Makes context->namespace_bindings (see the header). */
static sxml_status read_namespaces(const static_reader *reader, const prolog_survey *survey,
                                   sxml_xquery_static_context *context) {
    const sxml_ast *ast = reader->ast;
    const sxml_namespace_binding_view host =
        reader->request->host_static_context->namespace_bindings;
    /* Prolog declarations, the default element namespace, the host's,
       local, output and xq. */
    const size_t capacity = survey->namespace_declaration_count + survey->module_import_count +
                            survey->schema_import_count + 2 + host.namespace_binding_count + 3;
    void *memory = NULL;
    sxml_status status = allocate_array(
        reader,
        (array_request){.element_count = capacity,
                        .element_size = sizeof(sxml_namespace_binding),
                        .element_alignment = alignof(sxml_namespace_binding)},
        &memory);
    void *declaration_memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_array(
            reader,
            (array_request){.element_count = capacity,
                            .element_size = sizeof(sxml_ast_node_index),
                            .element_alignment = alignof(sxml_ast_node_index)},
            &declaration_memory);
    }
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(reader->allocator, memory);
        return status;
    }
    binding_list list = {.bindings = memory, .binding_count = 0};
    sxml_ast_node_index *declarations = declaration_memory;
    const sxml_ast_node_index module = child_of(ast, ast->root, SXML_AST_FIELD_MODULE);
    if (node_of(ast, module)->kind == SXML_AST_KIND_LIBRARY_MODULE) {
        /* 5.2: the module declaration's prefix comes first. */
        const sxml_ast_node_index declaration = child_of(ast, module, SXML_AST_FIELD_DECLARATION);
        declarations[0] = declaration;
        status = read_namespace_declaration(reader, declaration, &list);
    }
    for (sxml_ast_node_index declaration = first_declaration(ast);
         status == SXML_STATUS_OK && declaration.value != 0;
         declaration = next_of(ast, declaration)) {
        const sxml_ast_kind kind = node_of(ast, declaration)->kind;
        if (kind == SXML_AST_KIND_NAMESPACE_DECLARATION ||
            ((kind == SXML_AST_KIND_MODULE_IMPORT || kind == SXML_AST_KIND_SCHEMA_IMPORT) &&
             declared_prefix(ast, declaration).value != 0)) {
            declarations[list.binding_count] = declaration;
            status = read_namespace_declaration(reader, declaration, &list);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = check_prefixes(reader, (prefix_check){.bindings = list.bindings,
                                                       .declarations = declarations,
                                                       .binding_count = list.binding_count});
    }
    if (status == SXML_STATUS_OK && survey->default_element_namespace.value != 0) {
        const sxml_byte_view uri_text =
            text_of(ast, child_of(ast, survey->default_element_namespace, SXML_AST_FIELD_URI));
        sxml_namespace_uri_identifier uri = {0};
        status = intern_uri(reader, uri_text, &uri);
        if (status == SXML_STATUS_OK && uri_text.byte_count == 5 &&
            memcmp(uri_text.bytes, "##any", 5) == 0) {
            /* 5.14.1: "##any" binds no namespace for constructed elements
               and makes unprefixed name tests wildcards. */
            context->xpath.has_any_default_element_namespace = true;
            uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_NONE);
        }
        if (status == SXML_STATUS_OK && is_xml_namespace(uri)) {
            status = report(reader, survey->default_element_namespace,
                            SXML_BYTE_VIEW_FROM_LITERAL(
                                "XQST0070 the XML namespaces cannot be the default namespace"));
        }
        if (status == SXML_STATUS_OK) {
            list.bindings[list.binding_count] = (sxml_namespace_binding){
                .namespace_prefix = {.index = 0}, .namespace_uri = uri};
            list.binding_count += 1;
        }
    }
    const size_t prolog_binding_count = list.binding_count;
    for (size_t host_index = 0;
         status == SXML_STATUS_OK && host_index < host.namespace_binding_count;
         host_index += 1) {
        const sxml_namespace_binding binding = host.namespace_bindings[host_index];
        if (!binds_prefix(list.bindings, prolog_binding_count, binding.namespace_prefix)) {
            list.bindings[list.binding_count] = binding;
            list.binding_count += 1;
        }
    }
    /* 2.1.4: XQuery also predeclares local, output and xq. */
    static const struct {
        sxml_byte_view prefix;
        sxml_byte_view namespace_uri;
    } predeclared[] = {
        {SXML_BYTE_VIEW_INITIALIZER("local"),
         SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/2005/xquery-local-functions")},
        {SXML_BYTE_VIEW_INITIALIZER("output"),
         SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/2010/xslt-xquery-serialization")},
        {SXML_BYTE_VIEW_INITIALIZER("xq"), SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/2012/xquery")},
    };
    for (size_t predeclared_index = 0;
         status == SXML_STATUS_OK && predeclared_index < sizeof(predeclared) / sizeof(predeclared[0]);
         predeclared_index += 1) {
        sxml_namespace_prefix_identifier prefix = {0};
        status = sxml_name_pool_intern_namespace_prefix(
            reader->name_pool, predeclared[predeclared_index].prefix, &prefix);
        sxml_namespace_uri_identifier uri = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_intern_namespace_uri(
                reader->name_pool, predeclared[predeclared_index].namespace_uri, &uri);
        }
        if (status == SXML_STATUS_OK && !binds_prefix(list.bindings, list.binding_count, prefix)) {
            list.bindings[list.binding_count] =
                (sxml_namespace_binding){.namespace_prefix = prefix, .namespace_uri = uri};
            list.binding_count += 1;
        }
    }
    sxml_allocator_release(reader->allocator, declaration_memory);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(reader->allocator, memory);
        return status;
    }
    context->namespace_bindings = list.bindings;
    context->namespace_binding_count = list.binding_count;
    context->xpath.namespace_bindings = (sxml_namespace_binding_view){
        .namespace_bindings = list.bindings, .namespace_binding_count = list.binding_count};
    context->xpath.function_namespace_rule = SXML_XPATH_FUNCTION_NAMESPACE_RULE_NO_NAMESPACE_FIRST;
    if (survey->default_function_namespace.value != 0) {
        context->has_default_function_namespace = true;
        context->xpath.function_namespace_rule = SXML_XPATH_FUNCTION_NAMESPACE_RULE_DECLARED;
        status = intern_uri(
            reader,
            text_of(ast, child_of(ast, survey->default_function_namespace, SXML_AST_FIELD_URI)),
            &context->default_function_namespace);
        context->xpath.default_function_namespace = context->default_function_namespace;
        if (status == SXML_STATUS_OK && is_xml_namespace(context->default_function_namespace)) {
            status = report(reader, survey->default_function_namespace,
                            SXML_BYTE_VIEW_FROM_LITERAL(
                                "XQST0070 the XML namespaces cannot be the default namespace"));
        }
    }
    return status;
}

/* ----------------------------------------------------- Base URI, collation */

/* The base URI declaration (5.5), resolved against the host's static base
   URI, and the default collation declaration (5.4), which must name a
   collation sxml supports (XQST0038). */
/* The text of a URILiteral, whitespace normalized (A.3.1): interned as a
   namespace URI, so the name pool owns it. */
static sxml_status normalize_uri_text(const static_reader *reader, sxml_byte_view text,
                                      sxml_byte_view *normalized) {
    sxml_namespace_uri_identifier uri = {0};
    sxml_status status = intern_uri(reader, text, &uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_namespace_uri(reader->name_pool, uri, normalized);
    }
    return status;
}

/* True when collation_uri is one sxml supports or the host binds. */
static bool is_known_collation(const sxml_xquery_static_context *context,
                               sxml_byte_view collation_uri) {
    const sxml_collation_binding_view bindings =
        context->xpath.unit_static_context.collation_bindings;
    for (size_t binding_index = 0; binding_index < bindings.collation_binding_count;
         binding_index += 1) {
        if (sxml_byte_view_equals(bindings.collation_bindings[binding_index].collation_uri,
                                  collation_uri)) {
            return true;
        }
    }
    return sxml_collation_is_supported(collation_uri);
}

static sxml_status read_base_uri_and_collation(const static_reader *reader,
                                               const prolog_survey *survey,
                                               sxml_xquery_static_context *context) {
    const sxml_ast *ast = reader->ast;
    sxml_status status = SXML_STATUS_OK;
    if (survey->base_uri_declaration.value != 0) {
        sxml_byte_view reference = {0};
        status = normalize_uri_text(
            reader, text_of(ast, child_of(ast, survey->base_uri_declaration, SXML_AST_FIELD_URI)),
            &reference);
        sxml_uri_resolution resolution = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_uri_resolve(
                &(sxml_uri_resolution_request){
                    .reference = reference,
                    .base_uri = reader->request->host_static_context->static_base_uri,
                    .allocator = reader->allocator},
                &resolution);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        context->static_base_uri_memory = resolution.memory;
        context->xpath.static_base_uri = resolution.uri;
    }
    if (survey->default_collation_declaration.value != 0) {
        sxml_byte_view reference = {0};
        status = normalize_uri_text(
            reader,
            text_of(ast, child_of(ast, survey->default_collation_declaration, SXML_AST_FIELD_URI)),
            &reference);
        sxml_uri_resolution resolution = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_uri_resolve(
                &(sxml_uri_resolution_request){.reference = reference,
                                               .base_uri = context->xpath.static_base_uri,
                                               .allocator = reader->allocator},
                &resolution);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        context->default_collation_memory = resolution.memory;
        if (!is_known_collation(context, resolution.uri)) {
            return report(reader, survey->default_collation_declaration,
                          SXML_BYTE_VIEW_FROM_LITERAL(
                              "XQST0038 the default collation is not supported"));
        }
        /* 5.4; the unit's collation resolver reads it (D131). */
        context->xpath.unit_static_context.default_collation_uri = resolution.uri;
    }
    return status;
}

/* ---------------------------------------------------- Schema imports */

/* The location hints of a SCHEMA_IMPORT, as STRING_LITERAL nodes. */
static size_t count_locations(const sxml_ast *ast, sxml_ast_node_index declaration) {
    size_t location_count = 0;
    for (sxml_ast_node_index location = child_of(ast, declaration, SXML_AST_FIELD_LOCATION);
         location.value != 0; location = sxml_ast_find_next_with_same_field(ast, location)) {
        location_count += 1;
    }
    return location_count;
}

/* True when a host schema defines namespace_uri: its import is taken from
   there, not read again (D174). The XML Schema namespace is every
   schema's (Part 1, 3.4.7, 3.16.7). */
static bool is_in_host_schemas(const static_reader *reader,
                               sxml_namespace_uri_identifier namespace_uri) {
    if (is_well_known(namespace_uri, SXML_NAMESPACE_XML_SCHEMA)) {
        return true;
    }
    const sxml_schema_view host =
        reader->request->host_static_context->unit_static_context.in_scope_schemas;
    for (size_t schema_index = 0; schema_index < host.schema_count; schema_index += 1) {
        if (sxml_schema_scope_defines_namespace(host.schemas[schema_index], namespace_uri)) {
            return true;
        }
    }
    return false;
}

/* The error code a schema error of the imports is raised with (5.11):
   XQST0059 when a document cannot be read or has another target
   namespace (src-import, schema_reference), XQST0035 for two components
   of one name (sch-props-correct.2), XQST0012 for any other breach of
   XSD's constraints. */
static sxml_byte_view schema_import_error_code(const sxml_error_record *record) {
    const sxml_byte_view constraint = record->code.local_name;
    if (sxml_byte_view_equals(constraint, SXML_BYTE_VIEW_FROM_LITERAL("src-import")) ||
        sxml_byte_view_equals(constraint, SXML_BYTE_VIEW_FROM_LITERAL("schema_reference"))) {
        return SXML_BYTE_VIEW_FROM_LITERAL("XQST0059");
    }
    if (sxml_byte_view_equals(constraint, SXML_BYTE_VIEW_FROM_LITERAL("sch-props-correct.2"))) {
        return SXML_BYTE_VIEW_FROM_LITERAL("XQST0035");
    }
    return SXML_BYTE_VIEW_FROM_LITERAL("XQST0012");
}

/* Fills documents[0 ..) with the documents of the imports of namespaces
   no host schema defines, after XQST0058 (one namespace imported twice);
   *document_count is how many. */
typedef struct schema_documents {
    sxml_schema_document *documents;
    size_t document_count;
    /* The first import that has documents, for errors. */
    sxml_ast_node_index first_import;
    /* Location hints the resolver does not deliver are left out, and an
       import none of whose hints it delivers asks by namespace alone
       (5.11: hints may be ignored); set after a first compilation found a
       document missing. */
    bool probes_hints;
} schema_documents;

/* True when the resolver delivers the schema document hint names,
   resolved against the static base URI. Reads it once and releases it. */
typedef struct hint_delivery {
    bool is_delivered;
} hint_delivery;

static sxml_status probe_hint(const static_reader *reader, sxml_schema_document document,
                              hint_delivery *delivery) {
    sxml_schema_scope_delivery delivered = {.is_delivered = false};
    const sxml_status status = sxml_schema_scope_probe_document(
        &(sxml_schema_scope_probe){.processor = reader->context->xpath.processor,
                                   .document = document,
                                   .base_uri = reader->context->xpath.static_base_uri},
        &delivered);
    delivery->is_delivered = delivered.is_delivered;
    return status;
}

/* Adds the documents of one import: one per location hint (those the
   resolver delivers, when probing), or one by namespace alone. */
typedef struct import_documents {
    sxml_ast_node_index declaration;
    sxml_byte_view namespace_text;
} import_documents;

static sxml_status add_import_documents(const static_reader *reader, import_documents import,
                                        schema_documents *collected) {
    const sxml_ast_node_index declaration = import.declaration;
    const sxml_byte_view namespace_text = import.namespace_text;
    const sxml_ast *ast = reader->ast;
    const sxml_schema_document document = {
        .document_source = SXML_SCHEMA_DOCUMENT_SOURCE_RESOLVED,
        .document_uri = {.bytes = NULL, .byte_count = 0},
        .checks_target_namespace = true,
        .target_namespace_uri = namespace_text};
    const size_t first_document = collected->document_count;
    sxml_status status = SXML_STATUS_OK;
    for (sxml_ast_node_index location = child_of(ast, declaration, SXML_AST_FIELD_LOCATION);
         status == SXML_STATUS_OK && location.value != 0;
         location = sxml_ast_find_next_with_same_field(ast, location)) {
        sxml_schema_document hinted = document;
        status = normalize_uri_text(reader, text_of(ast, location), &hinted.document_uri);
        hint_delivery delivery = {.is_delivered = true};
        if (status == SXML_STATUS_OK && collected->probes_hints) {
            status = probe_hint(reader, hinted, &delivery);
        }
        if (status == SXML_STATUS_OK && delivery.is_delivered) {
            collected->documents[collected->document_count] = hinted;
            collected->document_count += 1;
        }
    }
    if (status == SXML_STATUS_OK && collected->document_count == first_document) {
        /* No hint, or none delivered: the resolver chooses by namespace. */
        collected->documents[collected->document_count] = document;
        collected->document_count += 1;
    }
    return status;
}

static sxml_status collect_schema_documents(const static_reader *reader,
                                            schema_documents *collected) {
    const sxml_ast *ast = reader->ast;
    collected->document_count = 0;
    sxml_status status = SXML_STATUS_OK;
    for (sxml_ast_node_index declaration = first_declaration(ast);
         status == SXML_STATUS_OK && declaration.value != 0;
         declaration = next_of(ast, declaration)) {
        if (node_of(ast, declaration)->kind != SXML_AST_KIND_SCHEMA_IMPORT) {
            continue;
        }
        sxml_namespace_uri_identifier namespace_uri = {0};
        status = intern_uri(reader, text_of(ast, child_of(ast, declaration, SXML_AST_FIELD_URI)),
                            &namespace_uri);
        for (sxml_ast_node_index earlier = first_declaration(ast);
             status == SXML_STATUS_OK && earlier.value != declaration.value;
             earlier = next_of(ast, earlier)) {
            sxml_namespace_uri_identifier earlier_uri = {0};
            if (node_of(ast, earlier)->kind != SXML_AST_KIND_SCHEMA_IMPORT) {
                continue;
            }
            status = intern_uri(reader, text_of(ast, child_of(ast, earlier, SXML_AST_FIELD_URI)),
                                &earlier_uri);
            if (status == SXML_STATUS_OK && earlier_uri.index == namespace_uri.index) {
                status = report(reader, declaration,
                                SXML_BYTE_VIEW_FROM_LITERAL(
                                    "XQST0058 two schema imports name one target namespace"));
            }
        }
        if (status != SXML_STATUS_OK || is_in_host_schemas(reader, namespace_uri)) {
            continue;
        }
        sxml_byte_view namespace_text = {0};
        status = sxml_name_pool_get_namespace_uri(reader->name_pool, namespace_uri,
                                                  &namespace_text);
        if (collected->first_import.value == 0) {
            collected->first_import = declaration;
        }
        if (status == SXML_STATUS_OK) {
            status = add_import_documents(
                reader,
                (import_documents){.declaration = declaration, .namespace_text = namespace_text},
                collected);
        }
    }
    return status;
}

/* Compiles collected's documents into *schema, the schema's first error
   in *schema_error. */
typedef struct imported_schema {
    sxml_error_record *schema_error;
    sxml_schema **schema;
} imported_schema;

static sxml_status compile_imported_schema(const static_reader *reader,
                                           const schema_documents *collected,
                                           imported_schema result) {
    sxml_error_record *schema_error = result.schema_error;
    sxml_schema **schema = result.schema;
    memset(schema_error, 0, sizeof *schema_error);
    return sxml_schema_create(
        reader->allocator,
        &(sxml_schema_configuration){
            .processor = reader->context->xpath.processor,
            .schema_documents = {.schema_documents = collected->documents,
                                 .schema_document_count = collected->document_count},
            .base_uri = reader->context->xpath.static_base_uri,
            .report_sink = {.state = NULL, .operations = NULL},
            .maximum_content_model_state_count = 0,
            .error_record = schema_error},
        schema);
}

/* Adds schema after the host's in-scope schemas. */
static sxml_status add_in_scope_schema(const static_reader *reader, const sxml_schema *schema,
                                       sxml_xquery_static_context *context) {
    const sxml_schema_view host = context->xpath.unit_static_context.in_scope_schemas;
    void *schema_memory = NULL;
    const sxml_status status =
        allocate_array(reader,
                       (array_request){.element_count = host.schema_count + 1,
                                       .element_size = sizeof(const sxml_schema *),
                                       .element_alignment = alignof(const sxml_schema *)},
                       &schema_memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_schema **schemas = (const sxml_schema **)schema_memory;
    for (size_t schema_index = 0; schema_index < host.schema_count; schema_index += 1) {
        schemas[schema_index] = host.schemas[schema_index];
    }
    schemas[host.schema_count] = schema;
    context->in_scope_schemas = schemas;
    context->xpath.unit_static_context.in_scope_schemas =
        (sxml_schema_view){.schemas = schemas, .schema_count = host.schema_count + 1};
    return SXML_STATUS_OK;
}

/* Compiles the imports of the namespaces the host's schemas do not
   define into one schema the processor keeps, added to the in-scope
   schemas. */
static sxml_status compile_schema_imports(const static_reader *reader,
                                          sxml_xquery_static_context *context) {
    const sxml_ast *ast = reader->ast;
    size_t capacity = 0;
    for (sxml_ast_node_index declaration = first_declaration(ast); declaration.value != 0;
         declaration = next_of(ast, declaration)) {
        if (node_of(ast, declaration)->kind == SXML_AST_KIND_SCHEMA_IMPORT) {
            const size_t location_count = count_locations(ast, declaration);
            capacity += location_count == 0 ? 1 : location_count;
        }
    }
    void *memory = NULL;
    sxml_status status = allocate_array(
        reader,
        (array_request){.element_count = capacity,
                        .element_size = sizeof(sxml_schema_document),
                        .element_alignment = alignof(sxml_schema_document)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    schema_documents collected = {.documents = memory, .document_count = 0};
    status = collect_schema_documents(reader, &collected);
    sxml_schema *schema = NULL;
    sxml_error_record schema_error;
    const bool compiles = collected.document_count > 0;
    if (status == SXML_STATUS_OK && compiles && context->xpath.processor == NULL) {
        status = report(reader, collected.first_import,
                        SXML_BYTE_VIEW_FROM_LITERAL(
                            "XQST0059 schema documents cannot be read here"));
    } else if (status == SXML_STATUS_OK && compiles) {
        status = compile_imported_schema(
                reader, &collected, (imported_schema){.schema_error = &schema_error, .schema = &schema});
        if (status == SXML_STATUS_STATIC_ERROR &&
            sxml_byte_view_equals(schema_error.code.local_name,
                                  SXML_BYTE_VIEW_FROM_LITERAL("src-import"))) {
            /* A location hint that is not delivered (5.11): again without
               the hints the resolver does not deliver. */
            sxml_item_heap_clear_raised_error(context->xpath.item_heap);
            collected.probes_hints = true;
            status = collect_schema_documents(reader, &collected);
            if (status == SXML_STATUS_OK) {
                status = compile_imported_schema(
                reader, &collected, (imported_schema){.schema_error = &schema_error, .schema = &schema});
            }
        }
        if (status == SXML_STATUS_STATIC_ERROR && schema_error.code.local_name.byte_count != 0) {
            status = sxml_schema_scope_raise(&(sxml_schema_scope_error){
                .item_heap = context->xpath.item_heap,
                .allocator = reader->allocator,
                .error_status = SXML_STATUS_STATIC_ERROR,
                .error_code = schema_import_error_code(&schema_error),
                .schema_error = &schema_error,
                .error_record = reader->request->error_record,
                .location = sxml_ast_get_source_location(ast, collected.first_import)});
        }
    }
    sxml_allocator_release(reader->allocator, memory);
    if (status != SXML_STATUS_OK || schema == NULL) {
        return status;
    }
    status = sxml_processor_keep_schema(context->xpath.processor, schema);
    if (status != SXML_STATUS_OK) {
        sxml_schema_destroy(schema);
        return status;
    }
    return add_in_scope_schema(reader, schema, context);
}

/*
 * The in-scope schema definitions (5.11; D174): the host's schemas, then
 * one schema compiled from the imports of the namespaces the host's do
 * not define, against the static base URI; or, for a main module
 * without schemas, the schema of the built-in components alone (XPath
 * 4.0, 2.2.1: the in-scope schema types include the built-in types), so
 * that validation (lax, xsi:type, by a built-in type) always has a schema.
 * The processor owns both schemas (sxml_processor_keep_schema,
 * sxml_processor_get_builtin_schema).
 */
static sxml_status read_schema_imports(const static_reader *reader, const prolog_survey *survey,
                                       sxml_xquery_static_context *context) {
    sxml_status status = SXML_STATUS_OK;
    if (survey->schema_import_count > 0) {
        status = compile_schema_imports(reader, context);
    }
    if (status == SXML_STATUS_OK && reader->request->target_namespace.index == 0 &&
        context->xpath.unit_static_context.in_scope_schemas.schema_count == 0 &&
        context->xpath.processor != NULL) {
        const sxml_schema *builtin_schema = NULL;
        status = sxml_processor_get_builtin_schema(context->xpath.processor, &builtin_schema);
        if (status == SXML_STATUS_OK) {
            status = add_in_scope_schema(reader, builtin_schema, context);
        }
    }
    return status;
}

/* ---------------------------------------------------- Decimal formats */

static sxml_status resolve_variable_name(const static_reader *reader,
                                         sxml_ast_node_index name_node,
                                         sxml_name_identifier *name);

/* The kinds whose value is one marker character (F&O 4.0, 4.9.1). */
static bool is_marker_only(sxml_decimal_format_property_kind kind) {
    return kind == SXML_DECIMAL_FORMAT_PROPERTY_ZERO_DIGIT ||
           kind == SXML_DECIMAL_FORMAT_PROPERTY_DIGIT ||
           kind == SXML_DECIMAL_FORMAT_PROPERTY_PATTERN_SEPARATOR;
}

/* The kinds whose value is a marker and an optional ":" rendition. */
static bool has_marker(sxml_decimal_format_property_kind kind) {
    return is_marker_only(kind) || kind == SXML_DECIMAL_FORMAT_PROPERTY_DECIMAL_SEPARATOR ||
           kind == SXML_DECIMAL_FORMAT_PROPERTY_GROUPING_SEPARATOR ||
           kind == SXML_DECIMAL_FORMAT_PROPERTY_EXPONENT_SEPARATOR ||
           kind == SXML_DECIMAL_FORMAT_PROPERTY_PERCENT ||
           kind == SXML_DECIMAL_FORMAT_PROPERTY_PER_MILLE;
}

/* The bytes of the first UTF-8 character of text (1 for a stray byte). */
static size_t first_character_byte_count(sxml_byte_view text) {
    if (text.byte_count == 0) {
        return 0;
    }
    const uint8_t first = text.bytes[0];
    const size_t byte_count = first < 0x80U ? 1U : first < 0xE0U ? 2U : first < 0xF0U ? 3U : 4U;
    return byte_count <= text.byte_count ? byte_count : 1U;
}

/* XQST0097 (5.10): a property whose value has the wrong form, judged
   alone: one character for the markers, one character with an optional
   ":" rendition for the markers that have a rendition, and a zero-digit
   that F&O 4.0 accepts. */
static bool has_valid_form(const sxml_decimal_format_property *property) {
    const sxml_byte_view value = property->property_value;
    if (!has_marker(property->property_kind)) {
        return true;
    }
    const size_t marker_count = first_character_byte_count(value);
    if (marker_count == 0 || value.bytes == NULL) {
        return false;
    }
    if (is_marker_only(property->property_kind) && marker_count != value.byte_count) {
        return false;
    }
    if (marker_count != value.byte_count && value.bytes[marker_count] != ':') {
        return false;
    }
    if (property->property_kind == SXML_DECIMAL_FORMAT_PROPERTY_ZERO_DIGIT) {
        const sxml_decimal_format_settings defaults = sxml_decimal_format_default_settings();
        sxml_decimal_format_settings settings = defaults;
        return sxml_decimal_format_apply(
                   &(sxml_decimal_format_change){
                       .base_settings = &defaults,
                       .decimal_format_properties = {.decimal_format_properties = property,
                                                     .decimal_format_property_count = 1}},
                   &settings) == SXML_STATUS_OK;
    }
    return true;
}

/* One decimal format declaration's properties into properties[...] from
   *property_count on: XQST0114 for one twice, XQST0097 or XQST0098 for
   values the format cannot take. */
typedef struct property_list {
    sxml_decimal_format_property *properties;
    size_t property_count;
} property_list;

static sxml_status read_decimal_format_properties(const static_reader *reader,
                                                  sxml_ast_node_index declaration,
                                                  property_list *list) {
    const sxml_ast *ast = reader->ast;
    const size_t first = list->property_count;
    for (sxml_ast_node_index property = child_of(ast, declaration, SXML_AST_FIELD_PROPERTY);
         property.value != 0; property = next_of(ast, property)) {
        const sxml_decimal_format_property read = {
            .property_kind = node_of(ast, property)->detail.decimal_format_property,
            .property_value = text_of(ast, child_of(ast, property, SXML_AST_FIELD_VALUE))};
        for (size_t earlier_index = first; earlier_index < list->property_count;
             earlier_index += 1) {
            if (list->properties[earlier_index].property_kind == read.property_kind) {
                return report(reader, property,
                              SXML_BYTE_VIEW_FROM_LITERAL(
                                  "XQST0114 a decimal format declares a property twice"));
            }
        }
        if (!has_valid_form(&read)) {
            return report(reader, property,
                          SXML_BYTE_VIEW_FROM_LITERAL(
                              "XQST0097 a decimal format property has an invalid value"));
        }
        list->properties[list->property_count] = read;
        list->property_count += 1;
    }
    const sxml_decimal_format_settings defaults = sxml_decimal_format_default_settings();
    sxml_decimal_format_settings settings = defaults;
    const sxml_status status = sxml_decimal_format_apply(
        &(sxml_decimal_format_change){
            .base_settings = &defaults,
            .decimal_format_properties = {.decimal_format_properties = &list->properties[first],
                                          .decimal_format_property_count =
                                              list->property_count - first}},
        &settings);
    if (status == SXML_STATUS_MALFORMED_INPUT) {
        /* Each value has the right form, so the markers are not distinct
           (F&O 4.0, 4.9.1). */
        return report(reader, declaration,
                      SXML_BYTE_VIEW_FROM_LITERAL(
                          "XQST0098 the markers of a decimal format are not distinct"));
    }
    return status;
}

/* The decimal formats (5.10): the prolog's, then the host's of other
   names; XQST0111 for a name declared twice (the unnamed one included). */
static sxml_status read_decimal_formats(const static_reader *reader,
                                        const prolog_survey *survey,
                                        sxml_xquery_static_context *context) {
    const sxml_ast *ast = reader->ast;
    if (survey->decimal_format_declaration_count == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_decimal_format_view host =
        reader->request->host_static_context->unit_static_context.decimal_formats;
    void *memory = NULL;
    sxml_status status = allocate_array(
        reader,
        (array_request){.element_count =
                            survey->decimal_format_declaration_count + host.decimal_format_count,
                        .element_size = sizeof(sxml_decimal_format),
                        .element_alignment = alignof(sxml_decimal_format)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    context->decimal_formats = memory;
    status = allocate_array(
        reader,
        (array_request){.element_count = survey->decimal_format_property_count,
                        .element_size = sizeof(sxml_decimal_format_property),
                        .element_alignment = alignof(sxml_decimal_format_property)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    context->decimal_format_properties = memory;
    property_list list = {.properties = context->decimal_format_properties,
                          .property_count = 0};
    size_t format_count = 0;
    for (sxml_ast_node_index declaration = first_declaration(ast);
         status == SXML_STATUS_OK && declaration.value != 0;
         declaration = next_of(ast, declaration)) {
        if (node_of(ast, declaration)->kind != SXML_AST_KIND_DECIMAL_FORMAT_DECLARATION) {
            continue;
        }
        sxml_decimal_format *format = &context->decimal_formats[format_count];
        *format = (sxml_decimal_format){.format_name = {.index = 0}};
        const sxml_ast_node_index name = child_of(ast, declaration, SXML_AST_FIELD_NAME);
        if (name.value != 0) {
            status = resolve_variable_name(reader, name, &format->format_name);
        }
        for (size_t earlier_index = 0; status == SXML_STATUS_OK && earlier_index < format_count;
             earlier_index += 1) {
            if (context->decimal_formats[earlier_index].format_name.index ==
                format->format_name.index) {
                status = report(reader, declaration,
                                SXML_BYTE_VIEW_FROM_LITERAL(
                                    "XQST0111 two decimal formats have the same name"));
            }
        }
        const size_t first_property = list.property_count;
        if (status == SXML_STATUS_OK) {
            status = read_decimal_format_properties(reader, declaration, &list);
        }
        format->decimal_format_properties = (sxml_decimal_format_property_view){
            .decimal_format_properties = &list.properties[first_property],
            .decimal_format_property_count = list.property_count - first_property};
        format_count += 1;
    }
    const size_t prolog_count = format_count;
    for (size_t host_index = 0; status == SXML_STATUS_OK && host_index < host.decimal_format_count;
         host_index += 1) {
        bool is_replaced = false;
        for (size_t prolog_index = 0; prolog_index < prolog_count; prolog_index += 1) {
            if (context->decimal_formats[prolog_index].format_name.index ==
                host.decimal_formats[host_index].format_name.index) {
                is_replaced = true;
            }
        }
        if (!is_replaced) {
            context->decimal_formats[format_count] = host.decimal_formats[host_index];
            format_count += 1;
        }
    }
    context->xpath.unit_static_context.decimal_formats = (sxml_decimal_format_view){
        .decimal_formats = context->decimal_formats, .decimal_format_count = format_count};
    return status;
}

/* --------------------------------------------------------- Annotations */

/* The expanded name of an annotation (5.15: an unprefixed name is in the
   XQuery namespace) or an option. */
typedef struct expanded_name {
    sxml_namespace_uri_identifier namespace_uri;
    sxml_byte_view local_name;
} expanded_name;

static sxml_status resolve_xquery_name(const static_reader *reader,
                                       sxml_ast_node_index name_node, expanded_name *name) {
    const sxml_xquery_static_context *context = reader->context;
    sxml_xpath_resolved_name resolved = {0};
    sxml_status status = sxml_xpath_resolve_name(
        &(sxml_xpath_name_request){.ast = reader->ast,
                                   .name_node = name_node,
                                   .name_role = SXML_XPATH_NAME_ROLE_NO_NAMESPACE,
                                   .static_context = &context->xpath,
                                   .error_record = reader->request->error_record},
        &resolved);
    if (status == SXML_STATUS_OK && node_of(reader->ast, name_node)->kind == SXML_AST_KIND_NCNAME) {
        status = sxml_name_pool_intern_namespace_uri(reader->name_pool, xquery_namespace_text,
                                                     &resolved.namespace_uri);
    }
    if (status == SXML_STATUS_OK) {
        *name = (expanded_name){.namespace_uri = resolved.namespace_uri,
                                .local_name = resolved.local_name};
    }
    return status;
}

/* Checks the annotations of a variable or function declaration: none in a
   reserved namespace but %public and %private (XQST0045), and at most one
   of those (duplicate_error: XQST0116 for variables, XQST0106 for
   functions). The XQuery Update Facility 3.0 (5.4, D187) adds %updating
   and %simple, and "declare updating": on a function at most one of them
   (XUST0033), on a variable none (XUST0032). */
typedef struct annotation_check {
    sxml_ast_node_index declaration;
    sxml_byte_view duplicate_error;
    /* A record declaration: %constructor is known (5.20.2). */
    bool allows_constructor;
    /* Receives whether %private is among them; may be NULL. */
    bool *is_private;
} annotation_check;

/* An update annotation (%updating, %simple or "declare updating") of a
   declaration, counted in *count. */
typedef struct update_annotation_check {
    sxml_ast_node_index annotation;
    sxml_ast_kind declaration_kind;
    size_t *count;
} update_annotation_check;

static sxml_status check_update_annotation(const static_reader *reader,
                                           update_annotation_check check) {
    if (check.declaration_kind == SXML_AST_KIND_VARIABLE_DECLARATION) {
        return report(reader, check.annotation,
                      SXML_BYTE_VIEW_FROM_LITERAL(
                          "XUST0032 a variable declaration is annotated %updating or %simple"));
    }
    if (check.declaration_kind != SXML_AST_KIND_FUNCTION_DECLARATION) {
        return report(reader, check.annotation,
                      SXML_BYTE_VIEW_FROM_LITERAL(
                          "XQST0045 an annotation is in a reserved namespace"));
    }
    *check.count += 1;
    if (*check.count > 1) {
        return report(reader, check.annotation,
                      SXML_BYTE_VIEW_FROM_LITERAL(
                          "XUST0033 a function declaration has two of %updating and %simple"));
    }
    return SXML_STATUS_OK;
}

static sxml_status check_annotations(const static_reader *reader, annotation_check check) {
    sxml_namespace_uri_identifier xquery_namespace = {0};
    sxml_status status = sxml_name_pool_intern_namespace_uri(
        reader->name_pool, xquery_namespace_text, &xquery_namespace);
    size_t visibility_count = 0;
    size_t update_annotation_count = 0;
    const sxml_ast_kind declaration_kind = node_of(reader->ast, check.declaration)->kind;
    for (sxml_ast_node_index annotation =
             child_of(reader->ast, check.declaration, SXML_AST_FIELD_ANNOTATION);
         status == SXML_STATUS_OK && annotation.value != 0;
         annotation = next_of(reader->ast, annotation)) {
        bool is_update_annotation =
            node_of(reader->ast, annotation)->kind == SXML_AST_KIND_COMPATIBILITY_ANNOTATION;
        expanded_name name = {0};
        if (is_update_annotation) {
            status = check_update_annotation(
                reader, (update_annotation_check){.annotation = annotation,
                                                  .declaration_kind = declaration_kind,
                                                  .count = &update_annotation_count});
            continue;
        }
        status = resolve_xquery_name(
            reader, child_of(reader->ast, annotation, SXML_AST_FIELD_NAME), &name);
        if (status != SXML_STATUS_OK) {
            break;
        }
        const bool is_visibility =
            name.namespace_uri.index == xquery_namespace.index &&
            (sxml_byte_view_equals(name.local_name, SXML_BYTE_VIEW_FROM_LITERAL("public")) ||
             sxml_byte_view_equals(name.local_name, SXML_BYTE_VIEW_FROM_LITERAL("private")));
        const bool is_constructor =
            check.allows_constructor && name.namespace_uri.index == xquery_namespace.index &&
            sxml_byte_view_equals(name.local_name, SXML_BYTE_VIEW_FROM_LITERAL("constructor"));
        is_update_annotation =
            name.namespace_uri.index == xquery_namespace.index &&
            (sxml_byte_view_equals(name.local_name, SXML_BYTE_VIEW_FROM_LITERAL("updating")) ||
             sxml_byte_view_equals(name.local_name, SXML_BYTE_VIEW_FROM_LITERAL("simple")));
        if (is_constructor) {
            continue;
        }
        if (is_update_annotation &&
            (declaration_kind == SXML_AST_KIND_FUNCTION_DECLARATION ||
             declaration_kind == SXML_AST_KIND_VARIABLE_DECLARATION)) {
            status = check_update_annotation(
                reader, (update_annotation_check){.annotation = annotation,
                                                  .declaration_kind = declaration_kind,
                                                  .count = &update_annotation_count});
        } else if (is_visibility) {
            if (check.is_private != NULL &&
                sxml_byte_view_equals(name.local_name, SXML_BYTE_VIEW_FROM_LITERAL("private"))) {
                *check.is_private = true;
            }
            visibility_count += 1;
            if (visibility_count > 1) {
                status = report(reader, annotation, check.duplicate_error);
            }
        } else if (name.namespace_uri.index == xquery_namespace.index ||
                   is_reserved_namespace(name.namespace_uri)) {
            status = report(reader, annotation,
                            SXML_BYTE_VIEW_FROM_LITERAL(
                                "XQST0045 an annotation is in a reserved namespace"));
        }
    }
    return status;
}

/* XQST0048: a public declaration of a library module whose name is not in
   the target namespace (5.16, 5.18, 5.19, 5.20). */
typedef struct target_namespace_check {
    sxml_ast_node_index declaration;
    sxml_name_identifier name;
    bool is_private;
} target_namespace_check;

static sxml_status check_target_namespace(const static_reader *reader,
                                          target_namespace_check check) {
    const sxml_namespace_uri_identifier target = reader->request->target_namespace;
    if (target.index == 0 || check.is_private) {
        return SXML_STATUS_OK;
    }
    sxml_name_parts parts = {0};
    sxml_status status = sxml_name_pool_get_name_parts(reader->name_pool, check.name, &parts);
    if (status == SXML_STATUS_OK && parts.namespace_uri.index != target.index) {
        status = report(reader, check.declaration,
                        SXML_BYTE_VIEW_FROM_LITERAL(
                            "XQST0048 a public declaration of a library module is not in its "
                            "target namespace"));
    }
    return status;
}

/* ------------------------------------------------------------ Options */

/* The serialization parameters an output declaration may set (5.22;
   Serialization 4.0, section 3): all but use-character-maps, and
   parameter-document. */
static const sxml_byte_view output_parameter_names[] = {
    SXML_BYTE_VIEW_INITIALIZER("allow-duplicate-names"),
    SXML_BYTE_VIEW_INITIALIZER("byte-order-mark"),
    SXML_BYTE_VIEW_INITIALIZER("canonical"),
    SXML_BYTE_VIEW_INITIALIZER("cdata-section-elements"),
    SXML_BYTE_VIEW_INITIALIZER("csv-header"),
    SXML_BYTE_VIEW_INITIALIZER("csv-quote-character"),
    SXML_BYTE_VIEW_INITIALIZER("csv-separator"),
    SXML_BYTE_VIEW_INITIALIZER("doctype-public"),
    SXML_BYTE_VIEW_INITIALIZER("doctype-system"),
    SXML_BYTE_VIEW_INITIALIZER("encoding"),
    SXML_BYTE_VIEW_INITIALIZER("escape-solidus"),
    SXML_BYTE_VIEW_INITIALIZER("escape-uri-attributes"),
    SXML_BYTE_VIEW_INITIALIZER("html-version"),
    SXML_BYTE_VIEW_INITIALIZER("include-content-type"),
    SXML_BYTE_VIEW_INITIALIZER("indent"),
    SXML_BYTE_VIEW_INITIALIZER("indent-attributes"),
    SXML_BYTE_VIEW_INITIALIZER("indent-unit"),
    SXML_BYTE_VIEW_INITIALIZER("item-separator"),
    SXML_BYTE_VIEW_INITIALIZER("json-lines"),
    SXML_BYTE_VIEW_INITIALIZER("json-node-output-method"),
    SXML_BYTE_VIEW_INITIALIZER("line-ending"),
    SXML_BYTE_VIEW_INITIALIZER("media-type"),
    SXML_BYTE_VIEW_INITIALIZER("method"),
    SXML_BYTE_VIEW_INITIALIZER("normalization-form"),
    SXML_BYTE_VIEW_INITIALIZER("omit-xml-declaration"),
    SXML_BYTE_VIEW_INITIALIZER("parameter-document"),
    SXML_BYTE_VIEW_INITIALIZER("standalone"),
    SXML_BYTE_VIEW_INITIALIZER("suppress-indentation"),
    SXML_BYTE_VIEW_INITIALIZER("undeclare-prefixes"),
    SXML_BYTE_VIEW_INITIALIZER("version"),
};

/* The index of an output parameter name; SIZE_MAX for none. */
static size_t find_output_parameter(sxml_byte_view local_name) {
    for (size_t name_index = 0;
         name_index < sizeof(output_parameter_names) / sizeof(output_parameter_names[0]);
         name_index += 1) {
        if (sxml_byte_view_equals(output_parameter_names[name_index], local_name)) {
            return name_index;
        }
    }
    return SIZE_MAX;
}

/* Option declarations (5.21): names must resolve (an unprefixed name is
   in the XQuery namespace); options sxml does not know are ignored. An
   output declaration (5.22) names a serialization parameter (XQST0109)
   at most once (XQST0110); its value is the serializer's to read. */
static sxml_status read_options(const static_reader *reader) {
    const sxml_ast *ast = reader->ast;
    bool is_declared[sizeof(output_parameter_names) / sizeof(output_parameter_names[0])] = {
        false};
    sxml_status status = SXML_STATUS_OK;
    for (sxml_ast_node_index declaration = first_declaration(ast);
         status == SXML_STATUS_OK && declaration.value != 0;
         declaration = next_of(ast, declaration)) {
        if (node_of(ast, declaration)->kind != SXML_AST_KIND_OPTION_DECLARATION) {
            continue;
        }
        expanded_name name = {0};
        status = resolve_xquery_name(reader, child_of(ast, declaration, SXML_AST_FIELD_NAME), &name);
        if (status != SXML_STATUS_OK ||
            !is_well_known(name.namespace_uri, SXML_NAMESPACE_SERIALIZATION)) {
            continue;
        }
        const size_t parameter = find_output_parameter(name.local_name);
        if (reader->request->target_namespace.index != 0) {
            /* 5.22: only a main module declares serialization parameters. */
            status = report(reader, declaration,
                            SXML_BYTE_VIEW_FROM_LITERAL(
                                "XQST0108 a library module has an output declaration"));
        } else if (parameter == SIZE_MAX) {
            status = report(reader, declaration,
                            SXML_BYTE_VIEW_FROM_LITERAL(
                                "XQST0109 an output declaration names no serialization "
                                "parameter"));
        } else if (is_declared[parameter]) {
            status = report(reader, declaration,
                            SXML_BYTE_VIEW_FROM_LITERAL(
                                "XQST0110 an output declaration sets a parameter twice"));
        } else {
            is_declared[parameter] = true;
        }
    }
    return status;
}

/* -------------------------------------------------------------- Types */

/* The sequence type of a SEQUENCE_TYPE node; item()* for none. */
static sxml_status resolve_type_node(const static_reader *reader, sxml_ast_node_index node,
                                     sxml_sequence_type *sequence_type) {
    if (node.value == 0) {
        *sequence_type = (sxml_sequence_type){
            .item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
            .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};
        return SXML_STATUS_OK;
    }
    return sxml_xpath_resolve_sequence_type(
        &(sxml_xpath_type_request){.ast = reader->ast,
                                   .type_node = node,
                                   .static_context = &reader->context->xpath,
                                   .error_record = reader->request->error_record},
        sequence_type);
}

/* The expanded name of a type or record declaration (the default type
   namespace rule: the default element namespace, 5.19, 5.20). */
static sxml_status resolve_type_name(const static_reader *reader, sxml_ast_node_index name_node,
                                     sxml_name_identifier *name) {
    sxml_xpath_resolved_name resolved = {0};
    sxml_status status = sxml_xpath_resolve_name(
        &(sxml_xpath_name_request){.ast = reader->ast,
                                   .name_node = name_node,
                                   .name_role = SXML_XPATH_NAME_ROLE_ELEMENT_OR_TYPE,
                                   .static_context = &reader->context->xpath,
                                   .error_record = reader->request->error_record},
        &resolved);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_name_pool_intern_name(
        reader->name_pool,
        &(sxml_name_request){.namespace_uri = resolved.namespace_uri,
                             .local_name = resolved.local_name},
        name);
}

/* Declares each record's name in the type registry, so every type may name
   it (records may be recursive, 5.20.1). */
static sxml_status declare_records(const static_reader *reader,
                                   sxml_xquery_static_context *context) {
    const sxml_ast *ast = reader->ast;
    sxml_status status = SXML_STATUS_OK;
    for (sxml_ast_node_index declaration = first_declaration(ast);
         status == SXML_STATUS_OK && declaration.value != 0;
         declaration = next_of(ast, declaration)) {
        if (node_of(ast, declaration)->kind != SXML_AST_KIND_NOMINATIVE_RECORD_TYPE_DECLARATION) {
            continue;
        }
        sxml_xquery_declared_record *record =
            &context->declared_records[context->declared_record_count];
        *record = (sxml_xquery_declared_record){.declaration = declaration};
        status = check_annotations(
            reader, (annotation_check){.declaration = declaration,
                                       .duplicate_error = SXML_BYTE_VIEW_FROM_LITERAL(
                                           "XQST0106 a record has two visibility annotations"),
                                       .allows_constructor = true,
                                       .is_private = &record->is_private});
        if (status == SXML_STATUS_OK) {
            status = resolve_type_name(reader, child_of(ast, declaration, SXML_AST_FIELD_NAME),
                                       &record->record_name);
        }
        if (status == SXML_STATUS_OK) {
            status = check_target_namespace(
                reader, (target_namespace_check){.declaration = declaration,
                                                 .name = record->record_name,
                                                 .is_private = record->is_private});
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        for (sxml_ast_node_index annotation =
                 child_of(ast, declaration, SXML_AST_FIELD_ANNOTATION);
             annotation.value != 0; annotation = next_of(ast, annotation)) {
            expanded_name name = {0};
            if (resolve_xquery_name(reader, child_of(ast, annotation, SXML_AST_FIELD_NAME),
                                    &name) == SXML_STATUS_OK &&
                sxml_byte_view_equals(name.local_name,
                                      SXML_BYTE_VIEW_FROM_LITERAL("constructor"))) {
                record->has_constructor = true;
            }
        }
        /* A record declared by an earlier compilation for this item heap
           keeps its fields: the registry cannot redefine a type. */
        if (!sxml_type_registry_find_type(reader->type_registry, record->record_name,
                                          &record->record_type)) {
            record->is_new = true;
            status = sxml_type_registry_declare_nominative_record_type(
                reader->type_registry, record->record_name, &record->record_type);
        }
        context->declared_record_count += 1;
    }
    return status;
}

/* Gives each newly declared record its fields (5.20). */
static sxml_status define_records(const static_reader *reader,
                                  const sxml_xquery_static_context *context) {
    const sxml_ast *ast = reader->ast;
    sxml_status status = SXML_STATUS_OK;
    for (size_t record_index = 0;
         status == SXML_STATUS_OK && record_index < context->declared_record_count;
         record_index += 1) {
        const sxml_xquery_declared_record *record = &context->declared_records[record_index];
        size_t field_count = 0;
        for (sxml_ast_node_index field = child_of(ast, record->declaration, SXML_AST_FIELD_FIELD);
             field.value != 0; field = next_of(ast, field)) {
            field_count += 1;
        }
        if (!record->is_new) {
            continue;
        }
        void *memory = NULL;
        status = allocate_array(
            reader,
            (array_request){.element_count = field_count,
                            .element_size = sizeof(sxml_record_field),
                            .element_alignment = alignof(sxml_record_field)},
            &memory);
        sxml_record_field *fields = memory;
        size_t field_index = 0;
        for (sxml_ast_node_index field = child_of(ast, record->declaration, SXML_AST_FIELD_FIELD);
             status == SXML_STATUS_OK && field.value != 0; field = next_of(ast, field)) {
            fields[field_index].field_name = sxml_xquery_field_name(ast, field);
            for (size_t earlier_index = 0; earlier_index < field_index; earlier_index += 1) {
                if (sxml_byte_view_equals(fields[earlier_index].field_name,
                                          fields[field_index].field_name)) {
                    status = report(reader, field,
                                    SXML_BYTE_VIEW_FROM_LITERAL(
                                        "XPST0021 two fields of a record have the same name"));
                }
            }
            if (status == SXML_STATUS_OK) {
                status = resolve_type_node(reader, child_of(ast, field, SXML_AST_FIELD_TYPE),
                                           &fields[field_index].field_type);
            }
            field_index += 1;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_type_registry_define_record_fields(
                reader->type_registry,
                &(sxml_record_definition){
                    .record_type = record->record_type,
                    .record_fields = {.record_fields = fields, .record_field_count = field_count}});
        }
        sxml_allocator_release(reader->allocator, memory);
    }
    return status;
}

/* The named item types (5.19), in an order that lets each name the ones it
   uses: rounds over the declarations not resolved yet, until one makes
   no progress; then the first left reports its error (XPST0051 for a name
   that is never declared or a cycle). */
/* The public item types an imported module declares, read already (an
   import cycle may leave one unread: it has none yet). */
typedef struct imported_item_types {
    const sxml_xquery_static_context *module;
    size_t first;
    size_t end;
} imported_item_types;

static imported_item_types public_item_types_of(const sxml_xquery_static_context *module) {
    return (imported_item_types){.module = module,
                                 .first = module->imported_item_type_count,
                                 .end = module->named_item_type_count};
}

/* Puts the imported modules' public item types first (5.12). */
static void import_item_types(const static_reader *reader, sxml_xquery_static_context *context) {
    const sxml_xquery_static_context_view imports = reader->request->imported_modules;
    for (size_t import_index = 0; import_index < imports.static_context_count;
         import_index += 1) {
        const imported_item_types types =
            public_item_types_of(imports.static_contexts[import_index]);
        for (size_t type_index = types.first; type_index < types.end; type_index += 1) {
            if (!types.module->private_item_types[type_index]) {
                context->named_item_types[context->named_item_type_count] =
                    types.module->named_item_types[type_index];
                context->named_item_type_count += 1;
            }
        }
    }
    context->imported_item_type_count = context->named_item_type_count;
    context->xpath.named_item_types = (sxml_xpath_named_item_type_view){
        .named_item_types = context->named_item_types,
        .named_item_type_count = context->named_item_type_count};
}

static sxml_status read_item_types(const static_reader *reader, const prolog_survey *survey,
                                   sxml_xquery_static_context *context) {
    const sxml_ast *ast = reader->ast;
    size_t imported_count = 0;
    const sxml_xquery_static_context_view imports = reader->request->imported_modules;
    for (size_t import_index = 0; import_index < imports.static_context_count;
         import_index += 1) {
        const imported_item_types types =
            public_item_types_of(imports.static_contexts[import_index]);
        imported_count += types.end - types.first;
    }
    const size_t declaration_count = survey->item_type_declaration_count;
    if (declaration_count + imported_count == 0) {
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    sxml_status status = allocate_array(
        reader,
        (array_request){.element_count = declaration_count + imported_count,
                        .element_size = sizeof(sxml_xpath_named_item_type),
                        .element_alignment = alignof(sxml_xpath_named_item_type)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    context->named_item_types = memory;
    status = allocate_array(
        reader,
        (array_request){.element_count = declaration_count + imported_count,
                        .element_size = sizeof(bool),
                        .element_alignment = alignof(bool)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    context->private_item_types = memory;
    import_item_types(reader, context);
    status = allocate_array(
        reader,
        (array_request){.element_count = declaration_count,
                        .element_size = sizeof(sxml_ast_node_index),
                        .element_alignment = alignof(sxml_ast_node_index)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ast_node_index *pending = memory;
    size_t pending_count = 0;
    for (sxml_ast_node_index declaration = first_declaration(ast);
         status == SXML_STATUS_OK && declaration.value != 0;
         declaration = next_of(ast, declaration)) {
        if (node_of(ast, declaration)->kind == SXML_AST_KIND_ITEM_TYPE_DECLARATION) {
            pending[pending_count] = declaration;
            pending_count += 1;
            status = check_annotations(
                reader, (annotation_check){.declaration = declaration,
                                           .duplicate_error = SXML_BYTE_VIEW_FROM_LITERAL(
                                               "XQST0106 an item type has two visibility "
                                               "annotations"),
                                           .allows_constructor = false,
                                           .is_private = NULL});
        }
    }
    bool has_progress = true;
    while (status == SXML_STATUS_OK && pending_count > 0 && has_progress) {
        has_progress = false;
        size_t kept_count = 0;
        for (size_t pending_index = 0; status == SXML_STATUS_OK && pending_index < pending_count;
             pending_index += 1) {
            const sxml_ast_node_index declaration = pending[pending_index];
            sxml_xpath_named_item_type *entry =
                &context->named_item_types[context->named_item_type_count];
            status = resolve_type_name(reader, child_of(ast, declaration, SXML_AST_FIELD_NAME),
                                       &entry->type_name);
            if (status != SXML_STATUS_OK) {
                break;
            }
            const sxml_status type_status = sxml_xpath_resolve_item_type(
                &(sxml_xpath_type_request){.ast = ast,
                                           .type_node =
                                               child_of(ast, declaration, SXML_AST_FIELD_TYPE),
                                           .static_context = &context->xpath,
                                           .error_record = NULL},
                &entry->item_type);
            bool *is_private = &context->private_item_types[context->named_item_type_count];
            if (type_status == SXML_STATUS_OK) {
                status = check_annotations(
                    reader, (annotation_check){.declaration = declaration,
                                               .duplicate_error = {.bytes = NULL, .byte_count = 0},
                                               .allows_constructor = false,
                                               .is_private = is_private});
            }
            if (type_status == SXML_STATUS_OK && status == SXML_STATUS_OK) {
                status = check_target_namespace(
                    reader, (target_namespace_check){.declaration = declaration,
                                                     .name = entry->type_name,
                                                     .is_private = *is_private});
            }
            if (status != SXML_STATUS_OK) {
                break;
            }
            if (type_status == SXML_STATUS_OK) {
                context->named_item_type_count += 1;
                context->xpath.named_item_types = (sxml_xpath_named_item_type_view){
                    .named_item_types = context->named_item_types,
                    .named_item_type_count = context->named_item_type_count};
                has_progress = true;
            } else if (type_status == SXML_STATUS_STATIC_ERROR) {
                pending[kept_count] = declaration;
                kept_count += 1;
            } else {
                status = type_status;
            }
        }
        pending_count = kept_count;
    }
    if (status == SXML_STATUS_OK && pending_count > 0) {
        sxml_type_identifier unused = {0};
        status = sxml_xpath_resolve_item_type(
            &(sxml_xpath_type_request){.ast = ast,
                                       .type_node = child_of(ast, pending[0], SXML_AST_FIELD_TYPE),
                                       .static_context = &context->xpath,
                                       .error_record = reader->request->error_record},
            &unused);
        if (status == SXML_STATUS_OK) {
            status = report(reader, pending[0],
                            SXML_BYTE_VIEW_FROM_LITERAL(
                                "XPST0051 a named item type refers to itself"));
        }
    }
    sxml_allocator_release(reader->allocator, pending);
    return status;
}

/* XQST0146: two named item or record types of one name (5.19, 5.20). */
static sxml_status check_type_names(const static_reader *reader,
                                    const sxml_xquery_static_context *context) {
    const size_t alias_count = context->named_item_type_count;
    const size_t type_count = alias_count + context->declared_record_count;
    if (type_count < 2) {
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    sxml_status status = allocate_array(
        reader,
        (array_request){.element_count = type_count,
                        .element_size = sizeof(sort_entry),
                        .element_alignment = alignof(sort_entry)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sort_entry *entries = memory;
    for (size_t type_index = 0; type_index < type_count; type_index += 1) {
        const sxml_name_identifier name =
            type_index < alias_count
                ? context->named_item_types[type_index].type_name
                : context->declared_records[type_index - alias_count].record_name;
        entries[type_index] = (sort_entry){.key = name.index, .position = type_index};
    }
    sort_entries(entries, type_count);
    for (size_t entry_index = 1; status == SXML_STATUS_OK && entry_index < type_count;
         entry_index += 1) {
        if (entries[entry_index].key == entries[entry_index - 1].key) {
            const size_t position = entries[entry_index].position;
            status = report(reader,
                            position < alias_count
                                ? first_declaration(reader->ast)
                                : context->declared_records[position - alias_count].declaration,
                            SXML_BYTE_VIEW_FROM_LITERAL(
                                "XQST0146 two named types have the same name"));
        }
    }
    sxml_allocator_release(reader->allocator, memory);
    return status;
}

static sxml_status read_types(const static_reader *reader, const prolog_survey *survey,
                              sxml_xquery_static_context *context) {
    void *memory = NULL;
    sxml_status status = SXML_STATUS_OK;
    if (survey->record_declaration_count > 0) {
        status = allocate_array(
            reader,
            (array_request){.element_count = survey->record_declaration_count,
                            .element_size = sizeof(sxml_xquery_declared_record),
                            .element_alignment = alignof(sxml_xquery_declared_record)},
            &memory);
        context->declared_records = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = declare_records(reader, context);
    }
    if (status == SXML_STATUS_OK) {
        status = read_item_types(reader, survey, context);
    }
    if (status == SXML_STATUS_OK) {
        status = define_records(reader, context);
    }
    if (status == SXML_STATUS_OK) {
        status = check_type_names(reader, context);
    }
    return status;
}

/* Annotations outside the prolog's declarations: on inline functions and
   function types (5.15): none in a reserved namespace (XQST0045), and
   %public or %private only on declarations (XQST0125). One pass over the
   AST's nodes. */
static sxml_status check_expression_annotations(const static_reader *reader) {
    const sxml_ast *ast = reader->ast;
    sxml_namespace_uri_identifier xquery_namespace = {0};
    sxml_status status = sxml_name_pool_intern_namespace_uri(
        reader->name_pool, xquery_namespace_text, &xquery_namespace);
    for (uint32_t node_number = 1; status == SXML_STATUS_OK && node_number < ast->node_count;
         node_number += 1) {
        const sxml_ast_node_index annotation = {.value = node_number};
        const sxml_ast_node *node = node_of(ast, annotation);
        if (node->kind != SXML_AST_KIND_ANNOTATION ||
            node_of(ast, node->parent)->field == SXML_AST_FIELD_DECLARATION) {
            continue;
        }
        expanded_name name = {0};
        status = resolve_xquery_name(reader, child_of(ast, annotation, SXML_AST_FIELD_NAME),
                                     &name);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (node_of(ast, node->parent)->kind == SXML_AST_KIND_INLINE_FUNCTION_EXPRESSION &&
            name.namespace_uri.index == xquery_namespace.index &&
            (sxml_byte_view_equals(name.local_name, SXML_BYTE_VIEW_FROM_LITERAL("public")) ||
             sxml_byte_view_equals(name.local_name, SXML_BYTE_VIEW_FROM_LITERAL("private")))) {
            status = report(reader, annotation,
                            SXML_BYTE_VIEW_FROM_LITERAL(
                                "XQST0125 an inline function is annotated %public or %private"));
        } else if (is_reserved_namespace(name.namespace_uri) ||
                   (name.namespace_uri.index == xquery_namespace.index &&
                    !sxml_byte_view_equals(name.local_name, SXML_BYTE_VIEW_FROM_LITERAL("public")) &&
                    !sxml_byte_view_equals(name.local_name,
                                           SXML_BYTE_VIEW_FROM_LITERAL("private")) &&
                    /* XQuery Update Facility 3.0, 5.3 (D187). */
                    !sxml_byte_view_equals(name.local_name,
                                           SXML_BYTE_VIEW_FROM_LITERAL("updating")) &&
                    !sxml_byte_view_equals(name.local_name,
                                           SXML_BYTE_VIEW_FROM_LITERAL("simple")))) {
            status = report(reader, annotation,
                            SXML_BYTE_VIEW_FROM_LITERAL(
                                "XQST0045 an annotation is in a reserved namespace"));
        }
    }
    return status;
}

/* ------------------------------------------------------------ Variables */

/* The sequence type of an optional TYPE_DECLARATION; item()* without. */
static sxml_status resolve_declared_type(const static_reader *reader,
                                         sxml_ast_node_index type_declaration,
                                         sxml_sequence_type *sequence_type) {
    const sxml_xquery_static_context *context = reader->context;
    if (type_declaration.value == 0) {
        *sequence_type = (sxml_sequence_type){
            .item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
            .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};
        return SXML_STATUS_OK;
    }
    return sxml_xpath_resolve_sequence_type(
        &(sxml_xpath_type_request){
            .ast = reader->ast,
            .type_node = child_of(reader->ast, type_declaration, SXML_AST_FIELD_TYPE),
            .static_context = &context->xpath,
            .error_record = reader->request->error_record},
        sequence_type);
}

/* The expanded name of a variable or parameter (no-namespace rule). */
static sxml_status resolve_variable_name(const static_reader *reader,
                                         sxml_ast_node_index name_node,
                                         sxml_name_identifier *name) {
    const sxml_xquery_static_context *context = reader->context;
    sxml_xpath_resolved_name resolved = {0};
    sxml_status status = sxml_xpath_resolve_name(
        &(sxml_xpath_name_request){.ast = reader->ast,
                                   .name_node = name_node,
                                   .name_role = SXML_XPATH_NAME_ROLE_NO_NAMESPACE,
                                   .static_context = &context->xpath,
                                   .error_record = reader->request->error_record},
        &resolved);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_name_pool_intern_name(
        reader->name_pool,
        &(sxml_name_request){.namespace_uri = resolved.namespace_uri,
                             .local_name = resolved.local_name},
        name);
}

/* XQST0049: a variable name declared twice, by the prolog or by the
   prolog and the host (5.16, D106). */
static sxml_status check_variable_names(const static_reader *reader,
                                        const sxml_xquery_static_context *context) {
    const size_t declaration_count = context->variable_declaration_count;
    if (declaration_count < 2) {
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    sxml_status status = allocate_array(
        reader,
        (array_request){.element_count = declaration_count,
                        .element_size = sizeof(sort_entry),
                        .element_alignment = alignof(sort_entry)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sort_entry *entries = memory;
    for (size_t declaration_index = 0; declaration_index < declaration_count;
         declaration_index += 1) {
        entries[declaration_index] = (sort_entry){
            .key = context->variable_declarations[declaration_index].variable_name.index,
            .position = declaration_index};
    }
    sort_entries(entries, declaration_count);
    for (size_t entry_index = 1; status == SXML_STATUS_OK && entry_index < declaration_count;
         entry_index += 1) {
        if (entries[entry_index].key == entries[entry_index - 1].key) {
            /* Located at a prolog variable of the two (the host checked its
               own); two imported ones have no place in this module. */
            sxml_ast_node_index place = {0};
            for (size_t pair_index = 0; pair_index < 2; pair_index += 1) {
                const size_t position = entries[entry_index - pair_index].position;
                if (place.value == 0 && position >= context->host_variable_count &&
                    position < context->host_variable_count + context->prolog_variable_count) {
                    place = context->variable_nodes[position - context->host_variable_count];
                }
            }
            status = report(reader, place,
                            SXML_BYTE_VIEW_FROM_LITERAL("XQST0049 a variable is declared twice"));
        }
    }
    sxml_allocator_release(reader->allocator, memory);
    return status;
}

/* The variable declarations: the host's, then the prolog's (5.16). */
static sxml_status read_variables(const static_reader *reader, const prolog_survey *survey,
                                  sxml_xquery_static_context *context) {
    const sxml_ast *ast = reader->ast;
    const sxml_variable_declaration_view host =
        reader->request->host_static_context->variable_declarations;
    const size_t declaration_count =
        host.variable_declaration_count + survey->variable_declaration_count;
    void *memory = NULL;
    sxml_status status = allocate_array(
        reader,
        (array_request){.element_count = declaration_count,
                        .element_size = sizeof(sxml_variable_declaration),
                        .element_alignment = alignof(sxml_variable_declaration)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    context->variable_declarations = memory;
    status = allocate_array(
        reader,
        (array_request){.element_count = survey->variable_declaration_count,
                        .element_size = sizeof(sxml_ast_node_index),
                        .element_alignment = alignof(sxml_ast_node_index)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    context->variable_nodes = memory;
    status = allocate_array(
        reader,
        (array_request){.element_count = survey->variable_declaration_count,
                        .element_size = sizeof(bool),
                        .element_alignment = alignof(bool)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    context->private_variables = memory;
    status = allocate_array(
        reader,
        (array_request){.element_count = declaration_count,
                        .element_size = sizeof(uint32_t),
                        .element_alignment = alignof(uint32_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    context->global_numbers = memory;
    for (size_t host_index = 0; host_index < host.variable_declaration_count; host_index += 1) {
        context->variable_declarations[host_index] = host.variable_declarations[host_index];
        context->global_numbers[host_index] = (uint32_t)host_index + 1U;
    }
    context->host_variable_count = host.variable_declaration_count;
    context->variable_declaration_count = host.variable_declaration_count;
    for (sxml_ast_node_index declaration = first_declaration(ast);
         status == SXML_STATUS_OK && declaration.value != 0;
         declaration = next_of(ast, declaration)) {
        if (node_of(ast, declaration)->kind != SXML_AST_KIND_VARIABLE_DECLARATION) {
            continue;
        }
        const size_t prolog_index = context->variable_declaration_count - context->host_variable_count;
        status = check_annotations(
            reader, (annotation_check){.declaration = declaration,
                               .duplicate_error = SXML_BYTE_VIEW_FROM_LITERAL(
                                   "XQST0116 a variable has two visibility annotations"),
                               .allows_constructor = false,
                               .is_private = &context->private_variables[prolog_index]});
        const sxml_ast_node_index variable = child_of(ast, declaration, SXML_AST_FIELD_VARIABLE);
        sxml_variable_declaration *entry =
            &context->variable_declarations[context->variable_declaration_count];
        if (status == SXML_STATUS_OK) {
            status = resolve_variable_name(reader, child_of(ast, variable, SXML_AST_FIELD_NAME),
                                           &entry->variable_name);
        }
        if (status == SXML_STATUS_OK) {
            status = check_target_namespace(
                reader, (target_namespace_check){
                            .declaration = declaration,
                            .name = entry->variable_name,
                            .is_private = context->private_variables[prolog_index]});
        }
        if (status == SXML_STATUS_OK) {
            status = resolve_declared_type(reader, child_of(ast, variable, SXML_AST_FIELD_TYPE),
                                           &entry->declared_type);
        }
        if (status == SXML_STATUS_OK) {
            context->variable_nodes[prolog_index] = declaration;
            context->global_numbers[context->variable_declaration_count] =
                reader->request->global_number_base + (uint32_t)prolog_index + 1U;
            context->variable_declaration_count += 1;
            context->prolog_variable_count += 1;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = check_variable_names(reader, context);
    }
    if (status == SXML_STATUS_OK) {
        context->xpath.variable_declarations = (sxml_variable_declaration_view){
            .variable_declarations = context->variable_declarations,
            .variable_declaration_count = context->variable_declaration_count};
        context->xpath.global_numbers = context->global_numbers;
    }
    return status;
}

/* ------------------------------------------------------------ Functions */

/* The expanded name of a declared function (5.18): an unprefixed name is
   in the default function namespace when one is declared, else in no
   namespace (5.14.2). */
static sxml_status resolve_function_name(const static_reader *reader,
                                         sxml_ast_node_index name_node,
                                         sxml_name_identifier *name) {
    const sxml_xquery_static_context *context = reader->context;
    sxml_xpath_resolved_name resolved = {0};
    sxml_status status = sxml_xpath_resolve_name(
        &(sxml_xpath_name_request){.ast = reader->ast,
                                   .name_node = name_node,
                                   .name_role = SXML_XPATH_NAME_ROLE_NO_NAMESPACE,
                                   .static_context = &context->xpath,
                                   .error_record = reader->request->error_record},
        &resolved);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (node_of(reader->ast, name_node)->kind == SXML_AST_KIND_NCNAME &&
        context->has_default_function_namespace) {
        resolved.namespace_uri = context->default_function_namespace;
    }
    if (is_reserved_namespace(resolved.namespace_uri)) {
        return report(reader, name_node,
                      SXML_BYTE_VIEW_FROM_LITERAL(
                          "XQST0045 a declared function is in a reserved namespace"));
    }
    return sxml_name_pool_intern_name(
        reader->name_pool,
        &(sxml_name_request){.namespace_uri = resolved.namespace_uri,
                             .local_name = resolved.local_name},
        name);
}

/* The name of a parameter: a variable name, or for a record field its
   name in no namespace (5.20.2). */
static sxml_status resolve_parameter_name(const static_reader *reader,
                                          sxml_ast_node_index parameter,
                                          sxml_name_identifier *name) {
    if (node_of(reader->ast, parameter)->kind != SXML_AST_KIND_EXTENDED_FIELD_DECLARATION) {
        return resolve_variable_name(reader, sxml_xquery_parameter_name(reader->ast, parameter),
                                     name);
    }
    return sxml_name_pool_intern_name(
        reader->name_pool,
        &(sxml_name_request){
            .namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_NONE),
            .local_name = sxml_xquery_field_name(reader->ast, parameter)},
        name);
}

/* A function to declare: a FUNCTION_DECLARATION, or the constructor of a
   record (record_type set, function_name its name). */
typedef struct function_source {
    sxml_ast_node_index declaration;
    sxml_type_identifier record_type;
    sxml_name_identifier function_name;
    /* A record's %private, which its constructor shares. */
    bool is_private;
} function_source;

/* The default functions of one declared function's parameters (glossary
   `default_function`), after its other IR functions: three parameters,
   the caller's context value, position and size, and the default's
   type as result. Their focus names all three parts until prolog
   lowering knows which the default reads, so a call lowered before
   that passes the whole focus. */
typedef struct default_functions {
    sxml_xquery_declared_function *function;
    sxml_xquery_static_context *context;
} default_functions;

static sxml_status add_default_functions(const static_reader *reader,
                                         default_functions request) {
    sxml_xquery_declared_function *function = request.function;
    sxml_xquery_static_context *context = request.context;
    static const sxml_sequence_type focus_types[3] = {
        {.item_type = {.index = SXML_TYPE_ITEM}, .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE},
        {.item_type = {.index = SXML_TYPE_ITEM}, .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE},
        {.item_type = {.index = SXML_TYPE_ITEM}, .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}};
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t parameter_index = 0;
         status == SXML_STATUS_OK && parameter_index < function->parameter_count;
         parameter_index += 1) {
        sxml_function_parameter *parameter =
            &context->function_parameters[function->first_parameter + parameter_index];
        if (parameter->parameter_default != SXML_PARAMETER_DEFAULT_EXPRESSION) {
            continue;
        }
        sxml_type_identifier signature = {0};
        status = sxml_type_registry_register_item_type(
            reader->type_registry,
            &(sxml_type_description){
                .variety = SXML_TYPE_VARIETY_FUNCTION,
                .variety_details = {.function_type = {
                                        .is_any_function = false,
                                        .parameter_types = {.sequence_types = focus_types,
                                                            .sequence_type_count = 3},
                                        .result_type = context->parameter_types
                                                           [function->first_parameter +
                                                            parameter_index]}}},
            &signature);
        sxml_ir_function_index index = {0};
        if (status == SXML_STATUS_OK) {
            const sxml_ast_source_span span = node_of(reader->ast, function->declaration)->source_span;
            status = sxml_ir_unit_add_function(
                reader->request->unit,
                &(sxml_ir_function){.signature = signature,
                                    .parameter_count = 3,
                                    .slot_count = 3,
                                    .focus = {.context_value = {.value = 1},
                                              .context_position = {.value = 2},
                                              .context_size = {.value = 3}},
                                    .source_span = {.byte_offset = span.byte_offset,
                                                    .byte_count = span.byte_count,
                                                    .line_number = span.line_number,
                                                    .column_number = span.column_number}},
                &index);
        }
        if (status == SXML_STATUS_OK) {
            parameter->default_function_number = index.value;
            function->default_function_count += 1;
        }
    }
    return status;
}

/* The parameters, signatures and IR functions of one declared function. */
static sxml_status read_function(const static_reader *reader, function_source source,
                                 sxml_xquery_static_context *context) {
    const sxml_ast *ast = reader->ast;
    const sxml_ast_node_index declaration = source.declaration;
    const bool is_constructor = source.record_type.index != 0;
    sxml_xquery_declared_function *function =
        &context->declared_functions[context->declared_function_count];
    size_t first_parameter = 0;
    size_t first_signature = 0;
    if (context->declared_function_count > 0) {
        const sxml_xquery_declared_function *previous = function - 1;
        first_parameter = previous->first_parameter + previous->parameter_count;
        first_signature = previous->first_signature +
                          (previous->parameter_count - previous->required_count) + 1;
    }
    *function = (sxml_xquery_declared_function){.declaration = declaration,
                                                .function_name = source.function_name,
                                                .first_parameter = first_parameter,
                                                .first_signature = first_signature,
                                                .record_type = source.record_type,
                                                .is_private = source.is_private};
    sxml_status status = SXML_STATUS_OK;
    if (!is_constructor) {
        status = check_annotations(
            reader, (annotation_check){.declaration = declaration,
                                       .duplicate_error = SXML_BYTE_VIEW_FROM_LITERAL(
                                           "XQST0106 a function has two visibility annotations"),
                                       .allows_constructor = false,
                                       .is_private = &function->is_private});
    }
    if (status == SXML_STATUS_OK && !is_constructor) {
        status = resolve_function_name(reader, child_of(ast, declaration, SXML_AST_FIELD_NAME),
                                       &function->function_name);
        if (status == SXML_STATUS_OK) {
            status = check_target_namespace(
                reader, (target_namespace_check){.declaration = declaration,
                                                 .name = function->function_name,
                                                 .is_private = function->is_private});
        }
    }
    sxml_name_identifier *parameter_names = NULL;
    void *names_memory = NULL;
    size_t parameter_count = 0;
    for (sxml_ast_node_index parameter = sxml_xquery_first_parameter(ast, declaration);
         parameter.value != 0; parameter = sxml_xquery_next_parameter(ast, parameter)) {
        parameter_count += 1;
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_array(
            reader,
            (array_request){.element_count = parameter_count,
                            .element_size = sizeof(sxml_name_identifier),
                            .element_alignment = alignof(sxml_name_identifier)},
            &names_memory);
        parameter_names = names_memory;
    }
    size_t parameter_index = 0;
    for (sxml_ast_node_index parameter = sxml_xquery_first_parameter(ast, declaration);
         status == SXML_STATUS_OK && parameter.value != 0;
         parameter = sxml_xquery_next_parameter(ast, parameter)) {
        status = resolve_parameter_name(reader, parameter, &parameter_names[parameter_index]);
        for (size_t earlier_index = 0; status == SXML_STATUS_OK && earlier_index < parameter_index;
             earlier_index += 1) {
            if (parameter_names[earlier_index].index == parameter_names[parameter_index].index) {
                status = report(reader, parameter,
                                SXML_BYTE_VIEW_FROM_LITERAL(
                                    "XQST0039 two parameters of a function have the same name"));
            }
        }
        if (status == SXML_STATUS_OK) {
            status = resolve_type_node(reader, sxml_xquery_parameter_type(ast, parameter),
                                       &context->parameter_types[first_parameter +
                                                                 parameter_index]);
        }
        const sxml_ast_node_index default_value = child_of(ast, parameter, SXML_AST_FIELD_DEFAULT);
        sxml_parameter_default parameter_default = SXML_PARAMETER_DEFAULT_REQUIRED;
        if (default_value.value != 0) {
            /* 5.18.3: ":= ." is the caller's context value, which the core
               supplies as for library functions; other defaults are
               computed by a default function the caller calls with its
               focus (a record constructor's by the IR function of the
               shorter arity). */
            parameter_default =
                node_of(ast, default_value)->kind == SXML_AST_KIND_CONTEXT_VALUE_REFERENCE
                    ? SXML_PARAMETER_DEFAULT_CONTEXT_VALUE
                : is_constructor ? SXML_PARAMETER_DEFAULT_SUPPLIED_BY_BODY
                                 : SXML_PARAMETER_DEFAULT_EXPRESSION;
        } else {
            function->required_count += 1;
        }
        const sxml_ast_node_index name_node = sxml_xquery_parameter_name(ast, parameter);
        context->function_parameters[first_parameter + parameter_index] = (sxml_function_parameter){
            .parameter_name =
                is_constructor
                    ? sxml_xquery_field_name(ast, parameter)
                    : sxml_ast_get_text(ast, node_of(ast, name_node)->payload.name.local_name),
            .parameter_type = {.bytes = NULL, .byte_count = 0},
            .parameter_default = parameter_default};
        parameter_index += 1;
    }
    sxml_allocator_release(reader->allocator, names_memory);
    function->parameter_count = (uint32_t)parameter_count;
    if (status == SXML_STATUS_OK && is_constructor) {
        /* 5.20.2: the record type, no occurrence indicator. */
        function->result_type = (sxml_sequence_type){.item_type = source.record_type,
                                                      .occurrence = SXML_OCCURRENCE_EXACTLY_ONE};
    } else if (status == SXML_STATUS_OK) {
        status = resolve_declared_type(reader, child_of(ast, declaration, SXML_AST_FIELD_RETURN_TYPE),
                                       &function->result_type);
    }
    if (status == SXML_STATUS_OK && (node_of(ast, declaration)->flags & SXML_AST_FLAG_EXTERNAL) != 0) {
        /* 5.18.4: sxml has no external functions. */
        return report(reader, declaration,
                      SXML_BYTE_VIEW_FROM_LITERAL(
                          "XPST0017 external functions are not supported"));
    }
    const sxml_ast_source_span span = node_of(ast, declaration)->source_span;
    for (uint32_t arity = function->required_count;
         status == SXML_STATUS_OK && arity <= function->parameter_count; arity += 1) {
        sxml_type_identifier *signature =
            &context->signatures[first_signature + (arity - function->required_count)];
        status = sxml_type_registry_register_item_type(
            reader->type_registry,
            &(sxml_type_description){
                .variety = SXML_TYPE_VARIETY_FUNCTION,
                .variety_details = {.function_type = {
                                        .is_any_function = false,
                                        .parameter_types = {.sequence_types =
                                                                &context->parameter_types
                                                                     [first_parameter],
                                                            .sequence_type_count = arity},
                                        .result_type = function->result_type}}},
            signature);
        sxml_ir_function_index index = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_ir_unit_add_function(
                reader->request->unit,
                &(sxml_ir_function){.function_name = function->function_name,
                                    .signature = *signature,
                                    .parameter_count = arity,
                                    .source_span = {.byte_offset = span.byte_offset,
                                                    .byte_count = span.byte_count,
                                                    .line_number = span.line_number,
                                                    .column_number = span.column_number}},
                &index);
        }
        if (status == SXML_STATUS_OK && arity == function->required_count) {
            function->first_function = index;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = add_default_functions(
            reader, (default_functions){.function = function, .context = context});
    }
    if (status == SXML_STATUS_OK) {
        context->declared_function_count += 1;
    }
    return status;
}

/* XQST0034: two declared functions of one name whose arity ranges overlap
   (5.18). */
static sxml_status check_function_names(const static_reader *reader,
                                        const sxml_xquery_static_context *context) {
    const size_t function_count = context->declared_function_count;
    if (function_count < 2) {
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    sxml_status status = allocate_array(
        reader,
        (array_request){.element_count = function_count,
                        .element_size = sizeof(sort_entry),
                        .element_alignment = alignof(sort_entry)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sort_entry *entries = memory;
    for (size_t function_index = 0; function_index < function_count; function_index += 1) {
        entries[function_index] = (sort_entry){
            .key = context->declared_functions[function_index].function_name.index,
            .position = function_index};
    }
    sort_entries(entries, function_count);
    size_t run_start = 0;
    for (size_t entry_index = 1; status == SXML_STATUS_OK && entry_index < function_count;
         entry_index += 1) {
        if (entries[entry_index].key != entries[entry_index - 1].key) {
            run_start = entry_index;
            continue;
        }
        const sxml_xquery_declared_function *later =
            &context->declared_functions[entries[entry_index].position];
        for (size_t earlier_index = run_start;
             status == SXML_STATUS_OK && earlier_index < entry_index; earlier_index += 1) {
            const sxml_xquery_declared_function *earlier =
                &context->declared_functions[entries[earlier_index].position];
            if (earlier->required_count <= later->parameter_count &&
                later->required_count <= earlier->parameter_count) {
                status = report(reader, later->declaration,
                                SXML_BYTE_VIEW_FROM_LITERAL(
                                    "XQST0034 two declared functions have the same name and "
                                    "overlapping arities"));
            }
        }
    }
    sxml_allocator_release(reader->allocator, memory);
    return status;
}

static sxml_status read_functions(const static_reader *reader, const prolog_survey *survey,
                                  sxml_xquery_static_context *context) {
    void *memory = NULL;
    sxml_status status = allocate_array(
        reader,
        (array_request){.element_count = survey->function_declaration_count,
                        .element_size = sizeof(sxml_xquery_declared_function),
                        .element_alignment = alignof(sxml_xquery_declared_function)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    context->declared_functions = memory;
    status = allocate_array(
        reader,
        (array_request){.element_count = survey->parameter_count,
                        .element_size = sizeof(sxml_sequence_type),
                        .element_alignment = alignof(sxml_sequence_type)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    context->parameter_types = memory;
    status = allocate_array(
        reader,
        (array_request){.element_count = survey->parameter_count,
                        .element_size = sizeof(sxml_function_parameter),
                        .element_alignment = alignof(sxml_function_parameter)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    context->function_parameters = memory;
    status = allocate_array(
        reader,
        (array_request){.element_count = survey->signature_count,
                        .element_size = sizeof(sxml_type_identifier),
                        .element_alignment = alignof(sxml_type_identifier)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    context->signatures = memory;
    for (sxml_ast_node_index declaration = first_declaration(reader->ast);
         status == SXML_STATUS_OK && declaration.value != 0;
         declaration = next_of(reader->ast, declaration)) {
        if (node_of(reader->ast, declaration)->kind == SXML_AST_KIND_FUNCTION_DECLARATION) {
            status = read_function(reader,
                                   (function_source){.declaration = declaration,
                                                     .record_type = {.index = 0},
                                                     .function_name = {.index = 0},
                                                     .is_private = false},
                                   context);
        }
    }
    for (size_t record_index = 0;
         status == SXML_STATUS_OK && record_index < context->declared_record_count;
         record_index += 1) {
        /* Every record gets its constructor: the QT4 tests call
           constructors of records without %constructor (5.20.2 asks for the
           annotation; D135). */
        const sxml_xquery_declared_record *record = &context->declared_records[record_index];
        {
            status = read_function(reader,
                                   (function_source){.declaration = record->declaration,
                                                     .record_type = record->record_type,
                                                     .function_name = record->record_name,
                                                     .is_private = record->is_private},
                                   context);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = check_function_names(reader, context);
    }
    return status;
}

/* -------------------------------------------------------- Function table */

/* The declared function named name whose arity range holds arity; NULL
   for none. */
static const sxml_xquery_declared_function *find_declared(
    const sxml_xquery_static_context *context, const sxml_function_query *query) {
    for (size_t function_index = 0; function_index < context->declared_function_count;
         function_index += 1) {
        const sxml_xquery_declared_function *function = &context->declared_functions[function_index];
        if (function->function_name.index == query->function_name.index &&
            query->arity >= function->required_count && query->arity <= function->parameter_count) {
            return function;
        }
    }
    return NULL;
}

/* The same local name in another namespace (interned); 0 when that
   fails. */
typedef struct renamed_query {
    sxml_name_identifier function_name;
    sxml_namespace_uri_identifier namespace_uri;
} renamed_query;

static sxml_name_identifier rename_function(const sxml_xquery_static_context *context,
                                            renamed_query request) {
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(context->xpath.item_heap);
    sxml_name_parts parts = {0};
    sxml_name_identifier renamed = {.index = 0};
    if (sxml_name_pool_get_name_parts(name_pool, request.function_name, &parts) ==
        SXML_STATUS_OK &&
        sxml_name_pool_intern_name(name_pool,
                                   &(sxml_name_request){.namespace_uri = request.namespace_uri,
                                                        .local_name = parts.local_name},
                                   &renamed) != SXML_STATUS_OK) {
        renamed = (sxml_name_identifier){.index = 0};
    }
    return renamed;
}

/* The constructor function of a named item type that is a generalized
   atomic type (5.19: "if the item type is a generalized atomic type, the
   declaration also declares a constructor function"): a cast. */
/* The named item types a constructor is searched among: all of a
   module's, or an imported module's public ones. */
typedef struct alias_search {
    const sxml_xquery_static_context *module;
    const sxml_function_query *query;
    bool is_imported;
} alias_search;

static sxml_status find_alias_constructor(alias_search search, sxml_function_entry *entry) {
    const sxml_xquery_static_context *context = search.module;
    const sxml_function_query *query = search.query;
    sxml_type_registry *registry = sxml_item_heap_get_type_registry(context->xpath.item_heap);
    for (size_t alias_index = search.is_imported ? context->imported_item_type_count : 0;
         alias_index < context->named_item_type_count; alias_index += 1) {
        const sxml_xpath_named_item_type *alias = &context->named_item_types[alias_index];
        if (alias->type_name.index != query->function_name.index || query->arity != 1 ||
            (search.is_imported && context->private_item_types[alias_index])) {
            continue;
        }
        sxml_type_description description = {0};
        sxml_status status = sxml_type_registry_describe_type(registry, alias->item_type,
                                                              &description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        switch (description.variety) {
        case SXML_TYPE_VARIETY_ATOMIC:
        case SXML_TYPE_VARIETY_UNION:
        case SXML_TYPE_VARIETY_ENUMERATION:
        case SXML_TYPE_VARIETY_CHOICE:
            break;
        case SXML_TYPE_VARIETY_LIST:
        case SXML_TYPE_VARIETY_COMPLEX:
        case SXML_TYPE_VARIETY_ANY_SIMPLE:
        case SXML_TYPE_VARIETY_ANY_ITEM:
        case SXML_TYPE_VARIETY_NODE_TEST:
        case SXML_TYPE_VARIETY_JSON_NODE_TEST:
        case SXML_TYPE_VARIETY_FUNCTION:
        case SXML_TYPE_VARIETY_MAP:
        case SXML_TYPE_VARIETY_ARRAY:
        case SXML_TYPE_VARIETY_RECORD:
            return SXML_STATUS_OK;
        }
        static const sxml_sequence_type parameter_type = {
            .item_type = {.index = SXML_TYPE_XS_ANY_ATOMIC_TYPE},
            .occurrence = SXML_OCCURRENCE_ZERO_OR_ONE};
        const sxml_sequence_type result_type = {.item_type = alias->item_type,
                                                .occurrence = SXML_OCCURRENCE_ZERO_OR_ONE};
        sxml_type_identifier signature = {0};
        status = sxml_type_registry_register_item_type(
            registry,
            &(sxml_type_description){
                .variety = SXML_TYPE_VARIETY_FUNCTION,
                .variety_details = {.function_type = {
                                        .is_any_function = false,
                                        .parameter_types = {.sequence_types = &parameter_type,
                                                            .sequence_type_count = 1},
                                        .result_type = result_type}}},
            &signature);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        *entry = (sxml_function_entry){
            .entry_kind = SXML_FUNCTION_ENTRY_KIND_CONSTRUCTOR,
            .native_function = NULL,
            .constructor_type = alias->item_type,
            .ir_function_number = 0,
            .parameters = {.function_parameters = NULL, .function_parameter_count = 0},
            .signature = signature,
            .parameter_types = {.sequence_types = &parameter_type, .sequence_type_count = 1},
            .result_type = result_type};
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_OK;
}

/*
 * Declared functions, then the constructors of named atomic types, then
 * the host's table. Without a default function namespace declaration the
 * core expands unprefixed names to no namespace
 * (SXML_XPATH_FUNCTION_NAMESPACE_RULE_NO_NAMESPACE_FIRST), so a
 * no-namespace name no declared function has is looked up in fn
 * (5.14.2).
 */
/* The entry of a declared function of module (5.18) for query's arity. */
typedef struct declared_entry {
    const sxml_xquery_static_context *module;
    const sxml_xquery_declared_function *function;
} declared_entry;

static sxml_function_entry entry_of_declared(declared_entry declared,
                                             const sxml_function_query *query) {
    const sxml_xquery_static_context *module = declared.module;
    const sxml_xquery_declared_function *function = declared.function;
    const size_t arity_offset = query->arity - function->required_count;
    return (sxml_function_entry){
        .entry_kind = SXML_FUNCTION_ENTRY_KIND_IR_FUNCTION,
        .native_function = NULL,
        .ir_function_number = function->first_function.value + (uint32_t)arity_offset,
        .parameters = {.function_parameters = &module->function_parameters[function->first_parameter],
                       .function_parameter_count = function->parameter_count},
        .signature = module->signatures[function->first_signature + arity_offset],
        .parameter_types = {.sequence_types = &module->parameter_types[function->first_parameter],
                            .sequence_type_count = query->arity},
        .result_type = function->result_type};
}

/* A public function or constructor of an imported module (5.12). */
static sxml_status find_imported(const sxml_xquery_static_context *context,
                                 const sxml_function_query *query, sxml_function_entry *entry) {
    sxml_status status = SXML_STATUS_OK;
    const sxml_xquery_static_context_view imports = context->imported_modules;
    for (size_t import_index = 0; status == SXML_STATUS_OK &&
                                  entry->entry_kind == SXML_FUNCTION_ENTRY_KIND_NONE &&
                                  import_index < imports.static_context_count;
         import_index += 1) {
        const sxml_xquery_static_context *module = imports.static_contexts[import_index];
        const sxml_xquery_declared_function *function = find_declared(module, query);
        if (function != NULL && !function->is_private) {
            *entry = entry_of_declared((declared_entry){.module = module, .function = function},
                                       query);
            break;
        }
        status = find_alias_constructor(
            (alias_search){.module = module, .query = query, .is_imported = true}, entry);
    }
    return status;
}

static sxml_status find_function(void *state, const sxml_function_query *query,
                                 sxml_function_entry *entry) {
    const sxml_xquery_static_context *context = state;
    const sxml_xquery_declared_function *function = find_declared(context, query);
    if (function != NULL) {
        *entry = entry_of_declared((declared_entry){.module = context, .function = function},
                                   query);
        return SXML_STATUS_OK;
    }
    *entry = (sxml_function_entry){.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
    sxml_status status = find_alias_constructor(
        (alias_search){.module = context, .query = query, .is_imported = false}, entry);
    if (status == SXML_STATUS_OK && entry->entry_kind == SXML_FUNCTION_ENTRY_KIND_NONE) {
        status = find_imported(context, query, entry);
    }
    if (status != SXML_STATUS_OK || entry->entry_kind != SXML_FUNCTION_ENTRY_KIND_NONE) {
        return status;
    }
    sxml_name_parts parts = {0};
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(context->xpath.item_heap);
    if (context->xpath.function_namespace_rule ==
            SXML_XPATH_FUNCTION_NAMESPACE_RULE_NO_NAMESPACE_FIRST &&
        sxml_name_pool_get_name_parts(name_pool, query->function_name, &parts) ==
            SXML_STATUS_OK &&
        parts.namespace_uri.index == SXML_NAMESPACE_NONE) {
        /* 5.14.2: no declared no-namespace function, so the standard
           function namespace. */
        const sxml_function_query standard = {
            .function_name = rename_function(
                context,
                (renamed_query){.function_name = query->function_name,
                                .namespace_uri = sxml_well_known_namespace_identifier(
                                    SXML_NAMESPACE_FUNCTIONS)}),
            .arity = query->arity};
        if (standard.function_name.index == 0) {
            return SXML_STATUS_OK;
        }
        return sxml_xpath_find_function(context->host_function_table, &standard, entry);
    }
    return sxml_xpath_find_function(context->host_function_table, query, entry);
}

static const sxml_function_table_operations declared_function_table_operations = {
    .find_function = find_function};

const sxml_xquery_declared_function *sxml_xquery_find_declared_function(
    const sxml_xquery_static_context *context, sxml_ast_node_index declaration) {
    for (size_t function_index = 0; function_index < context->declared_function_count;
         function_index += 1) {
        if (context->declared_functions[function_index].declaration.value == declaration.value) {
            return &context->declared_functions[function_index];
        }
    }
    return NULL;
}

/*
 * The core looks function names up without interning them (a name never
 * interned names no function), so the names unprefixed calls, references
 * and arrows expand to (no namespace, or the default function namespace)
 * are interned first, letting the function table see them: one pass over
 * the AST's nodes.
 */
static sxml_status intern_unprefixed_calls(const static_reader *reader,
                                           const sxml_xquery_static_context *context) {
    const sxml_ast *ast = reader->ast;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t node_number = 1; status == SXML_STATUS_OK && node_number < ast->node_count;
         node_number += 1) {
        const sxml_ast_node *node = node_of(ast, (sxml_ast_node_index){.value = node_number});
        if (node->kind != SXML_AST_KIND_NCNAME ||
            (node->field != SXML_AST_FIELD_NAME && node->field != SXML_AST_FIELD_FUNCTION)) {
            continue;
        }
        const sxml_ast_kind parent_kind = node_of(ast, node->parent)->kind;
        if (parent_kind != SXML_AST_KIND_FUNCTION_CALL &&
            parent_kind != SXML_AST_KIND_NAMED_FUNCTION_REFERENCE &&
            parent_kind != SXML_AST_KIND_ARROW_EXPRESSION) {
            continue;
        }
        sxml_name_identifier name = {0};
        status = sxml_name_pool_intern_name(
            reader->name_pool,
            &(sxml_name_request){
                .namespace_uri = context->has_default_function_namespace
                                     ? context->default_function_namespace
                                     : sxml_well_known_namespace_identifier(SXML_NAMESPACE_NONE),
                .local_name = sxml_ast_get_text(ast, node->payload.name.local_name)},
            &name);
    }
    return status;
}

/* ---------------------------------------------------- Context value */

/* The required type of the initial context value (5.17): "context value
   as T" (item()* without), "context item as I" (item() without). */
static sxml_status read_context_value_type(const static_reader *reader,
                                           const prolog_survey *survey,
                                           sxml_xquery_static_context *context) {
    const sxml_ast_node_index declaration = survey->context_value_declaration;
    context->context_value_declaration = declaration;
    context->context_value_type = (sxml_sequence_type){
        .item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
        .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};
    if (declaration.value == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_ast_node_index type = child_of(reader->ast, declaration, SXML_AST_FIELD_TYPE);
    const bool is_item = node_of(reader->ast, declaration)->detail.keyword == SXML_AST_KEYWORD_ITEM;
    if (is_item) {
        context->context_value_type.occurrence = SXML_OCCURRENCE_EXACTLY_ONE;
    }
    if (type.value == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_xpath_type_request request = {.ast = reader->ast,
                                             .type_node = type,
                                             .static_context = &context->xpath,
                                             .error_record = reader->request->error_record};
    if (is_item) {
        return sxml_xpath_resolve_item_type(&request, &context->context_value_type.item_type);
    }
    return sxml_xpath_resolve_sequence_type(&request, &context->context_value_type);
}

/* ------------------------------------------------------------- Linking */

/* Appends the imported modules' public variables (5.12) to the
   variables in scope. */
static sxml_status import_variables(const static_reader *reader,
                                    sxml_xquery_static_context *context) {
    const sxml_xquery_static_context_view imports = reader->context->imported_modules;
    size_t imported_count = 0;
    for (size_t import_index = 0; import_index < imports.static_context_count;
         import_index += 1) {
        const sxml_xquery_static_context *module = imports.static_contexts[import_index];
        for (size_t variable_index = 0; variable_index < module->prolog_variable_count;
             variable_index += 1) {
            imported_count += module->private_variables[variable_index] ? 0 : 1;
        }
    }
    if (imported_count == 0) {
        return SXML_STATUS_OK;
    }
    const size_t declaration_count = context->variable_declaration_count + imported_count;
    void *declaration_memory = NULL;
    void *number_memory = NULL;
    sxml_status status = allocate_array(
        reader,
        (array_request){.element_count = declaration_count,
                        .element_size = sizeof(sxml_variable_declaration),
                        .element_alignment = alignof(sxml_variable_declaration)},
        &declaration_memory);
    if (status == SXML_STATUS_OK) {
        status = allocate_array(reader,
                                (array_request){.element_count = declaration_count,
                                                .element_size = sizeof(uint32_t),
                                                .element_alignment = alignof(uint32_t)},
                                &number_memory);
    }
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(reader->allocator, declaration_memory);
        return status;
    }
    sxml_variable_declaration *declarations = declaration_memory;
    uint32_t *numbers = number_memory;
    size_t filled = context->variable_declaration_count;
    memcpy(declarations, context->variable_declarations,
           filled * sizeof(sxml_variable_declaration));
    memcpy(numbers, context->global_numbers, filled * sizeof(uint32_t));
    for (size_t import_index = 0; import_index < imports.static_context_count;
         import_index += 1) {
        const sxml_xquery_static_context *module = imports.static_contexts[import_index];
        for (size_t variable_index = 0; variable_index < module->prolog_variable_count;
             variable_index += 1) {
            if (module->private_variables[variable_index]) {
                continue;
            }
            const size_t position = module->host_variable_count + variable_index;
            declarations[filled] = module->variable_declarations[position];
            numbers[filled] = module->global_numbers[position];
            filled += 1;
        }
    }
    sxml_allocator_release(reader->allocator, context->variable_declarations);
    sxml_allocator_release(reader->allocator, context->global_numbers);
    context->variable_declarations = declarations;
    context->global_numbers = numbers;
    context->variable_declaration_count = declaration_count;
    context->xpath.variable_declarations = (sxml_variable_declaration_view){
        .variable_declarations = declarations, .variable_declaration_count = declaration_count};
    context->xpath.global_numbers = numbers;
    return SXML_STATUS_OK;
}

/* A function a module sees by name: declared (module_index 0) or
   imported (module_index n for import n - 1). */
typedef struct visible_function {
    const sxml_xquery_declared_function *function;
    size_t module_index;
} visible_function;

/* XQST0034 among the declared functions and the imported public ones
   (5.12, 5.18): the declared ones were checked among themselves, as each
   imported module's were. */
static sxml_status check_imported_functions(const static_reader *reader,
                                            const sxml_xquery_static_context *context) {
    const sxml_xquery_static_context_view imports = context->imported_modules;
    size_t function_count = context->declared_function_count;
    for (size_t import_index = 0; import_index < imports.static_context_count;
         import_index += 1) {
        function_count += imports.static_contexts[import_index]->declared_function_count;
    }
    if (function_count < 2 || imports.static_context_count == 0) {
        return SXML_STATUS_OK;
    }
    void *function_memory = NULL;
    void *entry_memory = NULL;
    sxml_status status = allocate_array(
        reader,
        (array_request){.element_count = function_count,
                        .element_size = sizeof(visible_function),
                        .element_alignment = alignof(visible_function)},
        &function_memory);
    if (status == SXML_STATUS_OK) {
        status = allocate_array(reader,
                                (array_request){.element_count = function_count,
                                                .element_size = sizeof(sort_entry),
                                                .element_alignment = alignof(sort_entry)},
                                &entry_memory);
    }
    visible_function *functions = function_memory;
    sort_entry *entries = entry_memory;
    size_t visible_count = 0;
    for (size_t module_index = 0;
         status == SXML_STATUS_OK && module_index <= imports.static_context_count;
         module_index += 1) {
        const sxml_xquery_static_context *module =
            module_index == 0 ? context : imports.static_contexts[module_index - 1];
        for (size_t function_index = 0; function_index < module->declared_function_count;
             function_index += 1) {
            const sxml_xquery_declared_function *function =
                &module->declared_functions[function_index];
            if (module_index > 0 && function->is_private) {
                continue;
            }
            functions[visible_count] =
                (visible_function){.function = function, .module_index = module_index};
            entries[visible_count] =
                (sort_entry){.key = function->function_name.index, .position = visible_count};
            visible_count += 1;
        }
    }
    if (status == SXML_STATUS_OK) {
        sort_entries(entries, visible_count);
    }
    size_t run_start = 0;
    for (size_t entry_index = 1; status == SXML_STATUS_OK && entry_index < visible_count;
         entry_index += 1) {
        if (entries[entry_index].key != entries[entry_index - 1].key) {
            run_start = entry_index;
            continue;
        }
        const visible_function later = functions[entries[entry_index].position];
        for (size_t earlier_index = run_start;
             status == SXML_STATUS_OK && earlier_index < entry_index; earlier_index += 1) {
            const visible_function earlier = functions[entries[earlier_index].position];
            if (earlier.module_index != later.module_index &&
                earlier.function->required_count <= later.function->parameter_count &&
                later.function->required_count <= earlier.function->parameter_count) {
                /* Positions follow module order, so a declared one is
                   earlier. */
                status = report(reader,
                                earlier.module_index == 0 ? earlier.function->declaration
                                                          : (sxml_ast_node_index){.value = 0},
                                SXML_BYTE_VIEW_FROM_LITERAL(
                                    "XQST0034 a declared or imported function has the name and "
                                    "an arity of another"));
            }
        }
    }
    sxml_allocator_release(reader->allocator, function_memory);
    sxml_allocator_release(reader->allocator, entry_memory);
    return status;
}

sxml_status sxml_xquery_link_static_context(const sxml_xquery_link_request *request,
                                            sxml_xquery_static_context *context) {
    if (request == NULL || context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_xquery_static_request read_request = {
        .ast = context->ast,
        .host_static_context = &context->xpath,
        .unit = NULL,
        .error_record = request->error_record,
        .target_namespace = context->target_namespace,
        .global_number_base = 0,
        .imported_modules = request->imports};
    sxml_xquery_static_context linked = *context;
    linked.imported_modules = request->imports;
    const static_reader reader = {
        .request = &read_request,
        .ast = context->ast,
        .name_pool = sxml_item_heap_get_name_pool(context->xpath.item_heap),
        .type_registry = sxml_item_heap_get_type_registry(context->xpath.item_heap),
        .allocator = context->xpath.allocator,
        .context = &linked};
    sxml_status status = import_variables(&reader, &linked);
    if (status == SXML_STATUS_OK) {
        status = check_variable_names(&reader, &linked);
    }
    if (status == SXML_STATUS_OK) {
        status = check_imported_functions(&reader, &linked);
    }
    /* The arrays moved even on failure; the context owns them. */
    *context = linked;
    if (status != SXML_STATUS_OK) {
        context->imported_modules =
            (sxml_xquery_static_context_view){.static_contexts = NULL, .static_context_count = 0};
    }
    return status;
}

/* ------------------------------------------------------------- Entry */

void sxml_xquery_release_static_context(sxml_xquery_static_context *context) {
    if (context == NULL) {
        return;
    }
    const sxml_allocator allocator = context->xpath.allocator;
    sxml_allocator_release(allocator, context->namespace_bindings);
    sxml_allocator_release(allocator, context->variable_declarations);
    sxml_allocator_release(allocator, context->variable_nodes);
    sxml_allocator_release(allocator, context->private_variables);
    sxml_allocator_release(allocator, context->global_numbers);
    sxml_allocator_release(allocator, context->private_item_types);
    sxml_allocator_release(allocator, context->declared_functions);
    sxml_allocator_release(allocator, context->parameter_types);
    sxml_allocator_release(allocator, context->function_parameters);
    sxml_allocator_release(allocator, context->signatures);
    sxml_allocator_release(allocator, context->static_base_uri_memory);
    sxml_allocator_release(allocator, context->default_collation_memory);
    sxml_allocator_release(allocator, context->decimal_formats);
    sxml_allocator_release(allocator, context->decimal_format_properties);
    sxml_allocator_release(allocator, context->named_item_types);
    sxml_allocator_release(allocator, context->declared_records);
    sxml_allocator_release(allocator, (void *)context->in_scope_schemas);
    *context = (sxml_xquery_static_context){.xpath = {.allocator = allocator}};
}

sxml_status sxml_xquery_read_static_context(const sxml_xquery_static_request *request,
                                            sxml_xquery_static_context *context) {
    if (request == NULL || request->ast == NULL || request->host_static_context == NULL ||
        request->unit == NULL || context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_xpath_static_context *host = request->host_static_context;
    sxml_xquery_static_context built = {.xpath = *host,
                                        .ast = request->ast,
                                        .host_function_table = host->function_table,
                                        .target_namespace = request->target_namespace};
    const static_reader reader = {
        .request = request,
        .ast = request->ast,
        .name_pool = sxml_item_heap_get_name_pool(host->item_heap),
        .type_registry = sxml_item_heap_get_type_registry(host->item_heap),
        .allocator = host->allocator,
        .context = &built};
    prolog_survey survey = {0};
    sxml_status status = survey_prolog(&reader, &survey);
    if (status == SXML_STATUS_OK) {
        status = read_namespaces(&reader, &survey, &built);
    }
    if (status == SXML_STATUS_OK) {
        status = read_base_uri_and_collation(&reader, &survey, &built);
    }
    if (status == SXML_STATUS_OK) {
        status = read_schema_imports(&reader, &survey, &built);
    }
    if (status == SXML_STATUS_OK) {
        status = read_decimal_formats(&reader, &survey, &built);
    }
    if (status == SXML_STATUS_OK) {
        status = read_types(&reader, &survey, &built);
    }
    if (status == SXML_STATUS_OK) {
        status = read_options(&reader);
    }
    if (status == SXML_STATUS_OK) {
        status = check_expression_annotations(&reader);
    }
    if (status == SXML_STATUS_OK) {
        status = read_variables(&reader, &survey, &built);
    }
    if (status == SXML_STATUS_OK) {
        status = read_functions(&reader, &survey, &built);
    }
    if (status == SXML_STATUS_OK) {
        status = intern_unprefixed_calls(&reader, &built);
    }
    if (status == SXML_STATUS_OK) {
        status = read_context_value_type(&reader, &survey, &built);
    }
    if (status != SXML_STATUS_OK) {
        sxml_xquery_release_static_context(&built);
        return status;
    }
    *context = built;
    context->xpath.function_table = (sxml_function_table){
        .state = context, .operations = &declared_function_table_operations};
    return SXML_STATUS_OK;
}
