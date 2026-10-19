/*
 * Applying a pending update list (src/xquery_update_internal.h;
 * docs/DECISIONS.md D187; packet 7.7a): XQuery Update Facility 3.0,
 * section 3.2.2, upd:mergeUpdates and upd:applyUpdates, and the copy and
 * modify-copies rows of the copy-modify expression (5.4.5).
 *
 * Golden files: src/tree.c (the copy of sxml_tree_builder_copy_node:
 * nodes replayed in document order without recursion),
 * src/function_library_schema.c (a copy in a new arena tree whose memory
 * is the item heap's).
 *
 * Trees are immutable (D39, D99), so applying never changes one: every
 * tree some primitive targets is rebuilt through the tree builder, in one
 * pass over the old tree in document order, with an explicit stack of the
 * open elements (D71). Each node's primitives are kept in a list per node
 * (by node index), and are applied as 3.2.2 orders them:
 *
 *     insertInto, insertAttributes, replaceValue, rename;
 *     insertBefore, insertAfter, insertIntoAsFirst, insertIntoAsLast;
 *     replaceNode; replaceElementContent; delete.
 *
 * Within one pass that order gives, for a node N: the content of its
 * insertBefore primitives; then its replacement (replaceNode), or nothing
 * when it is deleted, or N with its new name and value; then the content
 * of its insertAfter primitives. An element's children are its
 * insertIntoAsFirst content, its own children, its insertInto content
 * (an implementation-dependent position, 3.1.1: here after the children)
 * and its insertIntoAsLast content; replaceElementContent, applied after
 * every insert, replaces all of them with its text. Attributes are the
 * element's own, replaced, renamed or given new values, then those of
 * insertAttributes. A node without a parent is not deleted (3.1.2) and
 * cannot be replaced (5.4.3: XUDY0009 before).
 *
 * Copied content is copied as the content of an element constructor
 * (copy-namespaces preserve, inherit; construction strip): new nodes,
 * untyped. Rebuilt nodes that a primitive targets, and their ancestors,
 * become untyped (3.2.2: upd:removeType, without revalidation); the rest
 * keep their annotations.
 *
 * Section numbers are of the XQuery Update Facility 3.0 (W3C
 * Recommendation, 24 January 2017).
 */
#include "xquery_update_internal.h"

#include "function_library_core_internal.h"
#include "memory_internal.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "xdm_internal.h"

#include <stdalign.h>
#include <string.h>

/* --------------------------------------------------------------- Errors */

#define APPLY_ERROR(name, code, text)                                                       \
    static const sxml_static_error name = {.error_status = SXML_STATUS_DYNAMIC_ERROR,      \
                                           .code_local_name = SXML_BYTE_VIEW_INITIALIZER(code), \
                                           .description = SXML_BYTE_VIEW_INITIALIZER(text)}

APPLY_ERROR(copy_target_error, "XUDY0014",
            "the modify clause changes a node that its copy clauses did not create (5.4.5)");
APPLY_ERROR(rename_twice_error, "XUDY0015", "a node is renamed twice (3.2.2)");
APPLY_ERROR(replace_twice_error, "XUDY0016", "a node is replaced twice (3.2.2)");
APPLY_ERROR(replace_value_twice_error, "XUDY0017",
            "the value of a node is replaced twice (3.2.2)");
APPLY_ERROR(instance_error, "XUDY0021",
            "the updated tree breaks a constraint of the data model: two attributes of one "
            "name, or conflicting namespace bindings (3.2.2)");
APPLY_ERROR(namespace_conflict_error, "XUDY0024",
            "two updates of one element give conflicting namespace bindings (3.2.2)");

static const sxml_static_error copy_source_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XUTY0013"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the source of a copy clause is not exactly one node (5.4.5)")};

/* ---------------------------------------------------------- Primitives */

/* One primitive of the list and the next one of the same target node
   (a primitive number, from 1; 0 for none). */
typedef struct primitive_entry {
    sxml_xquery_update_primitive primitive;
    uint32_t next_number;
} primitive_entry;

/* The first and last primitive of a node, by primitive number. */
typedef struct node_primitives {
    uint32_t first_number;
    uint32_t last_number;
    /* A primitive targets the node or one of its descendants: rebuilt
       untyped. */
    bool is_touched;
} node_primitives;

/* One tree to rebuild and the primitives of each of its nodes. */
typedef struct tree_edit {
    sxml_tree *tree;
    uint32_t node_count;
    node_primitives *nodes;
} tree_edit;

/* What applying one list works with. */
typedef struct application {
    const sxml_xquery_update_application *request;
    sxml_allocator allocator;
    primitive_entry *entries;
    size_t entry_count;
    tree_edit *trees;
    size_t tree_count;
} application;

static sxml_status report(const application *work, const sxml_static_error *error) {
    return sxml_error_record_report(work->request->error_record, error);
}

static const primitive_entry *entry_of(const application *work, uint32_t primitive_number) {
    return &work->entries[primitive_number - 1];
}

