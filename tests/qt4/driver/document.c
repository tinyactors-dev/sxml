/*
 * Catalog and test-set files as trees (document.h). Imitates
 * apps/sxml/command_check.c for reading and parsing a file, and the arena
 * tree use of tests/xml_test.c.
 */
#include "document.h"

#include "suite_path.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_qt4_empty_text = {.bytes = NULL,
                                                   .byte_count = 0};

static void sxml_qt4_set_problem(sxml_qt4_problem *problem,
                                 const sxml_qt4_problem *value) {
    if (problem != NULL) {
        *problem = *value;
    }
}

sxml_status sxml_qt4_document_load(const sxml_qt4_document_reader *reader,
                                   const sxml_qt4_document_request *request,
                                   sxml_qt4_document *document) {
    sxml_byte_view contents = sxml_qt4_empty_text;
    sxml_status status = sxml_cli_file_reader_read(
        reader->file_reader,
        &(sxml_cli_file_read_request){.path = request->file_path,
                                      .allocator = reader->allocator},
        &contents);
    if (status != SXML_STATUS_OK) {
        sxml_qt4_set_problem(
            request->problem,
            &(sxml_qt4_problem){.description = SXML_TEXT("cannot read"),
                                .subject = request->file_path,
                                .location = {.source_name = sxml_qt4_empty_text,
                                             .line_number = 0,
                                             .column_number = 0,
                                             .byte_offset = 0}});
        return status;
    }
    sxml_arena_tree *arena_tree = NULL;
    status = sxml_arena_tree_create(
        reader->allocator,
        &(sxml_arena_tree_configuration){
            .allocator = reader->allocator,
            .initial_memory = {.bytes = NULL, .byte_count = 0},
            .item_heap = reader->item_heap,
            .maximum_node_count = 0,
            .maximum_depth = 0,
            .maximum_text_byte_count = 0,
            .keeps_locations = false,
            .duplicate_attribute_policy =
                SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
            .is_destroyed_when_unreferenced = false,
            .error_record = NULL},
        &arena_tree);
    sxml_diagnostic diagnostic = {
        .description = sxml_qt4_empty_text,
        .location = {.source_name = request->suite_path,
                     .line_number = 0,
                     .column_number = 0,
                     .byte_offset = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_parser_parse(
            reader->xml_parser,
            &(sxml_xml_parse_request){.input = contents,
                                      .source_name = request->suite_path,
                                      .document_uri = request->suite_path,
                                      .base_uri = request->suite_path,
                                      .encoding_name = sxml_qt4_empty_text,
                                      .diagnostic = &diagnostic},
            sxml_arena_tree_as_tree_builder(arena_tree));
        if (status != SXML_STATUS_OK) {
            if (diagnostic.description.byte_count == 0) {
                diagnostic.description = sxml_status_name(status);
            }
            sxml_qt4_set_problem(
                request->problem,
                &(sxml_qt4_problem){.description = diagnostic.description,
                                    .subject = sxml_qt4_empty_text,
                                    .location = diagnostic.location});
        }
    }
    sxml_allocator_release(reader->allocator, (void *)contents.bytes);
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    sxml_tree_description description = {.node_count = 0,
                                         .document_uri = sxml_qt4_empty_text,
                                         .base_uri = sxml_qt4_empty_text,
                                         .name_pool = NULL};
    if (status == SXML_STATUS_OK) {
        status = sxml_tree_describe(tree, &description);
    }
    sxml_namespace_uri_identifier catalog_namespace = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_uri(
            description.name_pool, SXML_TEXT(SXML_QT4_CATALOG_NAMESPACE_URI),
            &catalog_namespace);
    }
    if (status != SXML_STATUS_OK) {
        sxml_tree_release(tree);
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *document = (sxml_qt4_document){.arena_tree = arena_tree,
                                    .tree = tree,
                                    .name_pool = description.name_pool,
                                    .catalog_namespace = catalog_namespace};
    return SXML_STATUS_OK;
}

void sxml_qt4_document_unload(sxml_qt4_document *document) {
    if (document == NULL || document->tree == NULL) {
        return;
    }
    sxml_tree_release(document->tree);
    sxml_arena_tree_destroy(document->arena_tree);
    document->tree = NULL;
    document->arena_tree = NULL;
}

sxml_node_reference
sxml_qt4_document_reference(const sxml_qt4_document *document,
                            sxml_node_index node_index) {
    return (sxml_node_reference){.tree = document->tree,
                                 .node_index = node_index,
                                 .namespace_number = 0};
}

static sxml_node_description sxml_qt4_empty_description(void) {
    const sxml_node_index none = {.index = 0};
    return (sxml_node_description){.node_kind = SXML_NODE_KIND_DOCUMENT,
                                   .node_name = {.index = 0},
                                   .namespace_prefix = {.index = 0},
                                   .has_parent = false,
                                   .parent = none,
                                   .last_descendant = none,
                                   .attribute_count = 0,
                                   .has_children = false,
                                   .first_child = none,
                                   .last_child = none,
                                   .has_previous_sibling = false,
                                   .previous_sibling = none,
                                   .has_next_sibling = false,
                                   .next_sibling = none,
                                   .type_annotation = {.index = SXML_TYPE_NONE},
                                   .is_id = false,
                                   .is_idrefs = false,
                                   .is_nilled = false};
}

/* Describes node_index; for an element in the catalog namespace, also
   writes its local name (otherwise an empty one). */
typedef struct sxml_qt4_named_description {
    sxml_node_description description;
    bool is_catalog_element;
    sxml_byte_view local_name;
} sxml_qt4_named_description;

static sxml_status sxml_qt4_describe(const sxml_qt4_document *document,
                                     sxml_node_index node_index,
                                     sxml_qt4_named_description *named) {
    named->description = sxml_qt4_empty_description();
    named->is_catalog_element = false;
    named->local_name = sxml_qt4_empty_text;
    sxml_status status = sxml_node_describe(
        sxml_qt4_document_reference(document, node_index), &named->description);
    if (status != SXML_STATUS_OK ||
        named->description.node_kind != SXML_NODE_KIND_ELEMENT) {
        return status;
    }
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = sxml_qt4_empty_text};
    status = sxml_name_pool_get_name_parts(
        document->name_pool, named->description.node_name, &parts);
    if (status == SXML_STATUS_OK &&
        parts.namespace_uri.index == document->catalog_namespace.index) {
        named->is_catalog_element = true;
        named->local_name = parts.local_name;
    }
    return status;
}

