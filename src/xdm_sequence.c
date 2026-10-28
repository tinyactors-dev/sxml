/*
 * Integer ranges, concatenations, lazy sequences and sequence slices: the
 * sequence cells that src/xdm.c reads through their cell classes, and the
 * sequence functions of include/sxml/xdm.h built on them (D34, D41, D43,
 * D60, D62; sequence slices perf-5).
 *
 * Golden file: src/xdm.c (vector cells: a cell struct whose first member is
 * the header, a static const cell class, checked sizes, outputs untouched
 * on failure).
 *
 * Specification: XDM 4.0, section 3.2 (sequences; dm:sequence-concat,
 * dm:iterate-sequence); XPath 4.0, section 4.8.2 "Range Expressions";
 * XPath 4.0, section 2.4.2 and appendix "Error Conditions" (XPDY0130, an
 * implementation-dependent limit).
 */
#include "xdm_internal.h"

#include <stdalign.h>
#include <string.h>

/*
 * XPDY0130: a count that does not fit uint64_t. Only a range of all 2^64
 * integers, or a concatenation of more than 2^64 - 1 items, reaches it.
 */
static const sxml_static_error sxml_sequence_too_long_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPDY0130"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the number of items in the sequence exceeds 2^64 - 1"),
};

/* The end of an iteration: no item. */
static sxml_iteration_step sxml_iteration_step_end(void) {
    return (sxml_iteration_step){.has_item = false,
                                 .item = {.representation = 0}};
}

/* ------------------------------------------------------- Integer ranges */

/*
 * The integers first_integer to last_integer, first_integer below
 * last_integer (an empty range is the empty sequence, a range of one
 * integer a single item; neither has a cell). A descending range's cell
 * class yields them from last_integer down (D143).
 */
typedef struct sxml_integer_range_cell {
    sxml_cell cell;
    sxml_integer_range range;
} sxml_integer_range_cell;

static sxml_integer_range_cell *sxml_integer_range_cell_from_cell(
    sxml_cell *cell) {
    return (sxml_integer_range_cell *)cell;
}

/* last_integer - first_integer, exact: at most 2^64 - 1. */
static uint64_t sxml_integer_range_get_last_offset(sxml_integer_range range) {
    return (uint64_t)range.last_integer - (uint64_t)range.first_integer;
}

/*
 * first_integer + offset, for an offset that stays within the range, so the
 * result fits int64_t. Computed in uint64_t to avoid signed overflow, then
 * converted back without relying on implementation-defined conversion.
 */
static int64_t sxml_integer_range_get_member(sxml_integer_range range,
                                             uint64_t offset) {
    uint64_t sum = (uint64_t)range.first_integer + offset;
    if (sum <= (uint64_t)INT64_MAX) {
        return (int64_t)sum;
    }
    return -(int64_t)(UINT64_MAX - sum) - 1;
}

static sxml_status sxml_integer_range_cell_count(
    sxml_cell *cell, sxml_error_record *error_record, uint64_t *item_count) {
    uint64_t last_offset = sxml_integer_range_get_last_offset(
        sxml_integer_range_cell_from_cell(cell)->range);
    if (last_offset == UINT64_MAX) {
        /* INT64_MIN to INT64_MAX holds 2^64 integers. */
        return sxml_error_record_report(error_record,
                                        &sxml_sequence_too_long_error);
    }
    *item_count = last_offset + 1;
    return SXML_STATUS_OK;
}

/* A descending range's cell class has its own operations (D143). */
static bool sxml_integer_range_cell_is_descending(const sxml_cell *cell) {
    return cell->cell_class->sequence_operations->sequence_representation ==
           SXML_SEQUENCE_REPRESENTATION_DESCENDING_INTEGER_RANGE;
}

/* The integer at item_index of a range cell, which item_index must not
   pass. */
static int64_t sxml_integer_range_cell_member_at(sxml_cell *cell,
                                                 uint64_t item_index) {
    const sxml_integer_range range =
        sxml_integer_range_cell_from_cell(cell)->range;
    return sxml_integer_range_get_member(
        range, sxml_integer_range_cell_is_descending(cell)
                   ? sxml_integer_range_get_last_offset(range) - item_index
                   : item_index);
}

static sxml_status sxml_integer_range_cell_get_item(sxml_cell *cell,
                                                    sxml_item_request request,
                                                    sxml_item *item) {
    sxml_integer_range range = sxml_integer_range_cell_from_cell(cell)->range;
    if (request.item_index > sxml_integer_range_get_last_offset(range)) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    *item = sxml_atomic_item_from_integer(
        sxml_integer_range_cell_member_at(cell, request.item_index));
    return SXML_STATUS_OK;
}

/*
 * The iterator's part_index is 1 once the last integer was yielded:
 * next_item_index cannot mark the end of a range of 2^64 integers.
 */
static sxml_status sxml_integer_range_cell_next(
    sxml_sequence_iterator *iterator, sxml_error_record *error_record,
    sxml_iteration_step *step) {
    (void)error_record;
    sxml_cell *cell = sxml_item_get_cell(iterator->sequence.content);
    sxml_integer_range range = sxml_integer_range_cell_from_cell(cell)->range;
    if (iterator->part_index != 0) {
        *step = sxml_iteration_step_end();
        return SXML_STATUS_OK;
    }
    uint64_t item_index = iterator->next_item_index;
    *step = (sxml_iteration_step){
        .has_item = true,
        .item = sxml_atomic_item_from_integer(
            sxml_integer_range_cell_member_at(cell, item_index))};
    if (item_index == sxml_integer_range_get_last_offset(range)) {
        iterator->part_index = 1;
    } else {
        iterator->next_item_index = item_index + 1;
    }
    return SXML_STATUS_OK;
}

