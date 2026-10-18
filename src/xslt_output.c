/*
 * Output and result documents (src/xslt_output_internal.h; XSLT 4.0,
 * section 26; docs/DECISIONS.md D112, D146, D156; packet 4.13): output
 * definitions, named character maps, xsl:result-document, the delivery of
 * results, the result writer and the result document collector of
 * include/sxml/xslt.h.
 *
 * Golden files: src/xslt_lower_declaration.c (declarations merged by
 * import precedence, static errors at the declaration, explicit-stack
 * walks); src/xslt_lower_control.c (xsl:message: an instruction as an
 * internal native row its lowering calls); src/xquery.c (output
 * declarations through the parameter reader, D146).
 *
 * Nothing of the serializer is copied: an output definition is the
 * texts of its attributes read by sxml_serialization_parameter_reader_read_texts,
 * a result tree is the serializer's sequence normalizer feeding an arena
 * tree, and serializing is sxml_serialize_sequence. No function recurses
 * (D71): character maps expand on an explicit stack.
 *
 * Section numbers are of XSLT 4.0 (W3C QT4 CG draft, October 2026).
 */
#include "xslt_output_internal.h"

#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "processor_internal.h"
#include "serialize_internal.h"
#include "sort_internal.h"
#include "sxml/tree.h"
#include "xdm_internal.h"
#include "xslt_lower_internal.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ================================================= Serialization attributes */

/* What an attribute of xsl:output or xsl:result-document gives. */
typedef enum attribute_role {
    /* A serialization parameter, passed to the parameter reader as text. */
    SXML_ATTRIBUTE_ROLE_PARAMETER,
    /* cdata-section-elements, suppress-indentation: lists that merge
       (26.1.1, 26.2.4). */
    SXML_ATTRIBUTE_ROLE_NAMES,
    /* doctype-public, doctype-system: the empty string means absent. */
    SXML_ATTRIBUTE_ROLE_ABSENT_WHEN_EMPTY,
    /* item-separator: "#absent" means absent. */
    SXML_ATTRIBUTE_ROLE_ITEM_SEPARATOR,
    SXML_ATTRIBUTE_ROLE_BUILD_TREE,
    SXML_ATTRIBUTE_ROLE_PARAMETER_DOCUMENT,
    /* xsl:result-document only. */
    SXML_ATTRIBUTE_ROLE_FORMAT,
    SXML_ATTRIBUTE_ROLE_HREF,
} attribute_role;

typedef struct serialization_attribute {
    /* The attribute on xsl:output; NONE for those it does not have. */
    sxml_xslt_ast_attribute_kind output_kind;
    /* The attribute on xsl:result-document (a value template). */
    sxml_xslt_ast_attribute_kind instruction_kind;
    /* The serialization parameter's name. */
    sxml_byte_view parameter_name;
    attribute_role role;
} serialization_attribute;

#define PARAMETER_ATTRIBUTE(kind, name)                                                           \
    {.output_kind = (kind), .instruction_kind = (kind),                                           \
     .parameter_name = SXML_BYTE_VIEW_INITIALIZER(name), .role = SXML_ATTRIBUTE_ROLE_PARAMETER}

/* The order of a result document instruction's values (format and href
   first, as the lowering evaluates them). use-character-maps is no value
   template (26.2.4) and is read statically. */
static const serialization_attribute serialization_attributes[] = {
    {.output_kind = SXML_XSLT_AST_ATTRIBUTE_NONE,
     .instruction_kind = SXML_XSLT_AST_ATTRIBUTE_FORMAT,
     .parameter_name = SXML_BYTE_VIEW_INITIALIZER("format"),
     .role = SXML_ATTRIBUTE_ROLE_FORMAT},
    {.output_kind = SXML_XSLT_AST_ATTRIBUTE_NONE,
     .instruction_kind = SXML_XSLT_AST_ATTRIBUTE_HREF,
     .parameter_name = SXML_BYTE_VIEW_INITIALIZER("href"),
     .role = SXML_ATTRIBUTE_ROLE_HREF},
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_METHOD, "method"),
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_ALLOW_DUPLICATE_NAMES, "allow-duplicate-names"),
    {.output_kind = SXML_XSLT_AST_ATTRIBUTE_BUILD_TREE,
     .instruction_kind = SXML_XSLT_AST_ATTRIBUTE_BUILD_TREE,
     .parameter_name = SXML_BYTE_VIEW_INITIALIZER("build-tree"),
     .role = SXML_ATTRIBUTE_ROLE_BUILD_TREE},
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_BYTE_ORDER_MARK, "byte-order-mark"),
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_CANONICAL, "canonical"),
    {.output_kind = SXML_XSLT_AST_ATTRIBUTE_CDATA_SECTION_ELEMENTS,
     .instruction_kind = SXML_XSLT_AST_ATTRIBUTE_CDATA_SECTION_ELEMENTS,
     .parameter_name = SXML_BYTE_VIEW_INITIALIZER("cdata-section-elements"),
     .role = SXML_ATTRIBUTE_ROLE_NAMES},
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_CSV_HEADER, "csv-header"),
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_CSV_QUOTE_CHARACTER, "csv-quote-character"),
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_CSV_SEPARATOR, "csv-separator"),
    {.output_kind = SXML_XSLT_AST_ATTRIBUTE_DOCTYPE_PUBLIC,
     .instruction_kind = SXML_XSLT_AST_ATTRIBUTE_DOCTYPE_PUBLIC,
     .parameter_name = SXML_BYTE_VIEW_INITIALIZER("doctype-public"),
     .role = SXML_ATTRIBUTE_ROLE_ABSENT_WHEN_EMPTY},
    {.output_kind = SXML_XSLT_AST_ATTRIBUTE_DOCTYPE_SYSTEM,
     .instruction_kind = SXML_XSLT_AST_ATTRIBUTE_DOCTYPE_SYSTEM,
     .parameter_name = SXML_BYTE_VIEW_INITIALIZER("doctype-system"),
     .role = SXML_ATTRIBUTE_ROLE_ABSENT_WHEN_EMPTY},
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_ENCODING, "encoding"),
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_ESCAPE_SOLIDUS, "escape-solidus"),
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_ESCAPE_URI_ATTRIBUTES, "escape-uri-attributes"),
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_HTML_VERSION, "html-version"),
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_INCLUDE_CONTENT_TYPE, "include-content-type"),
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_INDENT, "indent"),
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_INDENT_ATTRIBUTES, "indent-attributes"),
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_INDENT_UNIT, "indent-unit"),
    {.output_kind = SXML_XSLT_AST_ATTRIBUTE_ITEM_SEPARATOR,
     .instruction_kind = SXML_XSLT_AST_ATTRIBUTE_ITEM_SEPARATOR,
     .parameter_name = SXML_BYTE_VIEW_INITIALIZER("item-separator"),
     .role = SXML_ATTRIBUTE_ROLE_ITEM_SEPARATOR},
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_JSON_LINES, "json-lines"),
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_JSON_NODE_OUTPUT_METHOD, "json-node-output-method"),
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_LINE_ENDING, "line-ending"),
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_MEDIA_TYPE, "media-type"),
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_NORMALIZATION_FORM, "normalization-form"),
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_OMIT_XML_DECLARATION, "omit-xml-declaration"),
    {.output_kind = SXML_XSLT_AST_ATTRIBUTE_PARAMETER_DOCUMENT,
     .instruction_kind = SXML_XSLT_AST_ATTRIBUTE_PARAMETER_DOCUMENT,
     .parameter_name = SXML_BYTE_VIEW_INITIALIZER("parameter-document"),
     .role = SXML_ATTRIBUTE_ROLE_PARAMETER_DOCUMENT},
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_STANDALONE, "standalone"),
    {.output_kind = SXML_XSLT_AST_ATTRIBUTE_SUPPRESS_INDENTATION,
     .instruction_kind = SXML_XSLT_AST_ATTRIBUTE_SUPPRESS_INDENTATION,
     .parameter_name = SXML_BYTE_VIEW_INITIALIZER("suppress-indentation"),
     .role = SXML_ATTRIBUTE_ROLE_NAMES},
    PARAMETER_ATTRIBUTE(SXML_XSLT_AST_ATTRIBUTE_UNDECLARE_PREFIXES, "undeclare-prefixes"),
    /* 26.2.4: output-version on xsl:result-document is xsl:output's
       version. */
    {.output_kind = SXML_XSLT_AST_ATTRIBUTE_VERSION,
     .instruction_kind = SXML_XSLT_AST_ATTRIBUTE_OUTPUT_VERSION,
     .parameter_name = SXML_BYTE_VIEW_INITIALIZER("version"),
     .role = SXML_ATTRIBUTE_ROLE_PARAMETER},
};

#define SERIALIZATION_ATTRIBUTE_COUNT                                                             \
    (sizeof(serialization_attributes) / sizeof(serialization_attributes[0]))

/* ============================================================ Text builder */

/* Bytes collected in heap memory, then copied where they are kept. */
typedef struct text_builder {
    sxml_allocator allocator;
    uint8_t *bytes;
    size_t byte_count;
    size_t byte_capacity;
} text_builder;