static sxml_status find_tree_edit(application *work, sxml_tree *tree, tree_edit **edit) {
    for (size_t tree_index = 0; tree_index < work->tree_count; tree_index += 1) {
        if (work->trees[tree_index].tree == tree) {
            *edit = &work->trees[tree_index];
            return SXML_STATUS_OK;
        }
    }
    sxml_tree_description description;
    sxml_status status = sxml_tree_describe(tree, &description);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(
            work->allocator,
            (sxml_array_allocation_request){.element_count = description.node_count,
                                            .element_size = sizeof(node_primitives),
                                            .element_alignment = alignof(node_primitives)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *edit = &work->trees[work->tree_count];
    **edit = (tree_edit){.tree = tree, .node_count = description.node_count, .nodes = memory};
    work->tree_count += 1;
    return SXML_STATUS_OK;
}

static bool is_value_replacement(sxml_xquery_update_primitive_kind kind) {
    return kind == SXML_XQUERY_UPDATE_PRIMITIVE_REPLACE_VALUE ||
           kind == SXML_XQUERY_UPDATE_PRIMITIVE_REPLACE_ELEMENT_CONTENT;
}

/* upd:mergeUpdates' checks for a new primitive of a node against the
   node's earlier ones (3.2.2: XUDY0015, XUDY0016, XUDY0017). */
static sxml_status check_compatibility(const application *work, const node_primitives *node,
                                       sxml_xquery_update_primitive_kind kind) {
    for (uint32_t number = node->first_number; number != 0;
         number = entry_of(work, number)->next_number) {
        const sxml_xquery_update_primitive_kind earlier =
            entry_of(work, number)->primitive.primitive_kind;
        if (kind == SXML_XQUERY_UPDATE_PRIMITIVE_RENAME && earlier == kind) {
            return report(work, &rename_twice_error);
        }
        if (kind == SXML_XQUERY_UPDATE_PRIMITIVE_REPLACE_NODE && earlier == kind) {
            return report(work, &replace_twice_error);
        }
        if (is_value_replacement(kind) && is_value_replacement(earlier)) {
            return report(work, &replace_value_twice_error);
        }
    }
    return SXML_STATUS_OK;
}

/* Marks node and its ancestors touched. */
static sxml_status mark_touched(const tree_edit *edit, sxml_node_reference node) {
    sxml_node_index index = node.node_index;
    while (!edit->nodes[index.index].is_touched) {
        edit->nodes[index.index].is_touched = true;
        sxml_node_description description;
        const sxml_status status = sxml_node_describe(
            (sxml_node_reference){.tree = edit->tree, .node_index = index, .namespace_number = 0},
            &description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!description.has_parent) {
            break;
        }
        index = description.parent;
    }
    return SXML_STATUS_OK;
}

/* Whether content has an element, an attribute or text (an xs:string
   item): what changes the type of the node it goes into. */
static bool content_has_typed_part(sxml_sequence content) {
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(content);
    for (;;) {
        sxml_iteration_step step = {.has_item = false};
        if (sxml_sequence_iterator_next(&iterator, NULL, &step) != SXML_STATUS_OK ||
            !step.has_item) {
            return false;
        }
        sxml_node_reference node;
        sxml_node_description description;
        if (sxml_item_get_node(step.item, &node) != SXML_STATUS_OK) {
            return true;
        }
        if (sxml_node_describe(node, &description) == SXML_STATUS_OK &&
            description.node_kind != SXML_NODE_KIND_COMMENT &&
            description.node_kind != SXML_NODE_KIND_PROCESSING_INSTRUCTION) {
            return true;
        }
    }
}

/* upd:removeType (3.1.x, and each primitive's rule of when it calls it):
   the node it applies to and every ancestor are touched, and rebuilt
   without their type. A comment or processing instruction inserted,
   deleted or changed changes no type. */
static sxml_status remove_types(const tree_edit *edit,
                                const sxml_xquery_update_primitive *primitive) {
    sxml_node_description target;
    sxml_status status = sxml_node_describe(primitive->target, &target);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool is_leaf = target.node_kind == SXML_NODE_KIND_COMMENT ||
                         target.node_kind == SXML_NODE_KIND_PROCESSING_INSTRUCTION;
    sxml_node_reference node = primitive->target;
    bool removes = true;
    bool from_parent = false;
    switch (primitive->primitive_kind) {
    case SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_INTO:
    case SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_INTO_AS_FIRST:
    case SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_INTO_AS_LAST:
        removes = content_has_typed_part(primitive->parameter);
        break;
    case SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_BEFORE:
    case SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_AFTER:
        removes = content_has_typed_part(primitive->parameter);
        from_parent = true;
        break;
    case SXML_XQUERY_UPDATE_PRIMITIVE_DELETE:
        removes = !is_leaf;
        from_parent = true;
        break;
    case SXML_XQUERY_UPDATE_PRIMITIVE_REPLACE_NODE:
        removes = !is_leaf || content_has_typed_part(primitive->parameter);
        from_parent = true;
        break;
    case SXML_XQUERY_UPDATE_PRIMITIVE_REPLACE_VALUE:
        removes = !is_leaf;
        from_parent = target.node_kind == SXML_NODE_KIND_TEXT;
        break;
    case SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_ATTRIBUTES:
    case SXML_XQUERY_UPDATE_PRIMITIVE_REPLACE_ELEMENT_CONTENT:
        break;
    case SXML_XQUERY_UPDATE_PRIMITIVE_RENAME:
        removes = !is_leaf;
        break;
    }
    if (!removes) {
        return SXML_STATUS_OK;
    }
    if (from_parent) {
        if (!target.has_parent) {
            return SXML_STATUS_OK;
        }
        node.node_index = target.parent;
    }
    return mark_touched(edit, node);
}

/* Whether tree is the tree of one of the allowed roots (5.4.5). */
static bool is_allowed_tree(const application *work, const sxml_tree *tree) {
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(work->request->allowed_roots);
    for (;;) {
        sxml_iteration_step step = {.has_item = false};
        if (sxml_sequence_iterator_next(&iterator, NULL, &step) != SXML_STATUS_OK ||
            !step.has_item) {
            return false;
        }
        sxml_node_reference root;
        if (sxml_item_get_node(step.item, &root) == SXML_STATUS_OK && root.tree == tree) {
            return true;
        }
    }
}

/* Reads the list into entries and links every entry to its node. */
static sxml_status read_primitives(application *work) {
    const sxml_xquery_update_application *request = work->request;
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(request->pending_updates, request->error_record,
                                             &item_count);
    if (status == SXML_STATUS_OK && item_count > UINT32_MAX - 1U) {
        status = SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(
            work->allocator,
            (sxml_array_allocation_request){.element_count = (size_t)item_count,
                                            .element_size = sizeof(primitive_entry),
                                            .element_alignment = alignof(primitive_entry)},
            (void **)&work->entries);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(
            work->allocator,
            (sxml_array_allocation_request){.element_count = (size_t)item_count,
                                            .element_size = sizeof(tree_edit),
                                            .element_alignment = alignof(tree_edit)},
            (void **)&work->trees);
    }
    sxml_emptiness emptiness = {.is_empty = true};
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_test_emptiness(request->allowed_roots, NULL, &emptiness);
    }
    const bool has_allowed_roots = !emptiness.is_empty;
    for (uint64_t item_index = 0; status == SXML_STATUS_OK && item_index < item_count;
         item_index += 1) {
        sxml_item item = {.representation = 0};
        status = sxml_sequence_get_item(
            request->pending_updates,
            (sxml_item_request){.item_index = item_index, .error_record = request->error_record},
            &item);
        primitive_entry *entry = &work->entries[work->entry_count];
        if (status == SXML_STATUS_OK) {
            status = sxml_xquery_read_update_primitive(item, &entry->primitive);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (has_allowed_roots && !is_allowed_tree(work, entry->primitive.target.tree)) {
            status = report(work, &copy_target_error);
            break;
        }
        work->entry_count += 1;
        const uint32_t number = (uint32_t)work->entry_count;
        tree_edit *edit = NULL;
        status = find_tree_edit(work, entry->primitive.target.tree, &edit);
        node_primitives *node = NULL;
        if (status == SXML_STATUS_OK) {
            node = &edit->nodes[entry->primitive.target.node_index.index];
            status = check_compatibility(work, node, entry->primitive.primitive_kind);
        }
        if (status == SXML_STATUS_OK) {
            if (node->last_number == 0) {
                node->first_number = number;
            } else {
                work->entries[node->last_number - 1].next_number = number;
            }
            node->last_number = number;
            status = remove_types(edit, &entry->primitive);
        }
    }
    return status;
}

/* ------------------------------------------- Attribute names (XUDY0021) */

/* One name in the attribute set of an element after the update. */
typedef struct attribute_names {
    const application *work;
    sxml_name_identifier *names;
    size_t name_count;
    size_t name_capacity;
} attribute_names;

static sxml_status add_attribute_name(attribute_names *names, sxml_name_identifier name) {
    for (size_t name_index = 0; name_index < names->name_count; name_index += 1) {
        if (names->names[name_index].index == name.index) {
            return report(names->work, &instance_error);
        }
    }
    if (names->name_count == names->name_capacity) {
        const size_t capacity = names->name_capacity == 0 ? 16 : names->name_capacity * 2;
        void *memory = NULL;
        const sxml_status status = sxml_allocator_allocate_array(
            names->work->allocator,
            (sxml_array_allocation_request){.element_count = capacity,
                                            .element_size = sizeof(sxml_name_identifier),
                                            .element_alignment = alignof(sxml_name_identifier)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (names->name_count != 0) {
            memcpy(memory, names->names, names->name_count * sizeof(sxml_name_identifier));
        }
        sxml_allocator_release(names->work->allocator, names->names);
        names->names = memory;
        names->name_capacity = capacity;
    }
    names->names[names->name_count] = name;
    names->name_count += 1;
    return SXML_STATUS_OK;
}

/* The names of the attribute nodes in content. */
static sxml_status add_content_names(attribute_names *names, sxml_sequence content) {
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(content);
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, NULL, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_node_reference node;
        sxml_node_description description;
        if (sxml_item_get_node(step.item, &node) == SXML_STATUS_OK &&
            sxml_node_describe(node, &description) == SXML_STATUS_OK &&
            description.node_kind == SXML_NODE_KIND_ATTRIBUTE) {
            status = add_attribute_name(names, description.node_name);
        }
    }
    return status;
}

/* The primitive of kind among node's, or NULL. */
typedef struct primitive_query {
    const tree_edit *edit;
    sxml_node_index node;
    sxml_xquery_update_primitive_kind primitive_kind;
} primitive_query;

static const sxml_xquery_update_primitive *find_node_primitive(const application *work,
                                                               primitive_query query) {
    for (uint32_t number = query.edit->nodes[query.node.index].first_number; number != 0;
         number = entry_of(work, number)->next_number) {
        if (entry_of(work, number)->primitive.primitive_kind == query.primitive_kind) {
            return &entry_of(work, number)->primitive;
        }
    }
    return NULL;
}

/* One attribute of the element as it is after the update. */
static sxml_status add_old_attribute_name(attribute_names *names, const tree_edit *edit,
                                          sxml_node_index attribute) {
    const application *work = names->work;
    const sxml_xquery_update_primitive *replacement = find_node_primitive(
        work, (primitive_query){.edit = edit,
                                .node = attribute,
                                .primitive_kind = SXML_XQUERY_UPDATE_PRIMITIVE_REPLACE_NODE});
    if (replacement != NULL) {
        return add_content_names(names, replacement->parameter);
    }
    if (find_node_primitive(work, (primitive_query){.edit = edit,
                                                    .node = attribute,
                                                    .primitive_kind =
                                                        SXML_XQUERY_UPDATE_PRIMITIVE_DELETE}) !=
        NULL) {
        return SXML_STATUS_OK;
    }
    const sxml_xquery_update_primitive *rename = find_node_primitive(
        work, (primitive_query){.edit = edit,
                                .node = attribute,
                                .primitive_kind = SXML_XQUERY_UPDATE_PRIMITIVE_RENAME});
    sxml_atomic_datum datum;
    sxml_item item = {.representation = 0};
    if (rename != NULL) {
        sxml_status status = sxml_sequence_get_item(
            rename->parameter, (sxml_item_request){.item_index = 0, .error_record = NULL}, &item);
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_get_datum(item, &datum);
        }
        return status == SXML_STATUS_OK
                   ? add_attribute_name(names, datum.datum_details.qname.expanded_name)
                   : status;
    }
    sxml_node_description description;
    const sxml_status status = sxml_node_describe(
        (sxml_node_reference){.tree = edit->tree, .node_index = attribute, .namespace_number = 0},
        &description);
    return status == SXML_STATUS_OK ? add_attribute_name(names, description.node_name) : status;
}

/* XUDY0021 when an element would have two attributes of one name after
   the update (3.2.2, whether or not the element itself stays). */
typedef struct element_in_edit {
    const tree_edit *edit;
    sxml_node_index element;
} element_in_edit;

static sxml_status check_attribute_names(const application *work, element_in_edit request) {
    sxml_node_description description;
    sxml_status status = sxml_node_describe((sxml_node_reference){.tree = request.edit->tree,
                                                                  .node_index = request.element,
                                                                  .namespace_number = 0},
                                            &description);
    if (status != SXML_STATUS_OK || description.node_kind != SXML_NODE_KIND_ELEMENT) {
        return status;
    }
    attribute_names names = {.work = work, .names = NULL, .name_count = 0, .name_capacity = 0};
    for (uint32_t offset = 1; status == SXML_STATUS_OK && offset <= description.attribute_count;
         offset += 1) {
        status = add_old_attribute_name(
            &names, request.edit, (sxml_node_index){.index = request.element.index + offset});
    }
    for (uint32_t number = request.edit->nodes[request.element.index].first_number;
         status == SXML_STATUS_OK && number != 0; number = entry_of(work, number)->next_number) {
        const sxml_xquery_update_primitive *primitive = &entry_of(work, number)->primitive;
        if (primitive->primitive_kind == SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_ATTRIBUTES) {
            status = add_content_names(&names, primitive->parameter);
        }
    }
    sxml_allocator_release(work->allocator, names.names);
    return status;
}

/* Checks the attribute names of every element whose attributes a
   primitive changes. */
static sxml_status check_changed_attributes(const application *work) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t entry_index = 0; status == SXML_STATUS_OK && entry_index < work->entry_count;
         entry_index += 1) {
        const sxml_xquery_update_primitive *primitive = &work->entries[entry_index].primitive;
        sxml_node_description description;
        status = sxml_node_describe(primitive->target, &description);
        sxml_node_index element = primitive->target.node_index;
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (description.node_kind == SXML_NODE_KIND_ATTRIBUTE) {
            if (!description.has_parent) {
                continue;
            }
            element = description.parent;
        } else if (primitive->primitive_kind != SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_ATTRIBUTES) {
            continue;
        }
        const tree_edit *edit = NULL;
        for (size_t tree_index = 0; tree_index < work->tree_count; tree_index += 1) {
            if (work->trees[tree_index].tree == primitive->target.tree) {
                edit = &work->trees[tree_index];
            }
        }
        if (edit != NULL) {
            status = check_attribute_names(work, (element_in_edit){.edit = edit, .element = element});
        }
    }
    return status;
}