static const sxml_sequence_cell_operations
    sxml_integer_range_sequence_operations = {
        .sequence_representation = SXML_SEQUENCE_REPRESENTATION_INTEGER_RANGE,
        .count = sxml_integer_range_cell_count,
        .get_item = sxml_integer_range_cell_get_item,
        .next = sxml_integer_range_cell_next,
};

static const sxml_sequence_cell_operations
    sxml_descending_integer_range_sequence_operations = {
        .sequence_representation =
            SXML_SEQUENCE_REPRESENTATION_DESCENDING_INTEGER_RANGE,
        .count = sxml_integer_range_cell_count,
        .get_item = sxml_integer_range_cell_get_item,
        .next = sxml_integer_range_cell_next,
};

static const sxml_cell_class sxml_integer_range_cell_class = {
    .class_name = SXML_BYTE_VIEW_INITIALIZER("integer range"),
    .release_contents = NULL,
    .sequence_operations = &sxml_integer_range_sequence_operations,
};

static const sxml_cell_class sxml_descending_integer_range_cell_class = {
    .class_name = SXML_BYTE_VIEW_INITIALIZER("descending integer range"),
    .release_contents = NULL,
    .sequence_operations = &sxml_descending_integer_range_sequence_operations,
};

/* Makes a range sequence of cell_class (ascending or descending). */
typedef struct sxml_integer_range_request {
    sxml_integer_range range;
    const sxml_cell_class *cell_class;
} sxml_integer_range_request;

