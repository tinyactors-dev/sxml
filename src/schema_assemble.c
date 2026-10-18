/*
 * Compile stage READ, the assembling half: the work list of schema
 * documents (src/schema_read_internal.h, glossary schema_assembly;
 * docs/DECISIONS.md D165, D167, D170).
 *
 * Golden files: src/xquery_module.c (modules found through the resource
 * resolver, each read once, in a work list instead of recursion) and
 * src/xslt_read.c (parse_module: the bytes the resolver delivers parsed
 * into an arena tree with locations).
 *
 * Specification: XSD 1.1 Part 1, 4.2 "Assembling a schema from multiple
 * schema documents": 4.2.3 (xs:include, chameleon inclusion with F.1),
 * 4.2.4 (xs:redefine), 4.2.5 (xs:override), 4.2.6 (xs:import), and 4.3.2
 * (a document element other than xs:schema, "schema_reference").
 *
 * Each document is known by its absolute URI and the target namespace its
 * components get, so a document included twice into one namespace is
 * read once, and a document without a targetNamespace included into two
 * namespaces is read for each (its tree parsed once). Include cycles end
 * there (4.2.3). The children of xs:redefine and xs:override are read
 * after every document, so that what they replace exists; the trees are
 * released when READ returns.
 */
#include "schema_read_internal.h"

#include "evaluator_internal.h"
#include "processor_internal.h"
#include "xdm_internal.h"
#include "xml_internal.h"

#include "sxml/xml.h"

#include <stdalign.h>
#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* One document of the assembly. Allocated on its own, so the reader's
   pointers to it (its defaults) survive the list's growth. */
typedef struct assembly_document {
    sxml_schema_document_index index;
    /* Owned: the document node or schema element the tree is held by. */
    sxml_item root;
    sxml_tree *tree;
    sxml_node_index schema_element;
    /* The absolute URI, owned; may be empty. */
    uint8_t *uri_bytes;
    size_t uri_byte_count;
    /* The namespace of its components, and whether it took it from its
       includer. */
    sxml_namespace_uri_identifier target_namespace;
    bool is_chameleon;
    sxml_schema_document_defaults defaults;
    /* Marks the document while reaches_document walks. */
    bool is_reached;
} assembly_document;

/* A reference from one document to another. */
typedef struct document_edge {
    size_t from_position;
    size_t to_position;
} document_edge;

/* A redefine's or override's children to read once every document is. */
typedef struct pending_read {
    sxml_schema_read_kind read_kind;
    /* The document that has the xs:redefine or xs:override element. */
    size_t document_position;
    sxml_node_index element;
} pending_read;

struct sxml_schema_assembly {
    sxml_schema_compilation *compilation;
    sxml_schema_components *components;
    sxml_processor *processor;
    sxml_allocator allocator;
    sxml_xml_parser *parser;
    /* assembly_document pointers, in the order they were added. */
    assembly_document **documents;
    size_t document_count;
    size_t document_capacity;
    pending_read *pending_reads;
    size_t pending_read_count;
    size_t pending_read_capacity;
    /* The components replaced by redefine and override children. */
    sxml_schema_name_key *redefined_names;
    size_t redefined_name_count;
    size_t redefined_name_capacity;
    /* Which document refers to which (by position): xs:redefine must not
       be part of a cycle (4.2.4). */
    document_edge *edges;
    size_t edge_count;
    size_t edge_capacity;
};

sxml_schema_compilation *
sxml_schema_assembly_get_compilation(const sxml_schema_assembly *assembly) {
    return assembly->compilation;
}

sxml_status sxml_schema_assembly_note_redefinition(sxml_schema_assembly *assembly,
                                                   sxml_schema_name_key key,
                                                   sxml_schema_redefinition_note *note) {
    for (size_t name_index = 0; name_index < assembly->redefined_name_count; name_index += 1) {
        const sxml_schema_name_key noted = assembly->redefined_names[name_index];
        if (noted.symbol_space == key.symbol_space && noted.name.index == key.name.index) {
            note->is_first = false;
            return SXML_STATUS_OK;
        }
    }
    void *elements = assembly->redefined_names;
    sxml_status status = sxml_evaluator_grow(
        assembly->allocator,
        &(sxml_evaluator_growth){.elements = &elements,
                                 .element_capacity = &assembly->redefined_name_capacity,
                                 .element_count = assembly->redefined_name_count,
                                 .required_count = assembly->redefined_name_count + 1U,
                                 .element_byte_count = sizeof(sxml_schema_name_key),
                                 .alignment = alignof(sxml_schema_name_key)});
    assembly->redefined_names = elements;
    if (status == SXML_STATUS_OK) {
        assembly->redefined_names[assembly->redefined_name_count] = key;
        assembly->redefined_name_count += 1;
        note->is_first = true;
    }
    return status;
}

