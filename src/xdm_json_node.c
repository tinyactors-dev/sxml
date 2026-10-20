/*
 * JNodes (include/sxml/xdm.h; D33, D39, D60) and the generic node
 * operations over XNodes and JNodes: identity and document order.
 *
 * Golden file: src/xdm_function.c (a cell class of its own, failure
 * paths), after src/memory.c.
 *
 * A JNode is a cell holding its parent (retained), its key and value
 * (retained), its position among its parent's children, its depth and
 * the identifier of its JTree. Children are made when navigation asks for
 * them; making one twice gives two cells, which are the same node because
 * identity is decided by parent and key (here: by the child positions
 * from the root, which select the keys). Releasing a deep chain of JNodes
 * goes through the heap's pending list, in constant C stack.
 *
 * Specification: XDM 4.0, section 8.5 (JNodes: ·jparent·, ·jkey·,
 * ·jvalue·, dm:j-children; identity), section 6.2 (document order) and
 * F&O 4.0, section 16.1.1 (fn:jtree).
 */
#include "xdm_internal.h"

#include <stdalign.h>

/* ------------------------------------------------------------ JNode cells */

typedef struct sxml_json_node_cell {
    sxml_cell cell;
    /* SXML_ITEM_REPRESENTATION_NONE for a root. */
    sxml_item parent;
    /* Not an item for a root. */
    sxml_item json_key;
    sxml_sequence json_value;
    uint64_t child_index;
    uint64_t child_count;
    /* 0 for a root, the parent's depth plus 1 otherwise. */
    uint64_t depth;
    sxml_tree_identifier json_tree;
} sxml_json_node_cell;

static sxml_json_node_cell *sxml_json_node_cell_from_cell(sxml_cell *cell) {
    return (sxml_json_node_cell *)cell;
}

static const sxml_json_node_cell *
sxml_json_node_cell_of_item(sxml_item json_node) {
    return sxml_json_node_cell_from_cell(sxml_item_get_cell(json_node));
}

static bool sxml_item_is_json_node(sxml_item item) {
    return sxml_item_get_representation(item) ==
           SXML_ITEM_REPRESENTATION_JSON_NODE_CELL;
}

static void sxml_json_node_cell_release_contents(sxml_cell *cell) {
    sxml_json_node_cell *json_node = sxml_json_node_cell_from_cell(cell);
    sxml_sequence_release(json_node->json_value);
    sxml_item_release(json_node->json_key);
    sxml_item_release(json_node->parent);
}

static const sxml_cell_class sxml_json_node_cell_class = {
    .class_name = SXML_BYTE_VIEW_INITIALIZER("json node"),
    .release_contents = sxml_json_node_cell_release_contents,
    .sequence_operations = NULL,
};

/* ---------------------------------------------------- Children of values */

/* What the children of a JNode are, by its ·jvalue· (dm:j-children, XDM
   4.0, section 8.5). */
typedef enum sxml_json_value_shape {
    /* No children: the empty sequence, or one item that is neither a map
       nor an array. */
    SXML_JSON_VALUE_SHAPE_LEAF,
    /* One map: a child per entry, keyed by the entry's key. */
    SXML_JSON_VALUE_SHAPE_MAP,
    /* One array: a child per member, keyed by its position from 1. */
    SXML_JSON_VALUE_SHAPE_ARRAY,
    /* Two or more items: a child per item, keyed by its position from 1. */
    SXML_JSON_VALUE_SHAPE_SEQUENCE,
} sxml_json_value_shape;

/* The shape of a value with its number of children, and the single item
   for a map or an array (borrowed). */
typedef struct sxml_json_value_children {
    sxml_json_value_shape shape;
    sxml_item single_item;
    uint64_t child_count;
} sxml_json_value_children;

/* Classifies value. Counting reads a lazy sequence to its end, which may
   fail with its generator's error. */
