/*
 * The tree wrapper, node accessors, document order and the deep copy of a
 * node into a tree builder (include/sxml/tree.h, src/tree_internal.h;
 * docs/DECISIONS.md D39, D40 and D67).
 *
 * Golden file: src/memory.c (lifecycle, checked sizes, comments).
 *
 * Specification: XDM 4.0, section 6 (nodes, identity, document order) and
 * section 7.5.4 (namespace nodes); XQuery 4.0, "Copy-Namespaces
 * Declaration", through sxml_copy_request.
 */
#include "tree_internal.h"

#include "sxml/xdm.h"

#include <stdalign.h>

/* ------------------------------------------------------------ Lifecycle */

static bool sxml_tree_operations_are_valid(
    const sxml_tree_operations *operations) {
    return operations != NULL && operations->describe_tree != NULL &&
           operations->describe_node != NULL &&
           operations->get_node_content != NULL &&
           operations->get_in_scope_namespaces != NULL;
}

static bool sxml_tree_configuration_is_valid(
    const sxml_tree_configuration *configuration) {
    bool owner_is_valid = configuration->owner.operations == NULL ||
                          configuration->owner.operations->release != NULL;
    return sxml_tree_operations_are_valid(configuration->operations) &&
           configuration->item_heap != NULL && owner_is_valid;
}

sxml_status sxml_tree_memory_requirements(
    const sxml_tree_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (!sxml_tree_configuration_is_valid(configuration)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){
        .byte_count = sizeof(struct sxml_tree),
        .alignment = alignof(struct sxml_tree),
    };
    return SXML_STATUS_OK;
}

