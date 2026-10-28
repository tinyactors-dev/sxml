/*
 * Reading a Schematron schema (src/schematron_internal.h, "Reading"; D315
 * "The path", step 1): the schematron documents, their inclusions (6.2,
 * 5.4.4, 5.4.6), the grammar of Annex A and the constraints of Annex B,
 * and the schematron index. Every error goes to the compilation's report
 * sink and reading goes on without the element in error.
 *
 * Golden files: src/xslt_read.c (an XML vocabulary read from arena trees
 * with locations into what a compiler needs, one explicit stack, D71) and
 * src/schema_read.c (a schema language's grammar checked as its elements
 * are met, errors to a report sink). Clause numbers are of ISO/IEC
 * 19757-3:2025.
 */
#include "schematron_internal.h"

#include "processor_internal.h"
#include "tree_internal.h"
#include "xdm_internal.h"

#include "sxml/xml.h"

#include <stdalign.h>
#include <stddef.h>
#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view no_text = {.bytes = NULL, .byte_count = 0};

/* ------------------------------------------------------------ Arrays */

sxml_status sxml_schematron_array_push(sxml_schematron_compilation *compilation,
                                       sxml_schematron_array *array,
                                       sxml_schematron_array_element element) {
    if (array->count == array->capacity) {
        const size_t new_capacity = array->capacity < 8 ? 8 : array->capacity * 2U;
        size_t byte_count = 0;
        sxml_status status = sxml_size_multiply(new_capacity, element.element_size, &byte_count);
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                compilation->allocator,
                (sxml_allocation_request){.byte_count = byte_count,
                                          .alignment = element.element_alignment},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (array->count > 0) {
            memcpy(memory, array->elements, array->count * element.element_size);
        }
        sxml_allocator_release(compilation->allocator, array->elements);
        array->elements = memory;
        array->capacity = new_capacity;
    }
    memcpy((uint8_t *)array->elements + array->count * element.element_size, element.element,
           element.element_size);
    array->count += 1;
    return SXML_STATUS_OK;
}

void sxml_schematron_array_release(sxml_schematron_compilation *compilation,
                                   sxml_schematron_array *array) {
    sxml_allocator_release(compilation->allocator, array->elements);
    *array = (sxml_schematron_array){.elements = NULL, .count = 0, .capacity = 0};
}

/* The element value, pushed by copy; aligned for any type, as alignof of
   an expression is not standard C. */
#define SXML_SCHEMATRON_ELEMENT_OF(value)                                                     \
    ((sxml_schematron_array_element){                                                         \
        .element = &(value), .element_size = sizeof(value),                                   \
        .element_alignment = alignof(max_align_t)})

static sxml_status push_place(sxml_schematron_compilation *compilation,
                              sxml_schematron_array *array, sxml_schematron_place place) {
    return sxml_schematron_array_push(compilation, array, SXML_SCHEMATRON_ELEMENT_OF(place));
}

static sxml_status push_identified(sxml_schematron_compilation *compilation,
                                   sxml_schematron_array *array,
                                   sxml_schematron_identified identified) {
    return sxml_schematron_array_push(compilation, array, SXML_SCHEMATRON_ELEMENT_OF(identified));
}

