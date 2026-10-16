/*
 * The item heap, cells, generic items, the raised error, vector sequences,
 * the sequence read interface and the sequence builder (include/sxml/xdm.h,
 * src/xdm_internal.h; D33, D34, D41, D60).
 *
 * Golden file: src/memory.c (lifecycle, checked sizes, comments).
 *
 * Specification: XDM 4.0, section 3 (items and sequences); XPath 4.0,
 * section 2.4.2 (error values, for the raised error).
 */
#include "xdm_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------ Item heap */

struct sxml_item_heap {
    sxml_item_heap_configuration configuration;
    /* The allocator sxml_item_heap_create used; the null allocator for a
       heap placed by sxml_item_heap_initialize. */
    sxml_allocator owning_allocator;
    uint64_t live_cell_count;
    uint64_t live_byte_count;
    uint64_t issued_tree_identifier_count;
    /* Cells whose last reference is gone and whose contents are not yet
       released; see "Cells" in src/xdm_internal.h. */
    sxml_cell *pending_cells;
    bool is_releasing_cells;
    sxml_decimal_configuration decimal_configuration;
    /* The raised error (D41): its copied text in one block, and its
       value. */
    bool has_raised_error;
    sxml_heap_bytes raised_error_text;
    bool raised_error_has_value;
    sxml_sequence raised_error_value;
};

static bool sxml_item_heap_configuration_is_valid(
    const sxml_item_heap_configuration *configuration) {
    return !sxml_allocator_is_null(configuration->allocator) &&
           configuration->name_pool != NULL &&
           configuration->type_registry != NULL;
}

sxml_status sxml_item_heap_memory_requirements(
    const sxml_item_heap_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (!sxml_item_heap_configuration_is_valid(configuration)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){
        .byte_count = sizeof(struct sxml_item_heap),
        .alignment = alignof(struct sxml_item_heap),
    };
    return SXML_STATUS_OK;
}

sxml_status sxml_item_heap_initialize(
    sxml_memory_view memory, const sxml_item_heap_configuration *configuration,
    sxml_item_heap **item_heap) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_item_heap_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_memory_view_check_requirements(memory, requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_item_heap *new_heap = placement;
    *new_heap = (sxml_item_heap){
        .configuration = *configuration,
        .owning_allocator = {.state = NULL, .operations = NULL},
        .live_cell_count = 0,
        .live_byte_count = 0,
        .issued_tree_identifier_count = 0,
        .pending_cells = NULL,
        .is_releasing_cells = false,
        .has_raised_error = false,
        .raised_error_text = {.memory = NULL, .byte_count = 0},
        .raised_error_has_value = false,
        .raised_error_value = {.content = {.representation = 0}},
    };
    /* Decimal coefficients go through the heap so that they are counted
       and limited like cells (D57 sets the defaults). */
    new_heap->decimal_configuration = (sxml_decimal_configuration){
        .allocator = sxml_item_heap_as_allocator(new_heap),
        .maximum_digit_count = SXML_DECIMAL_DEFAULT_MAXIMUM_DIGIT_COUNT,
        .division_digit_count = SXML_DECIMAL_DEFAULT_DIVISION_DIGIT_COUNT,
    };
    *item_heap = new_heap;
    return SXML_STATUS_OK;
}

void sxml_item_heap_finalize(sxml_item_heap *item_heap) {
    if (item_heap == NULL) {
        return;
    }
    sxml_item_heap_clear_raised_error(item_heap);
}

sxml_status sxml_item_heap_create(
    sxml_allocator allocator, const sxml_item_heap_configuration *configuration,
    sxml_item_heap **item_heap) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_item_heap_memory_requirements(configuration, &requirements);
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
    sxml_item_heap *new_heap = NULL;
    status = sxml_item_heap_initialize(
        (sxml_memory_view){.bytes = memory,
                           .byte_count = requirements.byte_count},
        configuration, &new_heap);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_heap->owning_allocator = allocator;
    *item_heap = new_heap;
    return SXML_STATUS_OK;
}

void sxml_item_heap_destroy(sxml_item_heap *item_heap) {
    if (item_heap == NULL) {
        return;
    }
    sxml_allocator owning_allocator = item_heap->owning_allocator;
    sxml_item_heap_finalize(item_heap);
    sxml_allocator_release(owning_allocator, item_heap);
}

sxml_item_heap_counts sxml_item_heap_read_counts(
    const sxml_item_heap *item_heap) {
    return (sxml_item_heap_counts){
        .live_cell_count = item_heap->live_cell_count,
        .live_byte_count = item_heap->live_byte_count,
        .issued_tree_identifier_count =
            item_heap->issued_tree_identifier_count,
    };
}

