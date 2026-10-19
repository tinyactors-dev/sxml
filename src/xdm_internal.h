#pragma once

/*
 * The internal interface of the XDM value model (include/sxml/xdm.h;
 * docs/DECISIONS.md D33 to D38, D41 and D60): how items and sequences are
 * laid out, what a cell is, how cells are allocated, counted and freed, and
 * the atomic-item helpers the operators, casting and the function library
 * build on.
 *
 * Files
 * -----
 * - src/xdm.c: the item heap, cells, the generic item functions, the
 *   raised error, vector sequences, the sequence read interface (count,
 *   get item, iterator), the sequence builder.
 * - src/xdm_atomic.c: atomic items (make, datum, atomic-equal, hash,
 *   compare) and the helpers declared at the end of this header.
 * - Later modules add a cell class each, in their own file, without
 *   editing these: integer ranges, concatenations and lazy sequences
 *   (src/xdm_sequence.c), maps and records (src/xdm_map.c), arrays
 *   (src/xdm_array.c), function items (src/xdm_function.c), JNodes
 *   (src/xdm_json_node.c). sxml_item_release reaches their code through
 *   the cell class pointer in every cell, never through a symbol, so
 *   src/xdm.c does not depend on files that come later.
 *
 * Item layout
 * -----------
 * sxml_item (16 bytes, D34) is read through these fields:
 *
 * - representation: an sxml_item_representation below. 0 is "not an item";
 *   as the content of an sxml_sequence it is the empty sequence, so a
 *   zero-initialized sequence is empty.
 * - flags: for atomic items, the index of the primitive type
 *   (SXML_TYPE_XS_DECIMAL for every integer type, SXML_TYPE_XS_STRING for
 *   xs:token, ...), which is a built-in type below 256, so the type family
 *   of any atomic item is known without the type registry. 0 for every
 *   other representation.
 * - namespace_number: of a node item (sxml_node_reference); for an
 *   xs:QName or xs:NOTATION item, unused (0).
 * - type_or_node_index: the type annotation (an sxml_type_identifier
 *   index) of an atomic item, map, record, array or function item; the node
 *   index of a node item; 0 (SXML_TYPE_NONE) for a JNode and for sequence
 *   cells.
 * - payload: the inline value or the cell, by representation:
 *
 *     BOOLEAN        payload.integer 0 or 1
 *     INTEGER        payload.integer (any integer type that fits int64_t)
 *     FLOAT          payload.float_number (other bytes of payload are 0)
 *     DOUBLE         payload.double_number
 *     QNAME          payload.bits: name identifier in the low 32 bits,
 *                    namespace prefix identifier in the high 32 bits
 *     NODE           payload.tree; node index and namespace number above
 *     every *_CELL   payload.cell, a pointer to an sxml_cell
 *
 * An sxml_sequence holds one sxml_item, its content: representation 0 for
 * the empty sequence, an item representation for a singleton sequence (an
 * item is a singleton sequence, XDM 4.0, section 3.2), or one of the
 * sequence cell representations.
 *
 * Cells
 * -----
 * A cell is a block from the item heap that starts with an sxml_cell header:
 * its cell class (static const operations), its heap, its byte count and
 * its reference count. A module defines its cell as a struct whose first
 * member is the header ("sxml_cell cell;") and converts between the two
 * pointers. Cells are immutable once made, except for caches that do not
 * change the value (a map's lazily built index, a lazy sequence's items).
 *
 * Releasing a cell's last reference does not recurse on the C stack: the
 * cell is put on the heap's pending list, and the outermost release frees
 * pending cells one by one, calling release_contents, which may release
 * more cells onto the list. Arbitrarily deep values (an array nested 10^6
 * deep) are freed in constant stack depth.
 *
 * Ownership and counting rules are those of xdm.h (D33). A test checks for
 * leaks with sxml_item_heap_read_counts: live_cell_count and
 * live_byte_count count every cell and every byte the heap hands out
 * (side tables of cells, decimal coefficients, the raised error).
 *
 * Thread safety: everything here follows D35: one thread per heap.
 */
#include "decimal_internal.h"
#include "sxml/xdm.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ----------------------------------------------------- Representations */

/*
 * The value of sxml_item.representation. Item representations are below
 * SXML_ITEM_REPRESENTATION_FIRST_SEQUENCE_CELL; sequence cells (which are
 * only ever the content of an sxml_sequence, never an item) are at or above
 * it. Values are internal and may change; the layout of sxml_item may not.
 */