/* --------------------------------------------------------- Rebuilding */

/* A tree being rebuilt: the old tree and its primitives, the builder of
   the new one, and the stack of open elements (and the root) of the old
   tree, by node index. */
typedef struct rebuild {
    const application *work;
    const tree_edit *edit;
    sxml_tree_builder builder;
    sxml_node_index *open_nodes;
    size_t open_count;
} rebuild;

static sxml_node_reference old_node(const rebuild *state, sxml_node_index index) {
    return (sxml_node_reference){.tree = state->edit->tree, .node_index = index, .namespace_number = 0};
}

static const node_primitives *primitives_of(const rebuild *state, sxml_node_index index) {
    return &state->edit->nodes[index.index];
}

/* The first primitive of kind on node; NULL for none. */
static const sxml_xquery_update_primitive *find_primitive(const rebuild *state,
                                                          sxml_node_index node,
                                                          sxml_xquery_update_primitive_kind kind) {
    for (uint32_t number = primitives_of(state, node)->first_number; number != 0;
         number = entry_of(state->work, number)->next_number) {
        if (entry_of(state->work, number)->primitive.primitive_kind == kind) {
            return &entry_of(state->work, number)->primitive;
        }
    }
    return NULL;
}

static const sxml_source_location no_location = {
    .source_name = {.bytes = NULL, .byte_count = 0}, .line_number = 0, .column_number = 0,
    .byte_offset = 0};