static sxml_status sxml_sequence_make_range_of_class(
    sxml_item_heap *item_heap, sxml_integer_range_request request,
    sxml_sequence *sequence) {
    const sxml_integer_range range = request.range;
    /* XPath 4.0, section 4.8.2: empty when the first integer is greater
       than the last; one integer when they are equal. */
    if (range.first_integer > range.last_integer) {
        *sequence = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    if (range.first_integer == range.last_integer) {
        *sequence = sxml_sequence_from_item(
            sxml_atomic_item_from_integer(range.first_integer));
        return SXML_STATUS_OK;
    }
    sxml_cell *cell = NULL;
    sxml_status status = sxml_item_heap_allocate_cell(
        item_heap,
        &(sxml_cell_request){.cell_class = request.cell_class,
                             .byte_count = sizeof(sxml_integer_range_cell),
                             .alignment = alignof(sxml_integer_range_cell)},
        &cell);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_integer_range_cell_from_cell(cell)->range = range;
    *sequence = sxml_sequence_from_cell(
        SXML_ITEM_REPRESENTATION_INTEGER_RANGE_CELL, cell);
    return SXML_STATUS_OK;
}

sxml_status sxml_sequence_make_integer_range(sxml_item_heap *item_heap,
                                             sxml_integer_range range,
                                             sxml_sequence *sequence) {
    return sxml_sequence_make_range_of_class(
        item_heap,
        (sxml_integer_range_request){
            .range = range, .cell_class = &sxml_integer_range_cell_class},
        sequence);
}

sxml_status sxml_sequence_make_descending_integer_range(
    sxml_item_heap *item_heap, sxml_integer_range range,
    sxml_sequence *sequence) {
    return sxml_sequence_make_range_of_class(
        item_heap,
        (sxml_integer_range_request){
            .range = range,
            .cell_class = &sxml_descending_integer_range_cell_class},
        sequence);
}

/* ------------------------------------------------------- Concatenations */

/* One part of a concatenation and its part end. */
typedef struct sxml_concatenation_part {
    /* Holds one reference; never empty, never a concatenation. */
    sxml_sequence sequence;
    /* The number of items in this part and the parts before it; known
       only for the first resolved_part_count parts. */
    uint64_t part_end;
} sxml_concatenation_part;

/*
 * Two or more parts (nested concatenations are flattened). The part ends
 * are a cache filled as reads count the parts in order; a lazy part is
 * counted only once it has been read to its end.
 */
typedef struct sxml_concatenation_cell {
    sxml_cell cell;
    size_t part_count;
    size_t resolved_part_count;
    sxml_concatenation_part parts[];
} sxml_concatenation_cell;

static sxml_concatenation_cell *sxml_concatenation_cell_from_cell(
    sxml_cell *cell) {
    return (sxml_concatenation_cell *)cell;
}

static void sxml_concatenation_cell_release_contents(sxml_cell *cell) {
    sxml_concatenation_cell *concatenation =
        sxml_concatenation_cell_from_cell(cell);
    for (size_t part_index = 0; part_index < concatenation->part_count;
         part_index += 1) {
        sxml_sequence_release(concatenation->parts[part_index].sequence);
    }
}

/* The number of items before part part_index, which must be resolved or
   the first unresolved part. */
static uint64_t sxml_concatenation_get_part_start(
    const sxml_concatenation_cell *concatenation, size_t part_index) {
    return part_index == 0 ? 0 : concatenation->parts[part_index - 1].part_end;
}

/*
 * Counts the first unresolved part (reading a lazy part to its end) and
 * records its part end. Returns the part's failures and XPDY0130 when the
 * total passes 2^64 - 1; the cache is then unchanged.
 */
static sxml_status sxml_concatenation_resolve_next_part(
    sxml_concatenation_cell *concatenation, sxml_error_record *error_record) {
    size_t part_index = concatenation->resolved_part_count;
    uint64_t part_item_count = 0;
    sxml_status status = sxml_sequence_count(
        concatenation->parts[part_index].sequence, error_record,
        &part_item_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint64_t part_start =
        sxml_concatenation_get_part_start(concatenation, part_index);
    if (part_item_count > UINT64_MAX - part_start) {
        return sxml_error_record_report(error_record,
                                        &sxml_sequence_too_long_error);
    }
    concatenation->parts[part_index].part_end = part_start + part_item_count;
    concatenation->resolved_part_count = part_index + 1;
    return SXML_STATUS_OK;
}

static sxml_status sxml_concatenation_cell_count(
    sxml_cell *cell, sxml_error_record *error_record, uint64_t *item_count) {
    sxml_concatenation_cell *concatenation =
        sxml_concatenation_cell_from_cell(cell);
    while (concatenation->resolved_part_count < concatenation->part_count) {
        sxml_status status =
            sxml_concatenation_resolve_next_part(concatenation, error_record);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    *item_count = concatenation->parts[concatenation->part_count - 1].part_end;
    return SXML_STATUS_OK;
}

/*
 * Reads the item at request.item_index: by binary search over the resolved
 * parts, else by walking the unresolved ones in order. Each part is asked
 * for the item before it is counted, so a lazy part is read only as far as
 * needed and every index below 2^64 is readable even when the total count
 * is not.
 */
static sxml_status sxml_concatenation_cell_get_item(sxml_cell *cell,
                                                    sxml_item_request request,
                                                    sxml_item *item) {
    sxml_concatenation_cell *concatenation =
        sxml_concatenation_cell_from_cell(cell);
    size_t resolved_part_count = concatenation->resolved_part_count;
    if (resolved_part_count != 0 &&
        request.item_index <
            concatenation->parts[resolved_part_count - 1].part_end) {
        size_t low_part_index = 0;
        size_t high_part_index = resolved_part_count - 1;
        while (low_part_index < high_part_index) {
            size_t middle_part_index =
                low_part_index + (high_part_index - low_part_index) / 2;
            if (concatenation->parts[middle_part_index].part_end >
                request.item_index) {
                high_part_index = middle_part_index;
            } else {
                low_part_index = middle_part_index + 1;
            }
        }
        return sxml_sequence_get_item(
            concatenation->parts[low_part_index].sequence,
            (sxml_item_request){
                .item_index = request.item_index -
                              sxml_concatenation_get_part_start(
                                  concatenation, low_part_index),
                .error_record = request.error_record},
            item);
    }
    while (concatenation->resolved_part_count < concatenation->part_count) {
        size_t part_index = concatenation->resolved_part_count;
        sxml_sequence part = concatenation->parts[part_index].sequence;
        sxml_item_request part_request = {
            .item_index =
                request.item_index -
                sxml_concatenation_get_part_start(concatenation, part_index),
            .error_record = request.error_record};
        sxml_status status = sxml_sequence_get_item(part, part_request, item);
        if (status != SXML_STATUS_INDEX_OUT_OF_RANGE) {
            return status;
        }
        /* The part ends before the index, so counting it neither reads a
           lazy part further nor passes 2^64 - 1. */
        status = sxml_concatenation_resolve_next_part(concatenation,
                                                      request.error_record);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_INDEX_OUT_OF_RANGE;
}

/*
 * The iterator's part_index and next_index_in_part say where it stands;
 * every part answers get_item in O(1) for the index after the one it last
 * answered, so iteration is O(1) per item.
 */
static sxml_status sxml_concatenation_cell_next(
    sxml_sequence_iterator *iterator, sxml_error_record *error_record,
    sxml_iteration_step *step) {
    sxml_concatenation_cell *concatenation = sxml_concatenation_cell_from_cell(
        sxml_item_get_cell(iterator->sequence.content));
    size_t part_index = iterator->part_index;
    uint64_t index_in_part = iterator->next_index_in_part;
    while (part_index < concatenation->part_count) {
        sxml_item item = {.representation = 0};
        sxml_status status = sxml_sequence_get_item(
            concatenation->parts[part_index].sequence,
            (sxml_item_request){.item_index = index_in_part,
                                .error_record = error_record},
            &item);
        if (status == SXML_STATUS_OK) {
            iterator->part_index = part_index;
            iterator->next_index_in_part = index_in_part + 1;
            iterator->next_item_index += 1;
            *step = (sxml_iteration_step){.has_item = true, .item = item};
            return SXML_STATUS_OK;
        }
        if (status != SXML_STATUS_INDEX_OUT_OF_RANGE) {
            return status;
        }
        part_index += 1;
        index_in_part = 0;
    }
    iterator->part_index = part_index;
    iterator->next_index_in_part = 0;
    *step = sxml_iteration_step_end();
    return SXML_STATUS_OK;
}

static const sxml_sequence_cell_operations
    sxml_concatenation_sequence_operations = {
        .sequence_representation = SXML_SEQUENCE_REPRESENTATION_CONCATENATION,
        .count = sxml_concatenation_cell_count,
        .get_item = sxml_concatenation_cell_get_item,
        .next = sxml_concatenation_cell_next,
};

static const sxml_cell_class sxml_concatenation_cell_class = {
    .class_name = SXML_BYTE_VIEW_INITIALIZER("concatenation"),
    .release_contents = sxml_concatenation_cell_release_contents,
    .sequence_operations = &sxml_concatenation_sequence_operations,
};

/* The number of parts sequence contributes once flattened. */
static size_t sxml_sequence_get_flattened_part_count(sxml_sequence sequence) {
    if (sequence.content.representation == SXML_ITEM_REPRESENTATION_NONE) {
        return 0;
    }
    if (sequence.content.representation ==
        SXML_ITEM_REPRESENTATION_CONCATENATION_CELL) {
        return sxml_concatenation_cell_from_cell(
                   sxml_item_get_cell(sequence.content))
            ->part_count;
    }
    return 1;
}

/* The byte count of a concatenation cell with part_count parts. */
static sxml_status sxml_concatenation_cell_get_byte_count(
    size_t part_count, size_t *byte_count) {
    size_t part_byte_count = 0;
    sxml_status status = sxml_size_multiply(
        part_count, sizeof(sxml_concatenation_part), &part_byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_size_add(sizeof(sxml_concatenation_cell), part_byte_count,
                         byte_count);
}

sxml_status sxml_sequence_concatenate(sxml_item_heap *item_heap,
                                      sxml_sequence_view sequences,
                                      sxml_sequence *sequence) {
    size_t part_count = 0;
    size_t last_part_sequence_index = 0;
    for (size_t sequence_index = 0; sequence_index < sequences.sequence_count;
         sequence_index += 1) {
        size_t flattened_part_count = sxml_sequence_get_flattened_part_count(
            sequences.sequences[sequence_index]);
        if (flattened_part_count != 0) {
            last_part_sequence_index = sequence_index;
        }
        sxml_status status =
            sxml_size_add(part_count, flattened_part_count, &part_count);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    /* XDM 4.0, section 3.2.2: concatenating with the empty sequence gives
       the other sequence; nothing to share means no cell. */
    if (part_count == 0) {
        *sequence = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    if (part_count == 1) {
        sxml_sequence only_part = sequences.sequences[last_part_sequence_index];
        sxml_sequence_retain(only_part);
        *sequence = only_part;
        return SXML_STATUS_OK;
    }
    size_t cell_byte_count = 0;
    sxml_status status =
        sxml_concatenation_cell_get_byte_count(part_count, &cell_byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_cell *cell = NULL;
    status = sxml_item_heap_allocate_cell(
        item_heap,
        &(sxml_cell_request){.cell_class = &sxml_concatenation_cell_class,
                             .byte_count = cell_byte_count,
                             .alignment = alignof(sxml_concatenation_cell)},
        &cell);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_concatenation_cell *concatenation =
        sxml_concatenation_cell_from_cell(cell);
    concatenation->part_count = part_count;
    concatenation->resolved_part_count = 0;
    size_t part_index = 0;
    for (size_t sequence_index = 0; sequence_index < sequences.sequence_count;
         sequence_index += 1) {
        sxml_sequence input = sequences.sequences[sequence_index];
        if (input.content.representation == SXML_ITEM_REPRESENTATION_NONE) {
            continue;
        }
        if (input.content.representation !=
            SXML_ITEM_REPRESENTATION_CONCATENATION_CELL) {
            sxml_sequence_retain(input);
            concatenation->parts[part_index] =
                (sxml_concatenation_part){.sequence = input, .part_end = 0};
            part_index += 1;
            continue;
        }
        /* Flattened: the inner parts, not the inner concatenation. */
        const sxml_concatenation_cell *inner =
            sxml_concatenation_cell_from_cell(
                sxml_item_get_cell(input.content));
        for (size_t inner_part_index = 0; inner_part_index < inner->part_count;
             inner_part_index += 1) {
            sxml_sequence inner_part = inner->parts[inner_part_index].sequence;
            sxml_sequence_retain(inner_part);
            concatenation->parts[part_index] = (sxml_concatenation_part){
                .sequence = inner_part, .part_end = 0};
            part_index += 1;
        }
    }
    *sequence = sxml_sequence_from_cell(
        SXML_ITEM_REPRESENTATION_CONCATENATION_CELL, cell);
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------ Sequence slices */

/*
 * A sequence slice: item_count consecutive items of its slice base, a
 * vector, from first_index on. The base holds the items and their
 * references; the slice holds one reference to the base, so the items
 * live while either does. A slice of a slice has the outer slice's base,
 * so a slice never reads through another slice and reading an item is
 * O(1). Cells are immutable, so the shared items never change.
 */
typedef struct sxml_slice_cell {
    sxml_cell cell;
    /* A vector cell; holds one reference. */
    sxml_sequence slice_base;
    uint64_t first_index;
    /* At least 2: shorter spans have no cell. */
    uint64_t item_count;
} sxml_slice_cell;

/*
 * When a slice copies its items instead of sharing them: a span of at
 * most SXML_SLICE_MAXIMUM_COPIED_ITEM_COUNT items, or one shorter than
 * its base's count divided by SXML_SLICE_MINIMUM_BASE_SHARE. A short
 * vector costs about what a slice cell does, and a slice that would keep
 * a base more than eight times its own length alive copies instead, so
 * memory held through slices stays within eight times what they show.
 * Recursion over the rest of a sequence of n items copies at most n/8 +
 * n/64 + ... items in all: linear.
 */
enum {
    SXML_SLICE_MAXIMUM_COPIED_ITEM_COUNT = 16,
    SXML_SLICE_MINIMUM_BASE_SHARE = 8,
};

static sxml_slice_cell *sxml_slice_cell_from_cell(sxml_cell *cell) {
    return (sxml_slice_cell *)cell;
}

static void sxml_slice_cell_release_contents(sxml_cell *cell) {
    sxml_sequence_release(sxml_slice_cell_from_cell(cell)->slice_base);
}

static sxml_status sxml_slice_cell_count(sxml_cell *cell,
                                         sxml_error_record *error_record,
                                         uint64_t *item_count) {
    (void)error_record;
    *item_count = sxml_slice_cell_from_cell(cell)->item_count;
    return SXML_STATUS_OK;
}

static sxml_status sxml_slice_cell_get_item(sxml_cell *cell,
                                            sxml_item_request request,
                                            sxml_item *item) {
    const sxml_slice_cell *slice = sxml_slice_cell_from_cell(cell);
    if (request.item_index >= slice->item_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    /* The base is a vector: O(1), borrowed from the base. */
    return sxml_sequence_get_item(
        slice->slice_base,
        (sxml_item_request){.item_index = slice->first_index + request.item_index,
                            .error_record = request.error_record},
        item);
}

static sxml_status sxml_slice_cell_next(sxml_sequence_iterator *iterator,
                                        sxml_error_record *error_record,
                                        sxml_iteration_step *step) {
    sxml_item item = {.representation = 0};
    sxml_status status = sxml_slice_cell_get_item(
        sxml_item_get_cell(iterator->sequence.content),
        (sxml_item_request){.item_index = iterator->next_item_index,
                            .error_record = error_record},
        &item);
    if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
        *step = sxml_iteration_step_end();
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    iterator->next_item_index += 1;
    *step = (sxml_iteration_step){.has_item = true, .item = item};
    return SXML_STATUS_OK;
}

/* A slice is read as its items in an array in a cell: a vector to every
   reader (sxml_sequence_get_representation). */
static const sxml_sequence_cell_operations sxml_slice_sequence_operations = {
    .sequence_representation = SXML_SEQUENCE_REPRESENTATION_VECTOR,
    .count = sxml_slice_cell_count,
    .get_item = sxml_slice_cell_get_item,
    .next = sxml_slice_cell_next,
};

static const sxml_cell_class sxml_slice_cell_class = {
    .class_name = SXML_BYTE_VIEW_INITIALIZER("sequence slice"),
    .release_contents = sxml_slice_cell_release_contents,
    .sequence_operations = &sxml_slice_sequence_operations,
};

/* The span of a range, ascending or descending (D143), as a range. */
static sxml_status sxml_sequence_make_range_slice(sxml_item_heap *item_heap,
                                                  const sxml_item_span *span,
                                                  sxml_sequence *sequence) {
    sxml_cell *cell = sxml_item_get_cell(span->input.content);
    const sxml_integer_range range =
        sxml_integer_range_cell_from_cell(cell)->range;
    const uint64_t last_offset = sxml_integer_range_get_last_offset(range);
    if (span->first_index > last_offset ||
        (span->has_end && span->end_index <= span->first_index)) {
        *sequence = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    /* The last index taken, at most last_offset (also for a range of 2^64
       integers, whose count does not fit uint64_t). */
    const uint64_t last_index =
        span->has_end && span->end_index - 1 < last_offset ? span->end_index - 1
                                                           : last_offset;
    const int64_t first_member =
        sxml_integer_range_cell_member_at(cell, span->first_index);
    const int64_t last_member = sxml_integer_range_cell_member_at(cell, last_index);
    const bool is_descending = sxml_integer_range_cell_is_descending(cell);
    return sxml_sequence_make_range_of_class(
        item_heap,
        (sxml_integer_range_request){
            .range = {.first_integer = is_descending ? last_member : first_member,
                      .last_integer = is_descending ? first_member : last_member},
            .cell_class = is_descending
                              ? &sxml_descending_integer_range_cell_class
                              : &sxml_integer_range_cell_class},
        sequence);
}

/* Reads span's items one by one into a new vector: a concatenation or a
   lazy input (read only as far as the span reaches), or a short span. */
static sxml_status sxml_sequence_copy_span(sxml_item_heap *item_heap,
                                           const sxml_item_span *span,
                                           sxml_sequence *sequence) {
    sxml_sequence_builder *builder = NULL;
    sxml_status status = sxml_sequence_builder_create(
        sxml_item_heap_as_allocator(item_heap),
        &(sxml_sequence_builder_configuration){.item_heap = item_heap},
        &builder);
    for (uint64_t item_index = span->first_index;
         status == SXML_STATUS_OK &&
         (!span->has_end || item_index < span->end_index);
         item_index += 1) {
        sxml_item item = {.representation = 0};
        status = sxml_sequence_get_item(
            span->input,
            (sxml_item_request){.item_index = item_index,
                                .error_record = span->error_record},
            &item);
        if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
            status = SXML_STATUS_OK;
            break;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_builder_append_item(builder, item);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_builder_finish(builder, sequence);
    }
    sxml_sequence_builder_destroy(builder);
    return status;
}

sxml_status sxml_sequence_make_slice(sxml_item_heap *item_heap,
                                     const sxml_item_span *span,
                                     sxml_sequence *sequence) {
    const sxml_sequence input = span->input;
    if (input.content.representation ==
        SXML_ITEM_REPRESENTATION_INTEGER_RANGE_CELL) {
        return sxml_sequence_make_range_slice(item_heap, span, sequence);
    }
    if (sxml_sequence_get_representation(input) !=
        SXML_SEQUENCE_REPRESENTATION_VECTOR) {
        /* Empty, one item, a concatenation or a lazy sequence. */
        return sxml_sequence_copy_span(item_heap, span, sequence);
    }
    /* A vector or a slice: the count is known without reading. */
    sxml_cell *cell = sxml_item_get_cell(input.content);
    const bool is_slice = cell->cell_class == &sxml_slice_cell_class;
    uint64_t input_count = 0;
    sxml_status status = sxml_sequence_count(input, span->error_record,
                                             &input_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint64_t end_index =
        span->has_end && span->end_index < input_count ? span->end_index
                                                       : input_count;
    if (span->first_index >= end_index) {
        *sequence = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    if (span->first_index == 0 && end_index == input_count) {
        sxml_sequence_retain(input);
        *sequence = input;
        return SXML_STATUS_OK;
    }
    const uint64_t item_count = end_index - span->first_index;
    sxml_sequence slice_base = input;
    uint64_t base_first_index = span->first_index;
    uint64_t base_count = input_count;
    if (is_slice) {
        const sxml_slice_cell *outer = sxml_slice_cell_from_cell(cell);
        slice_base = outer->slice_base;
        base_first_index += outer->first_index;
        status = sxml_sequence_count(slice_base, span->error_record, &base_count);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (item_count <= SXML_SLICE_MAXIMUM_COPIED_ITEM_COUNT ||
        item_count < base_count / SXML_SLICE_MINIMUM_BASE_SHARE) {
        const sxml_item_span copied = {.input = slice_base,
                                       .first_index = base_first_index,
                                       .has_end = true,
                                       .end_index = base_first_index + item_count,
                                       .error_record = span->error_record};
        return sxml_sequence_copy_span(item_heap, &copied, sequence);
    }
    sxml_cell *slice_cell = NULL;
    status = sxml_item_heap_allocate_cell(
        item_heap,
        &(sxml_cell_request){.cell_class = &sxml_slice_cell_class,
                             .byte_count = sizeof(sxml_slice_cell),
                             .alignment = alignof(sxml_slice_cell)},
        &slice_cell);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence_retain(slice_base);
    *sxml_slice_cell_from_cell(slice_cell) =
        (sxml_slice_cell){.cell = *slice_cell,
                          .slice_base = slice_base,
                          .first_index = base_first_index,
                          .item_count = item_count};
    *sequence = sxml_sequence_from_cell(SXML_ITEM_REPRESENTATION_VECTOR_CELL,
                                        slice_cell);
    return SXML_STATUS_OK;
}

sxml_sequence sxml_sequence_get_slice_base(sxml_sequence sequence) {
    if (sequence.content.representation ==
            SXML_ITEM_REPRESENTATION_VECTOR_CELL &&
        sxml_item_get_cell(sequence.content)->cell_class ==
            &sxml_slice_cell_class) {
        return sxml_slice_cell_from_cell(sxml_item_get_cell(sequence.content))
            ->slice_base;
    }
    return sequence;
}

/* ------------------------------------------------------ Lazy sequences */

/* The views of a kept error that are copied, in their order in its text. */
enum { SXML_KEPT_ERROR_TEXT_COUNT = 5 };

/*
 * A generator's error, copied so that the lazy sequence can raise it again
 * on every later read (D41): the raised error of the heap is replaced by
 * the next error raised anywhere.
 */
typedef struct sxml_kept_error {
    /* The copied views, in one block of heap memory. */
    sxml_heap_bytes text;
    sxml_error_raise_request request;
} sxml_kept_error;

/*
 * The produced items (each holding one reference) in heap memory, and the
 * generator. Once the generator has failed, failure_status is its status
 * (or the status of copying its error), and the generator is not called
 * again; kept_error holds the error when failure_status has an error
 * record.
 */
typedef struct sxml_lazy_cell {
    sxml_cell cell;
    sxml_sequence_generator generator;
    sxml_item *produced_items;
    size_t produced_item_count;
    size_t produced_item_capacity;
    bool is_finished;
    /* True while the generator's next runs: a generator that reads its
       own lazy sequence is refused rather than reentered. */
    bool is_producing;
    sxml_status failure_status;
    sxml_kept_error kept_error;
} sxml_lazy_cell;

/* The first capacity of a lazy sequence's produced items. */
enum { SXML_LAZY_FIRST_CAPACITY = 8 };

static sxml_lazy_cell *sxml_lazy_cell_from_cell(sxml_cell *cell) {
    return (sxml_lazy_cell *)cell;
}

static sxml_item_heap *sxml_lazy_cell_get_item_heap(
    const sxml_lazy_cell *lazy) {
    return sxml_cell_get_item_heap(&lazy->cell);
}

static void sxml_lazy_cell_release_contents(sxml_cell *cell) {
    sxml_lazy_cell *lazy = sxml_lazy_cell_from_cell(cell);
    sxml_item_heap *item_heap = sxml_lazy_cell_get_item_heap(lazy);
    for (size_t item_index = 0; item_index < lazy->produced_item_count;
         item_index += 1) {
        sxml_item_release(lazy->produced_items[item_index]);
    }
    sxml_item_heap_release_bytes(
        item_heap,
        (sxml_heap_bytes){.memory = lazy->produced_items,
                          .byte_count = lazy->produced_item_capacity *
                                        sizeof(sxml_item)});
    sxml_item_heap_release_bytes(item_heap, lazy->kept_error.text);
    if (lazy->kept_error.request.has_value) {
        sxml_sequence_release(lazy->kept_error.request.value);
    }
    if (lazy->generator.operations->release != NULL) {
        lazy->generator.operations->release(lazy->generator.state);
    }
}

/*
 * Copies record, which the generator filled with status, into
 * lazy->kept_error. Returns the heap's failures; kept_error is then
 * unchanged.
 */
static sxml_status sxml_lazy_keep_error(sxml_lazy_cell *lazy,
                                        sxml_status status,
                                        const sxml_error_record *record) {
    const sxml_byte_view texts[SXML_KEPT_ERROR_TEXT_COUNT] = {
        record->code.namespace_uri, record->code.local_name,
        record->code.namespace_prefix, record->diagnostic.description,
        record->diagnostic.location.source_name,
    };
    size_t text_byte_count = 0;
    for (size_t text_index = 0; text_index < SXML_KEPT_ERROR_TEXT_COUNT;
         text_index += 1) {
        sxml_status size_status = sxml_size_add(
            text_byte_count, texts[text_index].byte_count, &text_byte_count);
        if (size_status != SXML_STATUS_OK) {
            return size_status;
        }
    }
    void *text_memory = NULL;
    if (text_byte_count != 0) {
        sxml_status allocation_status = sxml_item_heap_allocate_bytes(
            sxml_lazy_cell_get_item_heap(lazy),
            (sxml_allocation_request){.byte_count = text_byte_count,
                                      .alignment = 1},
            &text_memory);
        if (allocation_status != SXML_STATUS_OK) {
            return allocation_status;
        }
    }
    sxml_byte_view copies[SXML_KEPT_ERROR_TEXT_COUNT];
    size_t byte_offset = 0;
    for (size_t text_index = 0; text_index < SXML_KEPT_ERROR_TEXT_COUNT;
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
    bool has_value = record->value != NULL;
    sxml_sequence value = has_value ? *record->value : sxml_sequence_empty();
    sxml_sequence_retain(value);
    sxml_source_location location = record->diagnostic.location;
    location.source_name = copies[4];
    lazy->kept_error = (sxml_kept_error){
        .text = {.memory = text_memory, .byte_count = text_byte_count},
        .request = {.error_status = status,
                    .code = {.namespace_uri = copies[0],
                             .local_name = copies[1],
                             .namespace_prefix = copies[2]},
                    .description = copies[3],
                    .has_value = has_value,
                    .value = value,
                    .location = location},
    };
    return SXML_STATUS_OK;
}

/* Reports the generator's failure again, filling error_record (may be
   NULL) through the heap's raised error (D41). */
static sxml_status sxml_lazy_report_failure(sxml_lazy_cell *lazy,
                                            sxml_error_record *error_record) {
    if (!sxml_status_has_error_record(lazy->failure_status)) {
        return lazy->failure_status;
    }
    return sxml_item_heap_raise_error(sxml_lazy_cell_get_item_heap(lazy),
                                      &lazy->kept_error.request, error_record);
}

/* Makes room for one more produced item (D22: allocate, copy, release). */
static sxml_status sxml_lazy_reserve_one(sxml_lazy_cell *lazy) {
    if (lazy->produced_item_count < lazy->produced_item_capacity) {
        return SXML_STATUS_OK;
    }
    size_t new_capacity = SXML_LAZY_FIRST_CAPACITY;
    sxml_status status = SXML_STATUS_OK;
    if (lazy->produced_item_capacity != 0) {
        status = sxml_size_multiply(lazy->produced_item_capacity, 2,
                                    &new_capacity);
    }
    size_t new_byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(new_capacity, sizeof(sxml_item),
                                    &new_byte_count);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item_heap *item_heap = sxml_lazy_cell_get_item_heap(lazy);
    void *new_items = NULL;
    status = sxml_item_heap_allocate_bytes(
        item_heap,
        (sxml_allocation_request){.byte_count = new_byte_count,
                                  .alignment = alignof(sxml_item)},
        &new_items);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (lazy->produced_item_count != 0) {
        memcpy(new_items, lazy->produced_items,
               lazy->produced_item_count * sizeof(sxml_item));
    }
    sxml_item_heap_release_bytes(
        item_heap,
        (sxml_heap_bytes){.memory = lazy->produced_items,
                          .byte_count = lazy->produced_item_capacity *
                                        sizeof(sxml_item)});
    lazy->produced_items = new_items;
    lazy->produced_item_capacity = new_capacity;
    return SXML_STATUS_OK;
}

/*
 * Asks the generator for one more item and keeps it, or notes the end.
 * Room for the item is made first, so a produced item is never lost; a
 * failure to make room leaves the generator uncalled, and a later read may
 * try again. A failure of the generator is kept and reported now and by
 * every later read that needs an item past the last produced one.
 */
static sxml_status sxml_lazy_produce_one(sxml_lazy_cell *lazy,
                                         sxml_error_record *error_record) {
    if (lazy->failure_status != SXML_STATUS_OK) {
        return sxml_lazy_report_failure(lazy, error_record);
    }
    if (lazy->is_producing) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_status status = sxml_lazy_reserve_one(lazy);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* The generator always gets a record, so its error can be kept even
       when this read's caller passed none. */
    sxml_error_record generator_record = {
        .code = {.namespace_uri = {.bytes = NULL, .byte_count = 0},
                 .local_name = {.bytes = NULL, .byte_count = 0},
                 .namespace_prefix = {.bytes = NULL, .byte_count = 0}},
        .value = NULL,
        .diagnostic = {.description = {.bytes = NULL, .byte_count = 0},
                       .location = {.source_name = {.bytes = NULL,
                                                    .byte_count = 0},
                                    .line_number = 0,
                                    .column_number = 0,
                                    .byte_offset = 0}},
    };
    sxml_iteration_step step = sxml_iteration_step_end();
    lazy->is_producing = true;
    status = lazy->generator.operations->next(
        lazy->generator.state,
        &(sxml_generation_request){
            .item_heap = sxml_lazy_cell_get_item_heap(lazy),
            .error_record = &generator_record},
        &step);
    lazy->is_producing = false;
    if (status != SXML_STATUS_OK) {
        lazy->failure_status = status;
        if (sxml_status_has_error_record(status)) {
            sxml_status keep_status =
                sxml_lazy_keep_error(lazy, status, &generator_record);
            if (keep_status != SXML_STATUS_OK) {
                /* The error could not be copied; its absence is what
                   every read reports from now on. */
                lazy->failure_status = keep_status;
            }
        }
        return sxml_lazy_report_failure(lazy, error_record);
    }
    if (!step.has_item) {
        lazy->is_finished = true;
        return SXML_STATUS_OK;
    }
    /* The generator handed over its reference. */
    lazy->produced_items[lazy->produced_item_count] = step.item;
    lazy->produced_item_count += 1;
    return SXML_STATUS_OK;
}

static sxml_status sxml_lazy_cell_count(sxml_cell *cell,
                                        sxml_error_record *error_record,
                                        uint64_t *item_count) {
    sxml_lazy_cell *lazy = sxml_lazy_cell_from_cell(cell);
    while (!lazy->is_finished) {
        sxml_status status = sxml_lazy_produce_one(lazy, error_record);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    *item_count = lazy->produced_item_count;
    return SXML_STATUS_OK;
}

static sxml_status sxml_lazy_cell_get_item(sxml_cell *cell,
                                           sxml_item_request request,
                                           sxml_item *item) {
    sxml_lazy_cell *lazy = sxml_lazy_cell_from_cell(cell);
    while (!lazy->is_finished &&
           (uint64_t)lazy->produced_item_count <= request.item_index) {
        sxml_status status = sxml_lazy_produce_one(lazy, request.error_record);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (request.item_index >= (uint64_t)lazy->produced_item_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    *item = lazy->produced_items[request.item_index];
    return SXML_STATUS_OK;
}

static sxml_status sxml_lazy_cell_next(sxml_sequence_iterator *iterator,
                                       sxml_error_record *error_record,
                                       sxml_iteration_step *step) {
    sxml_item item = {.representation = 0};
    sxml_status status = sxml_lazy_cell_get_item(
        sxml_item_get_cell(iterator->sequence.content),
        (sxml_item_request){.item_index = iterator->next_item_index,
                            .error_record = error_record},
        &item);
    if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
        *step = sxml_iteration_step_end();
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    iterator->next_item_index += 1;
    *step = (sxml_iteration_step){.has_item = true, .item = item};
    return SXML_STATUS_OK;
}

static const sxml_sequence_cell_operations sxml_lazy_sequence_operations = {
    .sequence_representation = SXML_SEQUENCE_REPRESENTATION_LAZY,
    .count = sxml_lazy_cell_count,
    .get_item = sxml_lazy_cell_get_item,
    .next = sxml_lazy_cell_next,
};

static const sxml_cell_class sxml_lazy_cell_class = {
    .class_name = SXML_BYTE_VIEW_INITIALIZER("lazy"),
    .release_contents = sxml_lazy_cell_release_contents,
    .sequence_operations = &sxml_lazy_sequence_operations,
};

sxml_status sxml_sequence_make_lazy(sxml_item_heap *item_heap,
                                    sxml_sequence_generator generator,
                                    sxml_sequence *sequence) {
    if (generator.operations == NULL || generator.operations->next == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_cell *cell = NULL;
    sxml_status status = sxml_item_heap_allocate_cell(
        item_heap,
        &(sxml_cell_request){.cell_class = &sxml_lazy_cell_class,
                             .byte_count = sizeof(sxml_lazy_cell),
                             .alignment = alignof(sxml_lazy_cell)},
        &cell);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_lazy_cell *lazy = sxml_lazy_cell_from_cell(cell);
    lazy->generator = generator;
    lazy->produced_items = NULL;
    lazy->produced_item_count = 0;
    lazy->produced_item_capacity = 0;
    lazy->is_finished = false;
    lazy->is_producing = false;
    lazy->failure_status = SXML_STATUS_OK;
    lazy->kept_error = (sxml_kept_error){
        .text = {.memory = NULL, .byte_count = 0},
        .request = {.error_status = SXML_STATUS_OK,
                    .code = {.namespace_uri = {.bytes = NULL,
                                               .byte_count = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0},
                             .namespace_prefix = {.bytes = NULL,
                                                  .byte_count = 0}},
                    .description = {.bytes = NULL, .byte_count = 0},
                    .has_value = false,
                    .value = sxml_sequence_empty(),
                    .location = {.source_name = {.bytes = NULL,
                                                 .byte_count = 0},
                                 .line_number = 0,
                                 .column_number = 0,
                                 .byte_offset = 0}},
    };
    *sequence = sxml_sequence_from_cell(SXML_ITEM_REPRESENTATION_LAZY_CELL,
                                        cell);
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Emptiness */

sxml_status sxml_sequence_test_emptiness(sxml_sequence sequence,
                                         sxml_error_record *error_record,
                                         sxml_emptiness *emptiness) {
    /* fn:empty and fn:exists need at most the first item (F&O 4.0,
       fn:empty, fn:exists); a lazy sequence's error raised while
       producing it is reported (D43). */
    sxml_item first_item = {.representation = 0};
    sxml_status status = sxml_sequence_get_item(
        sequence,
        (sxml_item_request){.item_index = 0, .error_record = error_record},
        &first_item);
    if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
        *emptiness = (sxml_emptiness){.is_empty = true};
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *emptiness = (sxml_emptiness){.is_empty = false};
    return SXML_STATUS_OK;
}