sxml_tree_identifier sxml_item_heap_issue_tree_identifier(
    sxml_item_heap *item_heap) {
    /* Identifiers start at 1 and only grow, so they order trees by the time
       they were made (XDM 4.0, section 6.2: an implementation-dependent
       but stable order across trees; D39). */
    item_heap->issued_tree_identifier_count += 1;
    return (sxml_tree_identifier){
        .value = item_heap->issued_tree_identifier_count};
}

sxml_name_pool *sxml_item_heap_get_name_pool(const sxml_item_heap *item_heap) {
    return item_heap->configuration.name_pool;
}

sxml_type_registry *sxml_item_heap_get_type_registry(
    const sxml_item_heap *item_heap) {
    return item_heap->configuration.type_registry;
}

const sxml_decimal_configuration *sxml_item_heap_get_decimal_configuration(
    const sxml_item_heap *item_heap) {
    return &item_heap->decimal_configuration;
}

/* --------------------------------------------------------- Heap memory */

sxml_status sxml_item_heap_allocate_bytes(sxml_item_heap *item_heap,
                                          sxml_allocation_request request,
                                          void **memory) {
    size_t maximum_byte_count = item_heap->configuration.maximum_byte_count;
    /* live_byte_count never exceeds a bound, so the subtraction is safe. */
    if (maximum_byte_count != 0 &&
        (uint64_t)request.byte_count >
            (uint64_t)maximum_byte_count - item_heap->live_byte_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    void *allocation = NULL;
    sxml_status status = sxml_allocator_allocate(
        item_heap->configuration.allocator, request, &allocation);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    item_heap->live_byte_count += request.byte_count;
    *memory = allocation;
    return SXML_STATUS_OK;
}

void sxml_item_heap_release_bytes(sxml_item_heap *item_heap,
                                  sxml_heap_bytes bytes) {
    if (bytes.memory == NULL) {
        return;
    }
    item_heap->live_byte_count -= bytes.byte_count;
    sxml_allocator_release(item_heap->configuration.allocator, bytes.memory);
}

/*
 * The hidden header in front of every block of sxml_item_heap_as_allocator:
 * where the block starts and how large it is, so release needs no size.
 */
typedef struct sxml_counted_block_header {
    size_t header_byte_count;
    size_t block_byte_count;
} sxml_counted_block_header;

static sxml_status sxml_item_heap_allocate_operation(
    void *state, sxml_allocation_request request, void **memory) {
    sxml_item_heap *item_heap = state;
    size_t alignment = request.alignment > alignof(sxml_counted_block_header)
                           ? request.alignment
                           : alignof(sxml_counted_block_header);
    /* The header sits right before the returned memory, which stays
       aligned because the header is padded to a multiple of alignment. */
    size_t header_byte_count = 0;
    sxml_status status = sxml_size_align_up(
        sizeof(sxml_counted_block_header), alignment, &header_byte_count);
    size_t block_byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_add(header_byte_count, request.byte_count,
                               &block_byte_count);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *block = NULL;
    status = sxml_item_heap_allocate_bytes(
        item_heap,
        (sxml_allocation_request){.byte_count = block_byte_count,
                                  .alignment = alignment},
        &block);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *usable = (uint8_t *)block + header_byte_count;
    sxml_counted_block_header header = {
        .header_byte_count = header_byte_count,
        .block_byte_count = block_byte_count,
    };
    memcpy(usable - sizeof(header), &header, sizeof(header));
    *memory = usable;
    return SXML_STATUS_OK;
}

static void sxml_item_heap_release_operation(void *state, void *memory) {
    sxml_item_heap *item_heap = state;
    uint8_t *usable = memory;
    sxml_counted_block_header header = {.header_byte_count = 0,
                                        .block_byte_count = 0};
    memcpy(&header, usable - sizeof(header), sizeof(header));
    sxml_item_heap_release_bytes(
        item_heap,
        (sxml_heap_bytes){.memory = usable - header.header_byte_count,
                          .byte_count = header.block_byte_count});
}

static const sxml_allocator_operations sxml_item_heap_allocator_operations = {
    .allocate = sxml_item_heap_allocate_operation,
    .release = sxml_item_heap_release_operation,
};

sxml_allocator sxml_item_heap_as_allocator(sxml_item_heap *item_heap) {
    return (sxml_allocator){
        .state = item_heap,
        .operations = &sxml_item_heap_allocator_operations,
    };
}

/* ------------------------------------------------------------------ Cells */

sxml_status sxml_item_heap_allocate_cell(sxml_item_heap *item_heap,
                                         const sxml_cell_request *request,
                                         sxml_cell **cell) {
    if (request->cell_class == NULL ||
        request->byte_count < sizeof(sxml_cell) ||
        !sxml_alignment_is_valid(request->alignment) ||
        request->alignment < alignof(sxml_cell)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    void *memory = NULL;
    sxml_status status = sxml_item_heap_allocate_bytes(
        item_heap,
        (sxml_allocation_request){.byte_count = request->byte_count,
                                  .alignment = request->alignment},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_cell *new_cell = memory;
    *new_cell = (sxml_cell){
        .cell_class = request->cell_class,
        .item_heap = item_heap,
        .byte_count = request->byte_count,
        .state = {.reference_count = 1},
    };
    item_heap->live_cell_count += 1;
    *cell = new_cell;
    return SXML_STATUS_OK;
}

/* Gives a cell's memory back; its contents are already released. */
static void sxml_item_heap_free_cell(sxml_cell *cell) {
    sxml_item_heap *item_heap = cell->item_heap;
    item_heap->live_cell_count -= 1;
    sxml_item_heap_release_bytes(
        item_heap,
        (sxml_heap_bytes){.memory = cell, .byte_count = cell->byte_count});
}

void sxml_item_heap_discard_cell(sxml_cell *cell) {
    if (cell == NULL) {
        return;
    }
    sxml_item_heap_free_cell(cell);
}

void sxml_cell_retain(sxml_cell *cell) {
    if (cell->state.reference_count == SXML_CELL_IMMORTAL_REFERENCE_COUNT) {
        return;
    }
    /* A count that reaches the maximum stays there: the cell becomes
       immortal rather than overflowing (D33). */
    cell->state.reference_count += 1;
}

/*
 * Frees the cells on the pending list until it is empty. Releasing a cell's
 * contents may put more cells on the list; they are freed in this loop
 * rather than by recursion, so the C stack stays flat however deep the
 * value was.
 */
static void sxml_item_heap_free_pending_cells(sxml_item_heap *item_heap) {
    item_heap->is_releasing_cells = true;
    while (item_heap->pending_cells != NULL) {
        sxml_cell *cell = item_heap->pending_cells;
        item_heap->pending_cells = cell->state.next_pending_cell;
        if (cell->cell_class->release_contents != NULL) {
            cell->cell_class->release_contents(cell);
        }
        sxml_item_heap_free_cell(cell);
    }
    item_heap->is_releasing_cells = false;
}

void sxml_cell_release(sxml_cell *cell) {
    if (cell->state.reference_count == SXML_CELL_IMMORTAL_REFERENCE_COUNT) {
        return;
    }
    cell->state.reference_count -= 1;
    if (cell->state.reference_count != 0) {
        return;
    }
    sxml_item_heap *item_heap = cell->item_heap;
    cell->state.next_pending_cell = item_heap->pending_cells;
    item_heap->pending_cells = cell;
    /* A release from within release_contents only queues its cell; the
       outermost release frees the whole list. */
    if (!item_heap->is_releasing_cells) {
        sxml_item_heap_free_pending_cells(item_heap);
    }
}

/* ----------------------------------------------------------------- Items */

sxml_item_kind sxml_item_get_kind(sxml_item item) {
    switch (sxml_item_get_representation(item)) {
    case SXML_ITEM_REPRESENTATION_NODE:
        return SXML_ITEM_KIND_NODE;
    case SXML_ITEM_REPRESENTATION_JSON_NODE_CELL:
        return SXML_ITEM_KIND_JSON_NODE;
    case SXML_ITEM_REPRESENTATION_FUNCTION_CELL:
        return SXML_ITEM_KIND_FUNCTION;
    case SXML_ITEM_REPRESENTATION_MAP_CELL:
        return SXML_ITEM_KIND_MAP;
    case SXML_ITEM_REPRESENTATION_ARRAY_CELL:
        return SXML_ITEM_KIND_ARRAY;
    case SXML_ITEM_REPRESENTATION_NONE:
    case SXML_ITEM_REPRESENTATION_BOOLEAN:
    case SXML_ITEM_REPRESENTATION_INTEGER:
    case SXML_ITEM_REPRESENTATION_FLOAT:
    case SXML_ITEM_REPRESENTATION_DOUBLE:
    case SXML_ITEM_REPRESENTATION_QNAME:
    case SXML_ITEM_REPRESENTATION_ATOMIC_CELL:
    case SXML_ITEM_REPRESENTATION_VECTOR_CELL:
    case SXML_ITEM_REPRESENTATION_INTEGER_RANGE_CELL:
    case SXML_ITEM_REPRESENTATION_CONCATENATION_CELL:
    case SXML_ITEM_REPRESENTATION_LAZY_CELL:
        return SXML_ITEM_KIND_ATOMIC;
    }
    return SXML_ITEM_KIND_ATOMIC;
}

bool sxml_item_is_function_item(sxml_item item) {
    /* XDM 4.0, section 8: maps and arrays are function items. */
    sxml_item_representation representation =
        sxml_item_get_representation(item);
    return representation == SXML_ITEM_REPRESENTATION_FUNCTION_CELL ||
           representation == SXML_ITEM_REPRESENTATION_MAP_CELL ||
           representation == SXML_ITEM_REPRESENTATION_ARRAY_CELL;
}

bool sxml_item_is_generic_node(sxml_item item) {
    sxml_item_representation representation =
        sxml_item_get_representation(item);
    return representation == SXML_ITEM_REPRESENTATION_NODE ||
           representation == SXML_ITEM_REPRESENTATION_JSON_NODE_CELL;
}

sxml_type_identifier sxml_item_get_type_annotation(sxml_item item) {
    switch (sxml_item_get_representation(item)) {
    case SXML_ITEM_REPRESENTATION_BOOLEAN:
    case SXML_ITEM_REPRESENTATION_INTEGER:
    case SXML_ITEM_REPRESENTATION_FLOAT:
    case SXML_ITEM_REPRESENTATION_DOUBLE:
    case SXML_ITEM_REPRESENTATION_QNAME:
    case SXML_ITEM_REPRESENTATION_ATOMIC_CELL:
    case SXML_ITEM_REPRESENTATION_FUNCTION_CELL:
    case SXML_ITEM_REPRESENTATION_MAP_CELL:
    case SXML_ITEM_REPRESENTATION_ARRAY_CELL:
        return (sxml_type_identifier){.index = item.type_or_node_index};
    case SXML_ITEM_REPRESENTATION_NONE:
    case SXML_ITEM_REPRESENTATION_NODE:
    case SXML_ITEM_REPRESENTATION_JSON_NODE_CELL:
    case SXML_ITEM_REPRESENTATION_VECTOR_CELL:
    case SXML_ITEM_REPRESENTATION_INTEGER_RANGE_CELL:
    case SXML_ITEM_REPRESENTATION_CONCATENATION_CELL:
    case SXML_ITEM_REPRESENTATION_LAZY_CELL:
        return sxml_builtin_type_identifier(SXML_TYPE_NONE);
    }
    return sxml_builtin_type_identifier(SXML_TYPE_NONE);
}

/*
 * A node item holds a reference to its tree (D33, D39). The tree's count
 * belongs to src/tree.c, which packet 1.11 adds after this file; until
 * then no node item can be made (sxml_node_item_make arrives with packet
 * 1.13), and these two calls are completed by packet 1.11 as recorded in
 * D60: sxml_tree_retain(tree) and sxml_tree_release(tree).
 */
static void sxml_node_item_retain_tree(sxml_tree *tree) { (void)tree; }

static void sxml_node_item_release_tree(sxml_tree *tree) { (void)tree; }

void sxml_item_retain(sxml_item item) {
    if (sxml_item_has_cell(item)) {
        sxml_cell_retain(sxml_item_get_cell(item));
    } else if (item.representation == SXML_ITEM_REPRESENTATION_NODE) {
        sxml_node_item_retain_tree(item.payload.tree);
    }
}

void sxml_item_release(sxml_item item) {
    if (sxml_item_has_cell(item)) {
        sxml_cell_release(sxml_item_get_cell(item));
    } else if (item.representation == SXML_ITEM_REPRESENTATION_NODE) {
        sxml_node_item_release_tree(item.payload.tree);
    }
}

/* -------------------------------------------------------- Static errors */

sxml_status sxml_error_record_report(sxml_error_record *error_record,
                                     const sxml_static_error *error) {
    if (error_record != NULL) {
        *error_record = (sxml_error_record){
            .code =
                {
                    .namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(
                        SXML_ERROR_NAMESPACE_URI),
                    .local_name = error->code_local_name,
                    .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err"),
                },
            .value = NULL,
            .diagnostic =
                {
                    .description = error->description,
                    .location = {.source_name = {.bytes = NULL,
                                                 .byte_count = 0},
                                 .line_number = 0,
                                 .column_number = 0,
                                 .byte_offset = 0},
                },
        };
    }
    return error->error_status;
}

/* -------------------------------------------------------- Raised errors */

/* The views of a raise request that are copied, in their order in the
   copied block. */
enum { SXML_RAISED_ERROR_TEXT_COUNT = 5 };

void sxml_item_heap_clear_raised_error(sxml_item_heap *item_heap) {
    if (!item_heap->has_raised_error) {
        return;
    }
    sxml_item_heap_release_bytes(item_heap, item_heap->raised_error_text);
    if (item_heap->raised_error_has_value) {
        sxml_sequence_release(item_heap->raised_error_value);
    }
    item_heap->has_raised_error = false;
    item_heap->raised_error_text =
        (sxml_heap_bytes){.memory = NULL, .byte_count = 0};
    item_heap->raised_error_has_value = false;
    item_heap->raised_error_value = sxml_sequence_empty();
}

sxml_status sxml_item_heap_raise_error(sxml_item_heap *item_heap,
                                       const sxml_error_raise_request *request,
                                       sxml_error_record *error_record) {
    if (!sxml_status_has_error_record(request->error_status)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (error_record == NULL) {
        return request->error_status;
    }
    const sxml_byte_view texts[SXML_RAISED_ERROR_TEXT_COUNT] = {
        request->code.namespace_uri, request->code.local_name,
        request->code.namespace_prefix, request->description,
        request->location.source_name,
    };
    size_t text_byte_count = 0;
    for (size_t text_index = 0; text_index < SXML_RAISED_ERROR_TEXT_COUNT;
         text_index += 1) {
        sxml_status status = sxml_size_add(
            text_byte_count, texts[text_index].byte_count, &text_byte_count);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    /* Copy before releasing the previous error: the request may point into
       it (a catch clause raising the error it caught again). */
    void *text_memory = NULL;
    if (text_byte_count != 0) {
        sxml_status status = sxml_item_heap_allocate_bytes(
            item_heap,
            (sxml_allocation_request){.byte_count = text_byte_count,
                                      .alignment = 1},
            &text_memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_byte_view copies[SXML_RAISED_ERROR_TEXT_COUNT];
    size_t byte_offset = 0;
    for (size_t text_index = 0; text_index < SXML_RAISED_ERROR_TEXT_COUNT;
         text_index += 1) {
        sxml_byte_view text = texts[text_index];
        uint8_t *copy = NULL;
        if (text.byte_count != 0) {
            copy = (uint8_t *)text_memory + byte_offset;
            memcpy(copy, text.bytes, text.byte_count);
            byte_offset += text.byte_count;
        }
        copies[text_index] =
            (sxml_byte_view){.bytes = copy, .byte_count = text.byte_count};
    }
    if (request->has_value) {
        sxml_sequence_retain(request->value);
    }
    sxml_item_heap_clear_raised_error(item_heap);
    item_heap->has_raised_error = true;
    item_heap->raised_error_text =
        (sxml_heap_bytes){.memory = text_memory, .byte_count = text_byte_count};
    item_heap->raised_error_has_value = request->has_value;
    item_heap->raised_error_value =
        request->has_value ? request->value : sxml_sequence_empty();
    *error_record = (sxml_error_record){
        .code = {.namespace_uri = copies[0],
                 .local_name = copies[1],
                 .namespace_prefix = copies[2]},
        .value =
            request->has_value ? &item_heap->raised_error_value : NULL,
        .diagnostic =
            {
                .description = copies[3],
                .location =
                    {
                        .source_name = copies[4],
                        .line_number = request->location.line_number,
                        .column_number = request->location.column_number,
                        .byte_offset = request->location.byte_offset,
                    },
            },
    };
    return request->error_status;
}

/* ------------------------------------------------------------ Sequences */

sxml_sequence sxml_sequence_empty(void) {
    return (sxml_sequence){
        .content = {.representation = SXML_ITEM_REPRESENTATION_NONE,
                    .flags = 0,
                    .namespace_number = 0,
                    .type_or_node_index = 0,
                    .payload = {.bits = 0}},
    };
}

sxml_sequence sxml_sequence_from_item(sxml_item item) {
    /* XDM 4.0, section 3.2: an item is identical to the singleton
       sequence containing it. */
    return (sxml_sequence){.content = item};
}

/* The cell class of a sequence cell. */
static const sxml_sequence_cell_operations *sxml_sequence_get_operations(
    sxml_sequence sequence) {
    return sxml_item_get_cell(sequence.content)
        ->cell_class->sequence_operations;
}

sxml_sequence_representation sxml_sequence_get_representation(
    sxml_sequence sequence) {
    if (sequence.content.representation == SXML_ITEM_REPRESENTATION_NONE) {
        return SXML_SEQUENCE_REPRESENTATION_EMPTY;
    }
    if (sequence.content.representation <
        SXML_ITEM_REPRESENTATION_FIRST_SEQUENCE_CELL) {
        return SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM;
    }
    return sxml_sequence_get_operations(sequence)->sequence_representation;
}

void sxml_sequence_retain(sxml_sequence sequence) {
    sxml_item_retain(sequence.content);
}

void sxml_sequence_release(sxml_sequence sequence) {
    sxml_item_release(sequence.content);
}

/* --------------------------------------------------------------- Vectors */

/* A vector sequence: its items, each holding one reference. */
typedef struct sxml_vector_cell {
    sxml_cell cell;
    size_t item_count;
    sxml_item items[];
} sxml_vector_cell;

static sxml_vector_cell *sxml_vector_cell_from_cell(sxml_cell *cell) {
    return (sxml_vector_cell *)cell;
}

static void sxml_vector_cell_release_contents(sxml_cell *cell) {
    sxml_vector_cell *vector = sxml_vector_cell_from_cell(cell);
    for (size_t item_index = 0; item_index < vector->item_count;
         item_index += 1) {
        sxml_item_release(vector->items[item_index]);
    }
}

static sxml_status sxml_vector_cell_count(sxml_cell *cell,
                                          sxml_error_record *error_record,
                                          uint64_t *item_count) {
    (void)error_record;
    *item_count = sxml_vector_cell_from_cell(cell)->item_count;
    return SXML_STATUS_OK;
}

static sxml_status sxml_vector_cell_get_item(sxml_cell *cell,
                                             sxml_item_request request,
                                             sxml_item *item) {
    sxml_vector_cell *vector = sxml_vector_cell_from_cell(cell);
    if (request.item_index >= vector->item_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    *item = vector->items[request.item_index];
    return SXML_STATUS_OK;
}

static sxml_status sxml_vector_cell_next(sxml_sequence_iterator *iterator,
                                         sxml_error_record *error_record,
                                         sxml_iteration_step *step) {
    (void)error_record;
    sxml_vector_cell *vector =
        sxml_vector_cell_from_cell(sxml_item_get_cell(iterator->sequence.content));
    if (iterator->next_item_index >= vector->item_count) {
        *step = (sxml_iteration_step){.has_item = false,
                                      .item = {.representation = 0}};
        return SXML_STATUS_OK;
    }
    *step = (sxml_iteration_step){
        .has_item = true, .item = vector->items[iterator->next_item_index]};
    iterator->next_item_index += 1;
    return SXML_STATUS_OK;
}

static const sxml_sequence_cell_operations sxml_vector_sequence_operations = {
    .sequence_representation = SXML_SEQUENCE_REPRESENTATION_VECTOR,
    .count = sxml_vector_cell_count,
    .get_item = sxml_vector_cell_get_item,
    .next = sxml_vector_cell_next,
};

static const sxml_cell_class sxml_vector_cell_class = {
    .class_name = SXML_BYTE_VIEW_INITIALIZER("vector"),
    .release_contents = sxml_vector_cell_release_contents,
    .sequence_operations = &sxml_vector_sequence_operations,
};

/*
 * Makes a vector of two or more items, copying the items without retaining
 * them: the caller hands over one reference to each on success.
 */
static sxml_status sxml_sequence_make_vector_taking(sxml_item_heap *item_heap,
                                                    sxml_item_view items,
                                                    sxml_sequence *sequence) {
    size_t items_byte_count = 0;
    sxml_status status =
        sxml_size_multiply(items.item_count, sizeof(sxml_item),
                           &items_byte_count);
    size_t cell_byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_add(sizeof(sxml_vector_cell), items_byte_count,
                               &cell_byte_count);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_cell *cell = NULL;
    status = sxml_item_heap_allocate_cell(
        item_heap,
        &(sxml_cell_request){.cell_class = &sxml_vector_cell_class,
                             .byte_count = cell_byte_count,
                             .alignment = alignof(sxml_vector_cell)},
        &cell);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_vector_cell *vector = sxml_vector_cell_from_cell(cell);
    vector->item_count = items.item_count;
    memcpy(vector->items, items.items, items_byte_count);
    *sequence =
        sxml_sequence_from_cell(SXML_ITEM_REPRESENTATION_VECTOR_CELL, cell);
    return SXML_STATUS_OK;
}

sxml_status sxml_sequence_make_vector(sxml_item_heap *item_heap,
                                      sxml_item_view items,
                                      sxml_sequence *sequence) {
    if (items.item_count == 0) {
        *sequence = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    if (items.item_count == 1) {
        sxml_item_retain(items.items[0]);
        *sequence = sxml_sequence_from_item(items.items[0]);
        return SXML_STATUS_OK;
    }
    sxml_sequence vector_sequence = sxml_sequence_empty();
    sxml_status status =
        sxml_sequence_make_vector_taking(item_heap, items, &vector_sequence);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    for (size_t item_index = 0; item_index < items.item_count;
         item_index += 1) {
        sxml_item_retain(items.items[item_index]);
    }
    *sequence = vector_sequence;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------- Reading any sequence */

sxml_status sxml_sequence_count(sxml_sequence sequence,
                                sxml_error_record *error_record,
                                uint64_t *item_count) {
    if (sequence.content.representation == SXML_ITEM_REPRESENTATION_NONE) {
        *item_count = 0;
        return SXML_STATUS_OK;
    }
    if (sequence.content.representation <
        SXML_ITEM_REPRESENTATION_FIRST_SEQUENCE_CELL) {
        *item_count = 1;
        return SXML_STATUS_OK;
    }
    return sxml_sequence_get_operations(sequence)->count(
        sxml_item_get_cell(sequence.content), error_record, item_count);
}

sxml_status sxml_sequence_get_item(sxml_sequence sequence,
                                   sxml_item_request request,
                                   sxml_item *item) {
    if (sequence.content.representation == SXML_ITEM_REPRESENTATION_NONE) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    if (sequence.content.representation <
        SXML_ITEM_REPRESENTATION_FIRST_SEQUENCE_CELL) {
        if (request.item_index != 0) {
            return SXML_STATUS_INDEX_OUT_OF_RANGE;
        }
        *item = sequence.content;
        return SXML_STATUS_OK;
    }
    return sxml_sequence_get_operations(sequence)->get_item(
        sxml_item_get_cell(sequence.content), request, item);
}

sxml_sequence_iterator sxml_sequence_iterator_start(sxml_sequence sequence) {
    return (sxml_sequence_iterator){
        .sequence = sequence,
        .next_item_index = 0,
        .part_index = 0,
        .next_index_in_part = 0,
    };
}

sxml_status sxml_sequence_iterator_next(sxml_sequence_iterator *iterator,
                                        sxml_error_record *error_record,
                                        sxml_iteration_step *step) {
    sxml_item content = iterator->sequence.content;
    if (content.representation >=
        SXML_ITEM_REPRESENTATION_FIRST_SEQUENCE_CELL) {
        return sxml_sequence_get_operations(iterator->sequence)
            ->next(iterator, error_record, step);
    }
    bool is_at_single_item =
        content.representation != SXML_ITEM_REPRESENTATION_NONE &&
        iterator->next_item_index == 0;
    if (!is_at_single_item) {
        *step = (sxml_iteration_step){.has_item = false,
                                      .item = {.representation = 0}};
        return SXML_STATUS_OK;
    }
    *step = (sxml_iteration_step){.has_item = true, .item = content};
    iterator->next_item_index = 1;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------ Sequence builder */

struct sxml_sequence_builder {
    sxml_item_heap *item_heap;
    /* The allocator sxml_sequence_builder_create used; the null allocator
       otherwise. */
    sxml_allocator owning_allocator;
    /* Collected items, each holding one reference, in heap memory. */
    sxml_item *items;
    size_t item_count;
    size_t item_capacity;
};

/* The first capacity of a builder's item array: most results are short. */
enum { SXML_SEQUENCE_BUILDER_FIRST_CAPACITY = 8 };

sxml_status sxml_sequence_builder_memory_requirements(
    const sxml_sequence_builder_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (configuration->item_heap == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){
        .byte_count = sizeof(struct sxml_sequence_builder),
        .alignment = alignof(struct sxml_sequence_builder),
    };
    return SXML_STATUS_OK;
}

sxml_status sxml_sequence_builder_initialize(
    sxml_memory_view memory,
    const sxml_sequence_builder_configuration *configuration,
    sxml_sequence_builder **builder) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_sequence_builder_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_memory_view_check_requirements(memory, requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_sequence_builder *new_builder = placement;
    *new_builder = (sxml_sequence_builder){
        .item_heap = configuration->item_heap,
        .owning_allocator = {.state = NULL, .operations = NULL},
        .items = NULL,
        .item_count = 0,
        .item_capacity = 0,
    };
    *builder = new_builder;
    return SXML_STATUS_OK;
}

/* Releases the items from first_item_index on and forgets them. */
static void sxml_sequence_builder_truncate(sxml_sequence_builder *builder,
                                           size_t first_item_index) {
    for (size_t item_index = first_item_index; item_index < builder->item_count;
         item_index += 1) {
        sxml_item_release(builder->items[item_index]);
    }
    builder->item_count = first_item_index;
}

void sxml_sequence_builder_finalize(sxml_sequence_builder *builder) {
    if (builder == NULL) {
        return;
    }
    sxml_sequence_builder_truncate(builder, 0);
    sxml_item_heap_release_bytes(
        builder->item_heap,
        (sxml_heap_bytes){.memory = builder->items,
                          .byte_count =
                              builder->item_capacity * sizeof(sxml_item)});
    builder->items = NULL;
    builder->item_capacity = 0;
}

sxml_status sxml_sequence_builder_create(
    sxml_allocator allocator,
    const sxml_sequence_builder_configuration *configuration,
    sxml_sequence_builder **builder) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_sequence_builder_memory_requirements(configuration, &requirements);
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
    sxml_sequence_builder *new_builder = NULL;
    status = sxml_sequence_builder_initialize(
        (sxml_memory_view){.bytes = memory,
                           .byte_count = requirements.byte_count},
        configuration, &new_builder);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_builder->owning_allocator = allocator;
    *builder = new_builder;
    return SXML_STATUS_OK;
}

void sxml_sequence_builder_destroy(sxml_sequence_builder *builder) {
    if (builder == NULL) {
        return;
    }
    sxml_allocator owning_allocator = builder->owning_allocator;
    sxml_sequence_builder_finalize(builder);
    sxml_allocator_release(owning_allocator, builder);
}

/* Makes room for one more item, doubling the capacity (D22: allocate,
   copy, release; amortized O(1)). */
static sxml_status sxml_sequence_builder_reserve_one(
    sxml_sequence_builder *builder) {
    if (builder->item_count < builder->item_capacity) {
        return SXML_STATUS_OK;
    }
    size_t new_capacity = SXML_SEQUENCE_BUILDER_FIRST_CAPACITY;
    sxml_status status = SXML_STATUS_OK;
    if (builder->item_capacity != 0) {
        status = sxml_size_multiply(builder->item_capacity, 2, &new_capacity);
    }
    size_t new_byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(new_capacity, sizeof(sxml_item),
                                    &new_byte_count);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *new_items = NULL;
    status = sxml_item_heap_allocate_bytes(
        builder->item_heap,
        (sxml_allocation_request){.byte_count = new_byte_count,
                                  .alignment = alignof(sxml_item)},
        &new_items);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (builder->item_count != 0) {
        memcpy(new_items, builder->items,
               builder->item_count * sizeof(sxml_item));
    }
    sxml_item_heap_release_bytes(
        builder->item_heap,
        (sxml_heap_bytes){.memory = builder->items,
                          .byte_count =
                              builder->item_capacity * sizeof(sxml_item)});
    builder->items = new_items;
    builder->item_capacity = new_capacity;
    return SXML_STATUS_OK;
}

sxml_status sxml_sequence_builder_append_item(sxml_sequence_builder *builder,
                                              sxml_item item) {
    sxml_status status = sxml_sequence_builder_reserve_one(builder);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item_retain(item);
    builder->items[builder->item_count] = item;
    builder->item_count += 1;
    return SXML_STATUS_OK;
}

sxml_status sxml_sequence_builder_append_sequence(
    sxml_sequence_builder *builder, sxml_sequence sequence,
    sxml_error_record *error_record) {
    size_t first_new_item_index = builder->item_count;
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(sequence);
    for (;;) {
        sxml_iteration_step step = {.has_item = false,
                                    .item = {.representation = 0}};
        sxml_status status =
            sxml_sequence_iterator_next(&iterator, error_record, &step);
        if (status == SXML_STATUS_OK && !step.has_item) {
            return SXML_STATUS_OK;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_item(builder, step.item);
        }
        if (status != SXML_STATUS_OK) {
            /* Leave the builder as it was before the call. */
            sxml_sequence_builder_truncate(builder, first_new_item_index);
            return status;
        }
    }
}

sxml_status sxml_sequence_builder_finish(sxml_sequence_builder *builder,
                                         sxml_sequence *sequence) {
    if (builder->item_count == 0) {
        *sequence = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    if (builder->item_count == 1) {
        /* The builder's reference passes to the sequence. */
        *sequence = sxml_sequence_from_item(builder->items[0]);
        builder->item_count = 0;
        return SXML_STATUS_OK;
    }
    sxml_sequence vector_sequence = sxml_sequence_empty();
    sxml_status status = sxml_sequence_make_vector_taking(
        builder->item_heap,
        (sxml_item_view){.items = builder->items,
                         .item_count = builder->item_count},
        &vector_sequence);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    builder->item_count = 0;
    *sequence = vector_sequence;
    return SXML_STATUS_OK;
}