/* Sends a primitive's content (nodes copied with its copy-namespaces
   modes, xs:string items as text) to the builder. */
static sxml_status send_content(const rebuild *state,
                                const sxml_xquery_update_primitive *primitive) {
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(primitive->parameter);
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, NULL, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_node_reference node;
        if (sxml_item_get_node(step.item, &node) == SXML_STATUS_OK) {
            status = sxml_tree_builder_copy_node(
                state->builder,
                &(sxml_copy_request){.node = node,
                                     .preserves_namespaces = primitive->preserves_namespaces,
                                     .inherits_namespaces = primitive->inherits_namespaces,
                                     .preserves_type_annotations = primitive->preserves_types});
            continue;
        }
        sxml_atomic_datum datum;
        status = sxml_atomic_item_get_datum(step.item, &datum);
        if (status == SXML_STATUS_OK) {
            status = state->builder.operations->add_text(
                state->builder.state, &(sxml_text_event){.text = datum.datum_details.string.utf8,
                                                         .location = no_location,
                                                         .is_cdata_section = false});
        }
    }
    return status;
}

/* Sends the content of every primitive of kind on node, in list order. */
typedef struct content_request {
    sxml_node_index node;
    sxml_xquery_update_primitive_kind primitive_kind;
} content_request;

static sxml_status send_primitive_content(const rebuild *state, content_request request) {
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t number = primitives_of(state, request.node)->first_number;
         status == SXML_STATUS_OK && number != 0;
         number = entry_of(state->work, number)->next_number) {
        const sxml_xquery_update_primitive *primitive = &entry_of(state->work, number)->primitive;
        if (primitive->primitive_kind == request.primitive_kind) {
            status = send_content(state, primitive);
        }
    }
    return status;
}

/* The one item of a primitive's parameter. */
static sxml_status parameter_item(const sxml_xquery_update_primitive *primitive, sxml_item *item) {
    return sxml_sequence_get_item(primitive->parameter,
                                  (sxml_item_request){.item_index = 0, .error_record = NULL}, item);
}

/* The new name of a renamed node, or its old one. */
typedef struct node_name {
    sxml_name_identifier expanded_name;
    sxml_namespace_prefix_identifier namespace_prefix;
} node_name;

/* A node of the old tree and its description. */
typedef struct described_node {
    sxml_node_index index;
    const sxml_node_description *description;
} described_node;

static sxml_status read_node_name(const rebuild *state, described_node node, node_name *name) {
    *name = (node_name){.expanded_name = node.description->node_name,
                        .namespace_prefix = node.description->namespace_prefix};
    const sxml_xquery_update_primitive *rename =
        find_primitive(state, node.index, SXML_XQUERY_UPDATE_PRIMITIVE_RENAME);
    if (rename == NULL) {
        return SXML_STATUS_OK;
    }
    sxml_item item = {.representation = 0};
    sxml_atomic_datum datum;
    sxml_status status = parameter_item(rename, &item);
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(item, &datum);
    }
    if (status == SXML_STATUS_OK) {
        *name = (node_name){.expanded_name = datum.datum_details.qname.expanded_name,
                            .namespace_prefix = datum.datum_details.qname.namespace_prefix};
    }
    return status;
}

/* The new value of a node whose value is replaced, or its old content. */
static sxml_status read_node_value(const rebuild *state, sxml_node_index node,
                                   sxml_byte_view *value) {
    const sxml_xquery_update_primitive *replacement =
        find_primitive(state, node, SXML_XQUERY_UPDATE_PRIMITIVE_REPLACE_VALUE);
    if (replacement == NULL) {
        return sxml_node_get_content(old_node(state, node), value);
    }
    sxml_item item = {.representation = 0};
    sxml_atomic_datum datum;
    sxml_status status = parameter_item(replacement, &item);
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(item, &datum);
    }
    if (status == SXML_STATUS_OK) {
        *value = datum.datum_details.string.utf8;
    }
    return status;
}

/* A rebuilt node's annotation: untyped_type when the node is touched. */
typedef struct annotation_request {
    sxml_node_index node;
    sxml_type_identifier type_annotation;
    sxml_builtin_type untyped_type;
} annotation_request;

/* upd:removeType (3.1.x): an untyped node stays untyped; a typed
   element becomes xs:anyType, a typed attribute xs:untypedAtomic. */
static sxml_type_identifier rebuilt_type(const rebuild *state, annotation_request request) {
    if (!primitives_of(state, request.node)->is_touched ||
        request.type_annotation.index == SXML_TYPE_NONE ||
        sxml_type_identifier_equals(request.type_annotation,
                                    sxml_builtin_type_identifier(request.untyped_type))) {
        return request.type_annotation;
    }
    return request.untyped_type == SXML_TYPE_XS_UNTYPED
               ? sxml_builtin_type_identifier(SXML_TYPE_XS_ANY_TYPE)
               : sxml_builtin_type_identifier(request.untyped_type);
}

/* An attribute of an element being rebuilt (or a parentless attribute). */
static sxml_status send_attribute(const rebuild *state, sxml_node_index attribute) {
    const node_primitives *primitives = primitives_of(state, attribute);
    if (primitives->first_number != 0) {
        const sxml_xquery_update_primitive *replacement =
            find_primitive(state, attribute, SXML_XQUERY_UPDATE_PRIMITIVE_REPLACE_NODE);
        if (replacement != NULL) {
            return send_content(state, replacement);
        }
        if (find_primitive(state, attribute, SXML_XQUERY_UPDATE_PRIMITIVE_DELETE) != NULL) {
            return SXML_STATUS_OK;
        }
    }
    sxml_node_description description;
    sxml_status status = sxml_node_describe(old_node(state, attribute), &description);
    node_name name = {.expanded_name = {0}, .namespace_prefix = {0}};
    if (status == SXML_STATUS_OK) {
        status = read_node_name(
            state, (described_node){.index = attribute, .description = &description}, &name);
    }
    sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = read_node_value(state, attribute, &value);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return state->builder.operations->add_attribute(
        state->builder.state,
        &(sxml_attribute_event){
            .attribute_name = name.expanded_name,
            .namespace_prefix = name.namespace_prefix,
            .attribute_value = value,
            .type_annotation =
                rebuilt_type(state, (annotation_request){.node = attribute,
                                                         .type_annotation =
                                                             description.type_annotation,
                                                         .untyped_type =
                                                             SXML_TYPE_XS_UNTYPED_ATOMIC}),
            .is_id = description.is_id && !primitives->is_touched,
            .is_idrefs = description.is_idrefs && !primitives->is_touched,
            .location = no_location});
}

/* ------------------------------------------- Namespace conflicts (3.2.2) */

/* The binding a new name needs: none without a prefix. */
typedef struct needed_binding {
    bool has_binding;
    sxml_namespace_binding binding;
} needed_binding;