static sxml_status append_bytes(text_builder *builder, sxml_byte_view bytes) {
    if (bytes.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    size_t needed = 0;
    sxml_status status = sxml_size_add(builder->byte_count, bytes.byte_count, &needed);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (needed > builder->byte_capacity) {
        size_t capacity = builder->byte_capacity < 64 ? 64 : builder->byte_capacity;
        while (capacity < needed) {
            status = sxml_size_multiply(capacity, 2, &capacity);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        void *memory = NULL;
        status = sxml_allocator_allocate(
            builder->allocator, (sxml_allocation_request){.byte_count = capacity, .alignment = 1},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (builder->byte_count > 0) {
            memcpy(memory, builder->bytes, builder->byte_count);
        }
        sxml_allocator_release(builder->allocator, builder->bytes);
        builder->bytes = memory;
        builder->byte_capacity = capacity;
    }
    memcpy(builder->bytes + builder->byte_count, bytes.bytes, bytes.byte_count);
    builder->byte_count = needed;
    return SXML_STATUS_OK;
}

static sxml_byte_view text_of(const text_builder *builder) {
    return (sxml_byte_view){.bytes = builder->bytes, .byte_count = builder->byte_count};
}

static void release_text(text_builder *builder) {
    sxml_allocator_release(builder->allocator, builder->bytes);
    builder->bytes = NULL;
    builder->byte_count = 0;
    builder->byte_capacity = 0;
}

/* Copies bytes into arena. */
static sxml_status keep_bytes(sxml_arena *arena, sxml_byte_view bytes, sxml_byte_view *kept) {
    if (bytes.byte_count == 0) {
        *kept = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    const sxml_status status = sxml_arena_allocate(
        arena, (sxml_allocation_request){.byte_count = bytes.byte_count, .alignment = 1}, &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memcpy(memory, bytes.bytes, bytes.byte_count);
    *kept = (sxml_byte_view){.bytes = memory, .byte_count = bytes.byte_count};
    return SXML_STATUS_OK;
}

/* An array of element_count elements in arena (none for 0). */
typedef struct array_request {
    size_t element_count;
    size_t element_size;
    size_t alignment;
} array_request;

static sxml_status allocate_array(sxml_arena *arena, array_request request, void **memory) {
    *memory = NULL;
    if (request.element_count == 0) {
        return SXML_STATUS_OK;
    }
    size_t byte_count = 0;
    const sxml_status status =
        sxml_size_multiply(request.element_count, request.element_size, &byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_arena_allocate(
        arena, (sxml_allocation_request){.byte_count = byte_count, .alignment = request.alignment},
        memory);
}

/* Appends Q{uri}local, the EQName of name (XPath 4.0, 2.2.1). */
typedef struct eqname_append {
    const sxml_name_pool *name_pool;
    sxml_name_identifier name;
} eqname_append;

static sxml_status append_eqname(text_builder *builder, eqname_append request) {
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    sxml_status status = sxml_name_pool_get_name_parts(request.name_pool, request.name, &parts);
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_namespace_uri(request.name_pool, parts.namespace_uri,
                                                  &namespace_uri);
    }
    if (status == SXML_STATUS_OK) {
        status = append_bytes(builder, SXML_BYTE_VIEW_FROM_LITERAL("Q{"));
    }
    if (status == SXML_STATUS_OK) {
        status = append_bytes(builder, namespace_uri);
    }
    if (status == SXML_STATUS_OK) {
        status = append_bytes(builder, SXML_BYTE_VIEW_FROM_LITERAL("}"));
    }
    if (status == SXML_STATUS_OK) {
        status = append_bytes(builder, parts.local_name);
    }
    return status;
}

static bool is_whitespace_text(sxml_byte_view text) {
    return sxml_xml_text_trim_whitespace(text).byte_count == 0;
}

/* yes, true, 1 or no, false, 0 (XSLT 4.0, 26.1.4), spaces trimmed. */
typedef struct boolean_text {
    bool is_valid;
    bool is_true;
} boolean_text;

static boolean_text read_boolean_text(sxml_byte_view text) {
    const sxml_byte_view trimmed = sxml_xml_text_trim_whitespace(text);
    static const char *const true_texts[] = {"yes", "true", "1"};
    static const char *const false_texts[] = {"no", "false", "0"};
    for (size_t text_index = 0; text_index < 3; text_index += 1) {
        if (sxml_byte_view_equals(trimmed, sxml_byte_view_from_c_string(true_texts[text_index]))) {
            return (boolean_text){.is_valid = true, .is_true = true};
        }
        if (sxml_byte_view_equals(trimmed, sxml_byte_view_from_c_string(false_texts[text_index]))) {
            return (boolean_text){.is_valid = true, .is_true = false};
        }
    }
    return (boolean_text){.is_valid = false, .is_true = false};
}

/* =========================================================== Mapping sets */

/* One mapping with its position in the concatenation, for sorting. */
typedef struct ordered_mapping {
    sxml_character_mapping mapping;
    size_t position;
} ordered_mapping;

static bool mapping_precedes(const void *state, sxml_sort_pair pair) {
    (void)state;
    const ordered_mapping *first = pair.first;
    const ordered_mapping *second = pair.second;
    if (first->mapping.codepoint != second->mapping.codepoint) {
        return first->mapping.codepoint < second->mapping.codepoint;
    }
    return first->position < second->position;
}

static void sort_mappings(ordered_mapping *mappings, size_t mapping_count) {
    sxml_sort_in_place(&(sxml_sort_request){.elements = mappings,
                                            .element_count = mapping_count,
                                            .element_size = sizeof(ordered_mapping),
                                            .is_before = mapping_precedes,
                                            .state = NULL});
}

/* Character maps concatenated in order, a later mapping of a character
   replacing an earlier one (26.1.6), into one sorted map in arena. */
typedef struct mapping_merge {
    sxml_allocator scratch_allocator;
    sxml_arena *arena;
    const sxml_character_map_view *parts;
    size_t part_count;
} mapping_merge;

static sxml_status merge_mappings(const mapping_merge *merge, sxml_character_map_view *merged) {
    *merged = (sxml_character_map_view){.character_mappings = NULL, .character_mapping_count = 0};
    size_t total = 0;
    sxml_status status = SXML_STATUS_OK;
    for (size_t part_index = 0; status == SXML_STATUS_OK && part_index < merge->part_count;
         part_index += 1) {
        status = sxml_size_add(total, merge->parts[part_index].character_mapping_count, &total);
    }
    if (status != SXML_STATUS_OK || total == 0) {
        return status;
    }
    size_t byte_count = 0;
    status = sxml_size_multiply(total, sizeof(ordered_mapping), &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            merge->scratch_allocator,
            (sxml_allocation_request){.byte_count = byte_count, .alignment = alignof(ordered_mapping)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    ordered_mapping *ordered = memory;
    size_t position = 0;
    for (size_t part_index = 0; part_index < merge->part_count; part_index += 1) {
        const sxml_character_map_view part = merge->parts[part_index];
        for (size_t mapping_index = 0; mapping_index < part.character_mapping_count;
             mapping_index += 1) {
            ordered[position] = (ordered_mapping){
                .mapping = part.character_mappings[mapping_index], .position = position};
            position += 1;
        }
    }
    sort_mappings(ordered, total);
    size_t distinct = 0;
    for (size_t mapping_index = 0; mapping_index < total; mapping_index += 1) {
        const bool is_last = mapping_index + 1 == total ||
                             ordered[mapping_index + 1].mapping.codepoint !=
                                 ordered[mapping_index].mapping.codepoint;
        distinct += is_last ? 1U : 0U;
    }
    void *kept = NULL;
    status = allocate_array(merge->arena,
                            (array_request){.element_count = distinct,
                                            .element_size = sizeof(sxml_character_mapping),
                                            .alignment = alignof(sxml_character_mapping)},
                            &kept);
    if (status == SXML_STATUS_OK) {
        sxml_character_mapping *mappings = kept;
        size_t filled = 0;
        for (size_t mapping_index = 0; mapping_index < total; mapping_index += 1) {
            if (mapping_index + 1 == total || ordered[mapping_index + 1].mapping.codepoint !=
                                                  ordered[mapping_index].mapping.codepoint) {
                mappings[filled] = ordered[mapping_index].mapping;
                filled += 1;
            }
        }
        *merged = (sxml_character_map_view){.character_mappings = mappings,
                                            .character_mapping_count = filled};
    }
    sxml_allocator_release(merge->scratch_allocator, memory);
    return status;
}

/* ====================================================== Stylesheet outputs */

/* One named character map (glossary `named_character_map`). */
typedef struct named_character_map {
    sxml_name_identifier name;
    /* The declaration of highest import precedence. */
    sxml_xslt_ast_node_index node;
    uint32_t precedence;
    /* 0 not expanded, 1 on the expansion stack, 2 expanded. */
    uint32_t expansion_state;
    sxml_character_map_view mappings;
} named_character_map;

/* One output definition. */
typedef struct compiled_definition {
    sxml_name_identifier name;
    /* A declaration of the definition, where its errors are reported. */
    sxml_xslt_ast_node_index node;
    /* The attributes as parameter texts (QNames as EQNames), without
       build-tree, parameter-document and use-character-maps, and without
       the parameters the parameter document gives; in the arena. */
    sxml_serialization_parameter_text *texts;
    size_t text_count;
    /* Owned: the parameter document's document node, and its element. */
    bool has_parameter_document;
    sxml_item parameter_document;
    sxml_node_reference parameter_element;
    /* The names of the parameters the document gives. */
    sxml_byte_view *document_names;
    size_t document_name_count;
    /* The character maps of use-character-maps, then the document's. */
    sxml_character_map_view mappings;
    /* Owns the views of definition.serialization_parameters. */
    sxml_serialization_parameter_reader *reader;
    sxml_xslt_output_definition definition;
} compiled_definition;

/* One result document instruction (glossary). */
typedef struct compiled_instruction {
    sxml_xslt_ast_node_index node;
    /* The serialization_attributes entries it has, in order. */
    uint8_t attribute_numbers[SERIALIZATION_ATTRIBUTE_COUNT];
    uint32_t attribute_count;
    /* Its in-scope namespaces, prefix 0 the default namespace. */
    sxml_namespace_binding_view namespace_bindings;
    sxml_byte_view static_base_uri;
    /* Its use-character-maps, expanded. */
    sxml_character_map_view mappings;
} compiled_instruction;

struct sxml_xslt_stylesheet_outputs {
    /* The allocator of create; the null allocator after initialize. */
    sxml_allocator creating_allocator;
    sxml_processor *processor;
    sxml_arena *arena;
    named_character_map *character_maps;
    size_t character_map_count;
    /* definitions[0] is the unnamed output definition. */
    compiled_definition *definitions;
    size_t definition_count;
    /* By node index, ascending. */
    compiled_instruction *instructions;
    size_t instruction_count;
};

/* What compiling reads. */
typedef struct output_compilation {
    const sxml_xslt_ast *ast;
    sxml_processor *processor;
    sxml_error_record *error_record;
    sxml_xslt_stylesheet_outputs *outputs;
} output_compilation;

static const sxml_xslt_ast_node *node_of(const output_compilation *compilation,
                                         sxml_xslt_ast_node_index node) {
    return sxml_xslt_ast_get_node(compilation->ast, node);
}

static const sxml_xslt_ast_attribute *attribute_of(const output_compilation *compilation,
                                                   sxml_xslt_ast_attribute_query query) {
    return sxml_xslt_ast_get_attribute(compilation->ast,
                                       sxml_xslt_ast_find_attribute(compilation->ast, query));
}

static uint32_t precedence_of(const output_compilation *compilation,
                              sxml_xslt_ast_node_index node) {
    return sxml_xslt_ast_get_module(compilation->ast, node_of(compilation, node)->module)
        ->import_precedence;
}

/* A static error at node: code_and_description is "XTSE1560 text". */
typedef struct output_error {
    sxml_xslt_ast_node_index node;
    sxml_byte_view code_and_description;
} output_error;

static sxml_status report_error(const output_compilation *compilation, output_error error) {
    if (compilation->error_record != NULL) {
        const sxml_byte_view text = error.code_and_description;
        *compilation->error_record = (sxml_error_record){
            .code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(SXML_ERROR_NAMESPACE_URI),
                     .local_name = {.bytes = text.bytes, .byte_count = 8},
                     .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err")},
            .value = NULL,
            .diagnostic = {.description = {.bytes = text.bytes + 9,
                                           .byte_count = text.byte_count - 9},
                           .location = sxml_xslt_ast_get_source_location(compilation->ast,
                                                                         error.node)}};
    }
    return SXML_STATUS_STATIC_ERROR;
}

/* A serialization error of the parameter reader becomes a static error at
   node (D112: SEPM0016 and its kin as static errors of xsl:output). */
static sxml_status report_as_static(const output_compilation *compilation,
                                    sxml_xslt_ast_node_index node) {
    if (compilation->error_record != NULL) {
        compilation->error_record->diagnostic.location =
            sxml_xslt_ast_get_source_location(compilation->ast, node);
    }
    return SXML_STATUS_STATIC_ERROR;
}

/* --------------------------------------------------------- Character maps */

static named_character_map *find_character_map(const sxml_xslt_stylesheet_outputs *outputs,
                                               sxml_name_identifier name) {
    for (size_t map_index = 0; map_index < outputs->character_map_count; map_index += 1) {
        if (outputs->character_maps[map_index].name.index == name.index) {
            return &outputs->character_maps[map_index];
        }
    }
    return NULL;
}

static sxml_xslt_ast_name_token_view use_character_maps_of(const output_compilation *compilation,
                                                           sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_attribute *attribute = attribute_of(
        compilation, (sxml_xslt_ast_attribute_query){
                         .node = node, .kind = SXML_XSLT_AST_ATTRIBUTE_USE_CHARACTER_MAPS});
    if (attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_NAME_LIST) {
        return (sxml_xslt_ast_name_token_view){.name_tokens = NULL, .name_token_count = 0};
    }
    return sxml_xslt_ast_get_name_tokens(compilation->ast, attribute->value.payload.name_tokens);
}

/* The names of use-character-maps on node must name character maps
   (XTSE1590). */
static sxml_status check_used_maps(const output_compilation *compilation,
                                   sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_name_token_view tokens = use_character_maps_of(compilation, node);
    for (size_t token_index = 0; token_index < tokens.name_token_count; token_index += 1) {
        if (find_character_map(compilation->outputs, tokens.name_tokens[token_index].name) ==
            NULL) {
            return report_error(
                compilation,
                (output_error){.node = node,
                               .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                   "XTSE1590 use-character-maps names a character map the "
                                   "stylesheet does not declare (XSLT 4.0, 26.1.6)")});
        }
    }
    return SXML_STATUS_OK;
}

/* The xsl:output-character children of a character map as mappings
   (scratch memory). */
static sxml_status read_own_mappings(const output_compilation *compilation,
                                     sxml_xslt_ast_node_index node,
                                     sxml_character_map_view *own) {
    size_t child_count = 0;
    for (sxml_xslt_ast_node_index child = node_of(compilation, node)->first_child;
         child.value != 0; child = node_of(compilation, child)->next_sibling) {
        child_count += node_of(compilation, child)->kind == SXML_XSLT_AST_KIND_OUTPUT_CHARACTER
                           ? 1U
                           : 0U;
    }
    void *memory = NULL;
    sxml_status status = allocate_array(compilation->outputs->arena,
                                        (array_request){.element_count = child_count,
                                                        .element_size =
                                                            sizeof(sxml_character_mapping),
                                                        .alignment =
                                                            alignof(sxml_character_mapping)},
                                        &memory);
    sxml_character_mapping *mappings = memory;
    size_t filled = 0;
    for (sxml_xslt_ast_node_index child = node_of(compilation, node)->first_child;
         status == SXML_STATUS_OK && child.value != 0;
         child = node_of(compilation, child)->next_sibling) {
        if (node_of(compilation, child)->kind != SXML_XSLT_AST_KIND_OUTPUT_CHARACTER) {
            continue;
        }
        const sxml_xslt_ast_attribute *character = attribute_of(
            compilation, (sxml_xslt_ast_attribute_query){
                             .node = child, .kind = SXML_XSLT_AST_ATTRIBUTE_CHARACTER});
        const sxml_xslt_ast_attribute *replacement = attribute_of(
            compilation,
            (sxml_xslt_ast_attribute_query){.node = child, .kind = SXML_XSLT_AST_ATTRIBUTE_STRING});
        sxml_string text = {0};
        uint32_t codepoint = 0;
        status = sxml_string_from_utf8(sxml_xslt_ast_get_text(compilation->ast,
                                                              character->value.payload.text),
                                       &text);
        sxml_codepoint_iterator iterator = {.string = text, .byte_offset = 0};
        if (status == SXML_STATUS_OK && !sxml_codepoint_iterator_next(&iterator, &codepoint)) {
            status = report_error(compilation,
                                  (output_error){.node = child,
                                                 .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                                     "XTSE0020 the character attribute of "
                                                     "xsl:output-character is not one character "
                                                     "(XSLT 4.0, 26.1.6)")});
        }
        sxml_byte_view kept = {.bytes = NULL, .byte_count = 0};
        if (status == SXML_STATUS_OK) {
            status = keep_bytes(compilation->outputs->arena,
                                sxml_xslt_ast_get_text(compilation->ast,
                                                       replacement->value.payload.text),
                                &kept);
        }
        if (status == SXML_STATUS_OK) {
            mappings[filled] =
                (sxml_character_mapping){.codepoint = codepoint, .replacement_text = kept};
            filled += 1;
        }
    }
    *own = (sxml_character_map_view){.character_mappings = mappings,
                                     .character_mapping_count = filled};
    return status;
}

/* Expands every character map in post-order on an explicit stack (D71):
   a map's mappings are those of its use-character-maps, in order, then
   its own (26.1.6); XTSE1590, XTSE1600. */
typedef struct expansion_frame {
    named_character_map *map;
    size_t next_token;
} expansion_frame;

static sxml_status expand_character_map(const output_compilation *compilation,
                                        named_character_map *map) {
    sxml_xslt_stylesheet_outputs *outputs = compilation->outputs;
    void *memory = NULL;
    sxml_status status = allocate_array(
        outputs->arena,
        (array_request){.element_count = outputs->character_map_count,
                        .element_size = sizeof(expansion_frame),
                        .alignment = alignof(expansion_frame)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    expansion_frame *stack = memory;
    size_t depth = 0;
    stack[depth] = (expansion_frame){.map = map, .next_token = 0};
    depth += 1;
    map->expansion_state = 1;
    while (status == SXML_STATUS_OK && depth > 0) {
        expansion_frame *top = &stack[depth - 1];
        const sxml_xslt_ast_name_token_view tokens =
            use_character_maps_of(compilation, top->map->node);
        if (top->next_token < tokens.name_token_count) {
            const sxml_name_identifier used = tokens.name_tokens[top->next_token].name;
            top->next_token += 1;
            named_character_map *child = find_character_map(outputs, used);
            if (child == NULL) {
                status = check_used_maps(compilation, top->map->node);
            } else if (child->expansion_state == 1) {
                status = report_error(compilation,
                                      (output_error){.node = top->map->node,
                                                     .code_and_description =
                                                         SXML_BYTE_VIEW_FROM_LITERAL(
                                                             "XTSE1600 a character map uses "
                                                             "itself (XSLT 4.0, 26.1.6)")});
            } else if (child->expansion_state == 0) {
                child->expansion_state = 1;
                stack[depth] = (expansion_frame){.map = child, .next_token = 0};
                depth += 1;
            }
            continue;
        }
        /* All used maps are expanded: concatenate them, then the own
           mappings. */
        sxml_character_map_view own = {.character_mappings = NULL, .character_mapping_count = 0};
        status = read_own_mappings(compilation, top->map->node, &own);
        void *parts_memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = allocate_array(outputs->arena,
                                    (array_request){.element_count = tokens.name_token_count + 1,
                                                    .element_size = sizeof(sxml_character_map_view),
                                                    .alignment = alignof(sxml_character_map_view)},
                                    &parts_memory);
        }
        if (status == SXML_STATUS_OK) {
            sxml_character_map_view *parts = parts_memory;
            for (size_t token_index = 0; token_index < tokens.name_token_count; token_index += 1) {
                parts[token_index] =
                    find_character_map(outputs, tokens.name_tokens[token_index].name)->mappings;
            }
            parts[tokens.name_token_count] = own;
            status = merge_mappings(
                &(mapping_merge){.scratch_allocator = compilation->processor->allocator,
                                 .arena = outputs->arena,
                                 .parts = parts,
                                 .part_count = tokens.name_token_count + 1},
                &top->map->mappings);
        }
        top->map->expansion_state = 2;
        depth -= 1;
    }
    return status;
}

/* Collects the character maps (the declaration of highest precedence per
   name, XTSE1580) and expands them. */
static sxml_status compile_character_maps(const output_compilation *compilation) {
    sxml_xslt_stylesheet_outputs *outputs = compilation->outputs;
    const sxml_xslt_ast *ast = compilation->ast;
    size_t declaration_count = 0;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        declaration_count +=
            node_of(compilation, sxml_xslt_ast_get_declaration(ast, order))->kind ==
                    SXML_XSLT_AST_KIND_CHARACTER_MAP
                ? 1U
                : 0U;
    }
    void *memory = NULL;
    sxml_status status = allocate_array(outputs->arena,
                                        (array_request){.element_count = declaration_count,
                                                        .element_size = sizeof(named_character_map),
                                                        .alignment = alignof(named_character_map)},
                                        &memory);
    outputs->character_maps = memory;
    outputs->character_map_count = 0;
    /* 26.1.6: XTSE1580 only for a tie at the highest precedence. */
    sxml_xslt_ast_node_index tie = {.value = 0};
    for (uint32_t order = 1;
         status == SXML_STATUS_OK && sxml_xslt_ast_get_declaration(ast, order).value != 0;
         order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        if (node_of(compilation, declaration)->kind != SXML_XSLT_AST_KIND_CHARACTER_MAP) {
            continue;
        }
        const sxml_name_identifier name =
            attribute_of(compilation, (sxml_xslt_ast_attribute_query){
                                          .node = declaration, .kind = SXML_XSLT_AST_ATTRIBUTE_NAME})
                ->value.payload.name;
        const uint32_t precedence = precedence_of(compilation, declaration);
        named_character_map *existing = find_character_map(outputs, name);
        if (existing == NULL) {
            outputs->character_maps[outputs->character_map_count] =
                (named_character_map){.name = name,
                                      .node = declaration,
                                      .precedence = precedence,
                                      .expansion_state = 0,
                                      .mappings = {.character_mappings = NULL,
                                                   .character_mapping_count = 0}};
            outputs->character_map_count += 1;
        } else if (precedence > existing->precedence) {
            existing->node = declaration;
            existing->precedence = precedence;
        } else if (precedence == existing->precedence) {
            tie = declaration;
        }
    }
    for (size_t map_index = 0; status == SXML_STATUS_OK && tie.value != 0 &&
                               map_index < outputs->character_map_count;
         map_index += 1) {
        const named_character_map *map = &outputs->character_maps[map_index];
        if (map->name.index ==
                attribute_of(compilation, (sxml_xslt_ast_attribute_query){
                                              .node = tie, .kind = SXML_XSLT_AST_ATTRIBUTE_NAME})
                    ->value.payload.name.index &&
            map->precedence == precedence_of(compilation, tie)) {
            status = report_error(
                compilation,
                (output_error){.node = tie,
                               .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                   "XTSE1580 two character maps have the same name and import "
                                   "precedence (XSLT 4.0, 26.1.6)")});
        }
    }
    for (size_t map_index = 0;
         status == SXML_STATUS_OK && map_index < outputs->character_map_count; map_index += 1) {
        named_character_map *map = &outputs->character_maps[map_index];
        if (map->expansion_state == 0) {
            status = expand_character_map(compilation, map);
        }
    }
    return status;
}

/* The mappings of the character maps tokens name, in order (XTSE1590). */
typedef struct used_maps_request {
    sxml_xslt_ast_node_index node;
    /* Mappings to put after them (a parameter document's); may be empty. */
    sxml_character_map_view after;
} used_maps_request;

static sxml_status merge_used_maps(const output_compilation *compilation,
                                   used_maps_request request, sxml_character_map_view *merged) {
    sxml_status status = check_used_maps(compilation, request.node);
    const sxml_xslt_ast_name_token_view tokens = use_character_maps_of(compilation, request.node);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_array(compilation->outputs->arena,
                                (array_request){.element_count = tokens.name_token_count + 1,
                                                .element_size = sizeof(sxml_character_map_view),
                                                .alignment = alignof(sxml_character_map_view)},
                                &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_character_map_view *parts = memory;
    for (size_t token_index = 0; token_index < tokens.name_token_count; token_index += 1) {
        parts[token_index] =
            find_character_map(compilation->outputs, tokens.name_tokens[token_index].name)
                ->mappings;
    }
    parts[tokens.name_token_count] = request.after;
    return merge_mappings(&(mapping_merge){.scratch_allocator = compilation->processor->allocator,
                                           .arena = compilation->outputs->arena,
                                           .parts = parts,
                                           .part_count = tokens.name_token_count + 1},
                          merged);
}

/* ------------------------------------------------ Attributes as texts */

/* The spellings of the KEYWORD values of serialization attributes. */
typedef struct keyword_spelling {
    sxml_xslt_ast_keyword keyword;
    sxml_byte_view text;
} keyword_spelling;

static const keyword_spelling keyword_spellings[] = {
    {.keyword = SXML_XSLT_AST_KEYWORD_XML, .text = SXML_BYTE_VIEW_INITIALIZER("xml")},
    {.keyword = SXML_XSLT_AST_KEYWORD_XHTML, .text = SXML_BYTE_VIEW_INITIALIZER("xhtml")},
    {.keyword = SXML_XSLT_AST_KEYWORD_HTML, .text = SXML_BYTE_VIEW_INITIALIZER("html")},
    {.keyword = SXML_XSLT_AST_KEYWORD_TEXT, .text = SXML_BYTE_VIEW_INITIALIZER("text")},
    {.keyword = SXML_XSLT_AST_KEYWORD_JSON, .text = SXML_BYTE_VIEW_INITIALIZER("json")},
    {.keyword = SXML_XSLT_AST_KEYWORD_CSV, .text = SXML_BYTE_VIEW_INITIALIZER("csv")},
    {.keyword = SXML_XSLT_AST_KEYWORD_ADAPTIVE, .text = SXML_BYTE_VIEW_INITIALIZER("adaptive")},
    {.keyword = SXML_XSLT_AST_KEYWORD_YES, .text = SXML_BYTE_VIEW_INITIALIZER("yes")},
    {.keyword = SXML_XSLT_AST_KEYWORD_NO, .text = SXML_BYTE_VIEW_INITIALIZER("no")},
    {.keyword = SXML_XSLT_AST_KEYWORD_OMIT, .text = SXML_BYTE_VIEW_INITIALIZER("omit")},
    {.keyword = SXML_XSLT_AST_KEYWORD_NFC, .text = SXML_BYTE_VIEW_INITIALIZER("NFC")},
    {.keyword = SXML_XSLT_AST_KEYWORD_NFD, .text = SXML_BYTE_VIEW_INITIALIZER("NFD")},
    {.keyword = SXML_XSLT_AST_KEYWORD_NFKC, .text = SXML_BYTE_VIEW_INITIALIZER("NFKC")},
    {.keyword = SXML_XSLT_AST_KEYWORD_NFKD, .text = SXML_BYTE_VIEW_INITIALIZER("NFKD")},
    {.keyword = SXML_XSLT_AST_KEYWORD_FULLY_NORMALIZED,
     .text = SXML_BYTE_VIEW_INITIALIZER("fully-normalized")},
    {.keyword = SXML_XSLT_AST_KEYWORD_NO_NORMALIZATION, .text = SXML_BYTE_VIEW_INITIALIZER("none")},
};

static sxml_byte_view keyword_text(sxml_xslt_ast_keyword keyword) {
    for (size_t spelling_index = 0;
         spelling_index < sizeof(keyword_spellings) / sizeof(keyword_spellings[0]);
         spelling_index += 1) {
        if (keyword_spellings[spelling_index].keyword == keyword) {
            return keyword_spellings[spelling_index].text;
        }
    }
    return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
}

/* Appends the decimal text of a non-negative number (html-version, read
   as a decimal: "5.0", "4.01"), to six fractional digits. */
static sxml_status append_decimal(text_builder *builder, double number) {
    if (!(number >= 0.0) || number > 1.0e12) {
        return append_bytes(builder, SXML_BYTE_VIEW_FROM_LITERAL("?"));
    }
    const uint64_t scaled = (uint64_t)(number * 1000000.0 + 0.5);
    uint64_t whole = scaled / 1000000U;
    uint64_t fraction = scaled % 1000000U;
    uint8_t digits[32];
    size_t digit_count = 0;
    do {
        digits[digit_count] = (uint8_t)('0' + (whole % 10U));
        digit_count += 1;
        whole /= 10U;
    } while (whole > 0);
    uint8_t text[48];
    size_t text_count = 0;
    while (digit_count > 0) {
        digit_count -= 1;
        text[text_count] = digits[digit_count];
        text_count += 1;
    }
    text[text_count] = '.';
    text_count += 1;
    uint8_t fraction_digits[6];
    for (size_t place = 6; place > 0; place -= 1) {
        fraction_digits[place - 1] = (uint8_t)('0' + (fraction % 10U));
        fraction /= 10U;
    }
    size_t fraction_count = 6;
    while (fraction_count > 1 && fraction_digits[fraction_count - 1] == '0') {
        fraction_count -= 1;
    }
    memcpy(text + text_count, fraction_digits, fraction_count);
    text_count += fraction_count;
    return append_bytes(builder, (sxml_byte_view){.bytes = text, .byte_count = text_count});
}

/* The text of an xsl:output attribute as a parameter document would give
   it (D146): booleans yes/no, names as EQNames. */
static sxml_status append_attribute_text(const output_compilation *compilation,
                                         const sxml_xslt_ast_attribute *attribute,
                                         text_builder *builder) {
    const sxml_xslt_ast_value value = attribute->value;
    switch (value.kind) {
    case SXML_XSLT_AST_VALUE_KIND_TEXT:
        return append_bytes(builder, sxml_xslt_ast_get_text(compilation->ast, value.payload.text));
    case SXML_XSLT_AST_VALUE_KIND_BOOLEAN:
        return append_bytes(builder, value.payload.is_true ? SXML_BYTE_VIEW_FROM_LITERAL("yes")
                                                           : SXML_BYTE_VIEW_FROM_LITERAL("no"));
    case SXML_XSLT_AST_VALUE_KIND_KEYWORD:
        return append_bytes(builder, keyword_text(value.payload.keyword));
    case SXML_XSLT_AST_VALUE_KIND_NUMBER:
        return append_decimal(builder, value.payload.number);
    case SXML_XSLT_AST_VALUE_KIND_NAME:
        return append_eqname(builder, (eqname_append){.name_pool = compilation->processor->name_pool,
                                                      .name = value.payload.name});
    case SXML_XSLT_AST_VALUE_KIND_NAME_LIST: {
        const sxml_xslt_ast_name_token_view tokens =
            sxml_xslt_ast_get_name_tokens(compilation->ast, value.payload.name_tokens);
        sxml_status status = SXML_STATUS_OK;
        for (size_t token_index = 0;
             status == SXML_STATUS_OK && token_index < tokens.name_token_count; token_index += 1) {
            if (token_index > 0) {
                status = append_bytes(builder, SXML_BYTE_VIEW_FROM_LITERAL(" "));
            }
            if (status == SXML_STATUS_OK) {
                status = append_eqname(
                    builder, (eqname_append){.name_pool = compilation->processor->name_pool,
                                             .name = tokens.name_tokens[token_index].name});
            }
        }
        return status;
    }
    case SXML_XSLT_AST_VALUE_KIND_NONE:
    case SXML_XSLT_AST_VALUE_KIND_EXPRESSION:
    case SXML_XSLT_AST_VALUE_KIND_PATTERN:
    case SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE:
    case SXML_XSLT_AST_VALUE_KIND_SEQUENCE_TYPE:
    case SXML_XSLT_AST_VALUE_KIND_ITEM_TYPE:
    case SXML_XSLT_AST_VALUE_KIND_NAMESPACE_BINDING:
        break;
    }
    return SXML_STATUS_OK;
}

/* ----------------------------------------------- Parameter documents */

/* The local names of the output:* children of a parameter document's
   element: the parameters it gives (Serialization 4.0, 3.1). */
typedef struct document_names {
    sxml_byte_view *names;
    size_t name_count;
} document_names;

typedef struct document_names_request {
    sxml_arena *arena;
    const sxml_name_pool *name_pool;
    sxml_node_reference element;
} document_names_request;

static sxml_status collect_document_names(const document_names_request *request,
                                          document_names *names) {
    names->names = NULL;
    names->name_count = 0;
    sxml_node_description description;
    sxml_status status = sxml_node_describe(request->element, &description);
    if (status != SXML_STATUS_OK || !description.has_children) {
        return status;
    }
    const sxml_node_index last = description.last_child;
    void *memory = NULL;
    status = allocate_array(request->arena,
                            (array_request){.element_count =
                                                (size_t)(last.index - description.first_child.index) + 1,
                                            .element_size = sizeof(sxml_byte_view),
                                            .alignment = alignof(sxml_byte_view)},
                            &memory);
    names->names = memory;
    bool has_child = true;
    sxml_node_index child = description.first_child;
    while (status == SXML_STATUS_OK && has_child) {
        const sxml_node_reference child_node = {
            .tree = request->element.tree, .node_index = child, .namespace_number = 0};
        sxml_node_description child_description;
        status = sxml_node_describe(child_node, &child_description);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (child_description.node_kind == SXML_NODE_KIND_ELEMENT) {
            sxml_name_parts parts;
            status = sxml_name_pool_get_name_parts(request->name_pool, child_description.node_name,
                                                   &parts);
            if (status == SXML_STATUS_OK &&
                parts.namespace_uri.index == SXML_NAMESPACE_SERIALIZATION) {
                names->names[names->name_count] = parts.local_name;
                names->name_count += 1;
            }
        }
        has_child = child_description.has_next_sibling;
        child = child_description.next_sibling;
    }
    return status;
}

static bool names_contain(const sxml_byte_view *names, size_t name_count, sxml_byte_view name) {
    for (size_t name_index = 0; name_index < name_count; name_index += 1) {
        if (sxml_byte_view_equals(names[name_index], name)) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------- Output definitions */

/* The definition named name, or NULL. */
static compiled_definition *find_definition(const sxml_xslt_stylesheet_outputs *outputs,
                                            sxml_name_identifier name) {
    for (size_t definition_index = 0; definition_index < outputs->definition_count;
         definition_index += 1) {
        if (outputs->definitions[definition_index].name.index == name.index) {
            return &outputs->definitions[definition_index];
        }
    }
    return NULL;
}

static sxml_name_identifier output_name_of(const output_compilation *compilation,
                                           sxml_xslt_ast_node_index node) {
    const sxml_xslt_ast_attribute *name = attribute_of(
        compilation,
        (sxml_xslt_ast_attribute_query){.node = node, .kind = SXML_XSLT_AST_ATTRIBUTE_NAME});
    return name->value.kind == SXML_XSLT_AST_VALUE_KIND_NAME
               ? name->value.payload.name
               : (sxml_name_identifier){.index = 0};
}

/* The xsl:output of definition that gives attribute: the one of highest
   precedence; XTSE1560 for two at that precedence with different values
   (26.1.1). The attribute's text is appended to builder. */
typedef struct attribute_merge {
    const compiled_definition *definition;
    const serialization_attribute *attribute;
    text_builder *builder;
} attribute_merge;

/* Whether merge_attribute found the attribute. */
typedef struct attribute_presence {
    bool is_given;
} attribute_presence;

static sxml_status merge_attribute(const output_compilation *compilation,
                                   const attribute_merge *merge, attribute_presence *presence) {
    const sxml_xslt_ast *ast = compilation->ast;
    presence->is_given = false;
    sxml_xslt_ast_node_index best = {.value = 0};
    uint32_t best_precedence = 0;
    sxml_status status = SXML_STATUS_OK;
    text_builder best_text = {.allocator = compilation->processor->allocator,
                              .bytes = NULL,
                              .byte_count = 0,
                              .byte_capacity = 0};
    for (uint32_t order = 1;
         status == SXML_STATUS_OK && sxml_xslt_ast_get_declaration(ast, order).value != 0;
         order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        if (node_of(compilation, declaration)->kind != SXML_XSLT_AST_KIND_OUTPUT ||
            output_name_of(compilation, declaration).index != merge->definition->name.index) {
            continue;
        }
        const sxml_xslt_ast_attribute *attribute = attribute_of(
            compilation, (sxml_xslt_ast_attribute_query){.node = declaration,
                                                         .kind = merge->attribute->output_kind});
        if (attribute->kind == SXML_XSLT_AST_ATTRIBUTE_NONE) {
            continue;
        }
        if (merge->attribute->role == SXML_ATTRIBUTE_ROLE_NAMES) {
            /* The union of every declaration's names. */
            if (presence->is_given) {
                status = append_bytes(merge->builder, SXML_BYTE_VIEW_FROM_LITERAL(" "));
            }
            if (status == SXML_STATUS_OK) {
                status = append_attribute_text(compilation, attribute, merge->builder);
            }
            presence->is_given = true;
            continue;
        }
        const uint32_t precedence = precedence_of(compilation, declaration);
        text_builder text = {.allocator = compilation->processor->allocator,
                             .bytes = NULL,
                             .byte_count = 0,
                             .byte_capacity = 0};
        status = append_attribute_text(compilation, attribute, &text);
        if (status == SXML_STATUS_OK && best.value != 0 && precedence == best_precedence &&
            !sxml_byte_view_equals(text_of(&text), text_of(&best_text))) {
            /* Reported only when nothing of higher precedence follows. */
            bool is_overridden = false;
            for (uint32_t later = order + 1; sxml_xslt_ast_get_declaration(ast, later).value != 0;
                 later += 1) {
                const sxml_xslt_ast_node_index other = sxml_xslt_ast_get_declaration(ast, later);
                if (node_of(compilation, other)->kind == SXML_XSLT_AST_KIND_OUTPUT &&
                    output_name_of(compilation, other).index == merge->definition->name.index &&
                    precedence_of(compilation, other) > precedence &&
                    attribute_of(compilation,
                                 (sxml_xslt_ast_attribute_query){
                                     .node = other, .kind = merge->attribute->output_kind})
                            ->kind != SXML_XSLT_AST_ATTRIBUTE_NONE) {
                    is_overridden = true;
                }
            }
            if (!is_overridden) {
                status = report_error(
                    compilation,
                    (output_error){.node = declaration,
                                   .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                       "XTSE1560 two xsl:output declarations of one output "
                                       "definition give an attribute different values (XSLT 4.0, "
                                       "26.1.1)")});
            }
        }
        if (status == SXML_STATUS_OK && (best.value == 0 || precedence >= best_precedence)) {
            if (best.value == 0 || precedence > best_precedence) {
                release_text(&best_text);
                best_text = text;
                text = (text_builder){.allocator = compilation->processor->allocator,
                                      .bytes = NULL,
                                      .byte_count = 0,
                                      .byte_capacity = 0};
                best = declaration;
                best_precedence = precedence;
            }
        }
        release_text(&text);
    }
    if (status == SXML_STATUS_OK && best.value != 0) {
        status = append_bytes(merge->builder, text_of(&best_text));
        presence->is_given = true;
    }
    release_text(&best_text);
    return status;
}

/* use-character-maps over every declaration of the definition, in order
   of increasing import precedence, then declaration order (26.1.1). */
static sxml_status merge_definition_maps(const output_compilation *compilation,
                                         compiled_definition *definition,
                                         sxml_character_map_view document_maps) {
    const sxml_xslt_ast *ast = compilation->ast;
    size_t part_count = 0;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t order = 1;
         status == SXML_STATUS_OK && sxml_xslt_ast_get_declaration(ast, order).value != 0;
         order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        if (node_of(compilation, declaration)->kind == SXML_XSLT_AST_KIND_OUTPUT &&
            output_name_of(compilation, declaration).index == definition->name.index) {
            status = check_used_maps(compilation, declaration);
            part_count += use_character_maps_of(compilation, declaration).name_token_count;
        }
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_array(compilation->outputs->arena,
                                (array_request){.element_count = part_count + 1,
                                                .element_size = sizeof(sxml_character_map_view),
                                                .alignment = alignof(sxml_character_map_view)},
                                &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_character_map_view *parts = memory;
    size_t filled = 0;
    /* Precedences ascending: a selection by the next precedence each
       round (declarations are few). */
    uint32_t floor = 0;
    bool has_more = true;
    while (has_more) {
        has_more = false;
        uint32_t next = UINT32_MAX;
        for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0;
             order += 1) {
            const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
            if (node_of(compilation, declaration)->kind == SXML_XSLT_AST_KIND_OUTPUT &&
                output_name_of(compilation, declaration).index == definition->name.index) {
                const uint32_t precedence = precedence_of(compilation, declaration);
                if (precedence > floor && precedence < next) {
                    next = precedence;
                    has_more = true;
                }
            }
        }
        if (!has_more) {
            break;
        }
        for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0;
             order += 1) {
            const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
            if (node_of(compilation, declaration)->kind != SXML_XSLT_AST_KIND_OUTPUT ||
                output_name_of(compilation, declaration).index != definition->name.index ||
                precedence_of(compilation, declaration) != next) {
                continue;
            }
            const sxml_xslt_ast_name_token_view tokens =
                use_character_maps_of(compilation, declaration);
            for (size_t token_index = 0; token_index < tokens.name_token_count; token_index += 1) {
                parts[filled] =
                    find_character_map(compilation->outputs, tokens.name_tokens[token_index].name)
                        ->mappings;
                filled += 1;
            }
        }
        floor = next;
    }
    parts[filled] = document_maps;
    return merge_mappings(&(mapping_merge){.scratch_allocator = compilation->processor->allocator,
                                           .arena = compilation->outputs->arena,
                                           .parts = parts,
                                           .part_count = filled + 1},
                          &definition->mappings);
}

/* A parameter reader for the outputs' heap. */
static sxml_status make_reader(sxml_processor *processor,
                               sxml_serialization_parameter_reader **reader) {
    return sxml_serialization_parameter_reader_create(
        processor->allocator,
        &(sxml_serialization_parameter_reader_configuration){.allocator = processor->allocator,
                                                              .item_heap = processor->item_heap},
        reader);
}

/* Whether a text list or a document gives the method. */
static bool gives_method(const sxml_serialization_parameter_text *texts, size_t text_count,
                         const document_names *names) {
    const sxml_byte_view method = SXML_BYTE_VIEW_FROM_LITERAL("method");
    for (size_t text_index = 0; text_index < text_count; text_index += 1) {
        if (sxml_byte_view_equals(texts[text_index].parameter_name, method)) {
            return true;
        }
    }
    return names_contain(names->names, names->name_count, method);
}

/* Reads the texts of definition over its parameter document into its
   definition (26.1.5: the document's parameters win over the
   attributes). */
static sxml_status read_definition(const output_compilation *compilation,
                                   compiled_definition *definition) {
    sxml_status status = make_reader(compilation->processor, &definition->reader);
    sxml_serialization_parameters parameters;
    memset(&parameters, 0, sizeof(parameters));
    if (status == SXML_STATUS_OK) {
        status = sxml_serialization_parameter_reader_read_texts(
            definition->reader,
            &(sxml_serialization_parameter_text_request){
                .has_parameter_document = definition->has_parameter_document,
                .parameter_document = definition->parameter_element,
                .parameter_texts = {.parameter_texts = definition->texts,
                                    .parameter_text_count = definition->text_count},
                .namespace_bindings = {.namespace_bindings = NULL, .namespace_binding_count = 0},
                .error_record = compilation->error_record,
                .failed_text_index = NULL},
            &parameters);
        if (status == SXML_STATUS_DYNAMIC_ERROR) {
            return report_as_static(compilation, definition->node);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = merge_definition_maps(compilation, definition, parameters.use_character_maps);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    parameters.use_character_maps = definition->mappings;
    /* 26.1.4: omit-xml-declaration defaults to no in XSLT (D112). */
    if (parameters.omit_xml_declaration == SXML_SERIALIZATION_FLAG_DEFAULT) {
        parameters.omit_xml_declaration = SXML_SERIALIZATION_FLAG_NO;
    }
    definition->definition.serialization_parameters = parameters;
    definition->definition.has_serialization_method = gives_method(
        definition->texts, definition->text_count,
        &(document_names){.names = definition->document_names,
                          .name_count = definition->document_name_count});
    return SXML_STATUS_OK;
}

/* Builds one definition: its texts, its parameter document, its maps. */
static sxml_status compile_definition(const output_compilation *compilation,
                                      compiled_definition *definition) {
    sxml_xslt_stylesheet_outputs *outputs = compilation->outputs;
    void *memory = NULL;
    sxml_status status = allocate_array(
        outputs->arena,
        (array_request){.element_count = SERIALIZATION_ATTRIBUTE_COUNT,
                        .element_size = sizeof(sxml_serialization_parameter_text),
                        .alignment = alignof(sxml_serialization_parameter_text)},
        &memory);
    definition->texts = memory;
    definition->text_count = 0;
    bool builds_tree = false;
    bool gives_build_tree = false;
    for (size_t attribute_index = 0;
         status == SXML_STATUS_OK && attribute_index < SERIALIZATION_ATTRIBUTE_COUNT;
         attribute_index += 1) {
        const serialization_attribute *attribute = &serialization_attributes[attribute_index];
        if (attribute->output_kind == SXML_XSLT_AST_ATTRIBUTE_NONE) {
            continue;
        }
        text_builder text = {.allocator = compilation->processor->allocator,
                             .bytes = NULL,
                             .byte_count = 0,
                             .byte_capacity = 0};
        attribute_presence presence = {.is_given = false};
        status = merge_attribute(
            compilation,
            &(attribute_merge){.definition = definition, .attribute = attribute, .builder = &text},
            &presence);
        const bool is_given = presence.is_given;
        sxml_byte_view kept = {.bytes = NULL, .byte_count = 0};
        if (status == SXML_STATUS_OK && is_given) {
            status = keep_bytes(outputs->arena, text_of(&text), &kept);
        }
        release_text(&text);
        if (status != SXML_STATUS_OK || !is_given) {
            continue;
        }
        switch (attribute->role) {
        case SXML_ATTRIBUTE_ROLE_BUILD_TREE:
            gives_build_tree = true;
            builds_tree = read_boolean_text(kept).is_true;
            continue;
        case SXML_ATTRIBUTE_ROLE_PARAMETER_DOCUMENT: {
            /* 26.1.5: a document that cannot be read, or has no
               element, is ignored. */
            sxml_processor_document_loader loader_memory;
            sxml_processor_document_loader *loader = NULL;
            status = sxml_processor_compile_document_loader_initialize(
                (sxml_memory_view){.bytes = (uint8_t *)&loader_memory,
                                   .byte_count = sizeof(loader_memory)},
                compilation->processor, &loader);
            sxml_parameter_document loaded = {.is_loaded = false};
            if (status == SXML_STATUS_OK) {
                status = sxml_parameter_document_load(
                    &(sxml_parameter_document_request){
                        .loader = sxml_processor_document_loader_as_document_loader(loader),
                        .uri_reference = kept,
                        .base_uri = sxml_xslt_ast_get_text(
                            compilation->ast,
                            sxml_xslt_ast_get_static_context(
                                compilation->ast,
                                node_of(compilation, definition->node)->static_context)
                                ->static_base_uri)},
                    &loaded);
                sxml_processor_document_loader_finalize(loader);
            }
            if (status == SXML_STATUS_OK && loaded.has_document_element) {
                definition->has_parameter_document = true;
                definition->parameter_document = loaded.document_node;
                definition->parameter_element = loaded.document_element;
            } else {
                sxml_parameter_document_release(&loaded);
            }
            continue;
        }
        case SXML_ATTRIBUTE_ROLE_ABSENT_WHEN_EMPTY:
            if (kept.byte_count == 0) {
                continue;
            }
            break;
        case SXML_ATTRIBUTE_ROLE_ITEM_SEPARATOR:
            if (sxml_byte_view_equals(kept, SXML_BYTE_VIEW_FROM_LITERAL("#absent"))) {
                continue;
            }
            break;
        case SXML_ATTRIBUTE_ROLE_PARAMETER:
        case SXML_ATTRIBUTE_ROLE_NAMES:
        case SXML_ATTRIBUTE_ROLE_FORMAT:
        case SXML_ATTRIBUTE_ROLE_HREF:
            break;
        }
        definition->texts[definition->text_count] = (sxml_serialization_parameter_text){
            .parameter_name = attribute->parameter_name, .parameter_value = kept};
        definition->text_count += 1;
    }
    document_names names = {.names = NULL, .name_count = 0};
    if (status == SXML_STATUS_OK && definition->has_parameter_document) {
        status = collect_document_names(
            &(document_names_request){.arena = outputs->arena,
                                      .name_pool = compilation->processor->name_pool,
                                      .element = definition->parameter_element},
            &names);
    }
    if (status == SXML_STATUS_OK) {
        /* 26.1.5: the document's parameters win over the attributes. */
        size_t kept_count = 0;
        for (size_t text_index = 0; text_index < definition->text_count; text_index += 1) {
            if (!names_contain(names.names, names.name_count,
                               definition->texts[text_index].parameter_name)) {
                definition->texts[kept_count] = definition->texts[text_index];
                kept_count += 1;
            }
        }
        definition->text_count = kept_count;
        definition->document_names = names.names;
        definition->document_name_count = names.name_count;
        definition->definition.build_tree =
            gives_build_tree ? (builds_tree ? SXML_SERIALIZATION_FLAG_YES : SXML_SERIALIZATION_FLAG_NO)
                             : SXML_SERIALIZATION_FLAG_DEFAULT;
        status = read_definition(compilation, definition);
    }
    return status;
}

static sxml_status compile_definitions(const output_compilation *compilation) {
    sxml_xslt_stylesheet_outputs *outputs = compilation->outputs;
    const sxml_xslt_ast *ast = compilation->ast;
    size_t declaration_count = 0;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        declaration_count +=
            node_of(compilation, sxml_xslt_ast_get_declaration(ast, order))->kind ==
                    SXML_XSLT_AST_KIND_OUTPUT
                ? 1U
                : 0U;
    }
    void *memory = NULL;
    sxml_status status = allocate_array(outputs->arena,
                                        (array_request){.element_count = declaration_count + 1,
                                                        .element_size = sizeof(compiled_definition),
                                                        .alignment = alignof(compiled_definition)},
                                        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    outputs->definitions = memory;
    memset(memory, 0, (declaration_count + 1) * sizeof(compiled_definition));
    /* The unnamed definition always exists (26.1.1). */
    outputs->definition_count = 1;
    outputs->definitions[0].node = sxml_xslt_ast_get_module(ast, (sxml_xslt_ast_module_index){.value = 1})->root;
    for (uint32_t order = 1; sxml_xslt_ast_get_declaration(ast, order).value != 0; order += 1) {
        const sxml_xslt_ast_node_index declaration = sxml_xslt_ast_get_declaration(ast, order);
        if (node_of(compilation, declaration)->kind != SXML_XSLT_AST_KIND_OUTPUT) {
            continue;
        }
        const sxml_name_identifier name = output_name_of(compilation, declaration);
        compiled_definition *definition = find_definition(outputs, name);
        if (definition == NULL) {
            definition = &outputs->definitions[outputs->definition_count];
            definition->name = name;
            outputs->definition_count += 1;
        }
        definition->node = declaration;
    }
    for (size_t definition_index = 0;
         status == SXML_STATUS_OK && definition_index < outputs->definition_count;
         definition_index += 1) {
        status = compile_definition(compilation, &outputs->definitions[definition_index]);
    }
    return status;
}

/* --------------------------------------------- Result document instructions */

static sxml_status compile_instruction(const output_compilation *compilation,
                                       compiled_instruction *instruction) {
    const sxml_xslt_ast_node *node = node_of(compilation, instruction->node);
    instruction->attribute_count = 0;
    for (size_t attribute_index = 0; attribute_index < SERIALIZATION_ATTRIBUTE_COUNT;
         attribute_index += 1) {
        const sxml_xslt_ast_attribute *attribute = attribute_of(
            compilation, (sxml_xslt_ast_attribute_query){
                             .node = instruction->node,
                             .kind = serialization_attributes[attribute_index].instruction_kind});
        if (attribute->value.kind == SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE) {
            instruction->attribute_numbers[instruction->attribute_count] = (uint8_t)attribute_index;
            instruction->attribute_count += 1;
        }
    }
    sxml_xslt_stylesheet_outputs *outputs = compilation->outputs;
    const sxml_xslt_ast_module *module = sxml_xslt_ast_get_module(compilation->ast, node->module);
    sxml_namespace_binding_view bindings = {.namespace_bindings = NULL,
                                            .namespace_binding_count = 0};
    sxml_status status = SXML_STATUS_OK;
    if (module->tree != NULL) {
        status = sxml_node_get_in_scope_namespaces(
            (sxml_node_reference){
                .tree = module->tree, .node_index = node->source_node, .namespace_number = 0},
            &bindings);
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_array(outputs->arena,
                                (array_request){.element_count = bindings.namespace_binding_count,
                                                .element_size = sizeof(sxml_namespace_binding),
                                                .alignment = alignof(sxml_namespace_binding)},
                                &memory);
    }
    if (status == SXML_STATUS_OK && bindings.namespace_binding_count > 0) {
        memcpy(memory, bindings.namespace_bindings,
               bindings.namespace_binding_count * sizeof(sxml_namespace_binding));
    }
    instruction->namespace_bindings = (sxml_namespace_binding_view){
        .namespace_bindings = memory, .namespace_binding_count = bindings.namespace_binding_count};
    if (status == SXML_STATUS_OK) {
        status = keep_bytes(
            outputs->arena,
            sxml_xslt_ast_get_text(
                compilation->ast,
                sxml_xslt_ast_get_static_context(compilation->ast, node->static_context)
                    ->static_base_uri),
            &instruction->static_base_uri);
    }
    if (status == SXML_STATUS_OK) {
        status = merge_used_maps(
            compilation,
            (used_maps_request){.node = instruction->node,
                                .after = {.character_mappings = NULL, .character_mapping_count = 0}},
            &instruction->mappings);
    }
    return status;
}

static sxml_status compile_instructions(const output_compilation *compilation) {
    sxml_xslt_stylesheet_outputs *outputs = compilation->outputs;
    const sxml_xslt_ast *ast = compilation->ast;
    size_t instruction_count = 0;
    for (uint32_t node_index = 1; node_index < ast->node_count; node_index += 1) {
        instruction_count +=
            node_of(compilation, (sxml_xslt_ast_node_index){.value = node_index})->kind ==
                    SXML_XSLT_AST_KIND_RESULT_DOCUMENT
                ? 1U
                : 0U;
    }
    void *memory = NULL;
    sxml_status status = allocate_array(outputs->arena,
                                        (array_request){.element_count = instruction_count,
                                                        .element_size = sizeof(compiled_instruction),
                                                        .alignment = alignof(compiled_instruction)},
                                        &memory);
    outputs->instructions = memory;
    outputs->instruction_count = 0;
    for (uint32_t node_index = 1; status == SXML_STATUS_OK && node_index < ast->node_count;
         node_index += 1) {
        const sxml_xslt_ast_node_index node = {.value = node_index};
        if (node_of(compilation, node)->kind != SXML_XSLT_AST_KIND_RESULT_DOCUMENT) {
            continue;
        }
        compiled_instruction *instruction = &outputs->instructions[outputs->instruction_count];
        memset(instruction, 0, sizeof(*instruction));
        instruction->node = node;
        outputs->instruction_count += 1;
        status = compile_instruction(compilation, instruction);
    }
    return status;
}

static const compiled_instruction *find_instruction(const sxml_xslt_stylesheet_outputs *outputs,
                                                    uint32_t node_index) {
    size_t low = 0;
    size_t high = outputs->instruction_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const uint32_t value = outputs->instructions[middle].node.value;
        if (value == node_index) {
            return &outputs->instructions[middle];
        }
        if (value < node_index) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ Lifecycle */

sxml_status sxml_xslt_stylesheet_outputs_memory_requirements(
    const sxml_xslt_stylesheet_outputs_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (configuration == NULL || configuration->ast == NULL ||
        configuration->processor == NULL || requirements == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){.byte_count = sizeof(sxml_xslt_stylesheet_outputs),
                                               .alignment = alignof(sxml_xslt_stylesheet_outputs)};
    return SXML_STATUS_OK;
}

static void release_outputs(sxml_xslt_stylesheet_outputs *outputs) {
    for (size_t definition_index = 0; definition_index < outputs->definition_count;
         definition_index += 1) {
        compiled_definition *definition = &outputs->definitions[definition_index];
        sxml_serialization_parameter_reader_destroy(definition->reader);
        if (definition->has_parameter_document) {
            sxml_item_release(definition->parameter_document);
        }
    }
    outputs->definition_count = 0;
    sxml_arena_destroy(outputs->arena);
    outputs->arena = NULL;
}

sxml_status sxml_xslt_stylesheet_outputs_initialize(
    sxml_memory_view memory, const sxml_xslt_stylesheet_outputs_configuration *configuration,
    sxml_xslt_stylesheet_outputs **outputs) {
    sxml_memory_requirements requirements;
    sxml_status status =
        sxml_xslt_stylesheet_outputs_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK || outputs == NULL || !configuration->ast->is_sealed) {
        return status == SXML_STATUS_OK ? SXML_STATUS_INVALID_ARGUMENT : status;
    }
    status = sxml_memory_view_check_requirements(memory, requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* The memory was checked for the size and alignment. */
    void *block = memory.bytes;
    sxml_xslt_stylesheet_outputs *made = block;
    memset(made, 0, sizeof(*made));
    made->creating_allocator = (sxml_allocator){.state = NULL, .operations = NULL};
    made->processor = configuration->processor;
    status = sxml_arena_create(
        configuration->processor->allocator,
        &(sxml_arena_configuration){.block_allocator = configuration->processor->allocator,
                                    .block_byte_count = 4096,
                                    .maximum_block_byte_count = 0,
                                    .initial_memory = {.bytes = NULL, .byte_count = 0}},
        &made->arena);
    const output_compilation compilation = {.ast = configuration->ast,
                                            .processor = configuration->processor,
                                            .error_record = configuration->error_record,
                                            .outputs = made};
    if (status == SXML_STATUS_OK) {
        status = compile_character_maps(&compilation);
    }
    if (status == SXML_STATUS_OK) {
        status = compile_definitions(&compilation);
    }
    if (status == SXML_STATUS_OK) {
        status = compile_instructions(&compilation);
    }
    if (status != SXML_STATUS_OK) {
        release_outputs(made);
        return status;
    }
    *outputs = made;
    return SXML_STATUS_OK;
}

void sxml_xslt_stylesheet_outputs_finalize(sxml_xslt_stylesheet_outputs *outputs) {
    if (outputs == NULL) {
        return;
    }
    release_outputs(outputs);
}

sxml_status sxml_xslt_stylesheet_outputs_create(
    sxml_allocator allocator, const sxml_xslt_stylesheet_outputs_configuration *configuration,
    sxml_xslt_stylesheet_outputs **outputs) {
    sxml_memory_requirements requirements;
    sxml_status status =
        sxml_xslt_stylesheet_outputs_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK || outputs == NULL) {
        return status == SXML_STATUS_OK ? SXML_STATUS_INVALID_ARGUMENT : status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = requirements.byte_count,
                                  .alignment = requirements.alignment},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xslt_stylesheet_outputs *made = NULL;
    status = sxml_xslt_stylesheet_outputs_initialize(
        (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count}, configuration,
        &made);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    made->creating_allocator = allocator;
    *outputs = made;
    return SXML_STATUS_OK;
}

void sxml_xslt_stylesheet_outputs_destroy(sxml_xslt_stylesheet_outputs *outputs) {
    if (outputs == NULL) {
        return;
    }
    const sxml_allocator allocator = outputs->creating_allocator;
    sxml_xslt_stylesheet_outputs_finalize(outputs);
    sxml_allocator_release(allocator, outputs);
}

sxml_status sxml_xslt_stylesheet_outputs_get_definition(
    const sxml_xslt_stylesheet_outputs *outputs, sxml_name_identifier output_definition_name,
    sxml_xslt_output_definition *output_definition) {
    if (outputs == NULL || output_definition == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const compiled_definition *definition = find_definition(outputs, output_definition_name);
    if (definition == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *output_definition = definition->definition;
    return SXML_STATUS_OK;
}

sxml_xslt_character_map_answer sxml_xslt_stylesheet_outputs_find_character_map(
    const sxml_xslt_stylesheet_outputs *outputs, sxml_name_identifier character_map_name) {
    const named_character_map *map =
        outputs == NULL ? NULL : find_character_map(outputs, character_map_name);
    if (map == NULL) {
        return (sxml_xslt_character_map_answer){
            .is_found = false,
            .character_map = {.character_mappings = NULL, .character_mapping_count = 0}};
    }
    return (sxml_xslt_character_map_answer){.is_found = true, .character_map = map->mappings};
}

/* ================================================================ Delivery */

static const sxml_byte_view xhtml_namespace_uri =
    SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/1999/xhtml");

static bool equals_ignoring_case(sxml_byte_view text, sxml_byte_view lower_case) {
    if (text.byte_count != lower_case.byte_count) {
        return false;
    }
    for (size_t offset = 0; offset < text.byte_count; offset += 1) {
        uint8_t byte = text.bytes[offset];
        if (byte >= 'A' && byte <= 'Z') {
            byte = (uint8_t)(byte - 'A' + 'a');
        }
        if (byte != lower_case.bytes[offset]) {
            return false;
        }
    }
    return true;
}

/* What one node of the raw result says about the method (26.1.2). */
typedef enum method_evidence {
    /* Nothing yet: whitespace text, a comment, a processing instruction. */
    SXML_METHOD_EVIDENCE_NONE,
    SXML_METHOD_EVIDENCE_XML,
    SXML_METHOD_EVIDENCE_XHTML,
    SXML_METHOD_EVIDENCE_HTML,
} method_evidence;

typedef struct evidence_request {
    const sxml_name_pool *name_pool;
    sxml_node_reference node;
} evidence_request;

static sxml_status read_evidence(evidence_request request, method_evidence *evidence) {
    sxml_node_description description;
    sxml_status status = sxml_node_describe(request.node, &description);
    *evidence = SXML_METHOD_EVIDENCE_NONE;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    switch (description.node_kind) {
    case SXML_NODE_KIND_ELEMENT: {
        sxml_name_parts parts;
        sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
        status = sxml_name_pool_get_name_parts(request.name_pool, description.node_name, &parts);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_namespace_uri(request.name_pool, parts.namespace_uri,
                                                      &namespace_uri);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const sxml_byte_view html = SXML_BYTE_VIEW_FROM_LITERAL("html");
        if (sxml_byte_view_equals(parts.local_name, html) &&
            sxml_byte_view_equals(namespace_uri, xhtml_namespace_uri)) {
            *evidence = SXML_METHOD_EVIDENCE_XHTML;
        } else if (namespace_uri.byte_count == 0 && equals_ignoring_case(parts.local_name, html)) {
            *evidence = SXML_METHOD_EVIDENCE_HTML;
        } else {
            *evidence = SXML_METHOD_EVIDENCE_XML;
        }
        return SXML_STATUS_OK;
    }
    case SXML_NODE_KIND_TEXT: {
        sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
        status = sxml_node_get_content(request.node, &content);
        if (status == SXML_STATUS_OK && !is_whitespace_text(content)) {
            *evidence = SXML_METHOD_EVIDENCE_XML;
        }
        return status;
    }
    case SXML_NODE_KIND_COMMENT:
    case SXML_NODE_KIND_PROCESSING_INSTRUCTION:
        return SXML_STATUS_OK;
    case SXML_NODE_KIND_DOCUMENT:
    case SXML_NODE_KIND_ATTRIBUTE:
    case SXML_NODE_KIND_NAMESPACE:
    case SXML_NODE_KIND_ANY:
        *evidence = SXML_METHOD_EVIDENCE_XML;
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_OK;
}

/* The evidence of a document node's children. */
static sxml_status read_document_evidence(evidence_request request, method_evidence *evidence) {
    sxml_node_description description;
    sxml_status status = sxml_node_describe(request.node, &description);
    *evidence = SXML_METHOD_EVIDENCE_NONE;
    bool has_child = status == SXML_STATUS_OK && description.has_children;
    sxml_node_index child = description.first_child;
    while (has_child && *evidence == SXML_METHOD_EVIDENCE_NONE) {
        const sxml_node_reference child_node = {
            .tree = request.node.tree, .node_index = child, .namespace_number = 0};
        status = read_evidence((evidence_request){.name_pool = request.name_pool,
                                                  .node = child_node},
                               evidence);
        sxml_node_description child_description;
        if (status == SXML_STATUS_OK) {
            status = sxml_node_describe(child_node, &child_description);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        has_child = child_description.has_next_sibling;
        child = child_description.next_sibling;
    }
    return status;
}

/* The method a definition without one takes for value (26.1.2): json for
   a map, an array or a JNode first; else by the first element of the
   result tree. */
static sxml_status choose_method(sxml_item_heap *item_heap, const sxml_xslt_result_request *request,
                                 sxml_serialization_method *method) {
    *method = SXML_SERIALIZATION_METHOD_XML;
    const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(item_heap);
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(request->value);
    method_evidence evidence = SXML_METHOD_EVIDENCE_NONE;
    bool is_first = true;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && evidence == SXML_METHOD_EVIDENCE_NONE) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, request->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        const sxml_item_kind kind = sxml_item_get_kind(step.item);
        if (is_first && (kind == SXML_ITEM_KIND_MAP || kind == SXML_ITEM_KIND_ARRAY ||
                         kind == SXML_ITEM_KIND_JSON_NODE)) {
            *method = SXML_SERIALIZATION_METHOD_JSON;
            return SXML_STATUS_OK;
        }
        is_first = false;
        if (kind != SXML_ITEM_KIND_NODE) {
            evidence = SXML_METHOD_EVIDENCE_XML;
            break;
        }
        sxml_node_reference node;
        sxml_node_description description;
        status = sxml_item_get_node(step.item, &node);
        if (status == SXML_STATUS_OK) {
            status = sxml_node_describe(node, &description);
        }
        if (status == SXML_STATUS_OK) {
            const evidence_request read = {.name_pool = name_pool, .node = node};
            status = description.node_kind == SXML_NODE_KIND_DOCUMENT
                         ? read_document_evidence(read, &evidence)
                         : read_evidence(read, &evidence);
        }
    }
    if (evidence == SXML_METHOD_EVIDENCE_XHTML && !request->prefers_xml_for_xhtml) {
        *method = SXML_SERIALIZATION_METHOD_XHTML;
    } else if (evidence == SXML_METHOD_EVIDENCE_HTML) {
        *method = SXML_SERIALIZATION_METHOD_HTML;
    }
    return status;
}

/* The parameters a serialization of request uses: its method or the one
   chosen, indent DEFAULT as yes for html and xhtml (26.1.4). */
static sxml_status effective_parameters(sxml_item_heap *item_heap,
                                        const sxml_xslt_result_request *request,
                                        sxml_serialization_parameters *parameters) {
    *parameters = request->output_definition->serialization_parameters;
    if (!request->output_definition->has_serialization_method) {
        const sxml_status status = choose_method(item_heap, request, &parameters->serialization_method);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (parameters->indent == SXML_SERIALIZATION_FLAG_DEFAULT &&
        (parameters->serialization_method == SXML_SERIALIZATION_METHOD_HTML ||
         parameters->serialization_method == SXML_SERIALIZATION_METHOD_XHTML)) {
        parameters->indent = SXML_SERIALIZATION_FLAG_YES;
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_xslt_serialize_result(sxml_item_heap *item_heap,
                                       const sxml_xslt_result_request *request,
                                       sxml_output_stream output_stream) {
    if (item_heap == NULL || request == NULL || request->output_definition == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_serialization_parameters parameters;
    const sxml_status status = effective_parameters(item_heap, request, &parameters);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_serialize_sequence(item_heap,
                                   &(sxml_serialization_request){.value = request->value,
                                                                 .serialization_parameters =
                                                                     parameters,
                                                                 .error_record =
                                                                     request->error_record},
                                   output_stream);
}

sxml_status sxml_xslt_result_builds_tree(sxml_item_heap *item_heap,
                                         const sxml_xslt_result_request *request,
                                         sxml_xslt_tree_choice *choice) {
    const sxml_xslt_output_definition *definition = request->output_definition;
    if (definition->build_tree != SXML_SERIALIZATION_FLAG_DEFAULT) {
        choice->builds_tree = definition->build_tree == SXML_SERIALIZATION_FLAG_YES;
        return SXML_STATUS_OK;
    }
    /* 26.1: yes for xml, xhtml, html and text, given or chosen. */
    sxml_serialization_method method = definition->serialization_parameters.serialization_method;
    if (!definition->has_serialization_method) {
        const sxml_status status = choose_method(item_heap, request, &method);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    choice->builds_tree = method == SXML_SERIALIZATION_METHOD_XML ||
                   method == SXML_SERIALIZATION_METHOD_XHTML ||
                   method == SXML_SERIALIZATION_METHOD_HTML ||
                   method == SXML_SERIALIZATION_METHOD_TEXT;
    return SXML_STATUS_OK;
}

/* Sends the normalized children to builder (Serialization 4.0, section 2;
   the serializer's normalizer, nothing of it copied). */
typedef struct normalization_feed {
    sxml_serializer *serializer;
    sxml_sequence value;
    sxml_tree_builder builder;
} normalization_feed;

static sxml_status feed_normalized_children(const normalization_feed *feed) {
    sxml_sequence_normalizer normalizer;
    sxml_sequence_normalizer_start(feed->serializer, feed->value, &normalizer);
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK) {
        sxml_normalized_event event;
        status = sxml_sequence_normalizer_next(&normalizer, &event);
        if (status != SXML_STATUS_OK || event.event_kind == SXML_NORMALIZED_EVENT_KIND_END) {
            break;
        }
        if (event.event_kind == SXML_NORMALIZED_EVENT_KIND_TEXT) {
            status = feed->builder.operations->add_text(
                feed->builder.state,
                &(sxml_text_event){.text = event.text,
                                   .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                                                .line_number = 0,
                                                .column_number = 0,
                                                .byte_offset = 0}});
        } else {
            status = sxml_tree_builder_copy_node(feed->builder,
                                                 &(sxml_copy_request){
                                                     .node = event.node,
                                                     .preserves_namespaces = true,
                                                     .inherits_namespaces = true,
                                                     .preserves_type_annotations = true});
        }
    }
    sxml_sequence_normalizer_finish(&normalizer);
    return status;
}

sxml_status sxml_xslt_build_result_tree(sxml_item_heap *item_heap,
                                        const sxml_xslt_result_request *request,
                                        sxml_sequence *document) {
    if (item_heap == NULL || request == NULL || request->output_definition == NULL ||
        document == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_allocator allocator = sxml_item_heap_as_allocator(item_heap);
    sxml_serializer serializer;
    memset(&serializer, 0, sizeof(serializer));
    serializer.item_heap = item_heap;
    serializer.name_pool = sxml_item_heap_get_name_pool(item_heap);
    serializer.registry = sxml_item_heap_get_type_registry(item_heap);
    serializer.allocator = allocator;
    serializer.error_record = request->error_record;
    serializer.parameters = &request->output_definition->serialization_parameters;
    sxml_arena_tree *arena_tree = NULL;
    sxml_status status = sxml_arena_tree_create(
        allocator,
        &(sxml_arena_tree_configuration){.allocator = allocator,
                                         .initial_memory = {.bytes = NULL, .byte_count = 0},
                                         .item_heap = item_heap,
                                         .maximum_node_count = 0,
                                         .maximum_depth = 0,
                                         .maximum_text_byte_count = 0,
                                         .keeps_locations = false,
                                         .duplicate_attribute_policy =
                                             SXML_DUPLICATE_ATTRIBUTE_POLICY_REPLACE,
                                         .is_destroyed_when_unreferenced = true,
                                         .error_record = request->error_record,
                                         .base_uri = request->result_uri},
        &arena_tree);
    sxml_tree_builder builder = {.state = NULL, .operations = NULL};
    if (status == SXML_STATUS_OK) {
        builder = sxml_arena_tree_as_tree_builder(arena_tree);
        status = builder.operations->start_document(
            builder.state,
            &(sxml_document_start_event){.document_uri = {.bytes = NULL, .byte_count = 0},
                                         .base_uri = request->result_uri,
                                         .location = {.source_name = {.bytes = NULL,
                                                                      .byte_count = 0},
                                                      .line_number = 0,
                                                      .column_number = 0,
                                                      .byte_offset = 0}});
    }
    if (status == SXML_STATUS_OK) {
        status = feed_normalized_children(&(normalization_feed){
            .serializer = &serializer, .value = request->value, .builder = builder});
    }
    if (status == SXML_STATUS_OK) {
        status = builder.operations->end_document(builder.state);
    }
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    const sxml_item item = sxml_node_item_make(
        (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0});
    sxml_tree_release(tree);
    *document = sxml_sequence_from_item(item);
    return SXML_STATUS_OK;
}

/* What a result is delivered as: delivery, the definition, the URI. */
typedef struct delivery_request {
    sxml_xslt_result_delivery result_delivery;
    sxml_output_stream output_stream;
    sxml_xslt_result_request result;
} delivery_request;

/* Delivers request->result.value as request asks: *delivered receives the
   value to hand over, owned (the empty sequence when serialized). */
static sxml_status deliver_result(sxml_item_heap *item_heap, const delivery_request *request,
                                  sxml_sequence *delivered) {
    sxml_xslt_result_delivery delivery = request->result_delivery;
    sxml_status status = SXML_STATUS_OK;
    if (delivery == SXML_XSLT_RESULT_DELIVERY_DEFAULT) {
        sxml_xslt_tree_choice choice = {.builds_tree = false};
        status = sxml_xslt_result_builds_tree(item_heap, &request->result, &choice);
        delivery = choice.builds_tree ? SXML_XSLT_RESULT_DELIVERY_TREE : SXML_XSLT_RESULT_DELIVERY_RAW;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    switch (delivery) {
    case SXML_XSLT_RESULT_DELIVERY_DEFAULT:
    case SXML_XSLT_RESULT_DELIVERY_RAW:
        sxml_sequence_retain(request->result.value);
        *delivered = request->result.value;
        return SXML_STATUS_OK;
    case SXML_XSLT_RESULT_DELIVERY_TREE:
        return sxml_xslt_build_result_tree(item_heap, &request->result, delivered);
    case SXML_XSLT_RESULT_DELIVERY_SERIALIZED:
        if (request->output_stream.operations == NULL) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        status = sxml_xslt_serialize_result(item_heap, &request->result, request->output_stream);
        if (status == SXML_STATUS_OK) {
            *delivered = sxml_sequence_empty();
        }
        return status;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* ============================================================ Result writer */

/* One result document between open and close (glossary
   `open_result_document`). */
typedef struct open_result_document {
    uint64_t number;
    /* Owned through the processor's allocator. */
    void *uri_memory;
    sxml_byte_view absolute_uri;
    /* The effective href, in arena. */
    sxml_byte_view href;
    /* Own the views of definition. */
    sxml_serialization_parameter_reader *reader;
    sxml_arena *arena;
    sxml_xslt_output_definition definition;
    sxml_xslt_result_document_target target;
} open_result_document;

/* A URI a result document was written to (XTDE1490). */
typedef struct written_uri {
    void *memory;
    sxml_byte_view uri;
} written_uri;

struct sxml_xslt_result_writer {
    sxml_allocator creating_allocator;
    const sxml_xslt_stylesheet_outputs *outputs;
    sxml_processor *processor;
    sxml_xslt_result_document_handler handler;
    void *base_output_uri_memory;
    sxml_byte_view base_output_uri;
    bool is_in_temporary_output_state;
    bool prefers_xml_for_xhtml;
    open_result_document *open_documents;
    size_t open_document_count;
    size_t open_document_capacity;
    written_uri *written_uris;
    size_t written_uri_count;
    size_t written_uri_capacity;
    uint64_t next_number;
    /* 26.2.2: the result document whose URI is the base output URI (an
       xsl:result-document without href) is the principal result: kept
       with its raw result (retained) and delivered as the principal
       result, not to the handler. */
    bool has_principal_document;
    open_result_document principal_document;
    sxml_sequence principal_raw_result;
};

/* Makes room for one more element of a heap array. */
typedef struct array_growth {
    sxml_allocator allocator;
    void **elements;
    size_t *capacity;
    size_t count;
    size_t element_size;
    size_t alignment;
} array_growth;

static sxml_status grow_array(const array_growth *growth) {
    if (growth->count < *growth->capacity) {
        return SXML_STATUS_OK;
    }
    const size_t capacity = *growth->capacity == 0 ? 4 : *growth->capacity * 2;
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(capacity, growth->element_size, &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            growth->allocator,
            (sxml_allocation_request){.byte_count = byte_count, .alignment = growth->alignment},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (growth->count > 0) {
        memcpy(memory, *growth->elements, growth->count * growth->element_size);
    }
    sxml_allocator_release(growth->allocator, *growth->elements);
    *growth->elements = memory;
    *growth->capacity = capacity;
    return SXML_STATUS_OK;
}

static sxml_status copy_bytes(sxml_allocator allocator, sxml_byte_view bytes, written_uri *copy) {
    copy->memory = NULL;
    copy->uri = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    if (bytes.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_status status = sxml_allocator_allocate(
        allocator, (sxml_allocation_request){.byte_count = bytes.byte_count, .alignment = 1},
        &copy->memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memcpy(copy->memory, bytes.bytes, bytes.byte_count);
    copy->uri = (sxml_byte_view){.bytes = copy->memory, .byte_count = bytes.byte_count};
    return SXML_STATUS_OK;
}

static void release_open_document(sxml_xslt_result_writer *writer,
                                  open_result_document *document) {
    sxml_allocator_release(writer->processor->allocator, document->uri_memory);
    sxml_serialization_parameter_reader_destroy(document->reader);
    sxml_arena_destroy(document->arena);
    document->uri_memory = NULL;
    document->reader = NULL;
    document->arena = NULL;
}

/* Drops the open result documents from index first on, innermost first:
   their content failed, and the handler, which sees a result document only
   when its raw result is complete, never saw them. */
static sxml_status abandon_documents(sxml_xslt_result_writer *writer, size_t first) {
    sxml_status status = SXML_STATUS_OK;
    while (writer->open_document_count > first) {
        open_result_document *document = &writer->open_documents[writer->open_document_count - 1];
        release_open_document(writer, document);
        writer->open_document_count -= 1;
    }
    return status;
}

sxml_status sxml_xslt_result_writer_memory_requirements(
    const sxml_xslt_result_writer_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (configuration == NULL || configuration->outputs == NULL ||
        configuration->processor == NULL || requirements == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){.byte_count = sizeof(sxml_xslt_result_writer),
                                               .alignment = alignof(sxml_xslt_result_writer)};
    return SXML_STATUS_OK;
}

sxml_status sxml_xslt_result_writer_initialize(
    sxml_memory_view memory, const sxml_xslt_result_writer_configuration *configuration,
    sxml_xslt_result_writer **writer) {
    sxml_memory_requirements requirements;
    sxml_status status = sxml_xslt_result_writer_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK && writer == NULL) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_view_check_requirements(memory, requirements);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* The memory was checked for the size and alignment. */
    void *block = memory.bytes;
    sxml_xslt_result_writer *made = block;
    memset(made, 0, sizeof(*made));
    made->outputs = configuration->outputs;
    made->processor = configuration->processor;
    made->handler = configuration->result_document_handler;
    made->is_in_temporary_output_state = configuration->is_in_temporary_output_state;
    made->prefers_xml_for_xhtml = configuration->prefers_xml_for_xhtml;
    made->next_number = 1;
    written_uri base = {.memory = NULL, .uri = {.bytes = NULL, .byte_count = 0}};
    status = copy_bytes(configuration->processor->allocator, configuration->base_output_uri, &base);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    made->base_output_uri_memory = base.memory;
    made->base_output_uri = base.uri;
    *writer = made;
    return SXML_STATUS_OK;
}

void sxml_xslt_result_writer_finalize(sxml_xslt_result_writer *writer) {
    if (writer == NULL) {
        return;
    }
    (void)abandon_documents(writer, 0);
    if (writer->has_principal_document) {
        release_open_document(writer, &writer->principal_document);
        sxml_sequence_release(writer->principal_raw_result);
        writer->has_principal_document = false;
    }
    const sxml_allocator allocator = writer->processor->allocator;
    for (size_t uri_index = 0; uri_index < writer->written_uri_count; uri_index += 1) {
        sxml_allocator_release(allocator, writer->written_uris[uri_index].memory);
    }
    sxml_allocator_release(allocator, writer->written_uris);
    sxml_allocator_release(allocator, writer->open_documents);
    sxml_allocator_release(allocator, writer->base_output_uri_memory);
    writer->written_uris = NULL;
    writer->open_documents = NULL;
    writer->base_output_uri_memory = NULL;
}

sxml_status sxml_xslt_result_writer_create(
    sxml_allocator allocator, const sxml_xslt_result_writer_configuration *configuration,
    sxml_xslt_result_writer **writer) {
    sxml_memory_requirements requirements;
    sxml_status status = sxml_xslt_result_writer_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK || writer == NULL) {
        return status == SXML_STATUS_OK ? SXML_STATUS_INVALID_ARGUMENT : status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = requirements.byte_count,
                                  .alignment = requirements.alignment},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xslt_result_writer *made = NULL;
    status = sxml_xslt_result_writer_initialize(
        (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count}, configuration,
        &made);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    made->creating_allocator = allocator;
    *writer = made;
    return SXML_STATUS_OK;
}

void sxml_xslt_result_writer_destroy(sxml_xslt_result_writer *writer) {
    if (writer == NULL) {
        return;
    }
    const sxml_allocator allocator = writer->creating_allocator;
    sxml_xslt_result_writer_finalize(writer);
    sxml_allocator_release(allocator, writer);
}

sxml_xslt_current_output_uri sxml_xslt_result_writer_get_current_output_uri(
    const sxml_xslt_result_writer *writer) {
    const sxml_xslt_current_output_uri absent = {.is_present = false,
                                                 .uri = {.bytes = NULL, .byte_count = 0}};
    if (writer == NULL || writer->is_in_temporary_output_state) {
        return absent;
    }
    if (writer->open_document_count > 0) {
        return (sxml_xslt_current_output_uri){
            .is_present = true,
            .uri = writer->open_documents[writer->open_document_count - 1].absolute_uri};
    }
    if (writer->base_output_uri.byte_count == 0) {
        return absent;
    }
    return (sxml_xslt_current_output_uri){.is_present = true, .uri = writer->base_output_uri};
}

const sxml_xslt_stylesheet_outputs *sxml_xslt_result_writer_get_outputs(
    const sxml_xslt_result_writer *writer) {
    return writer == NULL ? NULL : writer->outputs;
}

sxml_xslt_result_writer *sxml_xslt_result_writer_from_call(const sxml_function_call *call) {
    const sxml_evaluation_environment *environment =
        sxml_processor_get_evaluation_environment(call->dynamic_context);
    return environment == NULL ? NULL : environment->result_writer;
}

static bool has_written(const sxml_xslt_result_writer *writer, sxml_byte_view uri) {
    for (size_t uri_index = 0; uri_index < writer->written_uri_count; uri_index += 1) {
        if (sxml_byte_view_equals(writer->written_uris[uri_index].uri, uri)) {
            return true;
        }
    }
    return false;
}

/* A dynamic error with static text: "XTDE1490 description". */
static sxml_status raise_dynamic(sxml_error_record *error_record, sxml_byte_view text) {
    return sxml_error_record_report(
        error_record,
        &(sxml_static_error){.error_status = SXML_STATUS_DYNAMIC_ERROR,
                             .code_local_name = {.bytes = text.bytes, .byte_count = 8},
                             .description = {.bytes = text.bytes + 9,
                                             .byte_count = text.byte_count - 9}});
}

sxml_status sxml_xslt_result_writer_deliver_principal_result(
    sxml_xslt_result_writer *writer, const sxml_xslt_principal_result_request *request,
    sxml_sequence *principal_result) {
    if (writer == NULL || request == NULL || principal_result == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_status status = abandon_documents(writer, 0);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (writer->has_principal_document) {
        /* 26.2.2: the result document of the base output URI is the
           principal result; the raw result must then be empty. */
        sxml_emptiness emptiness;
        status = sxml_sequence_test_emptiness(request->value, request->error_record, &emptiness);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!emptiness.is_empty) {
            return raise_dynamic(request->error_record, SXML_BYTE_VIEW_FROM_LITERAL(
                                 "XTDE1490 a result document has the URI of the principal "
                                 "result, which is not empty (XSLT 4.0, 26.2.2)"));
        }
        return deliver_result(
            writer->processor->item_heap,
            &(delivery_request){.result_delivery = request->result_delivery,
                                .output_stream = request->output_stream,
                                .result = {.value = writer->principal_raw_result,
                                           .output_definition =
                                               &writer->principal_document.definition,
                                           .result_uri = writer->base_output_uri,
                                           .prefers_xml_for_xhtml =
                                               writer->prefers_xml_for_xhtml,
                                           .error_record = request->error_record}},
            principal_result);
    }
    if (has_written(writer, writer->base_output_uri)) {
        /* 26.2.2: an empty principal result is discarded. */
        sxml_emptiness emptiness;
        status = sxml_sequence_test_emptiness(request->value, request->error_record, &emptiness);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!emptiness.is_empty) {
            return raise_dynamic(request->error_record, SXML_BYTE_VIEW_FROM_LITERAL(
                                 "XTDE1490 a result document has the URI of the principal "
                                 "result, which is not empty (XSLT 4.0, 26.2.2)"));
        }
        *principal_result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    sxml_xslt_output_definition unnamed;
    const sxml_xslt_output_definition *definition = request->output_definition;
    if (definition == NULL) {
        status = sxml_xslt_stylesheet_outputs_get_definition(
            writer->outputs, (sxml_name_identifier){.index = 0}, &unnamed);
        definition = &unnamed;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return deliver_result(writer->processor->item_heap,
                          &(delivery_request){.result_delivery = request->result_delivery,
                                              .output_stream = request->output_stream,
                                              .result = {.value = request->value,
                                                         .output_definition = definition,
                                                         .result_uri = writer->base_output_uri,
                                                         .prefers_xml_for_xhtml =
                                                             writer->prefers_xml_for_xhtml,
                                                         .error_record = request->error_record}},
                          principal_result);
}

/* ======================================================== Result documents */

/* The values of a result document instruction, by attribute number. */
typedef struct instruction_values {
    const compiled_instruction *instruction;
    bool has_value[SERIALIZATION_ATTRIBUTE_COUNT];
    sxml_byte_view values[SERIALIZATION_ATTRIBUTE_COUNT];
} instruction_values;

static size_t attribute_number_of(attribute_role role) {
    for (size_t attribute_index = 0; attribute_index < SERIALIZATION_ATTRIBUTE_COUNT;
         attribute_index += 1) {
        if (serialization_attributes[attribute_index].role == role) {
            return attribute_index;
        }
    }
    return 0;
}

/* Reads the instruction's values (one xs:string per value template, in
   its order) from the sequence of the call. */
static sxml_status read_instruction_values(const sxml_function_call *call,
                                           instruction_values *values) {
    const sxml_sequence sequence = sxml_function_library_get_argument(call, 1);
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(sequence);
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t value_index = 0;
         status == SXML_STATUS_OK && value_index < values->instruction->attribute_count;
         value_index += 1) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, call->error_record, &step);
        sxml_atomic_datum datum;
        if (status == SXML_STATUS_OK && !step.has_item) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_get_datum(step.item, &datum);
        }
        if (status == SXML_STATUS_OK) {
            const uint8_t number = values->instruction->attribute_numbers[value_index];
            values->has_value[number] = true;
            values->values[number] = datum.datum_details.string.utf8;
        }
    }
    return status;
}

/* The prefix of a lexical QName among bindings (prefix 0 never matches:
   format names are not in the default namespace, 26.2.4). */
typedef struct prefix_lookup {
    const sxml_name_pool *name_pool;
    sxml_namespace_binding_view bindings;
    sxml_byte_view prefix;
} prefix_lookup;

static bool find_prefix(prefix_lookup lookup, sxml_namespace_uri_identifier *namespace_uri) {
    if (sxml_byte_view_equals(lookup.prefix, SXML_BYTE_VIEW_FROM_LITERAL("xml"))) {
        *namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_XML);
        return true;
    }
    for (size_t binding_index = 0; binding_index < lookup.bindings.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding binding = lookup.bindings.namespace_bindings[binding_index];
        sxml_byte_view prefix = {.bytes = NULL, .byte_count = 0};
        if (binding.namespace_prefix.index != 0 &&
            sxml_name_pool_get_namespace_prefix(lookup.name_pool, binding.namespace_prefix,
                                                &prefix) == SXML_STATUS_OK &&
            sxml_byte_view_equals(prefix, lookup.prefix)) {
            *namespace_uri = binding.namespace_uri;
            return true;
        }
    }
    return false;
}

/* The output definition the format value names; NULL when it names none
   or is no EQName (XTDE1460). */
typedef struct format_lookup {
    const sxml_xslt_result_writer *writer;
    const compiled_instruction *instruction;
    sxml_byte_view text;
} format_lookup;

static const compiled_definition *find_format(format_lookup lookup) {
    const sxml_name_pool *name_pool = lookup.writer->processor->name_pool;
    const sxml_byte_view text = sxml_xml_text_trim_whitespace(lookup.text);
    sxml_namespace_uri_identifier namespace_uri = {.index = SXML_NAMESPACE_NONE};
    sxml_byte_view local_name = text;
    if (text.byte_count > 2 && text.bytes[0] == 'Q' && text.bytes[1] == '{') {
        size_t close = 2;
        while (close < text.byte_count && text.bytes[close] != '}') {
            close += 1;
        }
        if (close == text.byte_count ||
            !sxml_name_pool_find_namespace_uri(
                name_pool, (sxml_byte_view){.bytes = text.bytes + 2, .byte_count = close - 2},
                &namespace_uri)) {
            return NULL;
        }
        local_name = (sxml_byte_view){.bytes = text.bytes + close + 1,
                                      .byte_count = text.byte_count - close - 1};
    } else {
        size_t colon = 0;
        while (colon < text.byte_count && text.bytes[colon] != ':') {
            colon += 1;
        }
        if (colon < text.byte_count) {
            if (!find_prefix((prefix_lookup){.name_pool = name_pool,
                                             .bindings = lookup.instruction->namespace_bindings,
                                             .prefix = {.bytes = text.bytes, .byte_count = colon}},
                             &namespace_uri)) {
                return NULL;
            }
            local_name = (sxml_byte_view){.bytes = text.bytes + colon + 1,
                                          .byte_count = text.byte_count - colon - 1};
        }
    }
    sxml_name_identifier name = {.index = 0};
    if (local_name.byte_count == 0 ||
        !sxml_name_pool_find_name(
            name_pool, &(sxml_name_request){.namespace_uri = namespace_uri, .local_name = local_name},
            &name)) {
        return NULL;
    }
    return find_definition(lookup.writer->outputs, name);
}

/* The texts of an effective definition while it is made. */
typedef struct text_list {
    sxml_serialization_parameter_text *texts;
    size_t text_count;
} text_list;

static size_t find_text(const text_list *list, sxml_byte_view name) {
    for (size_t text_index = 0; text_index < list->text_count; text_index += 1) {
        if (sxml_byte_view_equals(list->texts[text_index].parameter_name, name)) {
            return text_index;
        }
    }
    return SIZE_MAX;
}

static void remove_text(text_list *list, sxml_byte_view name) {
    const size_t found = find_text(list, name);
    if (found == SIZE_MAX) {
        return;
    }
    for (size_t text_index = found; text_index + 1 < list->text_count; text_index += 1) {
        list->texts[text_index] = list->texts[text_index + 1];
    }
    list->text_count -= 1;
}

static void set_text(text_list *list, sxml_serialization_parameter_text text) {
    const size_t found = find_text(list, text.parameter_name);
    if (found == SIZE_MAX) {
        list->texts[list->text_count] = text;
        list->text_count += 1;
    } else {
        list->texts[found] = text;
    }
}

/* What makes one result document's effective definition (26.2.4). */
typedef struct effective_definition_request {
    const sxml_function_call *call;
    const instruction_values *values;
    const compiled_definition *base;
} effective_definition_request;

/* Merges a names value into the definition's (the union, 26.2.4). */
typedef struct names_merge {
    sxml_arena *arena;
    text_list *list;
    sxml_serialization_parameter_text text;
} names_merge;

static sxml_status merge_names(sxml_allocator allocator, const names_merge *merge) {
    const size_t found = find_text(merge->list, merge->text.parameter_name);
    if (found == SIZE_MAX) {
        set_text(merge->list, merge->text);
        return SXML_STATUS_OK;
    }
    text_builder builder = {.allocator = allocator, .bytes = NULL, .byte_count = 0, .byte_capacity = 0};
    sxml_status status = append_bytes(&builder, merge->list->texts[found].parameter_value);
    if (status == SXML_STATUS_OK) {
        status = append_bytes(&builder, SXML_BYTE_VIEW_FROM_LITERAL(" "));
    }
    if (status == SXML_STATUS_OK) {
        status = append_bytes(&builder, merge->text.parameter_value);
    }
    sxml_byte_view kept = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = keep_bytes(merge->arena, text_of(&builder), &kept);
    }
    release_text(&builder);
    if (status == SXML_STATUS_OK) {
        merge->list->texts[found].parameter_value = kept;
    }
    return status;
}

static sxml_status make_effective_definition(sxml_xslt_result_writer *writer,
                                             const effective_definition_request *request,
                                             open_result_document *document) {
    const sxml_function_call *call = request->call;
    const compiled_instruction *instruction = request->values->instruction;
    const compiled_definition *base = request->base;
    sxml_allocator allocator = writer->processor->allocator;
    sxml_status status = sxml_arena_create(
        allocator,
        &(sxml_arena_configuration){.block_allocator = allocator,
                                    .block_byte_count = 1024,
                                    .maximum_block_byte_count = 0,
                                    .initial_memory = {.bytes = NULL, .byte_count = 0}},
        &document->arena);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_array(document->arena,
                                (array_request){.element_count =
                                                    base->text_count + SERIALIZATION_ATTRIBUTE_COUNT,
                                                .element_size =
                                                    sizeof(sxml_serialization_parameter_text),
                                                .alignment =
                                                    alignof(sxml_serialization_parameter_text)},
                                &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    text_list list = {.texts = memory, .text_count = base->text_count};
    if (base->text_count > 0 && list.texts != NULL) {
        memcpy(list.texts, base->texts, base->text_count * sizeof(*list.texts));
    }
    sxml_serialization_flag build_tree = base->definition.build_tree;
    bool has_method = base->definition.has_serialization_method;
    sxml_parameter_document loaded = {.is_loaded = false};
    for (size_t attribute_index = 0;
         status == SXML_STATUS_OK && attribute_index < SERIALIZATION_ATTRIBUTE_COUNT;
         attribute_index += 1) {
        if (!request->values->has_value[attribute_index]) {
            continue;
        }
        const serialization_attribute *attribute = &serialization_attributes[attribute_index];
        const sxml_byte_view value = request->values->values[attribute_index];
        const sxml_serialization_parameter_text text = {.parameter_name = attribute->parameter_name,
                                                        .parameter_value = value};
        switch (attribute->role) {
        case SXML_ATTRIBUTE_ROLE_FORMAT:
        case SXML_ATTRIBUTE_ROLE_HREF:
            break;
        case SXML_ATTRIBUTE_ROLE_BUILD_TREE: {
            const boolean_text reading = read_boolean_text(value);
            if (!reading.is_valid) {
                status = raise_dynamic(call->error_record, SXML_BYTE_VIEW_FROM_LITERAL(
                                       "XTDE0030 the build-tree attribute of xsl:result-document "
                                       "is no boolean (XSLT 4.0, 26.2)"));
            }
            build_tree = reading.is_true ? SXML_SERIALIZATION_FLAG_YES : SXML_SERIALIZATION_FLAG_NO;
            break;
        }
        case SXML_ATTRIBUTE_ROLE_PARAMETER_DOCUMENT: {
            sxml_processor_document_loader *loader =
                sxml_processor_document_loader_from_dynamic_context(call->dynamic_context);
            if (loader != NULL) {
                status = sxml_parameter_document_load(
                    &(sxml_parameter_document_request){
                        .loader = sxml_processor_document_loader_as_document_loader(loader),
                        .uri_reference = value,
                        .base_uri = instruction->static_base_uri},
                    &loaded);
            }
            break;
        }
        case SXML_ATTRIBUTE_ROLE_NAMES:
            status = merge_names(allocator, &(names_merge){.arena = document->arena,
                                                           .list = &list,
                                                           .text = text});
            break;
        case SXML_ATTRIBUTE_ROLE_ABSENT_WHEN_EMPTY:
            if (value.byte_count == 0) {
                remove_text(&list, attribute->parameter_name);
            } else {
                set_text(&list, text);
            }
            break;
        case SXML_ATTRIBUTE_ROLE_ITEM_SEPARATOR:
            if (sxml_byte_view_equals(value, SXML_BYTE_VIEW_FROM_LITERAL("#absent"))) {
                remove_text(&list, attribute->parameter_name);
            } else {
                set_text(&list, text);
            }
            break;
        case SXML_ATTRIBUTE_ROLE_PARAMETER:
            has_method = has_method || sxml_byte_view_equals(attribute->parameter_name,
                                                             SXML_BYTE_VIEW_FROM_LITERAL("method"));
            set_text(&list, text);
            break;
        }
    }
    document_names names = {.names = NULL, .name_count = 0};
    if (status == SXML_STATUS_OK && loaded.has_document_element) {
        /* 26.1.5: the instruction's document wins over everything. */
        status = collect_document_names(
            &(document_names_request){.arena = document->arena,
                                      .name_pool = writer->processor->name_pool,
                                      .element = loaded.document_element},
            &names);
        for (size_t name_index = 0; name_index < names.name_count; name_index += 1) {
            remove_text(&list, names.names[name_index]);
        }
        has_method = has_method || names_contain(names.names, names.name_count,
                                                 SXML_BYTE_VIEW_FROM_LITERAL("method"));
    }
    if (status == SXML_STATUS_OK) {
        status = make_reader(writer->processor, &document->reader);
    }
    sxml_serialization_parameters parameters;
    memset(&parameters, 0, sizeof(parameters));
    if (status == SXML_STATUS_OK) {
        const bool has_document = loaded.has_document_element || base->has_parameter_document;
        status = sxml_serialization_parameter_reader_read_texts(
            document->reader,
            &(sxml_serialization_parameter_text_request){
                .has_parameter_document = has_document,
                .parameter_document =
                    loaded.has_document_element ? loaded.document_element : base->parameter_element,
                .parameter_texts = {.parameter_texts = list.texts,
                                    .parameter_text_count = list.text_count},
                .namespace_bindings = instruction->namespace_bindings,
                .error_record = call->error_record,
                .failed_text_index = NULL},
            &parameters);
    }
    if (status == SXML_STATUS_OK) {
        const sxml_character_map_view parts[3] = {
            base->mappings, instruction->mappings,
            loaded.has_document_element
                ? parameters.use_character_maps
                : (sxml_character_map_view){.character_mappings = NULL,
                                            .character_mapping_count = 0}};
        status = merge_mappings(&(mapping_merge){.scratch_allocator = allocator,
                                                 .arena = document->arena,
                                                 .parts = parts,
                                                 .part_count = 3},
                                &parameters.use_character_maps);
    }
    sxml_parameter_document_release(&loaded);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (parameters.omit_xml_declaration == SXML_SERIALIZATION_FLAG_DEFAULT) {
        parameters.omit_xml_declaration = SXML_SERIALIZATION_FLAG_NO;
    }
    document->definition = (sxml_xslt_output_definition){
        .serialization_parameters = parameters,
        .build_tree = build_tree,
        .has_serialization_method = has_method};
    return SXML_STATUS_OK;
}

static sxml_status read_integer(const sxml_function_call *call, size_t argument_index,
                                int64_t *integer) {
    sxml_optional_item item = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(call, argument_index, &item);
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK && !item.has_item) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(item.item, &datum);
    }
    if (status == SXML_STATUS_OK) {
        *integer = datum.datum_details.integer;
    }
    return status;
}

/* Opens one result document (see src/xslt_output_internal.h). */
typedef struct opening {
    const sxml_function_call *call;
    const compiled_instruction *instruction;
    bool is_temporary;
} opening;

static sxml_status open_result_document_of(sxml_xslt_result_writer *writer, const opening *request,
                                           uint64_t *number) {
    const sxml_function_call *call = request->call;
    if (writer->is_in_temporary_output_state || request->is_temporary) {
        return raise_dynamic(call->error_record, SXML_BYTE_VIEW_FROM_LITERAL(
                             "XTDE1480 xsl:result-document is evaluated in temporary output "
                             "state (XSLT 4.0, 26.2.6)"));
    }
    instruction_values values;
    memset(&values, 0, sizeof(values));
    values.instruction = request->instruction;
    sxml_status status = read_instruction_values(call, &values);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const size_t format_number = attribute_number_of(SXML_ATTRIBUTE_ROLE_FORMAT);
    const size_t href_number = attribute_number_of(SXML_ATTRIBUTE_ROLE_HREF);
    const compiled_definition *base = &writer->outputs->definitions[0];
    if (values.has_value[format_number]) {
        base = find_format((format_lookup){.writer = writer,
                                           .instruction = request->instruction,
                                           .text = values.values[format_number]});
        if (base == NULL) {
            return raise_dynamic(call->error_record, SXML_BYTE_VIEW_FROM_LITERAL(
                                 "XTDE1460 the format of xsl:result-document names no output "
                                 "definition (XSLT 4.0, 26.2.4)"));
        }
    }
    const sxml_byte_view href = values.has_value[href_number]
                                    ? values.values[href_number]
                                    : (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    /* 26.2.2: href resolved against the base output URI. */
    sxml_uri_resolution resolution = {.uri = {.bytes = NULL, .byte_count = 0}, .memory = NULL};
    status = sxml_uri_resolve(&(sxml_uri_resolution_request){.base_uri = writer->base_output_uri,
                                                             .reference = sxml_xml_text_trim_whitespace(href),
                                                             .allocator = writer->processor->allocator},
                              &resolution);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    written_uri copy = {.memory = NULL, .uri = {.bytes = NULL, .byte_count = 0}};
    status = copy_bytes(writer->processor->allocator, resolution.uri, &copy);
    sxml_allocator_release(writer->processor->allocator, resolution.memory);
    if (status == SXML_STATUS_OK && has_written(writer, copy.uri)) {
        status = raise_dynamic(call->error_record, SXML_BYTE_VIEW_FROM_LITERAL(
                               "XTDE1490 two result documents of the transformation have the "
                               "same URI (XSLT 4.0, 26.2.2)"));
    }
    if (status == SXML_STATUS_OK) {
        status = grow_array(&(array_growth){.allocator = writer->processor->allocator,
                                            .elements = (void **)&writer->written_uris,
                                            .capacity = &writer->written_uri_capacity,
                                            .count = writer->written_uri_count,
                                            .element_size = sizeof(written_uri),
                                            .alignment = alignof(written_uri)});
    }
    written_uri kept = {.memory = NULL, .uri = {.bytes = NULL, .byte_count = 0}};
    if (status == SXML_STATUS_OK) {
        status = copy_bytes(writer->processor->allocator, copy.uri, &kept);
    }
    if (status == SXML_STATUS_OK) {
        writer->written_uris[writer->written_uri_count] = kept;
        writer->written_uri_count += 1;
        status = grow_array(&(array_growth){.allocator = writer->processor->allocator,
                                            .elements = (void **)&writer->open_documents,
                                            .capacity = &writer->open_document_capacity,
                                            .count = writer->open_document_count,
                                            .element_size = sizeof(open_result_document),
                                            .alignment = alignof(open_result_document)});
    }
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(writer->processor->allocator, copy.memory);
        return status;
    }
    open_result_document document;
    memset(&document, 0, sizeof(document));
    document.number = writer->next_number;
    document.uri_memory = copy.memory;
    document.absolute_uri = copy.uri;
    status = make_effective_definition(
        writer, &(effective_definition_request){.call = call, .values = &values, .base = base},
        &document);
    if (status == SXML_STATUS_OK) {
        status = keep_bytes(document.arena, href, &document.href);
    }
    if (status != SXML_STATUS_OK) {
        release_open_document(writer, &document);
        return status;
    }
    writer->next_number += 1;
    writer->open_documents[writer->open_document_count] = document;
    writer->open_document_count += 1;
    *number = document.number;
    return SXML_STATUS_OK;
}

/* result-document-open($instruction as item()*, $values as item()*,
   $temporary as item()*) as item()*: the number of the opened result
   document (an xs:integer; 0 outside a transformation). */
static sxml_status call_result_document_open(const sxml_function_call *call,
                                             sxml_sequence *result) {
    *result = sxml_sequence_from_item(sxml_atomic_item_from_integer(0));
    sxml_xslt_result_writer *writer = sxml_xslt_result_writer_from_call(call);
    int64_t node_index = 0;
    sxml_optional_item temporary = {.has_item = false};
    sxml_status status = read_integer(call, 0, &node_index);
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_optional_item(call, 2, &temporary);
    }
    sxml_atomic_datum temporary_datum = {0};
    if (status == SXML_STATUS_OK && temporary.has_item) {
        status = sxml_atomic_item_get_datum(temporary.item, &temporary_datum);
    }
    if (status != SXML_STATUS_OK || writer == NULL) {
        return status;
    }
    const compiled_instruction *instruction =
        node_index > 0 && node_index <= UINT32_MAX
            ? find_instruction(writer->outputs, (uint32_t)node_index)
            : NULL;
    if (instruction == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    uint64_t number = 0;
    status = open_result_document_of(
        writer,
        &(opening){.call = call,
                   .instruction = instruction,
                   .is_temporary = temporary.has_item && temporary_datum.datum_details.boolean},
        &number);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(sxml_atomic_item_from_integer((int64_t)number));
    }
    return status;
}

/* Delivers the raw result of open document document_index and closes it. */
typedef struct closing {
    const sxml_function_call *call;
    size_t document_index;
    sxml_sequence raw_result;
} closing;

static sxml_status close_result_document(sxml_xslt_result_writer *writer, const closing *request) {
    open_result_document *document = &writer->open_documents[request->document_index];
    sxml_status status = SXML_STATUS_OK;
    if (!writer->has_principal_document &&
        sxml_byte_view_equals(document->absolute_uri, writer->base_output_uri)) {
        /* 26.2.2: the principal result, delivered after the evaluation
           (sxml_xslt_result_writer_deliver_principal_result). */
        sxml_sequence_retain(request->raw_result);
        writer->principal_raw_result = request->raw_result;
        writer->principal_document = *document;
        writer->has_principal_document = true;
        writer->open_document_count -= 1;
        return SXML_STATUS_OK;
    }
    if (writer->handler.operations != NULL) {
        /* The handler sees each result document when its raw result is
           complete, so opens and closes alternate (D112). */
        status = writer->handler.operations->open(
            writer->handler.state,
            &(sxml_xslt_result_document_request){.href = document->href,
                                                 .absolute_uri = document->absolute_uri,
                                                 .output_definition = &document->definition,
                                                 .processor = writer->processor},
            &document->target);
    }
    if (status == SXML_STATUS_OK && writer->handler.operations != NULL) {
        sxml_sequence delivered = sxml_sequence_empty();
        status = deliver_result(
            writer->processor->item_heap,
            &(delivery_request){.result_delivery = document->target.result_delivery,
                                .output_stream = document->target.output_stream,
                                .result = {.value = request->raw_result,
                                           .output_definition = &document->definition,
                                           .result_uri = document->absolute_uri,
                                           .prefers_xml_for_xhtml = false,
                                           .error_record = request->call->error_record}},
            &delivered);
        const sxml_status closed = writer->handler.operations->close(
            writer->handler.state,
            &(sxml_xslt_result_document){.absolute_uri = document->absolute_uri,
                                         .value = delivered,
                                         .is_abandoned = status != SXML_STATUS_OK});
        sxml_sequence_release(delivered);
        status = status == SXML_STATUS_OK ? closed : status;
    }
    release_open_document(writer, document);
    writer->open_document_count -= 1;
    return status;
}

/* result-document-close($number as item()*, $raw as item()*) as
   empty-sequence(). */
static sxml_status call_result_document_close(const sxml_function_call *call,
                                              sxml_sequence *result) {
    *result = sxml_sequence_empty();
    sxml_xslt_result_writer *writer = sxml_xslt_result_writer_from_call(call);
    int64_t number = 0;
    const sxml_status status = read_integer(call, 0, &number);
    if (status != SXML_STATUS_OK || writer == NULL || number <= 0) {
        return status;
    }
    size_t document_index = writer->open_document_count;
    while (document_index > 0 &&
           writer->open_documents[document_index - 1].number != (uint64_t)number) {
        document_index -= 1;
    }
    if (document_index == 0) {
        return SXML_STATUS_OK;
    }
    /* Documents opened after it whose content failed are abandoned. */
    const sxml_status abandoned = abandon_documents(writer, document_index);
    if (abandoned != SXML_STATUS_OK) {
        return abandoned;
    }
    return close_result_document(
        writer, &(closing){.call = call,
                           .document_index = document_index - 1,
                           .raw_result = sxml_function_library_get_argument(call, 1)});
}

static const sxml_function_parameter open_parameters[] = {
    SXML_REQUIRED_PARAMETER("instruction", "item()*"),
    SXML_REQUIRED_PARAMETER("values", "item()*"),
    SXML_REQUIRED_PARAMETER("temporary", "item()*"),
};

static const sxml_function_parameter close_parameters[] = {
    SXML_REQUIRED_PARAMETER("number", "item()*"),
    SXML_REQUIRED_PARAMETER("raw", "item()*"),
};

/* Both read and change the transformation: no pass may evaluate or drop
   them. */
static const sxml_native_function result_document_open_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    .properties = (uint32_t)SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT |
                  (uint32_t)SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("result-document-open"),
    .parameters = {open_parameters, 3},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("item()*"),
    .call = call_result_document_open,
    .resumable_body = NULL};

static const sxml_native_function result_document_close_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    .properties = (uint32_t)SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT |
                  (uint32_t)SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("result-document-close"),
    .parameters = {close_parameters, 2},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("empty-sequence()"),
    .call = call_result_document_close,
    .resumable_body = NULL};

/* ================================================================ Lowering */

/* 26.2.6: instructions whose sequence constructors are evaluated in
   temporary output state. */
static bool is_temporary_container(sxml_xslt_ast_kind kind) {
    return kind == SXML_XSLT_AST_KIND_VARIABLE || kind == SXML_XSLT_AST_KIND_PARAM ||
           kind == SXML_XSLT_AST_KIND_WITH_PARAM || kind == SXML_XSLT_AST_KIND_FUNCTION ||
           kind == SXML_XSLT_AST_KIND_KEY || kind == SXML_XSLT_AST_KIND_SORT ||
           kind == SXML_XSLT_AST_KIND_ACCUMULATOR_RULE || kind == SXML_XSLT_AST_KIND_MERGE_KEY;
}

bool sxml_xslt_lowering_is_in_temporary_output_state(const sxml_xslt_lowering *lowering,
                                                     sxml_xslt_ast_node_index node) {
    for (sxml_xslt_ast_node_index ancestor = sxml_xslt_ast_get_node(lowering->ast, node)->parent;
         ancestor.value != 0;
         ancestor = sxml_xslt_ast_get_node(lowering->ast, ancestor)->parent) {
        const sxml_xslt_ast_kind kind = sxml_xslt_ast_get_node(lowering->ast, ancestor)->kind;
        if (kind == SXML_XSLT_AST_KIND_TEMPLATE) {
            return false;
        }
        if (is_temporary_container(kind)) {
            return true;
        }
    }
    return false;
}

/* The xs:QName of the tunnel parameter that marks temporary output
   state, as a constant. */
static sxml_status add_state_name(sxml_xpath_lowerer *lowerer, sxml_ir_atom *atom) {
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    sxml_status status = sxml_name_pool_intern_namespace_uri(
        lowerer->name_pool, SXML_BYTE_VIEW_FROM_LITERAL(SXML_XSLT_OUTPUT_STATE_NAMESPACE),
        &namespace_uri);
    sxml_name_identifier name = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            lowerer->name_pool,
            &(sxml_name_request){
                .namespace_uri = namespace_uri,
                .local_name = SXML_BYTE_VIEW_FROM_LITERAL(SXML_XSLT_OUTPUT_STATE_LOCAL_NAME)},
            &name);
    }
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_make(
            lowerer->item_heap,
            &(sxml_atomic_item_request){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
                .datum = {.datum_kind = SXML_DATUM_KIND_QNAME,
                          .datum_details = {.qname = {.expanded_name = name,
                                                      .namespace_prefix = {.index = 0}}}}},
            &item);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowerer_add_item_constant(lowerer, item, atom);
}

sxml_status sxml_xslt_lowering_mark_temporary_output_state(
    sxml_xpath_lowerer *lowerer, const sxml_xslt_output_state_mark *mark,
    sxml_ir_atom *tunnel_parameters) {
    if (!sxml_xslt_lowering_is_in_temporary_output_state(mark->lowering, mark->node)) {
        return SXML_STATUS_OK;
    }
    sxml_ir_atom name = {0};
    sxml_ir_atom marker = {0};
    sxml_status status = add_state_name(lowerer, &name);
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_item_constant(lowerer, sxml_atomic_item_true(), &marker);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_ir_atom arguments[3] = {*tunnel_parameters, name, marker};
    return sxml_xslt_lowering_bind_library_call(
        lowerer,
        &(sxml_xslt_library_call){.block = mark->block,
                                  .function_namespace = SXML_NAMESPACE_MAP_FUNCTIONS,
                                  .local_name = SXML_BYTE_VIEW_FROM_LITERAL("put"),
                                  .arguments = {.atoms = arguments, .atom_count = 3}},
        tunnel_parameters);
}

/* xsl:result-document (26.2): see src/xslt_output_internal.h. */
typedef struct result_document_locals {
    sxml_xslt_instruction_locals head;
    /* The next entry of serialization_attributes to look at. */
    uint32_t next_attribute;
    uint32_t value_count;
    sxml_ir_atom values[SERIALIZATION_ATTRIBUTE_COUNT];
    sxml_ir_atom opened;
    sxml_ir_atom raw_result;
} result_document_locals;

static const sxml_ast_node_index no_ast_node = {.value = 0};

/* A call of a native row, bound in block. */
typedef struct row_call {
    sxml_xpath_lowering_block *block;
    const sxml_native_function *native_function;
    sxml_ir_atom_view arguments;
} row_call;

static sxml_status bind_row_call(sxml_xpath_lowerer *lowerer, row_call call, sxml_ir_atom *atom) {
    sxml_ir_atom_list arguments = {0};
    const sxml_status status = sxml_ir_unit_add_atoms(lowerer->unit, call.arguments, &arguments);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xslt_lowering_bind_expression(
        lowerer,
        &(sxml_xslt_expression_to_bind){
            .block = call.block,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
                           .static_call = {.target = {.target_kind =
                                                          SXML_IR_FUNCTION_TARGET_KIND_NATIVE,
                                                      .native_function = call.native_function},
                                           .arguments = arguments}}},
        atom);
}

/* Whether the received tunnel parameters mark temporary output state,
   bound in block: false() outside a template function. */
static sxml_status bind_received_state(sxml_xpath_lowering_machine *machine,
                                       sxml_xpath_lowering_frame *frame, sxml_ir_atom *atom) {
    sxml_xpath_lowerer *lowerer = machine->state;
    const result_document_locals *locals = frame->locals;
    sxml_ir_atom tunnel_parameters = {0};
    if (!sxml_xslt_lowering_received_tunnel_parameters(locals->head.lowering,
                                                       &tunnel_parameters)) {
        return sxml_xpath_lowerer_add_item_constant(lowerer, sxml_atomic_item_false(), atom);
    }
    sxml_ir_atom name = {0};
    const sxml_status status = add_state_name(lowerer, &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_ir_atom arguments[2] = {tunnel_parameters, name};
    return sxml_xslt_lowering_bind_library_call(
        lowerer,
        &(sxml_xslt_library_call){.block = frame->block,
                                  .function_namespace = SXML_NAMESPACE_MAP_FUNCTIONS,
                                  .local_name = SXML_BYTE_VIEW_FROM_LITERAL("contains"),
                                  .arguments = {.atoms = arguments, .atom_count = 2}},
        atom);
}

/* Binds result-document-open(N, (values), temporary). */
static sxml_status bind_open(sxml_xpath_lowering_machine *machine,
                             sxml_xpath_lowering_frame *frame) {
    sxml_xpath_lowerer *lowerer = machine->state;
    result_document_locals *locals = frame->locals;
    sxml_ir_atom_list value_list = {0};
    sxml_status status = sxml_ir_unit_add_atoms(
        lowerer->unit,
        (sxml_ir_atom_view){.atoms = locals->values, .atom_count = locals->value_count},
        &value_list);
    sxml_ir_atom values = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_lowering_bind_expression(
            lowerer,
            &(sxml_xslt_expression_to_bind){
                .block = frame->block,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                               .sequence = value_list}},
            &values);
    }
    sxml_ir_atom instruction = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_item_constant(
            lowerer, sxml_atomic_item_from_integer((int64_t)locals->head.node.value), &instruction);
    }
    sxml_ir_atom temporary = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_received_state(machine, frame, &temporary);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_ir_atom arguments[3] = {instruction, values, temporary};
    return bind_row_call(lowerer,
                         (row_call){.block = frame->block,
                                    .native_function = &result_document_open_function,
                                    .arguments = {.atoms = arguments, .atom_count = 3}},
                         &locals->opened);
}

/* Pushes the lowering of the raw result: select or the content. */
static sxml_status start_content(sxml_xpath_lowering_machine *machine,
                                 sxml_xpath_lowering_frame *frame) {
    result_document_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    const sxml_xslt_ast_attribute *select = sxml_xslt_ast_get_attribute(
        lowering->ast,
        sxml_xslt_ast_find_attribute(lowering->ast,
                                     (sxml_xslt_ast_attribute_query){
                                         .node = locals->head.node,
                                         .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT}));
    if (select->value.kind == SXML_XSLT_AST_VALUE_KIND_EXPRESSION) {
        return sxml_xslt_lowering_call_expression(
            machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                  .expression = select->value.payload.expression,
                                                  .block = frame->block,
                                                  .atom = &locals->raw_result,
                                                  .focus = NULL});
    }
    return sxml_xslt_lowering_call_sequence_constructor(
        machine, &(sxml_xslt_sequence_constructor_call){
                     .lowering = lowering,
                     .first = sxml_xslt_lowering_first_constructor_child(lowering->ast,
                                                                         locals->head.node),
                     .block = frame->block,
                     .is_body = false,
                     .destination = {.expression = NULL,
                                     .atom = &locals->raw_result,
                                     .atom_block = frame->block,
                                     .focused = NULL},
                     .focus = NULL});
}

/* The checks before anything is lowered; *raised is set when the
   instruction is a RAISE of XTDE1480. */
/* Whether check_instruction finished the frame. */
typedef struct check_outcome {
    bool is_finished;
} check_outcome;

static sxml_status check_instruction(sxml_xpath_lowering_machine *machine,
                                     sxml_xpath_lowering_frame *frame, check_outcome *outcome) {
    const result_document_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    outcome->is_finished = false;
    const bool has_select =
        sxml_xslt_ast_find_attribute(lowering->ast,
                                     (sxml_xslt_ast_attribute_query){
                                         .node = locals->head.node,
                                         .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT})
            .value != 0;
    if (has_select &&
        sxml_xslt_lowering_first_constructor_child(lowering->ast, locals->head.node).value != 0) {
        return sxml_xslt_lowering_report_static_error(
            lowering,
            (sxml_xslt_static_error){.node = locals->head.node,
                                     .code_and_description = SXML_BYTE_VIEW_FROM_LITERAL(
                                         "XTSE3185 xsl:result-document has both a select "
                                         "attribute and content (XSLT 4.0, 26.2)")});
    }
    if (!sxml_xslt_lowering_is_in_temporary_output_state(lowering, locals->head.node)) {
        return SXML_STATUS_OK;
    }
    sxml_ir_expression_index raised = {0};
    const sxml_status status = sxml_xpath_lowerer_add_raise(
        machine->state,
        (sxml_xpath_lowering_raise_request){
            .node = no_ast_node,
            .error_status = SXML_STATUS_DYNAMIC_ERROR,
            .code_local_name = SXML_BYTE_VIEW_FROM_LITERAL("XTDE1480"),
            .description = SXML_BYTE_VIEW_FROM_LITERAL(
                "xsl:result-document is evaluated in temporary output state (XSLT 4.0, 26.2.6)")},
        &raised);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    outcome->is_finished = true;
    return sxml_xpath_lowering_finish_frame(machine, frame, raised);
}

static sxml_status continue_result_document(sxml_xpath_lowering_machine *machine,
                                            sxml_xpath_lowering_frame *frame) {
    result_document_locals *locals = frame->locals;
    const sxml_xslt_lowering *lowering = locals->head.lowering;
    sxml_status status = SXML_STATUS_OK;
    if (frame->step == 0) {
        check_outcome outcome = {.is_finished = false};
        status = check_instruction(machine, frame, &outcome);
        if (status != SXML_STATUS_OK || outcome.is_finished) {
            return status;
        }
        frame->step = 1;
    }
    if (frame->step == 1) {
        /* One value per value template, in the table's order. */
        while (locals->next_attribute < SERIALIZATION_ATTRIBUTE_COUNT) {
            const sxml_xslt_ast_attribute *attribute = sxml_xslt_ast_get_attribute(
                lowering->ast,
                sxml_xslt_ast_find_attribute(
                    lowering->ast,
                    (sxml_xslt_ast_attribute_query){
                        .node = locals->head.node,
                        .kind = serialization_attributes[locals->next_attribute].instruction_kind}));
            locals->next_attribute += 1;
            if (attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE) {
                continue;
            }
            sxml_ir_atom *atom = &locals->values[locals->value_count];
            locals->value_count += 1;
            return sxml_xslt_lowering_call_value_template(
                machine, &(sxml_xslt_expression_call){.lowering = lowering,
                                                      .expression = attribute->value.payload.expression,
                                                      .block = frame->block,
                                                      .atom = atom,
                                                      .focus = NULL});
        }
        status = bind_open(machine, frame);
        frame->step = 2;
        if (status == SXML_STATUS_OK) {
            status = start_content(machine, frame);
        }
        return status;
    }
    const sxml_ir_atom arguments[2] = {locals->opened, locals->raw_result};
    sxml_ir_atom closed = {0};
    status = bind_row_call(machine->state,
                           (row_call){.block = frame->block,
                                      .native_function = &result_document_close_function,
                                      .arguments = {.atoms = arguments, .atom_count = 2}},
                           &closed);
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lowerer_add_expression(
            machine->state,
            (sxml_xpath_lowering_expression_to_add){
                .node = no_ast_node,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM, .atom = closed}},
            &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xpath_lowering_finish_frame(machine, frame, result);
}

static const sxml_xpath_lowering_procedure result_document_procedure = {
    .procedure_name = SXML_BYTE_VIEW_INITIALIZER("xsl:result-document"),
    .step = continue_result_document,
    .locals_size = sizeof(result_document_locals)};

static const sxml_xslt_instruction_rule output_rules[] = {
    {.kind = SXML_XSLT_AST_KIND_RESULT_DOCUMENT, .procedure = &result_document_procedure},
};

const sxml_xslt_instruction_rule_view sxml_xslt_instruction_rules_output = {
    .rules = output_rules, .rule_count = sizeof(output_rules) / sizeof(output_rules[0])};

/* ============================================== Result document collector */

/* One collected result document. */
typedef struct collected_document {
    void *uri_memory;
    sxml_byte_view absolute_uri;
    /* Owned. */
    sxml_sequence value;
    void *byte_memory;
    sxml_byte_view serialized_bytes;
} collected_document;

struct sxml_xslt_result_document_collector {
    sxml_allocator allocator;
    sxml_allocator creating_allocator;
    size_t maximum_byte_count;
    size_t held_byte_count;
    sxml_xslt_result_delivery result_delivery;
    collected_document *documents;
    size_t document_count;
    size_t document_capacity;
    /* The document between open and close. */
    bool has_pending;
    written_uri pending_uri;
    text_builder pending_bytes;
};

static bool is_valid_delivery(sxml_xslt_result_delivery delivery) {
    switch (delivery) {
    case SXML_XSLT_RESULT_DELIVERY_DEFAULT:
    case SXML_XSLT_RESULT_DELIVERY_RAW:
    case SXML_XSLT_RESULT_DELIVERY_TREE:
    case SXML_XSLT_RESULT_DELIVERY_SERIALIZED:
        return true;
    }
    return false;
}

sxml_status sxml_xslt_result_document_collector_memory_requirements(
    const sxml_xslt_result_document_collector_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (configuration == NULL || requirements == NULL ||
        configuration->allocator.operations == NULL ||
        !is_valid_delivery(configuration->result_delivery)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements =
        (sxml_memory_requirements){.byte_count = sizeof(sxml_xslt_result_document_collector),
                                   .alignment = alignof(sxml_xslt_result_document_collector)};
    return SXML_STATUS_OK;
}

sxml_status sxml_xslt_result_document_collector_initialize(
    sxml_memory_view memory,
    const sxml_xslt_result_document_collector_configuration *configuration,
    sxml_xslt_result_document_collector **collector) {
    sxml_memory_requirements requirements;
    sxml_status status =
        sxml_xslt_result_document_collector_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK && collector == NULL) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_view_check_requirements(memory, requirements);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* The memory was checked for the size and alignment. */
    void *block = memory.bytes;
    sxml_xslt_result_document_collector *made = block;
    memset(made, 0, sizeof(*made));
    made->allocator = configuration->allocator;
    made->maximum_byte_count = configuration->maximum_byte_count;
    made->result_delivery = configuration->result_delivery;
    made->pending_bytes = (text_builder){
        .allocator = configuration->allocator, .bytes = NULL, .byte_count = 0, .byte_capacity = 0};
    *collector = made;
    return SXML_STATUS_OK;
}

static void drop_pending(sxml_xslt_result_document_collector *collector) {
    sxml_allocator_release(collector->allocator, collector->pending_uri.memory);
    collector->pending_uri =
        (written_uri){.memory = NULL, .uri = {.bytes = NULL, .byte_count = 0}};
    release_text(&collector->pending_bytes);
    collector->has_pending = false;
}

void sxml_xslt_result_document_collector_clear(sxml_xslt_result_document_collector *collector) {
    if (collector == NULL) {
        return;
    }
    for (size_t document_index = 0; document_index < collector->document_count;
         document_index += 1) {
        collected_document *document = &collector->documents[document_index];
        sxml_allocator_release(collector->allocator, document->uri_memory);
        sxml_allocator_release(collector->allocator, document->byte_memory);
        sxml_sequence_release(document->value);
    }
    collector->document_count = 0;
    collector->held_byte_count = 0;
    drop_pending(collector);
}

void sxml_xslt_result_document_collector_finalize(sxml_xslt_result_document_collector *collector) {
    if (collector == NULL) {
        return;
    }
    sxml_xslt_result_document_collector_clear(collector);
    sxml_allocator_release(collector->allocator, collector->documents);
    collector->documents = NULL;
    collector->document_capacity = 0;
}

sxml_status sxml_xslt_result_document_collector_create(
    sxml_allocator allocator,
    const sxml_xslt_result_document_collector_configuration *configuration,
    sxml_xslt_result_document_collector **collector) {
    sxml_memory_requirements requirements;
    sxml_status status =
        sxml_xslt_result_document_collector_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK || collector == NULL) {
        return status == SXML_STATUS_OK ? SXML_STATUS_INVALID_ARGUMENT : status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = requirements.byte_count,
                                  .alignment = requirements.alignment},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xslt_result_document_collector *made = NULL;
    status = sxml_xslt_result_document_collector_initialize(
        (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count}, configuration,
        &made);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    made->creating_allocator = allocator;
    *collector = made;
    return SXML_STATUS_OK;
}

void sxml_xslt_result_document_collector_destroy(sxml_xslt_result_document_collector *collector) {
    if (collector == NULL) {
        return;
    }
    const sxml_allocator allocator = collector->creating_allocator;
    sxml_xslt_result_document_collector_finalize(collector);
    sxml_allocator_release(allocator, collector);
}

static sxml_status write_collected_bytes(void *state, sxml_byte_view bytes) {
    sxml_xslt_result_document_collector *collector = state;
    size_t held = 0;
    sxml_status status = sxml_size_add(collector->held_byte_count, bytes.byte_count, &held);
    if (status == SXML_STATUS_OK && collector->maximum_byte_count != 0 &&
        held > collector->maximum_byte_count) {
        status = SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (status == SXML_STATUS_OK) {
        status = append_bytes(&collector->pending_bytes, bytes);
    }
    if (status == SXML_STATUS_OK) {
        collector->held_byte_count = held;
    }
    return status;
}

static const sxml_output_stream_operations collected_byte_operations = {
    .write = write_collected_bytes};

static sxml_status open_collected_document(void *state,
                                           const sxml_xslt_result_document_request *request,
                                           sxml_xslt_result_document_target *target) {
    sxml_xslt_result_document_collector *collector = state;
    drop_pending(collector);
    const sxml_status status =
        copy_bytes(collector->allocator, request->absolute_uri, &collector->pending_uri);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    collector->has_pending = true;
    target->result_delivery = collector->result_delivery;
    if (collector->result_delivery == SXML_XSLT_RESULT_DELIVERY_SERIALIZED) {
        target->output_stream = (sxml_output_stream){.state = collector,
                                                     .operations = &collected_byte_operations};
    }
    return SXML_STATUS_OK;
}

static sxml_status close_collected_document(void *state,
                                            const sxml_xslt_result_document *result_document) {
    sxml_xslt_result_document_collector *collector = state;
    if (result_document->is_abandoned || !collector->has_pending) {
        if (collector->has_pending) {
            collector->held_byte_count -= collector->pending_bytes.byte_count;
        }
        drop_pending(collector);
        return SXML_STATUS_OK;
    }
    const sxml_status status = grow_array(&(array_growth){.allocator = collector->allocator,
                                                          .elements = (void **)&collector->documents,
                                                          .capacity = &collector->document_capacity,
                                                          .count = collector->document_count,
                                                          .element_size = sizeof(collected_document),
                                                          .alignment = alignof(collected_document)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence_retain(result_document->value);
    collector->documents[collector->document_count] = (collected_document){
        .uri_memory = collector->pending_uri.memory,
        .absolute_uri = collector->pending_uri.uri,
        .value = result_document->value,
        .byte_memory = collector->pending_bytes.bytes,
        .serialized_bytes = text_of(&collector->pending_bytes)};
    collector->document_count += 1;
    collector->pending_uri = (written_uri){.memory = NULL, .uri = {.bytes = NULL, .byte_count = 0}};
    collector->pending_bytes = (text_builder){
        .allocator = collector->allocator, .bytes = NULL, .byte_count = 0, .byte_capacity = 0};
    collector->has_pending = false;
    return SXML_STATUS_OK;
}

static const sxml_xslt_result_document_handler_operations collector_operations = {
    .open = open_collected_document, .close = close_collected_document};

sxml_xslt_result_document_handler sxml_xslt_result_document_collector_as_handler(
    sxml_xslt_result_document_collector *collector) {
    return (sxml_xslt_result_document_handler){.state = collector,
                                               .operations = &collector_operations};
}

size_t sxml_xslt_result_document_collector_get_result_document_count(
    const sxml_xslt_result_document_collector *collector) {
    return collector == NULL ? 0 : collector->document_count;
}

sxml_status sxml_xslt_result_document_collector_get_result_document(
    const sxml_xslt_result_document_collector *collector, size_t result_document_index,
    sxml_xslt_collected_result_document *result_document) {
    if (collector == NULL || result_document == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (result_document_index >= collector->document_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    const collected_document *document = &collector->documents[result_document_index];
    *result_document = (sxml_xslt_collected_result_document){
        .absolute_uri = document->absolute_uri,
        .value = document->value,
        .serialized_bytes = document->serialized_bytes};
    return SXML_STATUS_OK;
}