static bool views_equal(sxml_byte_view first, sxml_byte_view second) {
    return first.byte_count == second.byte_count &&
           (first.byte_count == 0 || memcmp(first.bytes, second.bytes, first.byte_count) == 0);
}

static sxml_byte_view uri_of(const assembly_document *document) {
    return (sxml_byte_view){.bytes = document->uri_bytes, .byte_count = document->uri_byte_count};
}

/* ------------------------------------------------------------ Reports */

/* A problem with a document, reported where it is referred to. */
typedef struct document_error {
    const char *constraint_name;
    const char *description;
    /* Shown after the description: a URI or the parser's description. */
    sxml_byte_view subject;
    sxml_schema_position position;
} document_error;

static sxml_status report_document(sxml_schema_assembly *assembly, document_error error) {
    const sxml_byte_view description = sxml_byte_view_from_c_string(error.description);
    sxml_xml_buffer detail = sxml_xml_buffer_empty(assembly->allocator);
    sxml_status status = SXML_STATUS_OK;
    if (error.subject.byte_count != 0) {
        status = sxml_xml_buffer_append(&detail, description);
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(&detail, SXML_TEXT(": "));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(&detail, error.subject);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_compilation_report(
            assembly->compilation,
            &(sxml_schema_report_request){
                .constraint_name = sxml_byte_view_from_c_string(error.constraint_name),
                .description = description,
                .detailed_description = {.bytes = detail.bytes, .byte_count = detail.byte_count},
                .position = error.position});
    }
    sxml_xml_buffer_release(&detail);
    return status;
}

/* The constraint of a document that cannot be read or is not a schema
   document, by how it is referred to. */
typedef enum document_relation {
    SXML_DOCUMENT_RELATION_CONFIGURED = 0,
    SXML_DOCUMENT_RELATION_INCLUDE,
    SXML_DOCUMENT_RELATION_IMPORT,
    SXML_DOCUMENT_RELATION_REDEFINE,
    SXML_DOCUMENT_RELATION_OVERRIDE,
} document_relation;

static const char *const unreadable_constraints[] = {
    [SXML_DOCUMENT_RELATION_CONFIGURED] = "schema_reference",
    [SXML_DOCUMENT_RELATION_INCLUDE] = "src-include.1",
    [SXML_DOCUMENT_RELATION_IMPORT] = "src-import.2",
    [SXML_DOCUMENT_RELATION_REDEFINE] = "src-redefine.1",
    [SXML_DOCUMENT_RELATION_OVERRIDE] = "src-override.1",
};

static document_relation relation_of_reference(sxml_schema_document_relation relation) {
    switch (relation) {
    case SXML_SCHEMA_DOCUMENT_RELATION_INCLUDE:
        return SXML_DOCUMENT_RELATION_INCLUDE;
    case SXML_SCHEMA_DOCUMENT_RELATION_IMPORT:
        return SXML_DOCUMENT_RELATION_IMPORT;
    case SXML_SCHEMA_DOCUMENT_RELATION_REDEFINE:
        return SXML_DOCUMENT_RELATION_REDEFINE;
    case SXML_SCHEMA_DOCUMENT_RELATION_OVERRIDE:
        return SXML_DOCUMENT_RELATION_OVERRIDE;
    }
    return SXML_DOCUMENT_RELATION_INCLUDE;
}

/* ------------------------------------------------------------ Documents */

/* What to make a document of. */
typedef struct document_source {
    document_relation relation;
    /* BYTES to parse, or a node (owned by the caller; retained). */
    bool is_node;
    sxml_byte_view content;
    sxml_byte_view encoding_name;
    sxml_item node;
    /* The absolute URI (document and base URI); may be empty. */
    sxml_byte_view uri;
    /* The configuration's document_uri for a configured document. */
    sxml_byte_view configured_uri;
    /* Where errors about the document are reported: the referring
       element, or (document 0) nowhere in particular. */
    sxml_schema_position position;
    /* The target namespace the document must have: CONFIGURED with
       checks_target_namespace, IMPORT; include, redefine and override
       allow none as well (a chameleon inclusion). */
    bool checks_namespace;
    sxml_namespace_uri_identifier expected_namespace;
    /* The includer's target namespace (include, redefine, override). */
    sxml_namespace_uri_identifier includer_namespace;
} document_source;

/* Parses bytes into an arena tree with locations (src/xslt_read.c,
   parse_module); a document that is not well-formed is reported with the
   parser's description. */