sxml_status sxml_tree_initialize(sxml_memory_view memory,
                                 const sxml_tree_configuration *configuration,
                                 sxml_tree **tree) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_tree_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_memory_view_check_requirements(memory, requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_tree_description description = {
        .node_count = 0,
        .document_uri = {.bytes = NULL, .byte_count = 0},
        .base_uri = {.bytes = NULL, .byte_count = 0},
        .name_pool = NULL,
    };
    if (configuration->operations->describe_tree(configuration->state,
                                                 &description) !=
        SXML_STATUS_OK) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* Every tree has a root (XDM 4.0, section 6), and identifiers from two
       pools would mean nothing to each other (D31). */
    if (description.node_count == 0 || description.node_count == UINT32_MAX ||
        description.name_pool !=
            sxml_item_heap_get_name_pool(configuration->item_heap)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    void *placement = memory.bytes;
    sxml_tree *new_tree = placement;
    *new_tree = (sxml_tree){
        .state = configuration->state,
        .operations = configuration->operations,
        .item_heap = configuration->item_heap,
        .owner = configuration->owner,
        /* Issued last, once nothing can fail, so a failed initialize uses
           no identifier. */
        .identifier =
            sxml_item_heap_issue_tree_identifier(configuration->item_heap),
        .node_count = description.node_count,
        .name_pool = description.name_pool,
        .reference_count = 1,
        .owning_allocator = {.state = NULL, .operations = NULL},
    };
    *tree = new_tree;
    return SXML_STATUS_OK;
}

void sxml_tree_finalize(sxml_tree *tree) {
    /* The wrapper owns nothing: the implementation's data belongs to
       whoever configured it. */
    (void)tree;
}

sxml_status sxml_tree_create(sxml_allocator allocator,
                             const sxml_tree_configuration *configuration,
                             sxml_tree **tree) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_tree_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(allocator,
                                     (sxml_allocation_request){
                                         .byte_count = requirements.byte_count,
                                         .alignment = requirements.alignment,
                                     },
                                     &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_tree *new_tree = NULL;
    status = sxml_tree_initialize(
        (sxml_memory_view){
            .bytes = memory,
            .byte_count = requirements.byte_count,
        },
        configuration, &new_tree);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_tree->owning_allocator = allocator;
    *tree = new_tree;
    return SXML_STATUS_OK;
}

void sxml_tree_destroy(sxml_tree *tree) {
    if (tree == NULL) {
        return;
    }
    /* Finalizing may not touch the tree afterwards, so keep the allocator. */
    sxml_allocator owning_allocator = tree->owning_allocator;
    sxml_tree_finalize(tree);
    sxml_allocator_release(owning_allocator, tree);
}

void sxml_tree_retain(sxml_tree *tree) {
    if (tree == NULL ||
        tree->reference_count == SXML_TREE_IMMORTAL_REFERENCE_COUNT) {
        return;
    }
    /* Reaching the immortal count makes the tree immortal (tree.h). */
    tree->reference_count += 1;
}

void sxml_tree_release(sxml_tree *tree) {
    if (tree == NULL ||
        tree->reference_count == SXML_TREE_IMMORTAL_REFERENCE_COUNT ||
        tree->reference_count == 0) {
        return;
    }
    tree->reference_count -= 1;
    if (tree->reference_count == 0 && tree->owner.operations != NULL) {
        /* Last use of tree here: the owner may destroy it. */
        tree->owner.operations->release(tree->owner.state, tree);
    }
}

sxml_tree_identifier sxml_tree_get_identifier(const sxml_tree *tree) {
    return tree->identifier;
}

sxml_status sxml_tree_describe(const sxml_tree *tree,
                               sxml_tree_description *description) {
    sxml_tree_description new_description = {
        .node_count = 0,
        .document_uri = {.bytes = NULL, .byte_count = 0},
        .base_uri = {.bytes = NULL, .byte_count = 0},
        .name_pool = NULL,
    };
    sxml_status status =
        tree->operations->describe_tree(tree->state, &new_description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *description = new_description;
    return SXML_STATUS_OK;
}

/* -------------------------------------------------------- Node accessors */

static bool sxml_node_index_is_in_tree(sxml_node_reference node) {
    return node.node_index.index < node.tree->node_count;
}

/* An empty description, to start from. */
static sxml_node_description sxml_node_description_empty(void) {
    return (sxml_node_description){
        .node_kind = SXML_NODE_KIND_DOCUMENT,
        .node_name = {.index = 0},
        .namespace_prefix = {.index = 0},
        .has_parent = false,
        .parent = {.index = 0},
        .last_descendant = {.index = 0},
        .attribute_count = 0,
        .has_children = false,
        .first_child = {.index = 0},
        .last_child = {.index = 0},
        .has_previous_sibling = false,
        .previous_sibling = {.index = 0},
        .has_next_sibling = false,
        .next_sibling = {.index = 0},
        .type_annotation = {.index = SXML_TYPE_NONE},
        .is_id = false,
        .is_idrefs = false,
        .is_nilled = false,
    };
}

/* Describes a stored node (namespace number ignored). */
static sxml_status sxml_node_describe_stored(
    sxml_node_reference node, sxml_node_description *description) {
    if (!sxml_node_index_is_in_tree(node)) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    sxml_node_description new_description = sxml_node_description_empty();
    sxml_status status = node.tree->operations->describe_node(
        node.tree->state, node.node_index, &new_description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *description = new_description;
    return SXML_STATUS_OK;
}

/*
 * The binding a namespace node stands for: the namespace_number-th
 * in-scope namespace of its element. INDEX_OUT_OF_RANGE when the element
 * is not an element or has fewer in-scope namespaces.
 */
static sxml_status sxml_namespace_node_get_binding(
    sxml_node_reference node, sxml_namespace_binding *binding) {
    sxml_node_description element = sxml_node_description_empty();
    sxml_status status = sxml_node_describe_stored(node, &element);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (element.node_kind != SXML_NODE_KIND_ELEMENT) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    sxml_namespace_binding_view bindings = {.namespace_bindings = NULL,
                                            .namespace_binding_count = 0};
    status = node.tree->operations->get_in_scope_namespaces(
        node.tree->state, node.node_index, &bindings);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (node.namespace_number > bindings.namespace_binding_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    *binding = bindings.namespace_bindings[node.namespace_number - 1];
    return SXML_STATUS_OK;
}

/*
 * The node name of a namespace node: its prefix as a name in no namespace,
 * 0 for the default namespace (XDM 4.0, section 7.5.4.2, dm:node-name).
 * Finds the name first so that describing does not grow the pool when the
 * name is known, as it usually is.
 */
static sxml_status sxml_namespace_node_make_name(
    sxml_name_pool *name_pool, sxml_namespace_prefix_identifier prefix,
    sxml_name_identifier *node_name) {
    sxml_byte_view prefix_text = {.bytes = NULL, .byte_count = 0};
    sxml_status status =
        sxml_name_pool_get_namespace_prefix(name_pool, prefix, &prefix_text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (prefix_text.byte_count == 0) {
        *node_name = (sxml_name_identifier){.index = 0};
        return SXML_STATUS_OK;
    }
    const sxml_name_request request = {
        .namespace_uri = sxml_well_known_namespace_identifier(
            SXML_NAMESPACE_NONE),
        .local_name = prefix_text,
    };
    if (sxml_name_pool_find_name(name_pool, &request, node_name)) {
        return SXML_STATUS_OK;
    }
    return sxml_name_pool_intern_name(name_pool, &request, node_name);
}

sxml_status sxml_node_describe(sxml_node_reference node,
                               sxml_node_description *description) {
    if (node.namespace_number == 0) {
        return sxml_node_describe_stored(node, description);
    }
    sxml_namespace_binding binding = {.namespace_prefix = {.index = 0},
                                      .namespace_uri = {.index = 0}};
    sxml_status status = sxml_namespace_node_get_binding(node, &binding);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_node_description new_description = sxml_node_description_empty();
    status = sxml_namespace_node_make_name(
        node.tree->name_pool, binding.namespace_prefix,
        &new_description.node_name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* XDM 4.0, section 7.5.4: a namespace node's parent is its element; it
       has no siblings, children or attributes. Its subtree is itself, and
       its node_index is the element's, so last_descendant is that too. */
    new_description.node_kind = SXML_NODE_KIND_NAMESPACE;
    new_description.has_parent = true;
    new_description.parent = node.node_index;
    new_description.last_descendant = node.node_index;
    *description = new_description;
    return SXML_STATUS_OK;
}

sxml_status sxml_node_get_content(sxml_node_reference node,
                                  sxml_byte_view *content) {
    if (node.namespace_number == 0) {
        if (!sxml_node_index_is_in_tree(node)) {
            return SXML_STATUS_INDEX_OUT_OF_RANGE;
        }
        sxml_byte_view new_content = {.bytes = NULL, .byte_count = 0};
        sxml_status status = node.tree->operations->get_node_content(
            node.tree->state, node.node_index, &new_content);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        *content = new_content;
        return SXML_STATUS_OK;
    }
    /* The string value of a namespace node is its URI (XDM 4.0, section
       7.5.4.2, dm:string-value). */
    sxml_namespace_binding binding = {.namespace_prefix = {.index = 0},
                                      .namespace_uri = {.index = 0}};
    sxml_status status = sxml_namespace_node_get_binding(node, &binding);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_name_pool_get_namespace_uri(node.tree->name_pool,
                                            binding.namespace_uri, content);
}

sxml_status sxml_node_get_in_scope_namespaces(
    sxml_node_reference element,
    sxml_namespace_binding_view *namespace_bindings) {
    if (element.namespace_number != 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (!sxml_node_index_is_in_tree(element)) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    sxml_namespace_binding_view bindings = {.namespace_bindings = NULL,
                                            .namespace_binding_count = 0};
    sxml_status status = element.tree->operations->get_in_scope_namespaces(
        element.tree->state, element.node_index, &bindings);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *namespace_bindings = bindings;
    return SXML_STATUS_OK;
}

bool sxml_node_is_same_node(sxml_node_reference first,
                            sxml_node_reference second) {
    return first.tree == second.tree &&
           first.node_index.index == second.node_index.index &&
           first.namespace_number == second.namespace_number;
}

static sxml_ordering sxml_ordering_of_numbers(uint64_t first,
                                              uint64_t second) {
    if (first < second) {
        return SXML_ORDERING_LESS;
    }
    return first > second ? SXML_ORDERING_GREATER : SXML_ORDERING_EQUAL;
}

sxml_ordering sxml_node_compare_document_order(sxml_node_pair pair) {
    /* XDM 4.0, section 6.2: across trees by the order identifiers were
       issued (D39); within a tree by (node index, namespace number). */
    if (pair.first.tree != pair.second.tree) {
        return sxml_ordering_of_numbers(pair.first.tree->identifier.value,
                                        pair.second.tree->identifier.value);
    }
    if (pair.first.node_index.index != pair.second.node_index.index) {
        return sxml_ordering_of_numbers(pair.first.node_index.index,
                                        pair.second.node_index.index);
    }
    return sxml_ordering_of_numbers(pair.first.namespace_number,
                                    pair.second.namespace_number);
}

/* ------------------------------------------------------------ Copying */

/*
 * The deep copy of sxml_tree_builder_copy_node. The subtree is replayed in
 * node index order, which is document order (D39), without recursion: when
 * the next node is not a child of the previous one, the elements between
 * them are ended by walking parent links. Every node is ended once, so the
 * copy is linear and its C stack depth constant, whatever the tree's depth.
 */
typedef struct sxml_node_copy {
    sxml_tree_builder builder;
    const sxml_copy_request *request;
    sxml_tree *tree;
} sxml_node_copy;

static sxml_node_reference sxml_node_copy_reference(
    const sxml_node_copy *copy, uint32_t node_number) {
    return sxml_node_reference_of_stored_node((sxml_stored_node_request){
        .tree = copy->tree, .node_number = node_number});
}

static sxml_type_identifier sxml_node_copy_type(
    const sxml_node_copy *copy, sxml_type_identifier type_annotation,
    sxml_builtin_type untyped_type) {
    /* Without preservation, copies are untyped (XQuery 4.0
       construction mode strip; XSLT 4.0 validation="strip"). */
    if (copy->request->preserves_type_annotations) {
        return type_annotation;
    }
    return sxml_builtin_type_identifier(untyped_type);
}

static bool sxml_namespace_binding_view_contains(
    sxml_namespace_binding_view bindings, sxml_namespace_binding binding) {
    for (size_t binding_index = 0;
         binding_index < bindings.namespace_binding_count; binding_index++) {
        sxml_namespace_binding candidate =
            bindings.namespace_bindings[binding_index];
        if (candidate.namespace_prefix.index ==
                binding.namespace_prefix.index &&
            candidate.namespace_uri.index == binding.namespace_uri.index) {
            return true;
        }
    }
    return false;
}

static bool sxml_namespace_binding_view_has_prefix(
    sxml_namespace_binding_view bindings,
    sxml_namespace_prefix_identifier prefix) {
    for (size_t binding_index = 0;
         binding_index < bindings.namespace_binding_count; binding_index++) {
        if (bindings.namespace_bindings[binding_index]
                .namespace_prefix.index == prefix.index) {
            return true;
        }
    }
    return false;
}


/*
 * The in-scope namespaces of the node numbered node_number when it is an
 * element; none for a document node, whose children inherit nothing.
 */
static sxml_status sxml_node_copy_get_parent_bindings(
    const sxml_node_copy *copy, uint32_t node_number,
    sxml_namespace_binding_view *bindings) {
    sxml_node_description parent = sxml_node_description_empty();
    sxml_status status = sxml_node_describe_stored(
        sxml_node_copy_reference(copy, node_number), &parent);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (parent.node_kind != SXML_NODE_KIND_ELEMENT) {
        *bindings = (sxml_namespace_binding_view){
            .namespace_bindings = NULL, .namespace_binding_count = 0};
        return SXML_STATUS_OK;
    }
    return sxml_node_get_in_scope_namespaces(
        sxml_node_copy_reference(copy, node_number), bindings);
}

/* One element to start in the copy. */
typedef struct sxml_element_copy {
    uint32_t node_number;
    const sxml_node_description *description;
    /* The in-scope namespaces the copy's parent already has from this
       copy; empty for the copy's top element. */
    sxml_namespace_binding_view parent_bindings;
    bool is_top_element;
} sxml_element_copy;

/*
 * Sends the namespace bindings that make the copy of an element have the
 * source element's in-scope namespaces (copy-namespaces preserve): those
 * its parent in the copy lacks, and undeclarations for those it must not
 * inherit. The xml binding is implicit in every builder and not sent.
 * Without preserve nothing is sent: namespace fixup in the builder binds
 * exactly the prefixes the names use (XQuery 4.0, copy-namespaces
 * no-preserve).
 */
static sxml_status sxml_node_copy_send_bindings(
    const sxml_node_copy *copy, const sxml_element_copy *element) {
    if (!copy->request->preserves_namespaces) {
        return SXML_STATUS_OK;
    }
    sxml_namespace_binding_view bindings = {.namespace_bindings = NULL,
                                            .namespace_binding_count = 0};
    sxml_status status = sxml_node_get_in_scope_namespaces(
        sxml_node_copy_reference(copy, element->node_number), &bindings);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_tree_builder_operations *operations =
        copy->builder.operations;
    for (size_t binding_index = 0;
         binding_index < bindings.namespace_binding_count; binding_index++) {
        sxml_namespace_binding binding =
            bindings.namespace_bindings[binding_index];
        if (binding.namespace_uri.index == SXML_NAMESPACE_XML ||
            sxml_namespace_binding_view_contains(element->parent_bindings,
                                                 binding)) {
            continue;
        }
        status = operations->add_namespace_binding(copy->builder.state,
                                                   &binding);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    for (size_t binding_index = 0;
         binding_index < element->parent_bindings.namespace_binding_count;
         binding_index++) {
        sxml_namespace_binding inherited =
            element->parent_bindings.namespace_bindings[binding_index];
        if (inherited.namespace_uri.index == SXML_NAMESPACE_XML ||
            sxml_namespace_binding_view_has_prefix(
                bindings, inherited.namespace_prefix)) {
            continue;
        }
        const sxml_namespace_binding undeclaration = {
            .namespace_prefix = inherited.namespace_prefix,
            .namespace_uri = {.index = SXML_NAMESPACE_NONE},
        };
        status = operations->add_namespace_binding(copy->builder.state,
                                                   &undeclaration);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/* Sends one stored attribute node as an attribute event. */
static sxml_status sxml_node_copy_send_attribute(const sxml_node_copy *copy,
                                                 uint32_t node_number) {
    sxml_node_reference node = sxml_node_copy_reference(copy, node_number);
    sxml_node_description description = sxml_node_description_empty();
    sxml_status status = sxml_node_describe_stored(node, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
    status = sxml_node_get_content(node, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_attribute_event event = {
        .attribute_name = description.node_name,
        .namespace_prefix = description.namespace_prefix,
        .attribute_value = value,
        .type_annotation =
            sxml_node_copy_type(copy, description.type_annotation,
                                SXML_TYPE_XS_UNTYPED_ATOMIC),
        .is_id = description.is_id,
        .is_idrefs = description.is_idrefs,
        .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                     .line_number = 0,
                     .column_number = 0,
                     .byte_offset = 0},
    };
    return copy->builder.operations->add_attribute(copy->builder.state,
                                                   &event);
}

/* Starts the copy of an element: start event, bindings, attributes. */
static sxml_status sxml_node_copy_start_element(
    const sxml_node_copy *copy, const sxml_element_copy *element) {
    const sxml_node_description *description = element->description;
    /* Without type preservation nilled is false too: it is defined only
       for validated elements (XDM 4.0, section 7.6.10). The ID properties
       stay, since xml:id makes an ID without validation. */
    const bool preserves_types = copy->request->preserves_type_annotations;
    const sxml_element_start_event event = {
        .element_name = description->node_name,
        .namespace_prefix = description->namespace_prefix,
        .type_annotation = sxml_node_copy_type(
            copy, description->type_annotation, SXML_TYPE_XS_UNTYPED),
        .is_id = description->is_id,
        .is_idrefs = description->is_idrefs,
        .is_nilled = preserves_types && description->is_nilled,
        /* Only the top element of a copy can be cut off from its new
           parent's bindings; its descendants inherit from it. */
        .ignores_parent_namespaces = element->is_top_element &&
                                     !copy->request->inherits_namespaces,
        .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                     .line_number = 0,
                     .column_number = 0,
                     .byte_offset = 0},
    };
    sxml_status status =
        copy->builder.operations->start_element(copy->builder.state, &event);
    if (status == SXML_STATUS_OK) {
        status = sxml_node_copy_send_bindings(copy, element);
    }
    for (uint32_t attribute_offset = 1;
         status == SXML_STATUS_OK &&
         attribute_offset <= description->attribute_count;
         attribute_offset++) {
        status = sxml_node_copy_send_attribute(
            copy, element->node_number + attribute_offset);
    }
    return status;
}

/* Sends a text, comment or processing-instruction node. */
static sxml_status sxml_node_copy_send_leaf(
    const sxml_node_copy *copy, uint32_t node_number,
    const sxml_node_description *description) {
    sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_node_get_content(
        sxml_node_copy_reference(copy, node_number), &content);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_source_location location = {
        .source_name = {.bytes = NULL, .byte_count = 0},
        .line_number = 0,
        .column_number = 0,
        .byte_offset = 0};
    const sxml_tree_builder_operations *operations = copy->builder.operations;
    switch (description->node_kind) {
    case SXML_NODE_KIND_TEXT:
        return operations->add_text(
            copy->builder.state,
            &(sxml_text_event){.text = content, .location = location});
    case SXML_NODE_KIND_COMMENT:
        return operations->add_comment(
            copy->builder.state,
            &(sxml_comment_event){.comment = content, .location = location});
    case SXML_NODE_KIND_PROCESSING_INSTRUCTION: {
        /* The target is the local name of the node name (XDM 4.0, section
           7.5.6). */
        sxml_name_parts parts = {.namespace_uri = {.index = 0},
                                 .local_name = {.bytes = NULL,
                                                .byte_count = 0}};
        status = sxml_name_pool_get_name_parts(
            copy->tree->name_pool, description->node_name, &parts);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return operations->add_processing_instruction(
            copy->builder.state,
            &(sxml_processing_instruction_event){.target = parts.local_name,
                                                 .data = content,
                                                 .location = location});
    }
    case SXML_NODE_KIND_DOCUMENT:
    case SXML_NODE_KIND_ELEMENT:
    case SXML_NODE_KIND_ATTRIBUTE:
    case SXML_NODE_KIND_NAMESPACE:
    case SXML_NODE_KIND_ANY:
        break;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* Two stored nodes: end the elements from one up to the other. */
typedef struct sxml_node_number_range {
    uint32_t from_node_number;
    uint32_t until_node_number;
} sxml_node_number_range;

/*
 * Ends every element from range.from_node_number up its ancestors, stopping
 * before range.until_node_number, which is an ancestor of it or itself.
 */
static sxml_status sxml_node_copy_end_elements(const sxml_node_copy *copy,
                                               sxml_node_number_range range) {
    uint32_t node_number = range.from_node_number;
    while (node_number != range.until_node_number) {
        sxml_node_description description = sxml_node_description_empty();
        sxml_status status = sxml_node_describe_stored(
            sxml_node_copy_reference(copy, node_number), &description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (description.node_kind == SXML_NODE_KIND_ELEMENT) {
            status =
                copy->builder.operations->end_element(copy->builder.state);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        if (!description.has_parent) {
            /* The contract was broken: until is not an ancestor. */
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        node_number = description.parent.index;
    }
    return SXML_STATUS_OK;
}

/* Sends the start of a node inside the copied subtree (not its root). */
static sxml_status sxml_node_copy_send_descendant(
    const sxml_node_copy *copy, uint32_t node_number,
    const sxml_node_description *description) {
    if (description->node_kind != SXML_NODE_KIND_ELEMENT) {
        return sxml_node_copy_send_leaf(copy, node_number, description);
    }
    sxml_namespace_binding_view parent_bindings = {
        .namespace_bindings = NULL, .namespace_binding_count = 0};
    sxml_status status = sxml_node_copy_get_parent_bindings(
        copy, description->parent.index, &parent_bindings);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_node_copy_start_element(
        copy, &(sxml_element_copy){.node_number = node_number,
                                   .description = description,
                                   .parent_bindings = parent_bindings,
                                   .is_top_element = false});
}

/*
 * Sends the content of a document or element numbered root_number: every
 * node after its attributes up to its last descendant, in document order,
 * with the end events between them; leaves the root itself open.
 */
static sxml_status sxml_node_copy_send_content(
    const sxml_node_copy *copy, uint32_t root_number,
    const sxml_node_description *root) {
    uint32_t previous_number = root_number;
    uint64_t next_number = (uint64_t)root_number + 1 + root->attribute_count;
    while (next_number <= root->last_descendant.index) {
        uint32_t node_number = (uint32_t)next_number;
        sxml_node_description description = sxml_node_description_empty();
        sxml_status status = sxml_node_describe_stored(
            sxml_node_copy_reference(copy, node_number), &description);
        if (status == SXML_STATUS_OK) {
            status = sxml_node_copy_end_elements(
                copy, (sxml_node_number_range){
                          .from_node_number = previous_number,
                          .until_node_number = description.parent.index});
        }
        if (status == SXML_STATUS_OK) {
            status =
                sxml_node_copy_send_descendant(copy, node_number, &description);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        previous_number = node_number;
        next_number = (uint64_t)node_number + 1 + description.attribute_count;
    }
    return sxml_node_copy_end_elements(
        copy, (sxml_node_number_range){.from_node_number = previous_number,
                                       .until_node_number = root_number});
}

/* Copies a document node: start, content, end (XDM 4.0, section 7.5.1). */
static sxml_status sxml_node_copy_send_document(
    const sxml_node_copy *copy, const sxml_node_description *root) {
    sxml_tree_description tree_description = {
        .node_count = 0,
        .document_uri = {.bytes = NULL, .byte_count = 0},
        .base_uri = {.bytes = NULL, .byte_count = 0},
        .name_pool = NULL,
    };
    sxml_status status = sxml_tree_describe(copy->tree, &tree_description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_document_start_event event = {
        .document_uri = tree_description.document_uri,
        .base_uri = tree_description.base_uri,
        .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                     .line_number = 0,
                     .column_number = 0,
                     .byte_offset = 0},
    };
    status =
        copy->builder.operations->start_document(copy->builder.state, &event);
    if (status == SXML_STATUS_OK) {
        status = sxml_node_copy_send_content(
            copy, copy->request->node.node_index.index, root);
    }
    if (status == SXML_STATUS_OK) {
        status = copy->builder.operations->end_document(copy->builder.state);
    }
    return status;
}

/* Copies a namespace node as a binding of the element being built. */
static sxml_status sxml_node_copy_send_namespace(const sxml_node_copy *copy) {
    sxml_namespace_binding binding = {.namespace_prefix = {.index = 0},
                                      .namespace_uri = {.index = 0}};
    sxml_status status =
        sxml_namespace_node_get_binding(copy->request->node, &binding);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return copy->builder.operations->add_namespace_binding(
        copy->builder.state, &binding);
}

/*
 * Copies a stored namespace node, the root of a tree of its own (D98), as a
 * binding of the element being built: its name is the prefix, its content
 * the namespace URI.
 */
static sxml_status sxml_node_copy_send_stored_namespace(
    const sxml_node_copy *copy, const sxml_node_description *description) {
    sxml_name_pool *name_pool = copy->tree->name_pool;
    sxml_byte_view uri_text = {.bytes = NULL, .byte_count = 0};
    sxml_status status = copy->tree->operations->get_node_content(
        copy->tree->state, copy->request->node.node_index, &uri_text);
    sxml_byte_view prefix_text = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK && description->node_name.index != 0) {
        sxml_name_parts parts = {.namespace_uri = {.index = 0},
                                 .local_name = {.bytes = NULL, .byte_count = 0}};
        status = sxml_name_pool_get_name_parts(name_pool, description->node_name,
                                               &parts);
        prefix_text = parts.local_name;
    }
    sxml_namespace_binding binding = {.namespace_prefix = {.index = 0},
                                      .namespace_uri = {.index = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_prefix(
            name_pool, prefix_text, &binding.namespace_prefix);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_uri(
            name_pool, uri_text, &binding.namespace_uri);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return copy->builder.operations->add_namespace_binding(copy->builder.state,
                                                           &binding);
}

sxml_status sxml_tree_builder_copy_node(sxml_tree_builder builder,
                                        const sxml_copy_request *request) {
    if (builder.operations == NULL || request->node.tree == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_node_copy copy = {
        .builder = builder,
        .request = request,
        .tree = request->node.tree,
    };
    if (request->node.namespace_number != 0) {
        return sxml_node_copy_send_namespace(&copy);
    }
    const uint32_t root_number = request->node.node_index.index;
    sxml_node_description root = sxml_node_description_empty();
    sxml_status status = sxml_node_describe_stored(request->node, &root);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    switch (root.node_kind) {
    case SXML_NODE_KIND_DOCUMENT:
        return sxml_node_copy_send_document(&copy, &root);
    case SXML_NODE_KIND_ELEMENT:
        /* The top element gets every in-scope namespace it has; whether it
           also inherits those of the element it is copied into is the
           builder's work, asked for by ignores_parent_namespaces (D69). */
        status = sxml_node_copy_start_element(
            &copy, &(sxml_element_copy){
                       .node_number = root_number,
                       .description = &root,
                       .parent_bindings = {.namespace_bindings = NULL,
                                           .namespace_binding_count = 0},
                       .is_top_element = true});
        if (status == SXML_STATUS_OK) {
            status = sxml_node_copy_send_content(&copy, root_number, &root);
        }
        if (status == SXML_STATUS_OK) {
            status = builder.operations->end_element(builder.state);
        }
        return status;
    case SXML_NODE_KIND_ATTRIBUTE:
        return sxml_node_copy_send_attribute(&copy, root_number);
    case SXML_NODE_KIND_TEXT:
    case SXML_NODE_KIND_COMMENT:
    case SXML_NODE_KIND_PROCESSING_INSTRUCTION:
        return sxml_node_copy_send_leaf(&copy, root_number, &root);
    case SXML_NODE_KIND_NAMESPACE:
        return sxml_node_copy_send_stored_namespace(&copy, &root);
    case SXML_NODE_KIND_ANY:
        break;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}
