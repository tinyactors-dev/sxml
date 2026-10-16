/*
 * Maps and records (include/sxml/xdm.h; XDM 4.0, sections 8.2 and 8.3;
 * docs/DECISIONS.md D7, D37 and D61).
 *
 * Imitates src/memory.c for its lifecycle and failure paths and src/xdm.c
 * for its cells. A map item (SXML_ITEM_REPRESENTATION_MAP_CELL) points to
 * one of three cells, told apart by their cell class:
 *
 * - an entry block, made by sxml_map_make: the entries in entry order in
 *   one cell, plus a hash index for maps of more than
 *   SXML_MAP_SMALL_ENTRY_COUNT entries;
 * - a persistent map, made by sxml_map_put and sxml_map_remove: an entry
 *   tree and a key tree of map nodes, shared by path copying with the map
 *   it came from, so a change costs O(log n), not a copy of the map;
 * - a record, made by sxml_record_make: field values only; the field names
 *   and their index live once in the type registry, and field keys are
 *   made on first need.
 *
 * Keys compare by fn:atomic-equal and hash by sxml_atomic_item_hash, which
 * agree (F&O 4.0, section 2.2.1), so NaN finds NaN and xs:integer 1 finds
 * xs:double 1.0e0.
 */
#include "sxml/xdm.h"
#include "xdm_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Entry blocks of at most this many entries have no hash index (a linear
 * search is as fast), and put and remove keep results of at most this many
 * entries as entry blocks, copied, rather than persistent maps.
 */
#define SXML_MAP_SMALL_ENTRY_COUNT 8

/* The first hash index size: twice the entries of the smallest indexed
   block, rounded up to a power of two. */
#define SXML_MAP_MINIMUM_HASH_SLOT_COUNT 32

static const sxml_static_error sxml_map_duplicate_key_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XQDY0137"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Two entries of a map constructor have the same key"),
};

/* ---------------------------------------------------------- Cell kinds */

/* One slot of a hash index; entry_number 0 marks a free slot. */
typedef struct sxml_hash_slot {
    uint64_t key_hash;
    /* The entry index plus one. */
    size_t entry_number;
} sxml_hash_slot;

/* An entry block: entries in entry order, each holding its references. */
typedef struct sxml_entry_block {
    sxml_cell cell;
    size_t entry_count;
    /* A power of two, or 0 without a hash index. */
    size_t hash_slot_count;
    sxml_hash_slot *hash_slots;
    sxml_map_entry entries[];
} sxml_entry_block;

/*
 * A node of an entry tree or a key tree (AVL trees). Nodes are cells, so
 * trees share subtrees by reference. In a key tree the entry's value is the
 * empty sequence.
 */
typedef struct sxml_map_node {
    sxml_cell cell;
    struct sxml_map_node *left;
    struct sxml_map_node *right;
    uint64_t entry_stamp;
    uint64_t key_hash;
    /* Entries in the subtree rooted here, this one included. */
    size_t subtree_entry_count;
    /* 1 for a leaf. */
    size_t height;
    sxml_map_entry entry;
} sxml_map_node;

/* A persistent map: two trees over the same entries. */
typedef struct sxml_persistent_map {
    sxml_cell cell;
    /* Ordered by entry stamp; NULL when the map is empty. */
    sxml_map_node *entry_root;
    /* Ordered by key hash, then entry stamp. */
    sxml_map_node *key_root;
    uint64_t next_entry_stamp;
} sxml_persistent_map;

/* A record: one value per field of its record type, in field order. */
typedef struct sxml_record_cell {
    sxml_cell cell;
    sxml_type_identifier record_type;
    size_t field_count;
    /* field_count xs:string items, side memory of the cell; NULL until
       first needed (a cache that does not change the value). */
    sxml_item *field_keys;
    sxml_sequence field_values[];
} sxml_record_cell;

static void sxml_entry_block_release_contents(sxml_cell *cell);
static void sxml_map_node_release_contents(sxml_cell *cell);
static void sxml_persistent_map_release_contents(sxml_cell *cell);
static void sxml_record_cell_release_contents(sxml_cell *cell);

static const sxml_cell_class sxml_entry_block_class = {
    .class_name = SXML_BYTE_VIEW_INITIALIZER("map entry block"),
    .release_contents = sxml_entry_block_release_contents,
    .sequence_operations = NULL,
};

static const sxml_cell_class sxml_map_node_class = {
    .class_name = SXML_BYTE_VIEW_INITIALIZER("map node"),
    .release_contents = sxml_map_node_release_contents,
    .sequence_operations = NULL,
};

static const sxml_cell_class sxml_persistent_map_class = {
    .class_name = SXML_BYTE_VIEW_INITIALIZER("persistent map"),
    .release_contents = sxml_persistent_map_release_contents,
    .sequence_operations = NULL,
};

static const sxml_cell_class sxml_record_cell_class = {
    .class_name = SXML_BYTE_VIEW_INITIALIZER("record"),
    .release_contents = sxml_record_cell_release_contents,
    .sequence_operations = NULL,
};

/* Which cell a map item points to. */
typedef enum sxml_map_variety {
    SXML_MAP_VARIETY_NOT_A_MAP,
    SXML_MAP_VARIETY_ENTRY_BLOCK,
    SXML_MAP_VARIETY_PERSISTENT_MAP,
    SXML_MAP_VARIETY_RECORD,
} sxml_map_variety;

static sxml_map_variety sxml_map_get_variety(sxml_item map) {
    if (sxml_item_get_representation(map) !=
        SXML_ITEM_REPRESENTATION_MAP_CELL) {
        return SXML_MAP_VARIETY_NOT_A_MAP;
    }
    const sxml_cell_class *cell_class = sxml_item_get_cell(map)->cell_class;
    if (cell_class == &sxml_entry_block_class) {
        return SXML_MAP_VARIETY_ENTRY_BLOCK;
    }
    if (cell_class == &sxml_persistent_map_class) {
        return SXML_MAP_VARIETY_PERSISTENT_MAP;
    }
    if (cell_class == &sxml_record_cell_class) {
        return SXML_MAP_VARIETY_RECORD;
    }
    return SXML_MAP_VARIETY_NOT_A_MAP;
}

static sxml_entry_block *sxml_entry_block_of(sxml_item map) {
    return (sxml_entry_block *)sxml_item_get_cell(map);
}

static sxml_persistent_map *sxml_persistent_map_of(sxml_item map) {
    return (sxml_persistent_map *)sxml_item_get_cell(map);
}

static sxml_record_cell *sxml_record_cell_of(sxml_item map) {
    return (sxml_record_cell *)sxml_item_get_cell(map);
}

/* A map item, owning the reference cell came with. */
static sxml_item sxml_map_item_from_cell(sxml_cell *cell,
                                         sxml_type_identifier annotation) {
    return sxml_item_from_cell(&(sxml_cell_item_request){
        .representation = SXML_ITEM_REPRESENTATION_MAP_CELL,
        .type_annotation = annotation,
        .primitive_type = sxml_builtin_type_identifier(SXML_TYPE_NONE),
        .cell = cell,
    });
}

static void sxml_map_entry_retain(sxml_map_entry entry) {
    sxml_item_retain(entry.key);
    sxml_sequence_retain(entry.value);
}

static void sxml_map_entry_release(sxml_map_entry entry) {
    sxml_item_release(entry.key);
    sxml_sequence_release(entry.value);
}

/* A key to look for, with its hash where the search needs one. */
typedef struct sxml_key_search {
    sxml_item key;
    uint64_t key_hash;
} sxml_key_search;