typedef enum sxml_item_representation {
    /* Not an item. As sequence content: the empty sequence. */
    SXML_ITEM_REPRESENTATION_NONE = 0,
    /* Inline atomic items. */
    SXML_ITEM_REPRESENTATION_BOOLEAN = 1,
    SXML_ITEM_REPRESENTATION_INTEGER = 2,
    SXML_ITEM_REPRESENTATION_FLOAT = 3,
    SXML_ITEM_REPRESENTATION_DOUBLE = 4,
    SXML_ITEM_REPRESENTATION_QNAME = 5,
    /* An atomic item in a cell of src/xdm_atomic.c: strings, decimals,
       integers beyond int64_t, date/time values, durations, binary. */
    SXML_ITEM_REPRESENTATION_ATOMIC_CELL = 6,
    /* An XNode, inline: retains its tree. */
    SXML_ITEM_REPRESENTATION_NODE = 7,
    /* Non-atomic items in cells. */
    SXML_ITEM_REPRESENTATION_JSON_NODE_CELL = 8,
    SXML_ITEM_REPRESENTATION_FUNCTION_CELL = 9,
    /* A map or a record (type_or_node_index tells which). */
    SXML_ITEM_REPRESENTATION_MAP_CELL = 10,
    SXML_ITEM_REPRESENTATION_ARRAY_CELL = 11,

    /* Sequence cells. */
    SXML_ITEM_REPRESENTATION_FIRST_SEQUENCE_CELL = 64,
    SXML_ITEM_REPRESENTATION_VECTOR_CELL = 64,
    SXML_ITEM_REPRESENTATION_INTEGER_RANGE_CELL = 65,
    SXML_ITEM_REPRESENTATION_CONCATENATION_CELL = 66,
    SXML_ITEM_REPRESENTATION_LAZY_CELL = 67,
} sxml_item_representation;

/* The representation of item. */
static inline sxml_item_representation sxml_item_get_representation(
    sxml_item item) {
    return (sxml_item_representation)item.representation;
}

/* True when item's payload is a cell (items and sequence cells alike). */
static inline bool sxml_item_has_cell(sxml_item item) {
    switch (sxml_item_get_representation(item)) {
    case SXML_ITEM_REPRESENTATION_ATOMIC_CELL:
    case SXML_ITEM_REPRESENTATION_JSON_NODE_CELL:
    case SXML_ITEM_REPRESENTATION_FUNCTION_CELL:
    case SXML_ITEM_REPRESENTATION_MAP_CELL:
    case SXML_ITEM_REPRESENTATION_ARRAY_CELL:
    case SXML_ITEM_REPRESENTATION_VECTOR_CELL:
    case SXML_ITEM_REPRESENTATION_INTEGER_RANGE_CELL:
    case SXML_ITEM_REPRESENTATION_CONCATENATION_CELL:
    case SXML_ITEM_REPRESENTATION_LAZY_CELL:
        return true;
    case SXML_ITEM_REPRESENTATION_NONE:
    case SXML_ITEM_REPRESENTATION_BOOLEAN:
    case SXML_ITEM_REPRESENTATION_INTEGER:
    case SXML_ITEM_REPRESENTATION_FLOAT:
    case SXML_ITEM_REPRESENTATION_DOUBLE:
    case SXML_ITEM_REPRESENTATION_QNAME:
    case SXML_ITEM_REPRESENTATION_NODE:
        return false;
    }
    return false;
}

/* True for the representations of atomic items, inline or in a cell. */
static inline bool sxml_item_is_atomic(sxml_item item) {
    return item.representation >= SXML_ITEM_REPRESENTATION_BOOLEAN &&
           item.representation <= SXML_ITEM_REPRESENTATION_ATOMIC_CELL;
}

/* ------------------------------------------------------------------ Cells */

typedef struct sxml_cell sxml_cell;

/*
 * What a sequence cell answers: the read interface of xdm.h, which
 * src/xdm.c dispatches to for the representations it does not store
 * itself. Every operation may assume its arguments were checked (the
 * sequence has this cell; the iterator belongs to it). error_record may be
 * NULL; a failing operation leaves its outputs untouched (D20).
 */