static sxml_status parse_document(sxml_schema_assembly *assembly, const document_source *source,
                                  sxml_item *document) {
    sxml_processor *processor = assembly->processor;
    sxml_arena_tree *arena_tree = NULL;
    sxml_status status = sxml_arena_tree_create(
        assembly->allocator,
        &(sxml_arena_tree_configuration){
            .allocator = assembly->allocator,
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
    sxml_diagnostic diagnostic;
    memset(&diagnostic, 0, sizeof(diagnostic));
    if (status == SXML_STATUS_OK) {
        status =
            sxml_xml_parser_parse(assembly->parser,
                                  &(sxml_xml_parse_request){.input = source->content,
                                                            .source_name = source->uri,
                                                            .document_uri = source->uri,
                                                            .base_uri = source->uri,
                                                            .encoding_name = source->encoding_name,
                                                            .diagnostic = &diagnostic},
                                  sxml_arena_tree_as_tree_builder(arena_tree));
    }
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        if (status != SXML_STATUS_OUT_OF_MEMORY && status != SXML_STATUS_LIMIT_EXCEEDED &&
            status != SXML_STATUS_ARITHMETIC_OVERFLOW) {
            return report_document(
                assembly,
                (document_error){.constraint_name = unreadable_constraints[source->relation],
                                 .description = "A schema document is not well-formed",
                                 .subject = diagnostic.description,
                                 .position = source->position});
        }
        return status;
    }
    *document = sxml_node_item_make(
        (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0});
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

/* The xs:schema element of a document node or element, if it is one. */
typedef struct schema_element_search {
    bool is_found;
    sxml_tree *tree;
    sxml_node_index schema_element;
} schema_element_search;

static sxml_status find_schema_element(const sxml_schema_assembly *assembly, sxml_item root,
                                       schema_element_search *search) {
    *search =
        (schema_element_search){.is_found = false, .tree = NULL, .schema_element = {.index = 0}};
    sxml_node_reference node = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0};
    sxml_status status = sxml_item_get_node(root, &node);
    sxml_node_description description;
    memset(&description, 0, sizeof(description));
    if (status == SXML_STATUS_OK) {
        status = sxml_node_describe(node, &description);
    }
    if (status != SXML_STATUS_OK) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    search->tree = node.tree;
    if (description.node_kind == SXML_NODE_KIND_DOCUMENT) {
        bool has_child = description.has_children;
        sxml_node_index child = description.first_child;
        while (has_child) {
            sxml_node_description child_description;
            memset(&child_description, 0, sizeof(child_description));
            status = sxml_node_describe((sxml_node_reference){.tree = node.tree,
                                                              .node_index = child,
                                                              .namespace_number = 0},
                                        &child_description);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            if (child_description.node_kind == SXML_NODE_KIND_ELEMENT) {
                node.node_index = child;
                description = child_description;
                break;
            }
            has_child = child_description.has_next_sibling;
            child = child_description.next_sibling;
        }
    }
    if (description.node_kind != SXML_NODE_KIND_ELEMENT) {
        return SXML_STATUS_OK;
    }
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    status = sxml_name_pool_get_name_parts(assembly->processor->name_pool, description.node_name,
                                           &parts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    search->is_found = parts.namespace_uri.index == (uint32_t)SXML_NAMESPACE_XML_SCHEMA &&
                       views_equal(parts.local_name, SXML_TEXT("schema"));
    search->schema_element = node.node_index;
    return SXML_STATUS_OK;
}

/* The targetNamespace attribute of the schema element. */
static sxml_status read_target_namespace(const sxml_schema_assembly *assembly,
                                         const schema_element_search *search,
                                         sxml_namespace_uri_identifier *namespace_uri) {
    namespace_uri->index = SXML_NAMESPACE_NONE;
    sxml_node_description description;
    memset(&description, 0, sizeof(description));
    sxml_status status = sxml_node_describe(
        (sxml_node_reference){
            .tree = search->tree, .node_index = search->schema_element, .namespace_number = 0},
        &description);
    for (uint32_t attribute_index = 1;
         status == SXML_STATUS_OK && attribute_index <= description.attribute_count;
         attribute_index += 1) {
        const sxml_node_reference attribute = {
            .tree = search->tree,
            .node_index = {.index = search->schema_element.index + attribute_index},
            .namespace_number = 0};
        sxml_node_description attribute_description;
        memset(&attribute_description, 0, sizeof(attribute_description));
        status = sxml_node_describe(attribute, &attribute_description);
        sxml_name_parts parts = {.namespace_uri = {.index = 0},
                                 .local_name = {.bytes = NULL, .byte_count = 0}};
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_name_parts(assembly->processor->name_pool,
                                                   attribute_description.node_name, &parts);
        }
        if (status == SXML_STATUS_OK && parts.namespace_uri.index == SXML_NAMESPACE_NONE &&
            views_equal(parts.local_name, SXML_TEXT("targetNamespace"))) {
            sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
            status = sxml_node_get_content(attribute, &value);
            if (status == SXML_STATUS_OK) {
                status = sxml_name_pool_intern_namespace_uri(assembly->processor->name_pool, value,
                                                             namespace_uri);
            }
        }
    }
    return status;
}

/* A document already in the assembly with this URI whose components are
   in namespace_uri, if any. */
typedef struct document_match {
    bool has_same_uri;
    bool is_found;
    size_t position;
} document_match;

static document_match find_document(const sxml_schema_assembly *assembly,
                                    const document_source *source) {
    document_match match = {.has_same_uri = false, .is_found = false, .position = 0};
    if (source->uri.byte_count == 0 || source->is_node) {
        return match;
    }
    for (size_t position = 0; position < assembly->document_count; position += 1) {
        const assembly_document *document = assembly->documents[position];
        if (!views_equal(uri_of(document), source->uri)) {
            continue;
        }
        if (!match.has_same_uri) {
            match.has_same_uri = true;
            match.position = position;
        }
        const bool takes_includer_namespace =
            document->is_chameleon && (source->relation == SXML_DOCUMENT_RELATION_INCLUDE ||
                                       source->relation == SXML_DOCUMENT_RELATION_REDEFINE ||
                                       source->relation == SXML_DOCUMENT_RELATION_OVERRIDE);
        if (!takes_includer_namespace ||
            document->target_namespace.index == source->includer_namespace.index) {
            match.is_found = true;
            match.position = position;
            return match;
        }
    }
    return match;
}

static sxml_status grow_documents(sxml_schema_assembly *assembly) {
    void *elements = (void *)assembly->documents;
    sxml_status status = sxml_evaluator_grow(
        assembly->allocator,
        &(sxml_evaluator_growth){.elements = &elements,
                                 .element_capacity = &assembly->document_capacity,
                                 .element_count = assembly->document_count,
                                 .required_count = assembly->document_count + 1U,
                                 .element_byte_count = sizeof(assembly_document *),
                                 .alignment = alignof(assembly_document *)});
    assembly->documents = (assembly_document **)elements;
    return status;
}

/* Checks the document's target namespace against what refers to it
   (Part 1, 4.2.3 clause 2, 4.2.4, 4.2.5, 4.2.6.2 clause 3) and decides a
   chameleon inclusion. */
typedef struct namespace_decision {
    bool is_allowed;
    bool is_chameleon;
    sxml_namespace_uri_identifier effective_namespace;
} namespace_decision;

static namespace_decision decide_namespace(const document_source *source,
                                           sxml_namespace_uri_identifier own_namespace) {
    namespace_decision decision = {
        .is_allowed = true, .is_chameleon = false, .effective_namespace = own_namespace};
    switch (source->relation) {
    case SXML_DOCUMENT_RELATION_CONFIGURED:
    case SXML_DOCUMENT_RELATION_IMPORT:
        decision.is_allowed =
            !source->checks_namespace || own_namespace.index == source->expected_namespace.index;
        break;
    case SXML_DOCUMENT_RELATION_INCLUDE:
    case SXML_DOCUMENT_RELATION_REDEFINE:
    case SXML_DOCUMENT_RELATION_OVERRIDE:
        if (own_namespace.index == SXML_NAMESPACE_NONE &&
            source->includer_namespace.index != SXML_NAMESPACE_NONE) {
            decision.is_chameleon = true;
            decision.effective_namespace = source->includer_namespace;
        } else {
            decision.is_allowed = own_namespace.index == source->includer_namespace.index;
        }
        break;
    }
    return decision;
}

static const char *const namespace_constraints[] = {
    [SXML_DOCUMENT_RELATION_CONFIGURED] = "src-import",
    [SXML_DOCUMENT_RELATION_INCLUDE] = "src-include.2.1",
    [SXML_DOCUMENT_RELATION_IMPORT] = "src-import.3.1",
    [SXML_DOCUMENT_RELATION_REDEFINE] = "src-redefine.3.1",
    [SXML_DOCUMENT_RELATION_OVERRIDE] = "src-override.2",
};

/* Adds the document source describes, unless it was read already, and
   writes its position in the list (or SIZE_MAX when it was not added) to
   *position. */
static sxml_status add_document(sxml_schema_assembly *assembly, const document_source *source,
                                size_t *position) {
    *position = SIZE_MAX;
    const document_match match = find_document(assembly, source);
    if (match.is_found) {
        *position = match.position;
        return SXML_STATUS_OK;
    }
    sxml_item root = {.representation = 0};
    sxml_status status = SXML_STATUS_OK;
    if (match.has_same_uri) {
        /* The same document taken into another namespace: its tree again. */
        root = assembly->documents[match.position]->root;
        sxml_item_retain(root);
    } else if (source->is_node) {
        root = source->node;
        sxml_item_retain(root);
    } else {
        const size_t error_count = assembly->compilation->error_count;
        status = parse_document(assembly, source, &root);
        if (status != SXML_STATUS_OK || assembly->compilation->error_count != error_count) {
            return status;
        }
    }
    schema_element_search search = {
        .is_found = false, .tree = NULL, .schema_element = {.index = 0}};
    status = find_schema_element(assembly, root, &search);
    if (status == SXML_STATUS_OK && !search.is_found) {
        status = report_document(
            assembly, (document_error){.constraint_name = unreadable_constraints[source->relation],
                                       .description = "A document is not a schema document: "
                                                      "its document element is not xs:schema",
                                       .subject = source->uri,
                                       .position = source->position});
        sxml_item_release(root);
        return status;
    }
    sxml_namespace_uri_identifier own_namespace = {.index = SXML_NAMESPACE_NONE};
    if (status == SXML_STATUS_OK) {
        status = read_target_namespace(assembly, &search, &own_namespace);
    }
    const namespace_decision decision = decide_namespace(source, own_namespace);
    if (status == SXML_STATUS_OK && !decision.is_allowed) {
        status = report_document(
            assembly, (document_error){.constraint_name = namespace_constraints[source->relation],
                                       .description = "A schema document has another target "
                                                      "namespace than the one it is referred "
                                                      "to with",
                                       .subject = source->uri,
                                       .position = source->position});
        sxml_item_release(root);
        return status;
    }
    assembly_document *document = NULL;
    if (status == SXML_STATUS_OK) {
        status = grow_documents(assembly);
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            assembly->allocator,
            (sxml_allocation_request){.byte_count = sizeof(assembly_document),
                                      .alignment = alignof(assembly_document)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        sxml_item_release(root);
        return status;
    }
    document = memory;
    memset(document, 0, sizeof(*document));
    document->root = root;
    document->tree = search.tree;
    document->schema_element = search.schema_element;
    document->target_namespace = decision.effective_namespace;
    document->is_chameleon = decision.is_chameleon;
    assembly->documents[assembly->document_count] = document;
    assembly->document_count += 1;
    if (source->uri.byte_count != 0) {
        status = sxml_allocator_allocate(
            assembly->allocator,
            (sxml_allocation_request){.byte_count = source->uri.byte_count, .alignment = 1},
            &memory);
        if (status == SXML_STATUS_OK) {
            memcpy(memory, source->uri.bytes, source->uri.byte_count);
            document->uri_bytes = memory;
            document->uri_byte_count = source->uri.byte_count;
        }
    }
    sxml_schema_document_entry entry = {.configured_uri = source->configured_uri,
                                        .target_namespace = decision.effective_namespace};
    if (status == SXML_STATUS_OK) {
        status =
            sxml_schema_components_add_text(assembly->components, source->uri, &entry.document_uri);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_components_add_entries(
            assembly->components,
            &(sxml_schema_entry_request){
                .table = SXML_SCHEMA_TABLE_DOCUMENT, .entries = &entry, .entry_count = 1},
            &document->index.value);
    }
    if (status == SXML_STATUS_OK) {
        *position = assembly->document_count - 1U;
    }
    return status;
}

/* ---------------------------------------------------------- References */

/* Asks the resolver for a referred document (include/sxml/schema.h:
   kind SCHEMA_DOCUMENT, schemaLocation, its absolute form, the
   namespace) and adds it. */
typedef struct resolution_request {
    document_source *source;
    sxml_byte_view uri_reference;
    sxml_byte_view namespace_text;
    /* The document is not available: a description for the report. */
    bool *is_missing;
    size_t *position;
} resolution_request;

static sxml_status resolve_and_add(sxml_schema_assembly *assembly, resolution_request request) {
    *request.is_missing = false;
    *request.position = SIZE_MAX;
    document_source *source = request.source;
    const document_match match = find_document(assembly, source);
    if (match.is_found) {
        *request.position = match.position;
        return SXML_STATUS_OK;
    }
    const sxml_resource_resolver resolver = assembly->processor->resource_resolver;
    if (resolver.operations == NULL || resolver.operations->resolve == NULL) {
        *request.is_missing = true;
        return SXML_STATUS_OK;
    }
    sxml_resource resource;
    memset(&resource, 0, sizeof(resource));
    sxml_status status = resolver.operations->resolve(
        resolver.state,
        &(sxml_resource_request){.resource_kind = SXML_RESOURCE_KIND_SCHEMA_DOCUMENT,
                                 .uri_reference = request.uri_reference,
                                 .absolute_uri = source->uri,
                                 .processor = assembly->processor,
                                 .encoding_name = {.bytes = NULL, .byte_count = 0},
                                 .is_default_collection = false,
                                 .target_namespace_uri = request.namespace_text,
                                 .package_version = {.bytes = NULL, .byte_count = 0}},
        &resource);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (resource.resource_form == SXML_RESOURCE_FORM_BYTES) {
        source->is_node = false;
        source->content = resource.content;
        source->encoding_name = resource.encoding_name;
        if (resource.resource_uri.byte_count != 0) {
            source->uri = resource.resource_uri;
        }
        status = add_document(assembly, source, request.position);
    } else if (resource.resource_form == SXML_RESOURCE_FORM_DOCUMENT_NODE) {
        source->is_node = true;
        source->node = resource.document_node;
        status = add_document(assembly, source, request.position);
        sxml_item_release(resource.document_node);
    } else if (resource.resource_form == SXML_RESOURCE_FORM_NONE) {
        *request.is_missing = true;
    } else {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (resolver.operations->release != NULL) {
        resolver.operations->release(resolver.state, &resource);
    }
    return status;
}

static sxml_status namespace_text(const sxml_schema_assembly *assembly,
                                  sxml_namespace_uri_identifier namespace_uri,
                                  sxml_byte_view *text) {
    return sxml_name_pool_get_namespace_uri(assembly->processor->name_pool, namespace_uri, text);
}

static sxml_status add_pending_read(sxml_schema_assembly *assembly, pending_read read) {
    void *elements = assembly->pending_reads;
    sxml_status status = sxml_evaluator_grow(
        assembly->allocator,
        &(sxml_evaluator_growth){.elements = &elements,
                                 .element_capacity = &assembly->pending_read_capacity,
                                 .element_count = assembly->pending_read_count,
                                 .required_count = assembly->pending_read_count + 1U,
                                 .element_byte_count = sizeof(pending_read),
                                 .alignment = alignof(pending_read)});
    assembly->pending_reads = elements;
    if (status == SXML_STATUS_OK) {
        assembly->pending_reads[assembly->pending_read_count] = read;
        assembly->pending_read_count += 1;
    }
    return status;
}

/* The position of the document a read request is of. */
static size_t position_of_document(const sxml_schema_assembly *assembly,
                                   sxml_schema_document_index document) {
    for (size_t position = 0; position < assembly->document_count; position += 1) {
        if (assembly->documents[position]->index.value == document.value) {
            return position;
        }
    }
    return SIZE_MAX;
}

static sxml_status add_edge(sxml_schema_assembly *assembly, document_edge edge) {
    void *elements = assembly->edges;
    sxml_status status = sxml_evaluator_grow(
        assembly->allocator, &(sxml_evaluator_growth){.elements = &elements,
                                                      .element_capacity = &assembly->edge_capacity,
                                                      .element_count = assembly->edge_count,
                                                      .required_count = assembly->edge_count + 1U,
                                                      .element_byte_count = sizeof(document_edge),
                                                      .alignment = alignof(document_edge)});
    assembly->edges = elements;
    if (status == SXML_STATUS_OK) {
        assembly->edges[assembly->edge_count] = edge;
        assembly->edge_count += 1;
    }
    return status;
}

/* True when path.to_position is path.from_position or is reached from it
   through the references met so far: each round marks the targets of the
   edges from marked documents, until nothing changes. */
static bool reaches_document(const sxml_schema_assembly *assembly, document_edge path) {
    if (path.to_position == SIZE_MAX || path.from_position == SIZE_MAX) {
        return false;
    }
    if (path.from_position == path.to_position) {
        return true;
    }
    assembly->documents[path.from_position]->is_reached = true;
    bool has_changed = true;
    while (has_changed) {
        has_changed = false;
        for (size_t edge_index = 0; edge_index < assembly->edge_count; edge_index += 1) {
            const document_edge edge = assembly->edges[edge_index];
            if (assembly->documents[edge.from_position]->is_reached &&
                !assembly->documents[edge.to_position]->is_reached) {
                assembly->documents[edge.to_position]->is_reached = true;
                has_changed = true;
            }
        }
    }
    const bool is_reached = assembly->documents[path.to_position]->is_reached;
    for (size_t position = 0; position < assembly->document_count; position += 1) {
        assembly->documents[position]->is_reached = false;
    }
    return is_reached;
}

sxml_status sxml_schema_assembly_add_reference(sxml_schema_assembly *assembly,
                                               const sxml_schema_document_reference *reference) {
    const sxml_schema_read_request *referrer = reference->referrer;
    const document_relation relation = relation_of_reference(reference->relation);
    const bool is_import = reference->relation == SXML_SCHEMA_DOCUMENT_RELATION_IMPORT;
    if (is_import && reference->schema_location.byte_count == 0) {
        /* An import without a location: its namespace may be known
           already (4.2.6.2); else the resolver may know it. */
        for (size_t position = 0; position < assembly->document_count; position += 1) {
            if (assembly->documents[position]->target_namespace.index ==
                reference->imported_namespace.index) {
                return SXML_STATUS_OK;
            }
        }
    }
    if (is_import && reference->imported_namespace.index == (uint32_t)SXML_NAMESPACE_XML_SCHEMA) {
        /* The built-in components are every schema's (3.16.7). */
        return SXML_STATUS_OK;
    }
    sxml_uri_resolution resolution = {.uri = {.bytes = NULL, .byte_count = 0}, .memory = NULL};
    sxml_status status = SXML_STATUS_OK;
    if (reference->schema_location.byte_count != 0) {
        status =
            sxml_uri_resolve(&(sxml_uri_resolution_request){.base_uri = reference->base_uri,
                                                            .reference = reference->schema_location,
                                                            .allocator = assembly->allocator},
                             &resolution);
    }
    sxml_byte_view namespace_value = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = namespace_text(
            assembly, is_import ? reference->imported_namespace : referrer->target_namespace,
            &namespace_value);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    document_source source = {.relation = relation,
                              .is_node = false,
                              .uri = resolution.uri,
                              .configured_uri = {.bytes = NULL, .byte_count = 0},
                              .position = reference->position,
                              .checks_namespace = is_import,
                              .expected_namespace = reference->imported_namespace,
                              .includer_namespace = referrer->target_namespace};
    bool is_missing = false;
    size_t position = SIZE_MAX;
    status =
        resolve_and_add(assembly, (resolution_request){.source = &source,
                                                       .uri_reference = reference->schema_location,
                                                       .namespace_text = namespace_value,
                                                       .is_missing = &is_missing,
                                                       .position = &position});
    if (status == SXML_STATUS_OK && is_missing && !is_import) {
        /* include/sxml/schema.h (D165): a document an include, redefine or
           override names must be delivered. */
        status = report_document(
            assembly, (document_error){.constraint_name = unreadable_constraints[relation],
                                       .description = "The schema document a reference names "
                                                      "cannot be read",
                                       .subject = reference->schema_location,
                                       .position = reference->position});
    }
    sxml_allocator_release(assembly->allocator, resolution.memory);
    const size_t referrer_position = position_of_document(assembly, referrer->document);
    if (status == SXML_STATUS_OK && position != SIZE_MAX && referrer_position != SIZE_MAX) {
        status = add_edge(
            assembly, (document_edge){.from_position = referrer_position, .to_position = position});
    }
    if (status == SXML_STATUS_OK && position != SIZE_MAX &&
        reference->relation == SXML_SCHEMA_DOCUMENT_RELATION_REDEFINE &&
        reaches_document(assembly, (document_edge){.from_position = position,
                                                   .to_position = referrer_position})) {
        /* Part 1, 4.2.4: the redefined document, with what it includes,
           is part of the redefining one's schema, so it cannot refer back
           to it. */
        status = report_document(
            assembly, (document_error){.constraint_name = "src-redefine",
                                       .description = "A redefined schema document refers "
                                                      "back to the redefining one",
                                       .subject = reference->schema_location,
                                       .position = reference->position});
        position = SIZE_MAX;
    }
    if (status == SXML_STATUS_OK && position != SIZE_MAX &&
        (reference->relation == SXML_SCHEMA_DOCUMENT_RELATION_REDEFINE ||
         reference->relation == SXML_SCHEMA_DOCUMENT_RELATION_OVERRIDE)) {
        status = add_pending_read(
            assembly, (pending_read){
                          .read_kind = reference->relation == SXML_SCHEMA_DOCUMENT_RELATION_REDEFINE
                                           ? SXML_SCHEMA_READ_KIND_REDEFINE
                                           : SXML_SCHEMA_READ_KIND_OVERRIDE,
                          .document_position = position_of_document(assembly, referrer->document),
                          .element = reference->element});
    }
    return status;
}

/* ------------------------------------------------------------ The stage */

/* The configured documents (include/sxml/schema.h). */
static sxml_status add_configured_document(sxml_schema_assembly *assembly,
                                           const sxml_schema_document *configured) {
    sxml_status status = SXML_STATUS_OK;
    sxml_namespace_uri_identifier expected = {.index = SXML_NAMESPACE_NONE};
    if (configured->checks_target_namespace) {
        status = sxml_name_pool_intern_namespace_uri(assembly->processor->name_pool,
                                                     configured->target_namespace_uri, &expected);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    document_source source = {.relation = SXML_DOCUMENT_RELATION_CONFIGURED,
                              .is_node = false,
                              .content = configured->document_text,
                              .uri = configured->document_uri,
                              .configured_uri = configured->document_uri,
                              .position = {.document = {.value = 0}},
                              .checks_namespace = configured->checks_target_namespace,
                              .expected_namespace = expected,
                              .includer_namespace = {.index = SXML_NAMESPACE_NONE}};
    size_t position = SIZE_MAX;
    switch (configured->document_source) {
    case SXML_SCHEMA_DOCUMENT_SOURCE_TEXT:
        return add_document(assembly, &source, &position);
    case SXML_SCHEMA_DOCUMENT_SOURCE_NODE:
        source.is_node = true;
        source.node = configured->document_node;
        return add_document(assembly, &source, &position);
    case SXML_SCHEMA_DOCUMENT_SOURCE_RESOLVED: {
        const sxml_byte_view base_uri = assembly->compilation->configuration->base_uri;
        sxml_uri_resolution resolution = {.uri = {.bytes = NULL, .byte_count = 0}, .memory = NULL};
        if (configured->document_uri.byte_count != 0) {
            status = sxml_uri_resolve(
                &(sxml_uri_resolution_request){.base_uri = base_uri,
                                               .reference = configured->document_uri,
                                               .allocator = assembly->allocator},
                &resolution);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        source.uri = resolution.uri;
        bool is_missing = false;
        status = resolve_and_add(
            assembly, (resolution_request){.source = &source,
                                           .uri_reference = configured->document_uri,
                                           .namespace_text = configured->target_namespace_uri,
                                           .is_missing = &is_missing,
                                           .position = &position});
        if (status == SXML_STATUS_OK && is_missing) {
            /* include/sxml/schema.h: src-import, "the schema document
               cannot be read". */
            status = report_document(
                assembly, (document_error){.constraint_name = "src-import",
                                           .description = "The schema document cannot be read",
                                           .subject = configured->document_uri,
                                           .position = {.document = {.value = 0}}});
        }
        sxml_allocator_release(assembly->allocator, resolution.memory);
        return status;
    }
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

static sxml_status read_document(sxml_schema_assembly *assembly, size_t position,
                                 const pending_read *pending) {
    assembly_document *document = assembly->documents[position];
    const sxml_schema_read_request request = {
        .read_kind = pending == NULL ? SXML_SCHEMA_READ_KIND_DOCUMENT : pending->read_kind,
        .document = document->index,
        .tree = document->tree,
        .schema_element = document->schema_element,
        .start_element = pending == NULL ? document->schema_element : pending->element,
        .target_namespace = document->target_namespace,
        .is_chameleon = document->is_chameleon,
        .document_uri = uri_of(document),
        .defaults = &document->defaults};
    return sxml_schema_read_document(assembly, &request);
}

static void release_assembly(sxml_schema_assembly *assembly) {
    for (size_t position = 0; position < assembly->document_count; position += 1) {
        assembly_document *document = assembly->documents[position];
        sxml_item_release(document->root);
        sxml_allocator_release(assembly->allocator, document->uri_bytes);
        sxml_allocator_release(assembly->allocator, document);
    }
    sxml_allocator_release(assembly->allocator, (void *)assembly->documents);
    sxml_allocator_release(assembly->allocator, assembly->pending_reads);
    sxml_allocator_release(assembly->allocator, assembly->redefined_names);
    sxml_allocator_release(assembly->allocator, assembly->edges);
    sxml_xml_parser_destroy(assembly->parser);
}

sxml_status sxml_schema_read_documents(sxml_schema_compilation *compilation) {
    sxml_processor *processor = compilation->configuration->processor;
    sxml_schema_assembly assembly;
    memset(&assembly, 0, sizeof(assembly));
    assembly.compilation = compilation;
    assembly.components = compilation->components;
    assembly.processor = processor;
    assembly.allocator = processor->allocator;
    sxml_status status = sxml_xml_parser_create(
        assembly.allocator,
        &(sxml_xml_parser_configuration){.allocator = assembly.allocator,
                                         .name_pool = processor->name_pool,
                                         .limits = processor->limits.xml_limits},
        &assembly.parser);
    const sxml_schema_document_view configured = compilation->configuration->schema_documents;
    for (size_t document_index = 0;
         status == SXML_STATUS_OK && document_index < configured.schema_document_count;
         document_index += 1) {
        status = add_configured_document(&assembly, &configured.schema_documents[document_index]);
    }
    /* The work list: reading a document may add documents. */
    for (size_t position = 0; status == SXML_STATUS_OK && position < assembly.document_count;
         position += 1) {
        status = read_document(&assembly, position, NULL);
    }
    for (size_t read_index = 0;
         status == SXML_STATUS_OK && read_index < assembly.pending_read_count; read_index += 1) {
        const pending_read pending = assembly.pending_reads[read_index];
        if (pending.document_position != SIZE_MAX) {
            status = read_document(&assembly, pending.document_position, &pending);
        }
    }
    release_assembly(&assembly);
    return status;
}
