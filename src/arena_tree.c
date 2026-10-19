/*
 * The arena tree: the built-in tree and its tree builder
 * (include/sxml/tree.h; docs/DECISIONS.md D40 and D67).
 *
 * Golden file: src/memory.c (lifecycle, composite memory requirements,
 * checked sizes, comments).
 *
 * Specification: XDM 4.0, section 6 and 7.5 (node kinds and their
 * properties), section 7.5.7.1 (adjacent text merges, empty text makes no
 * node, except a parentless one, D163); XSLT 4.0, section 5.7.3 "Namespace Fixup"; XQuery 4.0, "Element
 * Constructors" (XQDY0025, XQDY0102, XQDY0101, XQDY0044).
 *
 * Layout
 * ------
 * All memory comes from one sxml_arena over initial_memory and the
 * configured allocator (D24), released together at finalize. A growable
 * tree without initial memory gets a small region with its own allocation
 * as the arena's first memory, so a tree of a few nodes needs no block
 * (sized arenas, D163).
 * - Nodes live in node segments: segment k holds 2 * 2^k nodes, allocated
 *   when the first of them is added, so nodes never move and no array is
 *   copied as the tree grows. Locations, when kept, use a second set of
 *   segments with the same layout.
 * - Node content (text, attribute values, comments, processing-instruction
 *   data) and copied URIs go to the text store: blocks filled back to
 *   back. Adjacent text events append to the store's pending bytes, which
 *   become one text node at the next other event; a pending run that
 *   outgrows its block moves to a larger one, the only copy.
 * - In-scope namespace lists are arrays in the arena. An element that
 *   declares nothing new shares its parent's list (D40).
 *
 * Building
 * --------
 * The open node is the innermost started document or element; parents are
 * found through the nodes, so no stack is kept. An element's header (its
 * namespace bindings and attributes) stays open until its first content
 * or its end; then namespace fixup runs and its in-scope namespace list is
 * made. Attributes are nodes right after their element (D39), added as
 * they come; duplicate names are found by a scan, or by a hash index once
 * an element has more than a few attributes, so many attributes stay
 * linear.
 */
#include "tree_internal.h"

#include "sxml/xdm.h"
#include "xdm_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------ Constants */

/* Nodes in segment 0; segment k holds this times 2^k. A power of two. */
#define SXML_ARENA_TREE_FIRST_SEGMENT_NODE_COUNT 2U
/* log2 of SXML_ARENA_TREE_FIRST_SEGMENT_NODE_COUNT. */
#define SXML_ARENA_TREE_FIRST_SEGMENT_SHIFT 1U
/* Enough segments for every node index below UINT32_MAX. */
#define SXML_ARENA_TREE_SEGMENT_COUNT 32U
/* The arena's block size when it can grow. Small: evaluation builds
   many one-element trees (each XQuery element constructor makes one), and
   the node segments, which double, outgrow it and get blocks of their
   own. */
#define SXML_ARENA_TREE_BLOCK_BYTE_COUNT 4096U
/* Memory placed right after a growable arena tree that has no initial
   memory of its own, used before the first block: a tree of a few nodes,
   the most common constructed tree, then takes one allocation of about
   one and a half kilobytes instead of a four-kilobyte block more (sized
   arenas, D163). */
#define SXML_ARENA_TREE_INLINE_BYTE_COUNT 768U
/* Text store blocks start at the minimum and double up to the maximum,
   unless one pending run needs more. */
#define SXML_ARENA_TREE_MINIMUM_TEXT_BLOCK_BYTE_COUNT 64U
#define SXML_ARENA_TREE_MAXIMUM_TEXT_BLOCK_BYTE_COUNT 1048576U
/* Above this many attributes an element's duplicate check uses a hash
   index instead of a scan. */
#define SXML_ARENA_TREE_ATTRIBUTE_SCAN_LIMIT 8U
#define SXML_ARENA_TREE_MINIMUM_ATTRIBUTE_SLOT_COUNT 32U
/* Initial capacity of the declared namespace binding list. */
#define SXML_ARENA_TREE_MINIMUM_DECLARED_CAPACITY 8U

/* Node flags. */
#define SXML_ARENA_NODE_HAS_PARENT 0x01U
#define SXML_ARENA_NODE_HAS_PREVIOUS_SIBLING 0x02U
#define SXML_ARENA_NODE_HAS_NEXT_SIBLING 0x04U
#define SXML_ARENA_NODE_HAS_CHILD 0x08U
#define SXML_ARENA_NODE_IS_ID 0x10U
#define SXML_ARENA_NODE_IS_IDREFS 0x20U
#define SXML_ARENA_NODE_IS_NILLED 0x40U
/* A text node to serialize as CDATA sections (glossary cdata_section). */
#define SXML_ARENA_NODE_IS_CDATA_SECTION 0x80U
/* A text node whose escaping is disabled (glossary
   disable_output_escaping, D188). */
#define SXML_ARENA_NODE_DISABLES_OUTPUT_ESCAPING 0x100U

/* --------------------------------------------------------------- Types */

/* One stored node. */
typedef struct sxml_arena_node {
    /* By node_kind (one or the other, so a node is 16 bytes smaller). */
    union {
        /* Text, comment, processing-instruction data, attribute value,
           namespace URI. */
        sxml_byte_view content;
        /* Elements: the in-scope namespaces, possibly the parent's
           array. */
        sxml_namespace_binding_view namespace_bindings;
    };
    sxml_name_identifier node_name;
    sxml_namespace_prefix_identifier namespace_prefix;
    sxml_type_identifier type_annotation;
    uint32_t parent;
    uint32_t last_descendant;
    uint32_t attribute_count;
    uint32_t last_child;
    uint32_t previous_sibling;
    uint32_t next_sibling;
    uint8_t node_kind;
    uint16_t flags;
} sxml_arena_node;

/* An unparsed entity, in a list newest first. */
typedef struct sxml_arena_unparsed_entity {
    sxml_unparsed_entity entity;
    struct sxml_arena_unparsed_entity *next_entity;
} sxml_arena_unparsed_entity;

/* Where building is. */
typedef enum sxml_arena_tree_phase {
    /* No event yet. */
    SXML_ARENA_TREE_PHASE_EMPTY,
    /* A document or element root is open. */
    SXML_ARENA_TREE_PHASE_BUILDING,
    /* The root is parentless text, still taking adjacent text events. */
    SXML_ARENA_TREE_PHASE_TOP_TEXT,
    /* The root is complete; only sxml_arena_tree_finish may follow. */
    SXML_ARENA_TREE_PHASE_COMPLETE,
    /* The tree has been handed out. */
    SXML_ARENA_TREE_PHASE_FINISHED,
} sxml_arena_tree_phase;

struct sxml_arena_tree {
    sxml_arena_tree_configuration configuration;
    sxml_allocator owning_allocator;
    sxml_arena *arena;
    sxml_name_pool *name_pool;
    sxml_arena_tree_phase phase;
    /* The tree wrapper, placed here by sxml_arena_tree_finish. */
    struct sxml_tree tree_storage;
    sxml_tree *tree;

    /* Nodes and their locations. */
    sxml_arena_node *node_segments[SXML_ARENA_TREE_SEGMENT_COUNT];
    /* SXML_ARENA_TREE_SEGMENT_COUNT pointers, made at the first node when
       keeps_locations; NULL otherwise. */
    sxml_source_location **location_segments;
    uint32_t node_count;

    /* The innermost open document or element, and how many elements are
       open (for maximum_depth). */
    bool has_open_node;
    uint32_t open_node;
    uint32_t element_depth;

    /* The element whose header is open, its declared namespace bindings,
       and the hash index of its attributes once it has many. */
    bool is_in_header;
    uint32_t header_element;
    /* The header element inherits nothing from its parent (copy-namespaces
       no-inherit, D69). */
    bool header_ignores_parent_namespaces;
    sxml_namespace_binding *declared_bindings;
    size_t declared_binding_count;
    size_t declared_binding_capacity;
    uint32_t *attribute_slots;
    size_t attribute_slot_capacity;
    size_t active_attribute_slot_count;
    bool has_attribute_index;
    /* Some event of the pending text asked for CDATA sections (D163). */
    bool pending_text_is_cdata_section;
    /* The pending text's events disable output escaping (D188). */
    bool pending_text_disables_output_escaping;

    /* The text store. */
    uint8_t *text_block;
    size_t text_block_byte_count;
    size_t text_used_byte_count;
    size_t pending_text_offset;
    size_t text_byte_total;
    sxml_source_location pending_text_location;
    sxml_byte_view last_source_name;

    /* The base namespace list, holding only the xml binding (XML
       Namespaces 1.0, section 3: xml is bound in every element). */
    sxml_namespace_binding_view base_bindings;
    uint32_t generated_prefix_count;

    sxml_byte_view document_uri;
    sxml_byte_view base_uri;
    sxml_arena_unparsed_entity *newest_entity;
};

/* -------------------------------------------------------------- Errors */

static const sxml_static_error sxml_arena_tree_duplicate_attribute_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XQDY0025"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "an element has two attributes with the same name"),
};

static const sxml_static_error sxml_arena_tree_conflicting_binding_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XQDY0102"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "an element binds one prefix to two namespace URIs"),
};

static const sxml_static_error sxml_arena_tree_reserved_binding_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XQDY0101"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "a namespace binding misuses the xml or xmlns prefix or namespace"),
};

static const sxml_static_error sxml_arena_tree_xmlns_attribute_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XQDY0044"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "an attribute name is in the xmlns namespace"),
};

static sxml_status sxml_arena_tree_report(
    const sxml_arena_tree *arena_tree, const sxml_static_error *error) {
    return sxml_error_record_report(arena_tree->configuration.error_record,
                                    error);
}

/* ------------------------------------------------------------ Lifecycle */

static bool sxml_arena_tree_configuration_is_valid(
    const sxml_arena_tree_configuration *configuration) {
    bool initial_memory_is_valid = configuration->initial_memory.bytes != NULL ||
                                   configuration->initial_memory.byte_count == 0;
    bool policy_is_valid =
        configuration->duplicate_attribute_policy ==
            SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT ||
        configuration->duplicate_attribute_policy ==
            SXML_DUPLICATE_ATTRIBUTE_POLICY_REPLACE;
    return configuration->item_heap != NULL && initial_memory_is_valid &&
           policy_is_valid;
}

static sxml_arena_configuration sxml_arena_tree_arena_configuration(
    const sxml_arena_tree_configuration *configuration) {
    bool can_grow = !sxml_allocator_is_null(configuration->allocator);
    return (sxml_arena_configuration){
        .block_allocator = configuration->allocator,
        .block_byte_count = can_grow ? SXML_ARENA_TREE_BLOCK_BYTE_COUNT : 0,
        .maximum_block_byte_count = 0,
        .initial_memory = configuration->initial_memory,
    };
}