const sxml_schematron_identified *sxml_schematron_find_identified(
    const sxml_schematron_array *table, sxml_byte_view identifier) {
    const sxml_schematron_identified *entries = table->elements;
    for (size_t entry_index = 0; entry_index < table->count; entry_index += 1) {
        if (sxml_byte_view_equals(entries[entry_index].identifier, identifier)) {
            return &entries[entry_index];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ Documents */

sxml_schematron_document *sxml_schematron_document_at(
    const sxml_schematron_compilation *compilation, uint32_t document) {
    sxml_schematron_document *documents = compilation->documents.elements;
    return &documents[document];
}

static sxml_node_reference reference_of(const sxml_schematron_compilation *compilation,
                                        sxml_schematron_place place) {
    return (sxml_node_reference){.tree = sxml_schematron_document_at(compilation, place.document)->tree,
                                 .node_index = place.element,
                                 .namespace_number = 0};
}

static sxml_status describe(const sxml_schematron_compilation *compilation,
                            sxml_schematron_place place, sxml_node_description *description) {
    memset(description, 0, sizeof(*description));
    return sxml_node_describe(reference_of(compilation, place), description);
}

sxml_ast_source_span sxml_schematron_element_span(const sxml_schematron_compilation *compilation,
                                                  sxml_schematron_place place) {
    sxml_source_location location;
    memset(&location, 0, sizeof(location));
    if (sxml_node_get_location(reference_of(compilation, place), &location) != SXML_STATUS_OK) {
        location.line_number = 0;
    }
    return (sxml_ast_source_span){
        .byte_offset = location.byte_offset > UINT32_MAX ? UINT32_MAX : (uint32_t)location.byte_offset,
        .byte_count = 0,
        .line_number = location.line_number,
        .column_number = location.column_number};
}

/* Keeps a document node (retained) under uri (copied) as the next
   document; writes its index. */
typedef struct document_addition {
    sxml_item document_node;
    sxml_byte_view uri;
} document_addition;

static sxml_status add_document(sxml_schematron_compilation *compilation,
                                document_addition addition, uint32_t *document) {
    sxml_node_reference node = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0};
    sxml_status status = sxml_item_get_node(addition.document_node, &node);
    if (status != SXML_STATUS_OK) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    void *uri_memory = NULL;
    if (addition.uri.byte_count > 0) {
        status = sxml_allocator_allocate(
            compilation->allocator,
            (sxml_allocation_request){.byte_count = addition.uri.byte_count, .alignment = 1},
            &uri_memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        memcpy(uri_memory, addition.uri.bytes, addition.uri.byte_count);
    }
    const sxml_schematron_document entry = {
        .document_node = addition.document_node,
        .tree = node.tree,
        .uri = {.bytes = uri_memory, .byte_count = addition.uri.byte_count},
        .module = {.value = 0},
        .static_context = {.value = 0},
        .declaration_context = {.value = 0}};
    status = sxml_schematron_array_push(compilation, &compilation->documents,
                                        SXML_SCHEMATRON_ELEMENT_OF(entry));
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(compilation->allocator, uri_memory);
        return status;
    }
    sxml_item_retain(addition.document_node);
    *document = (uint32_t)(compilation->documents.count - 1U);
    return SXML_STATUS_OK;
}

/* Text to parse into a document node with locations. */
typedef struct document_text {
    sxml_byte_view content;
    sxml_byte_view encoding_name;
    sxml_byte_view uri;
    /* Receives the parser's diagnostic; may be NULL. */
    sxml_diagnostic *diagnostic;
} document_text;

static sxml_status parse_document(sxml_schematron_compilation *compilation,
                                  const document_text *text, sxml_item *document_node) {
    sxml_processor *processor = compilation->processor;
    sxml_arena_tree *arena_tree = NULL;
    sxml_status status = sxml_arena_tree_create(
        compilation->allocator,
        &(sxml_arena_tree_configuration){
            .allocator = compilation->allocator,
            .initial_memory = {.bytes = NULL, .byte_count = 0},
            .item_heap = processor->item_heap,
            .maximum_node_count = processor->limits.maximum_tree_node_count,
            .maximum_depth = processor->limits.xml_limits.maximum_depth,
            .maximum_text_byte_count = processor->limits.maximum_tree_text_byte_count,
            .keeps_locations = true,
            .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
            .is_destroyed_when_unreferenced = true,
            .error_record = NULL,
            .base_uri = {.bytes = NULL, .byte_count = 0}},
        &arena_tree);
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_parser_parse(
            compilation->parser,
            &(sxml_xml_parse_request){.input = text->content,
                                      .source_name = text->uri,
                                      .document_uri = text->uri,
                                      .base_uri = text->uri,
                                      .encoding_name = text->encoding_name,
                                      .diagnostic = text->diagnostic,
                                      .entity_resolver = sxml_processor_entity_resolver(processor)},
            sxml_arena_tree_as_tree_builder(arena_tree));
    }
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *document_node = sxml_node_item_make(
        (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0});
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

/* The document already read from uri; false when none was. */
static bool find_document(const sxml_schematron_compilation *compilation, sxml_byte_view uri,
                          uint32_t *document) {
    for (uint32_t index = 0; index < compilation->documents.count; index += 1) {
        if (uri.byte_count > 0 &&
            sxml_byte_view_equals(sxml_schematron_document_at(compilation, index)->uri, uri)) {
            *document = index;
            return true;
        }
    }
    return false;
}

/* What asking the resolver for a document gave. */
typedef struct loaded_document {
    bool is_loaded;
    uint32_t document;
    /* Why not, for the error's description. */
    const char *reason;
} loaded_document;

/* A document to ask the resolver for. */
typedef struct document_request {
    sxml_byte_view uri_reference;
    sxml_byte_view absolute_uri;
} document_request;

/* Reads a byte reader to its end into content (D233's form, which the
   sxml command's resolver gives XML documents). */
static sxml_status read_whole(const sxml_schematron_compilation *compilation, sxml_byte_reader reader,
                              sxml_xml_buffer *content) {
    enum { SXML_SCHEMATRON_READ_BLOCK_BYTE_COUNT = 64 * 1024 };
    void *block = NULL;
    sxml_status status = sxml_allocator_allocate(
        compilation->allocator,
        (sxml_allocation_request){.byte_count = SXML_SCHEMATRON_READ_BLOCK_BYTE_COUNT, .alignment = 1},
        &block);
    bool is_end = reader.operations == NULL || reader.operations->read == NULL;
    while (status == SXML_STATUS_OK && !is_end) {
        sxml_byte_reading reading = {.byte_count = 0, .is_end = false};
        status = reader.operations->read(
            reader.state,
            (sxml_memory_view){.bytes = block, .byte_count = SXML_SCHEMATRON_READ_BLOCK_BYTE_COUNT},
            &reading);
        if (status == SXML_STATUS_OK && reading.byte_count > 0) {
            status = sxml_xml_buffer_append(
                content, (sxml_byte_view){.bytes = block, .byte_count = reading.byte_count});
        }
        is_end = reading.is_end || reading.byte_count == 0;
    }
    sxml_allocator_release(compilation->allocator, block);
    return status;
}

/*
 * The document at request->absolute_uri: one read before, or one the
 * resolver delivers (kind XML_DOCUMENT: bytes, parsed here with locations,
 * or a document node). A document not delivered or not well-formed is not
 * loaded; returns only the allocator's and the resolver's failures and
 * SXML_STATUS_INVALID_ARGUMENT for a form the kind does not allow.
 */
static sxml_status load_document(sxml_schematron_compilation *compilation,
                                 const document_request *request, loaded_document *loaded) {
    *loaded = (loaded_document){.is_loaded = false, .document = 0, .reason = NULL};
    if (find_document(compilation, request->absolute_uri, &loaded->document)) {
        loaded->is_loaded = true;
        return SXML_STATUS_OK;
    }
    const sxml_resource_resolver resolver = compilation->processor->resource_resolver;
    if (resolver.operations == NULL || resolver.operations->resolve == NULL) {
        loaded->reason = "the resolver delivers no documents";
        return SXML_STATUS_OK;
    }
    sxml_resource resource;
    memset(&resource, 0, sizeof(resource));
    sxml_status status = resolver.operations->resolve(
        resolver.state,
        &(sxml_resource_request){.resource_kind = SXML_RESOURCE_KIND_XML_DOCUMENT,
                                 .uri_reference = request->uri_reference,
                                 .absolute_uri = request->absolute_uri,
                                 .processor = compilation->processor,
                                 .encoding_name = no_text,
                                 .is_default_collection = false,
                                 .target_namespace_uri = no_text,
                                 .package_version = no_text},
        &resource);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item document_node = {.representation = 0};
    bool has_node = false;
    sxml_byte_view uri = request->absolute_uri;
    switch (resource.resource_form) {
    case SXML_RESOURCE_FORM_NONE:
        loaded->reason = "the resolver does not deliver it";
        break;
    case SXML_RESOURCE_FORM_BYTES: {
        if (resource.resource_uri.byte_count > 0) {
            uri = resource.resource_uri;
        }
        sxml_diagnostic diagnostic;
        memset(&diagnostic, 0, sizeof(diagnostic));
        status = parse_document(compilation,
                                &(document_text){.content = resource.content,
                                                 .encoding_name = resource.encoding_name,
                                                 .uri = uri,
                                                 .diagnostic = &diagnostic},
                                &document_node);
        has_node = status == SXML_STATUS_OK;
        if (status != SXML_STATUS_OK && status != SXML_STATUS_OUT_OF_MEMORY &&
            status != SXML_STATUS_LIMIT_EXCEEDED) {
            loaded->reason = "it is not well-formed";
            status = SXML_STATUS_OK;
        }
        break;
    }
    case SXML_RESOURCE_FORM_DOCUMENT_NODE:
        /* The resolver's reference is ours. */
        document_node = resource.document_node;
        has_node = true;
        break;
    case SXML_RESOURCE_FORM_BYTE_READER: {
        /* Read whole, then parsed as BYTES are (a schematron document is
           small; D323). */
        if (resource.resource_uri.byte_count > 0) {
            uri = resource.resource_uri;
        }
        sxml_xml_buffer content = sxml_xml_buffer_empty(compilation->allocator);
        status = read_whole(compilation, resource.byte_reader, &content);
        if (status == SXML_STATUS_OK) {
            sxml_diagnostic diagnostic;
            memset(&diagnostic, 0, sizeof(diagnostic));
            status = parse_document(compilation,
                                    &(document_text){.content = sxml_schematron_buffer_text(&content),
                                                     .encoding_name = resource.encoding_name,
                                                     .uri = uri,
                                                     .diagnostic = &diagnostic},
                                    &document_node);
            has_node = status == SXML_STATUS_OK;
        }
        if (status == SXML_STATUS_INPUT_OUTPUT_FAILED) {
            loaded->reason = "it cannot be read";
            status = SXML_STATUS_OK;
        } else if (status != SXML_STATUS_OK && status != SXML_STATUS_OUT_OF_MEMORY &&
                   status != SXML_STATUS_LIMIT_EXCEEDED) {
            loaded->reason = "it is not well-formed";
            status = SXML_STATUS_OK;
        }
        sxml_xml_buffer_release(&content);
        break;
    }
    case SXML_RESOURCE_FORM_SEQUENCE:
        sxml_sequence_release(resource.items);
        status = SXML_STATUS_INVALID_ARGUMENT;
        break;
    }
    if (status == SXML_STATUS_OK && has_node) {
        status = add_document(compilation,
                              (document_addition){.document_node = document_node, .uri = uri},
                              &loaded->document);
        loaded->is_loaded = status == SXML_STATUS_OK;
    }
    if (has_node) {
        sxml_item_release(document_node);
    }
    if (resolver.operations->release != NULL) {
        resolver.operations->release(resolver.state, &resource);
    }
    return status;
}

/* --------------------------------------------------------------- Names */

static const char *const element_local_names[SXML_SCHEMATRON_ELEMENT_KIND_COUNT] = {
    [SXML_SCHEMATRON_ELEMENT_ACTIVE] = "active",
    [SXML_SCHEMATRON_ELEMENT_ASSERT] = "assert",
    [SXML_SCHEMATRON_ELEMENT_DIAGNOSTIC] = "diagnostic",
    [SXML_SCHEMATRON_ELEMENT_DIAGNOSTICS] = "diagnostics",
    [SXML_SCHEMATRON_ELEMENT_DIR] = "dir",
    [SXML_SCHEMATRON_ELEMENT_EMPH] = "emph",
    [SXML_SCHEMATRON_ELEMENT_EXTENDS] = "extends",
    [SXML_SCHEMATRON_ELEMENT_GROUP] = "group",
    [SXML_SCHEMATRON_ELEMENT_INCLUDE] = "include",
    [SXML_SCHEMATRON_ELEMENT_LET] = "let",
    [SXML_SCHEMATRON_ELEMENT_LIBRARY] = "library",
    [SXML_SCHEMATRON_ELEMENT_NAME] = "name",
    [SXML_SCHEMATRON_ELEMENT_NS] = "ns",
    [SXML_SCHEMATRON_ELEMENT_P] = "p",
    [SXML_SCHEMATRON_ELEMENT_PARAM] = "param",
    [SXML_SCHEMATRON_ELEMENT_PATTERN] = "pattern",
    [SXML_SCHEMATRON_ELEMENT_PHASE] = "phase",
    [SXML_SCHEMATRON_ELEMENT_PROPERTIES] = "properties",
    [SXML_SCHEMATRON_ELEMENT_PROPERTY] = "property",
    [SXML_SCHEMATRON_ELEMENT_REPORT] = "report",
    [SXML_SCHEMATRON_ELEMENT_RULE] = "rule",
    [SXML_SCHEMATRON_ELEMENT_RULES] = "rules",
    [SXML_SCHEMATRON_ELEMENT_SCHEMA] = "schema",
    [SXML_SCHEMATRON_ELEMENT_SPAN] = "span",
    [SXML_SCHEMATRON_ELEMENT_TITLE] = "title",
    [SXML_SCHEMATRON_ELEMENT_VALUE_OF] = "value-of",
};

static const char *const attribute_local_names[SXML_SCHEMATRON_ATTRIBUTE_KIND_COUNT] = {
    [SXML_SCHEMATRON_ATTRIBUTE_ABSTRACT] = "abstract",
    [SXML_SCHEMATRON_ATTRIBUTE_AS] = "as",
    [SXML_SCHEMATRON_ATTRIBUTE_CLASS] = "class",
    [SXML_SCHEMATRON_ATTRIBUTE_CONTEXT] = "context",
    [SXML_SCHEMATRON_ATTRIBUTE_DEFAULT_PHASE] = "defaultPhase",
    [SXML_SCHEMATRON_ATTRIBUTE_DIAGNOSTICS] = "diagnostics",
    [SXML_SCHEMATRON_ATTRIBUTE_DOCUMENTS] = "documents",
    [SXML_SCHEMATRON_ATTRIBUTE_FLAG] = "flag",
    [SXML_SCHEMATRON_ATTRIBUTE_FPI] = "fpi",
    [SXML_SCHEMATRON_ATTRIBUTE_FROM] = "from",
    [SXML_SCHEMATRON_ATTRIBUTE_HREF] = "href",
    [SXML_SCHEMATRON_ATTRIBUTE_ICON] = "icon",
    [SXML_SCHEMATRON_ATTRIBUTE_ID] = "id",
    [SXML_SCHEMATRON_ATTRIBUTE_IS_A] = "is-a",
    [SXML_SCHEMATRON_ATTRIBUTE_NAME] = "name",
    [SXML_SCHEMATRON_ATTRIBUTE_PATH] = "path",
    [SXML_SCHEMATRON_ATTRIBUTE_PATTERN] = "pattern",
    [SXML_SCHEMATRON_ATTRIBUTE_PREFIX] = "prefix",
    [SXML_SCHEMATRON_ATTRIBUTE_PROPERTIES] = "properties",
    [SXML_SCHEMATRON_ATTRIBUTE_QUERY_BINDING] = "queryBinding",
    [SXML_SCHEMATRON_ATTRIBUTE_ROLE] = "role",
    [SXML_SCHEMATRON_ATTRIBUTE_RULE] = "rule",
    [SXML_SCHEMATRON_ATTRIBUTE_SCHEMA_VERSION] = "schemaVersion",
    [SXML_SCHEMATRON_ATTRIBUTE_SCHEMATRON_EDITION] = "schematronEdition",
    [SXML_SCHEMATRON_ATTRIBUTE_SCHEME] = "scheme",
    [SXML_SCHEMATRON_ATTRIBUTE_SEE] = "see",
    [SXML_SCHEMATRON_ATTRIBUTE_SELECT] = "select",
    [SXML_SCHEMATRON_ATTRIBUTE_SEVERITY] = "severity",
    [SXML_SCHEMATRON_ATTRIBUTE_SUBJECT] = "subject",
    [SXML_SCHEMATRON_ATTRIBUTE_TEST] = "test",
    [SXML_SCHEMATRON_ATTRIBUTE_URI] = "uri",
    [SXML_SCHEMATRON_ATTRIBUTE_VALUE] = "value",
    [SXML_SCHEMATRON_ATTRIBUTE_VISIT_EACH] = "visit-each",
    [SXML_SCHEMATRON_ATTRIBUTE_WHEN] = "when",
    [SXML_SCHEMATRON_ATTRIBUTE_XML_LANG] = "lang",
    [SXML_SCHEMATRON_ATTRIBUTE_XML_SPACE] = "space",
    [SXML_SCHEMATRON_ATTRIBUTE_XML_BASE] = "base",
};

static sxml_status intern_names(sxml_schematron_compilation *compilation) {
    sxml_schematron_names *names = &compilation->names;
    sxml_status status = sxml_name_pool_intern_namespace_uri(
        compilation->name_pool, SXML_TEXT(SXML_SCHEMATRON_SCHEMA_NAMESPACE_URI),
        &names->schematron_namespace);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_uri(compilation->name_pool,
                                                     SXML_TEXT(SXML_SCHEMATRON_NAMESPACE_URI),
                                                     &names->private_namespace);
    }
    for (size_t kind_index = 1; status == SXML_STATUS_OK && kind_index < SXML_SCHEMATRON_ELEMENT_KIND_COUNT;
         kind_index += 1) {
        if (element_local_names[kind_index] == NULL) {
            continue;
        }
        status = sxml_name_pool_intern_name(
            compilation->name_pool,
            &(sxml_name_request){.namespace_uri = names->schematron_namespace,
                                 .local_name = sxml_byte_view_from_c_string(element_local_names[kind_index])},
            &names->elements[kind_index]);
    }
    for (size_t kind_index = 1; status == SXML_STATUS_OK && kind_index < SXML_SCHEMATRON_ATTRIBUTE_KIND_COUNT;
         kind_index += 1) {
        const bool is_xml = kind_index == SXML_SCHEMATRON_ATTRIBUTE_XML_LANG ||
                            kind_index == SXML_SCHEMATRON_ATTRIBUTE_XML_SPACE ||
                            kind_index == SXML_SCHEMATRON_ATTRIBUTE_XML_BASE;
        status = sxml_name_pool_intern_name(
            compilation->name_pool,
            &(sxml_name_request){
                .namespace_uri = {.index = is_xml ? (uint32_t)SXML_NAMESPACE_XML
                                                  : (uint32_t)SXML_NAMESPACE_NONE},
                .local_name = sxml_byte_view_from_c_string(attribute_local_names[kind_index])},
            &names->attributes[kind_index]);
    }
    return status;
}

/* The kind of an element of the tree (NONE for other nodes). */
static sxml_schematron_element_kind kind_of_name(const sxml_schematron_compilation *compilation,
                                                 sxml_name_identifier name) {
    for (size_t kind_number = 1; kind_number < SXML_SCHEMATRON_ELEMENT_UNKNOWN; kind_number += 1) {
        if (compilation->names.elements[kind_number].index == name.index) {
            return (sxml_schematron_element_kind)kind_number;
        }
    }
    sxml_name_parts parts = {.namespace_uri = {.index = 0}, .local_name = {.bytes = NULL, .byte_count = 0}};
    if (sxml_name_pool_get_name_parts(compilation->name_pool, name, &parts) == SXML_STATUS_OK &&
        parts.namespace_uri.index == compilation->names.schematron_namespace.index) {
        return SXML_SCHEMATRON_ELEMENT_UNKNOWN;
    }
    return SXML_SCHEMATRON_ELEMENT_FOREIGN;
}

sxml_schematron_element_kind sxml_schematron_element_kind_of(
    const sxml_schematron_compilation *compilation, sxml_schematron_place place) {
    sxml_node_description description;
    if (describe(compilation, place, &description) != SXML_STATUS_OK ||
        description.node_kind != SXML_NODE_KIND_ELEMENT) {
        return SXML_SCHEMATRON_ELEMENT_NONE;
    }
    return kind_of_name(compilation, description.node_name);
}

sxml_schematron_attribute sxml_schematron_find_attribute(
    const sxml_schematron_compilation *compilation, sxml_schematron_attribute_query query) {
    sxml_schematron_attribute found = {.is_present = false,
                                       .value = no_text,
                                       .source_span = {.byte_offset = 0,
                                                       .byte_count = 0,
                                                       .line_number = 0,
                                                       .column_number = 0}};
    sxml_node_description description;
    if (describe(compilation, query.place, &description) != SXML_STATUS_OK) {
        return found;
    }
    const sxml_name_identifier wanted = compilation->names.attributes[query.attribute_kind];
    for (uint32_t offset = 1; offset <= description.attribute_count; offset += 1) {
        const sxml_schematron_place attribute = {
            .document = query.place.document,
            .element = {.index = query.place.element.index + offset}};
        sxml_node_description attribute_description;
        if (describe(compilation, attribute, &attribute_description) != SXML_STATUS_OK ||
            attribute_description.node_name.index != wanted.index) {
            continue;
        }
        found.is_present = true;
        if (sxml_node_get_content(reference_of(compilation, attribute), &found.value) !=
            SXML_STATUS_OK) {
            found.value = no_text;
        }
        found.source_span = sxml_schematron_element_span(compilation, attribute);
        break;
    }
    return found;
}

/* The trimmed value of an attribute; empty when absent. */
static sxml_byte_view trimmed_attribute(const sxml_schematron_compilation *compilation,
                                        sxml_schematron_attribute_query query) {
    return sxml_xml_text_trim_whitespace(sxml_schematron_find_attribute(compilation, query).value);
}

sxml_schematron_inherited sxml_schematron_inherited_of(
    const sxml_schematron_compilation *compilation, sxml_schematron_place place) {
    sxml_schematron_inherited inherited = {.language = no_text, .preserves_space = false};
    bool has_language = false;
    bool has_space = false;
    sxml_schematron_place current = place;
    while (!has_language || !has_space) {
        if (!has_language) {
            const sxml_schematron_attribute language = sxml_schematron_find_attribute(
                compilation, (sxml_schematron_attribute_query){
                                 .place = current, .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_XML_LANG});
            if (language.is_present) {
                inherited.language = sxml_xml_text_trim_whitespace(language.value);
                has_language = true;
            }
        }
        if (!has_space) {
            const sxml_schematron_attribute space = sxml_schematron_find_attribute(
                compilation, (sxml_schematron_attribute_query){
                                 .place = current, .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_XML_SPACE});
            if (space.is_present) {
                inherited.preserves_space = sxml_byte_view_equals(
                    sxml_xml_text_trim_whitespace(space.value), SXML_TEXT("preserve"));
                has_space = true;
            }
        }
        sxml_node_description description;
        if (describe(compilation, current, &description) != SXML_STATUS_OK || !description.has_parent) {
            break;
        }
        current.element = description.parent;
    }
    return inherited;
}

/* -------------------------------------------------------------- Errors */

static sxml_status deliver_report(sxml_schematron_compilation *compilation,
                                  const sxml_schema_report *report,
                                  const sxml_error_record *record_form) {
    const sxml_schema_report *reported = compilation->reported.elements;
    for (size_t report_index = 0; report_index < compilation->reported.count; report_index += 1) {
        const sxml_schema_report *earlier = &reported[report_index];
        if (earlier->location.line_number == report->location.line_number &&
            earlier->location.column_number == report->location.column_number &&
            sxml_byte_view_equals(earlier->location.source_name, report->location.source_name) &&
            sxml_byte_view_equals(earlier->code.local_name, report->code.local_name)) {
            /* The same error at the same place: an element read again. */
            return SXML_STATUS_OK;
        }
    }
    const sxml_schema_report key = {.code = report->code,
                                    .description = no_text,
                                    .location = report->location};
    const sxml_status pushed = sxml_schematron_array_push(compilation, &compilation->reported,
                                                          SXML_SCHEMATRON_ELEMENT_OF(key));
    if (pushed != SXML_STATUS_OK) {
        return pushed;
    }
    compilation->error_count += 1;
    const sxml_schematron_configuration *configuration = compilation->configuration;
    if (compilation->error_count == 1 && configuration->error_record != NULL) {
        *configuration->error_record = *record_form;
    }
    const sxml_schema_report_sink sink = configuration->report_sink;
    if (sink.operations == NULL || sink.operations->receive == NULL) {
        compilation->stop_status = SXML_STATUS_STATIC_ERROR;
        return SXML_STATUS_STATIC_ERROR;
    }
    const sxml_status status = sink.operations->receive(sink.state, report);
    if (status != SXML_STATUS_OK) {
        compilation->stop_status = status;
    }
    return status;
}

sxml_status sxml_schematron_report_error(sxml_schematron_compilation *compilation,
                                         const sxml_schematron_error *error) {
    if (compilation->stop_status != SXML_STATUS_OK) {
        return compilation->stop_status;
    }
    const sxml_error_code code =
        error->is_xpath
            ? (sxml_error_code){.namespace_uri = SXML_TEXT(SXML_ERROR_NAMESPACE_URI),
                                .local_name = sxml_byte_view_from_c_string(error->code),
                                .namespace_prefix = SXML_TEXT("err")}
            : (sxml_error_code){.namespace_uri = SXML_TEXT(SXML_SCHEMATRON_NAMESPACE_URI),
                                .local_name = sxml_byte_view_from_c_string(error->code),
                                .namespace_prefix = SXML_TEXT("schematron")};
    const sxml_byte_view description = sxml_byte_view_from_c_string(error->description);
    const sxml_source_location location = {
        .source_name = error->document < compilation->documents.count
                           ? sxml_schematron_document_at(compilation, error->document)->uri
                           : compilation->configuration->document_uri,
        .line_number = error->source_span.line_number,
        .column_number = error->source_span.column_number,
        .byte_offset = error->source_span.byte_offset};
    /* The record outlives the documents: the compiled document is named as
       the configuration names it, as XSD does (src/schema_components.c). */
    sxml_error_record record = {.code = code,
                                .value = NULL,
                                .diagnostic = {.description = description, .location = location}};
    record.diagnostic.location.source_name = compilation->configuration->document_uri;
    sxml_byte_view report_description = description;
    compilation->description.byte_count = 0;
    if (error->subject.byte_count > 0) {
        sxml_status status = sxml_xml_buffer_append(&compilation->description, description);
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(&compilation->description, SXML_TEXT(": "));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(&compilation->description, error->subject);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        report_description = sxml_schematron_buffer_text(&compilation->description);
    }
    return deliver_report(compilation,
                          &(sxml_schema_report){.code = code,
                                                .description = report_description,
                                                .location = location},
                          &record);
}

sxml_status sxml_schematron_report_record(sxml_schematron_compilation *compilation,
                                          const sxml_error_record *record) {
    if (compilation->stop_status != SXML_STATUS_OK) {
        return compilation->stop_status;
    }
    sxml_error_record record_form = *record;
    /* The record outlives the documents and the AST (whose module URIs
       expression errors name). */
    record_form.diagnostic.location.source_name = compilation->configuration->document_uri;
    return deliver_report(compilation,
                          &(sxml_schema_report){.code = record->code,
                                                .description = record->diagnostic.description,
                                                .location = record->diagnostic.location},
                          &record_form);
}

/* An error at an element or attribute. */
typedef struct place_error {
    const char *code;
    const char *description;
    sxml_schematron_place place;
    /* The attribute's span, when line_number is not 0; else the
       element's. */
    sxml_ast_source_span attribute_span;
    sxml_byte_view subject;
} place_error;

static sxml_status report_at(sxml_schematron_compilation *compilation, place_error error) {
    return sxml_schematron_report_error(
        compilation,
        &(sxml_schematron_error){.code = error.code,
                                 .is_xpath = false,
                                 .description = error.description,
                                 .subject = error.subject,
                                 .document = error.place.document,
                                 .source_span = error.attribute_span.line_number != 0
                                                    ? error.attribute_span
                                                    : sxml_schematron_element_span(compilation,
                                                                                   error.place)});
}

/* ----------------------------------------------------------- Inclusion */

/* Finds the element of document whose id or xml:id is identifier. */
/* An element to find by id or xml:id in a document. */
typedef struct fragment_query {
    uint32_t document;
    sxml_byte_view identifier;
} fragment_query;

static bool find_fragment(const sxml_schematron_compilation *compilation, fragment_query query,
                          sxml_node_index *element) {
    const uint32_t document = query.document;
    const sxml_byte_view identifier = query.identifier;
    sxml_node_description root;
    const sxml_schematron_place root_place = {.document = document, .element = {.index = 0}};
    if (describe(compilation, root_place, &root) != SXML_STATUS_OK) {
        return false;
    }
    sxml_name_identifier xml_id = {.index = 0};
    if (sxml_name_pool_intern_name(
            compilation->name_pool,
            &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_XML},
                                 .local_name = SXML_TEXT("id")},
            &xml_id) != SXML_STATUS_OK) {
        return false;
    }
    const sxml_name_identifier id_name = compilation->names.attributes[SXML_SCHEMATRON_ATTRIBUTE_ID];
    for (uint32_t index = 1; index <= root.last_descendant.index; index += 1) {
        const sxml_schematron_place place = {.document = document, .element = {.index = index}};
        sxml_node_description description;
        if (describe(compilation, place, &description) != SXML_STATUS_OK ||
            description.node_kind != SXML_NODE_KIND_ATTRIBUTE ||
            (description.node_name.index != id_name.index &&
             description.node_name.index != xml_id.index)) {
            continue;
        }
        sxml_byte_view value = no_text;
        if (sxml_node_get_content(reference_of(compilation, place), &value) == SXML_STATUS_OK &&
            sxml_byte_view_equals(sxml_xml_text_trim_whitespace(value), identifier)) {
            *element = description.parent;
            return true;
        }
    }
    return false;
}