static sxml_status binding_of(const rebuild *state, node_name name, needed_binding *binding) {
    sxml_name_parts parts;
    const sxml_status status = sxml_name_pool_get_name_parts(
        sxml_item_heap_get_name_pool(state->work->request->item_heap), name.expanded_name, &parts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *binding = (needed_binding){.has_binding = name.namespace_prefix.index != 0,
                                .binding = {.namespace_prefix = name.namespace_prefix,
                                            .namespace_uri = parts.namespace_uri}};
    return SXML_STATUS_OK;
}

/* The bindings the updates of one element introduce, compared as they
   come: two of one prefix to different URIs are XUDY0024. */
typedef struct binding_set {
    sxml_namespace_binding bindings[16];
    size_t binding_count;
} binding_set;

static sxml_status add_binding(const rebuild *state, binding_set *set, node_name name) {
    needed_binding needed = {.has_binding = false};
    const sxml_status status = binding_of(state, name, &needed);
    if (status != SXML_STATUS_OK || !needed.has_binding) {
        return status;
    }
    const sxml_namespace_binding binding = needed.binding;
    for (size_t binding_index = 0; binding_index < set->binding_count; binding_index += 1) {
        if (set->bindings[binding_index].namespace_prefix.index == binding.namespace_prefix.index &&
            set->bindings[binding_index].namespace_uri.index != binding.namespace_uri.index) {
            return report(state->work, &namespace_conflict_error);
        }
    }
    if (set->binding_count < sizeof set->bindings / sizeof set->bindings[0]) {
        set->bindings[set->binding_count] = binding;
        set->binding_count += 1;
    }
    return SXML_STATUS_OK;
}

/* The names of the attributes in content. */
static sxml_status add_content_bindings(const rebuild *state, binding_set *set,
                                        sxml_sequence content) {
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(content);
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, NULL, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_node_reference node;
        sxml_node_description description;
        if (sxml_item_get_node(step.item, &node) != SXML_STATUS_OK ||
            sxml_node_describe(node, &description) != SXML_STATUS_OK ||
            description.node_kind != SXML_NODE_KIND_ATTRIBUTE) {
            continue;
        }
        status = add_binding(state, set,
                             (node_name){.expanded_name = description.node_name,
                                         .namespace_prefix = description.namespace_prefix});
    }
    return status;
}

/* The bindings of one node's renames and attribute content. */
static sxml_status add_node_bindings(const rebuild *state, binding_set *set, sxml_node_index node) {
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t number = primitives_of(state, node)->first_number;
         status == SXML_STATUS_OK && number != 0;
         number = entry_of(state->work, number)->next_number) {
        const sxml_xquery_update_primitive *primitive = &entry_of(state->work, number)->primitive;
        if (primitive->primitive_kind == SXML_XQUERY_UPDATE_PRIMITIVE_RENAME) {
            sxml_item item = {.representation = 0};
            sxml_atomic_datum datum;
            status = parameter_item(primitive, &item);
            if (status == SXML_STATUS_OK) {
                status = sxml_atomic_item_get_datum(item, &datum);
            }
            if (status == SXML_STATUS_OK) {
                status = add_binding(
                    state, set,
                    (node_name){.expanded_name = datum.datum_details.qname.expanded_name,
                                .namespace_prefix = datum.datum_details.qname.namespace_prefix});
            }
        } else if (primitive->primitive_kind == SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_ATTRIBUTES ||
                   primitive->primitive_kind == SXML_XQUERY_UPDATE_PRIMITIVE_REPLACE_NODE) {
            status = add_content_bindings(state, set, primitive->parameter);
        }
    }
    return status;
}

/* XUDY0024 among the updates of element and of its attributes. */
static sxml_status check_element_bindings(const rebuild *state, sxml_node_index element,
                                          const sxml_node_description *description) {
    binding_set set = {.binding_count = 0};
    sxml_status status = add_node_bindings(state, &set, element);
    for (uint32_t offset = 1; status == SXML_STATUS_OK && offset <= description->attribute_count;
         offset += 1) {
        status = add_node_bindings(
            state, &set, (sxml_node_index){.index = element.index + offset});
    }
    return status;
}

/* ------------------------------------------------------------ Elements */

static bool view_has_binding(sxml_namespace_binding_view bindings, sxml_namespace_binding binding,
                             bool compares_uri) {
    for (size_t binding_index = 0; binding_index < bindings.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding candidate = bindings.namespace_bindings[binding_index];
        if (candidate.namespace_prefix.index == binding.namespace_prefix.index &&
            (!compares_uri || candidate.namespace_uri.index == binding.namespace_uri.index)) {
            return true;
        }
    }
    return false;
}

/* The in-scope namespaces of the old element that its old parent element
   lacks, and undeclarations of those it does not inherit (as
   sxml_tree_builder_copy_node does with copy-namespaces preserve). */
static sxml_status send_element_bindings(const rebuild *state, sxml_node_index element,
                                         const sxml_node_description *description) {
    sxml_namespace_binding_view bindings = {.namespace_bindings = NULL,
                                            .namespace_binding_count = 0};
    sxml_namespace_binding_view parent_bindings = {.namespace_bindings = NULL,
                                                   .namespace_binding_count = 0};
    sxml_status status = sxml_node_get_in_scope_namespaces(old_node(state, element), &bindings);
    if (status == SXML_STATUS_OK && description->has_parent) {
        sxml_node_description parent;
        status = sxml_node_describe(old_node(state, description->parent), &parent);
        if (status == SXML_STATUS_OK && parent.node_kind == SXML_NODE_KIND_ELEMENT) {
            status = sxml_node_get_in_scope_namespaces(old_node(state, description->parent),
                                                       &parent_bindings);
        }
    }
    const sxml_tree_builder builder = state->builder;
    for (size_t binding_index = 0;
         status == SXML_STATUS_OK && binding_index < bindings.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding binding = bindings.namespace_bindings[binding_index];
        if (binding.namespace_uri.index != SXML_NAMESPACE_XML &&
            !view_has_binding(parent_bindings, binding, true)) {
            status = builder.operations->add_namespace_binding(builder.state, &binding);
        }
    }
    for (size_t binding_index = 0;
         status == SXML_STATUS_OK && binding_index < parent_bindings.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding inherited = parent_bindings.namespace_bindings[binding_index];
        if (inherited.namespace_uri.index != SXML_NAMESPACE_XML &&
            !view_has_binding(bindings, inherited, false)) {
            const sxml_namespace_binding undeclaration = {
                .namespace_prefix = inherited.namespace_prefix,
                .namespace_uri = {.index = SXML_NAMESPACE_NONE}};
            status = builder.operations->add_namespace_binding(builder.state, &undeclaration);
        }
    }
    return status;
}

/*
 * Starts an element of the new tree: its (new) name, bindings, attributes
 * and inserted attributes; then, when its content is replaced, that text
 * and its end, writing *is_closed true; else its insertIntoAsFirst
 * content.
 */