typedef struct sxml_sequence_cell_operations {
    /* The sequence representation reported by
       sxml_sequence_get_representation. */
    sxml_sequence_representation sequence_representation;
    /* Writes the number of items (sxml_sequence_count). */
    sxml_status (*count)(sxml_cell *cell, sxml_error_record *error_record,
                         uint64_t *item_count);
    /* Writes the item at request.item_index, borrowed from the cell, or
       returns SXML_STATUS_INDEX_OUT_OF_RANGE (sxml_sequence_get_item). */
    sxml_status (*get_item)(sxml_cell *cell, sxml_item_request request,
                            sxml_item *item);
    /* Advances iterator (in/out; iterator->sequence holds this cell) and
       writes the next item, borrowed, or has_item false at the end
       (sxml_sequence_iterator_next). The iterator fields next_item_index,
       part_index and next_index_in_part are the class's to use; they start
       at 0. */
    sxml_status (*next)(sxml_sequence_iterator *iterator,
                        sxml_error_record *error_record,
                        sxml_iteration_step *step);
} sxml_sequence_cell_operations;

/*
 * The static const description of one kind of cell. One per kind, defined
 * by the module that makes such cells.
 */
typedef struct sxml_cell_class {
    /* For diagnostics and tests, such as "vector". */
    sxml_byte_view class_name;
    /* Releases what the cell holds: references to other cells and trees
       (with sxml_item_release, sxml_sequence_release), decimals, generator
       or function body state, side tables (sxml_item_heap_release_bytes).
       Must not free the cell itself: the heap does that after this
       returns. Runs once, when the last reference is released. NULL when
       the cell holds nothing. */
    void (*release_contents)(sxml_cell *cell);
    /* For sequence cells, the read interface; NULL for item cells. */
    const sxml_sequence_cell_operations *sequence_operations;
} sxml_cell_class;

/*
 * The header at the start of every cell. Filled by
 * sxml_item_heap_allocate_cell; modules read it but change only through
 * the functions below.
 */
struct sxml_cell {
    const sxml_cell_class *cell_class;
    /* The heap that allocated the cell; NULL for an immortal cell in
       static data. */
    sxml_item_heap *item_heap;
    /* The size of the whole cell, header included, as allocated. */
    size_t byte_count;
    union {
        /* While the cell is live: its references, or
           SXML_CELL_IMMORTAL_REFERENCE_COUNT. */
        size_t reference_count;
        /* After the last release, while the cell waits on the heap's
           pending list. */
        sxml_cell *next_pending_cell;
    } state;
};

/*
 * The reference count of an immortal cell: retain and release leave it
 * unchanged (D33). A count that reaches it by retains becomes immortal.
 */
#define SXML_CELL_IMMORTAL_REFERENCE_COUNT SIZE_MAX

/* A cell to allocate. */
typedef struct sxml_cell_request {
    const sxml_cell_class *cell_class;
    /* The whole cell, header included: at least sizeof(sxml_cell). */
    size_t byte_count;
    /* A power of two, at least alignof(sxml_cell). */
    size_t alignment;
} sxml_cell_request;

/*
 * Allocates a cell of request->byte_count bytes, fills its header (class,
 * heap, byte count, reference count 1) and writes it to *cell. The bytes
 * after the header are uninitialized. The cell counts in live_cell_count
 * and live_byte_count until it is freed.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for a request smaller than the
 * header or with an invalid alignment, SXML_STATUS_LIMIT_EXCEEDED when the
 * heap's maximum_byte_count would be passed, and the allocator's failures.
 */
SXML_NODISCARD sxml_status sxml_item_heap_allocate_cell(
    sxml_item_heap *item_heap, const sxml_cell_request *request,
    sxml_cell **cell);

/*
 * Frees a cell that was allocated and never handed out, without calling
 * release_contents: for the failure path of a make function that fails
 * after allocating its cell and before filling it.
 */
void sxml_item_heap_discard_cell(sxml_cell *cell);

/* Adds a reference to cell. Does nothing to an immortal cell. */
void sxml_cell_retain(sxml_cell *cell);

/*
 * Drops a reference to cell; at 0 the cell's release_contents runs and the
 * cell is freed, iteratively (see "Cells" above). Does nothing to an
 * immortal cell.
 */
void sxml_cell_release(sxml_cell *cell);

/* The cell of an item or sequence content whose representation has one. */
static inline sxml_cell *sxml_item_get_cell(sxml_item item) {
    return (sxml_cell *)item.payload.cell;
}

