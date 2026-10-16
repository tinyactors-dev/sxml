/*
 * Arrays (include/sxml/xdm.h; D33, D37, D60): persistent arrays of member
 * sequences with O(log n) append, put, insert, remove and slice, and O(1)
 * member access on arrays made in one piece (D64).
 *
 * Golden file: src/xdm.c (cells, cell classes, failure paths), after
 * src/memory.c.
 *
 * Specification: XDM 4.0, section 8.4 (arrays: an ordered list of members,
 * each a sequence; dm:array-append, dm:array-get); F&O 4.0, section 18
 * (array:put, array:insert-before, array:remove, array:subarray).
 *
 * Representation
 * --------------
 * An array is a balanced binary tree of array nodes, each a cell (glossary:
 * array_node). An array item's cell is the root node; trees share subtrees
 * between arrays, so an update copies only O(log n) nodes.
 *
 * - A flat node holds its members inline. sxml_array_make makes one flat
 *   node, so member access on such an array is O(1).
 * - A run node names a contiguous part of a flat node's members (always a
 *   flat node, never another run), so cutting a leaf in two copies no
 *   members.
 * - A branch node holds a left and a right subtree.
 *
 * Flat and run nodes are leaves (node height 0); a branch node is one higher
 * than its taller subtree, and the heights of its subtrees differ by at most
 * one (AVL balance), so a tree of n members has height at most about
 * 1.44 log2 n. Every update is a composition of two operations: join, which
 * concatenates two trees in time proportional to their height difference,
 * and split, which cuts a tree before a member index in O(log n) (the join
 * algorithm of Blelloch, Ferizovic and Sun, "Just Join for Parallel Ordered
 * Sets", 2016, on a tree whose members are in the leaves, so no middle key
 * is needed). Within the tree, an empty array is a NULL node; an empty array
 * item is a flat node with no members.
 *
 * Ownership inside this file follows D33: functions borrow the nodes they
 * are given and write owned nodes; on failure they release what they made
 * and leave their outputs untouched. Releasing a tree goes through
 * sxml_cell_release, which frees cells from the heap's pending list without
 * recursion, so arrays nested arbitrarily deep are freed in constant stack.
 * Join and split walk the tree in loops over fixed-size paths (D71).
 */
#include "xdm_internal.h"

#include <stdalign.h>

/* ----------------------------------------------------------- Array nodes */

typedef enum sxml_array_node_kind {
    SXML_ARRAY_NODE_KIND_FLAT,
    SXML_ARRAY_NODE_KIND_RUN,
    SXML_ARRAY_NODE_KIND_BRANCH,
} sxml_array_node_kind;

typedef struct sxml_array_node sxml_array_node;

/* A run node's part of a flat node's members. */
typedef struct sxml_array_run {
    /* Retained; always a flat node. */
    sxml_array_node *flat_node;
    size_t first_member_index;
} sxml_array_run;

/* A branch node's subtrees, both retained and never NULL. */
typedef struct sxml_array_branch {
    sxml_array_node *left_node;
    sxml_array_node *right_node;
} sxml_array_branch;

struct sxml_array_node {
    sxml_cell cell;
    sxml_array_node_kind node_kind;
    uint32_t node_height;
    size_t member_count;
    union {
        sxml_array_run run;
        sxml_array_branch branch;
    } shape;
    /* A flat node's members, each holding one reference; absent from other
       nodes. */
    sxml_sequence members[];
};

/* Two trees, either of which may be NULL (empty). */
typedef struct sxml_array_node_pair {
    sxml_array_node *left_node;
    sxml_array_node *right_node;
} sxml_array_node_pair;

static sxml_array_node *sxml_array_node_from_cell(sxml_cell *cell) {
    return (sxml_array_node *)cell;
}

static void sxml_array_node_retain(sxml_array_node *node) {
    if (node != NULL) {
        sxml_cell_retain(&node->cell);
    }
}

static void sxml_array_node_release(sxml_array_node *node) {
    if (node != NULL) {
        sxml_cell_release(&node->cell);
    }
}

static void sxml_array_node_pair_release(sxml_array_node_pair pair) {
    sxml_array_node_release(pair.left_node);
    sxml_array_node_release(pair.right_node);
}

static void sxml_array_node_release_contents(sxml_cell *cell) {
    sxml_array_node *node = sxml_array_node_from_cell(cell);
    switch (node->node_kind) {
    case SXML_ARRAY_NODE_KIND_FLAT:
        for (size_t member_index = 0; member_index < node->member_count;
             member_index += 1) {
            sxml_sequence_release(node->members[member_index]);
        }
        break;
    case SXML_ARRAY_NODE_KIND_RUN:
        sxml_array_node_release(node->shape.run.flat_node);
        break;
    case SXML_ARRAY_NODE_KIND_BRANCH:
        sxml_array_node_release(node->shape.branch.left_node);
        sxml_array_node_release(node->shape.branch.right_node);
        break;
    }
}