static sxml_status start_element(const rebuild *state, described_node node, bool *is_closed) {
    const sxml_node_index element = node.index;
    const sxml_node_description *description = node.description;
    *is_closed = false;
    node_name name = {.expanded_name = {0}, .namespace_prefix = {0}};
    sxml_status status = check_element_bindings(state, element, description);
    if (status == SXML_STATUS_OK) {
        status = read_node_name(state, node, &name);
    }
    const bool is_touched = primitives_of(state, element)->is_touched;
    if (status == SXML_STATUS_OK) {
        status = state->builder.operations->start_element(
            state->builder.state,
            &(sxml_element_start_event){
                .element_name = name.expanded_name,
                .namespace_prefix = name.namespace_prefix,
                .type_annotation =
                    rebuilt_type(state, (annotation_request){.node = element,
                                                             .type_annotation =
                                                                 description->type_annotation,
                                                             .untyped_type = SXML_TYPE_XS_UNTYPED}),
                .is_id = description->is_id && !is_touched,
                .is_idrefs = description->is_idrefs && !is_touched,
                .is_nilled = description->is_nilled && !is_touched,
                .ignores_parent_namespaces = false,
                .location = no_location});
    }
    if (status == SXML_STATUS_OK) {
        status = send_element_bindings(state, element, description);
    }
    for (uint32_t offset = 1; status == SXML_STATUS_OK && offset <= description->attribute_count;
         offset += 1) {
        status = send_attribute(state, (sxml_node_index){.index = element.index + offset});
    }
    if (status == SXML_STATUS_OK) {
        status = send_primitive_content(
            state, (content_request){.node = element,
                                     .primitive_kind =
                                         SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_ATTRIBUTES});
    }
    const sxml_xquery_update_primitive *content =
        find_primitive(state, element, SXML_XQUERY_UPDATE_PRIMITIVE_REPLACE_ELEMENT_CONTENT);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (content != NULL) {
        *is_closed = true;
        status = send_content(state, content);
        if (status == SXML_STATUS_OK) {
            status = state->builder.operations->end_element(state->builder.state);
        }
        return status;
    }
    return send_primitive_content(
        state, (content_request){.node = element,
                                 .primitive_kind = SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_INTO_AS_FIRST});
}

/* Ends an open element or document: its insertInto and insertIntoAsLast
   content, then the end event. */
static sxml_status close_node(const rebuild *state, sxml_node_index node, sxml_node_kind kind) {
    sxml_status status = send_primitive_content(
        state,
        (content_request){.node = node, .primitive_kind = SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_INTO});
    if (status == SXML_STATUS_OK) {
        status = send_primitive_content(
            state, (content_request){.node = node,
                                     .primitive_kind = SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_INTO_AS_LAST});
    }
    if (status == SXML_STATUS_OK) {
        status = kind == SXML_NODE_KIND_DOCUMENT
                     ? state->builder.operations->end_document(state->builder.state)
                     : state->builder.operations->end_element(state->builder.state);
    }
    return status;
}

/* A text, comment or processing instruction with its new value or
   target. */