/* -------------------------------------------------------- Entry blocks */

static void sxml_entry_block_release_hash_slots(sxml_entry_block *block) {
    sxml_item_heap_release_bytes(
        block->cell.item_heap,
        (sxml_heap_bytes){.memory = block->hash_slots,
                          .byte_count =
                              block->hash_slot_count * sizeof(sxml_hash_slot)});
    block->hash_slots = NULL;
}

static void sxml_entry_block_release_contents(sxml_cell *cell) {
    sxml_entry_block *block = (sxml_entry_block *)cell;
    for (size_t entry_index = 0; entry_index < block->entry_count;
         entry_index += 1) {
        sxml_map_entry_release(block->entries[entry_index]);
    }
    sxml_entry_block_release_hash_slots(block);
}

/* Frees a block that was never handed out. */
static void sxml_entry_block_discard(sxml_entry_block *block) {
    sxml_entry_block_release_contents(&block->cell);
    sxml_item_heap_discard_cell(&block->cell);
}

/*
 * Finds search->key among the block's entries. search->key_hash must be
 * the key's hash when the block has a hash index.
 */
static bool sxml_entry_block_find(const sxml_entry_block *block,
                                  const sxml_key_search *search,
                                  size_t *entry_index) {
    if (block->hash_slots == NULL) {
        for (size_t candidate_index = 0; candidate_index < block->entry_count;
             candidate_index += 1) {
            if (sxml_atomic_item_is_atomic_equal(
                    block->entries[candidate_index].key, search->key)) {
                *entry_index = candidate_index;
                return true;
            }
        }
        return false;
    }
    size_t slot_mask = block->hash_slot_count - 1;
    for (size_t slot_index = (size_t)search->key_hash & slot_mask;;
         slot_index = (slot_index + 1) & slot_mask) {
        const sxml_hash_slot *slot = &block->hash_slots[slot_index];
        if (slot->entry_number == 0) {
            return false;
        }
        if (slot->key_hash == search->key_hash &&
            sxml_atomic_item_is_atomic_equal(
                block->entries[slot->entry_number - 1].key, search->key)) {
            *entry_index = slot->entry_number - 1;
            return true;
        }
    }
}

/* Adds the entry at entry_index to the block's hash index. */
static void sxml_entry_block_index_entry(sxml_entry_block *block,
                                         const sxml_key_search *search,
                                         size_t entry_index) {
    size_t slot_mask = block->hash_slot_count - 1;
    size_t slot_index = (size_t)search->key_hash & slot_mask;
    while (block->hash_slots[slot_index].entry_number != 0) {
        slot_index = (slot_index + 1) & slot_mask;
    }
    block->hash_slots[slot_index] = (sxml_hash_slot){
        .key_hash = search->key_hash, .entry_number = entry_index + 1};
}