static const sxml_cell_class sxml_array_node_cell_class = {
    .class_name = SXML_BYTE_VIEW_INITIALIZER("array node"),
    .release_contents = sxml_array_node_release_contents,
    .sequence_operations = NULL,
};

/* A node to allocate: its kind and, for a flat node, its member count. */
typedef struct sxml_array_node_request {
    sxml_array_node_kind node_kind;
    size_t flat_member_count;
} sxml_array_node_request;

/* Allocates a node whose fields after the header the caller fills. */
static sxml_status sxml_array_node_allocate(sxml_item_heap *item_heap,
                                            sxml_array_node_request request,
                                            sxml_array_node **node) {
    size_t members_byte_count = 0;
    sxml_status status = sxml_size_multiply(
        request.flat_member_count, sizeof(sxml_sequence), &members_byte_count);
    size_t cell_byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_add(sizeof(sxml_array_node), members_byte_count,
                               &cell_byte_count);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_cell *cell = NULL;
    status = sxml_item_heap_allocate_cell(
        item_heap,
        &(sxml_cell_request){.cell_class = &sxml_array_node_cell_class,
                             .byte_count = cell_byte_count,
                             .alignment = alignof(sxml_array_node)},
        &cell);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_array_node *new_node = sxml_array_node_from_cell(cell);
    new_node->node_kind = request.node_kind;
    new_node->node_height = 0;
    new_node->member_count = request.flat_member_count;
    new_node->shape.branch =
        (sxml_array_branch){.left_node = NULL, .right_node = NULL};
    *node = new_node;
    return SXML_STATUS_OK;
}

/* Makes a flat node of members, retaining them. */
static sxml_status sxml_array_node_make_flat(sxml_item_heap *item_heap,
                                             sxml_sequence_view members,
                                             sxml_array_node **node) {
    sxml_array_node *flat_node = NULL;
    sxml_status status = sxml_array_node_allocate(
        item_heap,
        (sxml_array_node_request){.node_kind = SXML_ARRAY_NODE_KIND_FLAT,
                                  .flat_member_count = members.sequence_count},
        &flat_node);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    for (size_t member_index = 0; member_index < members.sequence_count;
         member_index += 1) {
        sxml_sequence member = members.sequences[member_index];
        sxml_sequence_retain(member);
        flat_node->members[member_index] = member;
    }
    *node = flat_node;
    return SXML_STATUS_OK;
}

/* A part of a leaf: member_count members from first_member_index. */
typedef struct sxml_array_leaf_part {
    sxml_array_node *leaf_node;
    size_t first_member_index;
    size_t member_count;
} sxml_array_leaf_part;

/*
 * Writes the node of part, owned, to *node: NULL for no members, the leaf
 * itself (retained) for all of them, otherwise a run node over the leaf's
 * flat node.
 */
static sxml_status sxml_array_leaf_make_part(sxml_item_heap *item_heap,
                                             const sxml_array_leaf_part *part,
                                             sxml_array_node **node) {
    sxml_array_node *leaf_node = part->leaf_node;
    if (part->member_count == 0) {
        *node = NULL;
        return SXML_STATUS_OK;
    }
    if (part->member_count == leaf_node->member_count) {
        sxml_array_node_retain(leaf_node);
        *node = leaf_node;
        return SXML_STATUS_OK;
    }
    sxml_array_run run = {.flat_node = leaf_node,
                          .first_member_index = part->first_member_index};
    if (leaf_node->node_kind == SXML_ARRAY_NODE_KIND_RUN) {
        run = (sxml_array_run){.flat_node = leaf_node->shape.run.flat_node,
                               .first_member_index =
                                   leaf_node->shape.run.first_member_index +
                                   part->first_member_index};
    }
    sxml_array_node *run_node = NULL;
    sxml_status status = sxml_array_node_allocate(
        item_heap,
        (sxml_array_node_request){.node_kind = SXML_ARRAY_NODE_KIND_RUN,
                                  .flat_member_count = 0},
        &run_node);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_array_node_retain(run.flat_node);
    run_node->member_count = part->member_count;
    run_node->shape.run = run;
    *node = run_node;
    return SXML_STATUS_OK;
}

static uint32_t sxml_array_node_get_height(const sxml_array_node *node) {
    return node->node_height;
}