static sxml_status send_leaf(const rebuild *state, sxml_node_index leaf,
                             const sxml_node_description *description) {
    sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
    sxml_status status = read_node_value(state, leaf, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_tree_builder builder = state->builder;
    switch (description->node_kind) {
    case SXML_NODE_KIND_TEXT:
        return builder.operations->add_text(
            builder.state, &(sxml_text_event){.text = value,
                                              .location = no_location,
                                              .is_cdata_section = description->is_cdata_section});
    case SXML_NODE_KIND_COMMENT:
        return builder.operations->add_comment(
            builder.state, &(sxml_comment_event){.comment = value, .location = no_location});
    case SXML_NODE_KIND_PROCESSING_INSTRUCTION: {
        node_name name = {.expanded_name = {0}, .namespace_prefix = {0}};
        status = read_node_name(state, (described_node){.index = leaf, .description = description},
                                &name);
        sxml_name_parts parts;
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_name_parts(
                sxml_item_heap_get_name_pool(state->work->request->item_heap), name.expanded_name,
                &parts);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return builder.operations->add_processing_instruction(
            builder.state, &(sxml_processing_instruction_event){
                               .target = parts.local_name, .data = value, .location = no_location});
    }
    case SXML_NODE_KIND_ATTRIBUTE:
        return send_attribute(state, leaf);
    case SXML_NODE_KIND_DOCUMENT:
    case SXML_NODE_KIND_ELEMENT:
    case SXML_NODE_KIND_NAMESPACE:
    case SXML_NODE_KIND_ANY:
        break;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* --------------------------------------------------------------- Walk */

static sxml_status push_open(rebuild *state, sxml_node_index node) {
    state->open_nodes[state->open_count] = node;
    state->open_count += 1;
    return SXML_STATUS_OK;
}

/* One child of an open node (3.2.2 order, see the file comment). Writes
   whether it was opened (an element whose children come next). */
static sxml_status send_child(rebuild *state, described_node node, bool *is_opened) {
    const sxml_node_index child = node.index;
    const sxml_node_description *description = node.description;
    *is_opened = false;
    sxml_status status = send_primitive_content(
        state, (content_request){.node = child,
                                 .primitive_kind = SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_BEFORE});
    const sxml_xquery_update_primitive *replacement =
        find_primitive(state, child, SXML_XQUERY_UPDATE_PRIMITIVE_REPLACE_NODE);
    const bool is_deleted =
        find_primitive(state, child, SXML_XQUERY_UPDATE_PRIMITIVE_DELETE) != NULL;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (replacement != NULL) {
        status = send_content(state, replacement);
    } else if (!is_deleted && description->node_kind == SXML_NODE_KIND_ELEMENT) {
        bool is_closed = false;
        status = start_element(state, node, &is_closed);
        if (status == SXML_STATUS_OK && !is_closed) {
            *is_opened = true;
            return push_open(state, child);
        }
    } else if (!is_deleted) {
        status = send_leaf(state, child, description);
    }
    if (status == SXML_STATUS_OK) {
        status = send_primitive_content(
            state, (content_request){.node = child,
                                     .primitive_kind = SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_AFTER});
    }
    return status;
}

/* Rebuilds the children of the open root, then closes it: the pass over
   the old tree, the open elements on state's stack. */
static sxml_status send_children(rebuild *state, const sxml_node_description *root) {
    bool has_next = root->has_children;
    sxml_node_index next = root->first_child;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK) {
        if (!has_next) {
            /* The last child of the innermost open node was sent. */
            state->open_count -= 1;
            const sxml_node_index closed = state->open_nodes[state->open_count];
            sxml_node_description description;
            status = sxml_node_describe(old_node(state, closed), &description);
            if (status == SXML_STATUS_OK) {
                status = close_node(state, closed, description.node_kind);
            }
            if (status != SXML_STATUS_OK || state->open_count == 0) {
                break;
            }
            status = send_primitive_content(
                state,
                (content_request){.node = closed,
                                  .primitive_kind = SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_AFTER});
            has_next = description.has_next_sibling;
            next = description.next_sibling;
            continue;
        }
        sxml_node_description description;
        status = sxml_node_describe(old_node(state, next), &description);
        bool is_opened = false;
        if (status == SXML_STATUS_OK) {
            status = send_child(state, (described_node){.index = next, .description = &description},
                                &is_opened);
        }
        if (is_opened) {
            has_next = description.has_children;
            next = description.first_child;
        } else {
            has_next = description.has_next_sibling;
            next = description.next_sibling;
        }
    }
    return status;
}

/* Rebuilds the tree from its root: a document, an element, or a
   parentless leaf. */
static sxml_status send_tree(rebuild *state) {
    const sxml_node_index root = {.index = 0};
    sxml_node_description description;
    sxml_status status = sxml_node_describe(old_node(state, root), &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description.node_kind == SXML_NODE_KIND_DOCUMENT) {
        sxml_tree_description tree;
        status = sxml_tree_describe(state->edit->tree, &tree);
        if (status == SXML_STATUS_OK) {
            status = state->builder.operations->start_document(
                state->builder.state,
                &(sxml_document_start_event){.document_uri = tree.document_uri,
                                             .base_uri = tree.base_uri,
                                             .location = no_location});
        }
        if (status == SXML_STATUS_OK) {
            status = send_primitive_content(
                state, (content_request){.node = root,
                                         .primitive_kind =
                                             SXML_XQUERY_UPDATE_PRIMITIVE_INSERT_INTO_AS_FIRST});
        }
        if (status == SXML_STATUS_OK) {
            status = push_open(state, root);
        }
        return status == SXML_STATUS_OK ? send_children(state, &description) : status;
    }
    if (description.node_kind == SXML_NODE_KIND_ELEMENT) {
        bool is_closed = false;
        status = start_element(state, (described_node){.index = root, .description = &description},
                               &is_closed);
        if (status == SXML_STATUS_OK && !is_closed) {
            status = push_open(state, root);
            if (status == SXML_STATUS_OK) {
                status = send_children(state, &description);
            }
        }
        return status;
    }
    return send_leaf(state, root, &description);
}

/* The builder's errors of the data model are XUDY0021 here (3.2.2). */
static sxml_status map_builder_error(const application *work, sxml_status status,
                                     const sxml_error_record *builder_error) {
    if (status != SXML_STATUS_DYNAMIC_ERROR) {
        return status;
    }
    if (sxml_byte_view_equals(builder_error->code.local_name,
                              SXML_BYTE_VIEW_FROM_LITERAL("XQDY0025")) ||
        sxml_byte_view_equals(builder_error->code.local_name,
                              SXML_BYTE_VIEW_FROM_LITERAL("XQDY0102"))) {
        return report(work, &instance_error);
    }
    /* An error of the builder's own; otherwise the record already has the
       error the rebuild reported. */
    if (work->request->error_record != NULL && builder_error->code.local_name.byte_count != 0) {
        *work->request->error_record = *builder_error;
    }
    return status;
}

/* Rebuilds one tree and writes the new root, owned, to *root. */
static sxml_status rebuild_tree(const application *work, const tree_edit *edit, sxml_item *root) {
    sxml_item_heap *item_heap = work->request->item_heap;
    sxml_tree_description tree_description;
    sxml_status status = sxml_tree_describe(edit->tree, &tree_description);
    sxml_error_record builder_error;
    memset(&builder_error, 0, sizeof builder_error);
    sxml_arena_tree *arena_tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_create(
            work->allocator,
            &(sxml_arena_tree_configuration){
                .allocator = work->allocator,
                .initial_memory = {.bytes = NULL, .byte_count = 0},
                .item_heap = item_heap,
                .maximum_node_count = 0,
                .maximum_depth = 0,
                .maximum_text_byte_count = 0,
                .keeps_locations = false,
                .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
                .is_destroyed_when_unreferenced = true,
                .error_record = &builder_error,
                .base_uri = tree_description.base_uri},
            &arena_tree);
    }
    void *stack = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(
            work->allocator,
            (sxml_array_allocation_request){.element_count = edit->node_count,
                                            .element_size = sizeof(sxml_node_index),
                                            .element_alignment = alignof(sxml_node_index)},
            &stack);
    }
    if (status == SXML_STATUS_OK) {
        rebuild state = {.work = work,
                         .edit = edit,
                         .builder = sxml_arena_tree_as_tree_builder(arena_tree),
                         .open_nodes = stack,
                         .open_count = 0};
        status = map_builder_error(work, send_tree(&state), &builder_error);
    }
    sxml_allocator_release(work->allocator, stack);
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *root = sxml_node_item_make(
        (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0});
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------- The entry */

static void release_application(application *work) {
    for (size_t tree_index = 0; tree_index < work->tree_count; tree_index += 1) {
        sxml_allocator_release(work->allocator, work->trees[tree_index].nodes);
    }
    sxml_allocator_release(work->allocator, work->trees);
    sxml_allocator_release(work->allocator, work->entries);
}

void sxml_xquery_release_updated_trees(sxml_xquery_updated_trees *updated_trees) {
    sxml_sequence_release(updated_trees->original_roots);
    sxml_sequence_release(updated_trees->updated_roots);
    updated_trees->original_roots = sxml_sequence_empty();
    updated_trees->updated_roots = sxml_sequence_empty();
}

/* Rebuilds every tree of work into the two builders. */
typedef struct root_builders {
    sxml_sequence_builder *original_roots;
    sxml_sequence_builder *updated_roots;
} root_builders;

static sxml_status rebuild_trees(const application *work, const root_builders *builders) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t tree_index = 0; status == SXML_STATUS_OK && tree_index < work->tree_count;
         tree_index += 1) {
        const tree_edit *edit = &work->trees[tree_index];
        sxml_item updated = {.representation = 0};
        status = rebuild_tree(work, edit, &updated);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_item(builders->updated_roots, updated);
            sxml_item_release(updated);
        }
        if (status == SXML_STATUS_OK) {
            const sxml_item original = sxml_node_item_make((sxml_node_reference){
                .tree = edit->tree, .node_index = {.index = 0}, .namespace_number = 0});
            status = sxml_sequence_builder_append_item(builders->original_roots, original);
            sxml_item_release(original);
        }
    }
    return status;
}

sxml_status sxml_xquery_apply_updates(const sxml_xquery_update_application *request,
                                      sxml_xquery_updated_trees *updated_trees) {
    application work = {.request = request,
                        .allocator = sxml_item_heap_as_allocator(request->item_heap),
                        .entries = NULL,
                        .entry_count = 0,
                        .trees = NULL,
                        .tree_count = 0};
    sxml_status status = read_primitives(&work);
    if (status == SXML_STATUS_OK) {
        status = check_changed_attributes(&work);
    }
    root_builders builders = {.original_roots = NULL, .updated_roots = NULL};
    const sxml_sequence_builder_configuration configuration = {.item_heap = request->item_heap};
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_create(work.allocator, &configuration,
                                              &builders.original_roots);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_create(work.allocator, &configuration,
                                              &builders.updated_roots);
    }
    if (status == SXML_STATUS_OK) {
        status = rebuild_trees(&work, &builders);
    }
    sxml_xquery_updated_trees result = {.original_roots = sxml_sequence_empty(),
                                        .updated_roots = sxml_sequence_empty()};
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builders.original_roots, &result.original_roots);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builders.updated_roots, &result.updated_roots);
    }
    sxml_sequence_builder_destroy(builders.original_roots);
    sxml_sequence_builder_destroy(builders.updated_roots);
    release_application(&work);
    if (status != SXML_STATUS_OK) {
        sxml_xquery_release_updated_trees(&result);
        return status;
    }
    *updated_trees = result;
    return SXML_STATUS_OK;
}