/* The heap of a cell; NULL for an immortal static cell. */
static inline sxml_item_heap *sxml_cell_get_item_heap(const sxml_cell *cell) {
    return cell->item_heap;
}

/* An item whose payload is cell. Takes over the caller's reference. */
typedef struct sxml_cell_item_request {
    sxml_item_representation representation;
    /* The type annotation (SXML_TYPE_NONE for JNodes). */
    sxml_type_identifier type_annotation;
    /* For atomic cells: the primitive type (flags); otherwise
       SXML_TYPE_NONE. */
    sxml_type_identifier primitive_type;
    sxml_cell *cell;
} sxml_cell_item_request;

/* The item for request->cell; it owns the reference the cell came with. */
static inline sxml_item sxml_item_from_cell(
    const sxml_cell_item_request *request) {
    sxml_item item = {
        .representation = (uint8_t)request->representation,
        .flags = (uint8_t)request->primitive_type.index,
        .namespace_number = 0,
        .type_or_node_index = request->type_annotation.index,
        .payload = {.bits = 0},
    };
    item.payload.cell = request->cell;
    return item;
}

/*
 * The sequence whose content is a sequence cell (representation at or above
 * SXML_ITEM_REPRESENTATION_FIRST_SEQUENCE_CELL). Takes over the caller's
 * reference to cell.
 */
static inline sxml_sequence sxml_sequence_from_cell(
    sxml_item_representation representation, sxml_cell *cell) {
    sxml_sequence sequence = {
        .content = {.representation = (uint8_t)representation,
                    .flags = 0,
                    .namespace_number = 0,
                    .type_or_node_index = 0,
                    .payload = {.bits = 0}},
    };
    sequence.content.payload.cell = cell;
    return sequence;
}

/*
 * Makes the sequence of items (the empty sequence, the item itself or a
 * vector), taking over the caller's reference to each item on success
 * instead of retaining them, so a caller that owns its items saves a
 * retain and a release per item; on failure the references stay the
 * caller's. The array itself stays the caller's.
 */
SXML_NODISCARD sxml_status sxml_sequence_make_vector_taking(
    sxml_item_heap *item_heap, sxml_item_view items, sxml_sequence *sequence);

/* --------------------------------------------------------- Heap memory */

/*
 * Memory a cell keeps beside itself (a map's hash index, a builder's item
 * array): allocated from the heap's allocator and counted in
 * live_byte_count (not live_cell_count) and against maximum_byte_count.
 * The owner releases it with the same byte count. Failures as for
 * sxml_item_heap_allocate_cell.
 */
SXML_NODISCARD sxml_status sxml_item_heap_allocate_bytes(
    sxml_item_heap *item_heap, sxml_allocation_request request,
    void **memory);

/* What sxml_item_heap_release_bytes gives back. */
typedef struct sxml_heap_bytes {
    void *memory;
    /* The byte_count it was allocated with. */
    size_t byte_count;
} sxml_heap_bytes;

/* Releases memory from sxml_item_heap_allocate_bytes. Does nothing for
   NULL memory. */
void sxml_item_heap_release_bytes(sxml_item_heap *item_heap,
                                  sxml_heap_bytes bytes);

/*
 * An allocator over the heap, for code that allocates through the
 * allocator interface without knowing sizes on release (libmpdec through
 * the decimal scope): each block carries a hidden header with its size, so
 * it is counted and limited like any heap memory. Valid as long as the
 * heap.
 */
sxml_allocator sxml_item_heap_as_allocator(sxml_item_heap *item_heap);

/*
 * The decimal configuration of the heap: sxml_item_heap_as_allocator, the
 * default maximum and division digit counts (D57). Operators and casting
 * compute decimals for this heap's items with it, so their coefficients
 * count against the heap.
 */
const sxml_decimal_configuration *sxml_item_heap_get_decimal_configuration(
    const sxml_item_heap *item_heap);

/* ---------------------------------------------------------- Static errors */

/*
 * An error with static text (D21): the code's local name in the err
 * namespace and a description, both with static storage duration.
 */
typedef struct sxml_static_error {
    /* SXML_STATUS_STATIC_ERROR, _DYNAMIC_ERROR or _TYPE_ERROR. */
    sxml_status error_status;
    /* Such as "XPTY0004". */
    sxml_byte_view code_local_name;
    sxml_byte_view description;
} sxml_static_error;