/* The layout of an arena tree's memory: the struct, then its arena. */
typedef struct sxml_arena_tree_layout {
    sxml_memory_requirements requirements;
    size_t arena_offset;
    /* SXML_ARENA_TREE_INLINE_BYTE_COUNT bytes for the arena's initial
       memory, when the tree can grow and has none of its own; else 0. */
    size_t inline_offset;
    size_t inline_byte_count;
} sxml_arena_tree_layout;

/* Whether the tree takes SXML_ARENA_TREE_INLINE_BYTE_COUNT bytes of
   initial memory with itself. */
static bool sxml_arena_tree_has_inline_memory(
    const sxml_arena_tree_configuration *configuration) {
    return !sxml_allocator_is_null(configuration->allocator) &&
           configuration->initial_memory.byte_count == 0;
}

static sxml_status sxml_arena_tree_compute_layout(
    const sxml_arena_tree_configuration *configuration,
    sxml_arena_tree_layout *layout) {
    if (!sxml_arena_tree_configuration_is_valid(configuration)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_arena_configuration arena_configuration =
        sxml_arena_tree_arena_configuration(configuration);
    sxml_memory_requirements arena_requirements = {.byte_count = 0,
                                                   .alignment = 1};
    sxml_status status = sxml_arena_memory_requirements(&arena_configuration,
                                                        &arena_requirements);
    if (status != SXML_STATUS_OK) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    size_t tree_offset = 0;
    size_t arena_offset = 0;
    status = sxml_memory_requirements_append(
        &requirements,
        (sxml_memory_requirements){.byte_count = sizeof(struct sxml_arena_tree),
                                   .alignment = alignof(struct sxml_arena_tree)},
        &tree_offset);
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_requirements_append(
            &requirements, arena_requirements, &arena_offset);
    }
    size_t inline_offset = 0;
    const size_t inline_byte_count = sxml_arena_tree_has_inline_memory(configuration)
                                         ? SXML_ARENA_TREE_INLINE_BYTE_COUNT
                                         : 0;
    if (status == SXML_STATUS_OK && inline_byte_count > 0) {
        status = sxml_memory_requirements_append(
            &requirements,
            (sxml_memory_requirements){.byte_count = inline_byte_count,
                                       .alignment = alignof(max_align_t)},
            &inline_offset);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *layout = (sxml_arena_tree_layout){.requirements = requirements,
                                       .arena_offset = arena_offset,
                                       .inline_offset = inline_offset,
                                       .inline_byte_count = inline_byte_count};
    return SXML_STATUS_OK;
}

sxml_status sxml_arena_tree_memory_requirements(
    const sxml_arena_tree_configuration *configuration,
    sxml_memory_requirements *requirements) {
    sxml_arena_tree_layout layout = {.requirements = {.byte_count = 0, .alignment = 1},
                                     .arena_offset = 0,
                                     .inline_offset = 0,
                                     .inline_byte_count = 0};
    sxml_status status = sxml_arena_tree_compute_layout(configuration, &layout);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *requirements = layout.requirements;
    return SXML_STATUS_OK;
}

/* Places the arena tree; initialize and create share it. */
static sxml_status sxml_arena_tree_place(
    sxml_memory_view memory,
    const sxml_arena_tree_configuration *configuration,
    sxml_arena_tree **arena_tree) {
    sxml_arena_tree_layout layout = {.requirements = {.byte_count = 0, .alignment = 1},
                                     .arena_offset = 0,
                                     .inline_offset = 0,
                                     .inline_byte_count = 0};
    sxml_status status = sxml_arena_tree_compute_layout(configuration, &layout);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_memory_view_check_requirements(memory, layout.requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_memory_view arena_memory = {.bytes = NULL, .byte_count = 0};
    status = sxml_memory_view_slice(
        memory,
        (sxml_byte_range){.byte_offset = layout.arena_offset,
                          .byte_count = (layout.inline_byte_count > 0 ? layout.inline_offset
                                                                     : memory.byte_count) -
                                        layout.arena_offset},
        &arena_memory);
    sxml_memory_view inline_memory = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK && layout.inline_byte_count > 0) {
        status = sxml_memory_view_slice(
            memory,
            (sxml_byte_range){.byte_offset = layout.inline_offset,
                              .byte_count = layout.inline_byte_count},
            &inline_memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_arena_configuration arena_configuration =
        sxml_arena_tree_arena_configuration(configuration);
    if (layout.inline_byte_count > 0) {
        arena_configuration.initial_memory = inline_memory;
    }
    sxml_arena *arena = NULL;
    status = sxml_arena_initialize(arena_memory, &arena_configuration, &arena);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_arena_tree *new_arena_tree = placement;
    memset(new_arena_tree, 0, sizeof(*new_arena_tree));
    new_arena_tree->configuration = *configuration;
    new_arena_tree->arena = arena;
    new_arena_tree->name_pool =
        sxml_item_heap_get_name_pool(configuration->item_heap);
    new_arena_tree->phase = SXML_ARENA_TREE_PHASE_EMPTY;
    new_arena_tree->tree = NULL;
    *arena_tree = new_arena_tree;
    return SXML_STATUS_OK;
}

sxml_status sxml_arena_tree_initialize(
    sxml_memory_view memory,
    const sxml_arena_tree_configuration *configuration,
    sxml_arena_tree **arena_tree) {
    /* A tree in caller memory cannot free that memory itself. */
    if (configuration->is_destroyed_when_unreferenced) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_arena_tree_place(memory, configuration, arena_tree);
}

void sxml_arena_tree_finalize(sxml_arena_tree *arena_tree) {
    if (arena_tree == NULL) {
        return;
    }
    sxml_tree_finalize(arena_tree->tree);
    arena_tree->tree = NULL;
    sxml_arena_finalize(arena_tree->arena);
}

sxml_status sxml_arena_tree_create(
    sxml_allocator allocator,
    const sxml_arena_tree_configuration *configuration,
    sxml_arena_tree **arena_tree) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_arena_tree_memory_requirements(configuration, &requirements);
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
    sxml_arena_tree *new_arena_tree = NULL;
    status = sxml_arena_tree_place(
        (sxml_memory_view){.bytes = memory,
                           .byte_count = requirements.byte_count},
        configuration, &new_arena_tree);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_arena_tree->owning_allocator = allocator;
    *arena_tree = new_arena_tree;
    return SXML_STATUS_OK;
}

void sxml_arena_tree_destroy(sxml_arena_tree *arena_tree) {
    if (arena_tree == NULL) {
        return;
    }
    /* Finalizing may not touch the tree afterwards, so keep the allocator. */
    sxml_allocator owning_allocator = arena_tree->owning_allocator;
    sxml_arena_tree_finalize(arena_tree);
    sxml_allocator_release(owning_allocator, arena_tree);
}

/* ---------------------------------------------------------- Segments */

/* Which segment holds a node, and where in it. */
typedef struct sxml_segment_position {
    size_t segment_index;
    size_t offset;
} sxml_segment_position;

static unsigned sxml_arena_tree_highest_bit(uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return 63U - (unsigned)__builtin_clzll(value);
#else
    unsigned bit = 0;
    while (value > 1) {
        value >>= 1;
        bit += 1;
    }
    return bit;
#endif
}

/*
 * Segment k covers node numbers 2 * (2^k - 1) to 2 * (2^(k+1) - 1) - 1,
 * so node_number + 2 has its highest bit at k + 1 and the bits below it
 * are the offset.
 */
static sxml_segment_position sxml_arena_tree_locate(uint32_t node_number) {
    uint64_t shifted =
        (uint64_t)node_number + SXML_ARENA_TREE_FIRST_SEGMENT_NODE_COUNT;
    unsigned highest_bit = sxml_arena_tree_highest_bit(shifted);
    return (sxml_segment_position){
        .segment_index = highest_bit - SXML_ARENA_TREE_FIRST_SEGMENT_SHIFT,
        .offset = (size_t)(shifted - ((uint64_t)1 << highest_bit)),
    };
}

static size_t sxml_arena_tree_segment_node_count(size_t segment_index) {
    return (size_t)SXML_ARENA_TREE_FIRST_SEGMENT_NODE_COUNT << segment_index;
}

static sxml_arena_node *sxml_arena_tree_get_node(
    const sxml_arena_tree *arena_tree, uint32_t node_number) {
    sxml_segment_position position = sxml_arena_tree_locate(node_number);
    return &arena_tree->node_segments[position.segment_index][position.offset];
}

static sxml_source_location *sxml_arena_tree_get_location(
    const sxml_arena_tree *arena_tree, uint32_t node_number) {
    sxml_segment_position position = sxml_arena_tree_locate(node_number);
    return &arena_tree
                ->location_segments[position.segment_index][position.offset];
}

static sxml_status sxml_arena_tree_allocate(sxml_arena_tree *arena_tree,
                                            sxml_allocation_request request,
                                            void **memory) {
    return sxml_arena_allocate(arena_tree->arena, request, memory);
}

/* An array of element_count elements of one size and alignment. */
typedef struct sxml_array_request {
    size_t element_count;
    size_t element_byte_count;
    size_t alignment;
} sxml_array_request;

static sxml_status sxml_arena_tree_allocate_array(sxml_arena_tree *arena_tree,
                                                  sxml_array_request request,
                                                  void **memory) {
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(
        request.element_count, request.element_byte_count, &byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_arena_tree_allocate(
        arena_tree,
        (sxml_allocation_request){.byte_count = byte_count,
                                  .alignment = request.alignment},
        memory);
}

/* Makes sure the segments for node_count holding one more node exist. */
static sxml_status sxml_arena_tree_reserve_node(sxml_arena_tree *arena_tree) {
    sxml_segment_position position =
        sxml_arena_tree_locate(arena_tree->node_count);
    size_t segment_index = position.segment_index;
    size_t node_count = sxml_arena_tree_segment_node_count(segment_index);
    if (arena_tree->node_segments[segment_index] == NULL) {
        void *memory = NULL;
        sxml_status status = sxml_arena_tree_allocate_array(
            arena_tree,
            (sxml_array_request){.element_count = node_count,
                                 .element_byte_count = sizeof(sxml_arena_node),
                                 .alignment = alignof(sxml_arena_node)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        arena_tree->node_segments[segment_index] = memory;
    }
    if (arena_tree->configuration.keeps_locations && arena_tree->location_segments == NULL) {
        void *memory = NULL;
        sxml_status status = sxml_arena_tree_allocate_array(
            arena_tree,
            (sxml_array_request){.element_count = SXML_ARENA_TREE_SEGMENT_COUNT,
                                 .element_byte_count = sizeof(sxml_source_location *),
                                 .alignment = alignof(sxml_source_location *)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        memset(memory, 0, SXML_ARENA_TREE_SEGMENT_COUNT * sizeof(sxml_source_location *));
        arena_tree->location_segments = (sxml_source_location **)memory;
    }
    if (arena_tree->configuration.keeps_locations &&
        arena_tree->location_segments[segment_index] == NULL) {
        void *memory = NULL;
        sxml_status status = sxml_arena_tree_allocate_array(
            arena_tree,
            (sxml_array_request){
                .element_count = node_count,
                .element_byte_count = sizeof(sxml_source_location),
                .alignment = alignof(sxml_source_location)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        arena_tree->location_segments[segment_index] = memory;
    }
    return SXML_STATUS_OK;
}

/* --------------------------------------------------------- Text store */

static bool sxml_arena_tree_has_pending_text(
    const sxml_arena_tree *arena_tree) {
    return arena_tree->text_used_byte_count > arena_tree->pending_text_offset;
}

/* Moves the pending bytes to a block with room for more_byte_count more. */
static sxml_status sxml_arena_tree_grow_text_store(sxml_arena_tree *arena_tree,
                                                   size_t more_byte_count) {
    size_t pending_byte_count =
        arena_tree->text_used_byte_count - arena_tree->pending_text_offset;
    size_t needed_byte_count = 0;
    sxml_status status =
        sxml_size_add(pending_byte_count, more_byte_count, &needed_byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t block_byte_count = arena_tree->text_block_byte_count * 2;
    if (block_byte_count < SXML_ARENA_TREE_MINIMUM_TEXT_BLOCK_BYTE_COUNT) {
        block_byte_count = SXML_ARENA_TREE_MINIMUM_TEXT_BLOCK_BYTE_COUNT;
    }
    if (block_byte_count > SXML_ARENA_TREE_MAXIMUM_TEXT_BLOCK_BYTE_COUNT) {
        block_byte_count = SXML_ARENA_TREE_MAXIMUM_TEXT_BLOCK_BYTE_COUNT;
    }
    if (block_byte_count < needed_byte_count) {
        block_byte_count = needed_byte_count;
    }
    void *memory = NULL;
    status = sxml_arena_tree_allocate(
        arena_tree,
        (sxml_allocation_request){.byte_count = block_byte_count,
                                  .alignment = 1},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *block = memory;
    if (pending_byte_count != 0) {
        memcpy(block, arena_tree->text_block + arena_tree->pending_text_offset,
               pending_byte_count);
    }
    arena_tree->text_block = block;
    arena_tree->text_block_byte_count = block_byte_count;
    arena_tree->text_used_byte_count = pending_byte_count;
    arena_tree->pending_text_offset = 0;
    return SXML_STATUS_OK;
}

/* Appends bytes to the pending bytes of the text store. */
static sxml_status sxml_arena_tree_append_text(sxml_arena_tree *arena_tree,
                                               sxml_byte_view bytes) {
    if (bytes.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    size_t maximum = arena_tree->configuration.maximum_text_byte_count;
    if (maximum != 0 && bytes.byte_count > maximum - arena_tree->text_byte_total) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (bytes.byte_count > arena_tree->text_block_byte_count -
                               arena_tree->text_used_byte_count) {
        sxml_status status =
            sxml_arena_tree_grow_text_store(arena_tree, bytes.byte_count);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    memcpy(arena_tree->text_block + arena_tree->text_used_byte_count,
           bytes.bytes, bytes.byte_count);
    arena_tree->text_used_byte_count += bytes.byte_count;
    arena_tree->text_byte_total += bytes.byte_count;
    return SXML_STATUS_OK;
}

/* Ends the pending bytes and returns them; the store owns them. */
static sxml_byte_view sxml_arena_tree_take_text(sxml_arena_tree *arena_tree) {
    size_t byte_count =
        arena_tree->text_used_byte_count - arena_tree->pending_text_offset;
    sxml_byte_view text = {
        .bytes = byte_count == 0
                     ? NULL
                     : arena_tree->text_block + arena_tree->pending_text_offset,
        .byte_count = byte_count,
    };
    arena_tree->pending_text_offset = arena_tree->text_used_byte_count;
    return text;
}

/* Copies bytes into the text store; must not run while text is pending. */
static sxml_status sxml_arena_tree_copy_text(sxml_arena_tree *arena_tree,
                                             sxml_byte_view bytes,
                                             sxml_byte_view *copy) {
    sxml_status status = sxml_arena_tree_append_text(arena_tree, bytes);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *copy = sxml_arena_tree_take_text(arena_tree);
    return SXML_STATUS_OK;
}

/*
 * A location with its source name copied, or the empty location when
 * locations are not kept. Consecutive nodes usually share a source name,
 * so the last copy is reused when the text is equal.
 */
static sxml_status sxml_arena_tree_copy_location(
    sxml_arena_tree *arena_tree, const sxml_source_location *location,
    sxml_source_location *copy) {
    sxml_source_location new_location = {
        .source_name = {.bytes = NULL, .byte_count = 0},
        .line_number = 0,
        .column_number = 0,
        .byte_offset = 0};
    if (arena_tree->configuration.keeps_locations) {
        new_location = *location;
        if (!sxml_byte_view_equals(location->source_name,
                                   arena_tree->last_source_name)) {
            sxml_status status = sxml_arena_tree_copy_text(
                arena_tree, location->source_name,
                &arena_tree->last_source_name);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        new_location.source_name = arena_tree->last_source_name;
    }
    *copy = new_location;
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------------- Nodes */

/* A node to add: its kind, name and where it was. */
typedef struct sxml_new_node {
    sxml_node_kind node_kind;
    sxml_name_identifier node_name;
    sxml_namespace_prefix_identifier namespace_prefix;
    sxml_type_identifier type_annotation;
    uint16_t flags;
    sxml_byte_view content;
    sxml_source_location location;
} sxml_new_node;

/*
 * Adds a node after the last one: as a child of the open node, as an
 * attribute of the header element, or as the parentless root. Location
 * source names must already be copied. Writes the node's number.
 */
static sxml_status sxml_arena_tree_add_node(sxml_arena_tree *arena_tree,
                                            const sxml_new_node *new_node,
                                            uint32_t *node_number) {
    uint32_t maximum = arena_tree->configuration.maximum_node_count;
    if ((maximum != 0 && arena_tree->node_count >= maximum) ||
        arena_tree->node_count >= UINT32_MAX - 1) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    sxml_status status = sxml_arena_tree_reserve_node(arena_tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t number = arena_tree->node_count;
    sxml_arena_node *node = sxml_arena_tree_get_node(arena_tree, number);
    *node = (sxml_arena_node){
        .content = new_node->content,
        .node_name = new_node->node_name,
        .namespace_prefix = new_node->namespace_prefix,
        .type_annotation = new_node->type_annotation,
        .parent = 0,
        .last_descendant = number,
        .attribute_count = 0,
        .last_child = 0,
        .previous_sibling = 0,
        .next_sibling = 0,
        .node_kind = (uint8_t)new_node->node_kind,
        .flags = new_node->flags,
    };
    if (new_node->node_kind == SXML_NODE_KIND_ATTRIBUTE &&
        arena_tree->is_in_header) {
        sxml_arena_node *element =
            sxml_arena_tree_get_node(arena_tree, arena_tree->header_element);
        node->parent = arena_tree->header_element;
        node->flags |= SXML_ARENA_NODE_HAS_PARENT;
        element->attribute_count += 1;
    } else if (arena_tree->has_open_node) {
        sxml_arena_node *parent =
            sxml_arena_tree_get_node(arena_tree, arena_tree->open_node);
        node->parent = arena_tree->open_node;
        node->flags |= SXML_ARENA_NODE_HAS_PARENT;
        if ((parent->flags & SXML_ARENA_NODE_HAS_CHILD) != 0) {
            sxml_arena_node *previous =
                sxml_arena_tree_get_node(arena_tree, parent->last_child);
            previous->next_sibling = number;
            previous->flags |= SXML_ARENA_NODE_HAS_NEXT_SIBLING;
            node->previous_sibling = parent->last_child;
            node->flags |= SXML_ARENA_NODE_HAS_PREVIOUS_SIBLING;
        }
        parent->last_child = number;
        parent->flags |= SXML_ARENA_NODE_HAS_CHILD;
    }
    if (arena_tree->configuration.keeps_locations) {
        *sxml_arena_tree_get_location(arena_tree, number) = new_node->location;
    }
    arena_tree->node_count += 1;
    *node_number = number;
    return SXML_STATUS_OK;
}

/* Turns pending text into a text node; empty text makes none (XDM 4.0,
   section 7.5.7.1). */
static sxml_status sxml_arena_tree_flush_text(sxml_arena_tree *arena_tree) {
    if (!sxml_arena_tree_has_pending_text(arena_tree)) {
        return SXML_STATUS_OK;
    }
    /* Reserve first, so that a failure keeps the text pending. */
    sxml_status status = sxml_arena_tree_reserve_node(arena_tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t pending_offset = arena_tree->pending_text_offset;
    const sxml_new_node text_node = {
        .node_kind = SXML_NODE_KIND_TEXT,
        .node_name = {.index = 0},
        .namespace_prefix = {.index = 0},
        .type_annotation = {.index = SXML_TYPE_NONE},
        .flags = (uint16_t)((arena_tree->pending_text_is_cdata_section
                                 ? SXML_ARENA_NODE_IS_CDATA_SECTION
                                 : 0U) |
                            (arena_tree->pending_text_disables_output_escaping
                                 ? SXML_ARENA_NODE_DISABLES_OUTPUT_ESCAPING
                                 : 0U)),
        .content = sxml_arena_tree_take_text(arena_tree),
        .location = arena_tree->pending_text_location,
    };
    uint32_t node_number = 0;
    status = sxml_arena_tree_add_node(arena_tree, &text_node, &node_number);
    if (status != SXML_STATUS_OK) {
        arena_tree->pending_text_offset = pending_offset;
    } else {
        arena_tree->pending_text_is_cdata_section = false;
        arena_tree->pending_text_disables_output_escaping = false;
    }
    return status;
}

/* The parentless zero-length text node of a tree whose only events were
   empty text (XDM 4.0, section 7.5.7.1: text nodes may be empty only if
   they have no parent; D163). */
static sxml_status sxml_arena_tree_add_empty_text(sxml_arena_tree *arena_tree) {
    sxml_status status = sxml_arena_tree_reserve_node(arena_tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_new_node text_node = {
        .node_kind = SXML_NODE_KIND_TEXT,
        .node_name = {.index = 0},
        .namespace_prefix = {.index = 0},
        .type_annotation = {.index = SXML_TYPE_NONE},
        .flags = 0,
        .content = {.bytes = NULL, .byte_count = 0},
        .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                     .line_number = 0,
                     .column_number = 0,
                     .byte_offset = 0},
    };
    uint32_t node_number = 0;
    return sxml_arena_tree_add_node(arena_tree, &text_node, &node_number);
}

/* ------------------------------------------------- Namespace bindings */

/* Interns a prefix written as text, such as "xml". */
static sxml_status sxml_arena_tree_intern_prefix(
    sxml_arena_tree *arena_tree, sxml_byte_view prefix_text,
    sxml_namespace_prefix_identifier *prefix) {
    return sxml_name_pool_intern_namespace_prefix(arena_tree->name_pool,
                                                  prefix_text, prefix);
}

/* The base list: the xml binding alone. Made on first need. */
static sxml_status sxml_arena_tree_get_base_bindings(
    sxml_arena_tree *arena_tree, sxml_namespace_binding_view *bindings) {
    if (arena_tree->base_bindings.namespace_binding_count == 0) {
        sxml_namespace_prefix_identifier xml_prefix = {.index = 0};
        sxml_status status = sxml_arena_tree_intern_prefix(
            arena_tree, SXML_BYTE_VIEW_FROM_LITERAL("xml"), &xml_prefix);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        void *memory = NULL;
        status = sxml_arena_tree_allocate(
            arena_tree,
            (sxml_allocation_request){
                .byte_count = sizeof(sxml_namespace_binding),
                .alignment = alignof(sxml_namespace_binding)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_namespace_binding *binding = memory;
        *binding = (sxml_namespace_binding){
            .namespace_prefix = xml_prefix,
            .namespace_uri =
                sxml_well_known_namespace_identifier(SXML_NAMESPACE_XML),
        };
        arena_tree->base_bindings = (sxml_namespace_binding_view){
            .namespace_bindings = binding, .namespace_binding_count = 1};
    }
    *bindings = arena_tree->base_bindings;
    return SXML_STATUS_OK;
}

/* The in-scope namespaces the header element inherits. */
static sxml_status sxml_arena_tree_get_inherited_bindings(
    sxml_arena_tree *arena_tree, sxml_namespace_binding_view *bindings) {
    const sxml_arena_node *element =
        sxml_arena_tree_get_node(arena_tree, arena_tree->header_element);
    if ((element->flags & SXML_ARENA_NODE_HAS_PARENT) != 0 &&
        !arena_tree->header_ignores_parent_namespaces) {
        const sxml_arena_node *parent =
            sxml_arena_tree_get_node(arena_tree, element->parent);
        if (parent->node_kind == SXML_NODE_KIND_ELEMENT) {
            *bindings = parent->namespace_bindings;
            return SXML_STATUS_OK;
        }
    }
    return sxml_arena_tree_get_base_bindings(arena_tree, bindings);
}

/* A lookup of one prefix. */
typedef struct sxml_prefix_lookup {
    bool is_found;
    sxml_namespace_uri_identifier namespace_uri;
} sxml_prefix_lookup;

static sxml_prefix_lookup sxml_binding_view_find_prefix(
    sxml_namespace_binding_view bindings,
    sxml_namespace_prefix_identifier prefix) {
    for (size_t binding_index = 0;
         binding_index < bindings.namespace_binding_count; binding_index++) {
        sxml_namespace_binding binding =
            bindings.namespace_bindings[binding_index];
        if (binding.namespace_prefix.index == prefix.index) {
            return (sxml_prefix_lookup){.is_found = true,
                                        .namespace_uri = binding.namespace_uri};
        }
    }
    return (sxml_prefix_lookup){.is_found = false,
                                .namespace_uri = {.index = 0}};
}

static sxml_namespace_binding_view sxml_arena_tree_declared_view(
    const sxml_arena_tree *arena_tree) {
    return (sxml_namespace_binding_view){
        .namespace_bindings = arena_tree->declared_bindings,
        .namespace_binding_count = arena_tree->declared_binding_count};
}

/* The header element's view of all bindings while fixup runs: its declared
   bindings over those it inherits. */
typedef struct sxml_header_scope {
    sxml_namespace_binding_view inherited;
    sxml_namespace_binding_view declared;
} sxml_header_scope;

/* The binding of prefix in scope; an undeclaration counts as absent. */
static sxml_prefix_lookup sxml_header_scope_find(
    const sxml_header_scope *scope, sxml_namespace_prefix_identifier prefix) {
    sxml_prefix_lookup lookup =
        sxml_binding_view_find_prefix(scope->declared, prefix);
    if (!lookup.is_found) {
        lookup = sxml_binding_view_find_prefix(scope->inherited, prefix);
    }
    if (lookup.is_found &&
        lookup.namespace_uri.index == SXML_NAMESPACE_NONE) {
        lookup.is_found = false;
    }
    return lookup;
}

/* Adds a binding to the declared list (growing it in the arena). */
static sxml_status sxml_arena_tree_declare(sxml_arena_tree *arena_tree,
                                           sxml_namespace_binding binding) {
    if (arena_tree->declared_binding_count ==
        arena_tree->declared_binding_capacity) {
        size_t capacity = arena_tree->declared_binding_capacity == 0
                              ? SXML_ARENA_TREE_MINIMUM_DECLARED_CAPACITY
                              : arena_tree->declared_binding_capacity * 2;
        void *memory = NULL;
        sxml_status status = sxml_arena_tree_allocate_array(
            arena_tree,
            (sxml_array_request){
                .element_count = capacity,
                .element_byte_count = sizeof(sxml_namespace_binding),
                .alignment = alignof(sxml_namespace_binding)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (arena_tree->declared_binding_count != 0) {
            memcpy(memory, arena_tree->declared_bindings,
                   arena_tree->declared_binding_count *
                       sizeof(sxml_namespace_binding));
        }
        arena_tree->declared_bindings = memory;
        arena_tree->declared_binding_capacity = capacity;
    }
    arena_tree->declared_bindings[arena_tree->declared_binding_count] = binding;
    arena_tree->declared_binding_count += 1;
    return SXML_STATUS_OK;
}

/* Whether a prefix's text is "xml" or "xmlns". */
typedef struct sxml_reserved_prefix {
    bool is_xml;
    bool is_xmlns;
} sxml_reserved_prefix;

static sxml_status sxml_arena_tree_classify_prefix(
    const sxml_arena_tree *arena_tree,
    sxml_namespace_prefix_identifier prefix,
    sxml_reserved_prefix *reserved) {
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_name_pool_get_namespace_prefix(
        arena_tree->name_pool, prefix, &text);
    if (status != SXML_STATUS_OK) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *reserved = (sxml_reserved_prefix){
        .is_xml =
            sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("xml")),
        .is_xmlns =
            sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("xmlns")),
    };
    return SXML_STATUS_OK;
}

/*
 * Makes a prefix no binding in scope uses: ns0, ns1, ... (XSLT 4.0, section
 * 5.7.3 leaves the choice to the implementation).
 */
static sxml_status sxml_arena_tree_generate_prefix(
    sxml_arena_tree *arena_tree, const sxml_header_scope *scope,
    sxml_namespace_prefix_identifier *prefix) {
    for (;;) {
        uint8_t text[16] = {'n', 's'};
        uint8_t digits[10];
        size_t digit_count = 0;
        uint32_t number = arena_tree->generated_prefix_count;
        do {
            digits[digit_count] = (uint8_t)('0' + number % 10);
            digit_count += 1;
            number /= 10;
        } while (number != 0);
        for (size_t digit_index = 0; digit_index < digit_count; digit_index++) {
            text[2 + digit_index] = digits[digit_count - 1 - digit_index];
        }
        arena_tree->generated_prefix_count += 1;
        sxml_namespace_prefix_identifier candidate = {.index = 0};
        sxml_status status = sxml_arena_tree_intern_prefix(
            arena_tree,
            (sxml_byte_view){.bytes = text, .byte_count = 2 + digit_count},
            &candidate);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!sxml_binding_view_find_prefix(scope->declared, candidate)
                 .is_found &&
            !sxml_binding_view_find_prefix(scope->inherited, candidate)
                 .is_found) {
            *prefix = candidate;
            return SXML_STATUS_OK;
        }
    }
}

/* A name to give a binding to during fixup. */
typedef struct sxml_fixup_name {
    sxml_namespace_uri_identifier namespace_uri;
    /* in/out: the prefix, changed when another must be used. */
    sxml_namespace_prefix_identifier *namespace_prefix;
    /* Attributes in a namespace need a non-empty prefix. */
    bool needs_prefix;
} sxml_fixup_name;

/*
 * A prefix in scope bound to namespace_uri (not the default namespace when
 * needs_prefix), declared bindings first; false when there is none.
 */
static bool sxml_header_scope_find_uri(const sxml_header_scope *scope,
                                       const sxml_fixup_name *name,
                                       sxml_namespace_prefix_identifier *prefix) {
    const sxml_namespace_binding_view views[2] = {scope->declared,
                                                  scope->inherited};
    for (size_t view_index = 0; view_index < 2; view_index++) {
        sxml_namespace_binding_view view = views[view_index];
        for (size_t binding_index = 0;
             binding_index < view.namespace_binding_count; binding_index++) {
            sxml_namespace_binding binding =
                view.namespace_bindings[binding_index];
            if (binding.namespace_uri.index != name->namespace_uri.index ||
                (name->needs_prefix && binding.namespace_prefix.index == 0)) {
                continue;
            }
            /* An inherited prefix the element redeclares is not bound to
               this URI here. */
            sxml_prefix_lookup lookup =
                sxml_header_scope_find(scope, binding.namespace_prefix);
            if (lookup.is_found &&
                lookup.namespace_uri.index == name->namespace_uri.index) {
                *prefix = binding.namespace_prefix;
                return true;
            }
        }
    }
    return false;
}

/*
 * Namespace fixup for one name in a namespace (XSLT 4.0, section 5.7.3):
 * keep its prefix when it is bound to the name's URI, bind it when the
 * element has not declared it otherwise, else use another prefix bound to
 * the URI or a generated one. Every prefix a name uses ends up declared,
 * so a later name cannot rebind it.
 */
static sxml_status sxml_arena_tree_fix_name(sxml_arena_tree *arena_tree,
                                            const sxml_fixup_name *name) {
    sxml_header_scope scope = {
        .inherited = {.namespace_bindings = NULL, .namespace_binding_count = 0},
        .declared = sxml_arena_tree_declared_view(arena_tree)};
    sxml_status status =
        sxml_arena_tree_get_inherited_bindings(arena_tree, &scope.inherited);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_namespace_prefix_identifier prefix = *name->namespace_prefix;
    bool prefix_is_usable = !name->needs_prefix || prefix.index != 0;
    if (prefix_is_usable) {
        sxml_prefix_lookup declared =
            sxml_binding_view_find_prefix(scope.declared, prefix);
        if (declared.is_found &&
            declared.namespace_uri.index == name->namespace_uri.index) {
            return SXML_STATUS_OK;
        }
        if (!declared.is_found) {
            /* Bound by inheritance or not at all: declaring it here keeps
               or makes the binding and reserves the prefix. */
            return sxml_arena_tree_declare(
                arena_tree,
                (sxml_namespace_binding){.namespace_prefix = prefix,
                                         .namespace_uri = name->namespace_uri});
        }
    }
    sxml_namespace_prefix_identifier other_prefix = {.index = 0};
    if (sxml_header_scope_find_uri(&scope, name, &other_prefix)) {
        sxml_prefix_lookup declared =
            sxml_binding_view_find_prefix(scope.declared, other_prefix);
        if (!declared.is_found) {
            status = sxml_arena_tree_declare(
                arena_tree, (sxml_namespace_binding){
                                .namespace_prefix = other_prefix,
                                .namespace_uri = name->namespace_uri});
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        *name->namespace_prefix = other_prefix;
        return SXML_STATUS_OK;
    }
    status = sxml_arena_tree_generate_prefix(arena_tree, &scope, &other_prefix);
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_declare(
            arena_tree,
            (sxml_namespace_binding){.namespace_prefix = other_prefix,
                                     .namespace_uri = name->namespace_uri});
    }
    if (status == SXML_STATUS_OK) {
        *name->namespace_prefix = other_prefix;
    }
    return status;
}

/* The namespace URI of an interned name. */
static sxml_status sxml_arena_tree_get_name_uri(
    const sxml_arena_tree *arena_tree, sxml_name_identifier name,
    sxml_namespace_uri_identifier *namespace_uri) {
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    if (sxml_name_pool_get_name_parts(arena_tree->name_pool, name, &parts) !=
        SXML_STATUS_OK) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *namespace_uri = parts.namespace_uri;
    return SXML_STATUS_OK;
}

/* Fixup for the header element's own name. */
static sxml_status sxml_arena_tree_fix_element_name(
    sxml_arena_tree *arena_tree, sxml_arena_node *element) {
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    sxml_status status = sxml_arena_tree_get_name_uri(
        arena_tree, element->node_name, &namespace_uri);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (namespace_uri.index != SXML_NAMESPACE_NONE) {
        return sxml_arena_tree_fix_name(
            arena_tree,
            &(sxml_fixup_name){.namespace_uri = namespace_uri,
                               .namespace_prefix = &element->namespace_prefix,
                               .needs_prefix = false});
    }
    /* A name in no namespace has no prefix, and the default namespace
       must be undeclared for it (XSLT 4.0, section 5.7.3; XQuery 4.0:
       XQDY0102 when the element itself declares a default namespace). */
    element->namespace_prefix = (sxml_namespace_prefix_identifier){.index = 0};
    const sxml_namespace_prefix_identifier default_prefix = {.index = 0};
    sxml_prefix_lookup declared = sxml_binding_view_find_prefix(
        sxml_arena_tree_declared_view(arena_tree), default_prefix);
    if (declared.is_found) {
        return declared.namespace_uri.index == SXML_NAMESPACE_NONE
                   ? SXML_STATUS_OK
                   : sxml_arena_tree_report(
                         arena_tree, &sxml_arena_tree_conflicting_binding_error);
    }
    sxml_namespace_binding_view inherited = {.namespace_bindings = NULL,
                                             .namespace_binding_count = 0};
    status = sxml_arena_tree_get_inherited_bindings(arena_tree, &inherited);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!sxml_binding_view_find_prefix(inherited, default_prefix).is_found) {
        return SXML_STATUS_OK;
    }
    return sxml_arena_tree_declare(
        arena_tree,
        (sxml_namespace_binding){.namespace_prefix = default_prefix,
                                 .namespace_uri = {.index = SXML_NAMESPACE_NONE}});
}

/* Fixup for the attributes of the header element. */
static sxml_status sxml_arena_tree_fix_attribute_names(
    sxml_arena_tree *arena_tree, uint32_t attribute_count) {
    for (uint32_t attribute_offset = 1; attribute_offset <= attribute_count;
         attribute_offset++) {
        sxml_arena_node *attribute = sxml_arena_tree_get_node(
            arena_tree, arena_tree->header_element + attribute_offset);
        sxml_namespace_uri_identifier namespace_uri = {.index = 0};
        sxml_status status = sxml_arena_tree_get_name_uri(
            arena_tree, attribute->node_name, &namespace_uri);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (namespace_uri.index == SXML_NAMESPACE_NONE) {
            attribute->namespace_prefix =
                (sxml_namespace_prefix_identifier){.index = 0};
            continue;
        }
        status = sxml_arena_tree_fix_name(
            arena_tree,
            &(sxml_fixup_name){.namespace_uri = namespace_uri,
                               .namespace_prefix = &attribute->namespace_prefix,
                               .needs_prefix = true});
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/* True when a declared binding changes what is inherited. */
static bool sxml_arena_tree_declaration_changes_scope(
    sxml_namespace_binding_view inherited, sxml_namespace_binding declared) {
    sxml_prefix_lookup lookup =
        sxml_binding_view_find_prefix(inherited, declared.namespace_prefix);
    if (!lookup.is_found) {
        return declared.namespace_uri.index != SXML_NAMESPACE_NONE;
    }
    return lookup.namespace_uri.index != declared.namespace_uri.index;
}

/*
 * Gives the header element its in-scope namespace list: the inherited list
 * when nothing changes it, otherwise a new list, inherited bindings first
 * (replaced or removed by declarations) then new prefixes, in declaration
 * order. The order is stable, so namespace numbers are too.
 */
static sxml_status sxml_arena_tree_make_scope(sxml_arena_tree *arena_tree,
                                              sxml_arena_node *element) {
    sxml_namespace_binding_view inherited = {.namespace_bindings = NULL,
                                             .namespace_binding_count = 0};
    sxml_status status =
        sxml_arena_tree_get_inherited_bindings(arena_tree, &inherited);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_namespace_binding_view declared =
        sxml_arena_tree_declared_view(arena_tree);
    bool changes_scope = false;
    for (size_t declared_index = 0;
         declared_index < declared.namespace_binding_count; declared_index++) {
        changes_scope = changes_scope ||
                        sxml_arena_tree_declaration_changes_scope(
                            inherited,
                            declared.namespace_bindings[declared_index]);
    }
    if (!changes_scope) {
        element->namespace_bindings = inherited;
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    status = sxml_arena_tree_allocate_array(
        arena_tree,
        (sxml_array_request){
            .element_count = inherited.namespace_binding_count +
                             declared.namespace_binding_count,
            .element_byte_count = sizeof(sxml_namespace_binding),
            .alignment = alignof(sxml_namespace_binding)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_namespace_binding *bindings = memory;
    size_t binding_count = 0;
    for (size_t inherited_index = 0;
         inherited_index < inherited.namespace_binding_count;
         inherited_index++) {
        sxml_namespace_binding binding =
            inherited.namespace_bindings[inherited_index];
        sxml_prefix_lookup lookup =
            sxml_binding_view_find_prefix(declared, binding.namespace_prefix);
        if (lookup.is_found) {
            binding.namespace_uri = lookup.namespace_uri;
        }
        if (binding.namespace_uri.index != SXML_NAMESPACE_NONE) {
            bindings[binding_count] = binding;
            binding_count += 1;
        }
    }
    for (size_t declared_index = 0;
         declared_index < declared.namespace_binding_count; declared_index++) {
        sxml_namespace_binding binding =
            declared.namespace_bindings[declared_index];
        if (binding.namespace_uri.index != SXML_NAMESPACE_NONE &&
            !sxml_binding_view_find_prefix(inherited, binding.namespace_prefix)
                 .is_found) {
            bindings[binding_count] = binding;
            binding_count += 1;
        }
    }
    element->namespace_bindings = (sxml_namespace_binding_view){
        .namespace_bindings = bindings, .namespace_binding_count = binding_count};
    return SXML_STATUS_OK;
}

/* Ends the header of the open element: fixup, then its namespace list. */
static sxml_status sxml_arena_tree_close_header(sxml_arena_tree *arena_tree) {
    if (!arena_tree->is_in_header) {
        return SXML_STATUS_OK;
    }
    sxml_arena_node *element =
        sxml_arena_tree_get_node(arena_tree, arena_tree->header_element);
    sxml_status status = sxml_arena_tree_fix_element_name(arena_tree, element);
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_fix_attribute_names(arena_tree,
                                                     element->attribute_count);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_make_scope(arena_tree, element);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    arena_tree->is_in_header = false;
    arena_tree->declared_binding_count = 0;
    arena_tree->has_attribute_index = false;
    return SXML_STATUS_OK;
}

/* Ends the header and the pending text before a content event. */
static sxml_status sxml_arena_tree_prepare_content(
    sxml_arena_tree *arena_tree) {
    sxml_status status = sxml_arena_tree_close_header(arena_tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_arena_tree_flush_text(arena_tree);
}

/* ---------------------------------------------------- Attribute index */

static size_t sxml_arena_tree_attribute_slot(sxml_name_identifier name,
                                             size_t slot_count) {
    /* Fibonacci hashing spreads the dense name identifiers. */
    uint32_t hash = name.index * 2654435769U;
    return (size_t)hash & (slot_count - 1);
}

/* Enters attribute offset (from 1) into the index; the index has room. */
static void sxml_arena_tree_index_attribute(sxml_arena_tree *arena_tree,
                                            uint32_t attribute_offset) {
    const sxml_arena_node *attribute = sxml_arena_tree_get_node(
        arena_tree, arena_tree->header_element + attribute_offset);
    size_t slot_count = arena_tree->active_attribute_slot_count;
    size_t slot =
        sxml_arena_tree_attribute_slot(attribute->node_name, slot_count);
    while (arena_tree->attribute_slots[slot] != 0) {
        slot = (slot + 1) & (slot_count - 1);
    }
    arena_tree->attribute_slots[slot] = attribute_offset;
}

/*
 * Makes sure the hash index holds attribute_count attributes at most half
 * full, rebuilding it from the attribute nodes when it grows.
 */
static sxml_status sxml_arena_tree_prepare_attribute_index(
    sxml_arena_tree *arena_tree, uint32_t attribute_count) {
    size_t needed_slot_count = SXML_ARENA_TREE_MINIMUM_ATTRIBUTE_SLOT_COUNT;
    while (needed_slot_count < (size_t)attribute_count * 2) {
        needed_slot_count *= 2;
    }
    if (arena_tree->has_attribute_index &&
        arena_tree->active_attribute_slot_count >= needed_slot_count) {
        return SXML_STATUS_OK;
    }
    if (arena_tree->attribute_slot_capacity < needed_slot_count) {
        void *memory = NULL;
        sxml_status status = sxml_arena_tree_allocate_array(
            arena_tree,
            (sxml_array_request){.element_count = needed_slot_count,
                                 .element_byte_count = sizeof(uint32_t),
                                 .alignment = alignof(uint32_t)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        arena_tree->attribute_slots = memory;
        arena_tree->attribute_slot_capacity = needed_slot_count;
    }
    arena_tree->active_attribute_slot_count = needed_slot_count;
    memset(arena_tree->attribute_slots, 0,
           needed_slot_count * sizeof(uint32_t));
    const sxml_arena_node *element =
        sxml_arena_tree_get_node(arena_tree, arena_tree->header_element);
    for (uint32_t attribute_offset = 1;
         attribute_offset <= element->attribute_count; attribute_offset++) {
        sxml_arena_tree_index_attribute(arena_tree, attribute_offset);
    }
    arena_tree->has_attribute_index = true;
    return SXML_STATUS_OK;
}

/* The attribute of the header element named name, if there is one. */
typedef struct sxml_attribute_lookup {
    bool is_found;
    uint32_t node_number;
} sxml_attribute_lookup;

static sxml_attribute_lookup sxml_arena_tree_find_attribute(
    const sxml_arena_tree *arena_tree, sxml_name_identifier name) {
    const sxml_arena_node *element =
        sxml_arena_tree_get_node(arena_tree, arena_tree->header_element);
    if (!arena_tree->has_attribute_index) {
        for (uint32_t attribute_offset = 1;
             attribute_offset <= element->attribute_count; attribute_offset++) {
            uint32_t node_number = arena_tree->header_element + attribute_offset;
            if (sxml_arena_tree_get_node(arena_tree, node_number)
                    ->node_name.index == name.index) {
                return (sxml_attribute_lookup){.is_found = true,
                                               .node_number = node_number};
            }
        }
        return (sxml_attribute_lookup){.is_found = false, .node_number = 0};
    }
    size_t slot_count = arena_tree->active_attribute_slot_count;
    size_t slot = sxml_arena_tree_attribute_slot(name, slot_count);
    while (arena_tree->attribute_slots[slot] != 0) {
        uint32_t node_number =
            arena_tree->header_element + arena_tree->attribute_slots[slot];
        if (sxml_arena_tree_get_node(arena_tree, node_number)
                ->node_name.index == name.index) {
            return (sxml_attribute_lookup){.is_found = true,
                                           .node_number = node_number};
        }
        slot = (slot + 1) & (slot_count - 1);
    }
    return (sxml_attribute_lookup){.is_found = false, .node_number = 0};
}

/* ------------------------------------------------------ Builder events */

/* The ID and nilled properties of a node (XDM 4.0, sections 7.6.8 to
   7.6.10). */
typedef struct sxml_node_properties {
    bool is_id;
    bool is_idrefs;
    bool is_nilled;
} sxml_node_properties;

static uint16_t sxml_arena_tree_flags_of(sxml_node_properties properties) {
    return (uint16_t)((properties.is_id ? SXML_ARENA_NODE_IS_ID : 0U) |
                     (properties.is_idrefs ? SXML_ARENA_NODE_IS_IDREFS : 0U) |
                     (properties.is_nilled ? SXML_ARENA_NODE_IS_NILLED : 0U));
}

static sxml_arena_tree *sxml_arena_tree_from_state(void *state) {
    return state;
}

static sxml_type_identifier sxml_type_or_default(
    sxml_type_identifier type_annotation, sxml_builtin_type default_type) {
    if (type_annotation.index == SXML_TYPE_NONE) {
        return sxml_builtin_type_identifier(default_type);
    }
    return type_annotation;
}

static sxml_status sxml_arena_tree_start_document(
    void *state, const sxml_document_start_event *event) {
    sxml_arena_tree *arena_tree = sxml_arena_tree_from_state(state);
    if (arena_tree->phase != SXML_ARENA_TREE_PHASE_EMPTY) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_byte_view document_uri = {.bytes = NULL, .byte_count = 0};
    sxml_byte_view base_uri = {.bytes = NULL, .byte_count = 0};
    sxml_new_node document = {
        .node_kind = SXML_NODE_KIND_DOCUMENT,
        .node_name = {.index = 0},
        .namespace_prefix = {.index = 0},
        .type_annotation = {.index = SXML_TYPE_NONE},
        .flags = 0,
        .content = {.bytes = NULL, .byte_count = 0},
        .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                     .line_number = 0,
                     .column_number = 0,
                     .byte_offset = 0},
    };
    sxml_status status = sxml_arena_tree_copy_text(
        arena_tree, event->document_uri, &document_uri);
    if (status == SXML_STATUS_OK) {
        status =
            sxml_arena_tree_copy_text(arena_tree, event->base_uri, &base_uri);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_copy_location(arena_tree, &event->location,
                                               &document.location);
    }
    uint32_t node_number = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_add_node(arena_tree, &document, &node_number);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    arena_tree->document_uri = document_uri;
    arena_tree->base_uri = base_uri;
    arena_tree->has_open_node = true;
    arena_tree->open_node = node_number;
    arena_tree->phase = SXML_ARENA_TREE_PHASE_BUILDING;
    return SXML_STATUS_OK;
}

/* True when the open node exists and has kind node_kind. */
static bool sxml_arena_tree_open_node_is(const sxml_arena_tree *arena_tree,
                                         sxml_node_kind node_kind) {
    return arena_tree->phase == SXML_ARENA_TREE_PHASE_BUILDING &&
           arena_tree->has_open_node &&
           sxml_arena_tree_get_node(arena_tree, arena_tree->open_node)
                   ->node_kind == (uint8_t)node_kind;
}

/* Ends the open node: its subtree is complete. */
static void sxml_arena_tree_end_open_node(sxml_arena_tree *arena_tree) {
    sxml_arena_node *node =
        sxml_arena_tree_get_node(arena_tree, arena_tree->open_node);
    node->last_descendant = arena_tree->node_count - 1;
    if ((node->flags & SXML_ARENA_NODE_HAS_PARENT) != 0) {
        arena_tree->open_node = node->parent;
    } else {
        arena_tree->has_open_node = false;
        arena_tree->phase = SXML_ARENA_TREE_PHASE_COMPLETE;
    }
}

static sxml_status sxml_arena_tree_end_document(void *state) {
    sxml_arena_tree *arena_tree = sxml_arena_tree_from_state(state);
    if (!sxml_arena_tree_open_node_is(arena_tree, SXML_NODE_KIND_DOCUMENT)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_status status = sxml_arena_tree_flush_text(arena_tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_arena_tree_end_open_node(arena_tree);
    return SXML_STATUS_OK;
}

/* True when a content node (element, text, ...) may come now, as the root
   or as a child of the open node. */
static bool sxml_arena_tree_accepts_content(const sxml_arena_tree *arena_tree) {
    return arena_tree->phase == SXML_ARENA_TREE_PHASE_EMPTY ||
           (arena_tree->phase == SXML_ARENA_TREE_PHASE_BUILDING &&
            arena_tree->has_open_node);
}

static sxml_status sxml_arena_tree_start_element(
    void *state, const sxml_element_start_event *event) {
    sxml_arena_tree *arena_tree = sxml_arena_tree_from_state(state);
    if (!sxml_arena_tree_accepts_content(arena_tree)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    uint32_t maximum_depth = arena_tree->configuration.maximum_depth;
    if (maximum_depth != 0 && arena_tree->element_depth >= maximum_depth) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    sxml_status status = sxml_arena_tree_get_name_uri(
        arena_tree, event->element_name, &namespace_uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_prepare_content(arena_tree);
    }
    sxml_new_node element = {
        .node_kind = SXML_NODE_KIND_ELEMENT,
        .node_name = event->element_name,
        .namespace_prefix = event->namespace_prefix,
        .type_annotation =
            sxml_type_or_default(event->type_annotation, SXML_TYPE_XS_UNTYPED),
        .flags = sxml_arena_tree_flags_of(
            (sxml_node_properties){.is_id = event->is_id,
                                   .is_idrefs = event->is_idrefs,
                                   .is_nilled = event->is_nilled}),
        .content = {.bytes = NULL, .byte_count = 0},
        .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                     .line_number = 0,
                     .column_number = 0,
                     .byte_offset = 0},
    };
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_copy_location(arena_tree, &event->location,
                                               &element.location);
    }
    uint32_t node_number = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_add_node(arena_tree, &element, &node_number);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    arena_tree->has_open_node = true;
    arena_tree->open_node = node_number;
    arena_tree->element_depth += 1;
    arena_tree->is_in_header = true;
    arena_tree->header_element = node_number;
    arena_tree->header_ignores_parent_namespaces =
        event->ignores_parent_namespaces;
    arena_tree->declared_binding_count = 0;
    arena_tree->has_attribute_index = false;
    arena_tree->phase = SXML_ARENA_TREE_PHASE_BUILDING;
    return SXML_STATUS_OK;
}

/*
 * Checks a binding against the reserved prefixes and namespaces (Namespaces
 * in XML 1.0, section 3: xml only with its namespace, xmlns never; XQuery
 * 4.0 XQDY0101). Writes which reserved prefix the binding has.
 */
static sxml_status sxml_arena_tree_check_binding(
    const sxml_arena_tree *arena_tree, sxml_namespace_binding binding,
    sxml_reserved_prefix *reserved_prefix) {
    sxml_reserved_prefix reserved = {.is_xml = false, .is_xmlns = false};
    sxml_status status = sxml_arena_tree_classify_prefix(
        arena_tree, binding.namespace_prefix, &reserved);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    bool is_xml_namespace = binding.namespace_uri.index == SXML_NAMESPACE_XML;
    if (reserved.is_xmlns ||
        binding.namespace_uri.index == SXML_NAMESPACE_XMLNS ||
        reserved.is_xml != is_xml_namespace) {
        return sxml_arena_tree_report(arena_tree,
                                      &sxml_arena_tree_reserved_binding_error);
    }
    *reserved_prefix = reserved;
    return SXML_STATUS_OK;
}

/*
 * A namespace binding sent before anything else is a parentless namespace
 * node (XQuery 4.0, section 5.7.3.7 "Computed Namespace Constructors"): the
 * one stored namespace node a tree may have, its root (D98). Its name is
 * the prefix as a name in no namespace, 0 for the default namespace; its
 * content is the namespace URI, which must not be empty.
 */
static sxml_status sxml_arena_tree_add_parentless_namespace(
    sxml_arena_tree *arena_tree, const sxml_namespace_binding *namespace_binding) {
    sxml_reserved_prefix reserved = {.is_xml = false, .is_xmlns = false};
    sxml_status status = sxml_arena_tree_check_binding(
        arena_tree, *namespace_binding, &reserved);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (namespace_binding->namespace_uri.index == SXML_NAMESPACE_NONE) {
        return sxml_arena_tree_report(arena_tree,
                                      &sxml_arena_tree_reserved_binding_error);
    }
    sxml_byte_view prefix_text = {.bytes = NULL, .byte_count = 0};
    sxml_byte_view uri_text = {.bytes = NULL, .byte_count = 0};
    status = sxml_name_pool_get_namespace_prefix(
        arena_tree->name_pool, namespace_binding->namespace_prefix, &prefix_text);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_namespace_uri(
            arena_tree->name_pool, namespace_binding->namespace_uri, &uri_text);
    }
    if (status != SXML_STATUS_OK) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_name_identifier node_name = {.index = 0};
    if (prefix_text.byte_count > 0) {
        status = sxml_name_pool_intern_name(
            arena_tree->name_pool,
            &(sxml_name_request){.namespace_uri = sxml_well_known_namespace_identifier(
                                     SXML_NAMESPACE_NONE),
                                 .local_name = prefix_text},
            &node_name);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_new_node namespace_node = {
        .node_kind = SXML_NODE_KIND_NAMESPACE,
        .node_name = node_name,
        .namespace_prefix = {.index = 0},
        .type_annotation = {.index = SXML_TYPE_NONE},
        .flags = 0,
        .content = {.bytes = NULL, .byte_count = 0},
        .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                     .line_number = 0,
                     .column_number = 0,
                     .byte_offset = 0},
    };
    status = sxml_arena_tree_copy_text(arena_tree, uri_text,
                                       &namespace_node.content);
    uint32_t node_number = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_add_node(arena_tree, &namespace_node, &node_number);
    }
    if (status == SXML_STATUS_OK) {
        arena_tree->phase = SXML_ARENA_TREE_PHASE_COMPLETE;
    }
    return status;
}

static sxml_status sxml_arena_tree_add_namespace_binding(
    void *state, const sxml_namespace_binding *namespace_binding) {
    sxml_arena_tree *arena_tree = sxml_arena_tree_from_state(state);
    if (arena_tree->phase == SXML_ARENA_TREE_PHASE_EMPTY) {
        return sxml_arena_tree_add_parentless_namespace(arena_tree,
                                                        namespace_binding);
    }
    if (arena_tree->phase != SXML_ARENA_TREE_PHASE_BUILDING ||
        !arena_tree->is_in_header) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_reserved_prefix reserved = {.is_xml = false, .is_xmlns = false};
    sxml_status status = sxml_arena_tree_check_binding(
        arena_tree, *namespace_binding, &reserved);
    /* The xml binding is implicit in every element and not declared. */
    if (status != SXML_STATUS_OK || reserved.is_xml) {
        return status;
    }
    sxml_prefix_lookup declared = sxml_binding_view_find_prefix(
        sxml_arena_tree_declared_view(arena_tree),
        namespace_binding->namespace_prefix);
    if (declared.is_found) {
        return declared.namespace_uri.index ==
                       namespace_binding->namespace_uri.index
                   ? SXML_STATUS_OK
                   : sxml_arena_tree_report(
                         arena_tree, &sxml_arena_tree_conflicting_binding_error);
    }
    return sxml_arena_tree_declare(arena_tree, *namespace_binding);
}

/* Replaces an earlier attribute in place (XSLT 4.0, section 5.7.1). */
static void sxml_arena_tree_replace_attribute(sxml_arena_tree *arena_tree,
                                              uint32_t node_number,
                                              const sxml_new_node *attribute) {
    sxml_arena_node *node = sxml_arena_tree_get_node(arena_tree, node_number);
    node->content = attribute->content;
    node->namespace_prefix = attribute->namespace_prefix;
    node->type_annotation = attribute->type_annotation;
    node->flags = (uint16_t)((node->flags & SXML_ARENA_NODE_HAS_PARENT) |
                            attribute->flags);
    if (arena_tree->configuration.keeps_locations) {
        *sxml_arena_tree_get_location(arena_tree, node_number) =
            attribute->location;
    }
}

static sxml_status sxml_arena_tree_add_attribute(
    void *state, const sxml_attribute_event *event) {
    sxml_arena_tree *arena_tree = sxml_arena_tree_from_state(state);
    bool is_parentless = arena_tree->phase == SXML_ARENA_TREE_PHASE_EMPTY;
    if (!is_parentless && (arena_tree->phase != SXML_ARENA_TREE_PHASE_BUILDING ||
                           !arena_tree->is_in_header)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_namespace_uri_identifier namespace_uri = {.index = 0};
    sxml_status status = sxml_arena_tree_get_name_uri(
        arena_tree, event->attribute_name, &namespace_uri);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (namespace_uri.index == SXML_NAMESPACE_XMLNS) {
        return sxml_arena_tree_report(arena_tree,
                                      &sxml_arena_tree_xmlns_attribute_error);
    }
    sxml_attribute_lookup duplicate = {.is_found = false, .node_number = 0};
    if (!is_parentless) {
        duplicate =
            sxml_arena_tree_find_attribute(arena_tree, event->attribute_name);
        if (duplicate.is_found && arena_tree->configuration
                                          .duplicate_attribute_policy ==
                                      SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT) {
            return sxml_arena_tree_report(
                arena_tree, &sxml_arena_tree_duplicate_attribute_error);
        }
    }
    sxml_new_node attribute = {
        .node_kind = SXML_NODE_KIND_ATTRIBUTE,
        .node_name = event->attribute_name,
        .namespace_prefix = event->namespace_prefix,
        .type_annotation = sxml_type_or_default(event->type_annotation,
                                                SXML_TYPE_XS_UNTYPED_ATOMIC),
        .flags = sxml_arena_tree_flags_of(
            (sxml_node_properties){.is_id = event->is_id,
                                   .is_idrefs = event->is_idrefs,
                                   .is_nilled = false}),
        .content = {.bytes = NULL, .byte_count = 0},
        .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                     .line_number = 0,
                     .column_number = 0,
                     .byte_offset = 0},
    };
    uint32_t attribute_count =
        is_parentless
            ? 0
            : sxml_arena_tree_get_node(arena_tree, arena_tree->header_element)
                  ->attribute_count;
    if (!is_parentless && !duplicate.is_found &&
        attribute_count + 1 > SXML_ARENA_TREE_ATTRIBUTE_SCAN_LIMIT) {
        status = sxml_arena_tree_prepare_attribute_index(arena_tree,
                                                         attribute_count + 1);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_copy_text(arena_tree, event->attribute_value,
                                           &attribute.content);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_copy_location(arena_tree, &event->location,
                                               &attribute.location);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (duplicate.is_found) {
        sxml_arena_tree_replace_attribute(arena_tree, duplicate.node_number,
                                          &attribute);
        return SXML_STATUS_OK;
    }
    uint32_t node_number = 0;
    status = sxml_arena_tree_add_node(arena_tree, &attribute, &node_number);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (is_parentless) {
        arena_tree->phase = SXML_ARENA_TREE_PHASE_COMPLETE;
    } else if (arena_tree->has_attribute_index) {
        sxml_arena_tree_index_attribute(arena_tree,
                                        node_number - arena_tree->header_element);
    }
    return SXML_STATUS_OK;
}

static sxml_status sxml_arena_tree_end_element(void *state) {
    sxml_arena_tree *arena_tree = sxml_arena_tree_from_state(state);
    if (!sxml_arena_tree_open_node_is(arena_tree, SXML_NODE_KIND_ELEMENT)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_status status = sxml_arena_tree_prepare_content(arena_tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    arena_tree->element_depth -= 1;
    sxml_arena_tree_end_open_node(arena_tree);
    return SXML_STATUS_OK;
}

static sxml_status sxml_arena_tree_add_text(void *state,
                                            const sxml_text_event *event) {
    sxml_arena_tree *arena_tree = sxml_arena_tree_from_state(state);
    if (arena_tree->phase != SXML_ARENA_TREE_PHASE_TOP_TEXT &&
        !sxml_arena_tree_accepts_content(arena_tree)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_status status = sxml_arena_tree_close_header(arena_tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (arena_tree->phase == SXML_ARENA_TREE_PHASE_EMPTY) {
        arena_tree->phase = SXML_ARENA_TREE_PHASE_TOP_TEXT;
    }
    if (event->text.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    /* Inside a parent, text that differs in disables_output_escaping
       starts a node of its own: the property belongs to each character
       (XSLT 4.0, section 26.1.8; D188). Parentless text keeps one node. */
    if (sxml_arena_tree_has_pending_text(arena_tree) &&
        arena_tree->phase != SXML_ARENA_TREE_PHASE_TOP_TEXT &&
        event->disables_output_escaping != arena_tree->pending_text_disables_output_escaping) {
        status = sxml_arena_tree_flush_text(arena_tree);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (!sxml_arena_tree_has_pending_text(arena_tree)) {
        /* A run's node is located where its first text is. The source name
           is copied before the run starts, so it is not part of it. */
        status = sxml_arena_tree_copy_location(
            arena_tree, &event->location, &arena_tree->pending_text_location);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    status = sxml_arena_tree_append_text(arena_tree, event->text);
    if (status == SXML_STATUS_OK && event->is_cdata_section) {
        arena_tree->pending_text_is_cdata_section = true;
    }
    if (status == SXML_STATUS_OK && event->disables_output_escaping) {
        arena_tree->pending_text_disables_output_escaping = true;
    }
    return status;
}

/* Adds a comment or processing-instruction node as root or child. */
static sxml_status sxml_arena_tree_add_leaf(sxml_arena_tree *arena_tree,
                                            sxml_new_node *leaf,
                                            const sxml_source_location *location) {
    if (!sxml_arena_tree_accepts_content(arena_tree)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_status status = sxml_arena_tree_prepare_content(arena_tree);
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_copy_text(arena_tree, leaf->content,
                                           &leaf->content);
    }
    if (status == SXML_STATUS_OK) {
        status =
            sxml_arena_tree_copy_location(arena_tree, location, &leaf->location);
    }
    uint32_t node_number = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_add_node(arena_tree, leaf, &node_number);
    }
    if (status == SXML_STATUS_OK &&
        arena_tree->phase == SXML_ARENA_TREE_PHASE_EMPTY) {
        arena_tree->phase = SXML_ARENA_TREE_PHASE_COMPLETE;
    }
    return status;
}

static sxml_status sxml_arena_tree_add_comment(
    void *state, const sxml_comment_event *event) {
    sxml_new_node comment = {
        .node_kind = SXML_NODE_KIND_COMMENT,
        .node_name = {.index = 0},
        .namespace_prefix = {.index = 0},
        .type_annotation = {.index = SXML_TYPE_NONE},
        .flags = 0,
        .content = event->comment,
        .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                     .line_number = 0,
                     .column_number = 0,
                     .byte_offset = 0},
    };
    return sxml_arena_tree_add_leaf(sxml_arena_tree_from_state(state), &comment,
                                    &event->location);
}

static sxml_status sxml_arena_tree_add_processing_instruction(
    void *state, const sxml_processing_instruction_event *event) {
    sxml_arena_tree *arena_tree = sxml_arena_tree_from_state(state);
    if (event->target.byte_count == 0 ||
        !sxml_arena_tree_accepts_content(arena_tree)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* The node name is the target, a name in no namespace (XDM 4.0,
       section 7.5.6). */
    sxml_name_identifier target_name = {.index = 0};
    sxml_status status = sxml_name_pool_intern_name(
        arena_tree->name_pool,
        &(sxml_name_request){.namespace_uri = sxml_well_known_namespace_identifier(
                                 SXML_NAMESPACE_NONE),
                             .local_name = event->target},
        &target_name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_new_node instruction = {
        .node_kind = SXML_NODE_KIND_PROCESSING_INSTRUCTION,
        .node_name = target_name,
        .namespace_prefix = {.index = 0},
        .type_annotation = {.index = SXML_TYPE_NONE},
        .flags = 0,
        .content = event->data,
        .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                     .line_number = 0,
                     .column_number = 0,
                     .byte_offset = 0},
    };
    return sxml_arena_tree_add_leaf(arena_tree, &instruction, &event->location);
}

static sxml_status sxml_arena_tree_add_unparsed_entity(
    void *state, const sxml_unparsed_entity *entity) {
    sxml_arena_tree *arena_tree = sxml_arena_tree_from_state(state);
    /* Unparsed entities come from the DTD, before the document's content. */
    if (!sxml_arena_tree_open_node_is(arena_tree, SXML_NODE_KIND_DOCUMENT) ||
        arena_tree->node_count != 1 ||
        sxml_arena_tree_has_pending_text(arena_tree)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    void *memory = NULL;
    sxml_status status = sxml_arena_tree_allocate(
        arena_tree,
        (sxml_allocation_request){
            .byte_count = sizeof(sxml_arena_unparsed_entity),
            .alignment = alignof(sxml_arena_unparsed_entity)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_arena_unparsed_entity *copy = memory;
    status = sxml_arena_tree_copy_text(arena_tree, entity->entity_name,
                                       &copy->entity.entity_name);
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_copy_text(arena_tree, entity->system_identifier,
                                           &copy->entity.system_identifier);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_copy_text(arena_tree, entity->public_identifier,
                                           &copy->entity.public_identifier);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    copy->next_entity = arena_tree->newest_entity;
    arena_tree->newest_entity = copy;
    return SXML_STATUS_OK;
}

static const sxml_tree_builder_operations sxml_arena_tree_builder_operations = {
    .start_document = sxml_arena_tree_start_document,
    .end_document = sxml_arena_tree_end_document,
    .start_element = sxml_arena_tree_start_element,
    .add_namespace_binding = sxml_arena_tree_add_namespace_binding,
    .add_attribute = sxml_arena_tree_add_attribute,
    .end_element = sxml_arena_tree_end_element,
    .add_text = sxml_arena_tree_add_text,
    .add_comment = sxml_arena_tree_add_comment,
    .add_processing_instruction = sxml_arena_tree_add_processing_instruction,
    .add_unparsed_entity = sxml_arena_tree_add_unparsed_entity,
};

sxml_tree_builder sxml_arena_tree_as_tree_builder(sxml_arena_tree *arena_tree) {
    return (sxml_tree_builder){
        .state = arena_tree,
        .operations = &sxml_arena_tree_builder_operations,
    };
}

/* ----------------------------------------------------- Tree operations */

static sxml_status sxml_arena_tree_describe_tree(
    void *state, sxml_tree_description *description) {
    const sxml_arena_tree *arena_tree = sxml_arena_tree_from_state(state);
    *description = (sxml_tree_description){
        .node_count = arena_tree->node_count,
        .document_uri = arena_tree->document_uri,
        .base_uri = arena_tree->base_uri,
        .name_pool = arena_tree->name_pool,
    };
    return SXML_STATUS_OK;
}

static bool sxml_arena_node_has_flag(const sxml_arena_node *node,
                                     unsigned flag) {
    return (node->flags & flag) != 0;
}

static sxml_status sxml_arena_tree_describe_node(
    void *state, sxml_node_index node_index,
    sxml_node_description *description) {
    const sxml_arena_tree *arena_tree = sxml_arena_tree_from_state(state);
    const sxml_arena_node *node =
        sxml_arena_tree_get_node(arena_tree, node_index.index);
    bool has_children = sxml_arena_node_has_flag(node, SXML_ARENA_NODE_HAS_CHILD);
    *description = (sxml_node_description){
        .node_kind = (sxml_node_kind)node->node_kind,
        .node_name = node->node_name,
        .namespace_prefix = node->namespace_prefix,
        .has_parent = sxml_arena_node_has_flag(node, SXML_ARENA_NODE_HAS_PARENT),
        .parent = {.index = node->parent},
        .last_descendant = {.index = node->last_descendant},
        .attribute_count = node->attribute_count,
        .has_children = has_children,
        .first_child = {.index = has_children ? node_index.index + 1 +
                                                    node->attribute_count
                                              : 0},
        .last_child = {.index = node->last_child},
        .has_previous_sibling =
            sxml_arena_node_has_flag(node, SXML_ARENA_NODE_HAS_PREVIOUS_SIBLING),
        .previous_sibling = {.index = node->previous_sibling},
        .has_next_sibling =
            sxml_arena_node_has_flag(node, SXML_ARENA_NODE_HAS_NEXT_SIBLING),
        .next_sibling = {.index = node->next_sibling},
        .type_annotation = node->type_annotation,
        .is_id = sxml_arena_node_has_flag(node, SXML_ARENA_NODE_IS_ID),
        .is_idrefs = sxml_arena_node_has_flag(node, SXML_ARENA_NODE_IS_IDREFS),
        .is_nilled = sxml_arena_node_has_flag(node, SXML_ARENA_NODE_IS_NILLED),
        .is_cdata_section = sxml_arena_node_has_flag(node, SXML_ARENA_NODE_IS_CDATA_SECTION),
        .disables_output_escaping =
            sxml_arena_node_has_flag(node, SXML_ARENA_NODE_DISABLES_OUTPUT_ESCAPING),
    };
    return SXML_STATUS_OK;
}

static sxml_status sxml_arena_tree_get_node_content(void *state,
                                                    sxml_node_index node_index,
                                                    sxml_byte_view *content) {
    const sxml_arena_tree *arena_tree = sxml_arena_tree_from_state(state);
    const sxml_arena_node *node =
        sxml_arena_tree_get_node(arena_tree, node_index.index);
    if (node->node_kind == SXML_NODE_KIND_DOCUMENT ||
        node->node_kind == SXML_NODE_KIND_ELEMENT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *content = node->content;
    return SXML_STATUS_OK;
}

static sxml_status sxml_arena_tree_get_in_scope_namespaces(
    void *state, sxml_node_index element,
    sxml_namespace_binding_view *namespace_bindings) {
    const sxml_arena_tree *arena_tree = sxml_arena_tree_from_state(state);
    const sxml_arena_node *node =
        sxml_arena_tree_get_node(arena_tree, element.index);
    if (node->node_kind != SXML_NODE_KIND_ELEMENT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *namespace_bindings = node->namespace_bindings;
    return SXML_STATUS_OK;
}

static sxml_status sxml_arena_tree_get_node_location(
    void *state, sxml_node_index node_index, sxml_source_location *location) {
    const sxml_arena_tree *arena_tree = sxml_arena_tree_from_state(state);
    if (!arena_tree->configuration.keeps_locations) {
        *location = (sxml_source_location){
            .source_name = {.bytes = NULL, .byte_count = 0},
            .line_number = 0,
            .column_number = 0,
            .byte_offset = 0};
        return SXML_STATUS_OK;
    }
    *location = *sxml_arena_tree_get_location(arena_tree, node_index.index);
    return SXML_STATUS_OK;
}

static sxml_status sxml_arena_tree_find_unparsed_entity(
    void *state, sxml_unparsed_entity *entity) {
    const sxml_arena_tree *arena_tree = sxml_arena_tree_from_state(state);
    for (const sxml_arena_unparsed_entity *candidate =
             arena_tree->newest_entity;
         candidate != NULL; candidate = candidate->next_entity) {
        if (sxml_byte_view_equals(candidate->entity.entity_name,
                                  entity->entity_name)) {
            *entity = candidate->entity;
            return SXML_STATUS_OK;
        }
    }
    return SXML_STATUS_INDEX_OUT_OF_RANGE;
}

static const sxml_tree_operations sxml_arena_tree_operations = {
    .describe_tree = sxml_arena_tree_describe_tree,
    .describe_node = sxml_arena_tree_describe_node,
    .get_node_content = sxml_arena_tree_get_node_content,
    .get_in_scope_namespaces = sxml_arena_tree_get_in_scope_namespaces,
    .get_typed_value = NULL,
    .get_location = sxml_arena_tree_get_node_location,
    .find_unparsed_entity = sxml_arena_tree_find_unparsed_entity,
    .find_element_by_id = NULL,
};

/* ---------------------------------------------------------- Finishing */

static void sxml_arena_tree_release_operation(void *state, sxml_tree *tree) {
    (void)tree;
    sxml_arena_tree_destroy(sxml_arena_tree_from_state(state));
}

static const sxml_tree_owner_operations sxml_arena_tree_owner_operations = {
    .release = sxml_arena_tree_release_operation,
};

sxml_status sxml_arena_tree_finish(sxml_arena_tree *arena_tree,
                                   sxml_tree **tree) {
    if (arena_tree->phase == SXML_ARENA_TREE_PHASE_TOP_TEXT) {
        sxml_status status = sxml_arena_tree_flush_text(arena_tree);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (arena_tree->node_count == 0) {
            /* Only empty text: a zero-length text node, which XDM allows
               for a text node without a parent (XDM 4.0, section 7.5.7.1;
               D163). */
            status = sxml_arena_tree_add_empty_text(arena_tree);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        arena_tree->phase = SXML_ARENA_TREE_PHASE_COMPLETE;
    }
    if (arena_tree->phase != SXML_ARENA_TREE_PHASE_COMPLETE) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* A parentless element, attribute, text, comment or processing
       instruction takes the configured base URI; a document took its own
       from start_document (D98). */
    if (sxml_arena_tree_get_node(arena_tree, 0)->node_kind !=
            SXML_NODE_KIND_DOCUMENT &&
        arena_tree->base_uri.byte_count == 0 &&
        arena_tree->configuration.base_uri.byte_count > 0) {
        /* An allocation of its own, not the text store: the store's block
           would grow (doubling) for these bytes alone, which every small
           constructed tree would pay (D163). */
        void *memory = NULL;
        sxml_status status = sxml_arena_tree_allocate(
            arena_tree,
            (sxml_allocation_request){
                .byte_count = arena_tree->configuration.base_uri.byte_count, .alignment = 1},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        memcpy(memory, arena_tree->configuration.base_uri.bytes,
               arena_tree->configuration.base_uri.byte_count);
        arena_tree->base_uri = (sxml_byte_view){
            .bytes = memory, .byte_count = arena_tree->configuration.base_uri.byte_count};
    }
    bool is_self_destroying =
        arena_tree->configuration.is_destroyed_when_unreferenced;
    const sxml_tree_configuration tree_configuration = {
        .state = arena_tree,
        .operations = &sxml_arena_tree_operations,
        .item_heap = arena_tree->configuration.item_heap,
        .owner = {.state = is_self_destroying ? arena_tree : NULL,
                  .operations = is_self_destroying
                                    ? &sxml_arena_tree_owner_operations
                                    : NULL},
    };
    sxml_tree *new_tree = NULL;
    void *tree_memory = &arena_tree->tree_storage;
    sxml_status status = sxml_tree_initialize(
        (sxml_memory_view){.bytes = tree_memory,
                           .byte_count = sizeof(arena_tree->tree_storage)},
        &tree_configuration, &new_tree);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    arena_tree->tree = new_tree;
    arena_tree->phase = SXML_ARENA_TREE_PHASE_FINISHED;
    *tree = new_tree;
    return SXML_STATUS_OK;
}