/*
 * Makes the branch node of two non-empty trees, retaining both. The caller
 * guarantees their heights differ by at most one.
 */
static sxml_status sxml_array_node_make_branch(sxml_item_heap *item_heap,
                                               sxml_array_node_pair pair,
                                               sxml_array_node **node) {
    size_t member_count = 0;
    sxml_status status =
        sxml_size_add(pair.left_node->member_count,
                      pair.right_node->member_count, &member_count);
    sxml_array_node *branch_node = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_array_node_allocate(
            item_heap,
            (sxml_array_node_request){.node_kind = SXML_ARRAY_NODE_KIND_BRANCH,
                                      .flat_member_count = 0},
            &branch_node);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t left_height = sxml_array_node_get_height(pair.left_node);
    uint32_t right_height = sxml_array_node_get_height(pair.right_node);
    sxml_array_node_retain(pair.left_node);
    sxml_array_node_retain(pair.right_node);
    branch_node->member_count = member_count;
    branch_node->node_height =
        (left_height > right_height ? left_height : right_height) + 1;
    branch_node->shape.branch = (sxml_array_branch){
        .left_node = pair.left_node, .right_node = pair.right_node};
    *node = branch_node;
    return SXML_STATUS_OK;
}

/* Three non-empty trees in order, for rotations. */
typedef struct sxml_array_node_triple {
    sxml_array_node *first_node;
    sxml_array_node *second_node;
    sxml_array_node *third_node;
} sxml_array_node_triple;

/* Makes branch(branch(first, second), third), borrowing all three. */
static sxml_status
sxml_array_node_make_left_leaning(sxml_item_heap *item_heap,
                                  const sxml_array_node_triple *triple,
                                  sxml_array_node **node) {
    sxml_array_node *inner_node = NULL;
    sxml_status status = sxml_array_node_make_branch(
        item_heap,
        (sxml_array_node_pair){.left_node = triple->first_node,
                               .right_node = triple->second_node},
        &inner_node);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_array_node_make_branch(
        item_heap,
        (sxml_array_node_pair){.left_node = inner_node,
                               .right_node = triple->third_node},
        node);
    sxml_array_node_release(inner_node);
    return status;
}

/* Makes branch(first, branch(second, third)), borrowing all three. */
static sxml_status
sxml_array_node_make_right_leaning(sxml_item_heap *item_heap,
                                   const sxml_array_node_triple *triple,
                                   sxml_array_node **node) {
    sxml_array_node *inner_node = NULL;
    sxml_status status = sxml_array_node_make_branch(
        item_heap,
        (sxml_array_node_pair){.left_node = triple->second_node,
                               .right_node = triple->third_node},
        &inner_node);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_array_node_make_branch(
        item_heap,
        (sxml_array_node_pair){.left_node = triple->first_node,
                               .right_node = inner_node},
        node);
    sxml_array_node_release(inner_node);
    return status;
}

/* Four non-empty trees in order, for double rotations. */
typedef struct sxml_array_node_quadruple {
    sxml_array_node *nodes[4];
} sxml_array_node_quadruple;

/* Makes branch(branch(first, second), branch(third, fourth)). */
static sxml_status
sxml_array_node_make_even(sxml_item_heap *item_heap,
                          const sxml_array_node_quadruple *quadruple,
                          sxml_array_node **node) {
    sxml_array_node_pair halves = {.left_node = NULL, .right_node = NULL};
    sxml_status status = sxml_array_node_make_branch(
        item_heap,
        (sxml_array_node_pair){.left_node = quadruple->nodes[0],
                               .right_node = quadruple->nodes[1]},
        &halves.left_node);
    if (status == SXML_STATUS_OK) {
        status = sxml_array_node_make_branch(
            item_heap,
            (sxml_array_node_pair){.left_node = quadruple->nodes[2],
                                   .right_node = quadruple->nodes[3]},
            &halves.right_node);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_array_node_make_branch(item_heap, halves, node);
    }
    sxml_array_node_pair_release(halves);
    return status;
}

/*
 * Makes a balanced tree of the members of pair.left_node followed by those
 * of pair.right_node, both non-empty and borrowed, whose heights differ by
 * at most two: a plain branch, or a single or double rotation (the AVL
 * rebalancing cases). The result is one or two higher than the taller
 * input, or as high.
 */