/*
 * Fills *error_record (which may be NULL) with error, code in the err
 * namespace (SXML_ERROR_NAMESPACE_URI, prefix "err"), no value and an
 * unknown location, and returns error->error_status, so a function can end
 * with "return sxml_error_record_report(...)".
 */
sxml_status sxml_error_record_report(sxml_error_record *error_record,
                                     const sxml_static_error *error);

/* --------------------------------------------------------- Atomic items */

/*
 * The primitive type of an atomic item (from its flags): for example
 * SXML_TYPE_XS_DECIMAL for an xs:short. SXML_TYPE_NONE when item is not
 * atomic.
 */
sxml_type_identifier sxml_atomic_item_get_primitive_type(sxml_item item);

/* True for atomic items of a numeric type: xs:decimal (and so every
   integer type), xs:float, xs:double. */
bool sxml_atomic_item_is_numeric(sxml_item item);

/*
 * The decimal of an xs:decimal item, or of an integer item beyond int64_t,
 * borrowed from its cell; NULL for every other item, inline integers
 * included (use sxml_decimal_from_integer on those).
 */
const sxml_decimal *sxml_atomic_item_get_decimal(sxml_item item);

/* A decimal that becomes an atomic item. */
typedef struct sxml_decimal_item_request {
    /* xs:decimal, xs:integer or a type derived from them. */
    sxml_type_identifier type;
    /* Taken on success: the item owns it and the caller must not finalize
       it. Left with the caller on failure. */
    sxml_decimal *decimal;
} sxml_decimal_item_request;

/*
 * Makes an atomic item from a computed decimal without converting it to
 * text and back (the operators and casting). An integer type with a value
 * that fits int64_t gives an inline item (and finalizes the decimal); a
 * value outside the type's range or a fraction for an integer type is
 * SXML_STATUS_INVALID_ARGUMENT. Writes the item, owned, to *item. Returns
 * the heap's failures.
 */
SXML_NODISCARD sxml_status sxml_atomic_item_make_taking_decimal(
    sxml_item_heap *item_heap, const sxml_decimal_item_request *request,
    sxml_item *item);

/*
 * sxml_atomic_item_compare with the name pool of item_heap, which orders
 * xs:QName and xs:NOTATION items by namespace URI, then local name, in
 * codepoint order (F&O 4.0, fn:compare). item_heap may be NULL: then QNames
 * with different expanded names give SXML_STATUS_UNSUPPORTED_FEATURE
 * (xdm.h has no name pool to compare them with; D60). Other failures as
 * for sxml_atomic_item_compare.
 */
SXML_NODISCARD sxml_status sxml_atomic_item_compare_in_heap(
    const sxml_item_heap *item_heap, const sxml_atomic_comparison *comparison,
    sxml_ordering *ordering);

/* ---------------------------------------------------------------- URIs */

/* A URI reference to resolve against a base URI (src/xdm_node.c). */
typedef struct sxml_uri_resolution_request {
    /* An absolute URI, or empty when there is no base. */
    sxml_byte_view base_uri;
    sxml_byte_view reference;
    /* Provides the memory of a resolved target URI. */
    sxml_allocator allocator;
} sxml_uri_resolution_request;

/* The target URI of a resolution. */
typedef struct sxml_uri_resolution {
    /* In memory, or request->reference itself when memory is NULL. */
    sxml_byte_view uri;
    /* Owned: release it through the request's allocator; NULL when nothing
       was allocated. */
    void *memory;
} sxml_uri_resolution;

/* True when text starts with a scheme followed by ":" (RFC 3986, section
   3.1, split as appendix B does): an absolute URI or a URI with a
   fragment. */
bool sxml_uri_has_scheme(sxml_byte_view text);

/*
 * Resolves request->reference against request->base_uri (RFC 3986,
 * section 5.2.2, strict, with remove_dot_segments and the recomposition of
 * section 5.3) and writes the target URI to *resolution. A reference with a
 * scheme, and any reference when the base is empty, is the target as it is
 * (F&O 4.0, fn:resolve-uri: an absolute $href is returned unchanged).
 * Returns the allocator's failures and SXML_STATUS_ARITHMETIC_OVERFLOW;
 * *resolution is then untouched. Used by the processor's document loader
 * (fn:doc) and fn:resolve-uri.
 */
SXML_NODISCARD sxml_status sxml_uri_resolve(
    const sxml_uri_resolution_request *request,
    sxml_uri_resolution *resolution);