/* The document element of a document. */
static bool find_document_element(const sxml_schematron_compilation *compilation,
                                  uint32_t document, sxml_node_index *element) {
    sxml_node_description description;
    if (describe(compilation, (sxml_schematron_place){.document = document, .element = {.index = 0}},
                 &description) != SXML_STATUS_OK ||
        !description.has_children) {
        return false;
    }
    sxml_node_index child = description.first_child;
    for (;;) {
        sxml_node_description child_description;
        if (describe(compilation, (sxml_schematron_place){.document = document, .element = child},
                     &child_description) != SXML_STATUS_OK) {
            return false;
        }
        if (child_description.node_kind == SXML_NODE_KIND_ELEMENT) {
            *element = child;
            return true;
        }
        if (!child_description.has_next_sibling) {
            return false;
        }
        child = child_description.next_sibling;
    }
}

/* The element an href names (5.4.4, 5.4.6): a document's document element
   or, with a fragment identifier, the element whose id it is. */
typedef struct resolved_href {
    bool is_found;
    sxml_schematron_place target;
} resolved_href;

static sxml_status resolve_href(sxml_schematron_compilation *compilation,
                                sxml_schematron_place referrer, resolved_href *resolved) {
    *resolved = (resolved_href){.is_found = false,
                                .target = {.document = 0, .element = {.index = 0}}};
    const sxml_schematron_attribute href = sxml_schematron_find_attribute(
        compilation, (sxml_schematron_attribute_query){.place = referrer,
                                                       .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_HREF});
    const sxml_byte_view text = sxml_xml_text_trim_whitespace(href.value);
    size_t hash_index = 0;
    while (hash_index < text.byte_count && text.bytes[hash_index] != '#') {
        hash_index += 1;
    }
    const sxml_byte_view reference = {.bytes = text.bytes, .byte_count = hash_index};
    const sxml_byte_view fragment =
        hash_index < text.byte_count
            ? (sxml_byte_view){.bytes = text.bytes + hash_index + 1,
                               .byte_count = text.byte_count - hash_index - 1}
            : no_text;
    uint32_t document = referrer.document;
    if (reference.byte_count > 0) {
        const char *reason = NULL;
        /* 6.8: relative to the base URI of the referring element's own
           document. */
        sxml_uri_resolution resolution = {.uri = no_text, .memory = NULL};
        sxml_status status = sxml_uri_resolve(
            &(sxml_uri_resolution_request){
                .base_uri = sxml_schematron_document_at(compilation, referrer.document)->uri,
                .reference = reference,
                .allocator = compilation->allocator},
            &resolution);
        if (status != SXML_STATUS_OK && status != SXML_STATUS_OUT_OF_MEMORY) {
            reason = "the reference is not a URI";
            status = SXML_STATUS_OK;
        }
        loaded_document loaded = {.is_loaded = false, .document = 0, .reason = NULL};
        if (status == SXML_STATUS_OK && reason == NULL) {
            status = load_document(compilation,
                                   &(document_request){.uri_reference = reference,
                                                       .absolute_uri = resolution.uri},
                                   &loaded);
        }
        sxml_allocator_release(compilation->allocator, resolution.memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!loaded.is_loaded) {
            return report_at(compilation,
                             (place_error){.code = "include",
                                           .description = loaded.reason != NULL
                                                              ? "the document an href names cannot be "
                                                                "read (5.4.4, 5.4.6)"
                                                              : "the href is not a URI (5.4.4, 5.4.6)",
                                           .place = referrer,
                                           .attribute_span = href.source_span,
                                           .subject = text});
        }
        document = loaded.document;
    }
    sxml_node_index element = {.index = 0};
    const bool is_found = fragment.byte_count > 0
                              ? find_fragment(compilation, (fragment_query){.document = document, .identifier = fragment}, &element)
                              : find_document_element(compilation, document, &element);
    if (!is_found) {
        return report_at(compilation,
                         (place_error){.code = "include",
                                       .description = "no element has the id the href's fragment "
                                                      "identifier names (5.4.4, 5.4.6)",
                                       .place = referrer,
                                       .attribute_span = href.source_span,
                                       .subject = text});
    }
    *resolved = (resolved_href){.is_found = true,
                                .target = {.document = document, .element = element}};
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------ Effective children */

/* One frame of a cursor: the element whose children are iterated. */
typedef struct cursor_frame {
    sxml_schematron_place parent;
    bool has_next;
    sxml_node_index next_child;
    /* The frame cursor_enter pushed: its end is the end of the level. */
    bool is_level_start;
} cursor_frame;

static cursor_frame *cursor_frame_at(const sxml_schematron_cursor *cursor, size_t frame_index) {
    cursor_frame *frames = cursor->frames.elements;
    return &frames[frame_index];
}

/* Pushes a frame over the children of parent. */
static sxml_status push_cursor_frame(sxml_schematron_cursor *cursor, cursor_frame frame) {
    sxml_node_description description;
    sxml_status status = describe(cursor->compilation, frame.parent, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    frame.has_next = description.has_children;
    frame.next_child = description.first_child;
    return sxml_schematron_array_push(cursor->compilation, &cursor->frames,
                                      SXML_SCHEMATRON_ELEMENT_OF(frame));
}

sxml_status sxml_schematron_cursor_start(sxml_schematron_cursor *cursor,
                                         sxml_schematron_place parent) {
    return push_cursor_frame(cursor, (cursor_frame){.parent = parent,
                                                    .has_next = false,
                                                    .next_child = {.index = 0},
                                                    .is_level_start = true});
}

void sxml_schematron_cursor_finish(sxml_schematron_cursor *cursor) {
    sxml_schematron_array_release(cursor->compilation, &cursor->frames);
}

/* True when place is the parent of a frame: an inclusion of it would put
   it inside itself (6.2). */
static bool is_on_cursor_path(const sxml_schematron_cursor *cursor, sxml_schematron_place place) {
    for (size_t frame_index = 0; frame_index < cursor->frames.count; frame_index += 1) {
        const cursor_frame *frame = cursor_frame_at(cursor, frame_index);
        if (frame->parent.document == place.document &&
            frame->parent.element.index == place.element.index) {
            return true;
        }
    }
    return false;
}

/* The kind of the element whose level is on top. */
static sxml_schematron_element_kind level_kind(const sxml_schematron_cursor *cursor) {
    size_t frame_index = cursor->frames.count;
    while (frame_index > 0) {
        frame_index -= 1;
        const cursor_frame *frame = cursor_frame_at(cursor, frame_index);
        if (frame->is_level_start) {
            return sxml_schematron_element_kind_of(cursor->compilation, frame->parent);
        }
    }
    return SXML_SCHEMATRON_ELEMENT_NONE;
}

/* What to do with an sch:extends: the external declaration's children in
   its place (href), the abstract rule's (rule, when the cursor expands
   rules), or yield the element. */
/* Whether an sch:extends was replaced by what it names. */
typedef struct extends_expansion {
    bool is_expanded;
} extends_expansion;

static sxml_status expand_extends(sxml_schematron_cursor *cursor, sxml_schematron_place extends,
                                  extends_expansion *expansion) {
    sxml_schematron_compilation *compilation = cursor->compilation;
    bool *is_expanded = &expansion->is_expanded;
    *is_expanded = false;
    const sxml_schematron_attribute href = sxml_schematron_find_attribute(
        compilation, (sxml_schematron_attribute_query){.place = extends,
                                                       .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_HREF});
    const sxml_schematron_attribute rule = sxml_schematron_find_attribute(
        compilation, (sxml_schematron_attribute_query){.place = extends,
                                                       .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_RULE});
    if (href.is_present == rule.is_present) {
        /* The grammar's matter (5.4.4): the checker reports it. */
        return SXML_STATUS_OK;
    }
    if (rule.is_present) {
        if (!cursor->expands_rules) {
            return SXML_STATUS_OK;
        }
        *is_expanded = true;
        const sxml_schematron_identified *abstract_rule = sxml_schematron_find_identified(
            &compilation->index.abstract_rules, sxml_xml_text_trim_whitespace(rule.value));
        if (abstract_rule == NULL) {
            /* schematron:reference, reported by reading. */
            return SXML_STATUS_OK;
        }
        if (is_on_cursor_path(cursor, abstract_rule->place)) {
            return report_at(compilation,
                             (place_error){.code = "reference",
                                           .description = "an abstract rule extends itself (5.4.4)",
                                           .place = extends,
                                           .attribute_span = rule.source_span,
                                           .subject = abstract_rule->identifier});
        }
        return push_cursor_frame(cursor, (cursor_frame){.parent = abstract_rule->place,
                                                        .has_next = false,
                                                        .next_child = {.index = 0},
                                                        .is_level_start = false});
    }
    *is_expanded = true;
    resolved_href resolved = {.is_found = false, .target = {.document = 0, .element = {.index = 0}}};
    sxml_status status = resolve_href(compilation, extends, &resolved);
    if (status != SXML_STATUS_OK || !resolved.is_found) {
        return status;
    }
    /* 5.4.4: an external declaration of the parent's type: a rule under a
       rule, a schema or library under a schema or library. */
    const sxml_schematron_element_kind parent_kind = level_kind(cursor);
    const sxml_schematron_element_kind target_kind =
        sxml_schematron_element_kind_of(compilation, resolved.target);
    const bool is_rule_level = parent_kind == SXML_SCHEMATRON_ELEMENT_RULE;
    const bool fits = is_rule_level ? target_kind == SXML_SCHEMATRON_ELEMENT_RULE
                                    : (target_kind == SXML_SCHEMATRON_ELEMENT_SCHEMA ||
                                       target_kind == SXML_SCHEMATRON_ELEMENT_LIBRARY);
    if (!fits) {
        return report_at(compilation,
                         (place_error){.code = "include",
                                       .description = "the external declaration an extends href "
                                                      "names is not of its parent's type (5.4.4)",
                                       .place = extends,
                                       .attribute_span = href.source_span,
                                       .subject = sxml_xml_text_trim_whitespace(href.value)});
    }
    if (is_on_cursor_path(cursor, resolved.target)) {
        return report_at(compilation,
                         (place_error){.code = "include",
                                       .description = "an extends href includes an element in "
                                                      "itself (6.2)",
                                       .place = extends,
                                       .attribute_span = href.source_span,
                                       .subject = sxml_xml_text_trim_whitespace(href.value)});
    }
    return push_cursor_frame(cursor, (cursor_frame){.parent = resolved.target,
                                                    .has_next = false,
                                                    .next_child = {.index = 0},
                                                    .is_level_start = false});
}

sxml_status sxml_schematron_cursor_next(sxml_schematron_cursor *cursor,
                                        sxml_schematron_child *child) {
    sxml_schematron_compilation *compilation = cursor->compilation;
    *child = (sxml_schematron_child){.has_child = false,
                                     .place = {.document = 0, .element = {.index = 0}},
                                     .element_kind = SXML_SCHEMATRON_ELEMENT_NONE};
    while (cursor->frames.count > 0) {
        cursor_frame *top = cursor_frame_at(cursor, cursor->frames.count - 1U);
        if (!top->has_next) {
            const bool ends_level = top->is_level_start;
            cursor->frames.count -= 1;
            if (ends_level) {
                return SXML_STATUS_OK;
            }
            continue;
        }
        const sxml_schematron_place place = {.document = top->parent.document,
                                             .element = top->next_child};
        sxml_node_description description;
        sxml_status status = describe(compilation, place, &description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        top->has_next = description.has_next_sibling;
        top->next_child = description.next_sibling;
        if (description.node_kind != SXML_NODE_KIND_ELEMENT) {
            continue;
        }
        const sxml_schematron_element_kind kind = kind_of_name(compilation, description.node_name);
        if (kind == SXML_SCHEMATRON_ELEMENT_EXTENDS) {
            extends_expansion expansion = {.is_expanded = false};
            status = expand_extends(cursor, place, &expansion);
            const bool is_expanded = expansion.is_expanded;
            if (status != SXML_STATUS_OK) {
                return status;
            }
            if (is_expanded) {
                continue;
            }
        } else if (kind == SXML_SCHEMATRON_ELEMENT_INCLUDE &&
                   sxml_schematron_find_attribute(
                       compilation,
                       (sxml_schematron_attribute_query){.place = place,
                                                         .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_HREF})
                       .is_present) {
            resolved_href resolved = {.is_found = false,
                                      .target = {.document = 0, .element = {.index = 0}}};
            status = resolve_href(compilation, place, &resolved);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            if (!resolved.is_found) {
                continue;
            }
            if (is_on_cursor_path(cursor, resolved.target)) {
                status = report_at(compilation,
                                   (place_error){.code = "include",
                                                 .description = "an include puts an element inside "
                                                                "itself (6.2)",
                                                 .place = place,
                                                 .attribute_span = {.byte_offset = 0,
                                                                    .byte_count = 0,
                                                                    .line_number = 0,
                                                                    .column_number = 0},
                                                 .subject = no_text});
                if (status != SXML_STATUS_OK) {
                    return status;
                }
                continue;
            }
            *child = (sxml_schematron_child){
                .has_child = true,
                .place = resolved.target,
                .element_kind = sxml_schematron_element_kind_of(compilation, resolved.target),
                .is_inserted = false,
                .is_included = true,
                .included_by = place};
            return SXML_STATUS_OK;
        }
        *child = (sxml_schematron_child){.has_child = true,
                                         .place = place,
                                         .element_kind = kind,
                                         .is_inserted = !top->is_level_start};
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------ Query binding */

/* 6.5: the binding names compared ignoring case; D313 as amended, D315
   "Query bindings". False for a binding sxml does not read. */
static bool read_query_binding(sxml_byte_view name, sxml_schematron_binding *binding) {
    static const struct {
        const char *name;
        double version;
        bool is_xslt;
        bool is_xslt_2;
    } bindings[] = {
        {"xslt", 4.0, true, false},   {"xslt2", 2.0, true, true},  {"xslt3", 3.0, true, false},
        {"xslt4", 4.0, true, false},  {"xpath", 4.0, false, false}, {"xpath2", 2.0, false, false},
        {"xpath3", 3.0, false, false}, {"xpath31", 3.0, false, false},
        {"xpath4", 4.0, false, false},
    };
    for (size_t binding_index = 0; binding_index < sizeof(bindings) / sizeof(bindings[0]);
         binding_index += 1) {
        const sxml_byte_view candidate = sxml_byte_view_from_c_string(bindings[binding_index].name);
        if (candidate.byte_count != name.byte_count) {
            continue;
        }
        bool is_same = true;
        for (size_t byte_index = 0; byte_index < name.byte_count; byte_index += 1) {
            uint8_t byte = name.bytes[byte_index];
            if (byte >= 'A' && byte <= 'Z') {
                byte = (uint8_t)(byte - 'A' + 'a');
            }
            if (byte != candidate.bytes[byte_index]) {
                is_same = false;
                break;
            }
        }
        if (is_same) {
            *binding = (sxml_schematron_binding){.version = bindings[binding_index].version,
                                                 .is_xslt = bindings[binding_index].is_xslt,
                                                 .is_xslt_2 = bindings[binding_index].is_xslt_2};
            return true;
        }
    }
    return false;
}

/* ----------------------------------------------------------- Grammar */

/* The production of Annex A an element is checked against. */
typedef enum grammar_production {
    SXML_PRODUCTION_NONE = 0,
    SXML_PRODUCTION_SCHEMA,
    SXML_PRODUCTION_LIBRARY,
    SXML_PRODUCTION_PATTERN,
    SXML_PRODUCTION_ABSTRACT_PATTERN,
    SXML_PRODUCTION_PATTERN_INSTANCE,
    SXML_PRODUCTION_RULE,
    SXML_PRODUCTION_ABSTRACT_RULE,
    SXML_PRODUCTION_RULES,
    SXML_PRODUCTION_PHASE,
    SXML_PRODUCTION_DIAGNOSTICS,
    SXML_PRODUCTION_PROPERTIES,
    /* assert, report, diagnostic, property: text, value-of, name, emph,
       dir, span. */
    SXML_PRODUCTION_MESSAGE,
    /* emph, dir, span: text, value-of, name. */
    SXML_PRODUCTION_DYNAMIC_TEXT,
    /* p, active: text, dir, emph, span. */
    SXML_PRODUCTION_PARAGRAPH,
    /* title: text, dir. */
    SXML_PRODUCTION_TITLE,
    /* let without value: foreign elements. */
    SXML_PRODUCTION_LET_CONTENT,
    /* value-of, name, ns, param, extends, include, let with value: no
       content. */
    SXML_PRODUCTION_EMPTY,
} grammar_production;

/* One element being checked. */
typedef struct check_frame {
    sxml_schematron_place place;
    sxml_schematron_element_kind element_kind;
    grammar_production production;
    /* The rank of the last child (the order of Annex A's sequences). */
    uint32_t last_rank;
    uint32_t title_count;
    uint32_t pattern_count;
    uint32_t diagnostics_count;
    uint32_t properties_count;
    /* Assertions, reports, extends and p of a rule; abstract rules of
       rules; foreign elements of a let. */
    uint32_t required_count;
    /* Inside an abstract pattern: its expressions hold placeholders. */
    bool is_in_abstract_pattern;
} check_frame;

typedef struct schema_reader {
    sxml_schematron_compilation *compilation;
    /* check_frame. */
    sxml_schematron_array frames;
    sxml_schematron_cursor cursor;
    /* sxml_schematron_identified: every id met, for 7.1's ID type. */
    sxml_schematron_array identifiers;
    /* Elements whose references are checked once every id is known:
       sch:active, sch:extends rule, patterns with is-a, assertions with
       diagnostics or properties. */
    sxml_schematron_array references;
    /* sch:let elements and abstract patterns' sch:param elements, for
       Annex B's param-variable-clash. */
    sxml_schematron_array lets;
} schema_reader;

static check_frame *frame_at(const schema_reader *reader, size_t frame_index) {
    check_frame *frames = reader->frames.elements;
    return &frames[frame_index];
}

/* The attributes in no namespace an element may have (Annex A), as bits
   of sxml_schematron_attribute_kind, and those it must have. */
typedef struct attribute_rule {
    uint64_t allowed;
    uint64_t required;
} attribute_rule;

#define SXML_BIT(kind) ((uint64_t)1U << (uint64_t)(SXML_SCHEMATRON_ATTRIBUTE_##kind))
#define SXML_RICH (SXML_BIT(ICON) | SXML_BIT(SEE) | SXML_BIT(FPI))
#define SXML_LINKABLE (SXML_BIT(ROLE) | SXML_BIT(SUBJECT))

static attribute_rule attribute_rule_of(sxml_schematron_element_kind kind) {
    switch ((uint32_t)kind) {
    case SXML_SCHEMATRON_ELEMENT_SCHEMA:
        return (attribute_rule){.allowed = SXML_BIT(ID) | SXML_RICH | SXML_BIT(SCHEMA_VERSION) |
                                           SXML_BIT(SCHEMATRON_EDITION) | SXML_BIT(DEFAULT_PHASE) |
                                           SXML_BIT(QUERY_BINDING),
                                .required = 0};
    case SXML_SCHEMATRON_ELEMENT_LIBRARY:
    case SXML_SCHEMATRON_ELEMENT_RULES:
        return (attribute_rule){.allowed = SXML_BIT(ID) | SXML_RICH, .required = 0};
    case SXML_SCHEMATRON_ELEMENT_ACTIVE:
        return (attribute_rule){.allowed = SXML_BIT(PATTERN), .required = SXML_BIT(PATTERN)};
    case SXML_SCHEMATRON_ELEMENT_ASSERT:
    case SXML_SCHEMATRON_ELEMENT_REPORT:
        return (attribute_rule){.allowed = SXML_BIT(TEST) | SXML_BIT(FLAG) | SXML_BIT(ID) |
                                           SXML_BIT(DIAGNOSTICS) | SXML_BIT(PROPERTIES) | SXML_RICH |
                                           SXML_LINKABLE | SXML_BIT(SEVERITY),
                                .required = SXML_BIT(TEST)};
    case SXML_SCHEMATRON_ELEMENT_DIAGNOSTIC:
        return (attribute_rule){.allowed = SXML_BIT(ID) | SXML_BIT(ROLE) | SXML_RICH,
                                .required = SXML_BIT(ID)};
    case SXML_SCHEMATRON_ELEMENT_DIR:
        return (attribute_rule){.allowed = SXML_BIT(VALUE), .required = 0};
    case SXML_SCHEMATRON_ELEMENT_EXTENDS:
        return (attribute_rule){.allowed = SXML_BIT(RULE) | SXML_BIT(HREF), .required = 0};
    case SXML_SCHEMATRON_ELEMENT_LET:
        return (attribute_rule){.allowed = SXML_BIT(NAME) | SXML_BIT(AS) | SXML_BIT(VALUE),
                                .required = SXML_BIT(NAME)};
    case SXML_SCHEMATRON_ELEMENT_NAME:
        return (attribute_rule){.allowed = SXML_BIT(PATH), .required = 0};
    case SXML_SCHEMATRON_ELEMENT_NS:
        return (attribute_rule){.allowed = SXML_BIT(URI) | SXML_BIT(PREFIX),
                                .required = SXML_BIT(URI) | SXML_BIT(PREFIX)};
    case SXML_SCHEMATRON_ELEMENT_P:
        return (attribute_rule){.allowed = SXML_BIT(ID) | SXML_BIT(CLASS) | SXML_BIT(ICON),
                                .required = 0};
    case SXML_SCHEMATRON_ELEMENT_PARAM:
        return (attribute_rule){.allowed = SXML_BIT(NAME) | SXML_BIT(VALUE),
                                .required = SXML_BIT(NAME)};
    case SXML_SCHEMATRON_ELEMENT_PATTERN:
    case SXML_SCHEMATRON_ELEMENT_GROUP:
        return (attribute_rule){.allowed = SXML_BIT(DOCUMENTS) | SXML_BIT(ROLE) | SXML_RICH |
                                           SXML_BIT(ABSTRACT) | SXML_BIT(ID) | SXML_BIT(IS_A),
                                .required = 0};
    case SXML_SCHEMATRON_ELEMENT_PHASE:
        return (attribute_rule){.allowed = SXML_BIT(ID) | SXML_BIT(FROM) | SXML_BIT(WHEN) | SXML_RICH,
                                .required = SXML_BIT(ID)};
    case SXML_SCHEMATRON_ELEMENT_PROPERTY:
        return (attribute_rule){.allowed = SXML_BIT(ID) | SXML_BIT(ROLE) | SXML_BIT(SCHEME),
                                .required = SXML_BIT(ID)};
    case SXML_SCHEMATRON_ELEMENT_RULE:
        return (attribute_rule){.allowed = SXML_BIT(FLAG) | SXML_RICH | SXML_LINKABLE |
                                           SXML_BIT(CONTEXT) | SXML_BIT(VISIT_EACH) | SXML_BIT(ID) |
                                           SXML_BIT(ABSTRACT),
                                .required = 0};
    case SXML_SCHEMATRON_ELEMENT_SPAN:
        return (attribute_rule){.allowed = SXML_BIT(CLASS), .required = SXML_BIT(CLASS)};
    case SXML_SCHEMATRON_ELEMENT_VALUE_OF:
        return (attribute_rule){.allowed = SXML_BIT(SELECT), .required = SXML_BIT(SELECT)};
    case SXML_SCHEMATRON_ELEMENT_INCLUDE:
        return (attribute_rule){.allowed = SXML_BIT(HREF), .required = SXML_BIT(HREF)};
    case SXML_SCHEMATRON_ELEMENT_DIAGNOSTICS:
    case SXML_SCHEMATRON_ELEMENT_PROPERTIES:
    case SXML_SCHEMATRON_ELEMENT_EMPH:
    case SXML_SCHEMATRON_ELEMENT_TITLE:
    case SXML_SCHEMATRON_ELEMENT_NONE:
    case SXML_SCHEMATRON_ELEMENT_UNKNOWN:
    case SXML_SCHEMATRON_ELEMENT_FOREIGN:
    case SXML_SCHEMATRON_ELEMENT_KIND_COUNT:
    default:
        break;
    }
    return (attribute_rule){.allowed = 0, .required = 0};
}

/* The names a list holds. */
typedef enum name_list_kind {
    SXML_SCHEMATRON_NAME_LIST_NAMES,
    SXML_SCHEMATRON_NAME_LIST_NCNAMES,
} name_list_kind;

static bool is_name_list(sxml_byte_view text, name_list_kind list_kind) {
    const bool is_ncname = list_kind == SXML_SCHEMATRON_NAME_LIST_NCNAMES;
    sxml_byte_view rest = text;
    sxml_byte_view token = no_text;
    bool has_token = false;
    while (sxml_xml_text_next_token(&rest, &token)) {
        has_token = true;
        if (is_ncname ? !sxml_xml_text_is_ncname(token) : !sxml_xml_text_is_name(token)) {
            return false;
        }
    }
    return has_token;
}

/* A dynamic attribute value: a variable reference (5.5.6, 5.5.14,
   5.5.16), or under an xslt binding an attribute value template. */
static bool is_dynamic_value(const sxml_schematron_compilation *compilation, sxml_byte_view text) {
    const sxml_byte_view trimmed = sxml_xml_text_trim_whitespace(text);
    if (trimmed.byte_count > 0 && trimmed.bytes[0] == '$') {
        return true;
    }
    if (!compilation->binding.is_xslt) {
        return false;
    }
    for (size_t byte_index = 0; byte_index < trimmed.byte_count; byte_index += 1) {
        if (trimmed.bytes[byte_index] == '{') {
            return true;
        }
    }
    return false;
}

/* Checks an element's attributes against its rule and the value types of
   Annex A and 7.2; records its id. */
static sxml_status check_attributes(schema_reader *reader, sxml_schematron_place place,
                                    sxml_schematron_element_kind kind) {
    sxml_schematron_compilation *compilation = reader->compilation;
    const attribute_rule rule = attribute_rule_of(kind);
    sxml_node_description description;
    sxml_status status = describe(compilation, place, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint64_t present = 0;
    for (uint32_t offset = 1; offset <= description.attribute_count; offset += 1) {
        const sxml_schematron_place attribute = {.document = place.document,
                                                 .element = {.index = place.element.index + offset}};
        sxml_node_description attribute_description;
        status = describe(compilation, attribute, &attribute_description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_name_parts parts = {.namespace_uri = {.index = 0}, .local_name = no_text};
        status = sxml_name_pool_get_name_parts(compilation->name_pool,
                                               attribute_description.node_name, &parts);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (parts.namespace_uri.index == SXML_NAMESPACE_XML) {
            continue;
        }
        const sxml_ast_source_span span = sxml_schematron_element_span(compilation, attribute);
        if (parts.namespace_uri.index == compilation->names.schematron_namespace.index) {
            /* 5.2: an attribute in the Schematron namespace is not
               foreign. */
            status = report_at(compilation,
                               (place_error){.code = "syntax",
                                             .description = "an attribute in the Schematron namespace "
                                                            "(5.2, Annex A)",
                                             .place = place,
                                             .attribute_span = span,
                                             .subject = parts.local_name});
        } else if (parts.namespace_uri.index == SXML_NAMESPACE_NONE) {
            size_t found = 0;
            for (size_t attribute_kind = 1; attribute_kind < SXML_SCHEMATRON_ATTRIBUTE_XML_LANG;
                 attribute_kind += 1) {
                if (compilation->names.attributes[attribute_kind].index ==
                    attribute_description.node_name.index) {
                    found = attribute_kind;
                    break;
                }
            }
            if (found == 0 || (rule.allowed & ((uint64_t)1U << found)) == 0) {
                status = report_at(compilation,
                                   (place_error){.code = "syntax",
                                                 .description = "an attribute the grammar does not "
                                                                "allow on this element (Annex A)",
                                                 .place = place,
                                                 .attribute_span = span,
                                                 .subject = parts.local_name});
            } else {
                present |= (uint64_t)1U << found;
            }
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    const uint64_t missing = rule.required & ~present;
    for (size_t attribute_kind = 1; attribute_kind < SXML_SCHEMATRON_ATTRIBUTE_KIND_COUNT;
         attribute_kind += 1) {
        if ((missing & ((uint64_t)1U << attribute_kind)) != 0) {
            status = report_at(compilation,
                               (place_error){.code = "syntax",
                                             .description = "a required attribute is missing "
                                                            "(Annex A)",
                                             .place = place,
                                             .attribute_span = {.byte_offset = 0,
                                                                .byte_count = 0,
                                                                .line_number = 0,
                                                                .column_number = 0},
                                             .subject = sxml_byte_view_from_c_string(
                                                 attribute_local_names[attribute_kind])});
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
    }
    /* Value types: names (7.2), tokens of Annex A. */
    typedef struct value_check {
        sxml_schematron_attribute_kind attribute_kind;
        /* 0 an NCName, 1 a Name, 2 a list of Names (flag), 3 true or
           false, 4 ltr or rtl, 5 a list of NCNames (IDREFS). */
        int value_type;
    } value_check;
    static const value_check checks[] = {
        {SXML_SCHEMATRON_ATTRIBUTE_ID, 0},          {SXML_SCHEMATRON_ATTRIBUTE_FLAG, 2},
        {SXML_SCHEMATRON_ATTRIBUTE_ABSTRACT, 3},    {SXML_SCHEMATRON_ATTRIBUTE_DIAGNOSTICS, 5},
        {SXML_SCHEMATRON_ATTRIBUTE_PROPERTIES, 5},  {SXML_SCHEMATRON_ATTRIBUTE_IS_A, 0},
        {SXML_SCHEMATRON_ATTRIBUTE_DEFAULT_PHASE, 0}, {SXML_SCHEMATRON_ATTRIBUTE_RULE, 0},
    };
    for (size_t check_index = 0; check_index < sizeof(checks) / sizeof(checks[0]);
         check_index += 1) {
        const value_check check = checks[check_index];
        if ((present & ((uint64_t)1U << (uint64_t)check.attribute_kind)) == 0) {
            continue;
        }
        const sxml_schematron_attribute attribute = sxml_schematron_find_attribute(
            compilation, (sxml_schematron_attribute_query){.place = place,
                                                           .attribute_kind = check.attribute_kind});
        const sxml_byte_view value = sxml_xml_text_trim_whitespace(attribute.value);
        bool is_valid = true;
        switch (check.value_type) {
        case 0:
            is_valid = sxml_xml_text_is_ncname(value);
            break;
        case 2:
            is_valid = is_dynamic_value(compilation, value) || is_name_list(value, SXML_SCHEMATRON_NAME_LIST_NAMES);
            break;
        case 3:
            is_valid = sxml_byte_view_equals(value, SXML_TEXT("true")) ||
                       sxml_byte_view_equals(value, SXML_TEXT("false"));
            break;
        case 5:
            is_valid = is_name_list(value, SXML_SCHEMATRON_NAME_LIST_NCNAMES);
            break;
        default:
            break;
        }
        if (!is_valid) {
            status = report_at(compilation,
                               (place_error){.code = "syntax",
                                             .description = "an attribute's value is not of the type "
                                                            "the grammar gives it (Annex A, 7.2)",
                                             .place = place,
                                             .attribute_span = attribute.source_span,
                                             .subject = value});
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
    }
    if (kind == SXML_SCHEMATRON_ELEMENT_DIR && (present & SXML_BIT(VALUE)) != 0) {
        const sxml_schematron_attribute value = sxml_schematron_find_attribute(
            compilation, (sxml_schematron_attribute_query){.place = place,
                                                           .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_VALUE});
        const sxml_byte_view text = sxml_xml_text_trim_whitespace(value.value);
        if (!sxml_byte_view_equals(text, SXML_TEXT("ltr")) &&
            !sxml_byte_view_equals(text, SXML_TEXT("rtl"))) {
            status = report_at(compilation,
                               (place_error){.code = "syntax",
                                             .description = "the value of dir is neither ltr nor rtl "
                                                            "(5.5.4, Annex A)",
                                             .place = place,
                                             .attribute_span = value.source_span,
                                             .subject = text});
        }
    }
    const bool has_name_attribute = (present & SXML_BIT(NAME)) != 0;
    if (status == SXML_STATUS_OK && has_name_attribute &&
        (kind == SXML_SCHEMATRON_ELEMENT_LET || kind == SXML_SCHEMATRON_ELEMENT_PARAM)) {
        const sxml_schematron_attribute name = sxml_schematron_find_attribute(
            compilation, (sxml_schematron_attribute_query){.place = place,
                                                           .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_NAME});
        const sxml_byte_view text = sxml_xml_text_trim_whitespace(name.value);
        /* 5.4.11: a parameter's name has no colon; 7.2: names are
           well-formed. */
        const bool is_valid = kind == SXML_SCHEMATRON_ELEMENT_PARAM ? sxml_xml_text_is_ncname(text)
                                                                     : sxml_xml_text_is_name(text);
        if (!is_valid) {
            status = report_at(compilation,
                               (place_error){.code = "syntax",
                                             .description = "a variable or parameter name is not a "
                                                            "name (5.4.7, 5.4.11, 7.2)",
                                             .place = place,
                                             .attribute_span = name.source_span,
                                             .subject = text});
        }
    }
    if (status == SXML_STATUS_OK && kind == SXML_SCHEMATRON_ELEMENT_NS &&
        (present & SXML_BIT(PREFIX)) != 0) {
        const sxml_schematron_attribute prefix = sxml_schematron_find_attribute(
            compilation, (sxml_schematron_attribute_query){.place = place,
                                                           .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_PREFIX});
        const sxml_byte_view text = sxml_xml_text_trim_whitespace(prefix.value);
        if (!sxml_xml_text_is_ncname(text)) {
            status = report_at(compilation,
                               (place_error){.code = "syntax",
                                             .description = "an ns prefix is not a name without a "
                                                            "colon (5.4.10)",
                                             .place = place,
                                             .attribute_span = prefix.source_span,
                                             .subject = text});
        }
    }
    if (status == SXML_STATUS_OK && (present & SXML_BIT(ID)) != 0) {
        const sxml_byte_view identifier = trimmed_attribute(
            compilation, (sxml_schematron_attribute_query){.place = place,
                                                           .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_ID});
        if (sxml_schematron_find_identified(&reader->identifiers, identifier) != NULL) {
            /* Annex A: id is of type ID, one per document. */
            status = report_at(compilation,
                               (place_error){.code = "syntax",
                                             .description = "two elements have one id (Annex A)",
                                             .place = place,
                                             .attribute_span = {.byte_offset = 0,
                                                                .byte_count = 0,
                                                                .line_number = 0,
                                                                .column_number = 0},
                                             .subject = identifier});
        } else {
            status = push_identified(compilation, &reader->identifiers,
                                     (sxml_schematron_identified){.place = place,
                                                                  .identifier = identifier});
        }
    }
    return status;
}

/* True when the element's text children are all whitespace (5.3). */
/* What an element's text children hold. */
typedef struct text_content {
    bool is_whitespace;
} text_content;

static sxml_status has_only_whitespace(const sxml_schematron_compilation *compilation,
                                       sxml_schematron_place place, text_content *content) {
    bool *is_whitespace = &content->is_whitespace;
    *is_whitespace = true;
    sxml_node_description description;
    sxml_status status = describe(compilation, place, &description);
    if (status != SXML_STATUS_OK || !description.has_children) {
        return status;
    }
    sxml_node_index child = description.first_child;
    for (;;) {
        const sxml_schematron_place child_place = {.document = place.document, .element = child};
        sxml_node_description child_description;
        status = describe(compilation, child_place, &child_description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (child_description.node_kind == SXML_NODE_KIND_TEXT) {
            sxml_byte_view text = no_text;
            status = sxml_node_get_content(reference_of(compilation, child_place), &text);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            if (!sxml_xml_text_is_whitespace(text)) {
                *is_whitespace = false;
                return SXML_STATUS_OK;
            }
        }
        if (!child_description.has_next_sibling) {
            return SXML_STATUS_OK;
        }
        child = child_description.next_sibling;
    }
}

static bool allows_text(grammar_production production) {
    return production == SXML_PRODUCTION_MESSAGE || production == SXML_PRODUCTION_DYNAMIC_TEXT ||
           production == SXML_PRODUCTION_PARAGRAPH || production == SXML_PRODUCTION_TITLE;
}

/* The value of attribute abstract is "true". */
static bool is_abstract(const sxml_schematron_compilation *compilation, sxml_schematron_place place) {
    return sxml_byte_view_equals(
        trimmed_attribute(compilation,
                          (sxml_schematron_attribute_query){
                              .place = place, .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_ABSTRACT}),
        SXML_TEXT("true"));
}

static bool has_attribute(const sxml_schematron_compilation *compilation, sxml_schematron_place place,
                          sxml_schematron_attribute_kind kind) {
    return sxml_schematron_find_attribute(
               compilation,
               (sxml_schematron_attribute_query){.place = place, .attribute_kind = kind})
        .is_present;
}

/* The production a child is checked against, and its rank among its
   parent's children (0: anywhere); NONE when the parent's production does
   not allow it. */
typedef struct child_role {
    grammar_production production;
    uint32_t rank;
} child_role;

static child_role role_of_child(const sxml_schematron_compilation *compilation,
                                const check_frame *parent, sxml_schematron_child child) {
    const sxml_schematron_element_kind kind = child.element_kind;
    const child_role none = {.production = SXML_PRODUCTION_NONE, .rank = 0};
    switch (parent->production) {
    case SXML_PRODUCTION_SCHEMA:
    case SXML_PRODUCTION_LIBRARY: {
        const bool is_schema = parent->production == SXML_PRODUCTION_SCHEMA;
        switch ((uint32_t)kind) {
        case SXML_SCHEMATRON_ELEMENT_TITLE:
            return (child_role){.production = SXML_PRODUCTION_TITLE, .rank = 1};
        case SXML_SCHEMATRON_ELEMENT_NS:
            return is_schema ? (child_role){.production = SXML_PRODUCTION_EMPTY, .rank = 2} : none;
        case SXML_SCHEMATRON_ELEMENT_P:
            return (child_role){.production = SXML_PRODUCTION_PARAGRAPH,
                                .rank = parent->pattern_count > 0 ? 9U : 3U};
        case SXML_SCHEMATRON_ELEMENT_PARAM:
            return (child_role){.production = SXML_PRODUCTION_EMPTY, .rank = 4};
        case SXML_SCHEMATRON_ELEMENT_LET:
            return (child_role){.production = has_attribute(compilation, child.place,
                                                            SXML_SCHEMATRON_ATTRIBUTE_VALUE)
                                                  ? SXML_PRODUCTION_EMPTY
                                                  : SXML_PRODUCTION_LET_CONTENT,
                                .rank = 5};
        case SXML_SCHEMATRON_ELEMENT_PHASE:
            return is_schema ? (child_role){.production = SXML_PRODUCTION_PHASE, .rank = 6} : none;
        case SXML_SCHEMATRON_ELEMENT_RULES:
            return (child_role){.production = SXML_PRODUCTION_RULES, .rank = 7};
        case SXML_SCHEMATRON_ELEMENT_PATTERN:
        case SXML_SCHEMATRON_ELEMENT_GROUP:
            return (child_role){
                .production = is_abstract(compilation, child.place) ? SXML_PRODUCTION_ABSTRACT_PATTERN
                              : has_attribute(compilation, child.place, SXML_SCHEMATRON_ATTRIBUTE_IS_A)
                                  ? SXML_PRODUCTION_PATTERN_INSTANCE
                                  : SXML_PRODUCTION_PATTERN,
                .rank = 8};
        case SXML_SCHEMATRON_ELEMENT_DIAGNOSTICS:
            return (child_role){.production = SXML_PRODUCTION_DIAGNOSTICS, .rank = 10};
        case SXML_SCHEMATRON_ELEMENT_PROPERTIES:
            return (child_role){.production = SXML_PRODUCTION_PROPERTIES, .rank = 11};
        case SXML_SCHEMATRON_ELEMENT_EXTENDS:
        case SXML_SCHEMATRON_ELEMENT_INCLUDE:
            /* Malformed: the cursor yields what it cannot expand. */
            return (child_role){.production = SXML_PRODUCTION_EMPTY, .rank = 0};
        default:
            return none;
        }
    }
    case SXML_PRODUCTION_PATTERN:
    case SXML_PRODUCTION_ABSTRACT_PATTERN:
    case SXML_PRODUCTION_PATTERN_INSTANCE: {
        const bool has_rules = parent->production != SXML_PRODUCTION_PATTERN_INSTANCE;
        const bool has_parameters = parent->production != SXML_PRODUCTION_PATTERN;
        switch ((uint32_t)kind) {
        case SXML_SCHEMATRON_ELEMENT_TITLE:
            return (child_role){.production = SXML_PRODUCTION_TITLE, .rank = 1};
        case SXML_SCHEMATRON_ELEMENT_P:
            return (child_role){.production = SXML_PRODUCTION_PARAGRAPH, .rank = 2};
        case SXML_SCHEMATRON_ELEMENT_PARAM:
            return has_parameters ? (child_role){.production = SXML_PRODUCTION_EMPTY, .rank = 3}
                                  : none;
        case SXML_SCHEMATRON_ELEMENT_LET:
            return has_rules
                       ? (child_role){.production = has_attribute(compilation, child.place,
                                                                  SXML_SCHEMATRON_ATTRIBUTE_VALUE)
                                                        ? SXML_PRODUCTION_EMPTY
                                                        : SXML_PRODUCTION_LET_CONTENT,
                                      .rank = 4}
                       : none;
        case SXML_SCHEMATRON_ELEMENT_RULE:
            return has_rules ? (child_role){.production = is_abstract(compilation, child.place)
                                                              ? SXML_PRODUCTION_ABSTRACT_RULE
                                                              : SXML_PRODUCTION_RULE,
                                            .rank = 5}
                             : none;
        case SXML_SCHEMATRON_ELEMENT_INCLUDE:
            return (child_role){.production = SXML_PRODUCTION_EMPTY, .rank = 0};
        default:
            return none;
        }
    }
    case SXML_PRODUCTION_RULE:
    case SXML_PRODUCTION_ABSTRACT_RULE:
        switch ((uint32_t)kind) {
        case SXML_SCHEMATRON_ELEMENT_TITLE:
            return (child_role){.production = SXML_PRODUCTION_TITLE, .rank = 1};
        case SXML_SCHEMATRON_ELEMENT_LET:
            return (child_role){.production = has_attribute(compilation, child.place,
                                                            SXML_SCHEMATRON_ATTRIBUTE_VALUE)
                                                  ? SXML_PRODUCTION_EMPTY
                                                  : SXML_PRODUCTION_LET_CONTENT,
                                .rank = 2};
        case SXML_SCHEMATRON_ELEMENT_ASSERT:
        case SXML_SCHEMATRON_ELEMENT_REPORT:
            return (child_role){.production = SXML_PRODUCTION_MESSAGE, .rank = 3};
        case SXML_SCHEMATRON_ELEMENT_EXTENDS:
            return (child_role){.production = SXML_PRODUCTION_EMPTY, .rank = 3};
        case SXML_SCHEMATRON_ELEMENT_P:
            return (child_role){.production = SXML_PRODUCTION_PARAGRAPH, .rank = 3};
        case SXML_SCHEMATRON_ELEMENT_INCLUDE:
            return (child_role){.production = SXML_PRODUCTION_EMPTY, .rank = 0};
        default:
            return none;
        }
    case SXML_PRODUCTION_RULES:
        switch ((uint32_t)kind) {
        case SXML_SCHEMATRON_ELEMENT_TITLE:
            return (child_role){.production = SXML_PRODUCTION_TITLE, .rank = 1};
        case SXML_SCHEMATRON_ELEMENT_P:
            return (child_role){.production = SXML_PRODUCTION_PARAGRAPH, .rank = 2};
        case SXML_SCHEMATRON_ELEMENT_RULE:
            return is_abstract(compilation, child.place)
                       ? (child_role){.production = SXML_PRODUCTION_ABSTRACT_RULE, .rank = 3}
                       : none;
        default:
            return none;
        }
    case SXML_PRODUCTION_PHASE:
        switch ((uint32_t)kind) {
        case SXML_SCHEMATRON_ELEMENT_P:
            return (child_role){.production = SXML_PRODUCTION_PARAGRAPH, .rank = 1};
        case SXML_SCHEMATRON_ELEMENT_LET:
            return (child_role){.production = has_attribute(compilation, child.place,
                                                            SXML_SCHEMATRON_ATTRIBUTE_VALUE)
                                                  ? SXML_PRODUCTION_EMPTY
                                                  : SXML_PRODUCTION_LET_CONTENT,
                                .rank = 2};
        case SXML_SCHEMATRON_ELEMENT_ACTIVE:
            return (child_role){.production = SXML_PRODUCTION_PARAGRAPH, .rank = 3};
        case SXML_SCHEMATRON_ELEMENT_INCLUDE:
            return (child_role){.production = SXML_PRODUCTION_EMPTY, .rank = 0};
        default:
            return none;
        }
    case SXML_PRODUCTION_DIAGNOSTICS:
        return kind == SXML_SCHEMATRON_ELEMENT_DIAGNOSTIC
                   ? (child_role){.production = SXML_PRODUCTION_MESSAGE, .rank = 0}
               : kind == SXML_SCHEMATRON_ELEMENT_INCLUDE
                   ? (child_role){.production = SXML_PRODUCTION_EMPTY, .rank = 0}
                   : none;
    case SXML_PRODUCTION_PROPERTIES:
        return kind == SXML_SCHEMATRON_ELEMENT_PROPERTY
                   ? (child_role){.production = SXML_PRODUCTION_MESSAGE, .rank = 0}
                   : none;
    case SXML_PRODUCTION_MESSAGE:
        switch ((uint32_t)kind) {
        case SXML_SCHEMATRON_ELEMENT_VALUE_OF:
        case SXML_SCHEMATRON_ELEMENT_NAME:
            return (child_role){.production = SXML_PRODUCTION_EMPTY, .rank = 0};
        case SXML_SCHEMATRON_ELEMENT_EMPH:
        case SXML_SCHEMATRON_ELEMENT_DIR:
        case SXML_SCHEMATRON_ELEMENT_SPAN:
            return (child_role){.production = SXML_PRODUCTION_DYNAMIC_TEXT, .rank = 0};
        default:
            return none;
        }
    case SXML_PRODUCTION_DYNAMIC_TEXT:
        return kind == SXML_SCHEMATRON_ELEMENT_VALUE_OF || kind == SXML_SCHEMATRON_ELEMENT_NAME
                   ? (child_role){.production = SXML_PRODUCTION_EMPTY, .rank = 0}
                   : none;
    case SXML_PRODUCTION_PARAGRAPH:
        switch ((uint32_t)kind) {
        case SXML_SCHEMATRON_ELEMENT_EMPH:
        case SXML_SCHEMATRON_ELEMENT_DIR:
        case SXML_SCHEMATRON_ELEMENT_SPAN:
            return (child_role){.production = SXML_PRODUCTION_DYNAMIC_TEXT, .rank = 0};
        default:
            return none;
        }
    case SXML_PRODUCTION_TITLE:
        return kind == SXML_SCHEMATRON_ELEMENT_DIR
                   ? (child_role){.production = SXML_PRODUCTION_DYNAMIC_TEXT, .rank = 0}
                   : none;
    case SXML_PRODUCTION_LET_CONTENT:
    case SXML_PRODUCTION_EMPTY:
    case SXML_PRODUCTION_NONE:
        break;
    }
    return none;
}

/* Records a child of the schema (or a library inserted in it) in the
   index (D315 "The path", step 1). */
static sxml_status record_top_level(schema_reader *reader, sxml_schematron_child child) {
    sxml_schematron_compilation *compilation = reader->compilation;
    sxml_schematron_index *index = &compilation->index;
    switch ((uint32_t)child.element_kind) {
    case SXML_SCHEMATRON_ELEMENT_TITLE:
        if (!index->has_title) {
            index->has_title = true;
            index->title = child.place;
        }
        return SXML_STATUS_OK;
    case SXML_SCHEMATRON_ELEMENT_NS:
        return push_place(compilation, &index->namespaces, child.place);
    case SXML_SCHEMATRON_ELEMENT_PARAM:
        return push_place(compilation, &index->schema_parameters, child.place);
    case SXML_SCHEMATRON_ELEMENT_LET:
        return push_place(compilation, &index->schema_lets, child.place);
    case SXML_SCHEMATRON_ELEMENT_PHASE:
        return push_identified(
            compilation, &index->phases,
            (sxml_schematron_identified){
                .place = child.place,
                .identifier = trimmed_attribute(
                    compilation, (sxml_schematron_attribute_query){
                                     .place = child.place, .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_ID})});
    case SXML_SCHEMATRON_ELEMENT_PATTERN:
    case SXML_SCHEMATRON_ELEMENT_GROUP: {
        sxml_status status = push_place(compilation, &index->patterns, child.place);
        const sxml_byte_view identifier = trimmed_attribute(
            compilation, (sxml_schematron_attribute_query){.place = child.place,
                                                           .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_ID});
        if (status == SXML_STATUS_OK && identifier.byte_count > 0) {
            status = push_identified(compilation,
                                     is_abstract(compilation, child.place)
                                         ? &index->abstract_patterns
                                         : &index->pattern_identifiers,
                                     (sxml_schematron_identified){.place = child.place,
                                                                  .identifier = identifier});
        }
        return status;
    }
    default:
        return SXML_STATUS_OK;
    }
}

/* What the walk notes of an element it enters, beyond its attributes. */
/* An element the walk enters: its parent's frame, the child and its
   production. */
typedef struct noted_element {
    const check_frame *parent;
    sxml_schematron_child child;
    grammar_production production;
} noted_element;

static sxml_status note_element(schema_reader *reader, noted_element noted) {
    const check_frame *parent = noted.parent;
    const sxml_schematron_child child = noted.child;
    const grammar_production child_production = noted.production;
    sxml_schematron_compilation *compilation = reader->compilation;
    sxml_schematron_index *index = &compilation->index;
    sxml_status status = SXML_STATUS_OK;
    const sxml_byte_view identifier = trimmed_attribute(
        compilation, (sxml_schematron_attribute_query){.place = child.place,
                                                       .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_ID});
    switch ((uint32_t)child.element_kind) {
    case SXML_SCHEMATRON_ELEMENT_RULE:
        if (child_production == SXML_PRODUCTION_ABSTRACT_RULE) {
            /* 5.4.15: an abstract rule has an id and no context. */
            if (identifier.byte_count == 0) {
                status = report_at(compilation,
                                   (place_error){.code = "syntax",
                                                 .description = "an abstract rule has no id (5.4.15, "
                                                                "Annex A)",
                                                 .place = child.place,
                                                 .attribute_span = {.line_number = 0},
                                                 .subject = no_text});
            } else if (has_attribute(compilation, child.place, SXML_SCHEMATRON_ATTRIBUTE_CONTEXT)) {
                status = report_at(compilation,
                                   (place_error){.code = "syntax",
                                                 .description = "an abstract rule has a context "
                                                                "(5.4.15)",
                                                 .place = child.place,
                                                 .attribute_span = {.line_number = 0},
                                                 .subject = identifier});
            } else if (has_attribute(compilation, child.place, SXML_SCHEMATRON_ATTRIBUTE_VISIT_EACH)) {
                status = report_at(compilation,
                                   (place_error){.code = "syntax",
                                                 .description = "an abstract rule has a visit-each "
                                                                "attribute (5.4.21, Annex A)",
                                                 .place = child.place,
                                                 .attribute_span = {.line_number = 0},
                                                 .subject = identifier});
            } else {
                status = push_identified(compilation, &index->abstract_rules,
                                         (sxml_schematron_identified){.place = child.place,
                                                                      .identifier = identifier});
            }
        } else if (!has_attribute(compilation, child.place, SXML_SCHEMATRON_ATTRIBUTE_CONTEXT)) {
            status = report_at(compilation,
                               (place_error){.code = "syntax",
                                             .description = "a rule that is not abstract has no "
                                                            "context (5.4.15, Annex A)",
                                             .place = child.place,
                                             .attribute_span = {.line_number = 0},
                                             .subject = no_text});
        }
        break;
    case SXML_SCHEMATRON_ELEMENT_PATTERN:
    case SXML_SCHEMATRON_ELEMENT_GROUP:
        if (child_production == SXML_PRODUCTION_ABSTRACT_PATTERN) {
            if (identifier.byte_count == 0 ||
                has_attribute(compilation, child.place, SXML_SCHEMATRON_ATTRIBUTE_IS_A)) {
                status = report_at(compilation,
                                   (place_error){.code = "syntax",
                                                 .description = "an abstract pattern has no id, or "
                                                                "has an is-a (5.4.12)",
                                                 .place = child.place,
                                                 .attribute_span = {.line_number = 0},
                                                 .subject = identifier});
            }
        } else if (child_production == SXML_PRODUCTION_PATTERN_INSTANCE) {
            status = push_place(compilation, &reader->references, child.place);
        }
        break;
    case SXML_SCHEMATRON_ELEMENT_DIAGNOSTIC:
        status = push_identified(compilation, &index->diagnostics,
                                 (sxml_schematron_identified){.place = child.place,
                                                              .identifier = identifier});
        break;
    case SXML_SCHEMATRON_ELEMENT_PROPERTY:
        status = push_identified(compilation, &index->properties,
                                 (sxml_schematron_identified){.place = child.place,
                                                              .identifier = identifier});
        break;
    case SXML_SCHEMATRON_ELEMENT_ASSERT:
    case SXML_SCHEMATRON_ELEMENT_REPORT:
        if (has_attribute(compilation, child.place, SXML_SCHEMATRON_ATTRIBUTE_DIAGNOSTICS) ||
            has_attribute(compilation, child.place, SXML_SCHEMATRON_ATTRIBUTE_PROPERTIES)) {
            status = push_place(compilation, &reader->references, child.place);
        }
        break;
    case SXML_SCHEMATRON_ELEMENT_ACTIVE:
        status = push_place(compilation, &reader->references, child.place);
        break;
    case SXML_SCHEMATRON_ELEMENT_EXTENDS: {
        const bool has_rule = has_attribute(compilation, child.place, SXML_SCHEMATRON_ATTRIBUTE_RULE);
        const bool has_href = has_attribute(compilation, child.place, SXML_SCHEMATRON_ATTRIBUTE_HREF);
        if (has_rule == has_href) {
            status = report_at(compilation,
                               (place_error){.code = "syntax",
                                             .description = "an extends has both or neither of rule "
                                                            "and href (5.4.4)",
                                             .place = child.place,
                                             .attribute_span = {.line_number = 0},
                                             .subject = no_text});
        } else if (has_rule && parent->production != SXML_PRODUCTION_RULE &&
                   parent->production != SXML_PRODUCTION_ABSTRACT_RULE) {
            status = report_at(compilation,
                               (place_error){.code = "syntax",
                                             .description = "an extends rule outside a rule (5.4.4)",
                                             .place = child.place,
                                             .attribute_span = {.line_number = 0},
                                             .subject = no_text});
        } else if (has_rule) {
            status = push_place(compilation, &reader->references, child.place);
        }
        break;
    }
    case SXML_SCHEMATRON_ELEMENT_PHASE: {
        /* 5.4.13: the reserved names are not phase names. */
        if (sxml_byte_view_equals(identifier, SXML_TEXT("#ALL")) ||
            sxml_byte_view_equals(identifier, SXML_TEXT("#ANY")) ||
            sxml_byte_view_equals(identifier, SXML_TEXT("#DEFAULT"))) {
            status = report_at(compilation,
                               (place_error){.code = "syntax",
                                             .description = "a phase has a reserved name (5.4.13)",
                                             .place = child.place,
                                             .attribute_span = {.line_number = 0},
                                             .subject = identifier});
        }
        break;
    }
    case SXML_SCHEMATRON_ELEMENT_LET:
        status = push_place(compilation, &reader->lets, child.place);
        break;
    case SXML_SCHEMATRON_ELEMENT_PARAM:
        if (parent->production == SXML_PRODUCTION_ABSTRACT_PATTERN) {
            status = push_place(compilation, &reader->lets, child.place);
        }
        break;
    default:
        break;
    }
    return status;
}

/* Pushes a frame for an element to check, and its cursor level. */
static sxml_status enter_element(schema_reader *reader, check_frame frame) {
    sxml_status status = sxml_schematron_cursor_start(&reader->cursor, frame.place);
    if (status == SXML_STATUS_OK) {
        status = sxml_schematron_array_push(reader->compilation, &reader->frames,
                                            SXML_SCHEMATRON_ELEMENT_OF(frame));
    }
    return status;
}

/* The checks at an element's end: the children its production requires. */
static sxml_status end_element(schema_reader *reader, const check_frame *frame) {
    sxml_schematron_compilation *compilation = reader->compilation;
    const char *description = NULL;
    switch ((uint32_t)frame->production) {
    case SXML_PRODUCTION_SCHEMA:
        if (frame->pattern_count == 0) {
            description = "a schema has no pattern or group (Annex A)";
        }
        break;
    case SXML_PRODUCTION_RULE:
    case SXML_PRODUCTION_ABSTRACT_RULE:
        if (frame->required_count == 0) {
            description = "a rule has no assert, report, extends or p (Annex A)";
        }
        break;
    case SXML_PRODUCTION_RULES:
        if (frame->required_count == 0) {
            description = "a rules element has no abstract rule (Annex A)";
        }
        break;
    case SXML_PRODUCTION_LET_CONTENT:
        if (frame->required_count == 0) {
            description = "a let has neither a value attribute nor content (5.4.7, Annex A)";
        }
        break;
    default:
        break;
    }
    if (description == NULL) {
        return SXML_STATUS_OK;
    }
    return report_at(compilation, (place_error){.code = "syntax",
                                                .description = description,
                                                .place = frame->place,
                                                .attribute_span = {.line_number = 0},
                                                .subject = no_text});
}

/* Checks one child of the element on top and enters it when its content
   is checked too. */
static sxml_status check_child(schema_reader *reader, sxml_schematron_child child) {
    sxml_schematron_compilation *compilation = reader->compilation;
    check_frame *parent = frame_at(reader, reader->frames.count - 1U);
    if (child.element_kind == SXML_SCHEMATRON_ELEMENT_FOREIGN) {
        /* 5.2: foreign elements anywhere; a let's content is foreign; XSLT
           declarations of the schema are read under the xslt bindings
           (Annexes C, H, J; the builder picks them). */
        if (parent->production == SXML_PRODUCTION_SCHEMA ||
            parent->production == SXML_PRODUCTION_LIBRARY) {
            return push_place(compilation, &compilation->index.foreign_declarations, child.place);
        }
        if (parent->production == SXML_PRODUCTION_LET_CONTENT) {
            parent->required_count += 1;
        } else if (parent->production == SXML_PRODUCTION_EMPTY) {
            return report_at(compilation,
                             (place_error){.code = "syntax",
                                           .description = "an element where the grammar allows none "
                                                          "(Annex A)",
                                           .place = child.place,
                                           .attribute_span = {.line_number = 0},
                                           .subject = no_text});
        }
        return SXML_STATUS_OK;
    }
    if (child.is_included &&
        role_of_child(compilation, parent,
                      (sxml_schematron_child){.has_child = true,
                                              .place = child.included_by,
                                              .element_kind = SXML_SCHEMATRON_ELEMENT_INCLUDE,
                                              .is_inserted = false,
                                              .is_included = false,
                                              .included_by = child.included_by})
                .production == SXML_PRODUCTION_NONE) {
        return report_at(compilation,
                         (place_error){.code = "syntax",
                                       .description = "an include where the grammar does not allow "
                                                      "one (Annex A)",
                                       .place = child.included_by,
                                       .attribute_span = {.line_number = 0},
                                       .subject = no_text});
    }
    if (child.is_inserted && child.element_kind == SXML_SCHEMATRON_ELEMENT_NS) {
        /* An ns of a library that sch:extends href inserts: the library's
           grammar has none (Annex A). */
        sxml_node_description description;
        sxml_status status = describe(compilation, child.place, &description);
        if (status == SXML_STATUS_OK && description.has_parent) {
            const sxml_schematron_place written_parent = {.document = child.place.document,
                                                          .element = description.parent};
            status = describe(compilation, written_parent, &description);
            if (status == SXML_STATUS_OK && description.node_kind == SXML_NODE_KIND_ELEMENT &&
                kind_of_name(compilation, description.node_name) == SXML_SCHEMATRON_ELEMENT_LIBRARY) {
                status = report_at(compilation,
                                   (place_error){.code = "syntax",
                                                 .description = "an element where the grammar does "
                                                                "not allow it (Annex A)",
                                                 .place = child.place,
                                                 .attribute_span = {.line_number = 0},
                                                 .subject = no_text});
            }
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    const child_role role = role_of_child(compilation, parent, child);
    if (role.production == SXML_PRODUCTION_NONE) {
        return report_at(compilation,
                         (place_error){.code = "syntax",
                                       .description = child.element_kind == SXML_SCHEMATRON_ELEMENT_UNKNOWN
                                                          ? "an element of the Schematron namespace "
                                                            "that the grammar does not define "
                                                            "(Annex A)"
                                                          : "an element where the grammar does not "
                                                            "allow it (Annex A)",
                                       .place = child.place,
                                       .attribute_span = {.line_number = 0},
                                       .subject = no_text});
    }
    sxml_status status = SXML_STATUS_OK;
    const bool is_top = parent->production == SXML_PRODUCTION_SCHEMA ||
                        parent->production == SXML_PRODUCTION_LIBRARY;
    /* Order (Annex A): ranks do not go down, single elements once. */
    if (role.rank != 0 && !child.is_inserted) {
        const bool is_once = child.element_kind == SXML_SCHEMATRON_ELEMENT_TITLE ||
                             child.element_kind == SXML_SCHEMATRON_ELEMENT_DIAGNOSTICS ||
                             child.element_kind == SXML_SCHEMATRON_ELEMENT_PROPERTIES;
        if (role.rank < parent->last_rank || (is_once && role.rank == parent->last_rank)) {
            status = report_at(compilation,
                               (place_error){.code = "syntax",
                                             .description = "an element out of the order of the "
                                                            "grammar, or repeated (Annex A)",
                                             .place = child.place,
                                             .attribute_span = {.line_number = 0},
                                             .subject = no_text});
            if (status != SXML_STATUS_OK) {
                return status;
            }
        } else {
            parent->last_rank = role.rank;
        }
    }
    if (child.element_kind == SXML_SCHEMATRON_ELEMENT_PATTERN ||
        child.element_kind == SXML_SCHEMATRON_ELEMENT_GROUP) {
        parent->pattern_count += 1;
    }
    if (((parent->production == SXML_PRODUCTION_RULE ||
          parent->production == SXML_PRODUCTION_ABSTRACT_RULE) &&
         role.rank == 3) ||
        (parent->production == SXML_PRODUCTION_RULES && role.rank == 3)) {
        parent->required_count += 1;
    }
    const bool is_in_abstract_pattern = parent->is_in_abstract_pattern ||
                                        role.production == SXML_PRODUCTION_ABSTRACT_PATTERN;
    const check_frame parent_copy = *parent;
    status = check_attributes(reader, child.place, child.element_kind);
    if (status == SXML_STATUS_OK) {
        status = note_element(reader, (noted_element){.parent = &parent_copy, .child = child, .production = role.production});
    }
    if (status == SXML_STATUS_OK && is_top) {
        status = record_top_level(reader, child);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!allows_text(role.production)) {
        text_content content = {.is_whitespace = true};
        status = has_only_whitespace(compilation, child.place, &content);
        if (status == SXML_STATUS_OK && !content.is_whitespace) {
            status = report_at(compilation,
                               (place_error){.code = "syntax",
                                             .description = "text where the grammar allows none "
                                                            "(Annex A, 5.3)",
                                             .place = child.place,
                                             .attribute_span = {.line_number = 0},
                                             .subject = no_text});
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return enter_element(reader, (check_frame){.place = child.place,
                                               .element_kind = child.element_kind,
                                               .production = role.production,
                                               .last_rank = 0,
                                               .title_count = 0,
                                               .pattern_count = 0,
                                               .diagnostics_count = 0,
                                               .properties_count = 0,
                                               .required_count = 0,
                                               .is_in_abstract_pattern = is_in_abstract_pattern});
}

/* Walks the schema's effective tree once, checking every element. */
static sxml_status walk_schema(schema_reader *reader) {
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && reader->frames.count > 0) {
        sxml_schematron_child child = {.has_child = false,
                                       .place = {.document = 0, .element = {.index = 0}},
                                       .element_kind = SXML_SCHEMATRON_ELEMENT_NONE};
        status = sxml_schematron_cursor_next(&reader->cursor, &child);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (!child.has_child) {
            const check_frame ended = *frame_at(reader, reader->frames.count - 1U);
            reader->frames.count -= 1;
            status = end_element(reader, &ended);
            continue;
        }
        status = check_child(reader, child);
    }
    return status;
}

/* ------------------------------------------------------ References */

/* Checks each id of an IDREFS attribute against a table. */
typedef struct reference_list_check {
    sxml_schematron_place place;
    sxml_schematron_attribute_kind attribute_kind;
    const sxml_schematron_array *table;
    const char *description;
} reference_list_check;

static sxml_status check_reference_list(sxml_schematron_compilation *compilation,
                                        reference_list_check check) {
    const sxml_schematron_attribute attribute = sxml_schematron_find_attribute(
        compilation,
        (sxml_schematron_attribute_query){.place = check.place, .attribute_kind = check.attribute_kind});
    if (!attribute.is_present) {
        return SXML_STATUS_OK;
    }
    sxml_byte_view rest = attribute.value;
    sxml_byte_view token = no_text;
    while (sxml_xml_text_next_token(&rest, &token)) {
        if (sxml_schematron_find_identified(check.table, token) == NULL) {
            const sxml_status status = report_at(compilation,
                                                 (place_error){.code = "reference",
                                                               .description = check.description,
                                                               .place = check.place,
                                                               .attribute_span = attribute.source_span,
                                                               .subject = token});
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
    }
    return SXML_STATUS_OK;
}

/* The sch:param children of an element, in order. */
static sxml_status collect_parameters(sxml_schematron_compilation *compilation,
                                      sxml_schematron_place place, sxml_schematron_array *parameters) {
    sxml_schematron_cursor cursor = {.compilation = compilation,
                                     .frames = {.elements = NULL, .count = 0, .capacity = 0},
                                     .expands_rules = false};
    sxml_status status = sxml_schematron_cursor_start(&cursor, place);
    for (;;) {
        sxml_schematron_child child = {.has_child = false,
                                       .place = {.document = 0, .element = {.index = 0}},
                                       .element_kind = SXML_SCHEMATRON_ELEMENT_NONE};
        if (status == SXML_STATUS_OK) {
            status = sxml_schematron_cursor_next(&cursor, &child);
        }
        if (status != SXML_STATUS_OK || !child.has_child) {
            break;
        }
        if (child.element_kind == SXML_SCHEMATRON_ELEMENT_PARAM) {
            status = push_place(compilation, parameters, child.place);
        }
    }
    sxml_schematron_cursor_finish(&cursor);
    return status;
}

static sxml_byte_view parameter_name(const sxml_schematron_compilation *compilation,
                                     sxml_schematron_place parameter) {
    return trimmed_attribute(compilation,
                             (sxml_schematron_attribute_query){
                                 .place = parameter, .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_NAME});
}

/* 5.4.11: names distinct within the scope of a pattern or schema. */
static sxml_status check_distinct_parameters(sxml_schematron_compilation *compilation,
                                             const sxml_schematron_array *parameters) {
    const sxml_schematron_place *places = parameters->elements;
    for (size_t parameter_index = 0; parameter_index < parameters->count; parameter_index += 1) {
        const sxml_byte_view name = parameter_name(compilation, places[parameter_index]);
        for (size_t earlier_index = 0; earlier_index < parameter_index; earlier_index += 1) {
            if (sxml_byte_view_equals(name, parameter_name(compilation, places[earlier_index]))) {
                const sxml_status status = report_at(
                    compilation, (place_error){.code = "parameter",
                                               .description = "a parameter is named twice in one "
                                                              "scope (5.4.11, Annex B)",
                                               .place = places[parameter_index],
                                               .attribute_span = {.line_number = 0},
                                               .subject = name});
                if (status != SXML_STATUS_OK) {
                    return status;
                }
                break;
            }
        }
    }
    return SXML_STATUS_OK;
}

/* A pattern instance against its abstract pattern (5.4.11, Annex B). */
static sxml_status check_instance(sxml_schematron_compilation *compilation,
                                  sxml_schematron_place instance) {
    const sxml_schematron_attribute is_a = sxml_schematron_find_attribute(
        compilation, (sxml_schematron_attribute_query){.place = instance,
                                                       .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_IS_A});
    const sxml_byte_view identifier = sxml_xml_text_trim_whitespace(is_a.value);
    const sxml_schematron_identified *abstract_pattern =
        sxml_schematron_find_identified(&compilation->index.abstract_patterns, identifier);
    if (abstract_pattern == NULL) {
        return report_at(compilation,
                         (place_error){.code = "reference",
                                       .description = "is-a names no abstract pattern (5.4.12, "
                                                      "Annex B)",
                                       .place = instance,
                                       .attribute_span = is_a.source_span,
                                       .subject = identifier});
    }
    sxml_schematron_array given = {.elements = NULL, .count = 0, .capacity = 0};
    sxml_schematron_array declared = {.elements = NULL, .count = 0, .capacity = 0};
    sxml_status status = collect_parameters(compilation, instance, &given);
    if (status == SXML_STATUS_OK) {
        status = collect_parameters(compilation, abstract_pattern->place, &declared);
    }
    if (status == SXML_STATUS_OK) {
        status = check_distinct_parameters(compilation, &given);
    }
    const sxml_schematron_place *given_places = given.elements;
    const sxml_schematron_place *declared_places = declared.elements;
    /* An abstract pattern that declares none takes the instance's table
       as it is (the third edition's form, D315). */
    for (size_t given_index = 0; status == SXML_STATUS_OK && declared.count > 0 &&
                                 given_index < given.count;
         given_index += 1) {
        const sxml_byte_view name = parameter_name(compilation, given_places[given_index]);
        bool is_declared = false;
        for (size_t declared_index = 0; declared_index < declared.count; declared_index += 1) {
            if (sxml_byte_view_equals(name,
                                      parameter_name(compilation, declared_places[declared_index]))) {
                is_declared = true;
            }
        }
        if (!is_declared) {
            status = report_at(compilation,
                               (place_error){.code = "parameter",
                                             .description = "the abstract pattern declares no "
                                                            "parameter of this name (5.4.11, Annex B)",
                                             .place = given_places[given_index],
                                             .attribute_span = {.line_number = 0},
                                             .subject = name});
        }
    }
    for (size_t declared_index = 0; status == SXML_STATUS_OK && declared_index < declared.count;
         declared_index += 1) {
        const sxml_schematron_place parameter = declared_places[declared_index];
        if (has_attribute(compilation, parameter, SXML_SCHEMATRON_ATTRIBUTE_VALUE)) {
            continue;
        }
        const sxml_byte_view name = parameter_name(compilation, parameter);
        bool has_value = false;
        for (size_t given_index = 0; given_index < given.count; given_index += 1) {
            if (sxml_byte_view_equals(name, parameter_name(compilation, given_places[given_index])) &&
                has_attribute(compilation, given_places[given_index], SXML_SCHEMATRON_ATTRIBUTE_VALUE)) {
                has_value = true;
            }
        }
        if (!has_value) {
            status = report_at(compilation,
                               (place_error){.code = "parameter",
                                             .description = "a parameter of the abstract pattern has "
                                                            "no value (5.4.11, Annex B)",
                                             .place = instance,
                                             .attribute_span = {.line_number = 0},
                                             .subject = name});
        }
    }
    /* An instance's parameter without a value takes the abstract
       pattern's default; with neither it was reported above, or (an
       abstract pattern that declares none) here. */
    for (size_t given_index = 0; status == SXML_STATUS_OK && declared.count == 0 &&
                                 given_index < given.count;
         given_index += 1) {
        if (!has_attribute(compilation, given_places[given_index], SXML_SCHEMATRON_ATTRIBUTE_VALUE)) {
            status = report_at(compilation,
                               (place_error){.code = "parameter",
                                             .description = "a pattern parameter has no value "
                                                            "(5.4.11, Annex B)",
                                             .place = given_places[given_index],
                                             .attribute_span = {.line_number = 0},
                                             .subject = parameter_name(compilation,
                                                                       given_places[given_index])});
        }
    }
    sxml_schematron_array_release(compilation, &given);
    sxml_schematron_array_release(compilation, &declared);
    return status;
}

/* Every reference noted while walking, once every id is known. */
static sxml_status check_references(schema_reader *reader) {
    sxml_schematron_compilation *compilation = reader->compilation;
    const sxml_schematron_index *index = &compilation->index;
    const sxml_schematron_place *places = reader->references.elements;
    sxml_status status = SXML_STATUS_OK;
    for (size_t place_index = 0; status == SXML_STATUS_OK && place_index < reader->references.count;
         place_index += 1) {
        const sxml_schematron_place place = places[place_index];
        switch ((uint32_t)sxml_schematron_element_kind_of(compilation, place)) {
        case SXML_SCHEMATRON_ELEMENT_ACTIVE: {
            const sxml_schematron_attribute pattern = sxml_schematron_find_attribute(
                compilation, (sxml_schematron_attribute_query){
                                 .place = place, .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_PATTERN});
            const sxml_byte_view identifier = sxml_xml_text_trim_whitespace(pattern.value);
            if (pattern.is_present &&
                sxml_schematron_find_identified(&index->pattern_identifiers, identifier) == NULL &&
                sxml_schematron_find_identified(&index->abstract_patterns, identifier) == NULL) {
                status = report_at(compilation,
                                   (place_error){.code = "reference",
                                                 .description = "an active names no pattern (5.4.2, "
                                                                "Annex B)",
                                                 .place = place,
                                                 .attribute_span = pattern.source_span,
                                                 .subject = identifier});
            }
            break;
        }
        case SXML_SCHEMATRON_ELEMENT_EXTENDS: {
            const sxml_schematron_attribute rule = sxml_schematron_find_attribute(
                compilation, (sxml_schematron_attribute_query){
                                 .place = place, .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_RULE});
            const sxml_byte_view identifier = sxml_xml_text_trim_whitespace(rule.value);
            if (sxml_schematron_find_identified(&index->abstract_rules, identifier) == NULL) {
                status = report_at(compilation,
                                   (place_error){.code = "reference",
                                                 .description = "an extends names no abstract rule "
                                                                "(5.4.4, Annex B)",
                                                 .place = place,
                                                 .attribute_span = rule.source_span,
                                                 .subject = identifier});
            }
            break;
        }
        case SXML_SCHEMATRON_ELEMENT_PATTERN:
        case SXML_SCHEMATRON_ELEMENT_GROUP:
            status = check_instance(compilation, place);
            break;
        case SXML_SCHEMATRON_ELEMENT_ASSERT:
        case SXML_SCHEMATRON_ELEMENT_REPORT:
            status = check_reference_list(
                compilation,
                (reference_list_check){.place = place,
                                       .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_DIAGNOSTICS,
                                       .table = &index->diagnostics,
                                       .description = "diagnostics names no diagnostic (5.5.2)"});
            if (status == SXML_STATUS_OK) {
                status = check_reference_list(
                    compilation,
                    (reference_list_check){.place = place,
                                           .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_PROPERTIES,
                                           .table = &index->properties,
                                           .description = "properties names no property (5.5.11)"});
            }
            break;
        default:
            break;
        }
    }
    /* defaultPhase (5.4.17). */
    const sxml_schematron_attribute default_phase = sxml_schematron_find_attribute(
        compilation, (sxml_schematron_attribute_query){.place = index->schema,
                                                       .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_DEFAULT_PHASE});
    const sxml_byte_view default_phase_id = sxml_xml_text_trim_whitespace(default_phase.value);
    if (status == SXML_STATUS_OK && default_phase.is_present &&
        sxml_schematron_find_identified(&index->phases, default_phase_id) == NULL) {
        status = report_at(compilation,
                           (place_error){.code = "reference",
                                         .description = "defaultPhase names no phase (5.4.17)",
                                         .place = index->schema,
                                         .attribute_span = default_phase.source_span,
                                         .subject = default_phase_id});
    }
    /* Schema parameters: distinct, each with a value (Annex B,
       schema-param-value) unless the configuration gives one, and every
       configured name a schema parameter (5.4.11). */
    if (status == SXML_STATUS_OK) {
        status = check_distinct_parameters(compilation, &index->schema_parameters);
    }
    const sxml_schematron_place *parameters = index->schema_parameters.elements;
    const sxml_schematron_parameter_view configured = compilation->configuration->parameters;
    for (size_t parameter_index = 0;
         status == SXML_STATUS_OK && parameter_index < index->schema_parameters.count;
         parameter_index += 1) {
        const sxml_byte_view name = parameter_name(compilation, parameters[parameter_index]);
        bool is_configured = false;
        for (size_t configured_index = 0; configured_index < configured.parameter_count;
             configured_index += 1) {
            if (sxml_byte_view_equals(configured.parameters[configured_index].name, name)) {
                is_configured = true;
            }
        }
        if (!is_configured &&
            !has_attribute(compilation, parameters[parameter_index], SXML_SCHEMATRON_ATTRIBUTE_VALUE)) {
            status = report_at(compilation,
                               (place_error){.code = "parameter",
                                             .description = "a schema parameter has no value (5.4.11, "
                                                            "Annex B)",
                                             .place = parameters[parameter_index],
                                             .attribute_span = {.line_number = 0},
                                             .subject = name});
        }
    }
    for (size_t configured_index = 0;
         status == SXML_STATUS_OK && configured_index < configured.parameter_count;
         configured_index += 1) {
        const sxml_byte_view name = configured.parameters[configured_index].name;
        bool is_declared = false;
        for (size_t parameter_index = 0; parameter_index < index->schema_parameters.count;
             parameter_index += 1) {
            if (sxml_byte_view_equals(parameter_name(compilation, parameters[parameter_index]), name)) {
                is_declared = true;
            }
        }
        if (!is_declared) {
            status = report_at(compilation,
                               (place_error){.code = "parameter",
                                             .description = "a configured parameter names no schema "
                                                            "parameter (5.4.11)",
                                             .place = index->schema,
                                             .attribute_span = {.line_number = 0},
                                             .subject = name});
        }
    }
    /* Annex B, param-variable-clash: a let named as an abstract pattern's
       parameter (schematron:variable, D315). */
    const sxml_schematron_place *lets = reader->lets.elements;
    for (size_t let_index = 0; status == SXML_STATUS_OK && let_index < reader->lets.count;
         let_index += 1) {
        if (sxml_schematron_element_kind_of(compilation, lets[let_index]) !=
            SXML_SCHEMATRON_ELEMENT_LET) {
            continue;
        }
        const sxml_byte_view name = parameter_name(compilation, lets[let_index]);
        for (size_t other_index = 0; other_index < reader->lets.count; other_index += 1) {
            if (sxml_schematron_element_kind_of(compilation, lets[other_index]) ==
                    SXML_SCHEMATRON_ELEMENT_PARAM &&
                sxml_byte_view_equals(name, parameter_name(compilation, lets[other_index]))) {
                status = report_at(compilation,
                                   (place_error){.code = "variable",
                                                 .description = "a variable has the name of an "
                                                                "abstract pattern's parameter "
                                                                "(Annex B)",
                                                 .place = lets[let_index],
                                                 .attribute_span = {.line_number = 0},
                                                 .subject = name});
                break;
            }
        }
    }
    return status;
}

/* ---------------------------------------------------------------- Entry */

/* The compiled document: parsed, resolved or given as a node. */
static sxml_status read_compiled_document(sxml_schematron_compilation *compilation) {
    const sxml_schematron_configuration *configuration = compilation->configuration;
    sxml_status status = SXML_STATUS_OK;
    sxml_item document_node = {.representation = 0};
    switch (configuration->document_source) {
    case SXML_SCHEMATRON_DOCUMENT_SOURCE_TEXT: {
        sxml_diagnostic diagnostic;
        memset(&diagnostic, 0, sizeof(diagnostic));
        status = parse_document(compilation,
                                &(document_text){.content = configuration->document_text,
                                                 .encoding_name = no_text,
                                                 .uri = configuration->document_uri,
                                                 .diagnostic = &diagnostic},
                                &document_node);
        if (status != SXML_STATUS_OK) {
            if (configuration->error_record != NULL && diagnostic.description.byte_count > 0) {
                memset(configuration->error_record, 0, sizeof(*configuration->error_record));
                configuration->error_record->diagnostic = diagnostic;
                configuration->error_record->diagnostic.location.source_name =
                    configuration->document_uri;
            }
            return status;
        }
        status = add_document(compilation,
                              (document_addition){.document_node = document_node,
                                                  .uri = configuration->document_uri},
                              &(uint32_t){0});
        sxml_item_release(document_node);
        break;
    }
    case SXML_SCHEMATRON_DOCUMENT_SOURCE_RESOLVED: {
        sxml_uri_resolution resolution = {.uri = no_text, .memory = NULL};
        status = sxml_uri_resolve(&(sxml_uri_resolution_request){.base_uri = configuration->base_uri,
                                                                 .reference = configuration->document_uri,
                                                                 .allocator = compilation->allocator},
                                  &resolution);
        loaded_document loaded = {.is_loaded = false, .document = 0, .reason = NULL};
        if (status == SXML_STATUS_OK) {
            status = load_document(compilation,
                                   &(document_request){.uri_reference = configuration->document_uri,
                                                       .absolute_uri = resolution.uri},
                                   &loaded);
        }
        sxml_allocator_release(compilation->allocator, resolution.memory);
        if (status == SXML_STATUS_OK && !loaded.is_loaded) {
            status = sxml_schematron_report_error(
                compilation,
                &(sxml_schematron_error){.code = "include",
                                         .is_xpath = false,
                                         .description = "the resolver does not deliver the "
                                                        "schematron document",
                                         .subject = configuration->document_uri,
                                         .document = UINT32_MAX,
                                         .source_span = {.line_number = 0}});
            return status == SXML_STATUS_OK ? SXML_STATUS_STATIC_ERROR : status;
        }
        break;
    }
    case SXML_SCHEMATRON_DOCUMENT_SOURCE_NODE: {
        sxml_node_reference node = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0};
        status = sxml_item_get_node(configuration->document_node, &node);
        if (status != SXML_STATUS_OK) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        status = add_document(
            compilation,
            (document_addition){.document_node = sxml_node_item_make((sxml_node_reference){
                                    .tree = node.tree, .node_index = {.index = 0}, .namespace_number = 0}),
                                .uri = configuration->document_uri},
            &(uint32_t){0});
        if (status == SXML_STATUS_OK) {
            /* add_document retained the root it was given; the reference
               made here is released. */
            sxml_item_release(sxml_schematron_document_at(compilation, 0)->document_node);
        }
        sxml_node_description description;
        if (status == SXML_STATUS_OK && sxml_node_describe(node, &description) == SXML_STATUS_OK &&
            description.node_kind == SXML_NODE_KIND_ELEMENT) {
            compilation->index.schema = (sxml_schematron_place){.document = 0, .element = node.node_index};
            return SXML_STATUS_OK;
        }
        break;
    }
    default:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_node_index element = {.index = 0};
    if (!find_document_element(compilation, 0, &element)) {
        element.index = 0;
    }
    compilation->index.schema = (sxml_schematron_place){.document = 0, .element = element};
    return SXML_STATUS_OK;
}

sxml_status sxml_schematron_read(sxml_schematron_compilation *compilation) {
    sxml_status status = intern_names(compilation);
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_parser_create(
            compilation->allocator,
            &(sxml_xml_parser_configuration){.allocator = compilation->allocator,
                                             .name_pool = compilation->name_pool,
                                             .limits = compilation->processor->limits.xml_limits},
            &compilation->parser);
    }
    if (status == SXML_STATUS_OK) {
        status = read_compiled_document(compilation);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_schematron_index *index = &compilation->index;
    const sxml_schematron_element_kind schema_kind =
        sxml_schematron_element_kind_of(compilation, index->schema);
    if (schema_kind != SXML_SCHEMATRON_ELEMENT_SCHEMA) {
        /* 5.4.8: a library is only ever included. */
        return sxml_schematron_report_error(
            compilation,
            &(sxml_schematron_error){.code = "not-schematron",
                                     .is_xpath = false,
                                     .description = schema_kind == SXML_SCHEMATRON_ELEMENT_LIBRARY
                                                        ? "a schematron library is compiled; it can "
                                                          "only be included by extends href (5.4.8)"
                                                        : "the document element is not sch:schema "
                                                          "(5.2, Annex A)",
                                     .subject = no_text,
                                     .document = 0,
                                     .source_span = sxml_schematron_element_span(compilation,
                                                                                 index->schema)});
    }
    /* 6.5: the query binding before anything else. */
    const sxml_schematron_attribute binding = sxml_schematron_find_attribute(
        compilation, (sxml_schematron_attribute_query){.place = index->schema,
                                                       .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_QUERY_BINDING});
    compilation->binding = (sxml_schematron_binding){.version = 4.0, .is_xslt = true, .is_xslt_2 = false};
    if (binding.is_present &&
        !read_query_binding(sxml_xml_text_trim_whitespace(binding.value), &compilation->binding)) {
        status = sxml_schematron_report_error(
            compilation,
            &(sxml_schematron_error){.code = "query-binding",
                                     .is_xpath = false,
                                     .description = "the query binding is not supported; sxml reads "
                                                    "xslt, xslt2, xslt3, xslt4, xpath, xpath2, "
                                                    "xpath3, xpath31 and xpath4 (6.5)",
                                     .subject = sxml_xml_text_trim_whitespace(binding.value),
                                     .document = 0,
                                     .source_span = binding.source_span});
        return status == SXML_STATUS_OK ? SXML_STATUS_STATIC_ERROR : status;
    }
    index->has_title = false;
    schema_reader reader = {.compilation = compilation,
                            .frames = {.elements = NULL, .count = 0, .capacity = 0},
                            .cursor = {.compilation = compilation,
                                       .frames = {.elements = NULL, .count = 0, .capacity = 0},
                                       .expands_rules = false},
                            .identifiers = {.elements = NULL, .count = 0, .capacity = 0},
                            .references = {.elements = NULL, .count = 0, .capacity = 0},
                            .lets = {.elements = NULL, .count = 0, .capacity = 0}};
    status = check_attributes(&reader, index->schema, SXML_SCHEMATRON_ELEMENT_SCHEMA);
    if (status == SXML_STATUS_OK) {
        status = enter_element(&reader, (check_frame){.place = index->schema,
                                                      .element_kind = SXML_SCHEMATRON_ELEMENT_SCHEMA,
                                                      .production = SXML_PRODUCTION_SCHEMA,
                                                      .last_rank = 0,
                                                      .title_count = 0,
                                                      .pattern_count = 0,
                                                      .diagnostics_count = 0,
                                                      .properties_count = 0,
                                                      .required_count = 0,
                                                      .is_in_abstract_pattern = false});
    }
    if (status == SXML_STATUS_OK) {
        text_content content = {.is_whitespace = true};
        status = has_only_whitespace(compilation, index->schema, &content);
        if (status == SXML_STATUS_OK && !content.is_whitespace) {
            status = report_at(compilation,
                               (place_error){.code = "syntax",
                                             .description = "text where the grammar allows none "
                                                            "(Annex A, 5.3)",
                                             .place = index->schema,
                                             .attribute_span = {.line_number = 0},
                                             .subject = no_text});
        }
    }
    if (status == SXML_STATUS_OK) {
        status = walk_schema(&reader);
    }
    if (status == SXML_STATUS_OK) {
        status = check_references(&reader);
    }
    sxml_schematron_cursor_finish(&reader.cursor);
    sxml_schematron_array_release(compilation, &reader.frames);
    sxml_schematron_array_release(compilation, &reader.identifiers);
    sxml_schematron_array_release(compilation, &reader.references);
    sxml_schematron_array_release(compilation, &reader.lets);
    return status;
}

void sxml_schematron_read_release(sxml_schematron_compilation *compilation) {
    for (uint32_t document = 0; document < compilation->documents.count; document += 1) {
        sxml_schematron_document *entry = sxml_schematron_document_at(compilation, document);
        sxml_item_release(entry->document_node);
        sxml_allocator_release(compilation->allocator, (void *)entry->uri.bytes);
    }
    sxml_schematron_array_release(compilation, &compilation->documents);
    sxml_schematron_index *index = &compilation->index;
    sxml_schematron_array *arrays[] = {
        &index->namespaces,       &index->schema_parameters, &index->schema_lets,
        &index->phases,           &index->patterns,          &index->abstract_rules,
        &index->abstract_patterns, &index->diagnostics,      &index->properties,
        &index->pattern_identifiers, &index->foreign_declarations};
    for (size_t array_index = 0; array_index < sizeof(arrays) / sizeof(arrays[0]);
         array_index += 1) {
        sxml_schematron_array_release(compilation, arrays[array_index]);
    }
    sxml_schematron_array_release(compilation, &compilation->reported);
    sxml_xml_parser_destroy(compilation->parser);
    compilation->parser = NULL;
}
