/*
 * The XML parser: XML 1.0 bytes in, tree builder events out
 * (include/sxml/xml.h; docs/DECISIONS.md D12, D28, D51, D81).
 *
 * Golden file: src/memory.c (lifecycle, layout, naming, outputs untouched
 * on failure); the walk over a concrete syntax tree follows
 * src/xpath_parse.c.
 *
 * A document is decoded to UTF-8 (src/xml_encoding.c) and parsed with the
 * tree-sitter grammar grammars/xml. Any ERROR or MISSING node makes it not
 * well-formed (D12). The concrete syntax tree is then walked twice:
 *
 * 1. the internal subset of the document type declaration, which fills the
 *    entity and attribute-definition tables (src/xml_entity.c);
 * 2. the document's children, which become tree builder events.
 *
 * Both walks move tree-sitter cursors over a stack of entity frames instead
 * of recursing, so deep documents and long chains of entities use no C
 * stack. An entity reference pushes a frame over the entity's replacement
 * text, parsed again with the same grammar wrapped in a dummy element (a
 * general entity in content, XML 1.0 section 4.3.2) or in a dummy document
 * type declaration (a parameter entity between declarations, 2.8): the
 * grammar then checks that the replacement text is well-formed.
 *
 * The constraints the grammar cannot express are checked here, numbered as
 * in grammars/xml/README.md, "What the tree builder checks": encodings (1),
 * line ends (2), Unique Att Spec (3), Legal Character (4), Entity Declared
 * (5), declarations after unread parameter entities (6), Parsed Entity (7),
 * No Recursion (8), well-formed parsed entities (9), No External Entity
 * References (10), No < in Attribute Values (11), attribute-value
 * normalization and defaults (12), first declaration wins (13), namespaces
 * (14), XML 1.1 (15) and the root element (16: guaranteed by the grammar).
 *
 * XML 1.1 (15): the version is read from the XML declaration before
 * parsing. A 1.1 document must not contain restricted characters
 * literally, and its NEL and LINE SEPARATOR line ends are translated
 * into a copy of the text before the grammar sees it (XML 1.1 Second
 * Edition, 2.2 and 2.11), so the grammar stays the 1.0 grammar; its
 * character references may denote the C0 and C1 controls (4.1), and its
 * prefixes may be undeclared (Namespaces in XML 1.1, section 5).
 *
 * The tree-sitter runtime allocates with malloc during the call; D12
 * accepts that for the parse step, and every tree it makes is deleted
 * before the call returns. Everything else comes from the configuration's
 * allocator and is kept for the next document.
 */
#include "sxml/xml.h"

#include "sxml/string.h"
#include "dtd_internal.h"
#include "xdm_internal.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <string.h>
#include <tree_sitter/api.h>

/* Defined by the generated parser of grammars/xml (D12). */
const TSLanguage *
tree_sitter_xml(void); // NOLINT(readability-identifier-naming)

/* The namespace names of Namespaces in XML 1.0, section 3. */
static const sxml_byte_view sxml_xml_namespace_text =
    SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/XML/1998/namespace");
static const sxml_byte_view sxml_xmlns_namespace_text =
    SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/2000/xmlns/");

/* Up to this many attributes, duplicates are found by comparing pairs. */
enum { SXML_XML_ATTRIBUTE_SCAN_LIMIT = 8 };

/* ------------------------------------------------------------ Objects */

/* One attribute of the start tag being processed. */
typedef struct sxml_xml_attribute {
    /* As written: in the frame's text, or in the DTD text for a default. */
    sxml_byte_view qualified_name;
    /* The normalized value: in the parser's attribute values, or in the
       DTD text when is_defaulted. */
    sxml_byte_range value;
    bool is_defaulted;
    sxml_xml_attribute_type attribute_type;
    sxml_source_location location;
    /* Filled by namespace processing. */
    bool is_namespace_declaration;
    sxml_name_identifier attribute_name;
    sxml_namespace_prefix_identifier namespace_prefix;
} sxml_xml_attribute;

/* One namespace binding in scope. URI 0 undeclares the default namespace. */
typedef struct sxml_xml_scoped_binding {
    sxml_namespace_prefix_identifier namespace_prefix;
    sxml_namespace_uri_identifier namespace_uri;
} sxml_xml_scoped_binding;

/* What an entity frame walks. */
typedef enum sxml_xml_frame_kind {
    /* The document itself: line ends are normalized (2.11). */
    SXML_XML_FRAME_KIND_DOCUMENT,
    /* The replacement text of a general entity referenced in content. */
    SXML_XML_FRAME_KIND_GENERAL_ENTITY,
    /* DTD text: the replacement text of a parameter entity, the external
       subset, an expanded declaration or an included conditional section. */
    SXML_XML_FRAME_KIND_PARAMETER_ENTITY,
    /* The text of an external parsed general entity referenced in content
       (4.3.2): its tree's walk root is the entity, not a wrapper. */
    SXML_XML_FRAME_KIND_EXTERNAL_GENERAL_ENTITY,
} sxml_xml_frame_kind;

/*
 * One level of the walk (glossary: entity_frame): a cursor over the
 * concrete syntax tree of the document or of a replacement text. Frames
 * live in an array that may move them: they hold no pointers into
 * themselves.
 */
typedef struct sxml_xml_frame {
    sxml_xml_frame_kind frame_kind;
    /* Owned for entity frames; the document's tree is the parse's. */
    TSTree *syntax_tree;
    TSTreeCursor cursor;
    /* The text the tree was parsed from, after prefix_byte_count bytes of
       wrapper. Parameter-entity frames own a copy (owned_text), because
       declarations they contain grow the DTD text the entity lives in. */
    sxml_byte_view text;
    uint32_t prefix_byte_count;
    /* For frames of the document's own text: where text starts in the
       document's text, which a document segment (D222) is a part of. */
    size_t document_offset;
    /* A content segment of the document (D222), whose items may be the
       Misc after the document element. */
    bool is_content_segment;
    sxml_xml_buffer owned_text;
    bool has_entity;
    uint32_t entity_index;
    /* Where the outermost entity reference is: every event and error in
       an entity frame is reported there. */
    sxml_source_location reference_location;
    /* DTD text read as the external subset is (2.8): parameter-entity
       references may occur inside declarations, conditional sections are
       allowed. Inherited by the frames it pushes. */
    bool is_external_context;
    /* The frame's text is the start of an external entity, where a text
       declaration may stand (4.3.1). */
    bool starts_external_entity;
    /* The base URI of the frame's text, in the DTD text (4.2.2). */
    sxml_byte_range base_uri;
    /* Whether the cursor moves on before the next visit, and whether it
       moves into the children of the current node. */
    bool needs_advance;
    bool descends;
    /* The depth of the cursor's node below the walk root, from 1: what
       ts_tree_cursor_current_depth gives, kept here because that is linear
       in the depth (a deep document would cost quadratic time). */
    uint32_t cursor_depth;
    /* The document's element depth when the frame was pushed: an element
       one deeper starts at the top of the frame's entity. */
    uint32_t element_depth;
} sxml_xml_frame;

/* One step of expanding entities inside an attribute value (4.4.5). */
typedef struct sxml_xml_expansion {
    uint32_t entity_index;
    size_t byte_offset;
    /* Which inclusion of a parameter entity this is, from 1, for the
       nesting constraints of declarations (2.8, 3.2.1). */
    size_t inclusion_number;
} sxml_xml_expansion;

struct sxml_xml_parser {
    sxml_xml_parser_configuration configuration;
    /* The allocator of sxml_xml_parser_create; null otherwise. */
    sxml_allocator owning_allocator;
    sxml_xml_buffer transcoded_text;
    sxml_xml_buffer attribute_values;
    sxml_xml_buffer scratch_text;
    sxml_xml_dtd dtd;
    sxml_xml_array attributes;
    sxml_xml_array namespace_bindings;
    /* For each open element, how many namespace bindings were in scope
       before it. */
    sxml_xml_array element_scopes;
    sxml_xml_array frames;
    sxml_xml_array expansions;
    sxml_xml_array duplicate_slots;
    /* Named children of nodes being looped over, as a stack of lists:
       ts_node_named_child is linear in the index, a cursor is not. */
    sxml_xml_array child_nodes;
    /* DTD validation (D199): the schema documents written from the DTD,
       their ranges and views, and the prefix bindings their names are
       resolved with. */
    sxml_xml_buffer schema_text;
    /* For each open element of a validating parse, what its declaration
       says of its content: sxml_xml_open_content. */
    sxml_xml_array open_contents;
    /* While a declaration is expanded: for each open parenthesis, the
       inclusion it was read in (size_t). */
    sxml_xml_array parenthesis_inclusions;
    sxml_xml_array schema_documents;
    sxml_xml_array schema_views;
    sxml_xml_array prefix_bindings;
    /* The qualified names of the open elements (sxml_byte_view), for the
       start tags written before a content segment (D222). Read only
       between segments, when every open element was started in the
       document's own text, so that the views point into it. */
    sxml_xml_array open_element_names;
    /* The marker and those start tags while a content segment is parsed. */
    sxml_xml_buffer segment_prefix;
};

/* What the declaration of an open element says of its content, for the
   validity constraints on content that the parser checks (D199). */
typedef struct sxml_xml_open_content {
    /* Whether the element type is declared; EMPTY, ANY, mixed or element
       content. */
    bool is_declared;
    sxml_xml_content_kind content_kind;
    bool is_declared_in_external_markup;
} sxml_xml_open_content;

/* The two builders of a validating parse: the parse's, which may be the
   null builder, and the validator's checking builder. */
typedef struct sxml_xml_tee {
    sxml_tree_builder first;
    sxml_tree_builder second;
} sxml_xml_tee;

/* The work of one parse. */
typedef struct sxml_xml_document {
    sxml_xml_parser *parser;
    const sxml_xml_parse_request *request;
    sxml_tree_builder builder;
    TSParser *syntax_parser;
    TSTree *syntax_tree;
    /* The document as UTF-8. */
    sxml_byte_view text;
    sxml_xml_position_tracker tracker;
    sxml_xml_encoding_detection detection;
    bool has_doctype;
    bool has_external_subset;
    bool has_parameter_entity_references;
    bool is_standalone;
    /* The XML declaration's version (15): 1.1 changes the characters
       allowed, the line ends and namespace undeclaration. */
    sxml_xml_version version;
    /* WFC: Entity Declared applies (4.1). */
    bool requires_declared_entities;
    /* A parameter entity was not read: later entity and attribute-list
       declarations are not processed (5.1). */
    bool skips_declarations;
    /* request->entity_resolver is not the null resolver. */
    bool reads_external_entities;
    /* request->validator is not the null validator. */
    bool validates;
    /* The DTD declares element types: the declarations of the open
       elements are kept (open_contents), for element content white space
       and validity. */
    bool tracks_contents;
    /* The name the document type declaration gives, for VC: Root Element
       Type (2.8). */
    sxml_byte_view doctype_name;
    /* The events of a validating parse go to both builders. */
    sxml_xml_tee tee;
    size_t expansion_byte_count;
    uint32_t element_depth;
    sxml_namespace_prefix_identifier xml_prefix;
    sxml_namespace_prefix_identifier xmlns_prefix;
    /* An incremental parse (glossary `incremental_parse`): the walk of
       the content stops after a visit whose events asked to pause, and
       the frames below content_base_frame_count are not the content's. */
    bool is_incremental;
    bool pause_requested;
    size_t content_base_frame_count;
    /* Where the next content segment starts (D222): the end of the head or
       of the last segment parsed. */
    size_t segment_end_offset;
} sxml_xml_document;

/* ------------------------------------------------------- Node types */

/* The node types of grammars/xml the walks act on. */
typedef enum sxml_xml_node_type {
    SXML_XML_NODE_OTHER,
    SXML_XML_NODE_ATTRIBUTE,
    SXML_XML_NODE_ATTRIBUTE_DEFINITION,
    SXML_XML_NODE_ATTRIBUTE_LIST_DECLARATION,
    SXML_XML_NODE_ATTRIBUTE_VALUE,
    SXML_XML_NODE_ATTRIBUTE_VALUE_TEXT,
    SXML_XML_NODE_CDATA_SECTION,
    SXML_XML_NODE_CDATA_TEXT,
    SXML_XML_NODE_CHARACTER_DATA,
    SXML_XML_NODE_CHARACTER_REFERENCE,
    SXML_XML_NODE_CHOICE_GROUP,
    SXML_XML_NODE_COMMENT,
    SXML_XML_NODE_COMMENT_TEXT,
    SXML_XML_NODE_CONDITIONAL_SECTION,
    SXML_XML_NODE_CONTENT_PARTICLE,
    SXML_XML_NODE_ELEMENT,
    SXML_XML_NODE_ELEMENT_CONTENT,
    SXML_XML_NODE_ELEMENT_DECLARATION,
    SXML_XML_NODE_EMPTY_ELEMENT_TAG,
    SXML_XML_NODE_END_TAG,
    SXML_XML_NODE_ENTITY_REFERENCE,
    SXML_XML_NODE_ENTITY_VALUE_TEXT,
    SXML_XML_NODE_ENUMERATION,
    SXML_XML_NODE_GENERAL_ENTITY_DECLARATION,
    SXML_XML_NODE_INCLUDE_SECTION,
    SXML_XML_NODE_MIXED_CONTENT,
    SXML_XML_NODE_NAME,
    SXML_XML_NODE_NOTATION_DECLARATION,
    SXML_XML_NODE_NOTATION_TYPE,
    SXML_XML_NODE_PARAMETER_ENTITY_DECLARATION,
    SXML_XML_NODE_PARAMETER_ENTITY_REFERENCE,
    SXML_XML_NODE_PROCESSING_INSTRUCTION,
    SXML_XML_NODE_SEQUENCE_GROUP,
    SXML_XML_NODE_START_TAG,
    SXML_XML_NODE_STRING_TYPE,
    SXML_XML_NODE_TEXT_DECLARATION,
    SXML_XML_NODE_TOKENIZED_TYPE,
    SXML_XML_NODE_UNEXPANDED_DECLARATION,
} sxml_xml_node_type;

typedef struct sxml_xml_node_type_entry {
    const char *type_name;
    sxml_xml_node_type node_type;
} sxml_xml_node_type_entry;

/* Sorted by type_name (strcmp order) for binary search. */
static const sxml_xml_node_type_entry sxml_xml_node_types[] = {
    {"attribute", SXML_XML_NODE_ATTRIBUTE},
    {"attribute_definition", SXML_XML_NODE_ATTRIBUTE_DEFINITION},
    {"attribute_list_declaration", SXML_XML_NODE_ATTRIBUTE_LIST_DECLARATION},
    {"attribute_value", SXML_XML_NODE_ATTRIBUTE_VALUE},
    {"attribute_value_text", SXML_XML_NODE_ATTRIBUTE_VALUE_TEXT},
    {"cdata_section", SXML_XML_NODE_CDATA_SECTION},
    {"cdata_text", SXML_XML_NODE_CDATA_TEXT},
    {"character_data", SXML_XML_NODE_CHARACTER_DATA},
    {"character_reference", SXML_XML_NODE_CHARACTER_REFERENCE},
    {"choice_group", SXML_XML_NODE_CHOICE_GROUP},
    {"comment", SXML_XML_NODE_COMMENT},
    {"comment_text", SXML_XML_NODE_COMMENT_TEXT},
    {"conditional_section", SXML_XML_NODE_CONDITIONAL_SECTION},
    {"content_particle", SXML_XML_NODE_CONTENT_PARTICLE},
    {"element", SXML_XML_NODE_ELEMENT},
    {"element_content", SXML_XML_NODE_ELEMENT_CONTENT},
    {"element_declaration", SXML_XML_NODE_ELEMENT_DECLARATION},
    {"empty_element_tag", SXML_XML_NODE_EMPTY_ELEMENT_TAG},
    {"end_tag", SXML_XML_NODE_END_TAG},
    {"entity_reference", SXML_XML_NODE_ENTITY_REFERENCE},
    {"entity_value_text", SXML_XML_NODE_ENTITY_VALUE_TEXT},
    {"enumeration", SXML_XML_NODE_ENUMERATION},
    {"general_entity_declaration", SXML_XML_NODE_GENERAL_ENTITY_DECLARATION},
    {"include_section", SXML_XML_NODE_INCLUDE_SECTION},
    {"mixed_content", SXML_XML_NODE_MIXED_CONTENT},
    {"name", SXML_XML_NODE_NAME},
    {"notation_declaration", SXML_XML_NODE_NOTATION_DECLARATION},
    {"notation_type", SXML_XML_NODE_NOTATION_TYPE},
    {"parameter_entity_declaration",
     SXML_XML_NODE_PARAMETER_ENTITY_DECLARATION},
    {"parameter_entity_reference", SXML_XML_NODE_PARAMETER_ENTITY_REFERENCE},
    {"processing_instruction", SXML_XML_NODE_PROCESSING_INSTRUCTION},
    {"sequence_group", SXML_XML_NODE_SEQUENCE_GROUP},
    {"start_tag", SXML_XML_NODE_START_TAG},
    {"string_type", SXML_XML_NODE_STRING_TYPE},
    {"text_declaration", SXML_XML_NODE_TEXT_DECLARATION},
    {"tokenized_type", SXML_XML_NODE_TOKENIZED_TYPE},
    {"unexpanded_declaration", SXML_XML_NODE_UNEXPANDED_DECLARATION},
};

static sxml_xml_node_type sxml_xml_type_of(TSNode node) {
    if (!ts_node_is_named(node)) {
        return SXML_XML_NODE_OTHER;
    }
    const char *type_name = ts_node_type(node);
    size_t low = 0;
    size_t high = sizeof sxml_xml_node_types / sizeof sxml_xml_node_types[0];
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const int comparison =
            strcmp(type_name, sxml_xml_node_types[middle].type_name);
        if (comparison == 0) {
            return sxml_xml_node_types[middle].node_type;
        }
        if (comparison < 0) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }
    return SXML_XML_NODE_OTHER;
}

/* The field names of grammars/xml the walks read. */
typedef enum sxml_xml_field_name {
    SXML_XML_FIELD_CONTENT,
    SXML_XML_FIELD_CONTENTS,
    SXML_XML_FIELD_DECLARATION,
    SXML_XML_FIELD_DEFAULT,
    SXML_XML_FIELD_DOCTYPE,
    SXML_XML_FIELD_ENCODING,
    SXML_XML_FIELD_EXTERNAL_ENTITY,
    SXML_XML_FIELD_EXTERNAL_ID,
    SXML_XML_FIELD_EXTERNAL_SUBSET,
    SXML_XML_FIELD_INTERNAL_SUBSET,
    SXML_XML_FIELD_KEYWORD,
    SXML_XML_FIELD_NAME,
    SXML_XML_FIELD_NOTATION,
    SXML_XML_FIELD_PUBLIC_ID,
    SXML_XML_FIELD_ROOT,
    SXML_XML_FIELD_STANDALONE,
    SXML_XML_FIELD_SYSTEM_ID,
    SXML_XML_FIELD_TARGET,
    SXML_XML_FIELD_TYPE,
    SXML_XML_FIELD_VALUE,
    SXML_XML_FIELD_VERSION,
} sxml_xml_field_name;

static const char *const sxml_xml_field_texts[] = {
    "content",         "contents",        "declaration",     "default",
    "doctype",         "encoding",        "external_entity", "external_id",
    "external_subset", "internal_subset", "keyword",         "name",
    "notation",        "public_id",       "root",            "standalone",
    "system_id",       "target",          "type",            "value",
    "version",
};

/* The child of node in the field field_name. */
static TSNode sxml_xml_field(TSNode node, sxml_xml_field_name field_name) {
    const char *text = sxml_xml_field_texts[field_name];
    return ts_node_child_by_field_name(node, text, (uint32_t)strlen(text));
}

/* --------------------------------------------------------- Lifecycle */

static bool sxml_xml_configuration_is_valid(
    const sxml_xml_parser_configuration *configuration) {
    return configuration != NULL &&
           !sxml_allocator_is_null(configuration->allocator) &&
           configuration->name_pool != NULL;
}

sxml_status sxml_xml_parser_memory_requirements(
    const sxml_xml_parser_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (!sxml_xml_configuration_is_valid(configuration) ||
        requirements == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){
        .byte_count = sizeof(struct sxml_xml_parser),
        .alignment = alignof(struct sxml_xml_parser)};
    return SXML_STATUS_OK;
}

static sxml_xml_array sxml_xml_parser_array(sxml_allocator allocator,
                                            size_t element_byte_count) {
    return sxml_xml_array_empty((sxml_xml_array_configuration){
        .allocator = allocator, .element_byte_count = element_byte_count});
}

sxml_status
sxml_xml_parser_initialize(sxml_memory_view memory,
                           const sxml_xml_parser_configuration *configuration,
                           sxml_xml_parser **parser) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_xml_parser_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK || parser == NULL) {
        return status == SXML_STATUS_OK ? SXML_STATUS_INVALID_ARGUMENT : status;
    }
    status = sxml_memory_view_check_requirements(memory, requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator = configuration->allocator;
    void *placement = memory.bytes;
    sxml_xml_parser *placed = placement;
    *placed = (sxml_xml_parser){
        .configuration = *configuration,
        .owning_allocator = {.state = NULL, .operations = NULL},
        .transcoded_text = sxml_xml_buffer_empty(allocator),
        .attribute_values = sxml_xml_buffer_empty(allocator),
        .scratch_text = sxml_xml_buffer_empty(allocator),
        .dtd = sxml_xml_dtd_empty(allocator),
        .attributes =
            sxml_xml_parser_array(allocator, sizeof(sxml_xml_attribute)),
        .namespace_bindings =
            sxml_xml_parser_array(allocator, sizeof(sxml_xml_scoped_binding)),
        .element_scopes = sxml_xml_parser_array(allocator, sizeof(size_t)),
        .frames = sxml_xml_parser_array(allocator, sizeof(sxml_xml_frame)),
        .expansions =
            sxml_xml_parser_array(allocator, sizeof(sxml_xml_expansion)),
        .duplicate_slots = sxml_xml_parser_array(allocator, sizeof(uint32_t)),
        .child_nodes = sxml_xml_parser_array(allocator, sizeof(TSNode)),
        .schema_text = sxml_xml_buffer_empty(allocator),
        .open_contents =
            sxml_xml_parser_array(allocator, sizeof(sxml_xml_open_content)),
        .parenthesis_inclusions = sxml_xml_parser_array(allocator, sizeof(size_t)),
        .schema_documents =
            sxml_xml_parser_array(allocator, sizeof(sxml_byte_range)),
        .schema_views = sxml_xml_parser_array(allocator, sizeof(sxml_byte_view)),
        .prefix_bindings =
            sxml_xml_parser_array(allocator, sizeof(sxml_dtd_prefix_binding)),
        .open_element_names =
            sxml_xml_parser_array(allocator, sizeof(sxml_byte_view)),
        .segment_prefix = sxml_xml_buffer_empty(allocator),
    };
    *parser = placed;
    return SXML_STATUS_OK;
}

void sxml_xml_parser_finalize(sxml_xml_parser *parser) {
    if (parser == NULL) {
        return;
    }
    sxml_xml_buffer_release(&parser->transcoded_text);
    sxml_xml_buffer_release(&parser->attribute_values);
    sxml_xml_buffer_release(&parser->scratch_text);
    sxml_xml_dtd_release(&parser->dtd);
    sxml_xml_array_release(&parser->attributes);
    sxml_xml_array_release(&parser->namespace_bindings);
    sxml_xml_array_release(&parser->element_scopes);
    sxml_xml_array_release(&parser->frames);
    sxml_xml_array_release(&parser->expansions);
    sxml_xml_array_release(&parser->duplicate_slots);
    sxml_xml_array_release(&parser->child_nodes);
    sxml_xml_buffer_release(&parser->schema_text);
    sxml_xml_array_release(&parser->open_contents);
    sxml_xml_array_release(&parser->parenthesis_inclusions);
    sxml_xml_array_release(&parser->schema_documents);
    sxml_xml_array_release(&parser->schema_views);
    sxml_xml_array_release(&parser->prefix_bindings);
    sxml_xml_array_release(&parser->open_element_names);
    sxml_xml_buffer_release(&parser->segment_prefix);
}

sxml_status
sxml_xml_parser_create(sxml_allocator allocator,
                       const sxml_xml_parser_configuration *configuration,
                       sxml_xml_parser **parser) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_xml_parser_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK || parser == NULL) {
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
    sxml_xml_parser *created = NULL;
    status = sxml_xml_parser_initialize(
        (sxml_memory_view){.bytes = memory,
                           .byte_count = requirements.byte_count},
        configuration, &created);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    created->owning_allocator = allocator;
    *parser = created;
    return SXML_STATUS_OK;
}

void sxml_xml_parser_destroy(sxml_xml_parser *parser) {
    if (parser == NULL) {
        return;
    }
    const sxml_allocator allocator = parser->owning_allocator;
    sxml_xml_parser_finalize(parser);
    sxml_allocator_release(allocator, parser);
}

/* ----------------------------------------------------------- Failures */

/* What went wrong and where. */
typedef struct sxml_xml_failure {
    sxml_status status;
    /* Static text naming the production or constraint. */
    sxml_byte_view description;
    sxml_source_location location;
} sxml_xml_failure;

#define SXML_XML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static sxml_status sxml_xml_fail(const sxml_xml_document *document,
                                 sxml_xml_failure failure) {
    if (document->request->diagnostic != NULL) {
        *document->request->diagnostic = (sxml_diagnostic){
            .description = failure.description, .location = failure.location};
    }
    return failure.status;
}

/* The location of a byte offset in the document's UTF-8 text. */
static sxml_source_location sxml_xml_location_at(sxml_xml_document *document,
                                                 size_t text_offset) {
    const sxml_xml_position position =
        sxml_xml_position_tracker_locate(&document->tracker, text_offset);
    return (sxml_source_location){
        .source_name = document->request->source_name,
        .line_number = position.line_number,
        .column_number = position.column_number,
        .byte_offset = position.input_byte_offset,
    };
}

/* Where node is, for events and errors: in an entity frame, the reference
   that started the expansion. */
/* The offset in the document's text of byte tree_offset of the tree of
   a frame over the document's own text: past the frame's prefix. */
static size_t sxml_xml_document_offset_of(const sxml_xml_frame *frame,
                                          uint32_t tree_offset) {
    return frame->document_offset + (tree_offset < frame->prefix_byte_count
                                         ? 0
                                         : tree_offset - frame->prefix_byte_count);
}

static sxml_source_location sxml_xml_node_location(sxml_xml_document *document,
                                                   const sxml_xml_frame *frame,
                                                   TSNode node) {
    if (frame->frame_kind != SXML_XML_FRAME_KIND_DOCUMENT) {
        return frame->reference_location;
    }
    return sxml_xml_location_at(
        document, sxml_xml_document_offset_of(frame, ts_node_start_byte(node)));
}

/* A well-formedness failure at a node of a frame. */
typedef struct sxml_xml_node_failure {
    const sxml_xml_frame *frame;
    TSNode node;
    sxml_byte_view description;
} sxml_xml_node_failure;

static sxml_status sxml_xml_fail_at(sxml_xml_document *document,
                                    sxml_xml_node_failure failure) {
    return sxml_xml_fail(
        document,
        (sxml_xml_failure){.status = SXML_STATUS_MALFORMED_INPUT,
                           .description = failure.description,
                           .location = sxml_xml_node_location(
                               document, failure.frame, failure.node)});
}

/* Reports an allocation or limit failure of the parser's own tables. */
static sxml_status sxml_xml_fail_resource(const sxml_xml_document *document,
                                          sxml_status status) {
    if (status == SXML_STATUS_OK) {
        return status;
    }
    const sxml_byte_view description =
        status == SXML_STATUS_LIMIT_EXCEEDED
            ? SXML_XML_TEXT("a limit of the configuration was exceeded")
            : SXML_XML_TEXT("the parser could not allocate memory");
    return sxml_xml_fail(
        document,
        (sxml_xml_failure){
            .status = status,
            .description = description,
            .location = {.source_name = document->request->source_name,
                         .line_number = 0,
                         .column_number = 0,
                         .byte_offset = 0}});
}

/*
 * Reports a validity error the parser checks itself to the request's
 * validator (D199), when the parse validates; the validator's status
 * decides whether the parse goes on.
 */
typedef struct sxml_xml_validity_failure {
    /* Static text naming the constraint, "VC: ..." (XML 1.0). */
    sxml_byte_view description;
    sxml_source_location location;
} sxml_xml_validity_failure;