sxml_status sxml_qt4_element_cursor_start(const sxml_qt4_document *document,
                                          sxml_node_index parent,
                                          sxml_qt4_element_cursor *cursor) {
    sxml_node_description description = sxml_qt4_empty_description();
    const sxml_status status = sxml_node_describe(
        sxml_qt4_document_reference(document, parent), &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *cursor =
        (sxml_qt4_element_cursor){.document = document,
                                  .has_next_node = description.has_children,
                                  .next_node = description.first_child};
    return SXML_STATUS_OK;
}

sxml_status sxml_qt4_element_cursor_next(sxml_qt4_element_cursor *cursor,
                                         sxml_qt4_element_step *step) {
    while (cursor->has_next_node) {
        sxml_qt4_named_description named = {.description =
                                                sxml_qt4_empty_description(),
                                            .is_catalog_element = false,
                                            .local_name = sxml_qt4_empty_text};
        const sxml_node_index node_index = cursor->next_node;
        const sxml_status status =
            sxml_qt4_describe(cursor->document, node_index, &named);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        cursor->has_next_node = named.description.has_next_sibling;
        cursor->next_node = named.description.next_sibling;
        if (named.is_catalog_element) {
            *step = (sxml_qt4_element_step){.has_element = true,
                                            .element = node_index,
                                            .local_name = named.local_name};
            return SXML_STATUS_OK;
        }
    }
    *step = (sxml_qt4_element_step){.has_element = false,
                                    .element = {.index = 0},
                                    .local_name = sxml_qt4_empty_text};
    return SXML_STATUS_OK;
}

sxml_status sxml_qt4_document_find_attribute(const sxml_qt4_document *document,
                                             sxml_qt4_attribute_lookup lookup,
                                             sxml_qt4_attribute_value *value) {
    sxml_node_description description = sxml_qt4_empty_description();
    sxml_status status = sxml_node_describe(
        sxml_qt4_document_reference(document, lookup.element), &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* An element's attributes are the nodes right after it (tree.h). */
    for (uint32_t attribute_number = 1;
         attribute_number <= description.attribute_count;
         attribute_number += 1) {
        const sxml_node_index attribute = {.index = lookup.element.index +
                                                    attribute_number};
        sxml_node_description attribute_description =
            sxml_qt4_empty_description();
        status =
            sxml_node_describe(sxml_qt4_document_reference(document, attribute),
                               &attribute_description);
        sxml_name_parts parts = {.namespace_uri = {.index = 0},
                                 .local_name = sxml_qt4_empty_text};
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_name_parts(
                document->name_pool, attribute_description.node_name, &parts);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (parts.namespace_uri.index != SXML_NAMESPACE_NONE ||
            !sxml_byte_view_equals(parts.local_name, lookup.local_name)) {
            continue;
        }
        sxml_byte_view content = sxml_qt4_empty_text;
        status = sxml_node_get_content(
            sxml_qt4_document_reference(document, attribute), &content);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        *value =
            (sxml_qt4_attribute_value){.is_present = true, .value = content};
        return SXML_STATUS_OK;
    }
    *value = (sxml_qt4_attribute_value){.is_present = false,
                                        .value = sxml_qt4_empty_text};
    return SXML_STATUS_OK;
}

sxml_status sxml_qt4_document_get_text(const sxml_qt4_document *document,
                                       const sxml_qt4_text_request *request,
                                       sxml_byte_view *text) {
    sxml_node_description description = sxml_qt4_empty_description();
    sxml_status status = sxml_node_describe(
        sxml_qt4_document_reference(document, request->element), &description);
    sxml_byte_view joined = sxml_qt4_empty_text;
    bool has_child = description.has_children;
    sxml_node_index child = description.first_child;
    while (status == SXML_STATUS_OK && has_child) {
        sxml_node_description child_description = sxml_qt4_empty_description();
        status = sxml_node_describe(
            sxml_qt4_document_reference(document, child), &child_description);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (child_description.node_kind == SXML_NODE_KIND_TEXT) {
            sxml_byte_view content = sxml_qt4_empty_text;
            status = sxml_node_get_content(
                sxml_qt4_document_reference(document, child), &content);
            if (status == SXML_STATUS_OK) {
                if (joined.byte_count == 0) {
                    joined = content;
                } else {
                    status = sxml_qt4_concatenate(
                        request->arena,
                        (sxml_qt4_concatenation){.first = joined,
                                                 .second = content},
                        &joined);
                }
            }
        }
        has_child = child_description.has_next_sibling;
        child = child_description.next_sibling;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *text = joined;
    return SXML_STATUS_OK;
}