static sxml_status sxml_array_node_make_balanced(sxml_item_heap *item_heap,
                                                 sxml_array_node_pair pair,
                                                 sxml_array_node **node) {
    uint32_t left_height = sxml_array_node_get_height(pair.left_node);
    uint32_t right_height = sxml_array_node_get_height(pair.right_node);
    if (right_height > left_height + 1) {
        /* Right-heavy: the right tree is a branch of height at least 2. */
        sxml_array_branch right = pair.right_node->shape.branch;
        if (sxml_array_node_get_height(right.right_node) >=
            sxml_array_node_get_height(right.left_node)) {
            return sxml_array_node_make_left_leaning(
                item_heap,
                &(sxml_array_node_triple){.first_node = pair.left_node,
                                          .second_node = right.left_node,
                                          .third_node = right.right_node},
                node);
        }
        /* The inner subtree is the taller, so it is a branch. */
        sxml_array_branch inner = right.left_node->shape.branch;
        return sxml_array_node_make_even(
            item_heap,
            &(sxml_array_node_quadruple){
                .nodes = {pair.left_node, inner.left_node, inner.right_node,
                          right.right_node}},
            node);
    }
    if (left_height > right_height + 1) {
        sxml_array_branch left = pair.left_node->shape.branch;
        if (sxml_array_node_get_height(left.left_node) >=
            sxml_array_node_get_height(left.right_node)) {
            return sxml_array_node_make_right_leaning(
                item_heap,
                &(sxml_array_node_triple){.first_node = left.left_node,
                                          .second_node = left.right_node,
                                          .third_node = pair.right_node},
                node);
        }
        sxml_array_branch inner = left.right_node->shape.branch;
        return sxml_array_node_make_even(
            item_heap,
            &(sxml_array_node_quadruple){
                .nodes = {left.left_node, inner.left_node, inner.right_node,
                          pair.right_node}},
            node);
    }
    return sxml_array_node_make_branch(item_heap, pair, node);
}

/*
 * The most nodes a walk down an array tree keeps. A tree of height h has
 * at least F(h + 3) - 1 nodes (F the Fibonacci numbers: a branch of height
 * h has subtrees of heights at least h - 1 and h - 2), and F(94) - 1 is
 * more than 2^64 - 1, so a tree of nodes that fit in any address space is
 * at most 90 high, and a path from its root has at most 91 nodes. Join and
 * split therefore keep their paths in fixed arrays on the C stack, need no
 * allocation and do not recurse (D71).
 */
#define SXML_ARRAY_TREE_PATH_CAPACITY 96

/*
 * The subtrees join leaves behind on its way down the taller tree's inner
 * spine, innermost last, each with the side of the joined part it goes
 * on.
 */
typedef struct sxml_array_join_path {
    sxml_array_node *siblings[SXML_ARRAY_TREE_PATH_CAPACITY];
    bool sibling_goes_left[SXML_ARRAY_TREE_PATH_CAPACITY];
    size_t sibling_count;
} sxml_array_join_path;

/*
 * Joins two trees (borrowed; either may be NULL) into one holding the left
 * tree's members, then the right tree's, and writes it, owned (NULL when
 * both are empty), to *node. Time and allocations are proportional to the
 * height difference plus one. The result is as high as the taller input or
 * one higher.
 *
 * While one tree is more than one higher than the other, the walk goes
 * down its inner spine (the left tree's right spine, or the right tree's
 * left spine), keeping the other subtrees; the remaining pair becomes a
 * branch, which is rebalanced with each kept subtree on the way back up
 * (each joined part is at most two higher than its sibling).
 */