/* ======================================================== copy-modify */

/* copy($node, $modes): a copy of one node in a tree of its own (5.4.5),
   with the prolog's construction and copy-namespaces modes. */
static sxml_status call_copy(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence source = call->arguments.sequences[0];
    sxml_item modes_item = {.representation = 0};
    sxml_atomic_datum modes = {.datum_kind = SXML_DATUM_KIND_INTEGER, .datum_details = {.integer = 0}};
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_get_item(
        call->arguments.sequences[1], (sxml_item_request){.item_index = 0, .error_record = NULL},
        &modes_item);
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(modes_item, &modes);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_count(source, call->error_record, &item_count);
    }
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK && item_count == 1) {
        status = sxml_sequence_get_item(
            source, (sxml_item_request){.item_index = 0, .error_record = NULL}, &item);
    }
    sxml_node_reference node;
    if (status == SXML_STATUS_OK &&
        (item_count != 1 || sxml_item_get_node(item, &node) != SXML_STATUS_OK)) {
        return sxml_function_library_report(call, &copy_source_error);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_byte_view base_uri = {.bytes = NULL, .byte_count = 0};
    sxml_tree_description tree_description;
    status = sxml_tree_describe(node.tree, &tree_description);
    if (status == SXML_STATUS_OK) {
        base_uri = tree_description.base_uri;
    }
    const sxml_allocator allocator = sxml_item_heap_as_allocator(call->item_heap);
    sxml_arena_tree *arena_tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_create(
            allocator,
            &(sxml_arena_tree_configuration){
                .allocator = allocator,
                .initial_memory = {.bytes = NULL, .byte_count = 0},
                .item_heap = call->item_heap,
                .maximum_node_count = 0,
                .maximum_depth = 0,
                .maximum_text_byte_count = 0,
                .keeps_locations = false,
                .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
                .is_destroyed_when_unreferenced = true,
                .error_record = call->error_record,
                .base_uri = base_uri},
            &arena_tree);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_tree_builder_copy_node(
            sxml_arena_tree_as_tree_builder(arena_tree),
            &(sxml_copy_request){
                .node = node,
                .preserves_namespaces =
                    (modes.datum_details.integer & SXML_XQUERY_UPDATE_NO_PRESERVE_NAMESPACES) == 0,
                .inherits_namespaces =
                    (modes.datum_details.integer & SXML_XQUERY_UPDATE_NO_INHERIT_NAMESPACES) == 0,
                .preserves_type_annotations =
                    (modes.datum_details.integer & SXML_XQUERY_UPDATE_PRESERVES_TYPES) != 0});
    }
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *result = sxml_sequence_from_item(sxml_node_item_make(
        (sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0}));
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

/* The root of copy after the update, owned in *root: the updated tree's
   when its tree was rebuilt, else copy itself. */
static sxml_status find_updated_copy(const sxml_xquery_updated_trees *updated_trees,
                                     sxml_item copy, sxml_item *root) {
    sxml_node_reference copy_node;
    sxml_status status = sxml_item_get_node(copy, &copy_node);
    sxml_sequence_iterator originals = sxml_sequence_iterator_start(updated_trees->original_roots);
    *root = copy;
    for (uint64_t root_index = 0; status == SXML_STATUS_OK; root_index += 1) {
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&originals, NULL, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        sxml_node_reference original;
        if (sxml_item_get_node(step.item, &original) == SXML_STATUS_OK &&
            original.tree == copy_node.tree) {
            status = sxml_sequence_get_item(
                updated_trees->updated_roots,
                (sxml_item_request){.item_index = root_index, .error_record = NULL}, root);
            break;
        }
    }
    if (status == SXML_STATUS_OK) {
        sxml_item_retain(*root);
    }
    return status;
}

/* modify-copies($copies, $updates) (5.4.5): one array whose members are
   the copies' roots after the update, in order. */
static sxml_status call_modify_copies(const sxml_function_call *call, sxml_sequence *result) {
    const sxml_sequence copies = call->arguments.sequences[0];
    sxml_xquery_updated_trees updated_trees = {.original_roots = sxml_sequence_empty(),
                                               .updated_roots = sxml_sequence_empty()};
    sxml_status status = sxml_xquery_apply_updates(
        &(sxml_xquery_update_application){.item_heap = call->item_heap,
                                          .pending_updates = call->arguments.sequences[1],
                                          .allowed_roots = copies,
                                          .error_record = call->error_record},
        &updated_trees);
    uint64_t copy_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_count(copies, call->error_record, &copy_count);
    }
    const sxml_allocator allocator = sxml_item_heap_as_allocator(call->item_heap);
    sxml_sequence *members = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(
            allocator,
            (sxml_array_allocation_request){.element_count = (size_t)copy_count,
                                            .element_size = sizeof(sxml_sequence),
                                            .element_alignment = alignof(sxml_sequence)},
            (void **)&members);
    }
    size_t member_count = 0;
    for (uint64_t copy_index = 0; status == SXML_STATUS_OK && copy_index < copy_count;
         copy_index += 1) {
        sxml_item copy = {.representation = 0};
        status = sxml_sequence_get_item(
            copies, (sxml_item_request){.item_index = copy_index, .error_record = NULL}, &copy);
        sxml_item root = {.representation = 0};
        if (status == SXML_STATUS_OK) {
            status = find_updated_copy(&updated_trees, copy, &root);
        }
        if (status == SXML_STATUS_OK) {
            members[member_count] = sxml_sequence_from_item(root);
            member_count += 1;
        }
    }
    sxml_item array = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_array_make(
            call->item_heap, (sxml_sequence_view){.sequences = members, .sequence_count = member_count},
            &array);
    }
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(array);
    }
    for (size_t member_index = 0; member_index < member_count; member_index += 1) {
        sxml_sequence_release(members[member_index]);
    }
    sxml_allocator_release(allocator, members);
    sxml_xquery_release_updated_trees(&updated_trees);
    return status;
}

static const sxml_function_parameter copy_parameters[] = {
    SXML_REQUIRED_PARAMETER("node", "item()*"),
    SXML_REQUIRED_PARAMETER("modes", "xs:integer"),
};

static const sxml_function_parameter modify_parameters[] = {
    SXML_REQUIRED_PARAMETER("copies", "node()*"),
    SXML_REQUIRED_PARAMETER("updates", "item()*"),
};

const sxml_native_function sxml_xquery_copy_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    /* Every call makes new nodes. */
    .properties = SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("copy"),
    .parameters = {copy_parameters, 2},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("node()"),
    .call = call_copy,
    .resumable_body = NULL};

const sxml_native_function sxml_xquery_modify_copies_function = {
    .function_namespace = SXML_NAMESPACE_NONE,
    .properties = SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC,
    .local_name = SXML_BYTE_VIEW_INITIALIZER("modify-copies"),
    .parameters = {modify_parameters, 2},
    .result_type = SXML_BYTE_VIEW_INITIALIZER("array(node())"),
    .call = call_modify_copies,
    .resumable_body = NULL};
