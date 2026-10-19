#pragma once

/*
 * The internal interface of the tree module (include/sxml/tree.h;
 * docs/DECISIONS.md D39, D40 and D67): the layout of sxml_tree, which the
 * arena tree embeds, and the helpers src/tree.c shares with the axes of
 * src/tree_axis.c.
 *
 * Files
 * -----
 * - src/tree.c: the tree wrapper (lifecycle, reference count, identifier),
 *   node accessors with namespace nodes, document order, and
 *   sxml_tree_builder_copy_node.
 * - src/tree_axis.c: the axis iterator (XPath 4.0, section 4.7.5).
 * - src/arena_tree.c: the built-in tree and its tree builder (D40).
 *
 * Thread safety: everything here follows D35: one thread per item heap.
 */
#include "sxml/tree.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The tree wrapper. Its layout is internal; the arena tree places one inside
 * its own memory, sized by sxml_tree_memory_requirements.
 */
struct sxml_tree {
    /* The implementation, as configured. */
    void *state;
    const sxml_tree_operations *operations;
    sxml_item_heap *item_heap;
    sxml_tree_owner owner;
    sxml_tree_identifier identifier;
    /* From describe_tree at initialize: trees are immutable, so the node
       count and name pool never change. */
    uint32_t node_count;
    sxml_name_pool *name_pool;
    /* References, or SXML_TREE_IMMORTAL_REFERENCE_COUNT. */
    size_t reference_count;
    /* The allocator sxml_tree_create used; the null allocator for a tree
       placed by sxml_tree_initialize. */
    sxml_allocator owning_allocator;
};

/* The reference count of an immortal tree: retain and release leave it
   unchanged (D33). */
#define SXML_TREE_IMMORTAL_REFERENCE_COUNT SIZE_MAX

/* The node count of tree, cached at initialize. */
static inline uint32_t sxml_tree_get_node_count(const sxml_tree *tree) {
    return tree->node_count;
}

/* A node index as a plain number; makes index arithmetic read better. */
static inline sxml_node_index sxml_node_index_from_number(uint32_t number) {
    return (sxml_node_index){.index = number};
}

/* The reference to a stored node (namespace number 0) of tree. */
typedef struct sxml_stored_node_request {
    sxml_tree *tree;
    uint32_t node_number;
} sxml_stored_node_request;

static inline sxml_node_reference sxml_node_reference_of_stored_node(
    sxml_stored_node_request request) {
    return (sxml_node_reference){
        .tree = request.tree,
        .node_index = sxml_node_index_from_number(request.node_number),
        .namespace_number = 0,
    };
}

/* ------------------------------------------------------------ Axis walks */

/*
 * An axis walk (glossary `axis_walk`, D224): the nodes of one axis from an
 * origin, as sxml_axis_iterator gives them, each handed out with its
 * description, so a caller that tests nodes describes each node once.
 * The child, following-sibling, descendant, descendant-or-self,
 * attribute, self and parent axes are walked from the descriptions
 * themselves (one description per node, D39's index order); the other
 * axes run an sxml_axis_iterator and describe the nodes it gives. A plain
 * value: copy it freely, it holds no memory.
 */
typedef struct sxml_axis_walk {
    sxml_node_reference origin;
    sxml_axis axis;
    /* The generic axes. */
    bool is_iterator_walk;
    sxml_axis_iterator iterator;
    /* The walked axes: the node to hand out next, if any. */
    bool has_next_node;
    sxml_node_reference next_node;
    /* The descendant axes: the last node number of the walk. */
    uint32_t last_number;
} sxml_axis_walk;

/* A node of an axis walk and its description. */
typedef struct sxml_described_node {
    bool has_node;
    sxml_node_reference node;
    sxml_node_description description;
} sxml_described_node;

/*
 * Starts a walk over request.axis from request.origin. Returns
 * SXML_STATUS_INVALID_ARGUMENT for a request sxml_axis_iterator_start
 * refuses, and the failures of sxml_node_describe.
 */
SXML_NODISCARD sxml_status sxml_axis_walk_start(sxml_axis_request request,
                                                sxml_axis_walk *walk);

/*
 * Moves walk (in/out) to the next node of its axis and writes it with its
 * description to *node, or sets node->has_node false at the end. Returns
 * the failures of the tree implementation; on failure walk is untouched.
 */
SXML_NODISCARD sxml_status sxml_axis_walk_next(sxml_axis_walk *walk,
                                               sxml_described_node *node);