static sxml_status sxml_array_node_join(sxml_item_heap *item_heap,
                                        sxml_array_node_pair pair,
                                        sxml_array_node **node) {
    if (pair.left_node == NULL || pair.right_node == NULL) {
        sxml_array_node *only_node =
            pair.left_node != NULL ? pair.left_node : pair.right_node;
        sxml_array_node_retain(only_node);
        *node = only_node;
        return SXML_STATUS_OK;
    }
    sxml_array_join_path path = {.sibling_count = 0};
    sxml_array_node_pair remaining = pair;
    for (;;) {
        uint32_t left_height = sxml_array_node_get_height(remaining.left_node);
        uint32_t right_height =
            sxml_array_node_get_height(remaining.right_node);
        bool is_left_taller = left_height > right_height + 1;
        if (!is_left_taller && right_height <= left_height + 1) {
            break;
        }
        if (path.sibling_count == SXML_ARRAY_TREE_PATH_CAPACITY) {
            /* Not reached: see SXML_ARRAY_TREE_PATH_CAPACITY. */
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        path.sibling_goes_left[path.sibling_count] = is_left_taller;
        if (is_left_taller) {
            sxml_array_branch left = remaining.left_node->shape.branch;
            path.siblings[path.sibling_count] = left.left_node;
            remaining.left_node = left.right_node;
        } else {
            sxml_array_branch right = remaining.right_node->shape.branch;
            path.siblings[path.sibling_count] = right.right_node;
            remaining.right_node = right.left_node;
        }
        path.sibling_count += 1;
    }
    sxml_array_node *joined_node = NULL;
    sxml_status status =
        sxml_array_node_make_branch(item_heap, remaining, &joined_node);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    while (path.sibling_count > 0) {
        path.sibling_count -= 1;
        sxml_array_node *sibling = path.siblings[path.sibling_count];
        sxml_array_node_pair balanced_pair =
            path.sibling_goes_left[path.sibling_count]
                ? (sxml_array_node_pair){.left_node = sibling,
                                         .right_node = joined_node}
                : (sxml_array_node_pair){.left_node = joined_node,
                                         .right_node = sibling};
        sxml_array_node *balanced_node = NULL;
        status = sxml_array_node_make_balanced(item_heap, balanced_pair,
                                               &balanced_node);
        sxml_array_node_release(joined_node);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        joined_node = balanced_node;
    }
    *node = joined_node;
    return SXML_STATUS_OK;
}

/* Where to split a tree. */
typedef struct sxml_array_split_request {
    /* Borrowed; may be NULL only when member_index is 0. */
    sxml_array_node *node;
    /* At most the node's member count. */
    size_t member_index;
} sxml_array_split_request;

/* The branches split passes on its way down, each with the side it goes
   down by. */
typedef struct sxml_array_split_path {
    const sxml_array_node *branches[SXML_ARRAY_TREE_PATH_CAPACITY];
    bool goes_left[SXML_ARRAY_TREE_PATH_CAPACITY];
    size_t branch_count;
} sxml_array_split_path;

/*
 * Splits a leaf (or a tree at either end) before member_index and writes
 * the two trees, owned (either may be NULL), to *pair.
 */
static sxml_status
sxml_array_node_split_at_bottom(sxml_item_heap *item_heap,
                                const sxml_array_split_request *request,
                                sxml_array_node_pair *pair) {
    sxml_array_node *node = request->node;
    size_t member_index = request->member_index;
    if (node == NULL || member_index == 0 ||
        member_index == node->member_count) {
        sxml_array_node_retain(node);
        *pair =
            member_index == 0
                ? (sxml_array_node_pair){.left_node = NULL, .right_node = node}
                : (sxml_array_node_pair){.left_node = node, .right_node = NULL};
        return SXML_STATUS_OK;
    }
    sxml_array_node_pair parts = {.left_node = NULL, .right_node = NULL};
    sxml_status status = sxml_array_leaf_make_part(
        item_heap,
        &(sxml_array_leaf_part){.leaf_node = node,
                                .first_member_index = 0,
                                .member_count = member_index},
        &parts.left_node);
    if (status == SXML_STATUS_OK) {
        status = sxml_array_leaf_make_part(
            item_heap,
            &(sxml_array_leaf_part){.leaf_node = node,
                                    .first_member_index = member_index,
                                    .member_count =
                                        node->member_count - member_index},
            &parts.right_node);
    }
    if (status != SXML_STATUS_OK) {
        sxml_array_node_pair_release(parts);
        return status;
    }
    *pair = parts;
    return SXML_STATUS_OK;
}

/*
 * Splits request->node before request->member_index and writes the two
 * trees, owned (either may be NULL), to *pair: the left holds the members
 * before the index. The walk goes down to the leaf holding the index (or
 * to a subtree the index is at an end of) and splits it; on the way back
 * up, at a branch it went left in, the right part is joined with the
 * branch's right subtree, else the branch's left subtree with the left
 * part. O(log n): the joins along the path cost a telescoping sum of
 * height differences.
 */
static sxml_status
sxml_array_node_split(sxml_item_heap *item_heap,
                      const sxml_array_split_request *request,
                      sxml_array_node_pair *pair) {
    sxml_array_split_path path = {.branch_count = 0};
    sxml_array_split_request bottom = *request;
    while (bottom.node != NULL && bottom.member_index != 0 &&
           bottom.member_index != bottom.node->member_count &&
           bottom.node->node_kind == SXML_ARRAY_NODE_KIND_BRANCH) {
        if (path.branch_count == SXML_ARRAY_TREE_PATH_CAPACITY) {
            /* Not reached: see SXML_ARRAY_TREE_PATH_CAPACITY. */
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        sxml_array_branch branch = bottom.node->shape.branch;
        size_t left_member_count = branch.left_node->member_count;
        bool goes_left = bottom.member_index <= left_member_count;
        path.branches[path.branch_count] = bottom.node;
        path.goes_left[path.branch_count] = goes_left;
        path.branch_count += 1;
        if (goes_left) {
            bottom.node = branch.left_node;
        } else {
            bottom.node = branch.right_node;
            bottom.member_index -= left_member_count;
        }
    }
    sxml_array_node_pair parts = {.left_node = NULL, .right_node = NULL};
    sxml_status status =
        sxml_array_node_split_at_bottom(item_heap, &bottom, &parts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    while (path.branch_count > 0) {
        path.branch_count -= 1;
        sxml_array_branch branch = path.branches[path.branch_count]->shape.branch;
        sxml_array_node *joined_node = NULL;
        if (path.goes_left[path.branch_count]) {
            /* The split left subtree's right part goes before the right
               subtree. */
            status = sxml_array_node_join(
                item_heap,
                (sxml_array_node_pair){.left_node = parts.right_node,
                                       .right_node = branch.right_node},
                &joined_node);
            if (status == SXML_STATUS_OK) {
                sxml_array_node_release(parts.right_node);
                parts.right_node = joined_node;
            }
        } else {
            status = sxml_array_node_join(
                item_heap,
                (sxml_array_node_pair){.left_node = branch.left_node,
                                       .right_node = parts.left_node},
                &joined_node);
            if (status == SXML_STATUS_OK) {
                sxml_array_node_release(parts.left_node);
                parts.left_node = joined_node;
            }
        }
        if (status != SXML_STATUS_OK) {
            sxml_array_node_pair_release(parts);
            return status;
        }
    }
    *pair = parts;
    return SXML_STATUS_OK;
}

/* The member at member_index (below the node's member count), borrowed. */
static sxml_sequence sxml_array_node_get_member(const sxml_array_node *node,
                                                size_t member_index) {
    while (node->node_kind == SXML_ARRAY_NODE_KIND_BRANCH) {
        const sxml_array_node *left_node = node->shape.branch.left_node;
        if (member_index < left_node->member_count) {
            node = left_node;
        } else {
            member_index -= left_node->member_count;
            node = node->shape.branch.right_node;
        }
    }
    if (node->node_kind == SXML_ARRAY_NODE_KIND_RUN) {
        member_index += node->shape.run.first_member_index;
        node = node->shape.run.flat_node;
    }
    return node->members[member_index];
}

/* ----------------------------------------------------------- Array items */

/* The root node of an array item, or NULL when item is not an array. */
static sxml_array_node *sxml_array_get_root(sxml_item array) {
    if (sxml_item_get_representation(array) !=
        SXML_ITEM_REPRESENTATION_ARRAY_CELL) {
        return NULL;
    }
    return sxml_array_node_from_cell(sxml_item_get_cell(array));
}

/* The tree of an array's members: NULL for the empty array. */
static sxml_array_node *sxml_array_get_tree(sxml_array_node *root_node) {
    return root_node->member_count == 0 ? NULL : root_node;
}

/*
 * Writes the array item whose root is node (owned, taken over; NULL for the
 * empty array, which gets a flat node of no members) to *array.
 */
static sxml_status sxml_array_item_from_tree(sxml_item_heap *item_heap,
                                             sxml_array_node *node,
                                             sxml_item *array) {
    if (node == NULL) {
        sxml_status status = sxml_array_node_make_flat(
            item_heap,
            (sxml_sequence_view){.sequences = NULL, .sequence_count = 0},
            &node);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    /* XDM 4.0, section 8.4: an array's type annotation is array(*). */
    *array = sxml_item_from_cell(&(sxml_cell_item_request){
        .representation = SXML_ITEM_REPRESENTATION_ARRAY_CELL,
        .type_annotation = sxml_builtin_type_identifier(SXML_TYPE_ARRAY),
        .primitive_type = sxml_builtin_type_identifier(SXML_TYPE_NONE),
        .cell = &node->cell});
    return SXML_STATUS_OK;
}

sxml_status sxml_array_make(sxml_item_heap *item_heap,
                            sxml_sequence_view members, sxml_item *array) {
    sxml_array_node *flat_node = NULL;
    sxml_status status =
        sxml_array_node_make_flat(item_heap, members, &flat_node);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_array_item_from_tree(item_heap, flat_node, array);
}

sxml_status sxml_array_get_size(sxml_item array, size_t *member_count) {
    const sxml_array_node *root_node = sxml_array_get_root(array);
    if (root_node == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *member_count = root_node->member_count;
    return SXML_STATUS_OK;
}

sxml_status sxml_array_get_member(sxml_item array, size_t member_index,
                                  sxml_sequence *member) {
    const sxml_array_node *root_node = sxml_array_get_root(array);
    if (root_node == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (member_index >= root_node->member_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    *member = sxml_array_node_get_member(root_node, member_index);
    return SXML_STATUS_OK;
}

/*
 * The trees one update works with: every non-NULL node is owned and
 * released by sxml_array_edit_finish, whatever happened.
 */
typedef struct sxml_array_edit {
    sxml_item_heap *item_heap;
    /* The members before the edited place, the new member's leaf, and the
       members after it. */
    sxml_array_node_pair outer;
    sxml_array_node *member_node;
    /* What split or join steps produced on the way. */
    sxml_array_node_pair scratch;
    sxml_array_node *joined_node;
} sxml_array_edit;

static sxml_array_edit sxml_array_edit_start(sxml_item_heap *item_heap) {
    return (sxml_array_edit){
        .item_heap = item_heap,
        .outer = {.left_node = NULL, .right_node = NULL},
        .member_node = NULL,
        .scratch = {.left_node = NULL, .right_node = NULL},
        .joined_node = NULL,
    };
}

/* Makes edit->member_node, the leaf of the single member. */
static sxml_status sxml_array_edit_make_member(sxml_array_edit *edit,
                                               sxml_sequence member) {
    return sxml_array_node_make_flat(
        edit->item_heap,
        (sxml_sequence_view){.sequences = &member, .sequence_count = 1},
        &edit->member_node);
}

/*
 * Splits root before member_index into edit->outer and, when
 * removed_member_count is 1, drops one member from the right part.
 */
typedef struct sxml_array_cut {
    sxml_array_node *tree_node;
    size_t member_index;
    size_t removed_member_count;
} sxml_array_cut;

static sxml_status sxml_array_edit_cut(sxml_array_edit *edit,
                                       const sxml_array_cut *cut) {
    sxml_status status = sxml_array_node_split(
        edit->item_heap,
        &(sxml_array_split_request){.node = cut->tree_node,
                                    .member_index = cut->member_index},
        &edit->outer);
    if (status != SXML_STATUS_OK || cut->removed_member_count == 0) {
        return status;
    }
    status = sxml_array_node_split(
        edit->item_heap,
        &(sxml_array_split_request){.node = edit->outer.right_node,
                                    .member_index = cut->removed_member_count},
        &edit->scratch);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_array_node_release(edit->outer.right_node);
    edit->outer.right_node = edit->scratch.right_node;
    edit->scratch.right_node = NULL;
    return SXML_STATUS_OK;
}

/* Joins outer.left, member_node (if any) and outer.right into
   edit->joined_node. */
static sxml_status sxml_array_edit_join(sxml_array_edit *edit) {
    sxml_array_node *left_node = edit->outer.left_node;
    sxml_array_node *prefix_node = NULL;
    sxml_status status = sxml_array_node_join(
        edit->item_heap,
        (sxml_array_node_pair){.left_node = left_node,
                               .right_node = edit->member_node},
        &prefix_node);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_array_node_join(
        edit->item_heap,
        (sxml_array_node_pair){.left_node = prefix_node,
                               .right_node = edit->outer.right_node},
        &edit->joined_node);
    sxml_array_node_release(prefix_node);
    return status;
}

/*
 * Releases everything edit holds; on success first makes the array item of
 * edit->joined_node and writes it to *array.
 */
typedef struct sxml_array_edit_outcome {
    sxml_status status;
    sxml_item *array;
} sxml_array_edit_outcome;

static sxml_status sxml_array_edit_finish(sxml_array_edit *edit,
                                          sxml_array_edit_outcome outcome) {
    sxml_status status = outcome.status;
    if (status == SXML_STATUS_OK) {
        status = sxml_array_item_from_tree(edit->item_heap, edit->joined_node,
                                           outcome.array);
        if (status == SXML_STATUS_OK) {
            edit->joined_node = NULL;
        }
    }
    sxml_array_node_pair_release(edit->outer);
    sxml_array_node_release(edit->member_node);
    sxml_array_node_pair_release(edit->scratch);
    sxml_array_node_release(edit->joined_node);
    return status;
}

/* The kinds of change sxml_array_change makes. */
typedef enum sxml_array_change_kind {
    SXML_ARRAY_CHANGE_KIND_APPEND,
    SXML_ARRAY_CHANGE_KIND_PUT,
    SXML_ARRAY_CHANGE_KIND_INSERT,
    SXML_ARRAY_CHANGE_KIND_REMOVE,
} sxml_array_change_kind;

typedef struct sxml_array_change {
    sxml_array_change_kind change_kind;
    const sxml_array_update *update;
} sxml_array_change;

/*
 * Every update is a cut, a new member (except for remove) and a join:
 * append cuts at the end, put removes one member and adds one, insert
 * adds one, remove removes one.
 */
static sxml_status sxml_array_change_apply(sxml_item_heap *item_heap,
                                           const sxml_array_change *change,
                                           sxml_item *array) {
    const sxml_array_update *update = change->update;
    sxml_array_node *root_node = sxml_array_get_root(update->array);
    if (root_node == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    size_t member_count = root_node->member_count;
    sxml_array_cut cut = {.tree_node = sxml_array_get_tree(root_node),
                          .member_index = update->member_index,
                          .removed_member_count = 0};
    bool adds_member = true;
    switch (change->change_kind) {
    case SXML_ARRAY_CHANGE_KIND_APPEND:
        cut.member_index = member_count;
        break;
    case SXML_ARRAY_CHANGE_KIND_PUT:
        cut.removed_member_count = 1;
        break;
    case SXML_ARRAY_CHANGE_KIND_INSERT:
        break;
    case SXML_ARRAY_CHANGE_KIND_REMOVE:
        cut.removed_member_count = 1;
        adds_member = false;
        break;
    }
    /* array:put and array:remove need an existing member; insert-before
       may also insert at the end (F&O 4.0, section 18; FOAY0001 is the
       caller's to raise). */
    if (cut.member_index > member_count ||
        member_count - cut.member_index < cut.removed_member_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    sxml_array_edit edit = sxml_array_edit_start(item_heap);
    sxml_status status = SXML_STATUS_OK;
    if (adds_member) {
        status = sxml_array_edit_make_member(&edit, update->member);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_array_edit_cut(&edit, &cut);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_array_edit_join(&edit);
    }
    return sxml_array_edit_finish(
        &edit, (sxml_array_edit_outcome){.status = status, .array = array});
}

sxml_status sxml_array_append(sxml_item_heap *item_heap,
                              const sxml_array_update *update,
                              sxml_item *array) {
    return sxml_array_change_apply(
        item_heap,
        &(sxml_array_change){.change_kind = SXML_ARRAY_CHANGE_KIND_APPEND,
                             .update = update},
        array);
}

sxml_status sxml_array_put(sxml_item_heap *item_heap,
                           const sxml_array_update *update, sxml_item *array) {
    return sxml_array_change_apply(
        item_heap,
        &(sxml_array_change){.change_kind = SXML_ARRAY_CHANGE_KIND_PUT,
                             .update = update},
        array);
}

sxml_status sxml_array_insert(sxml_item_heap *item_heap,
                              const sxml_array_update *update,
                              sxml_item *array) {
    return sxml_array_change_apply(
        item_heap,
        &(sxml_array_change){.change_kind = SXML_ARRAY_CHANGE_KIND_INSERT,
                             .update = update},
        array);
}

sxml_status sxml_array_remove(sxml_item_heap *item_heap,
                              const sxml_array_update *update,
                              sxml_item *array) {
    return sxml_array_change_apply(
        item_heap,
        &(sxml_array_change){.change_kind = SXML_ARRAY_CHANGE_KIND_REMOVE,
                             .update = update},
        array);
}

sxml_status sxml_array_slice(sxml_item_heap *item_heap,
                             const sxml_array_slice_request *request,
                             sxml_item *array) {
    sxml_array_node *root_node = sxml_array_get_root(request->array);
    if (root_node == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    size_t member_count = root_node->member_count;
    /* array:subarray (F&O 4.0, section 18): the part must lie within the
       array. */
    if (request->first_member_index > member_count ||
        member_count - request->first_member_index < request->member_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    if (request->member_count == member_count) {
        sxml_item_retain(request->array);
        *array = request->array;
        return SXML_STATUS_OK;
    }
    /* Cut away the members before the part, then those after it; the part
       is left in outer.left. */
    sxml_array_edit edit = sxml_array_edit_start(item_heap);
    sxml_status status = sxml_array_edit_cut(
        &edit, &(sxml_array_cut){.tree_node = sxml_array_get_tree(root_node),
                                 .member_index = request->first_member_index,
                                 .removed_member_count = 0});
    if (status == SXML_STATUS_OK) {
        status = sxml_array_node_split(
            item_heap,
            &(sxml_array_split_request){.node = edit.outer.right_node,
                                        .member_index = request->member_count},
            &edit.scratch);
    }
    if (status == SXML_STATUS_OK) {
        edit.joined_node = edit.scratch.left_node;
        edit.scratch.left_node = NULL;
    }
    return sxml_array_edit_finish(
        &edit, (sxml_array_edit_outcome){.status = status, .array = array});
}