static sxml_status
sxml_json_value_find_children(sxml_sequence value,
                              sxml_json_value_children *children) {
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(value, NULL, &item_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_json_value_children found = {
        .shape = SXML_JSON_VALUE_SHAPE_LEAF,
        .single_item = {.representation = SXML_ITEM_REPRESENTATION_NONE},
        .child_count = 0};
    if (item_count >= 2) {
        found.shape = SXML_JSON_VALUE_SHAPE_SEQUENCE;
        found.child_count = item_count;
    } else if (item_count == 1) {
        status = sxml_sequence_get_item(
            value, (sxml_item_request){.item_index = 0, .error_record = NULL},
            &found.single_item);
        size_t size = 0;
        if (status == SXML_STATUS_OK &&
            sxml_item_get_kind(found.single_item) == SXML_ITEM_KIND_MAP) {
            found.shape = SXML_JSON_VALUE_SHAPE_MAP;
            status = sxml_map_get_size(found.single_item, &size);
        } else if (status == SXML_STATUS_OK &&
                   sxml_item_get_kind(found.single_item) ==
                       SXML_ITEM_KIND_ARRAY) {
            found.shape = SXML_JSON_VALUE_SHAPE_ARRAY;
            status = sxml_array_get_size(found.single_item, &size);
        }
        found.child_count = size;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *children = found;
    return SXML_STATUS_OK;
}

/* The key and value of one child, borrowed from the parent's value except
   for inline keys. */
typedef struct sxml_json_child_entry {
    sxml_item json_key;
    sxml_sequence json_value;
} sxml_json_child_entry;

/* The position from 1 of child child_index, as the key under an array or a
   sequence. Positions beyond int64_t cannot occur: no sequence or array
   holds that many items in memory. */
static sxml_item sxml_json_position_key(uint64_t child_index) {
    return sxml_atomic_item_from_integer((int64_t)(child_index + 1));
}

/* A child to find in a value. */
typedef struct sxml_json_entry_request {
    sxml_sequence json_value;
    const sxml_json_value_children *children;
    uint64_t child_index;
} sxml_json_entry_request;

static sxml_status
sxml_json_value_get_child_entry(const sxml_json_entry_request *request,
                                sxml_json_child_entry *entry) {
    sxml_status status = SXML_STATUS_OK;
    switch (request->children->shape) {
    case SXML_JSON_VALUE_SHAPE_MAP: {
        sxml_map_entry map_entry;
        status = sxml_map_get_entry(request->children->single_item,
                                    (size_t)request->child_index, &map_entry);
        if (status == SXML_STATUS_OK) {
            *entry = (sxml_json_child_entry){.json_key = map_entry.key,
                                             .json_value = map_entry.value};
        }
        return status;
    }
    case SXML_JSON_VALUE_SHAPE_ARRAY: {
        sxml_sequence member = sxml_sequence_empty();
        status = sxml_array_get_member(request->children->single_item,
                                       (size_t)request->child_index, &member);
        if (status == SXML_STATUS_OK) {
            *entry = (sxml_json_child_entry){
                .json_key = sxml_json_position_key(request->child_index),
                .json_value = member};
        }
        return status;
    }
    case SXML_JSON_VALUE_SHAPE_SEQUENCE: {
        sxml_item item = {.representation = SXML_ITEM_REPRESENTATION_NONE};
        status = sxml_sequence_get_item(
            request->json_value,
            (sxml_item_request){.item_index = request->child_index,
                                .error_record = NULL},
            &item);
        if (status == SXML_STATUS_OK) {
            *entry = (sxml_json_child_entry){
                .json_key = sxml_json_position_key(request->child_index),
                .json_value = sxml_sequence_from_item(item)};
        }
        return status;
    }
    case SXML_JSON_VALUE_SHAPE_LEAF:
        break;
    }
    return SXML_STATUS_INDEX_OUT_OF_RANGE;
}

/* ---------------------------------------------------------- Making JNodes */

/* A JNode to make: everything but its child count, which comes from its
   value. Parent, key and value are borrowed; the cell retains them. */
typedef struct sxml_json_node_request {
    sxml_item parent;
    sxml_item json_key;
    sxml_sequence json_value;
    uint64_t child_index;
    uint64_t depth;
    sxml_tree_identifier json_tree;
} sxml_json_node_request;

static sxml_status
sxml_json_node_make(sxml_item_heap *item_heap,
                    const sxml_json_node_request *request,
                    sxml_item *json_node) {
    sxml_json_value_children children;
    sxml_status status =
        sxml_json_value_find_children(request->json_value, &children);
    sxml_cell *cell = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_allocate_cell(
            item_heap,
            &(sxml_cell_request){.cell_class = &sxml_json_node_cell_class,
                                 .byte_count = sizeof(sxml_json_node_cell),
                                 .alignment = alignof(sxml_json_node_cell)},
            &cell);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_json_node_cell *json_node_cell = sxml_json_node_cell_from_cell(cell);
    json_node_cell->parent = request->parent;
    json_node_cell->json_key = request->json_key;
    json_node_cell->json_value = request->json_value;
    json_node_cell->child_index = request->child_index;
    json_node_cell->child_count = children.child_count;
    json_node_cell->depth = request->depth;
    json_node_cell->json_tree = request->json_tree;
    sxml_item_retain(request->parent);
    sxml_item_retain(request->json_key);
    sxml_sequence_retain(request->json_value);
    *json_node = sxml_item_from_cell(&(sxml_cell_item_request){
        .representation = SXML_ITEM_REPRESENTATION_JSON_NODE_CELL,
        .type_annotation = sxml_builtin_type_identifier(SXML_TYPE_NONE),
        .primitive_type = sxml_builtin_type_identifier(SXML_TYPE_NONE),
        .cell = cell});
    return SXML_STATUS_OK;
}

sxml_status sxml_json_node_make_root(sxml_item_heap *item_heap,
                                     sxml_sequence value,
                                     sxml_item *json_node) {
    /* The identifier is issued only once the root is sure to exist, so a
       failure uses none; a root gets its own, which gives it its own
       identity and its JTree a place in document order (D39). */
    sxml_item root = {.representation = SXML_ITEM_REPRESENTATION_NONE};
    sxml_status status = sxml_json_node_make(
        item_heap,
        &(sxml_json_node_request){
            .parent = {.representation = SXML_ITEM_REPRESENTATION_NONE},
            .json_key = {.representation = SXML_ITEM_REPRESENTATION_NONE},
            .json_value = value,
            .child_index = 0,
            .depth = 0,
            .json_tree = {.value = 0}},
        &root);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_json_node_cell_from_cell(sxml_item_get_cell(root))->json_tree =
        sxml_item_heap_issue_tree_identifier(item_heap);
    *json_node = root;
    return SXML_STATUS_OK;
}

sxml_status sxml_json_node_make_keyed_root(sxml_item_heap *item_heap,
                                           sxml_json_keyed_root request,
                                           sxml_item *json_node) {
    sxml_item root = {.representation = SXML_ITEM_REPRESENTATION_NONE};
    sxml_status status = sxml_json_node_make(
        item_heap,
        &(sxml_json_node_request){
            .parent = {.representation = SXML_ITEM_REPRESENTATION_NONE},
            .json_key = request.json_key,
            .json_value = request.json_value,
            .child_index = 0,
            .depth = 0,
            .json_tree = {.value = 0}},
        &root);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_json_node_cell_from_cell(sxml_item_get_cell(root))->json_tree =
        sxml_item_heap_issue_tree_identifier(item_heap);
    *json_node = root;
    return SXML_STATUS_OK;
}

sxml_status sxml_json_node_describe(sxml_item json_node,
                                    sxml_json_node_description *description) {
    if (!sxml_item_is_json_node(json_node)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_json_node_cell *cell = sxml_json_node_cell_of_item(json_node);
    *description = (sxml_json_node_description){
        .has_parent = cell->depth > 0,
        .parent = cell->parent,
        .json_key = cell->json_key,
        .json_value = cell->json_value,
        .child_index = cell->child_index,
        .child_count = cell->child_count,
        .json_tree = cell->json_tree,
    };
    return SXML_STATUS_OK;
}

sxml_status sxml_json_node_make_child(sxml_item_heap *item_heap,
                                      sxml_json_child_request request,
                                      sxml_item *child) {
    if (!sxml_item_is_json_node(request.json_node)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_json_node_cell *parent =
        sxml_json_node_cell_of_item(request.json_node);
    if (request.child_index >= parent->child_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    sxml_json_value_children children;
    sxml_status status =
        sxml_json_value_find_children(parent->json_value, &children);
    sxml_json_child_entry entry;
    if (status == SXML_STATUS_OK) {
        status = sxml_json_value_get_child_entry(
            &(sxml_json_entry_request){.json_value = parent->json_value,
                                       .children = &children,
                                       .child_index = request.child_index},
            &entry);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_json_node_make(
        item_heap,
        &(sxml_json_node_request){.parent = request.json_node,
                                  .json_key = entry.json_key,
                                  .json_value = entry.json_value,
                                  .child_index = request.child_index,
                                  .depth = parent->depth + 1,
                                  .json_tree = parent->json_tree},
        child);
}

sxml_status sxml_json_node_make_item_node(sxml_item_heap *item_heap,
                                          sxml_item origin,
                                          sxml_item *json_node) {
    if (!sxml_item_is_json_node(origin)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_json_node_cell *parent = sxml_json_node_cell_of_item(origin);
    return sxml_json_node_make(
        item_heap,
        &(sxml_json_node_request){.parent = origin,
                                  .json_key = sxml_json_position_key(0),
                                  .json_value = parent->json_value,
                                  .child_index = parent->child_count,
                                  .depth = parent->depth + 1,
                                  .json_tree = parent->json_tree},
        json_node);
}

/* ---------------------------------------------- Identity and document order */

/* The ancestor of json_node at depth (at most its own depth). */
static const sxml_json_node_cell *
sxml_json_node_cell_climb(const sxml_json_node_cell *json_node,
                          uint64_t depth) {
    const sxml_json_node_cell *ancestor = json_node;
    while (ancestor->depth > depth) {
        ancestor = sxml_json_node_cell_of_item(ancestor->parent);
    }
    return ancestor;
}

/* The order of two numbers. */
static sxml_ordering sxml_ordering_of_numbers(uint64_t first,
                                              uint64_t second) {
    if (first < second) {
        return SXML_ORDERING_LESS;
    }
    return first > second ? SXML_ORDERING_GREATER : SXML_ORDERING_EQUAL;
}

/* The place of a JNode among its parent's: 0 for the JNode of the item
   axis (child_index past the parent's children), which comes directly
   after its parent, before the parent's children; the child_index plus 1
   for a child. */
static uint64_t sxml_json_node_place(const sxml_json_node_cell *json_node) {
    const sxml_json_node_cell *parent = sxml_json_node_cell_of_item(json_node->parent);
    return json_node->child_index >= parent->child_count ? 0 : json_node->child_index + 1;
}

/* Two JNodes. */
typedef struct sxml_json_node_pair {
    const sxml_json_node_cell *first;
    const sxml_json_node_cell *second;
} sxml_json_node_pair;

/*
 * Document order of two JNodes of one JTree (XDM 4.0, section 6.2): by
 * their child positions from the root, a node before its descendants.
 * Within one parent the child positions select distinct keys, so the
 * paths of positions identify nodes: equal paths are the same node (XDM
 * 4.0, section 8.5, parent and key). O(depth), no memory.
 */
static sxml_ordering
sxml_json_node_compare_in_tree(sxml_json_node_pair pair) {
    uint64_t common_depth = pair.first->depth < pair.second->depth
                                ? pair.first->depth
                                : pair.second->depth;
    const sxml_json_node_cell *first =
        sxml_json_node_cell_climb(pair.first, common_depth);
    const sxml_json_node_cell *second =
        sxml_json_node_cell_climb(pair.second, common_depth);
    /* Walking up, the last positions that differ are those nearest the
       root, under the deepest common ancestor. */
    sxml_ordering ordering = SXML_ORDERING_EQUAL;
    while (first->depth > 0) {
        if (first->child_index != second->child_index) {
            ordering = sxml_ordering_of_numbers(sxml_json_node_place(first),
                                                sxml_json_node_place(second));
        }
        first = sxml_json_node_cell_of_item(first->parent);
        second = sxml_json_node_cell_of_item(second->parent);
    }
    if (ordering != SXML_ORDERING_EQUAL) {
        return ordering;
    }
    /* One is an ancestor-or-self of the other. */
    return sxml_ordering_of_numbers(pair.first->depth, pair.second->depth);
}

/* The identifier of the tree or JTree of a generic node. */
static sxml_tree_identifier sxml_generic_node_get_tree(sxml_item node) {
    if (sxml_item_is_json_node(node)) {
        return sxml_json_node_cell_of_item(node)->json_tree;
    }
    return sxml_tree_get_identifier(node.payload.tree);
}

/* True for two XNodes of one tree: their order is that of (node index,
   namespace number), as sxml_node_compare_document_order has it, read
   from the items without making node references. */
static bool sxml_items_are_nodes_of_one_tree(sxml_item_pair pair) {
    return pair.first.representation == (uint8_t)SXML_ITEM_REPRESENTATION_NODE &&
           pair.second.representation == (uint8_t)SXML_ITEM_REPRESENTATION_NODE &&
           pair.first.payload.tree == pair.second.payload.tree;
}

bool sxml_generic_node_is_same_node(sxml_item_pair pair) {
    if (sxml_items_are_nodes_of_one_tree(pair)) {
        return pair.first.type_or_node_index == pair.second.type_or_node_index &&
               pair.first.namespace_number == pair.second.namespace_number;
    }
    if (!sxml_item_is_generic_node(pair.first) ||
        !sxml_item_is_generic_node(pair.second) ||
        pair.first.representation != pair.second.representation) {
        return false;
    }
    sxml_ordering ordering = SXML_ORDERING_LESS;
    sxml_status status =
        sxml_generic_node_compare_document_order(pair, &ordering);
    return status == SXML_STATUS_OK && ordering == SXML_ORDERING_EQUAL;
}

sxml_status sxml_generic_node_compare_document_order(sxml_item_pair pair,
                                                     sxml_ordering *ordering) {
    if (sxml_items_are_nodes_of_one_tree(pair)) {
        *ordering = pair.first.type_or_node_index != pair.second.type_or_node_index
                        ? sxml_ordering_of_numbers(pair.first.type_or_node_index,
                                                   pair.second.type_or_node_index)
                        : sxml_ordering_of_numbers(pair.first.namespace_number,
                                                   pair.second.namespace_number);
        return SXML_STATUS_OK;
    }
    if (!sxml_item_is_generic_node(pair.first) ||
        !sxml_item_is_generic_node(pair.second)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* XDM 4.0, section 6.2: nodes of different trees, XTrees and JTrees
       alike, by the order their identifiers were issued (D39). XTrees and
       JTrees never share an identifier. */
    sxml_tree_identifier first_tree = sxml_generic_node_get_tree(pair.first);
    sxml_tree_identifier second_tree = sxml_generic_node_get_tree(pair.second);
    if (first_tree.value != second_tree.value) {
        *ordering = sxml_ordering_of_numbers(first_tree.value,
                                             second_tree.value);
        return SXML_STATUS_OK;
    }
    if (sxml_item_is_json_node(pair.first)) {
        *ordering = sxml_json_node_compare_in_tree((sxml_json_node_pair){
            .first = sxml_json_node_cell_of_item(pair.first),
            .second = sxml_json_node_cell_of_item(pair.second)});
        return SXML_STATUS_OK;
    }
    sxml_node_reference first = {.tree = NULL};
    sxml_node_reference second = {.tree = NULL};
    sxml_status status = sxml_item_get_node(pair.first, &first);
    if (status == SXML_STATUS_OK) {
        status = sxml_item_get_node(pair.second, &second);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *ordering = sxml_node_compare_document_order(
        (sxml_node_pair){.first = first, .second = second});
    return SXML_STATUS_OK;
}