static sxml_status sxml_xml_report_validity(const sxml_xml_document *document,
                                            sxml_xml_validity_failure failure) {
    if (!document->validates) {
        return SXML_STATUS_OK;
    }
    const sxml_xml_validator validator = document->request->validator;
    const sxml_status status = validator.operations->report_validity_error(
        validator.state,
        &(sxml_xml_validity_error){.description = failure.description,
                                   .location = failure.location});
    if (status != SXML_STATUS_OK) {
        return sxml_xml_fail(document,
                             (sxml_xml_failure){.status = status,
                                                .description = failure.description,
                                                .location = failure.location});
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Text */

static sxml_byte_view sxml_xml_view_from_text(sxml_byte_view text,
                                              size_t byte_offset,
                                              size_t byte_count) {
    if (byte_count == 0) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return (sxml_byte_view){.bytes = text.bytes + byte_offset,
                            .byte_count = byte_count};
}

/* The bytes of node, borrowed from the frame's text. */
static sxml_byte_view sxml_xml_node_text(const sxml_xml_frame *frame,
                                         TSNode node) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    const uint32_t prefix = frame->prefix_byte_count;
    start = start < prefix ? 0 : start - prefix;
    end = end < prefix ? 0 : end - prefix;
    if (end > frame->text.byte_count) {
        end = (uint32_t)frame->text.byte_count;
    }
    if (start > end) {
        start = end;
    }
    return sxml_xml_view_from_text(frame->text, start, end - start);
}

/* The text of the child of node in field_name; empty when absent. */
static sxml_byte_view sxml_xml_field_text(const sxml_xml_frame *frame,
                                          TSNode node,
                                          sxml_xml_field_name field_name) {
    const TSNode child = sxml_xml_field(node, field_name);
    if (ts_node_is_null(child)) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return sxml_xml_node_text(frame, child);
}

static bool sxml_xml_view_contains_byte(sxml_byte_view view, uint8_t byte) {
    return view.byte_count != 0 &&
           memchr(view.bytes, byte, view.byte_count) != NULL;
}

/*
 * Appends text to buffer with line ends normalized (XML 1.0, section 2.11)
 * when normalizes is true; replacement text is not normalized again.
 */
typedef struct sxml_xml_text_append {
    sxml_byte_view text;
    bool normalizes_line_ends;
} sxml_xml_text_append;

static sxml_status sxml_xml_append_text(sxml_xml_buffer *buffer,
                                        sxml_xml_text_append append) {
    const sxml_byte_view text = append.text;
    if (!append.normalizes_line_ends ||
        !sxml_xml_view_contains_byte(text, '\r')) {
        return sxml_xml_buffer_append(buffer, text);
    }
    sxml_status status = sxml_xml_buffer_reserve(buffer, text.byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index++) {
        uint8_t byte = text.bytes[byte_index];
        if (byte == '\r') {
            byte = '\n';
            if (byte_index + 1 < text.byte_count &&
                text.bytes[byte_index + 1] == '\n') {
                byte_index += 1;
            }
        }
        buffer->bytes[buffer->byte_count] = byte;
        buffer->byte_count += 1;
    }
    return SXML_STATUS_OK;
}

/*
 * Decodes a character reference token, "&#123;" or "&#x7B;" ([66]
 * CharRef), into *codepoint. Returns false when it does not denote a [2]
 * Char of the document's version (WFC: Legal Character, 4.1; XML 1.1 adds
 * the C0 controls), however many digits it has.
 */
static bool
sxml_xml_decode_character_reference(const sxml_xml_document *document,
                                    sxml_byte_view reference,
                                    uint32_t *codepoint) {
    if (reference.byte_count < 4) {
        return false;
    }
    const bool is_hexadecimal = reference.bytes[2] == 'x';
    size_t byte_index = is_hexadecimal ? 3 : 2;
    uint32_t value = 0;
    bool is_too_large = false;
    size_t digit_count = 0;
    for (; byte_index < reference.byte_count - 1; byte_index++) {
        const uint8_t byte = reference.bytes[byte_index];
        uint32_t digit = 0;
        if (byte >= '0' && byte <= '9') {
            digit = (uint32_t)(byte - '0');
        } else if (is_hexadecimal && byte >= 'a' && byte <= 'f') {
            digit = (uint32_t)(byte - 'a' + 10);
        } else if (is_hexadecimal && byte >= 'A' && byte <= 'F') {
            digit = (uint32_t)(byte - 'A' + 10);
        } else {
            return false;
        }
        digit_count += 1;
        /* Stop growing past the largest codepoint, so that no digit count
           can overflow. */
        if (!is_too_large) {
            value = value * (is_hexadecimal ? 16U : 10U) + digit;
            is_too_large = value > 0x10FFFF;
        }
    }
    const bool is_char = document->version == SXML_XML_VERSION_1_1
                             ? sxml_xml_codepoint_is_version_1_1_char(value)
                             : sxml_xml_codepoint_is_char(value);
    if (digit_count == 0 || reference.bytes[reference.byte_count - 1] != ';' ||
        is_too_large || !is_char) {
        return false;
    }
    *codepoint = value;
    return true;
}

/* The character of a predefined entity (4.6), if name is one. */
typedef struct sxml_xml_predefined_entity {
    bool is_predefined;
    uint8_t character;
} sxml_xml_predefined_entity;

static sxml_xml_predefined_entity
sxml_xml_find_predefined_entity(sxml_byte_view name) {
    static const struct {
        const char *name;
        uint8_t character;
    } predefined[] = {
        {"lt", '<'}, {"gt", '>'}, {"amp", '&'}, {"apos", '\''}, {"quot", '"'},
    };
    for (size_t entry_index = 0;
         entry_index < sizeof predefined / sizeof predefined[0];
         entry_index++) {
        if (sxml_byte_view_equals(name, sxml_byte_view_from_c_string(
                                            predefined[entry_index].name))) {
            return (sxml_xml_predefined_entity){
                .is_predefined = true,
                .character = predefined[entry_index].character};
        }
    }
    return (sxml_xml_predefined_entity){.is_predefined = false, .character = 0};
}

/* ----------------------------------------------------------- Names */

/* A qualified name split at its colon (Namespaces in XML 1.0, [7]). */
typedef struct sxml_xml_name_split {
    bool is_qualified_name;
    sxml_byte_view namespace_prefix;
    sxml_byte_view local_name;
} sxml_xml_name_split;

/*
 * Splits a [5] Name into prefix and local part, checking that it is a
 * QName: at most one colon, not first or last, and a local part that
 * starts with a NameStartChar (NS 1.0, sections 3 and 7).
 */
static sxml_xml_name_split sxml_xml_split_name(sxml_byte_view name) {
    sxml_xml_name_split split = {
        .is_qualified_name = false,
        .namespace_prefix = {.bytes = NULL, .byte_count = 0},
        .local_name = name,
    };
    size_t colon_count = 0;
    size_t colon_offset = 0;
    for (size_t byte_index = 0; byte_index < name.byte_count; byte_index++) {
        if (name.bytes[byte_index] == ':') {
            colon_count += 1;
            colon_offset = byte_index;
        }
    }
    if (colon_count == 0) {
        split.is_qualified_name = name.byte_count != 0;
        return split;
    }
    if (colon_count > 1 || colon_offset == 0 ||
        colon_offset + 1 == name.byte_count) {
        return split;
    }
    split.namespace_prefix = sxml_xml_view_from_text(name, 0, colon_offset);
    split.local_name = sxml_xml_view_from_text(
        name, colon_offset + 1, name.byte_count - colon_offset - 1);
    split.is_qualified_name = sxml_xml_codepoint_is_name_start(
        sxml_xml_decode_codepoint(split.local_name, 0).codepoint);
    return split;
}

static const sxml_byte_view sxml_xml_qualified_name_description =
    SXML_BYTE_VIEW_INITIALIZER(
        "NSC: a name must be a QName: at most one colon, not first or last "
        "(Namespaces in XML 1.0, section 3)");

/* NS 1.0, section 7: no colon in entity names, PI targets, notation
   names. */
static sxml_status sxml_xml_check_no_colon(sxml_xml_document *document,
                                           const sxml_xml_frame *frame,
                                           TSNode name_node) {
    if (sxml_xml_view_contains_byte(sxml_xml_node_text(frame, name_node),
                                    ':')) {
        return sxml_xml_fail_at(
            document,
            (sxml_xml_node_failure){
                .frame = frame,
                .node = name_node,
                .description = SXML_XML_TEXT(
                    "NSC: entity names, processing-instruction targets "
                    "and notation names contain no colon (Namespaces "
                    "in XML 1.0, section 7)")});
    }
    return SXML_STATUS_OK;
}

static sxml_status sxml_xml_check_qualified_name(sxml_xml_document *document,
                                                 const sxml_xml_frame *frame,
                                                 TSNode name_node) {
    if (!sxml_xml_split_name(sxml_xml_node_text(frame, name_node))
             .is_qualified_name) {
        return sxml_xml_fail_at(
            document, (sxml_xml_node_failure){
                          .frame = frame,
                          .node = name_node,
                          .description = sxml_xml_qualified_name_description});
    }
    return SXML_STATUS_OK;
}

/* --------------------------------------------------------- Children */

/* The named children of one node, listed on the parser's child stack. */
typedef struct sxml_xml_children {
    size_t first_index;
    size_t child_count;
} sxml_xml_children;

/* Lists the named children of node with one cursor walk. Release the list
   with sxml_xml_release_children, innermost first. */
static sxml_status sxml_xml_list_children(sxml_xml_document *document,
                                          TSNode node,
                                          sxml_xml_children *children) {
    sxml_xml_array *child_nodes = &document->parser->child_nodes;
    const size_t first_index = child_nodes->element_count;
    sxml_status status = SXML_STATUS_OK;
    TSTreeCursor cursor = ts_tree_cursor_new(node);
    for (bool has_child = ts_tree_cursor_goto_first_child(&cursor);
         has_child && status == SXML_STATUS_OK;
         has_child = ts_tree_cursor_goto_next_sibling(&cursor)) {
        const TSNode child = ts_tree_cursor_current_node(&cursor);
        if (!ts_node_is_named(child)) {
            continue;
        }
        void *element = NULL;
        status = sxml_xml_array_push(child_nodes, &element);
        if (status == SXML_STATUS_OK) {
            *(TSNode *)element = child;
        }
    }
    ts_tree_cursor_delete(&cursor);
    if (status != SXML_STATUS_OK) {
        child_nodes->element_count = first_index;
        return status;
    }
    *children = (sxml_xml_children){.first_index = first_index,
                                    .child_count = child_nodes->element_count -
                                                   first_index};
    return SXML_STATUS_OK;
}

static TSNode sxml_xml_child_at(const sxml_xml_document *document,
                                sxml_xml_children children,
                                size_t child_index) {
    return *(const TSNode *)sxml_xml_array_at(
        &document->parser->child_nodes, children.first_index + child_index);
}

static void sxml_xml_release_children(sxml_xml_document *document,
                                      sxml_xml_children children) {
    document->parser->child_nodes.element_count = children.first_index;
}

/* ------------------------------------------------------------ Frames */

static sxml_xml_frame *sxml_xml_frame_at(const sxml_xml_document *document,
                                         size_t frame_index) {
    return sxml_xml_array_at(&document->parser->frames, frame_index);
}

static size_t sxml_xml_frame_count(const sxml_xml_document *document) {
    return document->parser->frames.element_count;
}

/* Ends the top frame: its cursor, its tree, its entity's expansion. */
static void sxml_xml_pop_frame(sxml_xml_document *document) {
    sxml_xml_array *frames = &document->parser->frames;
    sxml_xml_frame *frame =
        sxml_xml_array_at(frames, frames->element_count - 1);
    ts_tree_cursor_delete(&frame->cursor);
    if (frame->syntax_tree != NULL) {
        ts_tree_delete(frame->syntax_tree);
    }
    sxml_xml_buffer_release(&frame->owned_text);
    if (frame->has_entity) {
        sxml_xml_dtd_get_entity(&document->parser->dtd, frame->entity_index)
            ->is_being_expanded = false;
    }
    frames->element_count -= 1;
}

/* Pops every frame above frame_count. */
static void sxml_xml_pop_frames_to(sxml_xml_document *document,
                                   size_t frame_count) {
    while (sxml_xml_frame_count(document) > frame_count) {
        sxml_xml_pop_frame(document);
    }
}

/*
 * Pushes a frame whose cursor walks the children of walk_root. Takes
 * ownership of frame->syntax_tree and frame->owned_text even on failure.
 * A walk root without children pushes nothing.
 */
static sxml_status sxml_xml_push_frame(sxml_xml_document *document,
                                       const sxml_xml_frame *frame,
                                       TSNode walk_root) {
    TSTreeCursor cursor = ts_tree_cursor_new(walk_root);
    if (!ts_tree_cursor_goto_first_child(&cursor)) {
        ts_tree_cursor_delete(&cursor);
        if (frame->syntax_tree != NULL) {
            ts_tree_delete(frame->syntax_tree);
        }
        sxml_xml_buffer owned_text = frame->owned_text;
        sxml_xml_buffer_release(&owned_text);
        return SXML_STATUS_OK;
    }
    void *element = NULL;
    sxml_status status =
        sxml_xml_array_push(&document->parser->frames, &element);
    if (status != SXML_STATUS_OK) {
        ts_tree_cursor_delete(&cursor);
        if (frame->syntax_tree != NULL) {
            ts_tree_delete(frame->syntax_tree);
        }
        sxml_xml_buffer owned_text = frame->owned_text;
        sxml_xml_buffer_release(&owned_text);
        return sxml_xml_fail_resource(document, status);
    }
    sxml_xml_frame *pushed = element;
    *pushed = *frame;
    pushed->cursor = cursor;
    pushed->cursor_depth = 1;
    pushed->element_depth = document->element_depth;
    pushed->needs_advance = false;
    pushed->descends = false;
    if (pushed->has_entity) {
        sxml_xml_dtd_get_entity(&document->parser->dtd, pushed->entity_index)
            ->is_being_expanded = true;
    }
    return SXML_STATUS_OK;
}

/* Moves the frame's cursor to the next node of its walk; false at the end. */
static bool sxml_xml_frame_advance(sxml_xml_frame *frame) {
    TSTreeCursor *cursor = &frame->cursor;
    if (frame->descends && ts_tree_cursor_goto_first_child(cursor)) {
        frame->cursor_depth += 1;
        return true;
    }
    for (;;) {
        if (ts_tree_cursor_goto_next_sibling(cursor)) {
            return true;
        }
        if (!ts_tree_cursor_goto_parent(cursor)) {
            return false;
        }
        frame->cursor_depth -= 1;
        if (frame->cursor_depth == 0) {
            return false;
        }
    }
}

/* ---------------------------------------------------- Wrapped parses */

/* A replacement text read by tree-sitter between a prefix and a suffix. */
typedef struct sxml_xml_wrapped_text {
    sxml_byte_view prefix;
    sxml_byte_view text;
    sxml_byte_view suffix;
} sxml_xml_wrapped_text;

/*
 * The input callback of tree-sitter (TSInput.read). Its signature is the
 * runtime's and takes four parameters, an exception to the three-parameter
 * rule (docs/C_GUIDELINES.md, "Function signatures").
 */
// ast-grep-ignore: too-many-parameters
static const char *sxml_xml_read_wrapped( // NOLINT(readability-function-size)
    void *payload, uint32_t byte_index, TSPoint position,
    uint32_t *bytes_read) {
    (void)position;
    const sxml_xml_wrapped_text *wrapped = payload;
    size_t offset = byte_index;
    const sxml_byte_view parts[3] = {wrapped->prefix, wrapped->text,
                                     wrapped->suffix};
    for (size_t part_index = 0; part_index < 3; part_index++) {
        if (offset < parts[part_index].byte_count) {
            /* The total was checked to fit uint32_t. */
            *bytes_read = (uint32_t)(parts[part_index].byte_count - offset);
            return (const char *)parts[part_index].bytes + offset;
        }
        offset -= parts[part_index].byte_count;
    }
    *bytes_read = 0;
    return "";
}

/* Finds the first ERROR or MISSING node under root; null when none. */
static TSNode sxml_xml_find_error(TSNode root) {
    TSNode found = {.context = {0, 0, 0, 0}, .id = NULL, .tree = NULL};
    if (!ts_node_has_error(root)) {
        return found;
    }
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    for (;;) {
        const TSNode node = ts_tree_cursor_current_node(&cursor);
        if (ts_node_is_error(node) || ts_node_is_missing(node)) {
            found = node;
            break;
        }
        if (ts_node_has_error(node) &&
            ts_tree_cursor_goto_first_child(&cursor)) {
            continue;
        }
        bool has_next = false;
        while (!has_next) {
            has_next = ts_tree_cursor_goto_next_sibling(&cursor);
            if (!has_next && !ts_tree_cursor_goto_parent(&cursor)) {
                break;
            }
        }
        if (!has_next) {
            break;
        }
    }
    ts_tree_cursor_delete(&cursor);
    /* has_error without an ERROR or MISSING descendant: report the root
       rather than accept the text. */
    return ts_node_is_null(found) ? root : found;
}

/* Parses a wrapped replacement text; writes NULL when it has errors. */
static sxml_status sxml_xml_parse_wrapped(sxml_xml_document *document,
                                          const sxml_xml_wrapped_text *wrapped,
                                          TSTree **syntax_tree) {
    size_t total = 0;
    sxml_status status = sxml_size_add(wrapped->prefix.byte_count,
                                       wrapped->text.byte_count, &total);
    if (status == SXML_STATUS_OK) {
        status = sxml_size_add(total, wrapped->suffix.byte_count, &total);
    }
    if (status != SXML_STATUS_OK || total > UINT32_MAX) {
        return sxml_xml_fail_resource(document, SXML_STATUS_LIMIT_EXCEEDED);
    }
    TSTree *tree = ts_parser_parse(document->syntax_parser, NULL,
                                   (TSInput){.payload = (void *)wrapped,
                                             .read = sxml_xml_read_wrapped,
                                             .encoding = TSInputEncodingUTF8,
                                             .decode = NULL});
    if (tree == NULL) {
        return sxml_xml_fail_resource(document, SXML_STATUS_OUT_OF_MEMORY);
    }
    if (!ts_node_is_null(sxml_xml_find_error(ts_tree_root_node(tree)))) {
        ts_tree_delete(tree);
        *syntax_tree = NULL;
        return SXML_STATUS_OK;
    }
    *syntax_tree = tree;
    return SXML_STATUS_OK;
}

/* Counts replacement text against the expansion limit (4.4). */
/* Replacement text about to be expanded at a reference. */
typedef struct sxml_xml_expansion_count {
    size_t byte_count;
    sxml_source_location location;
} sxml_xml_expansion_count;

static sxml_status
sxml_xml_count_expansion(sxml_xml_document *document,
                         sxml_xml_expansion_count expansion) {
    const size_t byte_count = expansion.byte_count;
    const size_t limit = document->parser->configuration.limits
                             .maximum_entity_expansion_byte_count;
    size_t total = 0;
    if (sxml_size_add(document->expansion_byte_count, byte_count, &total) !=
            SXML_STATUS_OK ||
        (limit != 0 && total > limit)) {
        return sxml_xml_fail(
            document, (sxml_xml_failure){
                          .status = SXML_STATUS_LIMIT_EXCEEDED,
                          .description = SXML_XML_TEXT(
                              "entity references expand to more text than the "
                              "configured maximum (XML 1.0, section 4.4)"),
                          .location = expansion.location});
    }
    document->expansion_byte_count = total;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- Encodings */

static bool sxml_xml_is_utf16_family(sxml_xml_encoding_family family) {
    return family == SXML_XML_ENCODING_FAMILY_UTF16 ||
           family == SXML_XML_ENCODING_FAMILY_UTF16_LITTLE_ENDIAN ||
           family == SXML_XML_ENCODING_FAMILY_UTF16_BIG_ENDIAN;
}

/* The encoding detected from an entity's bytes and the name its XML or
   text declaration gives (empty when it gives none). */
typedef struct sxml_xml_encoding_check {
    const sxml_xml_encoding_detection *detection;
    sxml_byte_view encoding_name;
} sxml_xml_encoding_check;

/* True when the encoding declaration agrees with the bytes (4.3.3,
   Appendix F). */
static bool sxml_xml_encoding_is_consistent(const sxml_xml_encoding_check *check) {
    const sxml_xml_encoding_detection *detection = check->detection;
    const sxml_byte_view encoding_name = check->encoding_name;
    const bool is_utf16 =
        detection->encoding == SXML_XML_ENCODING_UTF16_LITTLE_ENDIAN ||
        detection->encoding == SXML_XML_ENCODING_UTF16_BIG_ENDIAN;
    const sxml_xml_encoding_family family =
        sxml_xml_classify_encoding_name(encoding_name);
    if (is_utf16) {
        /* Without a byte order mark only the byte-order-specific names
           say UTF-16 (4.3.3: UTF-16 entities begin with the mark). */
        return detection->has_utf16_byte_order_mark
                   ? encoding_name.byte_count == 0 ||
                         sxml_xml_is_utf16_family(family)
                   : (family == SXML_XML_ENCODING_FAMILY_UTF16_LITTLE_ENDIAN &&
                      detection->encoding ==
                          SXML_XML_ENCODING_UTF16_LITTLE_ENDIAN) ||
                         (family == SXML_XML_ENCODING_FAMILY_UTF16_BIG_ENDIAN &&
                          detection->encoding ==
                              SXML_XML_ENCODING_UTF16_BIG_ENDIAN);
    }
    if (detection->has_utf8_byte_order_mark) {
        return encoding_name.byte_count == 0 ||
               family == SXML_XML_ENCODING_FAMILY_UTF8;
    }
    return !sxml_xml_is_utf16_family(family);
}

/* ------------------------------------------------- External entities */

/* The markers the grammar reads before the text of external DTD text (the
   external subset, an external parameter entity, and any DTD text read as
   they are) and before an external parsed entity (grammars/xml,
   document). */
static const sxml_byte_view sxml_xml_external_subset_marker =
    SXML_BYTE_VIEW_INITIALIZER("\001S");
static const sxml_byte_view sxml_xml_external_entity_marker =
    SXML_BYTE_VIEW_INITIALIZER("\001E");

/* Normalizes the line ends of text in place (XML 1.0, section 2.11): CR LF
   and a lone CR become LF. */
static void sxml_xml_normalize_line_ends(sxml_xml_buffer *text) {
    if (text->byte_count == 0 ||
        memchr(text->bytes, '\r', text->byte_count) == NULL) {
        return;
    }
    size_t written = 0;
    for (size_t byte_index = 0; byte_index < text->byte_count; byte_index++) {
        uint8_t byte = text->bytes[byte_index];
        if (byte == '\r') {
            byte = '\n';
            if (byte_index + 1 < text->byte_count &&
                text->bytes[byte_index + 1] == '\n') {
                byte_index += 1;
            }
        }
        text->bytes[written] = byte;
        written += 1;
    }
    text->byte_count = written;
}

/* An external entity's bytes to decode, and where its reference is. */
typedef struct sxml_xml_external_decoding {
    const sxml_xml_external_entity *entity;
    sxml_source_location location;
} sxml_xml_external_decoding;

/*
 * Decodes an external entity into text as the document is decoded
 * (4.3.3, Appendix F): its own encoding, a byte order mark dropped, a
 * text declaration that must agree with the bytes; then its line ends
 * normalized (2.11) and, in an XML 1.1 document, XML 1.1's characters and
 * line ends (XML 1.1, 2.2, 2.11 and 4.3.4: the entities of a 1.1 document
 * are read as 1.1). Failures are reported at the reference.
 */
static sxml_status
sxml_xml_decode_external(sxml_xml_document *document,
                         const sxml_xml_external_decoding *decoding,
                         sxml_xml_buffer *text) {
    const sxml_xml_external_entity *entity = decoding->entity;
    sxml_xml_failure failure = {
        .status = SXML_STATUS_UNSUPPORTED_FEATURE,
        .description = SXML_XML_TEXT(
            "the encoding of an external entity is not one sxml reads: UTF-8, "
            "UTF-16, ISO-8859-1 or US-ASCII (XML 1.0, section 4.3.3)"),
        .location = decoding->location};
    sxml_xml_encoding_detection detection;
    sxml_status status = sxml_xml_detect_encoding(
        &(sxml_xml_encoding_request){.input = entity->content,
                                     .encoding_name = entity->encoding_name},
        &detection);
    if (status != SXML_STATUS_OK) {
        return sxml_xml_fail(document, failure);
    }
    sxml_xml_transcoding_failure transcoding_failure = {.input_byte_offset = 0};
    failure.status = SXML_STATUS_MALFORMED_INPUT;
    failure.description = SXML_XML_TEXT(
        "an external entity's bytes are not well-formed in its encoding "
        "(XML 1.0, section 4.3.3)");
    switch (detection.encoding) {
    case SXML_XML_ENCODING_UTF8: {
        sxml_utf8_validation validation = {
            .valid_byte_count = 0, .codepoint_count = 0, .is_ascii = false};
        status = sxml_utf8_validate(entity->content, &validation);
        if (status != SXML_STATUS_OK) {
            failure.status = SXML_STATUS_INVALID_UTF8;
            return sxml_xml_fail(document, failure);
        }
        status = sxml_xml_buffer_append(text, entity->content);
        break;
    }
    case SXML_XML_ENCODING_US_ASCII:
        status = sxml_xml_check_ascii(entity->content, &transcoding_failure);
        if (status != SXML_STATUS_OK) {
            return sxml_xml_fail(document, failure);
        }
        status = sxml_xml_buffer_append(text, entity->content);
        break;
    case SXML_XML_ENCODING_UTF16_LITTLE_ENDIAN:
    case SXML_XML_ENCODING_UTF16_BIG_ENDIAN:
    case SXML_XML_ENCODING_ISO_8859_1:
        status = sxml_xml_transcode(
            &(sxml_xml_transcoding_request){.input = entity->content,
                                            .encoding = detection.encoding},
            text, &transcoding_failure);
        if (status == SXML_STATUS_INVALID_UTF8) {
            failure.status = status;
            return sxml_xml_fail(document, failure);
        }
        break;
    }
    if (status != SXML_STATUS_OK) {
        return sxml_xml_fail_resource(document, status);
    }
    static const uint8_t utf8_mark[] = {0xEF, 0xBB, 0xBF};
    if (text->byte_count >= sizeof utf8_mark &&
        memcmp(text->bytes, utf8_mark, sizeof utf8_mark) == 0) {
        memmove(text->bytes, text->bytes + sizeof utf8_mark,
                text->byte_count - sizeof utf8_mark);
        text->byte_count -= sizeof utf8_mark;
    }
    const sxml_byte_view decoded = {.bytes = text->bytes,
                                    .byte_count = text->byte_count};
    if (entity->encoding_name.byte_count == 0 &&
        !sxml_xml_encoding_is_consistent(&(sxml_xml_encoding_check){
            .detection = &detection,
            .encoding_name =
                sxml_xml_read_declared_encoding(decoded).encoding_name})) {
        failure.description = SXML_XML_TEXT(
            "the encoding declaration of an external entity contradicts the "
            "encoding of its bytes (XML 1.0, section 4.3.3 and Appendix F)");
        return sxml_xml_fail(document, failure);
    }
    if (decoded.byte_count != 0 &&
        memchr(decoded.bytes, 0, decoded.byte_count) != NULL) {
        failure.description =
            SXML_XML_TEXT("U+0000 is not a character of XML ([2] Char)");
        return sxml_xml_fail(document, failure);
    }
    if (document->version == SXML_XML_VERSION_1_0 &&
        sxml_xml_read_declared_version(decoded).version ==
            SXML_XML_VERSION_1_1) {
        failure.description = SXML_XML_TEXT(
            "an external entity of an XML 1.0 document must not declare "
            "version 1.1 (XML 1.1, section 4.3.4)");
        return sxml_xml_fail(document, failure);
    }
    if (document->version == SXML_XML_VERSION_1_1) {
        if (sxml_xml_find_restricted_character(decoded) !=
            decoded.byte_count) {
            failure.description = SXML_XML_TEXT(
                "a control character must be written as a character "
                "reference in XML 1.1, also in the external entities of a 1.1 "
                "document ([2a] RestrictedChar, XML 1.1, sections 2.2 and "
                "4.3.4)");
            return sxml_xml_fail(document, failure);
        }
        sxml_xml_translate_version_1_1_line_ends(
            text, sxml_xml_read_declared_encoding(decoded)
                      .declaration_end_offset);
    }
    sxml_xml_normalize_line_ends(text);
    if (text->byte_count > UINT32_MAX - 64) {
        return sxml_xml_fail_resource(document, SXML_STATUS_LIMIT_EXCEEDED);
    }
    return SXML_STATUS_OK;
}

/* An external entity to read, what for, and where it is referenced. */
typedef struct sxml_xml_external_load {
    uint32_t entity_index;
    sxml_xml_external_entity_purpose purpose;
    sxml_source_location location;
} sxml_xml_external_load;

/*
 * Reads the external entity at load->entity_index through the request's
 * entity resolver, once per parse (4.2.2): its system identifier resolved
 * against the base URI of its declaration, its text decoded. Without a
 * resolver, or when the resolver does not deliver it, the entity is
 * unavailable and not read (4.4.3, 5.1).
 */
static sxml_status
sxml_xml_load_external_entity(sxml_xml_document *document,
                              const sxml_xml_external_load *load) {
    sxml_xml_parser *parser = document->parser;
    sxml_xml_dtd *dtd = &parser->dtd;
    sxml_xml_entity *entity = sxml_xml_dtd_get_entity(dtd, load->entity_index);
    if (entity->load_state != SXML_XML_LOAD_STATE_NOT_LOADED) {
        return SXML_STATUS_OK;
    }
    entity->load_state = SXML_XML_LOAD_STATE_UNAVAILABLE;
    if (!document->reads_external_entities) {
        return SXML_STATUS_OK;
    }
    const sxml_allocator allocator = parser->configuration.allocator;
    const sxml_byte_view base_uri = sxml_xml_dtd_text(dtd, entity->base_uri);
    const sxml_byte_view system_identifier =
        sxml_xml_dtd_text(dtd, entity->system_identifier);
    sxml_uri_resolution resolution = {.uri = {.bytes = NULL, .byte_count = 0},
                                      .memory = NULL};
    sxml_status status = sxml_uri_resolve(
        &(sxml_uri_resolution_request){.base_uri = base_uri,
                                       .reference = system_identifier,
                                       .allocator = allocator},
        &resolution);
    if (status != SXML_STATUS_OK) {
        return sxml_xml_fail_resource(document, status);
    }
    const sxml_byte_view absolute_uri =
        base_uri.byte_count != 0 || sxml_uri_has_scheme(system_identifier)
            ? resolution.uri
            : (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    const sxml_xml_entity_resolver resolver =
        document->request->entity_resolver;
    sxml_xml_external_entity delivered;
    memset(&delivered, 0, sizeof delivered);
    status = resolver.operations->resolve(
        resolver.state,
        &(sxml_xml_external_entity_request){
            .purpose = load->purpose,
            .entity_name =
                load->purpose == SXML_XML_EXTERNAL_ENTITY_PURPOSE_SUBSET
                    ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
                    : sxml_xml_dtd_text(dtd, entity->entity_name),
            .system_identifier = system_identifier,
            .public_identifier = sxml_xml_dtd_text(dtd, entity->public_identifier),
            .absolute_uri = absolute_uri},
        &delivered);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, resolution.memory);
        return status;
    }
    if (delivered.is_available) {
        sxml_xml_buffer text = sxml_xml_buffer_empty(allocator);
        status = sxml_xml_decode_external(
            document,
            &(sxml_xml_external_decoding){.entity = &delivered,
                                          .location = load->location},
            &text);
        const sxml_byte_view entity_uri =
            delivered.entity_uri.byte_count != 0 ? delivered.entity_uri
            : absolute_uri.byte_count != 0       ? absolute_uri
                                                 : system_identifier;
        sxml_byte_range entity_uri_range = {.byte_offset = 0, .byte_count = 0};
        const uint8_t *dtd_start = dtd->text.bytes;
        if (status == SXML_STATUS_OK && entity_uri.byte_count != 0 &&
            dtd_start != NULL && entity_uri.bytes >= dtd_start &&
            entity_uri.bytes < dtd_start + dtd->text.byte_count) {
            /* The system identifier itself, already in the DTD text: storing
               it again would read from the buffer while it grows. */
            entity_uri_range = (sxml_byte_range){
                .byte_offset = (size_t)(entity_uri.bytes - dtd_start),
                .byte_count = entity_uri.byte_count};
        } else if (status == SXML_STATUS_OK) {
            status = sxml_xml_fail_resource(
                document,
                sxml_xml_dtd_store_text(dtd, entity_uri, &entity_uri_range));
        }
        entity = sxml_xml_dtd_get_entity(dtd, load->entity_index);
        if (status == SXML_STATUS_OK) {
            entity->external_text = text;
            entity->entity_uri = entity_uri_range;
            entity->content_offset =
                sxml_xml_read_declared_encoding(
                    (sxml_byte_view){.bytes = text.bytes,
                                     .byte_count = text.byte_count})
                    .declaration_end_offset;
            entity->load_state = SXML_XML_LOAD_STATE_LOADED;
        } else {
            sxml_xml_buffer_release(&text);
        }
    }
    if (resolver.operations->release != NULL) {
        resolver.operations->release(resolver.state, &delivered);
    }
    sxml_allocator_release(allocator, resolution.memory);
    return status;
}

/* The text an entity stands for: an internal entity's replacement text,
   or a loaded external entity's text, from its start or after its text
   declaration (4.3.1: the declaration is not part of the replacement
   text). */
typedef struct sxml_xml_entity_text_request {
    uint32_t entity_index;
    bool skips_text_declaration;
} sxml_xml_entity_text_request;

static sxml_byte_view
sxml_xml_entity_text(const sxml_xml_document *document,
                     sxml_xml_entity_text_request request) {
    const sxml_xml_dtd *dtd = &document->parser->dtd;
    const sxml_xml_entity *entity =
        sxml_xml_dtd_get_entity(dtd, request.entity_index);
    if (entity->entity_kind == SXML_XML_ENTITY_KIND_INTERNAL) {
        return sxml_xml_dtd_text(dtd, entity->replacement_text);
    }
    if (entity->load_state != SXML_XML_LOAD_STATE_LOADED) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    const size_t start =
        request.skips_text_declaration ? entity->content_offset : 0;
    return sxml_xml_view_from_text(
        (sxml_byte_view){.bytes = entity->external_text.bytes,
                         .byte_count = entity->external_text.byte_count},
        start, entity->external_text.byte_count - start);
}

/* Releases what loading external entities kept. */
static void sxml_xml_release_external_texts(sxml_xml_document *document) {
    sxml_xml_dtd *dtd = &document->parser->dtd;
    for (size_t entity_index = 0; entity_index < dtd->entities.element_count;
         entity_index++) {
        sxml_xml_entity *entity =
            sxml_xml_dtd_get_entity(dtd, (uint32_t)entity_index);
        if (entity->load_state == SXML_XML_LOAD_STATE_LOADED) {
            sxml_xml_buffer_release(&entity->external_text);
        }
        entity->load_state = SXML_XML_LOAD_STATE_NOT_LOADED;
    }
}

/* ------------------------------------------------------------ Events */

static bool sxml_xml_has_builder(const sxml_xml_document *document) {
    return document->builder.operations != NULL;
}

/* An empty location, for events of check mode, which are not sent. */
static sxml_source_location sxml_xml_event_location(sxml_xml_document *document,
                                                    const sxml_xml_frame *frame,
                                                    TSNode node) {
    if (!sxml_xml_has_builder(document)) {
        return (sxml_source_location){
            .source_name = {.bytes = NULL, .byte_count = 0},
            .line_number = 0,
            .column_number = 0,
            .byte_offset = 0};
    }
    return sxml_xml_node_location(document, frame, node);
}

static sxml_status sxml_xml_send_text(sxml_xml_document *document,
                                      const sxml_text_event *event) {
    if (!sxml_xml_has_builder(document) || event->text.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_xml_array *open_contents = &document->parser->open_contents;
    if (open_contents->element_count != 0) {
        const sxml_xml_open_content *open =
            sxml_xml_array_at(open_contents, open_contents->element_count - 1);
        if (open->is_declared &&
            open->content_kind == SXML_XML_CONTENT_KIND_CHILDREN &&
            sxml_xml_text_is_whitespace(event->text)) {
            /* White space in element content, [element content
               whitespace]: no text node (XDM 4.0, 7.5.7.3). */
            return SXML_STATUS_OK;
        }
    }
    return document->builder.operations->add_text(document->builder.state,
                                                  event);
}

/*
 * Sends text, normalizing line ends in the document itself (2.11) by
 * sending the pieces between carriage returns: the builder joins adjacent
 * text.
 */
static sxml_status sxml_xml_send_character_data(sxml_xml_document *document,
                                                const sxml_xml_frame *frame,
                                                TSNode node) {
    if (!sxml_xml_has_builder(document)) {
        return SXML_STATUS_OK;
    }
    const sxml_byte_view text = sxml_xml_node_text(frame, node);
    const sxml_source_location location =
        sxml_xml_node_location(document, frame, node);
    if (frame->frame_kind != SXML_XML_FRAME_KIND_DOCUMENT ||
        !sxml_xml_view_contains_byte(text, '\r')) {
        return sxml_xml_send_text(
            document, &(sxml_text_event){.text = text, .location = location});
    }
    static const uint8_t line_feed[] = {'\n'};
    size_t piece_start = 0;
    for (size_t byte_index = 0; byte_index <= text.byte_count; byte_index++) {
        if (byte_index < text.byte_count && text.bytes[byte_index] != '\r') {
            continue;
        }
        sxml_status status = sxml_xml_send_text(
            document,
            &(sxml_text_event){.text = sxml_xml_view_from_text(
                                   text, piece_start, byte_index - piece_start),
                               .location = location});
        if (status == SXML_STATUS_OK && byte_index < text.byte_count) {
            status = sxml_xml_send_text(
                document, &(sxml_text_event){
                              .text = {.bytes = line_feed, .byte_count = 1},
                              .location = location});
            if (byte_index + 1 < text.byte_count &&
                text.bytes[byte_index + 1] == '\n') {
                byte_index += 1;
            }
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        piece_start = byte_index + 1;
    }
    return SXML_STATUS_OK;
}

static sxml_status
sxml_xml_visit_character_reference(sxml_xml_document *document,
                                   const sxml_xml_frame *frame, TSNode node) {
    uint32_t codepoint = 0;
    if (!sxml_xml_decode_character_reference(document, sxml_xml_node_text(frame, node),
                                             &codepoint)) {
        return sxml_xml_fail_at(
            document,
            (sxml_xml_node_failure){
                .frame = frame,
                .node = node,
                .description = SXML_XML_TEXT(
                    "WFC: Legal Character: a character reference must "
                    "denote a character (XML 1.0, section 4.1)")});
    }
    if (!sxml_xml_has_builder(document)) {
        return SXML_STATUS_OK;
    }
    sxml_xml_buffer *scratch = &document->parser->scratch_text;
    scratch->byte_count = 0;
    sxml_status status = sxml_xml_buffer_append_codepoint(scratch, codepoint);
    if (status != SXML_STATUS_OK) {
        return sxml_xml_fail_resource(document, status);
    }
    return sxml_xml_send_text(
        document,
        &(sxml_text_event){.text = {.bytes = scratch->bytes,
                                    .byte_count = scratch->byte_count},
                           .location =
                               sxml_xml_node_location(document, frame, node)});
}

static sxml_status sxml_xml_visit_cdata_section(sxml_xml_document *document,
                                                const sxml_xml_frame *frame,
                                                TSNode node) {
    sxml_xml_children children = {.first_index = 0, .child_count = 0};
    sxml_status status = sxml_xml_list_children(document, node, &children);
    if (status != SXML_STATUS_OK) {
        return sxml_xml_fail_resource(document, status);
    }
    for (size_t child_index = 0;
         status == SXML_STATUS_OK && child_index < children.child_count;
         child_index++) {
        const TSNode child = sxml_xml_child_at(document, children, child_index);
        if (sxml_xml_type_of(child) == SXML_XML_NODE_CDATA_TEXT) {
            status = sxml_xml_send_character_data(document, frame, child);
        }
    }
    sxml_xml_release_children(document, children);
    return status;
}

/* The text of node with line ends normalized, in the parser's scratch
   text when they change it; empty for a null node. */
typedef struct sxml_xml_frame_node {
    const sxml_xml_frame *frame;
    TSNode node;
} sxml_xml_frame_node;

static sxml_status sxml_xml_normalized_text(sxml_xml_document *document,
                                            sxml_xml_frame_node source,
                                            sxml_byte_view *text) {
    const sxml_xml_frame *frame = source.frame;
    const TSNode node = source.node;
    const sxml_byte_view raw =
        ts_node_is_null(node) ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
                              : sxml_xml_node_text(frame, node);
    if (frame->frame_kind != SXML_XML_FRAME_KIND_DOCUMENT ||
        !sxml_xml_view_contains_byte(raw, '\r')) {
        *text = raw;
        return SXML_STATUS_OK;
    }
    sxml_xml_buffer *scratch = &document->parser->scratch_text;
    scratch->byte_count = 0;
    sxml_status status = sxml_xml_append_text(
        scratch,
        (sxml_xml_text_append){.text = raw, .normalizes_line_ends = true});
    if (status != SXML_STATUS_OK) {
        return sxml_xml_fail_resource(document, status);
    }
    *text = (sxml_byte_view){.bytes = scratch->bytes,
                             .byte_count = scratch->byte_count};
    return SXML_STATUS_OK;
}

static sxml_status sxml_xml_visit_comment(sxml_xml_document *document,
                                          const sxml_xml_frame *frame,
                                          TSNode node) {
    if (!sxml_xml_has_builder(document)) {
        return SXML_STATUS_OK;
    }
    TSNode text_node = {.context = {0, 0, 0, 0}, .id = NULL, .tree = NULL};
    if (ts_node_named_child_count(node) != 0) {
        text_node = ts_node_named_child(node, 0);
    }
    sxml_byte_view comment = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_xml_normalized_text(
        document, (sxml_xml_frame_node){.frame = frame, .node = text_node},
        &comment);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return document->builder.operations->add_comment(
        document->builder.state,
        &(sxml_comment_event){
            .comment = comment,
            .location = sxml_xml_node_location(document, frame, node)});
}

static sxml_status sxml_xml_visit_processing_instruction(
    sxml_xml_document *document, const sxml_xml_frame *frame, TSNode node) {
    const TSNode target = sxml_xml_field(node, SXML_XML_FIELD_TARGET);
    sxml_status status = sxml_xml_check_no_colon(document, frame, target);
    if (status != SXML_STATUS_OK || !sxml_xml_has_builder(document)) {
        return status;
    }
    sxml_byte_view data = {.bytes = NULL, .byte_count = 0};
    status = sxml_xml_normalized_text(
        document,
        (sxml_xml_frame_node){.frame = frame,
                              .node =
                                  sxml_xml_field(node, SXML_XML_FIELD_CONTENT)},
        &data);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return document->builder.operations->add_processing_instruction(
        document->builder.state,
        &(sxml_processing_instruction_event){
            .target = sxml_xml_node_text(frame, target),
            .data = data,
            .location = sxml_xml_node_location(document, frame, node)});
}

/* ------------------------------------------------- Attribute values */

static const sxml_byte_view sxml_xml_legal_character_description =
    SXML_BYTE_VIEW_INITIALIZER(
        "WFC: Legal Character: a character reference must denote a character "
        "(XML 1.0, section 4.1)");
static const sxml_byte_view sxml_xml_entity_declared_description =
    SXML_BYTE_VIEW_INITIALIZER(
        "WFC: Entity Declared: the entity is not declared (XML 1.0, section "
        "4.1)");
static const sxml_byte_view sxml_xml_parsed_entity_description =
    SXML_BYTE_VIEW_INITIALIZER(
        "WFC: Parsed Entity: an entity reference must not name an unparsed "
        "entity (XML 1.0, section 4.1)");
static const sxml_byte_view sxml_xml_no_recursion_description =
    SXML_BYTE_VIEW_INITIALIZER(
        "WFC: No Recursion: an entity must not refer to itself (XML 1.0, "
        "section 4.1)");
static const sxml_byte_view sxml_xml_no_external_reference_description =
    SXML_BYTE_VIEW_INITIALIZER(
        "WFC: No External Entity References: attribute values must not refer "
        "to external entities (XML 1.0, section 3.1)");
static const sxml_byte_view sxml_xml_no_less_than_description =
    SXML_BYTE_VIEW_INITIALIZER(
        "WFC: No < in Attribute Values: the replacement text of an entity in "
        "an attribute value must not contain '<' (XML 1.0, section 3.1)");

/* An entity reference met in an attribute value and where it is. */
typedef struct sxml_xml_value_reference {
    sxml_byte_view entity_name;
    sxml_source_location location;
} sxml_xml_value_reference;

/* What to do with a general entity reference after the checks of its
   kind: skip it, or expand the entity at entity_index. */
typedef struct sxml_xml_reference_outcome {
    bool expands;
    uint32_t entity_index;
} sxml_xml_reference_outcome;

/*
 * The checks every reference to a general entity gets (4.1, 4.4): declared
 * (or skipped when the constraint does not apply), parsed, not recursive.
 * External parsed entities are read in content when the request has an
 * entity resolver and skipped otherwise (4.4.3); in an attribute value
 * they are an error (3.1).
 */
typedef struct sxml_xml_reference_check {
    sxml_xml_value_reference reference;
    bool is_in_attribute_value;
} sxml_xml_reference_check;

static sxml_status
sxml_xml_check_reference(sxml_xml_document *document,
                         const sxml_xml_reference_check *check,
                         sxml_xml_reference_outcome *outcome) {
    const sxml_xml_lookup lookup = sxml_xml_dtd_find_entity(
        &document->parser->dtd,
        (sxml_xml_entity_key){.entity_name = check->reference.entity_name,
                              .is_parameter_entity = false});
    sxml_xml_failure failure = {.status = SXML_STATUS_MALFORMED_INPUT,
                                .description = {.bytes = NULL, .byte_count = 0},
                                .location = check->reference.location};
    *outcome =
        (sxml_xml_reference_outcome){.expands = false, .entity_index = 0};
    /* The declaration must not be in the external subset or a parameter
       entity where the constraint applies (WFC: Entity Declared, 4.1). */
    if (!lookup.is_found ||
        (document->requires_declared_entities &&
         sxml_xml_dtd_get_entity(&document->parser->dtd, lookup.entry_index)
             ->is_declared_in_external_markup)) {
        if (!document->requires_declared_entities) {
            /* Then only VC: Entity Declared (4.1). */
            return sxml_xml_report_validity(
                document,
                (sxml_xml_validity_failure){
                    .description = SXML_XML_TEXT(
                        "VC: Entity Declared: the entity is not declared (XML "
                        "1.0, section 4.1)"),
                    .location = check->reference.location});
        }
        failure.description = sxml_xml_entity_declared_description;
        return sxml_xml_fail(document, failure);
    }
    const sxml_xml_entity *entity =
        sxml_xml_dtd_get_entity(&document->parser->dtd, lookup.entry_index);
    switch (entity->entity_kind) {
    case SXML_XML_ENTITY_KIND_UNPARSED:
        failure.description = sxml_xml_parsed_entity_description;
        return sxml_xml_fail(document, failure);
    case SXML_XML_ENTITY_KIND_EXTERNAL_PARSED:
        if (check->is_in_attribute_value) {
            failure.description = sxml_xml_no_external_reference_description;
            return sxml_xml_fail(document, failure);
        }
        if (!document->reads_external_entities) {
            return SXML_STATUS_OK;
        }
        break;
    case SXML_XML_ENTITY_KIND_INTERNAL:
        break;
    }
    if (entity->is_being_expanded) {
        failure.description = sxml_xml_no_recursion_description;
        return sxml_xml_fail(document, failure);
    }
    sxml_status status = SXML_STATUS_OK;
    if (entity->entity_kind == SXML_XML_ENTITY_KIND_EXTERNAL_PARSED) {
        status = sxml_xml_load_external_entity(
            document,
            &(sxml_xml_external_load){
                .entity_index = lookup.entry_index,
                .purpose = SXML_XML_EXTERNAL_ENTITY_PURPOSE_GENERAL_ENTITY,
                .location = check->reference.location});
        entity =
            sxml_xml_dtd_get_entity(&document->parser->dtd, lookup.entry_index);
        if (status != SXML_STATUS_OK ||
            entity->load_state != SXML_XML_LOAD_STATE_LOADED) {
            return status;
        }
    }
    status = sxml_xml_count_expansion(
        document,
        (sxml_xml_expansion_count){
            .byte_count =
                sxml_xml_entity_text(document,
                                     (sxml_xml_entity_text_request){
                                         .entity_index = lookup.entry_index,
                                         .skips_text_declaration = false})
                    .byte_count,
            .location = check->reference.location});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *outcome = (sxml_xml_reference_outcome){.expands = true,
                                            .entity_index = lookup.entry_index};
    return SXML_STATUS_OK;
}

/* Reads a reference at offset in replacement text (4.4.5): its kind and
   extent. Malformed references are not well-formed. */
typedef struct sxml_xml_text_reference {
    bool is_well_formed;
    bool is_character_reference;
    sxml_byte_view reference_text;
    sxml_byte_view entity_name;
} sxml_xml_text_reference;

static sxml_xml_text_reference
sxml_xml_read_text_reference(sxml_byte_view text, size_t byte_offset) {
    sxml_xml_text_reference reference = {
        .is_well_formed = false,
        .is_character_reference = false,
        .reference_text = {.bytes = NULL, .byte_count = 0},
        .entity_name = {.bytes = NULL, .byte_count = 0},
    };
    size_t end = byte_offset + 1;
    while (end < text.byte_count && text.bytes[end] != ';') {
        end += 1;
    }
    if (end >= text.byte_count) {
        return reference;
    }
    reference.reference_text =
        sxml_xml_view_from_text(text, byte_offset, end + 1 - byte_offset);
    if (byte_offset + 1 < end && text.bytes[byte_offset + 1] == '#') {
        reference.is_character_reference = true;
        reference.is_well_formed = true;
        return reference;
    }
    reference.entity_name =
        sxml_xml_view_from_text(text, byte_offset + 1, end - byte_offset - 1);
    reference.is_well_formed = sxml_xml_text_is_name(reference.entity_name);
    return reference;
}

/*
 * Appends the replacement text of the entity at entity_index to the
 * attribute values, processed as an attribute value (XML 1.0, 3.3.3 and
 * 4.4.5): references inside it are recognized and expanded, white space
 * becomes a space, '<' is an error. Iterative, with an explicit stack.
 */
typedef struct sxml_xml_value_expansion {
    uint32_t entity_index;
    sxml_source_location location;
} sxml_xml_value_expansion;

static sxml_status
sxml_xml_expand_in_value(sxml_xml_document *document,
                         const sxml_xml_value_expansion *expansion) {
    sxml_xml_parser *parser = document->parser;
    sxml_xml_array *stack = &parser->expansions;
    const size_t base_count = stack->element_count;
    sxml_xml_failure failure = {.status = SXML_STATUS_MALFORMED_INPUT,
                                .description = {.bytes = NULL, .byte_count = 0},
                                .location = expansion->location};
    void *element = NULL;
    sxml_status status = sxml_xml_array_push(stack, &element);
    if (status != SXML_STATUS_OK) {
        return sxml_xml_fail_resource(document, status);
    }
    *(sxml_xml_expansion *)element = (sxml_xml_expansion){
        .entity_index = expansion->entity_index,
        .byte_offset = 0,
        .inclusion_number = 0};
    sxml_xml_dtd_get_entity(&parser->dtd, expansion->entity_index)
        ->is_being_expanded = true;
    while (status == SXML_STATUS_OK && stack->element_count > base_count) {
        sxml_xml_expansion *top =
            sxml_xml_array_at(stack, stack->element_count - 1);
        sxml_xml_entity *entity =
            sxml_xml_dtd_get_entity(&parser->dtd, top->entity_index);
        const sxml_byte_view text =
            sxml_xml_dtd_text(&parser->dtd, entity->replacement_text);
        if (top->byte_offset >= text.byte_count) {
            entity->is_being_expanded = false;
            stack->element_count -= 1;
            continue;
        }
        const uint8_t byte = text.bytes[top->byte_offset];
        if (byte == '<') {
            failure.description = sxml_xml_no_less_than_description;
            status = sxml_xml_fail(document, failure);
            break;
        }
        if (sxml_xml_byte_is_whitespace(byte)) {
            status =
                sxml_xml_buffer_append_byte(&parser->attribute_values, ' ');
            top->byte_offset += 1;
            continue;
        }
        if (byte != '&') {
            size_t end = top->byte_offset;
            while (end < text.byte_count && text.bytes[end] != '<' &&
                   text.bytes[end] != '&' &&
                   !sxml_xml_byte_is_whitespace(text.bytes[end])) {
                end += 1;
            }
            status = sxml_xml_buffer_append(
                &parser->attribute_values,
                sxml_xml_view_from_text(text, top->byte_offset,
                                        end - top->byte_offset));
            top->byte_offset = end;
            continue;
        }
        const sxml_xml_text_reference reference =
            sxml_xml_read_text_reference(text, top->byte_offset);
        if (!reference.is_well_formed) {
            failure.description = SXML_XML_TEXT(
                "the replacement text of an entity in an attribute value has "
                "a malformed reference ([67] Reference, XML 1.0, section "
                "4.4.5)");
            status = sxml_xml_fail(document, failure);
            break;
        }
        top->byte_offset += reference.reference_text.byte_count;
        if (reference.is_character_reference) {
            uint32_t codepoint = 0;
            if (!sxml_xml_decode_character_reference(document, reference.reference_text,
                                                     &codepoint)) {
                failure.description = SXML_XML_TEXT(
                    "WFC: Legal Character: a character reference must denote "
                    "a character (XML 1.0, section 4.1)");
                status = sxml_xml_fail(document, failure);
                break;
            }
            status = sxml_xml_buffer_append_codepoint(&parser->attribute_values,
                                                      codepoint);
            continue;
        }
        const sxml_xml_predefined_entity predefined =
            sxml_xml_find_predefined_entity(reference.entity_name);
        if (predefined.is_predefined) {
            status = sxml_xml_buffer_append_byte(&parser->attribute_values,
                                                 predefined.character);
            continue;
        }
        sxml_xml_reference_outcome outcome = {.expands = false,
                                              .entity_index = 0};
        status = sxml_xml_check_reference(
            document,
            &(sxml_xml_reference_check){
                .reference = {.entity_name = reference.entity_name,
                              .location = expansion->location},
                .is_in_attribute_value = true},
            &outcome);
        if (status != SXML_STATUS_OK || !outcome.expands) {
            continue;
        }
        status = sxml_xml_array_push(stack, &element);
        if (status == SXML_STATUS_OK) {
            *(sxml_xml_expansion *)element = (sxml_xml_expansion){
                .entity_index = outcome.entity_index,
                .byte_offset = 0,
                .inclusion_number = 0};
            sxml_xml_dtd_get_entity(&parser->dtd, outcome.entity_index)
                ->is_being_expanded = true;
        }
    }
    /* On failure, unwind the marks of the entities still open. */
    while (stack->element_count > base_count) {
        const sxml_xml_expansion *top =
            sxml_xml_array_at(stack, stack->element_count - 1);
        sxml_xml_dtd_get_entity(&parser->dtd, top->entity_index)
            ->is_being_expanded = false;
        stack->element_count -= 1;
    }
    if (status == SXML_STATUS_OUT_OF_MEMORY) {
        return sxml_xml_fail_resource(document, status);
    }
    return status;
}

/*
 * Appends the normalized value of an attribute_value node to the
 * attribute values (XML 1.0, section 3.3.3, before the extra processing
 * of non-CDATA types).
 */
static sxml_status sxml_xml_normalize_value(sxml_xml_document *document,
                                            const sxml_xml_frame *frame,
                                            TSNode value_node) {
    sxml_xml_buffer *values = &document->parser->attribute_values;
    const bool normalizes_line_ends =
        frame->frame_kind == SXML_XML_FRAME_KIND_DOCUMENT;
    sxml_xml_children children = {.first_index = 0, .child_count = 0};
    sxml_status list_status =
        sxml_xml_list_children(document, value_node, &children);
    if (list_status != SXML_STATUS_OK) {
        return sxml_xml_fail_resource(document, list_status);
    }
    for (size_t child_index = 0; child_index < children.child_count;
         child_index++) {
        const TSNode child = sxml_xml_child_at(document, children, child_index);
        const sxml_xml_node_type child_type = sxml_xml_type_of(child);
        sxml_status status = SXML_STATUS_OK;
        if (child_type == SXML_XML_NODE_ATTRIBUTE_VALUE_TEXT) {
            const sxml_byte_view text = sxml_xml_node_text(frame, child);
            status = sxml_xml_buffer_reserve(values, text.byte_count);
            for (size_t byte_index = 0;
                 status == SXML_STATUS_OK && byte_index < text.byte_count;
                 byte_index++) {
                uint8_t byte = text.bytes[byte_index];
                if (sxml_xml_byte_is_whitespace(byte)) {
                    /* CR LF is one line end, so one space (2.11). */
                    if (normalizes_line_ends && byte == '\r' &&
                        byte_index + 1 < text.byte_count &&
                        text.bytes[byte_index + 1] == '\n') {
                        byte_index += 1;
                    }
                    byte = ' ';
                }
                values->bytes[values->byte_count] = byte;
                values->byte_count += 1;
            }
        } else if (child_type == SXML_XML_NODE_CHARACTER_REFERENCE) {
            uint32_t codepoint = 0;
            if (!sxml_xml_decode_character_reference(document, sxml_xml_node_text(frame, child), &codepoint)) {
                return sxml_xml_fail_at(
                    document,
                    (sxml_xml_node_failure){
                        .frame = frame,
                        .node = child,
                        .description = sxml_xml_legal_character_description});
            }
            status = sxml_xml_buffer_append_codepoint(values, codepoint);
        } else if (child_type == SXML_XML_NODE_ENTITY_REFERENCE) {
            const sxml_byte_view name =
                sxml_xml_field_text(frame, child, SXML_XML_FIELD_NAME);
            const sxml_xml_predefined_entity predefined =
                sxml_xml_find_predefined_entity(name);
            if (predefined.is_predefined) {
                status =
                    sxml_xml_buffer_append_byte(values, predefined.character);
            } else {
                const sxml_source_location location =
                    sxml_xml_node_location(document, frame, child);
                sxml_xml_reference_outcome outcome = {.expands = false,
                                                      .entity_index = 0};
                status = sxml_xml_check_reference(
                    document,
                    &(sxml_xml_reference_check){
                        .reference = {.entity_name = name,
                                      .location = location},
                        .is_in_attribute_value = true},
                    &outcome);
                if (status == SXML_STATUS_OK && outcome.expands) {
                    status = sxml_xml_expand_in_value(
                        document, &(sxml_xml_value_expansion){
                                      .entity_index = outcome.entity_index,
                                      .location = location});
                }
                if (status != SXML_STATUS_OK) {
                    return status;
                }
            }
        }
        if (status != SXML_STATUS_OK) {
            return sxml_xml_fail_resource(document, status);
        }
    }
    sxml_xml_release_children(document, children);
    return SXML_STATUS_OK;
}

/*
 * The extra normalization of attributes whose type is not CDATA (3.3.3):
 * no leading or trailing spaces, runs of spaces become one. In place;
 * returns the new byte count.
 */
static size_t sxml_xml_collapse_spaces(sxml_memory_view value) {
    uint8_t *bytes = value.bytes;
    const size_t byte_count = value.byte_count;
    size_t written = 0;
    bool pending_space = false;
    for (size_t byte_index = 0; byte_index < byte_count; byte_index++) {
        if (bytes[byte_index] == ' ') {
            pending_space = written != 0;
            continue;
        }
        if (pending_space) {
            bytes[written] = ' ';
            written += 1;
            pending_space = false;
        }
        bytes[written] = bytes[byte_index];
        written += 1;
    }
    return written;
}

/* ----------------------------------------------- Document type: DTD */

static const sxml_byte_view sxml_xml_pe_between_declarations_description =
    SXML_BYTE_VIEW_INITIALIZER(
        "WFC: PE Between Declarations: the replacement text of a parameter "
        "entity must be whole markup declarations (XML 1.0, section 2.8)");

/*
 * Resolves a parameter-entity reference met in DTD text (4.4.8, 4.4.5):
 * declared, not recursive, read when it is external, its replacement text
 * counted against the expansion limit. An entity that is not declared or
 * not read is not included: then later entity and attribute-list
 * declarations are not processed (5.1); in a standalone document an
 * undeclared one is WFC: Entity Declared (4.1).
 */
typedef struct sxml_xml_parameter_inclusion {
    bool is_included;
    uint32_t entity_index;
} sxml_xml_parameter_inclusion;

static sxml_status
sxml_xml_include_parameter_entity(sxml_xml_document *document,
                                  const sxml_xml_value_reference *reference,
                                  sxml_xml_parameter_inclusion *inclusion) {
    *inclusion =
        (sxml_xml_parameter_inclusion){.is_included = false, .entity_index = 0};
    const sxml_xml_lookup lookup = sxml_xml_dtd_find_entity(
        &document->parser->dtd,
        (sxml_xml_entity_key){.entity_name = reference->entity_name,
                              .is_parameter_entity = true});
    sxml_xml_failure failure = {.status = SXML_STATUS_MALFORMED_INPUT,
                                .description = sxml_xml_entity_declared_description,
                                .location = reference->location};
    if (!lookup.is_found) {
        if (document->is_standalone) {
            return sxml_xml_fail(document, failure);
        }
        document->skips_declarations = true;
        return SXML_STATUS_OK;
    }
    const sxml_xml_entity *entity =
        sxml_xml_dtd_get_entity(&document->parser->dtd, lookup.entry_index);
    if (entity->is_being_expanded) {
        failure.description = sxml_xml_no_recursion_description;
        return sxml_xml_fail(document, failure);
    }
    if (entity->entity_kind != SXML_XML_ENTITY_KIND_INTERNAL) {
        const sxml_status status = sxml_xml_load_external_entity(
            document,
            &(sxml_xml_external_load){
                .entity_index = lookup.entry_index,
                .purpose = SXML_XML_EXTERNAL_ENTITY_PURPOSE_PARAMETER_ENTITY,
                .location = reference->location});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        entity =
            sxml_xml_dtd_get_entity(&document->parser->dtd, lookup.entry_index);
        if (entity->load_state != SXML_XML_LOAD_STATE_LOADED) {
            document->skips_declarations =
                document->skips_declarations || !document->is_standalone;
            return SXML_STATUS_OK;
        }
    }
    const sxml_status status = sxml_xml_count_expansion(
        document,
        (sxml_xml_expansion_count){
            .byte_count =
                sxml_xml_entity_text(document,
                                     (sxml_xml_entity_text_request){
                                         .entity_index = lookup.entry_index,
                                         .skips_text_declaration = false})
                    .byte_count,
            .location = reference->location});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *inclusion = (sxml_xml_parameter_inclusion){
        .is_included = true, .entity_index = lookup.entry_index};
    return SXML_STATUS_OK;
}

/* A [69] PEReference at offset in text: its extent and name. */
typedef struct sxml_xml_parameter_reference {
    bool is_reference;
    size_t byte_count;
    sxml_byte_view entity_name;
} sxml_xml_parameter_reference;

static sxml_xml_parameter_reference
sxml_xml_read_parameter_reference(sxml_byte_view text, size_t byte_offset) {
    sxml_xml_parameter_reference reference = {
        .is_reference = false,
        .byte_count = 0,
        .entity_name = {.bytes = NULL, .byte_count = 0}};
    size_t end = byte_offset + 1;
    while (end < text.byte_count && text.bytes[end] != ';' &&
           !sxml_xml_byte_is_whitespace(text.bytes[end]) &&
           text.bytes[end] != '%' && text.bytes[end] != '"' &&
           text.bytes[end] != '\'') {
        end += 1;
    }
    if (end >= text.byte_count || text.bytes[end] != ';') {
        return reference;
    }
    reference.entity_name =
        sxml_xml_view_from_text(text, byte_offset + 1, end - byte_offset - 1);
    reference.is_reference = sxml_xml_text_is_name(reference.entity_name);
    reference.byte_count = end + 1 - byte_offset;
    return reference;
}

/*
 * Parameter-entity references to expand in DTD text, with an explicit
 * stack of the entities open:
 * - in a declaration of the external subset (4.4.8, Included as PE):
 *   references outside the declaration's literals are replaced by their
 *   replacement text enlarged by one leading and one trailing space;
 * - in an entity value (4.4.5, Included in Literal): the replacement text
 *   is processed as part of the literal, so its parameter-entity and
 *   character references are replaced too and general entity references
 *   kept (bypassed, 4.4.7), with no spaces added.
 * The replacement texts of the entities included are read the same way.
 */
typedef struct sxml_xml_parameter_expansion {
    /* The text to expand: the declaration, or one reference "%name;". */
    sxml_byte_view text;
    bool is_in_literal;
    sxml_source_location location;
    sxml_xml_buffer *output;
} sxml_xml_parameter_expansion;

/* Whether every reference was included; false when one names an entity
   that is not declared or not read (5.1). For a declaration, whether a
   replacement text holds a parenthesis of a group without its partner (VC:
   Proper Group/PE Nesting, 3.2.1) or the declaration's closing '>' (VC:
   Proper Declaration/PE Nesting, 2.8). */
typedef struct sxml_xml_parameter_expansion_result {
    bool is_complete;
    bool breaks_group_nesting;
    bool breaks_declaration_nesting;
} sxml_xml_parameter_expansion_result;

static sxml_status
sxml_xml_expand_parameter_entities(sxml_xml_document *document,
                                   const sxml_xml_parameter_expansion *expansion,
                                   sxml_xml_parameter_expansion_result *result) {
    sxml_xml_parser *parser = document->parser;
    sxml_xml_array *stack = &parser->expansions;
    const size_t base_count = stack->element_count;
    sxml_xml_buffer *output = expansion->output;
    const bool is_in_literal = expansion->is_in_literal;
    sxml_xml_failure failure = {.status = SXML_STATUS_MALFORMED_INPUT,
                                .description = {.bytes = NULL, .byte_count = 0},
                                .location = expansion->location};
    size_t outer_offset = 0;
    uint8_t quote = 0;
    sxml_status status = SXML_STATUS_OK;
    result->is_complete = true;
    result->breaks_group_nesting = false;
    result->breaks_declaration_nesting = false;
    size_t inclusion_count = 0;
    sxml_xml_array *parentheses = &parser->parenthesis_inclusions;
    parentheses->element_count = 0;
    while (status == SXML_STATUS_OK) {
        sxml_xml_expansion *top =
            stack->element_count > base_count
                ? sxml_xml_array_at(stack, stack->element_count - 1)
                : NULL;
        const sxml_byte_view text =
            top == NULL ? expansion->text
                        : sxml_xml_entity_text(
                              document, (sxml_xml_entity_text_request){
                                            .entity_index = top->entity_index,
                                            .skips_text_declaration = true});
        size_t *offset = top == NULL ? &outer_offset : &top->byte_offset;
        if (*offset >= text.byte_count) {
            if (top == NULL) {
                break;
            }
            sxml_xml_dtd_get_entity(&parser->dtd, top->entity_index)
                ->is_being_expanded = false;
            stack->element_count -= 1;
            if (!is_in_literal) {
                status = sxml_xml_buffer_append_byte(output, ' ');
            }
            continue;
        }
        const uint8_t byte = text.bytes[*offset];
        const size_t inclusion_number = top == NULL ? 0 : top->inclusion_number;
        if (!is_in_literal && quote != 0) {
            quote = byte == quote ? 0 : quote;
        } else if (!is_in_literal && (byte == '"' || byte == '\'')) {
            quote = byte;
        } else if (!is_in_literal && byte == '(') {
            void *element = NULL;
            status = sxml_xml_array_push(parentheses, &element);
            if (status == SXML_STATUS_OK) {
                *(size_t *)element = inclusion_number;
            }
        } else if (!is_in_literal && byte == ')' &&
                   parentheses->element_count != 0) {
            parentheses->element_count -= 1;
            result->breaks_group_nesting =
                result->breaks_group_nesting ||
                *(const size_t *)sxml_xml_array_at(parentheses,
                                                   parentheses->element_count) !=
                    inclusion_number;
        } else if (!is_in_literal && byte == '>' && inclusion_number != 0) {
            result->breaks_declaration_nesting = true;
        } else if (byte == '%') {
            const sxml_xml_parameter_reference reference =
                sxml_xml_read_parameter_reference(text, *offset);
            if (reference.is_reference) {
                *offset += reference.byte_count;
                sxml_xml_parameter_inclusion inclusion = {.is_included = false,
                                                          .entity_index = 0};
                status = sxml_xml_include_parameter_entity(
                    document,
                    &(sxml_xml_value_reference){
                        .entity_name = reference.entity_name,
                        .location = expansion->location},
                    &inclusion);
                if (status != SXML_STATUS_OK || !inclusion.is_included) {
                    result->is_complete = false;
                    break;
                }
                void *element = NULL;
                status = sxml_xml_array_push(stack, &element);
                if (status == SXML_STATUS_OK && !is_in_literal) {
                    status = sxml_xml_buffer_append_byte(output, ' ');
                }
                if (status == SXML_STATUS_OK) {
                    inclusion_count += 1;
                    *(sxml_xml_expansion *)element = (sxml_xml_expansion){
                        .entity_index = inclusion.entity_index,
                        .byte_offset = 0,
                        .inclusion_number = inclusion_count};
                    sxml_xml_dtd_get_entity(&parser->dtd, inclusion.entity_index)
                        ->is_being_expanded = true;
                }
                continue;
            }
        } else if (is_in_literal && byte == '&') {
            const sxml_xml_text_reference reference =
                sxml_xml_read_text_reference(text, *offset);
            if (!reference.is_well_formed) {
                failure.description = SXML_XML_TEXT(
                    "the replacement text of a parameter entity in an entity "
                    "value has a malformed reference ([67] Reference, XML 1.0, "
                    "section 4.4.5)");
                status = sxml_xml_fail(document, failure);
                break;
            }
            *offset += reference.reference_text.byte_count;
            if (!reference.is_character_reference) {
                /* Bypassed: kept as written (4.4.7). */
                status = sxml_xml_buffer_append(output, reference.reference_text);
                continue;
            }
            uint32_t codepoint = 0;
            if (!sxml_xml_decode_character_reference(
                    document, reference.reference_text, &codepoint)) {
                failure.description = sxml_xml_legal_character_description;
                status = sxml_xml_fail(document, failure);
                break;
            }
            status = sxml_xml_buffer_append_codepoint(output, codepoint);
            continue;
        }
        status = sxml_xml_buffer_append_byte(output, byte);
        *offset += 1;
    }
    /* Unwind the marks of the entities still open. */
    while (stack->element_count > base_count) {
        const sxml_xml_expansion *top =
            sxml_xml_array_at(stack, stack->element_count - 1);
        sxml_xml_dtd_get_entity(&parser->dtd, top->entity_index)
            ->is_being_expanded = false;
        stack->element_count -= 1;
    }
    if (!is_in_literal && (expansion->text.byte_count == 0 ||
                           expansion->text.bytes[expansion->text.byte_count - 1] !=
                               '>')) {
        /* The scanner ended the declaration before its '>', which an
           entity holds. */
        result->breaks_declaration_nesting = true;
    }
    if (status == SXML_STATUS_OUT_OF_MEMORY ||
        status == SXML_STATUS_ARITHMETIC_OVERFLOW) {
        return sxml_xml_fail_resource(document, SXML_STATUS_OUT_OF_MEMORY);
    }
    return status;
}

/* The replacement text of an entity_value node (4.5), in scratch text. */
static sxml_status sxml_xml_build_replacement_text(sxml_xml_document *document,
                                                   const sxml_xml_frame *frame,
                                                   TSNode value_node) {
    sxml_xml_buffer *scratch = &document->parser->scratch_text;
    scratch->byte_count = 0;
    const bool normalizes_line_ends =
        frame->frame_kind == SXML_XML_FRAME_KIND_DOCUMENT;
    sxml_xml_children children = {.first_index = 0, .child_count = 0};
    sxml_status list_status =
        sxml_xml_list_children(document, value_node, &children);
    if (list_status != SXML_STATUS_OK) {
        return sxml_xml_fail_resource(document, list_status);
    }
    for (size_t child_index = 0; child_index < children.child_count;
         child_index++) {
        const TSNode child = sxml_xml_child_at(document, children, child_index);
        const sxml_xml_node_type child_type = sxml_xml_type_of(child);
        sxml_status status = SXML_STATUS_OK;
        if (child_type == SXML_XML_NODE_ENTITY_VALUE_TEXT) {
            status = sxml_xml_append_text(
                scratch, (sxml_xml_text_append){
                             .text = sxml_xml_node_text(frame, child),
                             .normalizes_line_ends = normalizes_line_ends});
        } else if (child_type == SXML_XML_NODE_CHARACTER_REFERENCE) {
            uint32_t codepoint = 0;
            if (!sxml_xml_decode_character_reference(document, sxml_xml_node_text(frame, child), &codepoint)) {
                return sxml_xml_fail_at(
                    document,
                    (sxml_xml_node_failure){
                        .frame = frame,
                        .node = child,
                        .description = sxml_xml_legal_character_description});
            }
            status = sxml_xml_buffer_append_codepoint(scratch, codepoint);
        } else if (child_type == SXML_XML_NODE_ENTITY_REFERENCE) {
            /* Bypassed: kept as written (4.4.7). */
            status = sxml_xml_buffer_append(scratch,
                                            sxml_xml_node_text(frame, child));
        } else if (child_type == SXML_XML_NODE_PARAMETER_ENTITY_REFERENCE) {
            /* Only in the external subset and external parameter entities,
               the grammar's _external_entity_value (4.4.5). An entity not
               read leaves skips_declarations set: the declaration is then
               not processed (5.1). */
            sxml_xml_parameter_expansion_result result = {.is_complete = true};
            status = sxml_xml_expand_parameter_entities(
                document,
                &(sxml_xml_parameter_expansion){
                    .text = sxml_xml_node_text(frame, child),
                    .is_in_literal = true,
                    .location = sxml_xml_node_location(document, frame, child),
                    .output = scratch},
                &result);
            if (status != SXML_STATUS_OK || !result.is_complete) {
                sxml_xml_release_children(document, children);
                return status;
            }
            continue;
        }
        if (status != SXML_STATUS_OK) {
            return sxml_xml_fail_resource(document, status);
        }
    }
    sxml_xml_release_children(document, children);
    return SXML_STATUS_OK;
}

/* A system or public literal without its quotes ([11], [12]). */
static sxml_byte_view sxml_xml_literal_content(sxml_byte_view literal) {
    if (literal.byte_count < 2) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return sxml_xml_view_from_text(literal, 1, literal.byte_count - 2);
}

/* Stores text in the DTD text, reporting failures. */
static sxml_status sxml_xml_store(sxml_xml_document *document,
                                  sxml_byte_view text, sxml_byte_range *range) {
    return sxml_xml_fail_resource(
        document, sxml_xml_dtd_store_text(&document->parser->dtd, text, range));
}

/* [71] GEDecl and [72] PEDecl. */
static sxml_status sxml_xml_declare_entity(sxml_xml_document *document,
                                           const sxml_xml_frame *frame,
                                           TSNode node) {
    const bool is_parameter_entity =
        sxml_xml_type_of(node) == SXML_XML_NODE_PARAMETER_ENTITY_DECLARATION;
    const TSNode name_node = sxml_xml_field(node, SXML_XML_FIELD_NAME);
    sxml_status status = sxml_xml_check_no_colon(document, frame, name_node);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xml_entity entity = {
        .entity_kind = SXML_XML_ENTITY_KIND_INTERNAL,
        .is_parameter_entity = is_parameter_entity,
        .entity_name = {.byte_offset = 0, .byte_count = 0},
        .replacement_text = {.byte_offset = 0, .byte_count = 0},
        .system_identifier = {.byte_offset = 0, .byte_count = 0},
        .public_identifier = {.byte_offset = 0, .byte_count = 0},
        .notation_name = {.byte_offset = 0, .byte_count = 0},
        .is_declared_in_external_markup =
            frame->frame_kind != SXML_XML_FRAME_KIND_DOCUMENT,
        .base_uri = frame->base_uri,
        .load_state = SXML_XML_LOAD_STATE_NOT_LOADED,
        .external_text =
            sxml_xml_buffer_empty(document->parser->configuration.allocator),
        .content_offset = 0,
        .entity_uri = {.byte_offset = 0, .byte_count = 0},
        .is_being_expanded = false,
        .syntax_tree = NULL,
    };
    const TSNode value = sxml_xml_field(node, SXML_XML_FIELD_VALUE);
    const TSNode notation = sxml_xml_field(node, SXML_XML_FIELD_NOTATION);
    if (!ts_node_is_null(notation)) {
        status = sxml_xml_check_no_colon(document, frame, notation);
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_store(document, sxml_xml_node_text(frame, notation),
                                    &entity.notation_name);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (!ts_node_is_null(value)) {
        /* The replacement text is built even when the declaration loses
           to an earlier one: its character references must be legal. */
        status = sxml_xml_build_replacement_text(document, frame, value);
        if (status == SXML_STATUS_OK && document->skips_declarations) {
            /* A parameter entity in the value was not read (5.1). */
            return SXML_STATUS_OK;
        }
        if (status == SXML_STATUS_OK) {
            sxml_xml_buffer *scratch = &document->parser->scratch_text;
            status = sxml_xml_store(
                document,
                (sxml_byte_view){.bytes = scratch->bytes,
                                 .byte_count = scratch->byte_count},
                &entity.replacement_text);
        }
    } else {
        const TSNode external_id =
            sxml_xml_field(node, SXML_XML_FIELD_EXTERNAL_ID);
        entity.entity_kind = ts_node_is_null(notation)
                                 ? SXML_XML_ENTITY_KIND_EXTERNAL_PARSED
                                 : SXML_XML_ENTITY_KIND_UNPARSED;
        status =
            sxml_xml_store(document,
                           sxml_xml_literal_content(sxml_xml_field_text(
                               frame, external_id, SXML_XML_FIELD_SYSTEM_ID)),
                           &entity.system_identifier);
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_store(
                document,
                sxml_xml_literal_content(sxml_xml_field_text(
                    frame, external_id, SXML_XML_FIELD_PUBLIC_ID)),
                &entity.public_identifier);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_store(document, sxml_xml_node_text(frame, name_node),
                                &entity.entity_name);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xml_lookup added = {.is_found = false, .entry_index = 0};
    return sxml_xml_fail_resource(
        document,
        sxml_xml_dtd_add_entity(&document->parser->dtd, &entity, &added));
}

/* The declared type of an attribute_definition's type node. */
static sxml_xml_attribute_type
sxml_xml_attribute_type_of(const sxml_xml_frame *frame, TSNode type_node) {
    const sxml_xml_node_type node_type = sxml_xml_type_of(type_node);
    if (node_type == SXML_XML_NODE_STRING_TYPE) {
        return SXML_XML_ATTRIBUTE_TYPE_CDATA;
    }
    if (node_type == SXML_XML_NODE_TOKENIZED_TYPE) {
        const sxml_byte_view text = sxml_xml_node_text(frame, type_node);
        if (sxml_byte_view_equals(text, SXML_XML_TEXT("ID"))) {
            return SXML_XML_ATTRIBUTE_TYPE_ID;
        }
        if (sxml_byte_view_equals(text, SXML_XML_TEXT("IDREF"))) {
            return SXML_XML_ATTRIBUTE_TYPE_IDREF;
        }
        if (sxml_byte_view_equals(text, SXML_XML_TEXT("IDREFS"))) {
            return SXML_XML_ATTRIBUTE_TYPE_IDREFS;
        }
        if (sxml_byte_view_equals(text, SXML_XML_TEXT("ENTITY"))) {
            return SXML_XML_ATTRIBUTE_TYPE_ENTITY;
        }
        if (sxml_byte_view_equals(text, SXML_XML_TEXT("ENTITIES"))) {
            return SXML_XML_ATTRIBUTE_TYPE_ENTITIES;
        }
        if (sxml_byte_view_equals(text, SXML_XML_TEXT("NMTOKEN"))) {
            return SXML_XML_ATTRIBUTE_TYPE_NMTOKEN;
        }
        return SXML_XML_ATTRIBUTE_TYPE_NMTOKENS;
    }
    return node_type == SXML_XML_NODE_NOTATION_TYPE
               ? SXML_XML_ATTRIBUTE_TYPE_NOTATION
               : SXML_XML_ATTRIBUTE_TYPE_ENUMERATION;
}

/* The [60] DefaultDecl of a default_declaration node. */
static sxml_xml_default_kind
sxml_xml_default_kind_of(const sxml_xml_frame *frame, TSNode default_node) {
    const sxml_byte_view text = sxml_xml_node_text(frame, default_node);
    if (text.byte_count != 0 && text.bytes[0] == '#') {
        return text.byte_count > 1 && text.bytes[1] == 'R'
                   ? SXML_XML_DEFAULT_KIND_REQUIRED
               : text.byte_count > 1 && text.bytes[1] == 'I'
                   ? SXML_XML_DEFAULT_KIND_IMPLIED
                   : SXML_XML_DEFAULT_KIND_FIXED;
    }
    return SXML_XML_DEFAULT_KIND_VALUE;
}

/* Stores the names of a [58] NotationType or the name tokens of a [59]
   Enumeration, each followed by one space, in the DTD text. */
static sxml_status sxml_xml_store_enumeration(sxml_xml_document *document,
                                              sxml_xml_frame_node type,
                                              sxml_byte_range *range) {
    sxml_xml_dtd *dtd = &document->parser->dtd;
    const size_t start = dtd->text.byte_count;
    sxml_xml_children children = {.first_index = 0, .child_count = 0};
    sxml_status status = sxml_xml_fail_resource(
        document, sxml_xml_list_children(document, type.node, &children));
    for (size_t child_index = 0;
         status == SXML_STATUS_OK && child_index < children.child_count;
         child_index++) {
        status = sxml_xml_buffer_append(
            &dtd->text,
            sxml_xml_node_text(type.frame,
                               sxml_xml_child_at(document, children, child_index)));
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append_byte(&dtd->text, ' ');
        }
        status = sxml_xml_fail_resource(document, status);
    }
    sxml_xml_release_children(document, children);
    *range = (sxml_byte_range){.byte_offset = start,
                               .byte_count = dtd->text.byte_count - start};
    return status;
}

/* One [53] AttDef of an attribute-list declaration for element_name. */
typedef struct sxml_xml_definition_node {
    TSNode node;
    sxml_byte_view element_name;
} sxml_xml_definition_node;

static sxml_status
sxml_xml_declare_attribute(sxml_xml_document *document,
                           const sxml_xml_frame *frame,
                           const sxml_xml_definition_node *definition_node) {
    const TSNode node = definition_node->node;
    const TSNode name_node = sxml_xml_field(node, SXML_XML_FIELD_NAME);
    sxml_status status =
        sxml_xml_check_qualified_name(document, frame, name_node);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const TSNode type_node = sxml_xml_field(node, SXML_XML_FIELD_TYPE);
    sxml_xml_attribute_definition definition = {
        .attribute_name = {.byte_offset = 0, .byte_count = 0},
        .attribute_type = sxml_xml_attribute_type_of(frame, type_node),
        .enumeration = {.byte_offset = 0, .byte_count = 0},
        .default_kind = sxml_xml_default_kind_of(
            frame, sxml_xml_field(node, SXML_XML_FIELD_DEFAULT)),
        .has_default_value = false,
        .default_value = {.byte_offset = 0, .byte_count = 0},
        .is_declared_in_external_markup =
            frame->frame_kind != SXML_XML_FRAME_KIND_DOCUMENT,
        .next_definition = 0,
    };
    if (definition.attribute_type == SXML_XML_ATTRIBUTE_TYPE_NOTATION ||
        definition.attribute_type == SXML_XML_ATTRIBUTE_TYPE_ENUMERATION) {
        status = sxml_xml_store_enumeration(
            document, (sxml_xml_frame_node){.frame = frame, .node = type_node},
            &definition.enumeration);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    const TSNode value = sxml_xml_field(
        sxml_xml_field(node, SXML_XML_FIELD_DEFAULT), SXML_XML_FIELD_VALUE);
    if (!ts_node_is_null(value)) {
        /* The default is normalized now, with the entities declared so far
           (WFC: Entity Declared, 4.1; WFC: No < in Attribute Values, 3.1
           applies to defaults too, 3.3.2). */
        sxml_xml_buffer *values = &document->parser->attribute_values;
        values->byte_count = 0;
        status = sxml_xml_normalize_value(document, frame, value);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (definition.attribute_type != SXML_XML_ATTRIBUTE_TYPE_CDATA) {
            values->byte_count = sxml_xml_collapse_spaces((sxml_memory_view){
                .bytes = values->bytes, .byte_count = values->byte_count});
        }
        definition.has_default_value = true;
        status =
            sxml_xml_store(document,
                           (sxml_byte_view){.bytes = values->bytes,
                                            .byte_count = values->byte_count},
                           &definition.default_value);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_store(document, sxml_xml_node_text(frame, name_node),
                                &definition.attribute_name);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xml_addition addition = {.is_added = false};
    return sxml_xml_fail_resource(
        document, sxml_xml_dtd_add_attribute_definition(
                      &document->parser->dtd,
                      &(sxml_xml_attribute_definition_request){
                          .element_name = definition_node->element_name,
                          .definition = definition},
                      &addition));
}

/* [52] AttlistDecl. */
static sxml_status sxml_xml_declare_attribute_list(sxml_xml_document *document,
                                                   const sxml_xml_frame *frame,
                                                   TSNode node) {
    const TSNode name_node = sxml_xml_field(node, SXML_XML_FIELD_NAME);
    sxml_status status =
        sxml_xml_check_qualified_name(document, frame, name_node);
    sxml_xml_children children = {.first_index = 0, .child_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_fail_resource(
            document, sxml_xml_list_children(document, node, &children));
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    for (size_t child_index = 0;
         status == SXML_STATUS_OK && child_index < children.child_count;
         child_index++) {
        const TSNode child = sxml_xml_child_at(document, children, child_index);
        if (sxml_xml_type_of(child) == SXML_XML_NODE_ATTRIBUTE_DEFINITION) {
            status = sxml_xml_declare_attribute(
                document, frame,
                &(sxml_xml_definition_node){
                    .node = child,
                    .element_name = sxml_xml_node_text(frame, name_node)});
        }
    }
    sxml_xml_release_children(document, children);
    return status;
}

/* The wrapper that makes a parameter entity's replacement text a document
   whose internal subset it is (2.8, WFC: PE Between Declarations). */
static const sxml_byte_view sxml_xml_subset_prefix =
    SXML_BYTE_VIEW_INITIALIZER("<!DOCTYPE d [");
static const sxml_byte_view sxml_xml_subset_suffix =
    SXML_BYTE_VIEW_INITIALIZER("]><d/>");

/* [69] PEReference between declarations ([28a] DeclSep): the
   replacement text is parsed as whole declarations, of the internal subset
   or, in external DTD text and for an external entity, of the external
   subset (2.8, 4.4.8). */
static sxml_status
sxml_xml_reference_parameter_entity(sxml_xml_document *document,
                                    const sxml_xml_frame *frame, TSNode node) {
    const sxml_source_location location =
        sxml_xml_node_location(document, frame, node);
    sxml_xml_parameter_inclusion inclusion = {.is_included = false,
                                              .entity_index = 0};
    sxml_status status = sxml_xml_include_parameter_entity(
        document,
        &(sxml_xml_value_reference){
            .entity_name = sxml_xml_field_text(frame, node, SXML_XML_FIELD_NAME),
            .location = location},
        &inclusion);
    if (status != SXML_STATUS_OK || !inclusion.is_included) {
        return status;
    }
    const sxml_xml_dtd *dtd = &document->parser->dtd;
    const sxml_xml_entity *entity =
        sxml_xml_dtd_get_entity(dtd, inclusion.entity_index);
    const bool is_external =
        entity->entity_kind != SXML_XML_ENTITY_KIND_INTERNAL;
    const bool is_external_context = is_external || frame->is_external_context;
    const sxml_byte_view prefix =
        is_external_context ? sxml_xml_external_subset_marker
                            : sxml_xml_subset_prefix;
    sxml_xml_frame pushed = {
        .frame_kind = SXML_XML_FRAME_KIND_PARAMETER_ENTITY,
        .syntax_tree = NULL,
        .cursor = {.tree = NULL, .id = NULL, .context = {0, 0, 0}},
        .text = {.bytes = NULL, .byte_count = 0},
        .prefix_byte_count = (uint32_t)prefix.byte_count,
        .owned_text =
            sxml_xml_buffer_empty(document->parser->configuration.allocator),
        .has_entity = true,
        .entity_index = inclusion.entity_index,
        .reference_location = frame->frame_kind == SXML_XML_FRAME_KIND_DOCUMENT
                                  ? location
                                  : frame->reference_location,
        .is_external_context = is_external_context,
        .starts_external_entity = is_external,
        .base_uri = is_external ? entity->entity_uri : frame->base_uri,
        .needs_advance = false,
        .descends = false,
        .cursor_depth = 0,
    };
    if (is_external) {
        /* The entity's own text is kept until the parse ends. */
        pushed.text = sxml_xml_entity_text(
            document, (sxml_xml_entity_text_request){
                          .entity_index = inclusion.entity_index,
                          .skips_text_declaration = false});
    } else {
        /* A copy: declarations in it grow the DTD text it lives in. */
        status = sxml_xml_buffer_append(
            &pushed.owned_text, sxml_xml_dtd_text(dtd, entity->replacement_text));
        if (status != SXML_STATUS_OK) {
            sxml_xml_buffer_release(&pushed.owned_text);
            return sxml_xml_fail_resource(document, status);
        }
        pushed.text = (sxml_byte_view){.bytes = pushed.owned_text.bytes,
                                       .byte_count = pushed.owned_text.byte_count};
    }
    const sxml_xml_wrapped_text wrapped = {
        .prefix = prefix,
        .text = pushed.text,
        .suffix = is_external_context
                      ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
                      : sxml_xml_subset_suffix};
    status = sxml_xml_parse_wrapped(document, &wrapped, &pushed.syntax_tree);
    if (status == SXML_STATUS_OK && pushed.syntax_tree == NULL) {
        status = sxml_xml_fail(
            document,
            (sxml_xml_failure){
                .status = SXML_STATUS_MALFORMED_INPUT,
                .description = sxml_xml_pe_between_declarations_description,
                .location = pushed.reference_location});
    }
    if (status != SXML_STATUS_OK) {
        sxml_xml_buffer_release(&pushed.owned_text);
        return status;
    }
    const TSNode tree_root = ts_tree_root_node(pushed.syntax_tree);
    const TSNode subset =
        is_external_context
            ? sxml_xml_field(tree_root, SXML_XML_FIELD_EXTERNAL_SUBSET)
            : sxml_xml_field(sxml_xml_field(tree_root, SXML_XML_FIELD_DOCTYPE),
                             SXML_XML_FIELD_INTERNAL_SUBSET);
    if (ts_node_is_null(subset)) {
        ts_tree_delete(pushed.syntax_tree);
        sxml_xml_buffer_release(&pushed.owned_text);
        return SXML_STATUS_OK;
    }
    return sxml_xml_push_frame(document, &pushed, subset);
}

/* DTD text to read as the external subset: an expanded declaration or the
   contents of a conditional section, in a frame of its own. */
typedef struct sxml_xml_external_text {
    /* The text, or empty when owned_text holds it. */
    sxml_byte_view text;
    sxml_xml_buffer owned_text;
    sxml_source_location location;
    /* Static text naming the production the text must match. */
    sxml_byte_view description;
} sxml_xml_external_text;

/* Parses external DTD text and pushes a frame over it. Takes ownership of
   source->owned_text even on failure. */
static sxml_status
sxml_xml_push_external_text(sxml_xml_document *document,
                            const sxml_xml_frame *frame,
                            sxml_xml_external_text *source) {
    sxml_xml_frame pushed = {
        .frame_kind = SXML_XML_FRAME_KIND_PARAMETER_ENTITY,
        .syntax_tree = NULL,
        .cursor = {.tree = NULL, .id = NULL, .context = {0, 0, 0}},
        .text = source->owned_text.byte_count != 0
                    ? (sxml_byte_view){.bytes = source->owned_text.bytes,
                                       .byte_count = source->owned_text.byte_count}
                    : source->text,
        .prefix_byte_count =
            (uint32_t)sxml_xml_external_subset_marker.byte_count,
        .owned_text = source->owned_text,
        .has_entity = false,
        .entity_index = 0,
        .reference_location = frame->frame_kind == SXML_XML_FRAME_KIND_DOCUMENT
                                  ? source->location
                                  : frame->reference_location,
        .is_external_context = true,
        .starts_external_entity = false,
        .base_uri = frame->base_uri,
        .needs_advance = false,
        .descends = false,
        .cursor_depth = 0,
    };
    const sxml_xml_wrapped_text wrapped = {
        .prefix = sxml_xml_external_subset_marker,
        .text = pushed.text,
        .suffix = {.bytes = NULL, .byte_count = 0}};
    sxml_status status =
        sxml_xml_parse_wrapped(document, &wrapped, &pushed.syntax_tree);
    if (status == SXML_STATUS_OK && pushed.syntax_tree == NULL) {
        status = sxml_xml_fail(
            document, (sxml_xml_failure){.status = SXML_STATUS_MALFORMED_INPUT,
                                         .description = source->description,
                                         .location = pushed.reference_location});
    }
    if (status != SXML_STATUS_OK) {
        sxml_xml_buffer_release(&pushed.owned_text);
        return status;
    }
    const TSNode subset = sxml_xml_field(ts_tree_root_node(pushed.syntax_tree),
                                         SXML_XML_FIELD_EXTERNAL_SUBSET);
    if (ts_node_is_null(subset)) {
        ts_tree_delete(pushed.syntax_tree);
        sxml_xml_buffer_release(&pushed.owned_text);
        return SXML_STATUS_OK;
    }
    return sxml_xml_push_frame(document, &pushed, subset);
}

/*
 * A markup declaration of external DTD text with parameter-entity
 * references outside its literals (one unexpanded_declaration token): the
 * references are replaced (4.4.8, Included as PE) and the result is parsed
 * as the external subset. A reference to an entity that is not read
 * leaves the declaration unprocessed (5.1).
 */
static sxml_status
sxml_xml_expand_declaration(sxml_xml_document *document,
                            const sxml_xml_frame *frame, TSNode node) {
    if (document->skips_declarations) {
        return SXML_STATUS_OK;
    }
    const sxml_source_location location =
        sxml_xml_node_location(document, frame, node);
    sxml_xml_external_text source = {
        .text = {.bytes = NULL, .byte_count = 0},
        .owned_text =
            sxml_xml_buffer_empty(document->parser->configuration.allocator),
        .location = location,
        .description = SXML_XML_TEXT(
            "a markup declaration must match its production once its "
            "parameter-entity references are replaced (XML 1.0, sections 2.8 "
            "and 4.4.8)")};
    sxml_xml_parameter_expansion_result result = {.is_complete = true};
    sxml_status status = sxml_xml_expand_parameter_entities(
        document,
        &(sxml_xml_parameter_expansion){.text = sxml_xml_node_text(frame, node),
                                        .is_in_literal = false,
                                        .location = location,
                                        .output = &source.owned_text},
        &result);
    if (status == SXML_STATUS_OK && result.breaks_group_nesting) {
        status = sxml_xml_report_validity(
            document,
            (sxml_xml_validity_failure){
                .description = SXML_XML_TEXT(
                    "VC: Proper Group/PE Nesting: a group's parentheses are in "
                    "different replacement texts (XML 1.0, section 3.2.1)"),
                .location = location});
    }
    if (status == SXML_STATUS_OK && result.breaks_declaration_nesting) {
        status = sxml_xml_report_validity(
            document,
            (sxml_xml_validity_failure){
                .description = SXML_XML_TEXT(
                    "VC: Proper Declaration/PE Nesting: a declaration's start "
                    "and end are in different replacement texts (XML 1.0, "
                    "section 2.8)"),
                .location = location});
    }
    if (status != SXML_STATUS_OK || !result.is_complete ||
        source.owned_text.byte_count == 0) {
        sxml_xml_buffer_release(&source.owned_text);
        return status;
    }
    return sxml_xml_push_external_text(document, frame, &source);
}

/*
 * [61] conditionalSect whose keyword is IGNORE or a parameter-entity
 * reference (an include section with INCLUDE written out is walked as it
 * is). For a reference, the replacement text followed by the section's
 * text after the reference must read S? ('INCLUDE' | 'IGNORE') S? '[' and
 * the contents (3.4; the '[' and more may come from the entity, which only
 * VC: Proper Conditional Section/PE Nesting forbids); the contents of an
 * included section are parsed as the external subset.
 */
static sxml_status
sxml_xml_visit_conditional_section(sxml_xml_document *document,
                                   const sxml_xml_frame *frame, TSNode node) {
    const TSNode keyword = sxml_xml_field(node, SXML_XML_FIELD_KEYWORD);
    if (sxml_xml_type_of(keyword) != SXML_XML_NODE_PARAMETER_ENTITY_REFERENCE) {
        return SXML_STATUS_OK;
    }
    const sxml_source_location location =
        sxml_xml_node_location(document, frame, keyword);
    sxml_xml_parameter_inclusion inclusion = {.is_included = false,
                                              .entity_index = 0};
    sxml_status status = sxml_xml_include_parameter_entity(
        document,
        &(sxml_xml_value_reference){
            .entity_name =
                sxml_xml_field_text(frame, keyword, SXML_XML_FIELD_NAME),
            .location = location},
        &inclusion);
    if (status != SXML_STATUS_OK || !inclusion.is_included) {
        return status;
    }
    /* The section's text after the reference, without the final "]]>". */
    const sxml_byte_view section = sxml_xml_node_text(frame, node);
    const size_t after_keyword =
        ts_node_end_byte(keyword) - ts_node_start_byte(node);
    const sxml_byte_view rest = sxml_xml_view_from_text(
        section, after_keyword,
        section.byte_count >= after_keyword + 3
            ? section.byte_count - after_keyword - 3
            : 0);
    sxml_xml_external_text source = {
        .text = {.bytes = NULL, .byte_count = 0},
        .owned_text =
            sxml_xml_buffer_empty(document->parser->configuration.allocator),
        .location = location,
        .description = SXML_XML_TEXT(
            "the contents of an included conditional section must match [31] "
            "extSubsetDecl (XML 1.0, section 3.4)")};
    sxml_xml_buffer *text = &source.owned_text;
    status = sxml_xml_buffer_append(
        text, sxml_xml_entity_text(document,
                                   (sxml_xml_entity_text_request){
                                       .entity_index = inclusion.entity_index,
                                       .skips_text_declaration = true}));
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append(text, rest);
    }
    if (status != SXML_STATUS_OK) {
        sxml_xml_buffer_release(text);
        return sxml_xml_fail_resource(document, status);
    }
    const sxml_byte_view whole = {.bytes = text->bytes,
                                  .byte_count = text->byte_count};
    size_t offset = 0;
    while (offset < whole.byte_count &&
           sxml_xml_byte_is_whitespace(whole.bytes[offset])) {
        offset += 1;
    }
    static const char include_keyword[] = "INCLUDE";
    static const char ignore_keyword[] = "IGNORE";
    bool is_include = false;
    size_t keyword_byte_count = 0;
    if (whole.byte_count - offset >= sizeof include_keyword - 1 &&
        memcmp(whole.bytes + offset, include_keyword,
               sizeof include_keyword - 1) == 0) {
        is_include = true;
        keyword_byte_count = sizeof include_keyword - 1;
    } else if (whole.byte_count - offset >= sizeof ignore_keyword - 1 &&
               memcmp(whole.bytes + offset, ignore_keyword,
                      sizeof ignore_keyword - 1) == 0) {
        keyword_byte_count = sizeof ignore_keyword - 1;
    }
    offset += keyword_byte_count;
    while (offset < whole.byte_count &&
           sxml_xml_byte_is_whitespace(whole.bytes[offset])) {
        offset += 1;
    }
    if (keyword_byte_count == 0 || offset >= whole.byte_count ||
        whole.bytes[offset] != '[') {
        sxml_xml_buffer_release(text);
        return sxml_xml_fail(
            document,
            (sxml_xml_failure){
                .status = SXML_STATUS_MALFORMED_INPUT,
                .description = SXML_XML_TEXT(
                    "a conditional section must start with INCLUDE or IGNORE "
                    "and '[' ([62] includeSect, [63] ignoreSect, XML 1.0, "
                    "section 3.4)"),
                .location = location});
    }
    offset += 1;
    if (!sxml_byte_view_equals(
            sxml_xml_text_trim_whitespace(sxml_xml_entity_text(
                document, (sxml_xml_entity_text_request){
                              .entity_index = inclusion.entity_index,
                              .skips_text_declaration = true})),
            is_include ? SXML_XML_TEXT("INCLUDE") : SXML_XML_TEXT("IGNORE"))) {
        status = sxml_xml_report_validity(
            document,
            (sxml_xml_validity_failure){
                .description = SXML_XML_TEXT(
                    "VC: Proper Conditional Section/PE Nesting: a section's "
                    "'<![', '[' and ']]>' are in different replacement texts "
                    "(XML 1.0, section 3.4)"),
                .location = location});
        if (status != SXML_STATUS_OK) {
            sxml_xml_buffer_release(text);
            return status;
        }
    }
    if (!is_include || offset == whole.byte_count) {
        /* Ignored: the grammar has checked the text as [64]
           ignoreSectContents already. */
        sxml_xml_buffer_release(text);
        return SXML_STATUS_OK;
    }
    memmove(text->bytes, text->bytes + offset, text->byte_count - offset);
    text->byte_count -= offset;
    return sxml_xml_push_external_text(document, frame, &source);
}

/* --------------------------------------------- Element types, notations */

/* The quantifier after node, '?', '*' or '+', or 0 ([47], [48]). */
static uint8_t sxml_xml_quantifier_after(const sxml_xml_frame *frame,
                                         TSNode node) {
    const TSNode next = ts_node_next_sibling(node);
    if (ts_node_is_null(next) || ts_node_is_named(next)) {
        return 0;
    }
    const sxml_byte_view text = sxml_xml_node_text(frame, next);
    if (text.byte_count == 1 &&
        (text.bytes[0] == '?' || text.bytes[0] == '*' || text.bytes[0] == '+')) {
        return text.bytes[0];
    }
    return 0;
}

/* Appends one particle to the DTD's content models. */
static sxml_status sxml_xml_add_particle(sxml_xml_document *document,
                                         const sxml_xml_particle *particle) {
    void *element = NULL;
    const sxml_status status =
        sxml_xml_array_push(&document->parser->dtd.particles, &element);
    if (status != SXML_STATUS_OK) {
        return sxml_xml_fail_resource(document, status);
    }
    *(sxml_xml_particle *)element = *particle;
    return SXML_STATUS_OK;
}

/* An element name of a content model, stored as a particle. */
static sxml_status sxml_xml_add_element_particle(sxml_xml_document *document,
                                                 sxml_xml_frame_node name) {
    sxml_xml_particle particle = {
        .particle_kind = SXML_XML_PARTICLE_KIND_ELEMENT,
        .quantifier = sxml_xml_quantifier_after(name.frame, name.node),
        .element_name = {.byte_offset = 0, .byte_count = 0}};
    const sxml_byte_view element_name = sxml_xml_node_text(name.frame, name.node);
    uint32_t element_index = 0;
    sxml_status status = sxml_xml_fail_resource(
        document, sxml_xml_dtd_get_element(&document->parser->dtd, element_name,
                                           &element_index));
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xml_element_entry *element =
        sxml_xml_dtd_element_at(&document->parser->dtd, element_index);
    element->is_referred_to = true;
    particle.element_name = element->element_name;
    return sxml_xml_add_particle(document, &particle);
}

/*
 * The particles of a [47] children content model, in document order: a
 * cursor walks the groups with an explicit depth (D71), writing a group's
 * start (with the quantifier after it) on the way in and its end on the
 * way out, and each name of a [48] cp.
 */
static sxml_status sxml_xml_add_content_model(sxml_xml_document *document,
                                              sxml_xml_frame_node content) {
    const sxml_xml_frame *frame = content.frame;
    TSTreeCursor cursor = ts_tree_cursor_new(content.node);
    sxml_status status = SXML_STATUS_OK;
    bool is_entering = true;
    uint32_t depth = 0;
    while (status == SXML_STATUS_OK) {
        const TSNode current = ts_tree_cursor_current_node(&cursor);
        const sxml_xml_node_type current_type = sxml_xml_type_of(current);
        const bool is_group = current_type == SXML_XML_NODE_CHOICE_GROUP ||
                              current_type == SXML_XML_NODE_SEQUENCE_GROUP;
        if (is_entering) {
            if (is_group) {
                status = sxml_xml_add_particle(
                    document,
                    &(sxml_xml_particle){
                        .particle_kind = current_type == SXML_XML_NODE_CHOICE_GROUP
                                             ? SXML_XML_PARTICLE_KIND_CHOICE
                                             : SXML_XML_PARTICLE_KIND_SEQUENCE,
                        .quantifier = sxml_xml_quantifier_after(frame, current),
                        .element_name = {.byte_offset = 0, .byte_count = 0}});
            } else if (current_type == SXML_XML_NODE_NAME) {
                status = sxml_xml_add_element_particle(
                    document, (sxml_xml_frame_node){.frame = frame, .node = current});
            }
            if (!is_group && current_type != SXML_XML_NODE_ELEMENT_CONTENT &&
                current_type != SXML_XML_NODE_CONTENT_PARTICLE) {
                is_entering = false;
                continue;
            }
            if (ts_tree_cursor_goto_first_child(&cursor)) {
                depth += 1;
                continue;
            }
            is_entering = false;
            continue;
        }
        if (is_group && depth != 0 && ts_node_child_count(current) != 0) {
            status = sxml_xml_add_particle(
                document,
                &(sxml_xml_particle){
                    .particle_kind = SXML_XML_PARTICLE_KIND_GROUP_END,
                    .quantifier = 0,
                    .element_name = {.byte_offset = 0, .byte_count = 0}});
        }
        if (depth == 0) {
            break;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor)) {
            is_entering = true;
            continue;
        }
        (void)ts_tree_cursor_goto_parent(&cursor);
        depth -= 1;
    }
    ts_tree_cursor_delete(&cursor);
    return status;
}

static const sxml_byte_view sxml_xml_duplicate_types_description =
    SXML_BYTE_VIEW_INITIALIZER(
        "VC: No Duplicate Types: an element type appears twice in one mixed-"
        "content declaration (XML 1.0, section 3.2.2)");

/*
 * [45] elementdecl: the content model is kept for DTD validation (the
 * first declaration of an element type; another is VC: Unique Element
 * Type Declaration, 3.2).
 */
static sxml_status sxml_xml_declare_element(sxml_xml_document *document,
                                            const sxml_xml_frame *frame,
                                            TSNode node) {
    const TSNode name_node = sxml_xml_field(node, SXML_XML_FIELD_NAME);
    sxml_status status = sxml_xml_check_qualified_name(document, frame, name_node);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xml_dtd *dtd = &document->parser->dtd;
    const sxml_source_location location =
        sxml_xml_node_location(document, frame, node);
    uint32_t element_index = 0;
    status = sxml_xml_fail_resource(
        document, sxml_xml_dtd_get_element(
                      dtd, sxml_xml_node_text(frame, name_node), &element_index));
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (sxml_xml_dtd_element_at(dtd, element_index)->is_declared) {
        return sxml_xml_report_validity(
            document,
            (sxml_xml_validity_failure){
                .description = SXML_XML_TEXT(
                    "VC: Unique Element Type Declaration: an element type is "
                    "declared twice (XML 1.0, section 3.2)"),
                .location = location});
    }
    const TSNode specification =
        ts_node_named_child(sxml_xml_field(node, SXML_XML_FIELD_CONTENT), 0);
    const sxml_byte_view specification_text =
        sxml_xml_node_text(frame, sxml_xml_field(node, SXML_XML_FIELD_CONTENT));
    const size_t first_particle = dtd->particles.element_count;
    sxml_xml_content_kind content_kind = SXML_XML_CONTENT_KIND_ANY;
    if (ts_node_is_null(specification)) {
        content_kind = specification_text.byte_count != 0 &&
                               specification_text.bytes[0] == 'E'
                           ? SXML_XML_CONTENT_KIND_EMPTY
                           : SXML_XML_CONTENT_KIND_ANY;
    } else if (sxml_xml_type_of(specification) == SXML_XML_NODE_MIXED_CONTENT) {
        content_kind = SXML_XML_CONTENT_KIND_MIXED;
        sxml_xml_children children = {.first_index = 0, .child_count = 0};
        status = sxml_xml_fail_resource(
            document, sxml_xml_list_children(document, specification, &children));
        for (size_t child_index = 0;
             status == SXML_STATUS_OK && child_index < children.child_count;
             child_index++) {
            const TSNode child = sxml_xml_child_at(document, children, child_index);
            const sxml_byte_view child_name = sxml_xml_node_text(frame, child);
            for (size_t earlier = 0;
                 status == SXML_STATUS_OK && earlier < child_index; earlier++) {
                if (sxml_byte_view_equals(
                        sxml_xml_node_text(
                            frame, sxml_xml_child_at(document, children, earlier)),
                        child_name)) {
                    status = sxml_xml_report_validity(
                        document,
                        (sxml_xml_validity_failure){
                            .description = sxml_xml_duplicate_types_description,
                            .location = location});
                }
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_xml_add_element_particle(
                    document, (sxml_xml_frame_node){.frame = frame, .node = child});
            }
        }
        sxml_xml_release_children(document, children);
    } else {
        content_kind = SXML_XML_CONTENT_KIND_CHILDREN;
        status = sxml_xml_add_content_model(
            document, (sxml_xml_frame_node){.frame = frame, .node = specification});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xml_element_entry *element = sxml_xml_dtd_element_at(dtd, element_index);
    element->is_declared = true;
    element->is_declared_in_external_markup =
        frame->frame_kind != SXML_XML_FRAME_KIND_DOCUMENT;
    element->content_kind = content_kind;
    element->first_particle = first_particle;
    element->particle_count = dtd->particles.element_count - first_particle;
    return SXML_STATUS_OK;
}

/* [82] NotationDecl: kept for DTD validation; another declaration of the
   name is VC: Unique Notation Name (4.7). */
static sxml_status sxml_xml_declare_notation(sxml_xml_document *document,
                                             const sxml_xml_frame *frame,
                                             TSNode node) {
    const TSNode name_node = sxml_xml_field(node, SXML_XML_FIELD_NAME);
    sxml_status status = sxml_xml_check_no_colon(document, frame, name_node);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xml_dtd *dtd = &document->parser->dtd;
    const sxml_byte_view name = sxml_xml_node_text(frame, name_node);
    for (size_t notation_index = 0; notation_index < dtd->notations.element_count;
         notation_index++) {
        const sxml_xml_notation *notation =
            sxml_xml_array_at(&dtd->notations, notation_index);
        if (sxml_byte_view_equals(sxml_xml_dtd_text(dtd, notation->notation_name),
                                  name)) {
            return sxml_xml_report_validity(
                document,
                (sxml_xml_validity_failure){
                    .description = SXML_XML_TEXT(
                        "VC: Unique Notation Name: a notation is declared twice "
                        "(XML 1.0, section 4.7)"),
                    .location = sxml_xml_node_location(document, frame, node)});
        }
    }
    sxml_xml_notation notation = {.notation_name = {.byte_offset = 0, .byte_count = 0},
                                  .public_identifier = {.byte_offset = 0, .byte_count = 0},
                                  .system_identifier = {.byte_offset = 0, .byte_count = 0}};
    status = sxml_xml_store(document, name, &notation.notation_name);
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_store(
            document,
            sxml_xml_literal_content(sxml_xml_field_text(frame, node, SXML_XML_FIELD_PUBLIC_ID)),
            &notation.public_identifier);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_store(
            document,
            sxml_xml_literal_content(sxml_xml_field_text(frame, node, SXML_XML_FIELD_SYSTEM_ID)),
            &notation.system_identifier);
    }
    void *element = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_fail_resource(document,
                                        sxml_xml_array_push(&dtd->notations, &element));
    }
    if (status == SXML_STATUS_OK) {
        *(sxml_xml_notation *)element = notation;
    }
    return status;
}

/* What a visit asks of the walk. */
typedef struct sxml_xml_visit_result {
    /* Enter the children of the node visited. */
    bool descends;
} sxml_xml_visit_result;

/* One child of an internal subset or of external DTD text. */
static sxml_status sxml_xml_visit_declaration(sxml_xml_document *document,
                                              const sxml_xml_frame *frame,
                                              sxml_xml_visit_result *result) {
    const TSNode node = ts_tree_cursor_current_node(&frame->cursor);
    const sxml_xml_node_type node_type = sxml_xml_type_of(node);
    if (node_type == SXML_XML_NODE_INCLUDE_SECTION) {
        /* [62] includeSect: its declarations are read in place (3.4). */
        result->descends = true;
        return SXML_STATUS_OK;
    }
    if (node_type == SXML_XML_NODE_CONDITIONAL_SECTION) {
        return sxml_xml_visit_conditional_section(document, frame, node);
    }
    if (node_type == SXML_XML_NODE_UNEXPANDED_DECLARATION) {
        return sxml_xml_expand_declaration(document, frame, node);
    }
    if (node_type == SXML_XML_NODE_TEXT_DECLARATION &&
        !frame->starts_external_entity) {
        return sxml_xml_fail_at(
            document,
            (sxml_xml_node_failure){
                .frame = frame,
                .node = node,
                .description = SXML_XML_TEXT(
                    "a text declaration may stand only at the start of an "
                    "external entity (XML 1.0, section 4.3.1)")});
    }
    if (node_type == SXML_XML_NODE_GENERAL_ENTITY_DECLARATION ||
        node_type == SXML_XML_NODE_PARAMETER_ENTITY_DECLARATION) {
        return document->skips_declarations
                   ? sxml_xml_check_no_colon(
                         document, frame,
                         sxml_xml_field(node, SXML_XML_FIELD_NAME))
                   : sxml_xml_declare_entity(document, frame, node);
    }
    if (node_type == SXML_XML_NODE_ATTRIBUTE_LIST_DECLARATION) {
        return document->skips_declarations
                   ? sxml_xml_check_qualified_name(
                         document, frame,
                         sxml_xml_field(node, SXML_XML_FIELD_NAME))
                   : sxml_xml_declare_attribute_list(document, frame, node);
    }
    if (node_type == SXML_XML_NODE_ELEMENT_DECLARATION) {
        return sxml_xml_declare_element(document, frame, node);
    }
    if (node_type == SXML_XML_NODE_NOTATION_DECLARATION) {
        return sxml_xml_declare_notation(document, frame, node);
    }
    if (node_type == SXML_XML_NODE_PROCESSING_INSTRUCTION) {
        return sxml_xml_check_no_colon(
            document, frame, sxml_xml_field(node, SXML_XML_FIELD_TARGET));
    }
    if (node_type == SXML_XML_NODE_PARAMETER_ENTITY_REFERENCE) {
        return sxml_xml_reference_parameter_entity(document, frame, node);
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- Start tags */

static sxml_xml_attribute *
sxml_xml_attribute_at(const sxml_xml_document *document,
                      size_t attribute_index) {
    return sxml_xml_array_at(&document->parser->attributes, attribute_index);
}

static sxml_byte_view
sxml_xml_attribute_value(const sxml_xml_document *document,
                         const sxml_xml_attribute *attribute) {
    return attribute->is_defaulted
               ? sxml_xml_dtd_text(&document->parser->dtd, attribute->value)
               : sxml_xml_buffer_view(&document->parser->attribute_values,
                                      attribute->value);
}

/* The definition of attribute_name for the element, or NULL. */
typedef struct sxml_xml_definition_lookup {
    uint32_t first_definition;
    sxml_byte_view attribute_name;
} sxml_xml_definition_lookup;

static const sxml_xml_attribute_definition *
sxml_xml_find_definition(const sxml_xml_dtd *dtd,
                         sxml_xml_definition_lookup lookup) {
    for (uint32_t number = lookup.first_definition; number != 0;) {
        const sxml_xml_attribute_definition *definition =
            sxml_xml_dtd_get_definition(dtd, number);
        if (sxml_byte_view_equals(
                sxml_xml_dtd_text(dtd, definition->attribute_name),
                lookup.attribute_name)) {
            return definition;
        }
        number = definition->next_definition;
    }
    return NULL;
}

/* Which key duplicates are looked for by. */
typedef struct sxml_xml_duplicate_search {
    /* Expanded names (attribute_name) when true, names as written
       otherwise. Namespace declarations are skipped for expanded names. */
    bool compares_expanded_names;
} sxml_xml_duplicate_search;

static bool sxml_xml_attributes_collide(const sxml_xml_attribute *first,
                                        const sxml_xml_attribute *second,
                                        sxml_xml_duplicate_search search) {
    if (search.compares_expanded_names) {
        return first->attribute_name.index == second->attribute_name.index;
    }
    return sxml_byte_view_equals(first->qualified_name, second->qualified_name);
}

static uint32_t sxml_xml_attribute_hash(const sxml_xml_attribute *attribute,
                                        sxml_xml_duplicate_search search) {
    if (search.compares_expanded_names) {
        return attribute->attribute_name.index * 2654435761U;
    }
    uint32_t hash = 2166136261U;
    for (size_t byte_index = 0;
         byte_index < attribute->qualified_name.byte_count; byte_index++) {
        hash ^= attribute->qualified_name.bytes[byte_index];
        hash *= 16777619U;
    }
    return hash;
}

/* Finds whether two attributes share a key: pairwise for few attributes,
   through a hash index of the parser's slots for many. */
/* Whether a duplicate was found. */
typedef struct sxml_xml_duplicate_result {
    bool has_duplicate;
} sxml_xml_duplicate_result;

static sxml_status sxml_xml_has_duplicate(sxml_xml_document *document,
                                          sxml_xml_duplicate_search search,
                                          sxml_xml_duplicate_result *result) {
    bool *has_duplicate = &result->has_duplicate;
    sxml_xml_parser *parser = document->parser;
    const size_t attribute_count = parser->attributes.element_count;
    *has_duplicate = false;
    if (attribute_count <= SXML_XML_ATTRIBUTE_SCAN_LIMIT) {
        for (size_t first = 0; first < attribute_count; first++) {
            const sxml_xml_attribute *one =
                sxml_xml_attribute_at(document, first);
            if (search.compares_expanded_names &&
                one->is_namespace_declaration) {
                continue;
            }
            for (size_t second = first + 1; second < attribute_count;
                 second++) {
                const sxml_xml_attribute *other =
                    sxml_xml_attribute_at(document, second);
                if (search.compares_expanded_names &&
                    other->is_namespace_declaration) {
                    continue;
                }
                if (sxml_xml_attributes_collide(one, other, search)) {
                    *has_duplicate = true;
                    return SXML_STATUS_OK;
                }
            }
        }
        return SXML_STATUS_OK;
    }
    size_t slot_count = SXML_XML_ATTRIBUTE_SCAN_LIMIT;
    while (slot_count < attribute_count * 2) {
        slot_count *= 2;
    }
    sxml_xml_array *slots = &parser->duplicate_slots;
    while (slots->element_count < slot_count) {
        void *element = NULL;
        sxml_status status = sxml_xml_array_push(slots, &element);
        if (status != SXML_STATUS_OK) {
            return sxml_xml_fail_resource(document, status);
        }
    }
    memset(slots->elements, 0, slot_count * sizeof(uint32_t));
    const size_t mask = slot_count - 1;
    for (size_t attribute_index = 0; attribute_index < attribute_count;
         attribute_index++) {
        const sxml_xml_attribute *attribute =
            sxml_xml_attribute_at(document, attribute_index);
        if (search.compares_expanded_names &&
            attribute->is_namespace_declaration) {
            continue;
        }
        size_t slot_index = sxml_xml_attribute_hash(attribute, search) & mask;
        for (;;) {
            uint32_t *slot = sxml_xml_array_at(slots, slot_index);
            if (*slot == 0) {
                *slot = (uint32_t)attribute_index + 1;
                break;
            }
            if (sxml_xml_attributes_collide(
                    sxml_xml_attribute_at(document, *slot - 1), attribute,
                    search)) {
                *has_duplicate = true;
                return SXML_STATUS_OK;
            }
            slot_index = (slot_index + 1) & mask;
        }
    }
    return SXML_STATUS_OK;
}

/* VC: Standalone Document Declaration (2.9): in a document that says
   standalone="yes", a declaration from external markup must not change an
   attribute: by supplying its default or by normalizing it further. */
static sxml_status sxml_xml_report_standalone(const sxml_xml_document *document,
                                              const sxml_xml_frame *frame,
                                              TSNode node) {
    return sxml_xml_report_validity(
        document,
        (sxml_xml_validity_failure){
            .description = SXML_XML_TEXT(
                "VC: Standalone Document Declaration: a declaration outside the "
                "document entity changes an attribute of a standalone document "
                "(XML 1.0, section 2.9)"),
            .location = sxml_xml_node_location((sxml_xml_document *)document, frame,
                                               node)});
}

/* A specified attribute and its definition, which may be NULL. */
typedef struct sxml_xml_attribute_check {
    sxml_xml_frame_node tag;
    sxml_byte_view attribute_name;
    sxml_byte_view value;
    const sxml_xml_attribute_definition *definition;
} sxml_xml_attribute_check;

/*
 * The validity constraints on a specified attribute that the schema
 * cannot check (XML 1.0, 3.1 and 3.3): a namespace declaration is an
 * attribute to validation and must be declared, with its #FIXED value
 * (VC: Attribute Value Type, Fixed Attribute Default; namespace
 * declarations are no attributes in XDM, so the schema never sees them),
 * and a tokenized value separates its tokens by single spaces, which XSD's
 * whitespace collapsing would hide (VC: Name Token, IDREF, Entity Name,
 * Fifth Edition erratum E20).
 */
static sxml_status sxml_xml_check_attribute(const sxml_xml_document *document,
                                            const sxml_xml_attribute_check *check) {
    const sxml_xml_attribute_definition *definition = check->definition;
    const sxml_byte_view name = check->attribute_name;
    const bool is_namespace_declaration =
        sxml_byte_view_equals(name, SXML_XML_TEXT("xmlns")) ||
        (name.byte_count > 6 && memcmp(name.bytes, "xmlns:", 6) == 0);
    sxml_byte_view description = {.bytes = NULL, .byte_count = 0};
    if (is_namespace_declaration && definition == NULL) {
        description = SXML_XML_TEXT(
            "VC: Attribute Value Type: the attribute is not declared, and a "
            "namespace declaration is an attribute to DTD validation (XML 1.0, "
            "section 3.1)");
    } else if (is_namespace_declaration &&
               definition->default_kind == SXML_XML_DEFAULT_KIND_FIXED &&
               !sxml_byte_view_equals(check->value,
                                      sxml_xml_dtd_text(&document->parser->dtd,
                                                        definition->default_value))) {
        description = SXML_XML_TEXT(
            "VC: Fixed Attribute Default: the value differs from the #FIXED "
            "default (XML 1.0, section 3.3.2)");
    } else if (definition != NULL &&
               definition->attribute_type != SXML_XML_ATTRIBUTE_TYPE_CDATA &&
               (sxml_xml_view_contains_byte(check->value, '\t') ||
                sxml_xml_view_contains_byte(check->value, '\n') ||
                sxml_xml_view_contains_byte(check->value, '\r'))) {
        description = SXML_XML_TEXT(
            "VC: Name Token: the tokens of a tokenized attribute are separated "
            "by single spaces (XML 1.0, section 3.3.1)");
    } else {
        return SXML_STATUS_OK;
    }
    return sxml_xml_report_validity(
        document, (sxml_xml_validity_failure){
                      .description = description,
                      .location = sxml_xml_node_location((sxml_xml_document *)document,
                                                         check->tag.frame,
                                                         check->tag.node)});
}

/* A tag to collect the attributes of. */
typedef struct sxml_xml_tag {
    TSNode node;
    sxml_byte_view element_name;
} sxml_xml_tag;

/*
 * Collects the attributes of a start tag with normalized values, checks
 * WFC: Unique Att Spec (3.1) and adds the defaults of attribute-list
 * declarations (3.3.2).
 */
static sxml_status sxml_xml_collect_attributes(sxml_xml_document *document,
                                               const sxml_xml_frame *frame,
                                               const sxml_xml_tag *tag) {
    sxml_xml_parser *parser = document->parser;
    parser->attributes.element_count = 0;
    parser->attribute_values.byte_count = 0;
    const uint32_t first_definition =
        sxml_xml_dtd_first_definition(&parser->dtd, tag->element_name);
    sxml_xml_children children = {.first_index = 0, .child_count = 0};
    sxml_status list_status =
        sxml_xml_list_children(document, tag->node, &children);
    if (list_status != SXML_STATUS_OK) {
        return sxml_xml_fail_resource(document, list_status);
    }
    for (size_t child_index = 0; child_index < children.child_count;
         child_index++) {
        const TSNode child = sxml_xml_child_at(document, children, child_index);
        if (sxml_xml_type_of(child) != SXML_XML_NODE_ATTRIBUTE) {
            continue;
        }
        const sxml_byte_view name =
            sxml_xml_field_text(frame, child, SXML_XML_FIELD_NAME);
        const size_t value_start = parser->attribute_values.byte_count;
        sxml_status status = sxml_xml_normalize_value(
            document, frame, sxml_xml_field(child, SXML_XML_FIELD_VALUE));
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const sxml_xml_attribute_definition *definition =
            sxml_xml_find_definition(&parser->dtd,
                                     (sxml_xml_definition_lookup){
                                         .first_definition = first_definition,
                                         .attribute_name = name});
        sxml_byte_range value = {
            .byte_offset = value_start,
            .byte_count = parser->attribute_values.byte_count - value_start};
        const sxml_xml_attribute_type attribute_type =
            definition == NULL ? SXML_XML_ATTRIBUTE_TYPE_CDATA
                               : definition->attribute_type;
        /* An empty value may have no buffer yet: nothing to collapse, and
           no offset to apply to a null pointer. */
        if (attribute_type != SXML_XML_ATTRIBUTE_TYPE_CDATA &&
            value.byte_count != 0) {
            const size_t written_byte_count = value.byte_count;
            value.byte_count = sxml_xml_collapse_spaces((sxml_memory_view){
                .bytes = parser->attribute_values.bytes + value_start,
                .byte_count = value.byte_count});
            parser->attribute_values.byte_count =
                value_start + value.byte_count;
            if (document->is_standalone &&
                definition->is_declared_in_external_markup &&
                value.byte_count != written_byte_count) {
                status = sxml_xml_report_standalone(document, frame, child);
                if (status != SXML_STATUS_OK) {
                    return status;
                }
            }
        }
        if (document->validates) {
            status = sxml_xml_check_attribute(
                document, &(sxml_xml_attribute_check){
                              .tag = {.frame = frame, .node = child},
                              .attribute_name = name,
                              .value = sxml_xml_buffer_view(
                                  &parser->attribute_values, value),
                              .definition = definition});
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        void *element = NULL;
        status = sxml_xml_array_push(&parser->attributes, &element);
        if (status != SXML_STATUS_OK) {
            return sxml_xml_fail_resource(document, status);
        }
        *(sxml_xml_attribute *)element = (sxml_xml_attribute){
            .qualified_name = name,
            .value = value,
            .is_defaulted = false,
            .attribute_type = attribute_type,
            .location = sxml_xml_event_location(document, frame, child),
            .is_namespace_declaration = false,
            .attribute_name = {.index = 0},
            .namespace_prefix = {.index = 0},
        };
    }
    sxml_xml_release_children(document, children);
    sxml_xml_duplicate_result duplicate = {.has_duplicate = false};
    sxml_status status = sxml_xml_has_duplicate(
        document, (sxml_xml_duplicate_search){.compares_expanded_names = false},
        &duplicate);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (duplicate.has_duplicate) {
        return sxml_xml_fail_at(
            document, (sxml_xml_node_failure){
                          .frame = frame,
                          .node = tag->node,
                          .description = SXML_XML_TEXT(
                              "WFC: Unique Att Spec: an attribute name appears "
                              "twice in one tag (XML 1.0, section 3.1)")});
    }
    const size_t specified_count = parser->attributes.element_count;
    for (uint32_t number = first_definition; number != 0;) {
        const sxml_xml_attribute_definition *definition =
            sxml_xml_dtd_get_definition(&parser->dtd, number);
        number = definition->next_definition;
        if (!definition->has_default_value) {
            continue;
        }
        const sxml_byte_view name =
            sxml_xml_dtd_text(&parser->dtd, definition->attribute_name);
        bool is_specified = false;
        for (size_t attribute_index = 0;
             attribute_index < specified_count && !is_specified;
             attribute_index++) {
            is_specified = sxml_byte_view_equals(
                sxml_xml_attribute_at(document, attribute_index)
                    ->qualified_name,
                name);
        }
        if (is_specified) {
            continue;
        }
        if (document->is_standalone && definition->is_declared_in_external_markup) {
            status = sxml_xml_report_standalone(document, frame, tag->node);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        void *element = NULL;
        status = sxml_xml_array_push(&parser->attributes, &element);
        if (status != SXML_STATUS_OK) {
            return sxml_xml_fail_resource(document, status);
        }
        *(sxml_xml_attribute *)element = (sxml_xml_attribute){
            .qualified_name = name,
            .value = definition->default_value,
            .is_defaulted = true,
            .attribute_type = definition->attribute_type,
            .location = sxml_xml_event_location(document, frame, tag->node),
            .is_namespace_declaration = false,
            .attribute_name = {.index = 0},
            .namespace_prefix = {.index = 0},
        };
    }
    return SXML_STATUS_OK;
}

/* Finds the namespace a prefix is bound to in scope. */
typedef struct sxml_xml_prefix_resolution {
    bool is_bound;
    sxml_namespace_uri_identifier namespace_uri;
} sxml_xml_prefix_resolution;

static sxml_xml_prefix_resolution
sxml_xml_resolve_prefix(const sxml_xml_document *document,
                        sxml_namespace_prefix_identifier namespace_prefix) {
    const sxml_xml_array *bindings = &document->parser->namespace_bindings;
    if (namespace_prefix.index == document->xml_prefix.index) {
        return (sxml_xml_prefix_resolution){
            .is_bound = true,
            .namespace_uri =
                sxml_well_known_namespace_identifier(SXML_NAMESPACE_XML)};
    }
    for (size_t binding_index = bindings->element_count; binding_index > 0;
         binding_index--) {
        const sxml_xml_scoped_binding *binding =
            sxml_xml_array_at(bindings, binding_index - 1);
        if (binding->namespace_prefix.index == namespace_prefix.index) {
            return (sxml_xml_prefix_resolution){
                .is_bound = binding->namespace_uri.index != 0,
                .namespace_uri = binding->namespace_uri};
        }
    }
    return (sxml_xml_prefix_resolution){.is_bound = false,
                                        .namespace_uri = {.index = 0}};
}

static const sxml_byte_view sxml_xml_reserved_namespace_description =
    SXML_BYTE_VIEW_INITIALIZER(
        "NSC: Reserved Prefixes and Namespace Names: xml only with its "
        "namespace, xmlns never declared (Namespaces in XML 1.0, section 3)");

/* Records the namespace declaration in attribute (NS 1.0, section 3). */
static sxml_status sxml_xml_declare_namespace(sxml_xml_document *document,
                                              const sxml_xml_frame *frame,
                                              sxml_xml_attribute *attribute) {
    sxml_name_pool *name_pool = document->parser->configuration.name_pool;
    const sxml_byte_view value = sxml_xml_attribute_value(document, attribute);
    const sxml_xml_name_split split =
        sxml_xml_split_name(attribute->qualified_name);
    const bool is_default = split.namespace_prefix.byte_count == 0;
    const sxml_byte_view declared_prefix =
        is_default ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
                   : split.local_name;
    sxml_xml_failure failure = {.status = SXML_STATUS_MALFORMED_INPUT,
                                .description =
                                    sxml_xml_reserved_namespace_description,
                                .location = attribute->location};
    if (failure.location.line_number == 0) {
        failure.location = sxml_xml_node_location(
            document, frame, ts_tree_cursor_current_node(&frame->cursor));
    }
    const bool is_xml_prefix =
        sxml_byte_view_equals(declared_prefix, SXML_XML_TEXT("xml"));
    const bool is_xml_namespace =
        sxml_byte_view_equals(value, sxml_xml_namespace_text);
    if (sxml_byte_view_equals(declared_prefix, SXML_XML_TEXT("xmlns")) ||
        sxml_byte_view_equals(value, sxml_xmlns_namespace_text) ||
        is_xml_prefix != is_xml_namespace) {
        return sxml_xml_fail(document, failure);
    }
    if (!is_default && value.byte_count == 0 &&
        document->version != SXML_XML_VERSION_1_1) {
        failure.description = SXML_XML_TEXT(
            "NSC: a namespace declaration for a prefix must not have an "
            "empty value in XML 1.0 (Namespaces in XML 1.0, section 3)");
        return sxml_xml_fail(document, failure);
    }
    attribute->is_namespace_declaration = true;
    if (is_xml_prefix) {
        /* Bound in every element already; nothing to send. */
        attribute->attribute_name.index = UINT32_MAX;
        return SXML_STATUS_OK;
    }
    sxml_xml_scoped_binding binding = {.namespace_prefix = {.index = 0},
                                       .namespace_uri = {.index = 0}};
    sxml_status status = SXML_STATUS_OK;
    if (!is_default) {
        status = sxml_name_pool_intern_namespace_prefix(
            name_pool, declared_prefix, &binding.namespace_prefix);
    }
    if (status == SXML_STATUS_OK && value.byte_count != 0) {
        status = sxml_name_pool_intern_namespace_uri(name_pool, value,
                                                     &binding.namespace_uri);
    }
    void *element = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_array_push(&document->parser->namespace_bindings,
                                     &element);
    }
    if (status != SXML_STATUS_OK) {
        return sxml_xml_fail_resource(document, status);
    }
    *(sxml_xml_scoped_binding *)element = binding;
    return SXML_STATUS_OK;
}

/* Interns a name written with a prefix, resolving the prefix in scope.
   Unprefixed attribute names are in no namespace (NS 1.0, section 6.2). */
typedef struct sxml_xml_name_resolution_request {
    sxml_byte_view qualified_name;
    bool uses_default_namespace;
} sxml_xml_name_resolution_request;

typedef struct sxml_xml_resolved_name {
    sxml_name_identifier name;
    sxml_namespace_prefix_identifier namespace_prefix;
    sxml_namespace_uri_identifier namespace_uri;
} sxml_xml_resolved_name;

static sxml_status
sxml_xml_resolve_name(sxml_xml_document *document,
                      const sxml_xml_name_resolution_request *request,
                      sxml_xml_resolved_name *resolved) {
    sxml_name_pool *name_pool = document->parser->configuration.name_pool;
    const sxml_xml_name_split split =
        sxml_xml_split_name(request->qualified_name);
    if (!split.is_qualified_name) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    sxml_namespace_prefix_identifier prefix = {.index = 0};
    sxml_status status = SXML_STATUS_OK;
    if (split.namespace_prefix.byte_count != 0) {
        status = sxml_name_pool_intern_namespace_prefix(
            name_pool, split.namespace_prefix, &prefix);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (prefix.index == document->xmlns_prefix.index) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
    }
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    if (prefix.index != 0 || request->uses_default_namespace) {
        const sxml_xml_prefix_resolution resolution =
            sxml_xml_resolve_prefix(document, prefix);
        if (!resolution.is_bound && prefix.index != 0) {
            return SXML_STATUS_INDEX_OUT_OF_RANGE;
        }
        namespace_uri = resolution.namespace_uri;
    }
    sxml_name_identifier name = {.index = 0};
    status = sxml_name_pool_intern_name(
        name_pool,
        &(sxml_name_request){.namespace_uri = namespace_uri,
                             .local_name = split.local_name},
        &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *resolved = (sxml_xml_resolved_name){.name = name,
                                         .namespace_prefix = prefix,
                                         .namespace_uri = namespace_uri};
    return SXML_STATUS_OK;
}

/* Turns the failures of sxml_xml_resolve_name into reports. */
static sxml_status sxml_xml_report_name(sxml_xml_document *document,
                                        sxml_status status,
                                        sxml_source_location location) {
    sxml_byte_view description = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        return status;
    }
    if (status == SXML_STATUS_MALFORMED_INPUT) {
        description = sxml_xml_qualified_name_description;
    } else if (status == SXML_STATUS_INVALID_ARGUMENT) {
        description = SXML_XML_TEXT(
            "NSC: element names must not have the prefix xmlns (Namespaces in "
            "XML 1.0, section 3)");
    } else if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
        description = SXML_XML_TEXT(
            "NSC: Prefix Declared: the prefix is not bound to a namespace "
            "(Namespaces in XML 1.0, section 5)");
    } else {
        return sxml_xml_fail_resource(document, status);
    }
    return sxml_xml_fail(
        document, (sxml_xml_failure){.status = SXML_STATUS_MALFORMED_INPUT,
                                     .description = description,
                                     .location = location});
}

static bool sxml_xml_is_namespace_declaration_name(sxml_byte_view name) {
    static const uint8_t prefix[] = {'x', 'm', 'l', 'n', 's', ':'};
    return sxml_byte_view_equals(name, SXML_XML_TEXT("xmlns")) ||
           (name.byte_count > sizeof prefix &&
            memcmp(name.bytes, prefix, sizeof prefix) == 0);
}

/* Sends start_element, the namespace bindings and the attributes. */
typedef struct sxml_xml_element_header {
    TSNode tag_node;
    const sxml_xml_resolved_name *element;
    size_t scope_start;
} sxml_xml_element_header;

/*
 * The entity base URI of the element being started (D210): the URI of
 * the innermost external parsed entity being read, when the element is at
 * its top (XML 1.0, 4.2.2: relative URIs in an entity are relative to
 * where the entity is; XML Base, 4.2); empty otherwise. The element
 * depth already counts the element.
 */
static sxml_byte_view sxml_xml_entity_base_uri(const sxml_xml_document *document) {
    for (size_t frame_number = sxml_xml_frame_count(document); frame_number > 0;
         frame_number -= 1) {
        const sxml_xml_frame *frame = sxml_xml_frame_at(document, frame_number - 1);
        if (frame->frame_kind == SXML_XML_FRAME_KIND_EXTERNAL_GENERAL_ENTITY) {
            return frame->element_depth + 1 == document->element_depth
                       ? sxml_xml_dtd_text(&document->parser->dtd, frame->base_uri)
                       : (sxml_byte_view){.bytes = NULL, .byte_count = 0};
        }
    }
    return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
}

static sxml_status sxml_xml_send_header(sxml_xml_document *document,
                                        const sxml_xml_frame *frame,
                                        const sxml_xml_element_header *header) {
    if (!sxml_xml_has_builder(document)) {
        return SXML_STATUS_OK;
    }
    const sxml_tree_builder builder = document->builder;
    sxml_status status = builder.operations->start_element(
        builder.state,
        &(sxml_element_start_event){
            .element_name = header->element->name,
            .namespace_prefix = header->element->namespace_prefix,
            .type_annotation = {.index = SXML_TYPE_NONE},
            .is_id = false,
            .is_idrefs = false,
            .is_nilled = false,
            .ignores_parent_namespaces = false,
            .location =
                sxml_xml_node_location(document, frame, header->tag_node),
            .entity_base_uri = sxml_xml_entity_base_uri(document)});
    const sxml_xml_array *bindings = &document->parser->namespace_bindings;
    for (size_t binding_index = header->scope_start;
         status == SXML_STATUS_OK && binding_index < bindings->element_count;
         binding_index++) {
        const sxml_xml_scoped_binding *binding =
            sxml_xml_array_at(bindings, binding_index);
        status = builder.operations->add_namespace_binding(
            builder.state, &(sxml_namespace_binding){
                               .namespace_prefix = binding->namespace_prefix,
                               .namespace_uri = binding->namespace_uri});
    }
    const size_t attribute_count = document->parser->attributes.element_count;
    for (size_t attribute_index = 0;
         status == SXML_STATUS_OK && attribute_index < attribute_count;
         attribute_index++) {
        const sxml_xml_attribute *attribute =
            sxml_xml_attribute_at(document, attribute_index);
        if (attribute->is_namespace_declaration) {
            continue;
        }
        status = builder.operations->add_attribute(
            builder.state, &(sxml_attribute_event){
                               .attribute_name = attribute->attribute_name,
                               .namespace_prefix = attribute->namespace_prefix,
                               .attribute_value = sxml_xml_attribute_value(
                                   document, attribute),
                               .type_annotation = {.index = SXML_TYPE_NONE},
                               .is_id = attribute->attribute_type ==
                                        SXML_XML_ATTRIBUTE_TYPE_ID,
                               .is_idrefs = attribute->attribute_type ==
                                                SXML_XML_ATTRIBUTE_TYPE_IDREF ||
                                            attribute->attribute_type ==
                                                SXML_XML_ATTRIBUTE_TYPE_IDREFS,
                               .location = attribute->location});
    }
    return status;
}

/*
 * Resolves the names of the attributes that are not namespace
 * declarations and checks their uniqueness by expanded name (NS 1.0,
 * section 6.3). xml:id makes an ID (xml:id 1.0, section 4).
 */
static sxml_status sxml_xml_resolve_attributes(sxml_xml_document *document,
                                               const sxml_xml_frame *frame,
                                               TSNode tag_node) {
    sxml_xml_parser *parser = document->parser;
    const size_t attribute_count = parser->attributes.element_count;
    for (size_t attribute_index = 0; attribute_index < attribute_count;
         attribute_index++) {
        sxml_xml_attribute *attribute =
            sxml_xml_attribute_at(document, attribute_index);
        if (attribute->is_namespace_declaration) {
            continue;
        }
        sxml_xml_resolved_name resolved = {.name = {.index = 0},
                                           .namespace_prefix = {.index = 0},
                                           .namespace_uri = {.index = 0}};
        sxml_status status = sxml_xml_resolve_name(
            document,
            &(sxml_xml_name_resolution_request){
                .qualified_name = attribute->qualified_name,
                .uses_default_namespace = false},
            &resolved);
        if (status != SXML_STATUS_OK) {
            return sxml_xml_report_name(
                document, status,
                sxml_xml_node_location(document, frame, tag_node));
        }
        attribute->attribute_name = resolved.name;
        attribute->namespace_prefix = resolved.namespace_prefix;
        const bool is_xml_id =
            resolved.namespace_uri.index == SXML_NAMESPACE_XML &&
            sxml_byte_view_equals(
                sxml_xml_split_name(attribute->qualified_name).local_name,
                SXML_XML_TEXT("id"));
        if (is_xml_id &&
            attribute->attribute_type != SXML_XML_ATTRIBUTE_TYPE_ID) {
            /* xml:id values are normalized as IDs (xml:id 1.0, section 4). */
            const sxml_byte_view value =
                sxml_xml_attribute_value(document, attribute);
            sxml_byte_range copied = attribute->value;
            if (attribute->is_defaulted) {
                copied.byte_offset = parser->attribute_values.byte_count;
                status =
                    sxml_xml_buffer_append(&parser->attribute_values, value);
                if (status != SXML_STATUS_OK) {
                    return sxml_xml_fail_resource(document, status);
                }
                attribute->is_defaulted = false;
            }
            if (copied.byte_count != 0) {
                copied.byte_count = sxml_xml_collapse_spaces((sxml_memory_view){
                    .bytes = parser->attribute_values.bytes + copied.byte_offset,
                    .byte_count = copied.byte_count});
            }
            attribute->value = copied;
            attribute->attribute_type = SXML_XML_ATTRIBUTE_TYPE_ID;
        }
    }
    sxml_xml_duplicate_result duplicate = {.has_duplicate = false};
    sxml_status status = sxml_xml_has_duplicate(
        document, (sxml_xml_duplicate_search){.compares_expanded_names = true},
        &duplicate);
    if (status == SXML_STATUS_OK && duplicate.has_duplicate) {
        return sxml_xml_fail_at(
            document, (sxml_xml_node_failure){
                          .frame = frame,
                          .node = tag_node,
                          .description = SXML_XML_TEXT(
                              "NSC: Attributes Unique: two attributes have the "
                              "same expanded name (Namespaces in XML 1.0, "
                              "section 6.3)")});
    }
    return status;
}

static sxml_status sxml_xml_end_element(sxml_xml_document *document) {
    sxml_xml_parser *parser = document->parser;
    sxml_xml_array *scopes = &parser->element_scopes;
    const size_t *scope_start =
        sxml_xml_array_at(scopes, scopes->element_count - 1);
    parser->namespace_bindings.element_count = *scope_start;
    scopes->element_count -= 1;
    parser->open_element_names.element_count -= 1;
    document->element_depth -= 1;
    if (document->tracks_contents && parser->open_contents.element_count != 0) {
        parser->open_contents.element_count -= 1;
    }
    if (!sxml_xml_has_builder(document)) {
        return SXML_STATUS_OK;
    }
    return document->builder.operations->end_element(document->builder.state);
}

/* [40] STag and [44] EmptyElemTag. */
static sxml_status sxml_xml_start_element(sxml_xml_document *document,
                                          const sxml_xml_frame *frame,
                                          TSNode tag_node) {
    sxml_xml_parser *parser = document->parser;
    const uint32_t maximum_depth = parser->configuration.limits.maximum_depth;
    if (maximum_depth != 0 && document->element_depth >= maximum_depth) {
        return sxml_xml_fail(
            document,
            (sxml_xml_failure){
                .status = SXML_STATUS_LIMIT_EXCEEDED,
                .description = SXML_XML_TEXT(
                    "elements are nested deeper than the configured maximum"),
                .location = sxml_xml_node_location(document, frame, tag_node)});
    }
    const sxml_byte_view element_name =
        sxml_xml_field_text(frame, tag_node, SXML_XML_FIELD_NAME);
    if (document->element_depth == 0 && document->has_doctype &&
        !sxml_byte_view_equals(element_name, document->doctype_name)) {
        const sxml_status status = sxml_xml_report_validity(
            document,
            (sxml_xml_validity_failure){
                .description = SXML_XML_TEXT(
                    "VC: Root Element Type: the document element's name must be "
                    "the name of the document type declaration (XML 1.0, "
                    "section 2.8)"),
                .location = sxml_xml_node_location(document, frame, tag_node)});
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_status status = sxml_xml_collect_attributes(
        document, frame,
        &(sxml_xml_tag){.node = tag_node, .element_name = element_name});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const size_t scope_start = parser->namespace_bindings.element_count;
    void *element = NULL;
    status = sxml_xml_array_push(&parser->element_scopes, &element);
    if (status != SXML_STATUS_OK) {
        return sxml_xml_fail_resource(document, status);
    }
    *(size_t *)element = scope_start;
    status = sxml_xml_array_push(&parser->open_element_names, &element);
    if (status != SXML_STATUS_OK) {
        return sxml_xml_fail_resource(document, status);
    }
    *(sxml_byte_view *)element = element_name;
    document->element_depth += 1;
    if (document->tracks_contents) {
        const sxml_xml_dtd *dtd = &parser->dtd;
        const sxml_xml_lookup declaration =
            sxml_xml_dtd_find_element(dtd, element_name);
        const sxml_xml_element_entry *entry =
            declaration.is_found
                ? sxml_xml_dtd_element_at(dtd, declaration.entry_index)
                : NULL;
        status = sxml_xml_array_push(&parser->open_contents, &element);
        if (status != SXML_STATUS_OK) {
            return sxml_xml_fail_resource(document, status);
        }
        *(sxml_xml_open_content *)element = (sxml_xml_open_content){
            .is_declared = entry != NULL && entry->is_declared,
            .content_kind =
                entry != NULL ? entry->content_kind : SXML_XML_CONTENT_KIND_ANY,
            .is_declared_in_external_markup =
                entry != NULL && entry->is_declared_in_external_markup};
    }
    const size_t attribute_count = parser->attributes.element_count;
    for (size_t attribute_index = 0; attribute_index < attribute_count;
         attribute_index++) {
        sxml_xml_attribute *attribute =
            sxml_xml_attribute_at(document, attribute_index);
        if (!sxml_xml_is_namespace_declaration_name(
                attribute->qualified_name)) {
            continue;
        }
        if (!sxml_xml_split_name(attribute->qualified_name).is_qualified_name) {
            return sxml_xml_fail_at(
                document,
                (sxml_xml_node_failure){
                    .frame = frame,
                    .node = tag_node,
                    .description = sxml_xml_qualified_name_description});
        }
        status = sxml_xml_declare_namespace(document, frame, attribute);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_xml_resolved_name resolved = {.name = {.index = 0},
                                       .namespace_prefix = {.index = 0},
                                       .namespace_uri = {.index = 0}};
    status = sxml_xml_resolve_name(
        document,
        &(sxml_xml_name_resolution_request){.qualified_name = element_name,
                                            .uses_default_namespace = true},
        &resolved);
    if (status != SXML_STATUS_OK) {
        return sxml_xml_report_name(
            document, status,
            sxml_xml_node_location(document, frame, tag_node));
    }
    status = sxml_xml_resolve_attributes(document, frame, tag_node);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xml_send_header(
        document, frame,
        &(sxml_xml_element_header){.tag_node = tag_node,
                                   .element = &resolved,
                                   .scope_start = scope_start});
}

/* ---------------------------------------------------- Content walk */

/* The wrapper around a general entity's replacement text: an element of
   content (4.3.2). A replacement text that closes it early leaves an end
   tag without a start, which the grammar rejects. */
static const sxml_byte_view sxml_xml_content_prefix =
    SXML_BYTE_VIEW_INITIALIZER("<x>");
static const sxml_byte_view sxml_xml_content_suffix =
    SXML_BYTE_VIEW_INITIALIZER("</x>");

static const sxml_byte_view sxml_xml_content_entity_description =
    SXML_BYTE_VIEW_INITIALIZER(
        "the replacement text of an entity referenced in content must match "
        "[43] content (XML 1.0, section 4.3.2)");

static const sxml_byte_view sxml_xml_external_entity_description =
    SXML_BYTE_VIEW_INITIALIZER(
        "an external parsed entity referenced in content must match [78] "
        "extParsedEnt (XML 1.0, section 4.3.2)");

/*
 * [68] EntityRef in content to an external parsed entity that was read
 * (4.4.3, Included If Validating; sxml includes it whenever it reads it):
 * its text is parsed after the external entity marker, so that its text
 * declaration and content follow [78] extParsedEnt, and walked in a frame
 * of its own. The tree is kept for later references, as an internal
 * entity's is.
 */
static sxml_status
sxml_xml_reference_external_entity(sxml_xml_document *document,
                                   const sxml_xml_value_expansion *reference) {
    sxml_xml_dtd *dtd = &document->parser->dtd;
    sxml_xml_entity *entity = sxml_xml_dtd_get_entity(dtd, reference->entity_index);
    const sxml_xml_frame pushed = {
        .frame_kind = SXML_XML_FRAME_KIND_EXTERNAL_GENERAL_ENTITY,
        .syntax_tree = NULL,
        .cursor = {.tree = NULL, .id = NULL, .context = {0, 0, 0}},
        .text = sxml_xml_entity_text(
            document,
            (sxml_xml_entity_text_request){.entity_index = reference->entity_index,
                                           .skips_text_declaration = false}),
        .prefix_byte_count = (uint32_t)sxml_xml_external_entity_marker.byte_count,
        .owned_text =
            sxml_xml_buffer_empty(document->parser->configuration.allocator),
        .has_entity = true,
        .entity_index = reference->entity_index,
        .reference_location = reference->location,
        .is_external_context = false,
        .starts_external_entity = true,
        .base_uri = entity->entity_uri,
        .needs_advance = false,
        .descends = false,
        .cursor_depth = 0,
    };
    if (entity->syntax_tree == NULL) {
        const sxml_xml_wrapped_text wrapped = {
            .prefix = sxml_xml_external_entity_marker,
            .text = pushed.text,
            .suffix = {.bytes = NULL, .byte_count = 0}};
        TSTree *syntax_tree = NULL;
        sxml_status status = sxml_xml_parse_wrapped(document, &wrapped, &syntax_tree);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (syntax_tree == NULL) {
            return sxml_xml_fail(
                document, (sxml_xml_failure){
                              .status = SXML_STATUS_MALFORMED_INPUT,
                              .description = sxml_xml_external_entity_description,
                              .location = reference->location});
        }
        entity = sxml_xml_dtd_get_entity(dtd, reference->entity_index);
        entity->syntax_tree = syntax_tree;
    }
    const TSNode walk_root = sxml_xml_field(ts_tree_root_node(entity->syntax_tree),
                                            SXML_XML_FIELD_EXTERNAL_ENTITY);
    if (ts_node_is_null(walk_root)) {
        return SXML_STATUS_OK;
    }
    return sxml_xml_push_frame(document, &pushed, walk_root);
}

/* [68] EntityRef in content. */
static sxml_status
sxml_xml_reference_content_entity(sxml_xml_document *document,
                                  const sxml_xml_frame *frame, TSNode node) {
    const sxml_byte_view name =
        sxml_xml_field_text(frame, node, SXML_XML_FIELD_NAME);
    const sxml_source_location location =
        sxml_xml_node_location(document, frame, node);
    const sxml_xml_predefined_entity predefined =
        sxml_xml_find_predefined_entity(name);
    if (predefined.is_predefined) {
        return sxml_xml_send_text(
            document,
            &(sxml_text_event){
                .text = {.bytes = &predefined.character, .byte_count = 1},
                .location = location});
    }
    sxml_xml_reference_outcome outcome = {.expands = false, .entity_index = 0};
    sxml_status status = sxml_xml_check_reference(
        document,
        &(sxml_xml_reference_check){
            .reference = {.entity_name = name, .location = location},
            .is_in_attribute_value = false},
        &outcome);
    if (status != SXML_STATUS_OK || !outcome.expands) {
        return status;
    }
    const sxml_xml_entity *entity =
        sxml_xml_dtd_get_entity(&document->parser->dtd, outcome.entity_index);
    if (entity->entity_kind == SXML_XML_ENTITY_KIND_EXTERNAL_PARSED) {
        return sxml_xml_reference_external_entity(
            document, &(sxml_xml_value_expansion){
                          .entity_index = outcome.entity_index,
                          .location = location});
    }
    const sxml_byte_view replacement =
        sxml_xml_dtd_text(&document->parser->dtd, entity->replacement_text);
    if (!sxml_xml_view_contains_byte(replacement, '<') &&
        !sxml_xml_view_contains_byte(replacement, '&')) {
        /* Plain text needs no parse: it matches [43] content unless it
           holds "]]>" ([14] CharData). Sent as it is (no line-end
           normalization of replacement text). */
        for (size_t byte_index = 0; byte_index + 2 < replacement.byte_count;
             byte_index++) {
            if (memcmp(replacement.bytes + byte_index, "]]>", 3) == 0) {
                return sxml_xml_fail(
                    document,
                    (sxml_xml_failure){.status = SXML_STATUS_MALFORMED_INPUT,
                                       .description =
                                           sxml_xml_content_entity_description,
                                       .location = location});
            }
        }
        return sxml_xml_send_text(
            document,
            &(sxml_text_event){.text = replacement, .location = location});
    }
    sxml_xml_frame pushed = {
        .frame_kind = SXML_XML_FRAME_KIND_GENERAL_ENTITY,
        .syntax_tree = NULL,
        .cursor = {.tree = NULL, .id = NULL, .context = {0, 0, 0}},
        .text =
            sxml_xml_dtd_text(&document->parser->dtd, entity->replacement_text),
        .prefix_byte_count = (uint32_t)sxml_xml_content_prefix.byte_count,
        .owned_text =
            sxml_xml_buffer_empty(document->parser->configuration.allocator),
        .has_entity = true,
        .entity_index = outcome.entity_index,
        .reference_location = location,
        .needs_advance = false,
        .descends = false,
    };
    /* The tree of a replacement text is made at the first reference and
       kept until the parse ends, so repeated references cost a walk, not a
       parse; frames over it do not own it. */
    sxml_xml_entity *cached =
        sxml_xml_dtd_get_entity(&document->parser->dtd, outcome.entity_index);
    if (cached->syntax_tree != NULL) {
        return sxml_xml_push_frame(
            document, &pushed,
            sxml_xml_field(ts_tree_root_node(cached->syntax_tree),
                           SXML_XML_FIELD_ROOT));
    }
    const sxml_xml_wrapped_text wrapped = {.prefix = sxml_xml_content_prefix,
                                           .text = pushed.text,
                                           .suffix = sxml_xml_content_suffix};
    TSTree *syntax_tree = NULL;
    status = sxml_xml_parse_wrapped(document, &wrapped, &syntax_tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (syntax_tree == NULL) {
        return sxml_xml_fail(
            document, (sxml_xml_failure){
                          .status = SXML_STATUS_MALFORMED_INPUT,
                          .description = sxml_xml_content_entity_description,
                          .location = location});
    }
    cached->syntax_tree = syntax_tree;
    return sxml_xml_push_frame(
        document, &pushed,
        sxml_xml_field(ts_tree_root_node(syntax_tree), SXML_XML_FIELD_ROOT));
}

/*
 * The validity constraints on content that no schema component checks
 * (XML 1.0, 3 VC: Element Valid, 2.9 VC: Standalone Document
 * Declaration): an EMPTY element has no content at all, not a comment, a
 * processing instruction or an entity reference (Fifth Edition errata
 * E15); element content holds white space only as character data, not as
 * character references or CDATA sections; and in a standalone document
 * an element type declared outside the document entity must not get
 * white space in element content.
 */
static sxml_status sxml_xml_check_content(sxml_xml_document *document,
                                          const sxml_xml_frame_node *content) {
    const sxml_xml_array *open_contents = &document->parser->open_contents;
    if (open_contents->element_count == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_xml_open_content *open =
        sxml_xml_array_at(open_contents, open_contents->element_count - 1);
    if (!open->is_declared) {
        return SXML_STATUS_OK;
    }
    const sxml_xml_node_type node_type = sxml_xml_type_of(content->node);
    sxml_byte_view description = {.bytes = NULL, .byte_count = 0};
    if (open->content_kind == SXML_XML_CONTENT_KIND_EMPTY &&
        (node_type == SXML_XML_NODE_CHARACTER_DATA ||
         node_type == SXML_XML_NODE_CHARACTER_REFERENCE ||
         node_type == SXML_XML_NODE_ENTITY_REFERENCE ||
         node_type == SXML_XML_NODE_CDATA_SECTION ||
         node_type == SXML_XML_NODE_COMMENT ||
         node_type == SXML_XML_NODE_PROCESSING_INSTRUCTION)) {
        description = SXML_XML_TEXT(
            "VC: Element Valid: an element declared EMPTY has no content, not "
            "even a comment, a processing instruction or an entity reference "
            "(XML 1.0, section 3)");
    } else if (open->content_kind == SXML_XML_CONTENT_KIND_CHILDREN &&
               (node_type == SXML_XML_NODE_CHARACTER_REFERENCE ||
                node_type == SXML_XML_NODE_CDATA_SECTION)) {
        description = SXML_XML_TEXT(
            "VC: Element Valid: element content holds white space only as "
            "character data, not as character references or CDATA sections "
            "(XML 1.0, section 3)");
    } else if (open->content_kind == SXML_XML_CONTENT_KIND_CHILDREN &&
               node_type == SXML_XML_NODE_CHARACTER_DATA &&
               document->is_standalone && open->is_declared_in_external_markup &&
               sxml_xml_text_is_whitespace(
                   sxml_xml_node_text(content->frame, content->node))) {
        description = SXML_XML_TEXT(
            "VC: Standalone Document Declaration: white space in the element "
            "content of an element type declared outside the document entity "
            "of a standalone document (XML 1.0, section 2.9)");
    } else {
        return SXML_STATUS_OK;
    }
    return sxml_xml_report_validity(
        document, (sxml_xml_validity_failure){
                      .description = description,
                      .location = sxml_xml_node_location(document, content->frame,
                                                         content->node)});
}

/*
 * What an item of a content segment is after the document element has
 * ended ([1] document ::= prolog element Misc*; [27] Misc ::= Comment | PI
 * | S), which the grammar of content segments leaves to the builder
 * (D222): comments and processing instructions are visited as content,
 * white space is no text node and is ignored, anything else is not
 * well-formed.
 */
typedef enum sxml_xml_misc_kind {
    SXML_XML_MISC_KIND_MARKUP,
    SXML_XML_MISC_KIND_WHITE_SPACE,
    SXML_XML_MISC_KIND_NONE,
} sxml_xml_misc_kind;

static sxml_xml_misc_kind sxml_xml_misc_kind_of(const sxml_xml_frame_node *item) {
    const sxml_xml_node_type node_type = sxml_xml_type_of(item->node);
    if (node_type == SXML_XML_NODE_COMMENT ||
        node_type == SXML_XML_NODE_PROCESSING_INSTRUCTION) {
        return SXML_XML_MISC_KIND_MARKUP;
    }
    if (node_type == SXML_XML_NODE_CHARACTER_DATA &&
        sxml_xml_text_is_whitespace(sxml_xml_node_text(item->frame, item->node))) {
        return SXML_XML_MISC_KIND_WHITE_SPACE;
    }
    return SXML_XML_MISC_KIND_NONE;
}

/* One node of the content walk; sets result->descends to enter its
   children. */
static sxml_status sxml_xml_visit_content(sxml_xml_document *document,
                                          const sxml_xml_frame *frame,
                                          sxml_xml_visit_result *result) {
    const TSNode node = ts_tree_cursor_current_node(&frame->cursor);
    if (frame->frame_kind == SXML_XML_FRAME_KIND_DOCUMENT &&
        ts_node_start_byte(node) < frame->prefix_byte_count) {
        /* The marker of a document segment and the start tags written
           before a content segment (D222). */
        return SXML_STATUS_OK;
    }
    if (frame->is_content_segment && document->element_depth == 0) {
        const sxml_xml_misc_kind misc_kind = sxml_xml_misc_kind_of(
            &(sxml_xml_frame_node){.frame = frame, .node = node});
        if (misc_kind == SXML_XML_MISC_KIND_WHITE_SPACE) {
            return SXML_STATUS_OK;
        }
        if (misc_kind == SXML_XML_MISC_KIND_NONE) {
            return sxml_xml_fail_at(
                document,
                (sxml_xml_node_failure){
                    .frame = frame,
                    .node = node,
                    .description = SXML_XML_TEXT(
                        "not well-formed: only comments, processing "
                        "instructions and white space may follow the document "
                        "element (XML 1.0, [1] document, [27] Misc)")});
        }
    }
    /* The wrapper's own tags in an entity frame are the children of the
       walk root. */
    const bool is_wrapper_child =
        frame->cursor_depth == 1 &&
        frame->frame_kind == SXML_XML_FRAME_KIND_GENERAL_ENTITY;
    const sxml_xml_node_type node_type = sxml_xml_type_of(node);
    if (node_type == SXML_XML_NODE_ELEMENT) {
        result->descends = true;
        return SXML_STATUS_OK;
    }
    if (document->validates) {
        const sxml_status status = sxml_xml_check_content(
            document, &(sxml_xml_frame_node){.frame = frame, .node = node});
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (node_type == SXML_XML_NODE_START_TAG) {
        return is_wrapper_child ? SXML_STATUS_OK
                                : sxml_xml_start_element(document, frame, node);
    }
    if (node_type == SXML_XML_NODE_EMPTY_ELEMENT_TAG) {
        sxml_status status = sxml_xml_start_element(document, frame, node);
        return status == SXML_STATUS_OK ? sxml_xml_end_element(document)
                                        : status;
    }
    if (node_type == SXML_XML_NODE_END_TAG) {
        return is_wrapper_child ? SXML_STATUS_OK
                                : sxml_xml_end_element(document);
    }
    if (node_type == SXML_XML_NODE_CHARACTER_DATA) {
        return sxml_xml_send_character_data(document, frame, node);
    }
    if (node_type == SXML_XML_NODE_CHARACTER_REFERENCE) {
        return sxml_xml_visit_character_reference(document, frame, node);
    }
    if (node_type == SXML_XML_NODE_ENTITY_REFERENCE) {
        return sxml_xml_reference_content_entity(document, frame, node);
    }
    if (node_type == SXML_XML_NODE_CDATA_SECTION) {
        return sxml_xml_visit_cdata_section(document, frame, node);
    }
    if (node_type == SXML_XML_NODE_COMMENT) {
        return sxml_xml_visit_comment(document, frame, node);
    }
    if (node_type == SXML_XML_NODE_PROCESSING_INSTRUCTION) {
        return sxml_xml_visit_processing_instruction(document, frame, node);
    }
    return SXML_STATUS_OK;
}

/* True for the node types that are whole constructs when they appear
   among the children of an ERROR node: what came before the trouble. */
static bool sxml_xml_is_complete_construct(TSNode node) {
    if (ts_node_is_error(node) || ts_node_is_missing(node) ||
        ts_node_has_error(node)) {
        return false;
    }
    const sxml_xml_node_type node_type = sxml_xml_type_of(node);
    return node_type == SXML_XML_NODE_ELEMENT ||
           node_type == SXML_XML_NODE_START_TAG ||
           node_type == SXML_XML_NODE_END_TAG ||
           node_type == SXML_XML_NODE_EMPTY_ELEMENT_TAG ||
           node_type == SXML_XML_NODE_CHARACTER_DATA ||
           node_type == SXML_XML_NODE_CHARACTER_REFERENCE ||
           node_type == SXML_XML_NODE_ENTITY_REFERENCE ||
           node_type == SXML_XML_NODE_CDATA_SECTION ||
           node_type == SXML_XML_NODE_COMMENT ||
           node_type == SXML_XML_NODE_PROCESSING_INSTRUCTION ||
           node_type == SXML_XML_NODE_ATTRIBUTE ||
           strcmp(ts_node_type(node), "xml_declaration") == 0 ||
           strcmp(ts_node_type(node), "doctype_declaration") == 0;
}

/*
 * Where a syntax error is: tree-sitter's recovery often wraps much of the
 * document in one ERROR node, so the innermost ERROR is taken, and within
 * it the first child that is not a complete construct, or its end when
 * all its children are.
 */
static size_t sxml_xml_error_offset(TSNode error) {
    TSNode node = error;
    bool descends = true;
    while (descends) {
        descends = false;
        const uint32_t child_count = ts_node_child_count(node);
        for (uint32_t child_index = 0; child_index < child_count;
             child_index++) {
            const TSNode child = ts_node_child(node, child_index);
            if (ts_node_is_error(child) || ts_node_is_missing(child) ||
                ts_node_has_error(child)) {
                node = child;
                descends = true;
                break;
            }
        }
    }
    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t child_index = 0; child_index < child_count; child_index++) {
        const TSNode child = ts_node_child(node, child_index);
        if (!sxml_xml_is_complete_construct(child)) {
            return ts_node_start_byte(child);
        }
    }
    /* Nothing but complete constructs: the trouble is what is missing
       after them. */
    return child_count != 0 ? ts_node_end_byte(node) : ts_node_start_byte(node);
}

/* Reports the first ERROR or MISSING node of the tree of a document
   segment, whose root is tree->node. */
static sxml_status sxml_xml_check_syntax(sxml_xml_document *document,
                                         const sxml_xml_frame_node *tree) {
    const TSNode error = sxml_xml_find_error(tree->node);
    if (ts_node_is_null(error)) {
        return SXML_STATUS_OK;
    }
    return sxml_xml_fail(
        document,
        (sxml_xml_failure){
            .status = SXML_STATUS_MALFORMED_INPUT,
            .description =
                ts_node_is_missing(error)
                    ? SXML_XML_TEXT("not well-formed: the document ends or "
                                    "breaks off before a construct is "
                                    "complete (XML 1.0, section 2.1)")
                    : SXML_XML_TEXT("not well-formed: the text does not match "
                                    "the XML 1.0 grammar here (XML 1.0, "
                                    "section 2.1)"),
            .location =
                sxml_xml_location_at(
                    document, sxml_xml_document_offset_of(
                                  tree->frame, (uint32_t)sxml_xml_error_offset(error)))});
}

/* ------------------------------------------------- Document segments */

/*
 * The document is parsed in document segments (glossary
 * `document_segment`, D222): the head, then content segments of about
 * this many bytes, each parsed, walked and freed before the next, so that
 * the concrete syntax tree of one segment exists at a time. tree-sitter
 * needs about 37 bytes per byte of input for its tree (D219), so a
 * segment's tree takes about 600 KB; smaller segments parse no faster,
 * larger ones slower (D222).
 */
enum { SXML_XML_SEGMENT_BYTE_COUNT = 16384 };

/* The markers before the head and before a content segment (grammars/xml,
   document). */
static const sxml_byte_view sxml_xml_document_head_marker =
    SXML_BYTE_VIEW_INITIALIZER("\001D");
static const sxml_byte_view sxml_xml_content_segment_marker =
    SXML_BYTE_VIEW_INITIALIZER("\001C");

/* Parses a document segment, wrapped->text a part of the document's text
   at frame->document_offset, into *syntax_tree, reporting a syntax error
   at its place in the document. */
typedef struct sxml_xml_segment_parse {
    const sxml_xml_wrapped_text *wrapped;
    const sxml_xml_frame *frame;
} sxml_xml_segment_parse;

static sxml_status sxml_xml_parse_segment(sxml_xml_document *document,
                                          sxml_xml_segment_parse parse,
                                          TSTree **syntax_tree) {
    size_t total = 0;
    sxml_status status = sxml_size_add(parse.wrapped->prefix.byte_count,
                                       parse.wrapped->text.byte_count, &total);
    if (status != SXML_STATUS_OK || total > UINT32_MAX) {
        return sxml_xml_fail_resource(document, SXML_STATUS_LIMIT_EXCEEDED);
    }
    TSTree *tree = ts_parser_parse(document->syntax_parser, NULL,
                                   (TSInput){.payload = (void *)parse.wrapped,
                                             .read = sxml_xml_read_wrapped,
                                             .encoding = TSInputEncodingUTF8,
                                             .decode = NULL});
    if (tree == NULL) {
        return sxml_xml_fail_resource(document, SXML_STATUS_OUT_OF_MEMORY);
    }
    status = sxml_xml_check_syntax(
        document,
        &(sxml_xml_frame_node){.frame = parse.frame, .node = ts_tree_root_node(tree)});
    if (status != SXML_STATUS_OK) {
        ts_tree_delete(tree);
        return status;
    }
    *syntax_tree = tree;
    return SXML_STATUS_OK;
}

/*
 * Parses the next content segment and pushes a frame over it. The
 * segment's text follows the content segment marker and a start tag for
 * each open element, so that the external scanner checks its end tags
 * against their start tags (WFC: Element Type Match, 3); the walk skips
 * them. The segment is at least twice as long as those start tags, so
 * that writing them costs at most half the parse.
 */
static sxml_status sxml_xml_push_next_segment(sxml_xml_document *document) {
    sxml_xml_parser *parser = document->parser;
    sxml_xml_buffer *prefix = &parser->segment_prefix;
    prefix->byte_count = 0;
    sxml_status status =
        sxml_xml_buffer_append(prefix, sxml_xml_content_segment_marker);
    const sxml_xml_array *names = &parser->open_element_names;
    for (size_t name_index = 0;
         status == SXML_STATUS_OK && name_index < names->element_count;
         name_index++) {
        status = sxml_xml_buffer_append_byte(prefix, '<');
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(
                prefix, *(const sxml_byte_view *)sxml_xml_array_at(names, name_index));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append_byte(prefix, '>');
        }
    }
    if (status != SXML_STATUS_OK) {
        return sxml_xml_fail_resource(document, status);
    }
    if (prefix->byte_count > UINT32_MAX) {
        return sxml_xml_fail_resource(document, SXML_STATUS_LIMIT_EXCEEDED);
    }
    const size_t start_offset = document->segment_end_offset;
    const size_t end_offset = sxml_xml_find_segment_end(&(sxml_xml_segment_request){
        .text = document->text,
        .start_offset = start_offset,
        .target_byte_count = prefix->byte_count > SXML_XML_SEGMENT_BYTE_COUNT / 2
                                 ? prefix->byte_count * 2
                                 : SXML_XML_SEGMENT_BYTE_COUNT});
    document->segment_end_offset = end_offset;
    const sxml_xml_frame pushed = {
        .frame_kind = SXML_XML_FRAME_KIND_DOCUMENT,
        .syntax_tree = NULL,
        .cursor = {.tree = NULL, .id = NULL, .context = {0, 0, 0}},
        .text = {.bytes = document->text.bytes + start_offset,
                 .byte_count = end_offset - start_offset},
        .prefix_byte_count = (uint32_t)prefix->byte_count,
        .document_offset = start_offset,
        .is_content_segment = true,
        .owned_text = sxml_xml_buffer_empty(parser->configuration.allocator),
        .has_entity = false,
        .entity_index = 0,
        .reference_location = {.source_name = {.bytes = NULL, .byte_count = 0},
                               .line_number = 0,
                               .column_number = 0,
                               .byte_offset = 0},
        .is_external_context = false,
        .starts_external_entity = false,
        .base_uri = {.byte_offset = 0, .byte_count = 0},
        .needs_advance = false,
        .descends = false,
        .cursor_depth = 0,
        .element_depth = 0,
    };
    const sxml_xml_wrapped_text wrapped = {
        .prefix = {.bytes = prefix->bytes, .byte_count = prefix->byte_count},
        .text = pushed.text,
        .suffix = {.bytes = NULL, .byte_count = 0}};
    sxml_xml_frame owning = pushed;
    status = sxml_xml_parse_segment(
        document, (sxml_xml_segment_parse){.wrapped = &wrapped, .frame = &pushed},
        &owning.syntax_tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xml_push_frame(document, &owning,
                               ts_tree_root_node(owning.syntax_tree));
}

/* Whether content segments are left, after the frames of the content
   walk have ended. */
static bool sxml_xml_has_next_segment(const sxml_xml_document *document) {
    return document->segment_end_offset < document->text.byte_count;
}

/* After the last content segment: fails when the text ends inside an
   element ([1] document, [39] element). */
static sxml_status sxml_xml_check_content_end(sxml_xml_document *document) {
    if (document->element_depth == 0) {
        return SXML_STATUS_OK;
    }
    return sxml_xml_fail(
        document,
        (sxml_xml_failure){
            .status = SXML_STATUS_MALFORMED_INPUT,
            .description = SXML_XML_TEXT(
                "not well-formed: the document ends or breaks off before a "
                "construct is complete (XML 1.0, section 2.1)"),
            .location = sxml_xml_location_at(document, document->text.byte_count)});
}

/* Which walk the frames serve. */
typedef struct sxml_xml_walk {
    /* The internal subset when true, content otherwise. */
    bool walks_declarations;
    /* Frames below this belong to someone else. */
    size_t base_frame_count;
} sxml_xml_walk;

/*
 * Runs the frames above walk.base_frame_count to their end: each visit
 * may push a frame for an entity, which is walked first.
 */
static sxml_status sxml_xml_run_frames(sxml_xml_document *document,
                                       sxml_xml_walk walk) {
    for (;;) {
        if (sxml_xml_frame_count(document) <= walk.base_frame_count) {
            if (walk.walks_declarations) {
                return SXML_STATUS_OK;
            }
            if (!sxml_xml_has_next_segment(document)) {
                return sxml_xml_check_content_end(document);
            }
            /* The content goes on in the next document segment (D222). */
            const sxml_status status = sxml_xml_push_next_segment(document);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            continue;
        }
        const size_t frame_index = sxml_xml_frame_count(document) - 1;
        sxml_xml_frame *frame = sxml_xml_frame_at(document, frame_index);
        if (frame->needs_advance) {
            frame->needs_advance = false;
            if (!sxml_xml_frame_advance(frame)) {
                sxml_xml_pop_frame(document);
                continue;
            }
        }
        sxml_xml_visit_result result = {.descends = false};
        sxml_status status =
            walk.walks_declarations
                ? sxml_xml_visit_declaration(document, frame, &result)
                : sxml_xml_visit_content(document, frame, &result);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        /* A visit may have pushed a frame, moving the array. */
        frame = sxml_xml_frame_at(document, frame_index);
        frame->needs_advance = true;
        frame->descends = result.descends;
        if (document->pause_requested && !walk.walks_declarations) {
            /* An incremental parse resumes here (the frames stay). */
            document->pause_requested = false;
            return SXML_STATUS_OK;
        }
    }
}

/* ------------------------------------------------------ DTD validation */

/* The events of a validating parse go to the parse's builder, when it has
   one, and to the validator's checking builder (D199). */
static sxml_xml_tee *sxml_xml_tee_from_state(void *state) { return state; }

static bool sxml_xml_tee_has_first(const sxml_xml_tee *tee) {
    return tee->first.operations != NULL;
}

static sxml_status
sxml_xml_tee_start_document(void *state, const sxml_document_start_event *event) {
    const sxml_xml_tee *tee = sxml_xml_tee_from_state(state);
    sxml_status status = SXML_STATUS_OK;
    if (sxml_xml_tee_has_first(tee)) {
        status = tee->first.operations->start_document(tee->first.state, event);
    }
    return status == SXML_STATUS_OK
               ? tee->second.operations->start_document(tee->second.state, event)
               : status;
}

static sxml_status sxml_xml_tee_end_document(void *state) {
    const sxml_xml_tee *tee = sxml_xml_tee_from_state(state);
    sxml_status status = SXML_STATUS_OK;
    if (sxml_xml_tee_has_first(tee)) {
        status = tee->first.operations->end_document(tee->first.state);
    }
    return status == SXML_STATUS_OK
               ? tee->second.operations->end_document(tee->second.state)
               : status;
}

static sxml_status
sxml_xml_tee_start_element(void *state, const sxml_element_start_event *event) {
    const sxml_xml_tee *tee = sxml_xml_tee_from_state(state);
    sxml_status status = SXML_STATUS_OK;
    if (sxml_xml_tee_has_first(tee)) {
        status = tee->first.operations->start_element(tee->first.state, event);
    }
    return status == SXML_STATUS_OK
               ? tee->second.operations->start_element(tee->second.state, event)
               : status;
}

static sxml_status
sxml_xml_tee_add_namespace_binding(void *state,
                                   const sxml_namespace_binding *binding) {
    const sxml_xml_tee *tee = sxml_xml_tee_from_state(state);
    sxml_status status = SXML_STATUS_OK;
    if (sxml_xml_tee_has_first(tee)) {
        status = tee->first.operations->add_namespace_binding(tee->first.state,
                                                              binding);
    }
    return status == SXML_STATUS_OK ? tee->second.operations->add_namespace_binding(
                                          tee->second.state, binding)
                                    : status;
}

static sxml_status sxml_xml_tee_add_attribute(void *state,
                                              const sxml_attribute_event *event) {
    const sxml_xml_tee *tee = sxml_xml_tee_from_state(state);
    sxml_status status = SXML_STATUS_OK;
    if (sxml_xml_tee_has_first(tee)) {
        status = tee->first.operations->add_attribute(tee->first.state, event);
    }
    return status == SXML_STATUS_OK
               ? tee->second.operations->add_attribute(tee->second.state, event)
               : status;
}

static sxml_status sxml_xml_tee_end_element(void *state) {
    const sxml_xml_tee *tee = sxml_xml_tee_from_state(state);
    sxml_status status = SXML_STATUS_OK;
    if (sxml_xml_tee_has_first(tee)) {
        status = tee->first.operations->end_element(tee->first.state);
    }
    return status == SXML_STATUS_OK
               ? tee->second.operations->end_element(tee->second.state)
               : status;
}

static sxml_status sxml_xml_tee_add_text(void *state, const sxml_text_event *event) {
    const sxml_xml_tee *tee = sxml_xml_tee_from_state(state);
    sxml_status status = SXML_STATUS_OK;
    if (sxml_xml_tee_has_first(tee)) {
        status = tee->first.operations->add_text(tee->first.state, event);
    }
    return status == SXML_STATUS_OK
               ? tee->second.operations->add_text(tee->second.state, event)
               : status;
}

static sxml_status sxml_xml_tee_add_comment(void *state,
                                            const sxml_comment_event *event) {
    const sxml_xml_tee *tee = sxml_xml_tee_from_state(state);
    sxml_status status = SXML_STATUS_OK;
    if (sxml_xml_tee_has_first(tee)) {
        status = tee->first.operations->add_comment(tee->first.state, event);
    }
    return status == SXML_STATUS_OK
               ? tee->second.operations->add_comment(tee->second.state, event)
               : status;
}

static sxml_status sxml_xml_tee_add_processing_instruction(
    void *state, const sxml_processing_instruction_event *event) {
    const sxml_xml_tee *tee = sxml_xml_tee_from_state(state);
    sxml_status status = SXML_STATUS_OK;
    if (sxml_xml_tee_has_first(tee)) {
        status = tee->first.operations->add_processing_instruction(tee->first.state,
                                                                   event);
    }
    return status == SXML_STATUS_OK
               ? tee->second.operations->add_processing_instruction(
                     tee->second.state, event)
               : status;
}

static sxml_status sxml_xml_tee_add_unparsed_entity(void *state,
                                                    const sxml_unparsed_entity *entity) {
    const sxml_xml_tee *tee = sxml_xml_tee_from_state(state);
    sxml_status status = SXML_STATUS_OK;
    if (sxml_xml_tee_has_first(tee)) {
        status = tee->first.operations->add_unparsed_entity(tee->first.state, entity);
    }
    return status == SXML_STATUS_OK ? tee->second.operations->add_unparsed_entity(
                                          tee->second.state, entity)
                                    : status;
}

static const sxml_tree_builder_operations sxml_xml_tee_operations = {
    .start_document = sxml_xml_tee_start_document,
    .end_document = sxml_xml_tee_end_document,
    .start_element = sxml_xml_tee_start_element,
    .add_namespace_binding = sxml_xml_tee_add_namespace_binding,
    .add_attribute = sxml_xml_tee_add_attribute,
    .end_element = sxml_xml_tee_end_element,
    .add_text = sxml_xml_tee_add_text,
    .add_comment = sxml_xml_tee_add_comment,
    .add_processing_instruction = sxml_xml_tee_add_processing_instruction,
    .add_unparsed_entity = sxml_xml_tee_add_unparsed_entity,
};

/* Adds a prefix binding for resolving the DTD's names, unless the prefix
   has one already: the first wins. */
static sxml_status sxml_xml_add_prefix_binding(sxml_xml_document *document,
                                               const sxml_dtd_prefix_binding *binding) {
    sxml_xml_array *bindings = &document->parser->prefix_bindings;
    for (size_t binding_index = 0; binding_index < bindings->element_count;
         binding_index++) {
        if (sxml_byte_view_equals(
                ((const sxml_dtd_prefix_binding *)sxml_xml_array_at(bindings,
                                                                    binding_index))
                    ->namespace_prefix,
                binding->namespace_prefix)) {
            return SXML_STATUS_OK;
        }
    }
    void *element = NULL;
    const sxml_status status = sxml_xml_array_push(bindings, &element);
    if (status != SXML_STATUS_OK) {
        return sxml_xml_fail_resource(document, status);
    }
    *(sxml_dtd_prefix_binding *)element = *binding;
    return SXML_STATUS_OK;
}

/* The prefix an xmlns or xmlns:p attribute name declares; false for other
   names. */
typedef struct sxml_xml_declared_prefix {
    bool is_declaration;
    sxml_byte_view namespace_prefix;
} sxml_xml_declared_prefix;

static sxml_xml_declared_prefix sxml_xml_declared_prefix_of(sxml_byte_view name) {
    if (sxml_byte_view_equals(name, SXML_XML_TEXT("xmlns"))) {
        return (sxml_xml_declared_prefix){
            .is_declaration = true,
            .namespace_prefix = {.bytes = NULL, .byte_count = 0}};
    }
    if (name.byte_count > 6 && memcmp(name.bytes, "xmlns:", 6) == 0) {
        return (sxml_xml_declared_prefix){
            .is_declaration = true,
            .namespace_prefix = {.bytes = name.bytes + 6,
                                 .byte_count = name.byte_count - 6}};
    }
    return (sxml_xml_declared_prefix){
        .is_declaration = false, .namespace_prefix = {.bytes = NULL, .byte_count = 0}};
}

/*
 * The bindings the DTD's names are resolved with: the namespace
 * declarations written on the document element (the plain text of their
 * values), then the defaults of xmlns attributes in the DTD, the first
 * binding of a prefix winning; the xml prefix is bound as always.
 */
static sxml_status sxml_xml_collect_prefix_bindings(sxml_xml_document *document,
                                                    sxml_xml_frame_node root) {
    document->parser->prefix_bindings.element_count = 0;
    sxml_status status = sxml_xml_add_prefix_binding(
        document, &(sxml_dtd_prefix_binding){.namespace_prefix = SXML_XML_TEXT("xml"),
                                             .namespace_uri = sxml_xml_namespace_text});
    const TSNode element = sxml_xml_field(root.node, SXML_XML_FIELD_ROOT);
    const TSNode tag = ts_node_named_child(element, 0);
    sxml_xml_children attributes = {.first_index = 0, .child_count = 0};
    if (status == SXML_STATUS_OK && !ts_node_is_null(tag)) {
        status = sxml_xml_fail_resource(
            document, sxml_xml_list_children(document, tag, &attributes));
    }
    for (size_t attribute_index = 0;
         status == SXML_STATUS_OK && attribute_index < attributes.child_count;
         attribute_index++) {
        const TSNode attribute = sxml_xml_child_at(document, attributes, attribute_index);
        if (sxml_xml_type_of(attribute) != SXML_XML_NODE_ATTRIBUTE) {
            continue;
        }
        const sxml_xml_declared_prefix declared = sxml_xml_declared_prefix_of(
            sxml_xml_field_text(root.frame, attribute, SXML_XML_FIELD_NAME));
        const TSNode value = sxml_xml_field(attribute, SXML_XML_FIELD_VALUE);
        if (!declared.is_declaration || ts_node_named_child_count(value) > 1) {
            continue;
        }
        const TSNode text = ts_node_named_child(value, 0);
        status = sxml_xml_add_prefix_binding(
            document,
            &(sxml_dtd_prefix_binding){
                .namespace_prefix = declared.namespace_prefix,
                .namespace_uri = ts_node_is_null(text)
                                     ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
                                     : sxml_xml_node_text(root.frame, text)});
    }
    sxml_xml_release_children(document, attributes);
    const sxml_xml_dtd *dtd = &document->parser->dtd;
    for (uint32_t number = 1;
         status == SXML_STATUS_OK && number <= dtd->attribute_definitions.element_count;
         number++) {
        const sxml_xml_attribute_definition *definition =
            sxml_xml_dtd_get_definition(dtd, number);
        const sxml_xml_declared_prefix declared = sxml_xml_declared_prefix_of(
            sxml_xml_dtd_text(dtd, definition->attribute_name));
        if (declared.is_declaration && definition->has_default_value) {
            status = sxml_xml_add_prefix_binding(
                document,
                &(sxml_dtd_prefix_binding){
                    .namespace_prefix = declared.namespace_prefix,
                    .namespace_uri = sxml_xml_dtd_text(dtd, definition->default_value)});
        }
    }
    return status;
}

/*
 * Starts DTD validation once the DTD is read (D199): the schema documents
 * written from the DTD go to the validator, whose checking builder then
 * receives every event with the parse's builder.
 */
static sxml_status sxml_xml_start_validation(sxml_xml_document *document,
                                             sxml_xml_frame_node root) {
    sxml_xml_parser *parser = document->parser;
    sxml_status status = sxml_xml_collect_prefix_bindings(document, root);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    parser->schema_text.byte_count = 0;
    parser->schema_documents.element_count = 0;
    parser->schema_views.element_count = 0;
    status = sxml_dtd_write_schemas(&(sxml_dtd_schema_request){
        .dtd = &parser->dtd,
        .bindings = {.bindings = parser->prefix_bindings.elements == NULL
                                     ? NULL
                                     : sxml_xml_array_at(&parser->prefix_bindings, 0),
                     .binding_count = parser->prefix_bindings.element_count},
        .output = &parser->schema_text,
        .documents = &parser->schema_documents});
    for (size_t document_index = 0;
         status == SXML_STATUS_OK &&
         document_index < parser->schema_documents.element_count;
         document_index++) {
        void *element = NULL;
        status = sxml_xml_array_push(&parser->schema_views, &element);
        if (status == SXML_STATUS_OK) {
            *(sxml_byte_view *)element = sxml_xml_buffer_view(
                &parser->schema_text,
                *(const sxml_byte_range *)sxml_xml_array_at(&parser->schema_documents,
                                                            document_index));
        }
    }
    if (status != SXML_STATUS_OK) {
        return sxml_xml_fail_resource(document, status);
    }
    sxml_tree_builder checking_builder = {.state = NULL, .operations = NULL};
    const sxml_xml_validator validator = document->request->validator;
    status = validator.operations->start_validation(
        validator.state,
        &(sxml_xml_dtd_schema_view){
            .schema_texts = sxml_xml_array_at(&parser->schema_views, 0),
            .schema_text_count = parser->schema_views.element_count},
        &checking_builder);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (checking_builder.operations == NULL) {
        return SXML_STATUS_OK;
    }
    document->tee = (sxml_xml_tee){.first = document->builder, .second = checking_builder};
    document->builder =
        (sxml_tree_builder){.state = &document->tee, .operations = &sxml_xml_tee_operations};
    return SXML_STATUS_OK;
}

/* A validity error at the document type declaration. */
static sxml_status sxml_xml_report_declaration(const sxml_xml_document *document,
                                               sxml_byte_view description) {
    return sxml_xml_report_validity(
        document,
        (sxml_xml_validity_failure){
            .description = description,
            .location = {.source_name = document->request->source_name,
                         .line_number = 1,
                         .column_number = 1,
                         .byte_offset = 0}});
}

/* True when the space-separated list holds a token twice. */
static bool sxml_xml_has_duplicate_token(sxml_byte_view list) {
    sxml_byte_view rest = list;
    sxml_byte_view token = {.bytes = NULL, .byte_count = 0};
    while (sxml_xml_text_next_token(&rest, &token)) {
        sxml_byte_view later_rest = rest;
        sxml_byte_view later = {.bytes = NULL, .byte_count = 0};
        while (sxml_xml_text_next_token(&later_rest, &later)) {
            if (sxml_byte_view_equals(token, later)) {
                return true;
            }
        }
    }
    return false;
}

static bool sxml_xml_is_notation_declared(const sxml_xml_dtd *dtd,
                                          sxml_byte_view name) {
    for (size_t notation_index = 0; notation_index < dtd->notations.element_count;
         notation_index++) {
        if (sxml_byte_view_equals(
                sxml_xml_dtd_text(
                    dtd, ((const sxml_xml_notation *)sxml_xml_array_at(
                              &dtd->notations, notation_index))
                             ->notation_name),
                name)) {
            return true;
        }
    }
    return false;
}

/*
 * The validity constraints on the declarations that no schema component
 * checks (XML 1.0, 3.3.1 and 4.2.2): ID Attribute Default, One ID per
 * Element Type, One Notation Per Element Type, No Notation on Empty
 * Element, Notation Attributes, No Duplicate Tokens, Notation Declared.
 */
static sxml_status sxml_xml_check_declarations(sxml_xml_document *document) {
    const sxml_xml_dtd *dtd = &document->parser->dtd;
    sxml_status status = SXML_STATUS_OK;
    for (size_t element_index = 0;
         status == SXML_STATUS_OK && element_index < dtd->elements.element_count;
         element_index++) {
        const sxml_xml_element_entry *element =
            sxml_xml_dtd_element_at(dtd, (uint32_t)element_index);
        size_t identifier_count = 0;
        size_t notation_count = 0;
        for (uint32_t number = element->first_definition;
             status == SXML_STATUS_OK && number != 0;) {
            const sxml_xml_attribute_definition *definition =
                sxml_xml_dtd_get_definition(dtd, number);
            number = definition->next_definition;
            if (definition->attribute_type == SXML_XML_ATTRIBUTE_TYPE_ID) {
                identifier_count += 1;
                if (definition->has_default_value) {
                    status = sxml_xml_report_declaration(
                        document, SXML_XML_TEXT("VC: ID Attribute Default: an ID "
                                                "attribute must be #IMPLIED or "
                                                "#REQUIRED (XML 1.0, section "
                                                "3.3.1)"));
                }
            }
            if (status == SXML_STATUS_OK &&
                (definition->attribute_type == SXML_XML_ATTRIBUTE_TYPE_NOTATION ||
                 definition->attribute_type == SXML_XML_ATTRIBUTE_TYPE_ENUMERATION) &&
                sxml_xml_has_duplicate_token(
                    sxml_xml_dtd_text(dtd, definition->enumeration))) {
                status = sxml_xml_report_declaration(
                    document,
                    SXML_XML_TEXT("VC: No Duplicate Tokens: a name appears twice "
                                  "in an enumerated attribute type (XML 1.0, "
                                  "section 3.3.1)"));
            }
            if (status != SXML_STATUS_OK ||
                definition->attribute_type != SXML_XML_ATTRIBUTE_TYPE_NOTATION) {
                continue;
            }
            notation_count += 1;
            sxml_byte_view rest = sxml_xml_dtd_text(dtd, definition->enumeration);
            sxml_byte_view name = {.bytes = NULL, .byte_count = 0};
            while (status == SXML_STATUS_OK && sxml_xml_text_next_token(&rest, &name)) {
                if (!sxml_xml_is_notation_declared(dtd, name)) {
                    status = sxml_xml_report_declaration(
                        document,
                        SXML_XML_TEXT("VC: Notation Attributes: a notation the "
                                      "attribute type names is not declared (XML "
                                      "1.0, section 3.3.1)"));
                }
            }
        }
        if (status == SXML_STATUS_OK && identifier_count > 1) {
            status = sxml_xml_report_declaration(
                document, SXML_XML_TEXT("VC: One ID per Element Type: an element "
                                        "type has two ID attributes (XML 1.0, "
                                        "section 3.3.1)"));
        }
        if (status == SXML_STATUS_OK && notation_count > 1) {
            status = sxml_xml_report_declaration(
                document,
                SXML_XML_TEXT("VC: One Notation Per Element Type: an element type "
                              "has two NOTATION attributes (XML 1.0, section "
                              "3.3.1)"));
        }
        if (status == SXML_STATUS_OK && notation_count != 0 && element->is_declared &&
            element->content_kind == SXML_XML_CONTENT_KIND_EMPTY) {
            status = sxml_xml_report_declaration(
                document,
                SXML_XML_TEXT("VC: No Notation on Empty Element: an EMPTY element "
                              "type has a NOTATION attribute (XML 1.0, section "
                              "3.3.1)"));
        }
    }
    for (size_t entity_index = 0;
         status == SXML_STATUS_OK && entity_index < dtd->entities.element_count;
         entity_index++) {
        const sxml_xml_entity *entity =
            sxml_xml_dtd_get_entity(dtd, (uint32_t)entity_index);
        if (entity->entity_kind == SXML_XML_ENTITY_KIND_UNPARSED &&
            !sxml_xml_is_notation_declared(
                dtd, sxml_xml_dtd_text(dtd, entity->notation_name))) {
            status = sxml_xml_report_declaration(
                document,
                SXML_XML_TEXT("VC: Notation Declared: the notation of an unparsed "
                              "entity is not declared (XML 1.0, section 4.2.2)"));
        }
    }
    return status;
}

/*
 * What validation checks before the first event: a document type
 * declaration (XML 1.0, 2.8: a valid document has one, and F&O 4.0's
 * dtd-validation requires one), the declarations, then the validator is
 * started.
 */
static sxml_status sxml_xml_prepare_validation(sxml_xml_document *document,
                                               sxml_xml_frame_node root) {
    const sxml_xml_dtd *dtd = &document->parser->dtd;
    for (size_t element_index = 0;
         !document->tracks_contents && element_index < dtd->elements.element_count;
         element_index++) {
        document->tracks_contents =
            sxml_xml_dtd_element_at(dtd, (uint32_t)element_index)->is_declared;
    }
    document->tracks_contents = document->tracks_contents || document->validates;
    if (!document->validates) {
        return SXML_STATUS_OK;
    }
    sxml_status status = SXML_STATUS_OK;
    if (!document->has_doctype) {
        status = sxml_xml_report_declaration(
            document,
            SXML_XML_TEXT("VC: a valid document has a document type declaration "
                          "(XML 1.0, section 2.8)"));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_check_declarations(document);
    }
    return status == SXML_STATUS_OK ? sxml_xml_start_validation(document, root)
                                    : status;
}

/* ---------------------------------------------------- Whole document */

/*
 * Reads the version from the XML declaration (15) and applies XML 1.1's
 * character and line-end rules to the decoded text: no restricted
 * character may appear literally (XML 1.1, [1] document and 2.2), and NEL
 * and LINE SEPARATOR after the declaration are translated (2.11; in the
 * declaration itself they stay, and the grammar refuses them). The
 * translation shrinks the text, so it is done in the parser's copy;
 * diagnostics then give positions in the translated text.
 */
static sxml_status sxml_xml_apply_version(sxml_xml_document *document) {
    const sxml_xml_declared_version declared =
        sxml_xml_read_declared_version(document->text);
    document->version = declared.version;
    if (declared.version != SXML_XML_VERSION_1_1) {
        return SXML_STATUS_OK;
    }
    const size_t restricted_offset =
        sxml_xml_find_restricted_character(document->text);
    if (restricted_offset != document->text.byte_count) {
        return sxml_xml_fail(
            document,
            (sxml_xml_failure){
                .status = SXML_STATUS_MALFORMED_INPUT,
                .description = SXML_XML_TEXT(
                    "a control character must be written as a character "
                    "reference in XML 1.1 ([2a] RestrictedChar, XML 1.1, "
                    "section 2.2)"),
                .location = sxml_xml_location_at(document, restricted_offset)});
    }
    const sxml_byte_view after_declaration = {
        .bytes = document->text.bytes + declared.declaration_end_offset,
        .byte_count =
            document->text.byte_count - declared.declaration_end_offset};
    if (!sxml_xml_has_version_1_1_line_end(after_declaration)) {
        return SXML_STATUS_OK;
    }
    sxml_xml_buffer *copy = &document->parser->transcoded_text;
    if (document->text.bytes != copy->bytes) {
        copy->byte_count = 0;
        const sxml_status status =
            sxml_xml_buffer_append(copy, document->text);
        if (status != SXML_STATUS_OK) {
            return sxml_xml_fail_resource(document, status);
        }
    }
    sxml_xml_translate_version_1_1_line_ends(copy,
                                             declared.declaration_end_offset);
    document->text = (sxml_byte_view){.bytes = copy->bytes,
                                      .byte_count = copy->byte_count};
    document->tracker = sxml_xml_position_tracker_start(
        (sxml_xml_position_tracker_configuration){
            .text = document->text,
            .input_encoding = document->detection.encoding,
            .has_byte_order_mark =
                document->detection.has_utf8_byte_order_mark ||
                document->detection.has_utf16_byte_order_mark});
    return SXML_STATUS_OK;
}

/* Decodes the input into document->text (1: encodings, 4.3.3). */
static sxml_status sxml_xml_decode(sxml_xml_document *document) {
    const sxml_xml_parse_request *request = document->request;
    sxml_xml_failure failure = {
        .status = SXML_STATUS_UNSUPPORTED_FEATURE,
        .description = SXML_XML_TEXT(
            "the document's encoding is not one sxml reads: UTF-8, UTF-16, "
            "ISO-8859-1 or US-ASCII (XML 1.0, section 4.3.3)"),
        .location = {.source_name = request->source_name,
                     .line_number = 1,
                     .column_number = 1,
                     .byte_offset = 0}};
    sxml_status status = sxml_xml_detect_encoding(
        &(sxml_xml_encoding_request){.input = request->input,
                                     .encoding_name = request->encoding_name},
        &document->detection);
    if (status != SXML_STATUS_OK) {
        return sxml_xml_fail(document, failure);
    }
    const sxml_xml_encoding encoding = document->detection.encoding;
    sxml_xml_buffer *transcoded = &document->parser->transcoded_text;
    transcoded->byte_count = 0;
    sxml_xml_transcoding_failure transcoding_failure = {.input_byte_offset = 0};
    size_t failed_text_offset = 0;
    switch (encoding) {
    case SXML_XML_ENCODING_UTF8: {
        document->text = request->input;
        sxml_utf8_validation validation = {
            .valid_byte_count = 0, .codepoint_count = 0, .is_ascii = false};
        status = sxml_utf8_validate(request->input, &validation);
        failed_text_offset = validation.valid_byte_count;
        failure.description = SXML_XML_TEXT(
            "the bytes are not well-formed UTF-8 (XML 1.0, section 4.3.3)");
        break;
    }
    case SXML_XML_ENCODING_US_ASCII:
        document->text = request->input;
        status = sxml_xml_check_ascii(request->input, &transcoding_failure);
        failed_text_offset = transcoding_failure.input_byte_offset;
        failure.description = SXML_XML_TEXT(
            "a byte is not US-ASCII, the declared encoding (XML 1.0, section "
            "4.3.3)");
        break;
    case SXML_XML_ENCODING_UTF16_LITTLE_ENDIAN:
    case SXML_XML_ENCODING_UTF16_BIG_ENDIAN:
    case SXML_XML_ENCODING_ISO_8859_1:
        status = sxml_xml_transcode(
            &(sxml_xml_transcoding_request){.input = request->input,
                                            .encoding = encoding},
            transcoded, &transcoding_failure);
        document->text = (sxml_byte_view){.bytes = transcoded->bytes,
                                          .byte_count = transcoded->byte_count};
        failed_text_offset = transcoded->byte_count;
        failure.description = SXML_XML_TEXT(
            "the bytes are not well-formed UTF-16 (XML 1.0, section 4.3.3)");
        break;
    }
    document->tracker = sxml_xml_position_tracker_start((
        sxml_xml_position_tracker_configuration){
        .text = document->text,
        .input_encoding = encoding,
        .has_byte_order_mark = document->detection.has_utf8_byte_order_mark ||
                               document->detection.has_utf16_byte_order_mark});
    if (status == SXML_STATUS_OUT_OF_MEMORY ||
        status == SXML_STATUS_ARITHMETIC_OVERFLOW) {
        return sxml_xml_fail_resource(document, SXML_STATUS_OUT_OF_MEMORY);
    }
    if (status != SXML_STATUS_OK) {
        failure.status = status;
        failure.location = sxml_xml_location_at(document, failed_text_offset);
        if (encoding != SXML_XML_ENCODING_UTF8) {
            failure.location.byte_offset =
                transcoding_failure.input_byte_offset;
        }
        return sxml_xml_fail(document, failure);
    }
    if (document->text.byte_count > UINT32_MAX - 64) {
        return sxml_xml_fail_resource(document, SXML_STATUS_LIMIT_EXCEEDED);
    }
    /* U+0000 is no [2] Char; tree-sitter would also read it as the end. */
    const uint8_t *null_byte =
        document->text.byte_count == 0
            ? NULL
            : memchr(document->text.bytes, 0, document->text.byte_count);
    if (null_byte != NULL) {
        failure.status = SXML_STATUS_MALFORMED_INPUT;
        failure.description =
            SXML_XML_TEXT("U+0000 is not a character of XML 1.0 ([2] Char)");
        failure.location = sxml_xml_location_at(
            document, (size_t)(null_byte - document->text.bytes));
        return sxml_xml_fail(document, failure);
    }
    return sxml_xml_apply_version(document);
}

/*
 * The XML declaration ([23]): standalone, and an encoding declaration
 * that agrees with the bytes (4.3.3, Appendix F). The version was read
 * before parsing (sxml_xml_apply_version).
 */
static sxml_status sxml_xml_check_declaration(sxml_xml_document *document,
                                              const sxml_xml_frame *frame,
                                              TSNode root) {
    const TSNode declaration = sxml_xml_field(root, SXML_XML_FIELD_DECLARATION);
    sxml_byte_view encoding_name = {.bytes = NULL, .byte_count = 0};
    if (!ts_node_is_null(declaration)) {
        document->is_standalone = sxml_byte_view_equals(
            sxml_xml_field_text(frame, declaration, SXML_XML_FIELD_STANDALONE),
            SXML_XML_TEXT("yes"));
        encoding_name =
            sxml_xml_field_text(frame, declaration, SXML_XML_FIELD_ENCODING);
    }
    if (document->request->encoding_name.byte_count != 0) {
        return SXML_STATUS_OK;
    }
    const bool is_consistent = sxml_xml_encoding_is_consistent(
        &(sxml_xml_encoding_check){.detection = &document->detection,
                                   .encoding_name = encoding_name});
    if (is_consistent) {
        return SXML_STATUS_OK;
    }
    return sxml_xml_fail(
        document,
        (sxml_xml_failure){
            .status = SXML_STATUS_MALFORMED_INPUT,
            .description = SXML_XML_TEXT(
                "the encoding declaration contradicts the encoding of the "
                "bytes (XML 1.0, section 4.3.3 and Appendix F)"),
            .location = sxml_xml_location_at(
                document, ts_node_is_null(declaration)
                              ? 0
                              : sxml_xml_document_offset_of(
                                    frame, ts_node_start_byte(declaration)))});
}

/*
 * Reads the external subset that the document type declaration names
 * ([30] extSubset) through the entity resolver and pushes a frame over its
 * declarations. It is kept in the entity table as a parameter entity
 * whose name, "[dtd]", is no Name, so that no reference can reach it.
 */
static sxml_status
sxml_xml_read_external_subset(sxml_xml_document *document,
                              const sxml_xml_frame_node *doctype) {
    const sxml_xml_frame *frame = doctype->frame;
    const TSNode external_id =
        sxml_xml_field(doctype->node, SXML_XML_FIELD_EXTERNAL_ID);
    const sxml_source_location location =
        sxml_xml_node_location(document, frame, doctype->node);
    sxml_xml_entity entity = {
        .entity_kind = SXML_XML_ENTITY_KIND_EXTERNAL_PARSED,
        .is_parameter_entity = true,
        .entity_name = {.byte_offset = 0, .byte_count = 0},
        .replacement_text = {.byte_offset = 0, .byte_count = 0},
        .system_identifier = {.byte_offset = 0, .byte_count = 0},
        .public_identifier = {.byte_offset = 0, .byte_count = 0},
        .notation_name = {.byte_offset = 0, .byte_count = 0},
        .is_declared_in_external_markup = true,
        .base_uri = frame->base_uri,
        .load_state = SXML_XML_LOAD_STATE_NOT_LOADED,
        .external_text =
            sxml_xml_buffer_empty(document->parser->configuration.allocator),
        .content_offset = 0,
        .entity_uri = {.byte_offset = 0, .byte_count = 0},
        .is_being_expanded = false,
        .syntax_tree = NULL,
    };
    sxml_status status = sxml_xml_store(document, SXML_XML_TEXT("[dtd]"),
                                        &entity.entity_name);
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_store(document,
                                sxml_xml_literal_content(sxml_xml_field_text(
                                    frame, external_id, SXML_XML_FIELD_SYSTEM_ID)),
                                &entity.system_identifier);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_store(document,
                                sxml_xml_literal_content(sxml_xml_field_text(
                                    frame, external_id, SXML_XML_FIELD_PUBLIC_ID)),
                                &entity.public_identifier);
    }
    sxml_xml_lookup added = {.is_found = false, .entry_index = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_fail_resource(
            document,
            sxml_xml_dtd_add_entity(&document->parser->dtd, &entity, &added));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_load_external_entity(
            document,
            &(sxml_xml_external_load){
                .entity_index = added.entry_index,
                .purpose = SXML_XML_EXTERNAL_ENTITY_PURPOSE_SUBSET,
                .location = location});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_xml_entity *loaded =
        sxml_xml_dtd_get_entity(&document->parser->dtd, added.entry_index);
    if (loaded->load_state != SXML_XML_LOAD_STATE_LOADED) {
        return SXML_STATUS_OK;
    }
    status = sxml_xml_count_expansion(
        document,
        (sxml_xml_expansion_count){.byte_count = loaded->external_text.byte_count,
                                   .location = location});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xml_frame pushed = {
        .frame_kind = SXML_XML_FRAME_KIND_PARAMETER_ENTITY,
        .syntax_tree = NULL,
        .cursor = {.tree = NULL, .id = NULL, .context = {0, 0, 0}},
        .text = sxml_xml_entity_text(
            document, (sxml_xml_entity_text_request){
                          .entity_index = added.entry_index,
                          .skips_text_declaration = false}),
        .prefix_byte_count =
            (uint32_t)sxml_xml_external_subset_marker.byte_count,
        .owned_text =
            sxml_xml_buffer_empty(document->parser->configuration.allocator),
        .has_entity = true,
        .entity_index = added.entry_index,
        .reference_location = location,
        .is_external_context = true,
        .starts_external_entity = true,
        .base_uri = loaded->entity_uri,
        .needs_advance = false,
        .descends = false,
        .cursor_depth = 0,
    };
    const sxml_xml_wrapped_text wrapped = {
        .prefix = sxml_xml_external_subset_marker,
        .text = pushed.text,
        .suffix = {.bytes = NULL, .byte_count = 0}};
    status = sxml_xml_parse_wrapped(document, &wrapped, &pushed.syntax_tree);
    if (status == SXML_STATUS_OK && pushed.syntax_tree == NULL) {
        status = sxml_xml_fail(
            document,
            (sxml_xml_failure){
                .status = SXML_STATUS_MALFORMED_INPUT,
                .description = SXML_XML_TEXT(
                    "the external subset must match [30] extSubset (XML 1.0, "
                    "section 2.8)"),
                .location = location});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const TSNode subset = sxml_xml_field(ts_tree_root_node(pushed.syntax_tree),
                                         SXML_XML_FIELD_EXTERNAL_SUBSET);
    if (ts_node_is_null(subset)) {
        ts_tree_delete(pushed.syntax_tree);
        return SXML_STATUS_OK;
    }
    return sxml_xml_push_frame(document, &pushed, subset);
}

/* The document type declaration ([28]): the facts that decide which
   constraints apply, then the internal subset and the external subset. */
static sxml_status sxml_xml_process_doctype(sxml_xml_document *document,
                                            const sxml_xml_frame *frame,
                                            TSNode root) {
    const TSNode doctype = sxml_xml_field(root, SXML_XML_FIELD_DOCTYPE);
    document->has_doctype = !ts_node_is_null(doctype);
    if (!document->has_doctype) {
        document->requires_declared_entities = true;
        return SXML_STATUS_OK;
    }
    sxml_status status = sxml_xml_check_qualified_name(
        document, frame, sxml_xml_field(doctype, SXML_XML_FIELD_NAME));
    if (status != SXML_STATUS_OK) {
        return status;
    }
    document->doctype_name =
        sxml_xml_field_text(frame, doctype, SXML_XML_FIELD_NAME);
    document->has_external_subset =
        !ts_node_is_null(sxml_xml_field(doctype, SXML_XML_FIELD_EXTERNAL_ID));
    const TSNode subset =
        sxml_xml_field(doctype, SXML_XML_FIELD_INTERNAL_SUBSET);
    if (!ts_node_is_null(subset)) {
        TSTreeCursor cursor = ts_tree_cursor_new(subset);
        for (bool has_child = ts_tree_cursor_goto_first_child(&cursor);
             has_child; has_child = ts_tree_cursor_goto_next_sibling(&cursor)) {
            if (sxml_xml_type_of(ts_tree_cursor_current_node(&cursor)) ==
                SXML_XML_NODE_PARAMETER_ENTITY_REFERENCE) {
                document->has_parameter_entity_references = true;
                break;
            }
        }
        ts_tree_cursor_delete(&cursor);
    }
    /* WFC: Entity Declared (4.1). */
    document->requires_declared_entities =
        document->is_standalone || (!document->has_external_subset &&
                                    !document->has_parameter_entity_references);
    const size_t base_frame_count = sxml_xml_frame_count(document);
    if (!ts_node_is_null(subset)) {
        sxml_xml_frame pushed = *frame;
        pushed.syntax_tree = NULL;
        status = sxml_xml_push_frame(document, &pushed, subset);
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_run_frames(
                document, (sxml_xml_walk){.walks_declarations = true,
                                          .base_frame_count = base_frame_count});
        }
    }
    if (status != SXML_STATUS_OK || !document->has_external_subset ||
        !document->reads_external_entities) {
        return status;
    }
    /* The external subset is read after the internal subset, whose
       declarations take precedence (2.8). */
    status = sxml_xml_read_external_subset(
        document, &(sxml_xml_frame_node){.frame = frame, .node = doctype});
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_run_frames(
            document, (sxml_xml_walk){.walks_declarations = true,
                                      .base_frame_count = base_frame_count});
    }
    return status;
}

/* start_document, the unparsed entities, the content, end_document. */
static sxml_status sxml_xml_send_document(sxml_xml_document *document,
                                          const sxml_xml_frame *frame,
                                          TSNode root) {
    const sxml_xml_parse_request *request = document->request;
    if (sxml_xml_has_builder(document)) {
        sxml_status status = document->builder.operations->start_document(
            document->builder.state,
            &(sxml_document_start_event){
                .document_uri = request->document_uri,
                .base_uri = request->base_uri,
                .location = sxml_xml_location_at(document, 0)});
        const sxml_xml_dtd *dtd = &document->parser->dtd;
        for (size_t entity_index = 0;
             status == SXML_STATUS_OK &&
             entity_index < dtd->entities.element_count;
             entity_index++) {
            const sxml_xml_entity *entity =
                sxml_xml_dtd_get_entity(dtd, (uint32_t)entity_index);
            if (entity->entity_kind != SXML_XML_ENTITY_KIND_UNPARSED ||
                entity->is_parameter_entity) {
                continue;
            }
            /* dm:unparsed-entity-system-id is the system identifier made
               absolute against the base URI of its declaration (XDM 4.0,
               6.1.3; XML 1.0, 4.2.2). */
            sxml_uri_resolution resolution = {.uri = {.bytes = NULL, .byte_count = 0},
                                              .memory = NULL};
            status = sxml_uri_resolve(
                &(sxml_uri_resolution_request){
                    .base_uri = sxml_xml_dtd_text(dtd, entity->base_uri),
                    .reference = sxml_xml_dtd_text(dtd, entity->system_identifier),
                    .allocator = document->parser->configuration.allocator},
                &resolution);
            if (status != SXML_STATUS_OK) {
                return sxml_xml_fail_resource(document, status);
            }
            status = document->builder.operations->add_unparsed_entity(
                document->builder.state,
                &(sxml_unparsed_entity){
                    .entity_name = sxml_xml_dtd_text(dtd, entity->entity_name),
                    .system_identifier = resolution.uri,
                    .public_identifier =
                        sxml_xml_dtd_text(dtd, entity->public_identifier)});
            sxml_allocator_release(document->parser->configuration.allocator,
                                   resolution.memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    const size_t base_frame_count = sxml_xml_frame_count(document);
    /* The head's tree goes with its frame: the DTD is read, and the
       content walk frees each segment's tree when it ends (D222). */
    sxml_xml_frame pushed = *frame;
    pushed.syntax_tree = document->syntax_tree;
    document->syntax_tree = NULL;
    document->content_base_frame_count = base_frame_count;
    sxml_status status = sxml_xml_push_frame(document, &pushed, root);
    if (status != SXML_STATUS_OK || document->is_incremental) {
        /* An incremental parse walks the content when resumed. */
        return status;
    }
    status = sxml_xml_run_frames(
        document, (sxml_xml_walk){.walks_declarations = false,
                                  .base_frame_count = base_frame_count});
    if (status == SXML_STATUS_OK && sxml_xml_has_builder(document)) {
        status =
            document->builder.operations->end_document(document->builder.state);
    }
    return status;
}

/*
 * The steps after decoding: the head (D222), the prolog and the start tag
 * of the document element, is parsed and walked; the content segments
 * follow in the content walk.
 */
static sxml_status sxml_xml_walk_document(sxml_xml_document *document) {
    const sxml_byte_view base_uri = document->request->base_uri.byte_count != 0
                                        ? document->request->base_uri
                                        : document->request->document_uri;
    sxml_byte_range base_range = {.byte_offset = 0, .byte_count = 0};
    sxml_status status = sxml_xml_store(document, base_uri, &base_range);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* The head frame's template: its cursor is made when pushed. */
    const size_t head_end_offset = sxml_xml_find_head_end(document->text);
    const sxml_xml_frame frame = {
        .frame_kind = SXML_XML_FRAME_KIND_DOCUMENT,
        .syntax_tree = NULL,
        .cursor = {.tree = NULL, .id = NULL, .context = {0, 0, 0}},
        .text = {.bytes = document->text.bytes, .byte_count = head_end_offset},
        .prefix_byte_count = (uint32_t)sxml_xml_document_head_marker.byte_count,
        .document_offset = 0,
        .is_content_segment = false,
        .owned_text =
            sxml_xml_buffer_empty(document->parser->configuration.allocator),
        .has_entity = false,
        .entity_index = 0,
        .reference_location = {.source_name = {.bytes = NULL, .byte_count = 0},
                               .line_number = 0,
                               .column_number = 0,
                               .byte_offset = 0},
        .is_external_context = false,
        .starts_external_entity = false,
        .base_uri = base_range,
        .needs_advance = false,
        .descends = false,
        .cursor_depth = 0,
    };
    document->segment_end_offset = head_end_offset;
    const sxml_xml_wrapped_text wrapped = {
        .prefix = sxml_xml_document_head_marker,
        .text = frame.text,
        .suffix = {.bytes = NULL, .byte_count = 0}};
    status = sxml_xml_parse_segment(
        document, (sxml_xml_segment_parse){.wrapped = &wrapped, .frame = &frame},
        &document->syntax_tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const TSNode root = ts_tree_root_node(document->syntax_tree);
    if (ts_node_is_null(sxml_xml_field(root, SXML_XML_FIELD_ROOT))) {
        /* The grammar's other alternatives, which only the parser's own
           markers start (16). */
        return sxml_xml_fail(
            document,
            (sxml_xml_failure){
                .status = SXML_STATUS_MALFORMED_INPUT,
                .description = SXML_XML_TEXT(
                    "not well-formed: a document must have a root element "
                    "(XML 1.0, [1] document)"),
                .location = sxml_xml_location_at(document, 0)});
    }
    status = sxml_xml_check_declaration(document, &frame, root);
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_process_doctype(document, &frame, root);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_prepare_validation(
            document, (sxml_xml_frame_node){.frame = &frame, .node = root});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_send_document(document, &frame, root);
    }
    return status;
}

/* Interns the prefixes namespace processing compares with. */
static sxml_status sxml_xml_prepare_names(sxml_xml_document *document) {
    sxml_name_pool *name_pool = document->parser->configuration.name_pool;
    sxml_status status = sxml_name_pool_intern_namespace_prefix(
        name_pool, SXML_XML_TEXT("xml"), &document->xml_prefix);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_prefix(
            name_pool, SXML_XML_TEXT("xmlns"), &document->xmlns_prefix);
    }
    return sxml_xml_fail_resource(document, status);
}

/* Prepares *document for a parse of request into builder. */
typedef struct sxml_xml_document_start {
    sxml_xml_parser *parser;
    const sxml_xml_parse_request *request;
    sxml_tree_builder builder;
} sxml_xml_document_start;

static void sxml_xml_start_document(const sxml_xml_document_start *start,
                                    sxml_xml_document *document) {
    sxml_xml_parser *parser = start->parser;
    const sxml_xml_parse_request *request = start->request;
    const sxml_tree_builder builder = start->builder;
    sxml_xml_dtd_clear(&parser->dtd);
    parser->namespace_bindings.element_count = 0;
    parser->element_scopes.element_count = 0;
    parser->open_contents.element_count = 0;
    parser->expansions.element_count = 0;
    parser->child_nodes.element_count = 0;
    parser->open_element_names.element_count = 0;
    *document = (sxml_xml_document){
        .parser = parser,
        .request = request,
        .builder = builder,
        .syntax_parser = NULL,
        .syntax_tree = NULL,
        .text = {.bytes = NULL, .byte_count = 0},
        .tracker = sxml_xml_position_tracker_start(
            (sxml_xml_position_tracker_configuration){
                .text = {.bytes = NULL, .byte_count = 0},
                .input_encoding = SXML_XML_ENCODING_UTF8,
                .has_byte_order_mark = false}),
        .detection = {.has_utf8_byte_order_mark = false,
                      .has_utf16_byte_order_mark = false,
                      .encoding = SXML_XML_ENCODING_UTF8,
                      .declared_encoding_name = {.bytes = NULL,
                                                 .byte_count = 0}},
        .has_doctype = false,
        .has_external_subset = false,
        .has_parameter_entity_references = false,
        .is_standalone = false,
        .version = SXML_XML_VERSION_1_0,
        .requires_declared_entities = true,
        .skips_declarations = false,
        .reads_external_entities = request->entity_resolver.operations != NULL &&
                                   request->entity_resolver.operations->resolve != NULL,
        .validates = request->validator.operations != NULL,
        .tracks_contents = false,
        .doctype_name = {.bytes = NULL, .byte_count = 0},
        .tee = {.first = {.state = NULL, .operations = NULL},
                .second = {.state = NULL, .operations = NULL}},
        .expansion_byte_count = 0,
        .element_depth = 0,
        .xml_prefix = {.index = 0},
        .xmlns_prefix = {.index = 0},
        .is_incremental = false,
        .pause_requested = false,
        .content_base_frame_count = 0,
        .segment_end_offset = 0,
    };
}

/* Decodes the document and walks its head (to the start of the content
   for an incremental parse) and its content segments. */
static sxml_status sxml_xml_read_document(sxml_xml_document *document) {
    sxml_status status = sxml_xml_prepare_names(document);
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_decode(document);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    document->syntax_parser = ts_parser_new();
    if (document->syntax_parser == NULL ||
        !ts_parser_set_language(document->syntax_parser, tree_sitter_xml())) {
        status = sxml_xml_fail_resource(document, SXML_STATUS_OUT_OF_MEMORY);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_walk_document(document);
    }
    return status;
}

/* Frees what the parse of document made with the tree-sitter runtime and
   its external texts. */
static void sxml_xml_release_document(sxml_xml_document *document) {
    sxml_xml_pop_frames_to(document, 0);
    sxml_xml_dtd *dtd = &document->parser->dtd;
    for (size_t entity_index = 0; entity_index < dtd->entities.element_count;
         entity_index++) {
        sxml_xml_entity *entity =
            sxml_xml_dtd_get_entity(dtd, (uint32_t)entity_index);
        if (entity->syntax_tree != NULL) {
            ts_tree_delete(entity->syntax_tree);
            entity->syntax_tree = NULL;
        }
    }
    sxml_xml_release_external_texts(document);
    if (document->syntax_tree != NULL) {
        ts_tree_delete(document->syntax_tree);
        document->syntax_tree = NULL;
    }
    if (document->syntax_parser != NULL) {
        ts_parser_delete(document->syntax_parser);
        document->syntax_parser = NULL;
    }
}

static sxml_status
sxml_xml_parse_document(sxml_xml_parser *parser,
                        const sxml_xml_parse_request *request,
                        sxml_tree_builder builder) {
    sxml_xml_document document;
    sxml_xml_start_document(
        &(sxml_xml_document_start){.parser = parser, .request = request, .builder = builder},
        &document);
    const sxml_status status = sxml_xml_read_document(&document);
    sxml_xml_release_document(&document);
    return status;
}

/* --------------------------------------------------- Incremental parses */

struct sxml_xml_incremental_parse {
    sxml_allocator allocator;
    sxml_xml_parse_request request;
    sxml_xml_document document;
    bool is_finished;
};

sxml_status
sxml_xml_incremental_parse_create(const sxml_xml_incremental_parse_request *request,
                                  sxml_xml_incremental_parse **parse) {
    if (request == NULL || parse == NULL || request->parser == NULL ||
        request->parse_request == NULL || request->builder.operations == NULL ||
        sxml_allocator_is_null(request->allocator) ||
        (request->parse_request->input.bytes == NULL &&
         request->parse_request->input.byte_count != 0)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (request->parse_request->validator.operations != NULL) {
        /* Validation needs the whole document (D199). */
        return SXML_STATUS_UNSUPPORTED_FEATURE;
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        request->allocator,
        (sxml_allocation_request){.byte_count = sizeof(sxml_xml_incremental_parse),
                                  .alignment = alignof(sxml_xml_incremental_parse)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xml_incremental_parse *new_parse = memory;
    new_parse->allocator = request->allocator;
    new_parse->request = *request->parse_request;
    new_parse->is_finished = false;
    sxml_xml_start_document(&(sxml_xml_document_start){.parser = request->parser,
                                                       .request = &new_parse->request,
                                                       .builder = request->builder},
                            &new_parse->document);
    new_parse->document.is_incremental = true;
    status = sxml_xml_read_document(&new_parse->document);
    new_parse->document.pause_requested = false;
    if (status != SXML_STATUS_OK) {
        sxml_xml_incremental_parse_destroy(new_parse);
        return status;
    }
    *parse = new_parse;
    return SXML_STATUS_OK;
}

sxml_status sxml_xml_incremental_parse_resume(sxml_xml_incremental_parse *parse,
                                              bool *is_finished) {
    if (parse == NULL || is_finished == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_xml_document *document = &parse->document;
    if (!parse->is_finished) {
        sxml_status status = sxml_xml_run_frames(
            document,
            (sxml_xml_walk){.walks_declarations = false,
                            .base_frame_count = document->content_base_frame_count});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (sxml_xml_frame_count(document) <= document->content_base_frame_count) {
            parse->is_finished = true;
            status = document->builder.operations->end_document(document->builder.state);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
    }
    *is_finished = parse->is_finished;
    return SXML_STATUS_OK;
}

void sxml_xml_incremental_parse_pause(sxml_xml_incremental_parse *parse) {
    if (parse != NULL) {
        parse->document.pause_requested = true;
    }
}

void sxml_xml_incremental_parse_destroy(sxml_xml_incremental_parse *parse) {
    if (parse == NULL) {
        return;
    }
    sxml_xml_release_document(&parse->document);
    sxml_allocator_release(parse->allocator, parse);
}

sxml_status sxml_xml_parser_parse(sxml_xml_parser *parser,
                                  const sxml_xml_parse_request *request,
                                  sxml_tree_builder builder) {
    if (parser == NULL || request == NULL || builder.operations == NULL ||
        (request->input.bytes == NULL && request->input.byte_count != 0)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_xml_parse_document(parser, request, builder);
}

sxml_status sxml_xml_parser_check(sxml_xml_parser *parser,
                                  const sxml_xml_parse_request *request) {
    if (parser == NULL || request == NULL ||
        (request->input.bytes == NULL && request->input.byte_count != 0)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_xml_parse_document(
        parser, request,
        (sxml_tree_builder){.state = NULL, .operations = NULL});
}