/* Gives block a hash index for entry_capacity entries, load at most 1/2. */
static sxml_status sxml_entry_block_allocate_hash_slots(sxml_entry_block *block,
                                                        size_t entry_capacity) {
    size_t hash_slot_count = SXML_MAP_MINIMUM_HASH_SLOT_COUNT;
    while (hash_slot_count / 2 < entry_capacity) {
        if (hash_slot_count > SIZE_MAX / 2) {
            return SXML_STATUS_LIMIT_EXCEEDED;
        }
        hash_slot_count *= 2;
    }
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(
        hash_slot_count, sizeof(sxml_hash_slot), &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_allocate_bytes(
            block->cell.item_heap,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(sxml_hash_slot)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(memory, 0, byte_count);
    block->hash_slots = memory;
    block->hash_slot_count = hash_slot_count;
    return SXML_STATUS_OK;
}

/* Allocates an empty block with room for entry_capacity entries. */
static sxml_status sxml_entry_block_allocate(sxml_item_heap *item_heap,
                                             size_t entry_capacity,
                                             sxml_entry_block **block) {
    size_t entries_byte_count = 0;
    sxml_status status = sxml_size_multiply(
        entry_capacity, sizeof(sxml_map_entry), &entries_byte_count);
    size_t cell_byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_add(sizeof(sxml_entry_block), entries_byte_count,
                               &cell_byte_count);
    }
    sxml_cell *cell = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_allocate_cell(
            item_heap,
            &(sxml_cell_request){.cell_class = &sxml_entry_block_class,
                                 .byte_count = cell_byte_count,
                                 .alignment = alignof(sxml_entry_block)},
            &cell);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_entry_block *new_block = (sxml_entry_block *)cell;
    new_block->entry_count = 0;
    new_block->hash_slot_count = 0;
    new_block->hash_slots = NULL;
    if (entry_capacity > SXML_MAP_SMALL_ENTRY_COUNT) {
        status =
            sxml_entry_block_allocate_hash_slots(new_block, entry_capacity);
        if (status != SXML_STATUS_OK) {
            sxml_item_heap_discard_cell(cell);
            return status;
        }
    }
    *block = new_block;
    return SXML_STATUS_OK;
}

static bool
sxml_duplicate_key_policy_is_valid(sxml_duplicate_key_policy policy) {
    switch (policy) {
    case SXML_DUPLICATE_KEY_POLICY_REJECT:
    case SXML_DUPLICATE_KEY_POLICY_USE_FIRST:
    case SXML_DUPLICATE_KEY_POLICY_USE_LAST:
        return true;
    }
    return false;
}

/*
 * Adds one entry to a block being made, applying the duplicate key policy.
 * Returns SXML_STATUS_DYNAMIC_ERROR for a rejected duplicate, without
 * filling an error record.
 */
static sxml_status sxml_entry_block_add(sxml_entry_block *block,
                                        const sxml_map_request *request,
                                        sxml_map_entry entry) {
    sxml_key_search search = {.key = entry.key, .key_hash = 0};
    if (block->hash_slots != NULL) {
        search.key_hash = sxml_atomic_item_hash(entry.key);
    }
    size_t entry_index = 0;
    if (sxml_entry_block_find(block, &search, &entry_index)) {
        switch (request->duplicate_key_policy) {
        case SXML_DUPLICATE_KEY_POLICY_REJECT:
            return SXML_STATUS_DYNAMIC_ERROR;
        case SXML_DUPLICATE_KEY_POLICY_USE_FIRST:
            return SXML_STATUS_OK;
        case SXML_DUPLICATE_KEY_POLICY_USE_LAST: {
            /* The first entry's key and position, the last one's value. */
            sxml_sequence_retain(entry.value);
            sxml_sequence_release(block->entries[entry_index].value);
            block->entries[entry_index].value = entry.value;
            return SXML_STATUS_OK;
        }
        }
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_map_entry_retain(entry);
    block->entries[block->entry_count] = entry;
    if (block->hash_slots != NULL) {
        sxml_entry_block_index_entry(block, &search, block->entry_count);
    }
    block->entry_count += 1;
    return SXML_STATUS_OK;
}

sxml_status sxml_map_make(sxml_item_heap *item_heap,
                          const sxml_map_request *request, sxml_item *map) {
    if (!sxml_duplicate_key_policy_is_valid(request->duplicate_key_policy)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_map_entry_view entries = request->map_entries;
    for (size_t entry_index = 0; entry_index < entries.map_entry_count;
         entry_index += 1) {
        if (!sxml_item_is_atomic(entries.map_entries[entry_index].key)) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
    }
    sxml_entry_block *block = NULL;
    sxml_status status =
        sxml_entry_block_allocate(item_heap, entries.map_entry_count, &block);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    for (size_t entry_index = 0;
         status == SXML_STATUS_OK && entry_index < entries.map_entry_count;
         entry_index += 1) {
        status = sxml_entry_block_add(block, request,
                                      entries.map_entries[entry_index]);
    }
    if (status != SXML_STATUS_OK) {
        sxml_entry_block_discard(block);
        if (status != SXML_STATUS_DYNAMIC_ERROR) {
            return status;
        }
        /* XPath 4.0, section 4.15.1.1: duplicate keys in a map
           constructor raise XQDY0137. */
        return sxml_error_record_report(request->error_record,
                                        &sxml_map_duplicate_key_error);
    }
    *map = sxml_map_item_from_cell(&block->cell,
                                   sxml_builtin_type_identifier(SXML_TYPE_MAP));
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Map nodes */

static size_t sxml_map_node_get_height(const sxml_map_node *node) {
    return node == NULL ? 0 : node->height;
}

static size_t sxml_map_node_get_entry_count(const sxml_map_node *node) {
    return node == NULL ? 0 : node->subtree_entry_count;
}

static void sxml_map_node_retain(sxml_map_node *node) {
    if (node != NULL) {
        sxml_cell_retain(&node->cell);
    }
}

static void sxml_map_node_release(sxml_map_node *node) {
    if (node != NULL) {
        sxml_cell_release(&node->cell);
    }
}

static void sxml_map_node_release_contents(sxml_cell *cell) {
    sxml_map_node *node = (sxml_map_node *)cell;
    sxml_map_entry_release(node->entry);
    sxml_map_node_release(node->left);
    sxml_map_node_release(node->right);
}

/* What a node is made of; every pointer is borrowed and retained by the
   node made from it. */
typedef struct sxml_map_node_parts {
    sxml_map_entry entry;
    uint64_t entry_stamp;
    uint64_t key_hash;
    sxml_map_node *left;
    sxml_map_node *right;
} sxml_map_node_parts;

/* The parts of node, with its own subtrees. */
static sxml_map_node_parts sxml_map_node_get_parts(const sxml_map_node *node) {
    return (sxml_map_node_parts){.entry = node->entry,
                                 .entry_stamp = node->entry_stamp,
                                 .key_hash = node->key_hash,
                                 .left = node->left,
                                 .right = node->right};
}

/* The parts of node with other subtrees. */
static sxml_map_node_parts
sxml_map_node_get_parts_with(const sxml_map_node *node, sxml_map_node *left,
                             sxml_map_node *right) {
    sxml_map_node_parts parts = sxml_map_node_get_parts(node);
    parts.left = left;
    parts.right = right;
    return parts;
}

/* Makes a node from parts, without rebalancing, and writes it, owned. */
static sxml_status sxml_map_node_make(sxml_item_heap *item_heap,
                                      const sxml_map_node_parts *parts,
                                      sxml_map_node **node) {
    sxml_cell *cell = NULL;
    sxml_status status = sxml_item_heap_allocate_cell(
        item_heap,
        &(sxml_cell_request){.cell_class = &sxml_map_node_class,
                             .byte_count = sizeof(sxml_map_node),
                             .alignment = alignof(sxml_map_node)},
        &cell);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_map_node *new_node = (sxml_map_node *)cell;
    size_t left_height = sxml_map_node_get_height(parts->left);
    size_t right_height = sxml_map_node_get_height(parts->right);
    new_node->left = parts->left;
    new_node->right = parts->right;
    new_node->entry_stamp = parts->entry_stamp;
    new_node->key_hash = parts->key_hash;
    /* Entry counts are bounded by the memory the nodes take, so the sum
       cannot overflow. */
    new_node->subtree_entry_count = 1 +
                                    sxml_map_node_get_entry_count(parts->left) +
                                    sxml_map_node_get_entry_count(parts->right);
    new_node->height =
        1 + (left_height > right_height ? left_height : right_height);
    new_node->entry = parts->entry;
    sxml_map_entry_retain(parts->entry);
    sxml_map_node_retain(parts->left);
    sxml_map_node_retain(parts->right);
    *node = new_node;
    return SXML_STATUS_OK;
}

/* Makes the upper node of a rotation and the node above it. */
typedef struct sxml_rotation {
    /* The node that goes below, with its new subtrees. */
    sxml_map_node_parts lower;
    /* The node that goes on top; the lower node becomes its left subtree
       when lower_goes_left, else its right subtree. */
    const sxml_map_node *upper;
    sxml_map_node *upper_other_subtree;
    bool lower_goes_left;
} sxml_rotation;

static sxml_status sxml_map_node_rotate(sxml_item_heap *item_heap,
                                        const sxml_rotation *rotation,
                                        sxml_map_node **node) {
    sxml_map_node *lower = NULL;
    sxml_status status =
        sxml_map_node_make(item_heap, &rotation->lower, &lower);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_map_node_parts upper =
        rotation->lower_goes_left
            ? sxml_map_node_get_parts_with(rotation->upper, lower,
                                           rotation->upper_other_subtree)
            : sxml_map_node_get_parts_with(
                  rotation->upper, rotation->upper_other_subtree, lower);
    status = sxml_map_node_make(item_heap, &upper, node);
    sxml_map_node_release(lower);
    return status;
}

/*
 * Makes a node from parts whose subtrees are AVL trees with heights that
 * differ by at most 2, rotating so the result is an AVL tree, and writes
 * it, owned. One single or double rotation suffices after one insertion or
 * removal below.
 */
static sxml_status sxml_map_node_make_balanced(sxml_item_heap *item_heap,
                                               const sxml_map_node_parts *parts,
                                               sxml_map_node **node) {
    size_t left_height = sxml_map_node_get_height(parts->left);
    size_t right_height = sxml_map_node_get_height(parts->right);
    if (left_height > right_height + 1) {
        const sxml_map_node *left = parts->left;
        if (sxml_map_node_get_height(left->left) >=
            sxml_map_node_get_height(left->right)) {
            /* Single right rotation. */
            sxml_map_node_parts lower = *parts;
            lower.left = left->right;
            return sxml_map_node_rotate(
                item_heap,
                &(sxml_rotation){.lower = lower,
                                 .upper = left,
                                 .upper_other_subtree = left->left,
                                 .lower_goes_left = false},
                node);
        }
        /* Double rotation: left's right child goes on top. */
        const sxml_map_node *middle = left->right;
        sxml_map_node *new_left = NULL;
        sxml_map_node_parts left_parts =
            sxml_map_node_get_parts_with(left, left->left, middle->left);
        sxml_status status =
            sxml_map_node_make(item_heap, &left_parts, &new_left);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_map_node_parts lower = *parts;
        lower.left = middle->right;
        status = sxml_map_node_rotate(
            item_heap,
            &(sxml_rotation){.lower = lower,
                             .upper = middle,
                             .upper_other_subtree = new_left,
                             .lower_goes_left = false},
            node);
        sxml_map_node_release(new_left);
        return status;
    }
    if (right_height > left_height + 1) {
        const sxml_map_node *right = parts->right;
        if (sxml_map_node_get_height(right->right) >=
            sxml_map_node_get_height(right->left)) {
            /* Single left rotation. */
            sxml_map_node_parts lower = *parts;
            lower.right = right->left;
            return sxml_map_node_rotate(
                item_heap,
                &(sxml_rotation){.lower = lower,
                                 .upper = right,
                                 .upper_other_subtree = right->right,
                                 .lower_goes_left = true},
                node);
        }
        /* Double rotation: right's left child goes on top. */
        const sxml_map_node *middle = right->left;
        sxml_map_node *new_right = NULL;
        sxml_map_node_parts right_parts =
            sxml_map_node_get_parts_with(right, middle->right, right->right);
        sxml_status status =
            sxml_map_node_make(item_heap, &right_parts, &new_right);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_map_node_parts lower = *parts;
        lower.right = middle->left;
        status = sxml_map_node_rotate(
            item_heap,
            &(sxml_rotation){.lower = lower,
                             .upper = middle,
                             .upper_other_subtree = new_right,
                             .lower_goes_left = true},
            node);
        sxml_map_node_release(new_right);
        return status;
    }
    return sxml_map_node_make(item_heap, parts, node);
}

/* ---------------------------------------------------------------- Trees */

/* The two orders of map nodes. */
typedef enum sxml_map_tree_kind {
    /* By entry stamp: entry order. */
    SXML_MAP_TREE_KIND_ENTRY,
    /* By key hash, then entry stamp. */
    SXML_MAP_TREE_KIND_KEY,
} sxml_map_tree_kind;

static sxml_ordering sxml_compare_unsigned(uint64_t first, uint64_t second) {
    if (first < second) {
        return SXML_ORDERING_LESS;
    }
    return first > second ? SXML_ORDERING_GREATER : SXML_ORDERING_EQUAL;
}

/* Where a node with these stamp and hash goes relative to node. */
typedef struct sxml_map_tree_position {
    sxml_map_tree_kind tree_kind;
    uint64_t entry_stamp;
    uint64_t key_hash;
} sxml_map_tree_position;

static sxml_ordering
sxml_map_tree_position_compare(const sxml_map_tree_position *position,
                               const sxml_map_node *node) {
    switch (position->tree_kind) {
    case SXML_MAP_TREE_KIND_ENTRY:
        return sxml_compare_unsigned(position->entry_stamp, node->entry_stamp);
    case SXML_MAP_TREE_KIND_KEY: {
        sxml_ordering ordering =
            sxml_compare_unsigned(position->key_hash, node->key_hash);
        return ordering != SXML_ORDERING_EQUAL
                   ? ordering
                   : sxml_compare_unsigned(position->entry_stamp,
                                           node->entry_stamp);
    }
    }
    return SXML_ORDERING_EQUAL;
}

/* A node to insert into a tree that does not hold its position. */
typedef struct sxml_map_tree_insertion {
    sxml_map_tree_kind tree_kind;
    /* Borrowed; NULL for an empty tree. */
    sxml_map_node *root;
    /* Its subtrees are ignored. */
    sxml_map_node_parts parts;
} sxml_map_tree_insertion;

/* Writes the tree with the node inserted, owned, to *new_root. */
static sxml_status
sxml_map_tree_insert(sxml_item_heap *item_heap,
                     const sxml_map_tree_insertion *insertion,
                     sxml_map_node **new_root) {
    sxml_map_node *root = insertion->root;
    if (root == NULL) {
        sxml_map_node_parts leaf = insertion->parts;
        leaf.left = NULL;
        leaf.right = NULL;
        return sxml_map_node_make(item_heap, &leaf, new_root);
    }
    sxml_map_tree_position position = {.tree_kind = insertion->tree_kind,
                                       .entry_stamp =
                                           insertion->parts.entry_stamp,
                                       .key_hash = insertion->parts.key_hash};
    bool goes_left =
        sxml_map_tree_position_compare(&position, root) == SXML_ORDERING_LESS;
    sxml_map_tree_insertion below = *insertion;
    below.root = goes_left ? root->left : root->right;
    sxml_map_node *new_child = NULL;
    sxml_status status = sxml_map_tree_insert(item_heap, &below, &new_child);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_map_node_parts parts =
        goes_left ? sxml_map_node_get_parts_with(root, new_child, root->right)
                  : sxml_map_node_get_parts_with(root, root->left, new_child);
    status = sxml_map_node_make_balanced(item_heap, &parts, new_root);
    sxml_map_node_release(new_child);
    return status;
}

/* A node to remove from a tree that holds its position. */
typedef struct sxml_map_tree_removal {
    sxml_map_node *root;
    sxml_map_tree_position position;
} sxml_map_tree_removal;

static const sxml_map_node *
sxml_map_tree_get_leftmost(const sxml_map_node *node) {
    while (node->left != NULL) {
        node = node->left;
    }
    return node;
}

/* Writes the tree without the node, owned (NULL when empty), to
 *new_root. */
static sxml_status sxml_map_tree_remove(sxml_item_heap *item_heap,
                                        const sxml_map_tree_removal *removal,
                                        sxml_map_node **new_root) {
    sxml_map_node *root = removal->root;
    if (root == NULL) {
        /* Not reached: the caller found the position in this tree. */
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_ordering ordering =
        sxml_map_tree_position_compare(&removal->position, root);
    if (ordering == SXML_ORDERING_EQUAL) {
        if (root->left == NULL || root->right == NULL) {
            sxml_map_node *survivor =
                root->left == NULL ? root->right : root->left;
            sxml_map_node_retain(survivor);
            *new_root = survivor;
            return SXML_STATUS_OK;
        }
        /* The successor takes the removed node's place. */
        const sxml_map_node *successor =
            sxml_map_tree_get_leftmost(root->right);
        sxml_map_tree_removal below = {
            .root = root->right,
            .position = {.tree_kind = removal->position.tree_kind,
                         .entry_stamp = successor->entry_stamp,
                         .key_hash = successor->key_hash}};
        sxml_map_node *new_right = NULL;
        sxml_status status =
            sxml_map_tree_remove(item_heap, &below, &new_right);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_map_node_parts parts =
            sxml_map_node_get_parts_with(successor, root->left, new_right);
        status = sxml_map_node_make_balanced(item_heap, &parts, new_root);
        sxml_map_node_release(new_right);
        return status;
    }
    bool goes_left = ordering == SXML_ORDERING_LESS;
    sxml_map_tree_removal below = *removal;
    below.root = goes_left ? root->left : root->right;
    sxml_map_node *new_child = NULL;
    sxml_status status = sxml_map_tree_remove(item_heap, &below, &new_child);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_map_node_parts parts =
        goes_left ? sxml_map_node_get_parts_with(root, new_child, root->right)
                  : sxml_map_node_get_parts_with(root, root->left, new_child);
    status = sxml_map_node_make_balanced(item_heap, &parts, new_root);
    sxml_map_node_release(new_child);
    return status;
}

/* A new value for the entry with entry_stamp, which root holds. */
typedef struct sxml_entry_tree_replacement {
    sxml_map_node *root;
    uint64_t entry_stamp;
    sxml_sequence value;
} sxml_entry_tree_replacement;

/* Writes the entry tree with the value replaced, owned, to *new_root. The
   shape does not change, so no rebalancing. */
static sxml_status
sxml_entry_tree_replace_value(sxml_item_heap *item_heap,
                              const sxml_entry_tree_replacement *replacement,
                              sxml_map_node **new_root) {
    sxml_map_node *root = replacement->root;
    if (root == NULL) {
        /* Not reached: the caller found the stamp in this tree. */
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_ordering ordering =
        sxml_compare_unsigned(replacement->entry_stamp, root->entry_stamp);
    if (ordering == SXML_ORDERING_EQUAL) {
        sxml_map_node_parts parts = sxml_map_node_get_parts(root);
        parts.entry.value = replacement->value;
        return sxml_map_node_make(item_heap, &parts, new_root);
    }
    bool goes_left = ordering == SXML_ORDERING_LESS;
    sxml_entry_tree_replacement below = *replacement;
    below.root = goes_left ? root->left : root->right;
    sxml_map_node *new_child = NULL;
    sxml_status status =
        sxml_entry_tree_replace_value(item_heap, &below, &new_child);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_map_node_parts parts =
        goes_left ? sxml_map_node_get_parts_with(root, new_child, root->right)
                  : sxml_map_node_get_parts_with(root, root->left, new_child);
    status = sxml_map_node_make(item_heap, &parts, new_root);
    sxml_map_node_release(new_child);
    return status;
}

/* The node at entry_index of an entry tree, or NULL past the end. */
static const sxml_map_node *sxml_entry_tree_get_node(const sxml_map_node *root,
                                                     size_t entry_index) {
    const sxml_map_node *node = root;
    size_t remaining_index = entry_index;
    while (node != NULL) {
        size_t left_count = sxml_map_node_get_entry_count(node->left);
        if (remaining_index < left_count) {
            node = node->left;
        } else if (remaining_index == left_count) {
            return node;
        } else {
            remaining_index -= left_count + 1;
            node = node->right;
        }
    }
    return NULL;
}

/* A node of an entry tree, found by stamp, and its entry index. */
typedef struct sxml_entry_tree_position {
    const sxml_map_node *node;
    size_t entry_index;
} sxml_entry_tree_position;

/* Finds the node with entry_stamp, which root holds. */
static sxml_entry_tree_position
sxml_entry_tree_find_stamp(const sxml_map_node *root, uint64_t entry_stamp) {
    const sxml_map_node *node = root;
    size_t entries_before = 0;
    while (node != NULL) {
        sxml_ordering ordering =
            sxml_compare_unsigned(entry_stamp, node->entry_stamp);
        if (ordering == SXML_ORDERING_EQUAL) {
            break;
        }
        if (ordering == SXML_ORDERING_LESS) {
            node = node->left;
        } else {
            entries_before += sxml_map_node_get_entry_count(node->left) + 1;
            node = node->right;
        }
    }
    return (sxml_entry_tree_position){
        .node = node,
        .entry_index =
            entries_before +
            (node == NULL ? 0 : sxml_map_node_get_entry_count(node->left))};
}

/*
 * Finds the key in a key tree. Keys with equal hashes sit in one range of
 * the order, possibly in both subtrees of a node with that hash, so the
 * search visits every node of the hash (few: the hash is 64 bits). The
 * recursion is bounded by the tree's height.
 */
static const sxml_map_node *sxml_key_tree_find(const sxml_map_node *root,
                                               const sxml_key_search *search) {
    if (root == NULL) {
        return NULL;
    }
    if (search->key_hash < root->key_hash) {
        return sxml_key_tree_find(root->left, search);
    }
    if (search->key_hash > root->key_hash) {
        return sxml_key_tree_find(root->right, search);
    }
    if (sxml_atomic_item_is_atomic_equal(root->entry.key, search->key)) {
        return root;
    }
    const sxml_map_node *found = sxml_key_tree_find(root->left, search);
    return found != NULL ? found : sxml_key_tree_find(root->right, search);
}

typedef struct sxml_map_node_parts_view {
    const sxml_map_node_parts *parts;
    size_t part_count;
} sxml_map_node_parts_view;

/* Builds a perfectly balanced tree from parts in tree order and writes it,
   owned (NULL for no parts), to *root. */
static sxml_status sxml_map_tree_build(sxml_item_heap *item_heap,
                                       sxml_map_node_parts_view parts,
                                       sxml_map_node **root) {
    if (parts.part_count == 0) {
        *root = NULL;
        return SXML_STATUS_OK;
    }
    size_t middle = parts.part_count / 2;
    sxml_map_node *left = NULL;
    sxml_status status = sxml_map_tree_build(
        item_heap,
        (sxml_map_node_parts_view){.parts = parts.parts, .part_count = middle},
        &left);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_map_node *right = NULL;
    status = sxml_map_tree_build(
        item_heap,
        (sxml_map_node_parts_view){.parts = parts.parts + middle + 1,
                                   .part_count = parts.part_count - middle - 1},
        &right);
    if (status == SXML_STATUS_OK) {
        sxml_map_node_parts middle_parts = parts.parts[middle];
        middle_parts.left = left;
        middle_parts.right = right;
        status = sxml_map_node_make(item_heap, &middle_parts, root);
    }
    sxml_map_node_release(left);
    sxml_map_node_release(right);
    return status;
}

/* ------------------------------------------------------ Persistent maps */

static void sxml_persistent_map_release_contents(sxml_cell *cell) {
    sxml_persistent_map *map = (sxml_persistent_map *)cell;
    sxml_map_node_release(map->entry_root);
    sxml_map_node_release(map->key_root);
}

/* The trees of a persistent map to make. */
typedef struct sxml_persistent_map_parts {
    sxml_map_node *entry_root;
    sxml_map_node *key_root;
    uint64_t next_entry_stamp;
} sxml_persistent_map_parts;

/* Makes a persistent map and writes it, owned, to *map. Takes the roots,
   on failure too. */
static sxml_status
sxml_persistent_map_make_taking(sxml_item_heap *item_heap,
                                const sxml_persistent_map_parts *parts,
                                sxml_item *map) {
    sxml_cell *cell = NULL;
    sxml_status status = sxml_item_heap_allocate_cell(
        item_heap,
        &(sxml_cell_request){.cell_class = &sxml_persistent_map_class,
                             .byte_count = sizeof(sxml_persistent_map),
                             .alignment = alignof(sxml_persistent_map)},
        &cell);
    if (status != SXML_STATUS_OK) {
        sxml_map_node_release(parts->entry_root);
        sxml_map_node_release(parts->key_root);
        return status;
    }
    sxml_persistent_map *persistent_map = (sxml_persistent_map *)cell;
    persistent_map->entry_root = parts->entry_root;
    persistent_map->key_root = parts->key_root;
    persistent_map->next_entry_stamp = parts->next_entry_stamp;
    *map = sxml_map_item_from_cell(cell,
                                   sxml_builtin_type_identifier(SXML_TYPE_MAP));
    return SXML_STATUS_OK;
}

/*
 * Key tree order for qsort (whose comparison signature this is): key hash,
 * then entry stamp.
 */
static int sxml_map_node_parts_compare_in_key_order(const void *first,
                                                    const void *second) {
    const sxml_map_node_parts *first_parts = first;
    const sxml_map_node_parts *second_parts = second;
    sxml_ordering ordering =
        sxml_compare_unsigned(first_parts->key_hash, second_parts->key_hash);
    if (ordering == SXML_ORDERING_EQUAL) {
        ordering = sxml_compare_unsigned(first_parts->entry_stamp,
                                         second_parts->entry_stamp);
    }
    return (int)ordering;
}

/* Parts that may be reordered. */
typedef struct sxml_map_node_parts_list {
    sxml_map_node_parts *parts;
    size_t part_count;
} sxml_map_node_parts_list;

/* Builds both trees from parts in entry order (and reorders them). */
static sxml_status sxml_persistent_map_build(sxml_item_heap *item_heap,
                                             sxml_map_node_parts_list list,
                                             sxml_persistent_map_parts *trees) {
    sxml_map_node_parts *parts = list.parts;
    sxml_map_node_parts_view view = {.parts = parts,
                                     .part_count = list.part_count};
    sxml_status status =
        sxml_map_tree_build(item_heap, view, &trees->entry_root);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    for (size_t part_index = 0; part_index < view.part_count; part_index += 1) {
        /* Key tree nodes hold no values. */
        parts[part_index].entry.value = (sxml_sequence){0};
    }
    qsort(parts, view.part_count, sizeof(sxml_map_node_parts),
          sxml_map_node_parts_compare_in_key_order);
    status = sxml_map_tree_build(item_heap, view, &trees->key_root);
    if (status != SXML_STATUS_OK) {
        sxml_map_node_release(trees->entry_root);
        trees->entry_root = NULL;
    }
    return status;
}

/*
 * Makes the persistent map with the entries of map (an entry block or a
 * record) and writes it, owned, to *persistent_map. O(n log n).
 */
static sxml_status sxml_persistent_map_make_from(sxml_item_heap *item_heap,
                                                 sxml_item map,
                                                 sxml_item *persistent_map) {
    size_t entry_count = 0;
    sxml_status status = sxml_map_get_size(map, &entry_count);
    size_t byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(entry_count, sizeof(sxml_map_node_parts),
                                    &byte_count);
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_allocate_bytes(
            item_heap,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment =
                                          alignof(sxml_map_node_parts)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_map_node_parts *parts = memory;
    for (size_t entry_index = 0;
         status == SXML_STATUS_OK && entry_index < entry_count;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(map, entry_index, &entry);
        if (status == SXML_STATUS_OK) {
            parts[entry_index] = (sxml_map_node_parts){
                .entry = entry,
                .entry_stamp = entry_index,
                .key_hash = sxml_atomic_item_hash(entry.key),
                .left = NULL,
                .right = NULL};
        }
    }
    sxml_persistent_map_parts trees = {
        .entry_root = NULL, .key_root = NULL, .next_entry_stamp = entry_count};
    if (status == SXML_STATUS_OK) {
        status = sxml_persistent_map_build(
            item_heap,
            (sxml_map_node_parts_list){.parts = parts,
                                       .part_count = entry_count},
            &trees);
    }
    sxml_item_heap_release_bytes(
        item_heap,
        (sxml_heap_bytes){.memory = memory, .byte_count = byte_count});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_persistent_map_make_taking(item_heap, &trees, persistent_map);
}

static sxml_status sxml_persistent_map_put(sxml_item_heap *item_heap,
                                           const sxml_map_update *update,
                                           sxml_item *map) {
    const sxml_persistent_map *old_map = sxml_persistent_map_of(update->map);
    sxml_key_search search = {.key = update->key,
                              .key_hash = sxml_atomic_item_hash(update->key)};
    const sxml_map_node *key_node =
        sxml_key_tree_find(old_map->key_root, &search);
    sxml_persistent_map_parts trees = {.entry_root = NULL,
                                       .key_root = NULL,
                                       .next_entry_stamp =
                                           old_map->next_entry_stamp};
    if (key_node != NULL) {
        /* The entry keeps its key and position; which key a replaced entry
           keeps is implementation-dependent (F&O 4.0, map:put). */
        sxml_status status = sxml_entry_tree_replace_value(
            item_heap,
            &(sxml_entry_tree_replacement){.root = old_map->entry_root,
                                           .entry_stamp = key_node->entry_stamp,
                                           .value = update->value},
            &trees.entry_root);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        trees.key_root = old_map->key_root;
        sxml_map_node_retain(trees.key_root);
        return sxml_persistent_map_make_taking(item_heap, &trees, map);
    }
    /* A new key goes last (XDM 4.0, section 8.2.2, dm:map-put). */
    sxml_map_node_parts parts = {
        .entry = {.key = update->key, .value = update->value},
        .entry_stamp = old_map->next_entry_stamp,
        .key_hash = search.key_hash,
        .left = NULL,
        .right = NULL};
    sxml_status status = sxml_map_tree_insert(
        item_heap,
        &(sxml_map_tree_insertion){.tree_kind = SXML_MAP_TREE_KIND_ENTRY,
                                   .root = old_map->entry_root,
                                   .parts = parts},
        &trees.entry_root);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    parts.entry.value = (sxml_sequence){0};
    status = sxml_map_tree_insert(
        item_heap,
        &(sxml_map_tree_insertion){.tree_kind = SXML_MAP_TREE_KIND_KEY,
                                   .root = old_map->key_root,
                                   .parts = parts},
        &trees.key_root);
    if (status != SXML_STATUS_OK) {
        sxml_map_node_release(trees.entry_root);
        return status;
    }
    trees.next_entry_stamp += 1;
    return sxml_persistent_map_make_taking(item_heap, &trees, map);
}

/* Removes update->key from the persistent map update->map. */
static sxml_status sxml_persistent_map_remove(sxml_item_heap *item_heap,
                                              const sxml_map_update *update,
                                              sxml_item *map) {
    const sxml_persistent_map *old_map = sxml_persistent_map_of(update->map);
    sxml_key_search search = {.key = update->key,
                              .key_hash = sxml_atomic_item_hash(update->key)};
    const sxml_map_node *key_node =
        sxml_key_tree_find(old_map->key_root, &search);
    if (key_node == NULL) {
        sxml_item_retain(update->map);
        *map = update->map;
        return SXML_STATUS_OK;
    }
    sxml_persistent_map_parts trees = {.entry_root = NULL,
                                       .key_root = NULL,
                                       .next_entry_stamp =
                                           old_map->next_entry_stamp};
    sxml_map_tree_position position = {.tree_kind = SXML_MAP_TREE_KIND_ENTRY,
                                       .entry_stamp = key_node->entry_stamp,
                                       .key_hash = key_node->key_hash};
    sxml_status status = sxml_map_tree_remove(
        item_heap,
        &(sxml_map_tree_removal){.root = old_map->entry_root,
                                 .position = position},
        &trees.entry_root);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    position.tree_kind = SXML_MAP_TREE_KIND_KEY;
    status =
        sxml_map_tree_remove(item_heap,
                             &(sxml_map_tree_removal){.root = old_map->key_root,
                                                      .position = position},
                             &trees.key_root);
    if (status != SXML_STATUS_OK) {
        sxml_map_node_release(trees.entry_root);
        return status;
    }
    return sxml_persistent_map_make_taking(item_heap, &trees, map);
}

/* ---------------------------------------------------------------- Records */

static void sxml_record_cell_release_field_keys(sxml_record_cell *record) {
    if (record->field_keys == NULL) {
        return;
    }
    for (size_t field_index = 0; field_index < record->field_count;
         field_index += 1) {
        sxml_item_release(record->field_keys[field_index]);
    }
    sxml_item_heap_release_bytes(
        record->cell.item_heap,
        (sxml_heap_bytes){.memory = record->field_keys,
                          .byte_count =
                              record->field_count * sizeof(sxml_item)});
    record->field_keys = NULL;
}

static void sxml_record_cell_release_contents(sxml_cell *cell) {
    sxml_record_cell *record = (sxml_record_cell *)cell;
    for (size_t field_index = 0; field_index < record->field_count;
         field_index += 1) {
        sxml_sequence_release(record->field_values[field_index]);
    }
    sxml_record_cell_release_field_keys(record);
}

/* Describes record_type when it is a record type with fields (not
   record(*)). */
static sxml_status sxml_record_type_describe(const sxml_type_registry *registry,
                                             sxml_type_identifier record_type,
                                             sxml_record_type *description) {
    sxml_type_description type_description;
    sxml_status status = sxml_type_registry_describe_type(registry, record_type,
                                                          &type_description);
    if (status != SXML_STATUS_OK ||
        type_description.variety != SXML_TYPE_VARIETY_RECORD ||
        type_description.variety_details.record_type.is_any_record) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *description = type_description.variety_details.record_type;
    return SXML_STATUS_OK;
}

/*
 * Makes the record's field keys, xs:string items of the field names (XDM
 * 4.0, section 8.3.1, dm:make-record), unless it has them. Changes only
 * the cache, not the value.
 */
static sxml_status sxml_record_cell_make_field_keys(sxml_record_cell *record) {
    if (record->field_keys != NULL || record->field_count == 0) {
        return SXML_STATUS_OK;
    }
    sxml_item_heap *item_heap = record->cell.item_heap;
    sxml_record_type record_type;
    sxml_status status =
        sxml_record_type_describe(sxml_item_heap_get_type_registry(item_heap),
                                  record->record_type, &record_type);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_allocate_bytes(
            item_heap,
            (sxml_allocation_request){.byte_count = record->field_count *
                                                    sizeof(sxml_item),
                                      .alignment = alignof(sxml_item)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item *field_keys = memory;
    size_t made_count = 0;
    while (status == SXML_STATUS_OK && made_count < record->field_count) {
        sxml_string field_name = {.utf8 = {.bytes = NULL, .byte_count = 0},
                                  .codepoint_count = 0,
                                  .is_ascii = false};
        status = sxml_string_from_utf8(
            record_type.record_fields.record_fields[made_count].field_name,
            &field_name);
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_make(
                item_heap,
                &(sxml_atomic_item_request){
                    .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
                    .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                              .datum_details = {.string = field_name}}},
                &field_keys[made_count]);
        }
        if (status == SXML_STATUS_OK) {
            made_count += 1;
        }
    }
    if (status != SXML_STATUS_OK) {
        for (size_t field_index = 0; field_index < made_count;
             field_index += 1) {
            sxml_item_release(field_keys[field_index]);
        }
        sxml_item_heap_release_bytes(
            item_heap, (sxml_heap_bytes){.memory = memory,
                                         .byte_count = record->field_count *
                                                       sizeof(sxml_item)});
        return status;
    }
    record->field_keys = field_keys;
    return SXML_STATUS_OK;
}

sxml_status sxml_record_make(sxml_item_heap *item_heap,
                             const sxml_record_request *request,
                             sxml_item *record) {
    sxml_record_type record_type;
    sxml_status status =
        sxml_record_type_describe(sxml_item_heap_get_type_registry(item_heap),
                                  request->record_type, &record_type);
    size_t field_count = request->field_values.sequence_count;
    if (status == SXML_STATUS_OK &&
        record_type.record_fields.record_field_count != field_count) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    size_t values_byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(field_count, sizeof(sxml_sequence),
                                    &values_byte_count);
    }
    size_t cell_byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_add(sizeof(sxml_record_cell), values_byte_count,
                               &cell_byte_count);
    }
    sxml_cell *cell = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_allocate_cell(
            item_heap,
            &(sxml_cell_request){.cell_class = &sxml_record_cell_class,
                                 .byte_count = cell_byte_count,
                                 .alignment = alignof(sxml_record_cell)},
            &cell);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_record_cell *record_cell = (sxml_record_cell *)cell;
    record_cell->record_type = request->record_type;
    record_cell->field_count = field_count;
    record_cell->field_keys = NULL;
    for (size_t field_index = 0; field_index < field_count; field_index += 1) {
        sxml_sequence value = request->field_values.sequences[field_index];
        sxml_sequence_retain(value);
        record_cell->field_values[field_index] = value;
    }
    *record = sxml_map_item_from_cell(cell, request->record_type);
    return SXML_STATUS_OK;
}

sxml_status sxml_record_get_field_value(sxml_item record, size_t field_index,
                                        sxml_sequence *value) {
    if (sxml_map_get_variety(record) != SXML_MAP_VARIETY_RECORD) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_record_cell *record_cell = sxml_record_cell_of(record);
    if (field_index >= record_cell->field_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    *value = record_cell->field_values[field_index];
    return SXML_STATUS_OK;
}

/* True for keys that can be atomic-equal to a field name: the string
   family (F&O 4.0, section 2.2.1). */
static bool sxml_key_is_string(sxml_item key) {
    switch (sxml_atomic_item_get_primitive_type(key).index) {
    case SXML_TYPE_XS_STRING:
    case SXML_TYPE_XS_ANY_URI:
    case SXML_TYPE_XS_UNTYPED_ATOMIC:
        return true;
    default:
        return false;
    }
}

/* Finds key among the record's fields through the registry's shared
   field index (D7: records share their key "shape"). */
static bool sxml_record_find_field(const sxml_record_cell *record,
                                   sxml_item key, size_t *field_index) {
    sxml_atomic_datum datum;
    if (!sxml_key_is_string(key) ||
        sxml_atomic_item_get_datum(key, &datum) != SXML_STATUS_OK) {
        return false;
    }
    return sxml_type_registry_find_record_field(
        sxml_item_heap_get_type_registry(record->cell.item_heap),
        (sxml_record_field_query){.record_type = record->record_type,
                                  .field_name =
                                      datum.datum_details.string.utf8},
        field_index);
}

/* ------------------------------------------------------- Reading maps */

sxml_status sxml_map_get_size(sxml_item map, size_t *map_entry_count) {
    switch (sxml_map_get_variety(map)) {
    case SXML_MAP_VARIETY_ENTRY_BLOCK:
        *map_entry_count = sxml_entry_block_of(map)->entry_count;
        return SXML_STATUS_OK;
    case SXML_MAP_VARIETY_PERSISTENT_MAP:
        *map_entry_count = sxml_map_node_get_entry_count(
            sxml_persistent_map_of(map)->entry_root);
        return SXML_STATUS_OK;
    case SXML_MAP_VARIETY_RECORD:
        *map_entry_count = sxml_record_cell_of(map)->field_count;
        return SXML_STATUS_OK;
    case SXML_MAP_VARIETY_NOT_A_MAP:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* The entry of a record at a field index below its field count. */
static sxml_status sxml_record_get_entry(sxml_record_cell *record,
                                         size_t field_index,
                                         sxml_map_entry *entry) {
    sxml_status status = sxml_record_cell_make_field_keys(record);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *entry = (sxml_map_entry){.key = record->field_keys[field_index],
                              .value = record->field_values[field_index]};
    return SXML_STATUS_OK;
}

sxml_status sxml_map_get_entry(sxml_item map, size_t entry_index,
                               sxml_map_entry *entry) {
    size_t entry_count = 0;
    sxml_status status = sxml_map_get_size(map, &entry_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (entry_index >= entry_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    switch (sxml_map_get_variety(map)) {
    case SXML_MAP_VARIETY_ENTRY_BLOCK:
        *entry = sxml_entry_block_of(map)->entries[entry_index];
        return SXML_STATUS_OK;
    case SXML_MAP_VARIETY_PERSISTENT_MAP:
        *entry = sxml_entry_tree_get_node(
                     sxml_persistent_map_of(map)->entry_root, entry_index)
                     ->entry;
        return SXML_STATUS_OK;
    case SXML_MAP_VARIETY_RECORD:
        /* May make the field keys: the heap's failures (P3). */
        return sxml_record_get_entry(sxml_record_cell_of(map), entry_index,
                                     entry);
    case SXML_MAP_VARIETY_NOT_A_MAP:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

static const sxml_map_lookup sxml_map_lookup_not_found = {
    .is_found = false,
    .entry_index = 0,
    .entry = {.key = {.representation = 0}, .value = {{.representation = 0}}},
};

sxml_status sxml_map_find(sxml_item map, sxml_item key,
                          sxml_map_lookup *lookup) {
    switch (sxml_map_get_variety(map)) {
    case SXML_MAP_VARIETY_ENTRY_BLOCK: {
        const sxml_entry_block *block = sxml_entry_block_of(map);
        sxml_key_search search = {.key = key, .key_hash = 0};
        if (block->hash_slots != NULL) {
            search.key_hash = sxml_atomic_item_hash(key);
        }
        size_t entry_index = 0;
        if (!sxml_entry_block_find(block, &search, &entry_index)) {
            *lookup = sxml_map_lookup_not_found;
            return SXML_STATUS_OK;
        }
        *lookup = (sxml_map_lookup){.is_found = true,
                                    .entry_index = entry_index,
                                    .entry = block->entries[entry_index]};
        return SXML_STATUS_OK;
    }
    case SXML_MAP_VARIETY_PERSISTENT_MAP: {
        const sxml_persistent_map *persistent_map = sxml_persistent_map_of(map);
        sxml_key_search search = {.key = key,
                                  .key_hash = sxml_atomic_item_hash(key)};
        const sxml_map_node *key_node =
            sxml_key_tree_find(persistent_map->key_root, &search);
        if (key_node == NULL) {
            *lookup = sxml_map_lookup_not_found;
            return SXML_STATUS_OK;
        }
        sxml_entry_tree_position position = sxml_entry_tree_find_stamp(
            persistent_map->entry_root, key_node->entry_stamp);
        if (position.node == NULL) {
            /* Not reached: both trees hold the same stamps. */
            *lookup = sxml_map_lookup_not_found;
            return SXML_STATUS_OK;
        }
        *lookup = (sxml_map_lookup){.is_found = true,
                                    .entry_index = position.entry_index,
                                    .entry = position.node->entry};
        return SXML_STATUS_OK;
    }
    case SXML_MAP_VARIETY_RECORD: {
        sxml_record_cell *record = sxml_record_cell_of(map);
        size_t field_index = 0;
        if (!sxml_record_find_field(record, key, &field_index)) {
            *lookup = sxml_map_lookup_not_found;
            return SXML_STATUS_OK;
        }
        sxml_map_entry entry;
        sxml_status status = sxml_record_get_entry(record, field_index, &entry);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        *lookup = (sxml_map_lookup){
            .is_found = true, .entry_index = field_index, .entry = entry};
        return SXML_STATUS_OK;
    }
    case SXML_MAP_VARIETY_NOT_A_MAP:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* ------------------------------------------------------- Changing maps */

/* A change to a small map, whose result is an entry block. */
typedef struct sxml_small_map_change {
    const sxml_map_update *update;
    /* Where update->key is in update->map. */
    sxml_map_lookup lookup;
    bool is_removal;
} sxml_small_map_change;

/* Copies the entries of a small map with the change into a new block. */
static sxml_status
sxml_small_map_change_apply(sxml_item_heap *item_heap,
                            const sxml_small_map_change *change,
                            sxml_item *map) {
    sxml_map_entry entries[SXML_MAP_SMALL_ENTRY_COUNT];
    size_t entry_count = 0;
    size_t old_entry_count = 0;
    sxml_status status =
        sxml_map_get_size(change->update->map, &old_entry_count);
    for (size_t entry_index = 0;
         status == SXML_STATUS_OK && entry_index < old_entry_count;
         entry_index += 1) {
        sxml_map_entry entry;
        status = sxml_map_get_entry(change->update->map, entry_index, &entry);
        bool is_changed = change->lookup.is_found &&
                          entry_index == change->lookup.entry_index;
        if (status != SXML_STATUS_OK || (is_changed && change->is_removal)) {
            continue;
        }
        if (is_changed) {
            /* The entry keeps its key and position (F&O 4.0, map:put). */
            entry.value = change->update->value;
        }
        entries[entry_count] = entry;
        entry_count += 1;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!change->lookup.is_found && !change->is_removal) {
        entries[entry_count] = (sxml_map_entry){.key = change->update->key,
                                                .value = change->update->value};
        entry_count += 1;
    }
    return sxml_map_make(
        item_heap,
        &(sxml_map_request){.map_entries = {.map_entries = entries,
                                            .map_entry_count = entry_count},
                            .duplicate_key_policy =
                                SXML_DUPLICATE_KEY_POLICY_USE_FIRST,
                            .error_record = NULL},
        map);
}

/* The two changes. */
typedef enum sxml_map_change_kind {
    SXML_MAP_CHANGE_KIND_PUT,
    SXML_MAP_CHANGE_KIND_REMOVE,
} sxml_map_change_kind;

/*
 * Applies a change to an entry block or record: as a copy when the result
 * is small, else on the persistent map made from it.
 */
typedef struct sxml_map_change {
    const sxml_map_update *update;
    sxml_map_change_kind change_kind;
} sxml_map_change;

static sxml_status sxml_map_change_apply(sxml_item_heap *item_heap,
                                         const sxml_map_change *change,
                                         sxml_item *map) {
    const sxml_map_update *update = change->update;
    bool is_removal = change->change_kind == SXML_MAP_CHANGE_KIND_REMOVE;
    if (sxml_map_get_variety(update->map) == SXML_MAP_VARIETY_PERSISTENT_MAP) {
        return is_removal ? sxml_persistent_map_remove(item_heap, update, map)
                          : sxml_persistent_map_put(item_heap, update, map);
    }
    sxml_map_lookup lookup;
    sxml_status status = sxml_map_find(update->map, update->key, &lookup);
    size_t entry_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_map_get_size(update->map, &entry_count);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (is_removal && !lookup.is_found) {
        sxml_item_retain(update->map);
        *map = update->map;
        return SXML_STATUS_OK;
    }
    size_t result_count = entry_count;
    if (is_removal) {
        result_count -= 1;
    } else if (!lookup.is_found) {
        result_count += 1;
    }
    if (result_count <= SXML_MAP_SMALL_ENTRY_COUNT) {
        return sxml_small_map_change_apply(
            item_heap,
            &(sxml_small_map_change){
                .update = update, .lookup = lookup, .is_removal = is_removal},
            map);
    }
    sxml_item persistent_map = {.representation = 0};
    status =
        sxml_persistent_map_make_from(item_heap, update->map, &persistent_map);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_map_update persistent_update = *update;
    persistent_update.map = persistent_map;
    status =
        is_removal
            ? sxml_persistent_map_remove(item_heap, &persistent_update, map)
            : sxml_persistent_map_put(item_heap, &persistent_update, map);
    sxml_item_release(persistent_map);
    return status;
}

sxml_status sxml_map_put(sxml_item_heap *item_heap,
                         const sxml_map_update *update, sxml_item *map) {
    if (sxml_map_get_variety(update->map) == SXML_MAP_VARIETY_NOT_A_MAP ||
        !sxml_item_is_atomic(update->key)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* XDM 4.0, section 8.2.2 (dm:map-put): the result is a map, even from
       a record. */
    return sxml_map_change_apply(
        item_heap,
        &(sxml_map_change){.update = update,
                           .change_kind = SXML_MAP_CHANGE_KIND_PUT},
        map);
}

sxml_status sxml_map_remove(sxml_item_heap *item_heap,
                            const sxml_map_update *update, sxml_item *map) {
    if (sxml_map_get_variety(update->map) == SXML_MAP_VARIETY_NOT_A_MAP ||
        !sxml_item_is_atomic(update->key)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_map_change_apply(
        item_heap,
        &(sxml_map_change){.update = update,
                           .change_kind = SXML_MAP_CHANGE_KIND_REMOVE},
        map);
}
