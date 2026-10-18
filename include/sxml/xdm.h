#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/export.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/status.h"
#include "sxml/string.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/view.h"

/*
 * The XDM 4.0 value model: items, sequences, atomic items, node items,
 * function items, maps, records, arrays and JNodes (docs/DECISIONS.md,
 * D33 to D38). One model, shared by XPath, XQuery and XSLT.
 *
 * Values
 * ------
 * Every value is a sequence of items (XDM 4.0, section 3.1). sxml_item and
 * sxml_sequence are 16-byte plain values, passed and returned by value.
 * Small data lives inline in them: xs:boolean, xs:double, xs:float,
 * xs:QName and xs:NOTATION, every integer type while the value fits in
 * int64_t, node references, the empty sequence and a sequence of one item.
 * Everything else lives in a cell: an immutable, reference-counted object
 * in an item heap, which the item or sequence points to.
 *
 * Ownership (D33)
 * ---------------
 * Values are immutable and reference counted. A holder of an item or
 * sequence either owns one reference to it or borrows it:
 *
 * - Functions whose names contain make, concatenate, put, append, insert,
 *   remove, slice, finish or call write an owned value to their output: the
 *   caller must release it exactly once with sxml_item_release or
 *   sxml_sequence_release.
 * - Functions whose names contain get, describe or find, and the next
 *   functions of iterators, write borrowed values: valid as long as the
 *   value they were read from, never released by the caller. To keep one
 *   longer, retain it. (The next operation of a sequence generator, which
 *   sxml calls, is the one exception: it hands over an owned item, as its
 *   declaration says.)
 * - Inputs are always borrowed. A function that stores an input (a vector
 *   storing its items, a map storing a key) retains it itself; the caller
 *   still owns its own reference.
 * - Inline values need no memory: retain and release do nothing to them,
 *   so code may treat every value alike.
 *
 * XDM values cannot form cycles (they are built bottom-up and never
 * change, and closures capture values, not variables), so reference
 * counting reclaims everything without a cycle collector. Cells of static
 * data are immortal.
 *
 * Errors
 * ------
 * Requests that can raise a specification error carry an optional error
 * record; such a function returns SXML_STATUS_DYNAMIC_ERROR or
 * SXML_STATUS_TYPE_ERROR and fills it. Outputs are untouched on failure
 * (D20). An error with a computed description or value is kept by the item
 * heap (sxml_item_heap_raise_error, D41).
 *
 * Thread safety (D35)
 * -------------------
 * Reference counts are not atomic. An item heap, every value in it, and the
 * name pool, type registry and trees it serves are used by one thread at a
 * time. Separate heaps are independent. Functions that take only inline
 * values are safe from any thread.
 */

/* ------------------------------------------------------------ Item heap */

/*
 * The owner of cells: allocates them, frees them when their last reference
 * is released, issues tree identifiers, and knows the name pool and type
 * registry that the values it holds refer to. One per processor and thread.
 */
typedef struct sxml_item_heap sxml_item_heap;

typedef struct sxml_item_heap_configuration {
    /* Provides memory for cells. Must not be the null allocator; must
       outlive the heap. */
    sxml_allocator allocator;
    /* Kept by reference; both must outlive the heap, and the registry must
       use name_pool. */
    sxml_name_pool *name_pool;
    sxml_type_registry *type_registry;
    /* Most bytes the heap may hold in live cells, a bound for untrusted
       input; 0 means no bound. Reaching it gives
       SXML_STATUS_LIMIT_EXCEEDED. */
    size_t maximum_byte_count;
} sxml_item_heap_configuration;

/* What an item heap holds, for tests and limits. */
typedef struct sxml_item_heap_counts {
    /* Cells not yet freed. A test that released everything expects 0; a
       raised error's value counts until sxml_item_heap_clear_raised_error. */
    uint64_t live_cell_count;
    /* Bytes in live cells. */
    uint64_t live_byte_count;
    /* Tree identifiers issued so far, to trees and to JTrees. */
    uint64_t issued_tree_identifier_count;
} sxml_item_heap_counts;

/*
 * Writes the memory requirements of an item heap, without its cells, to
 * *requirements. Returns SXML_STATUS_INVALID_ARGUMENT for an invalid
 * configuration.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_item_heap_memory_requirements(
    const sxml_item_heap_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places an item heap in memory and writes its address to *item_heap.
 * memory must stay valid, unmoved, until sxml_item_heap_finalize.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for an invalid configuration, or
 * SXML_STATUS_INSUFFICIENT_MEMORY or SXML_STATUS_MISALIGNED_MEMORY when
 * memory does not meet the memory requirements.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_item_heap_initialize(
    sxml_memory_view memory, const sxml_item_heap_configuration *configuration,
    sxml_item_heap **item_heap);

/*
 * Releases the heap's own memory. Every value must have been released
 * before (live_cell_count 0); cells still live are not freed and become
 * invalid. Does nothing when item_heap is NULL.
 */
SXML_EXPORT void sxml_item_heap_finalize(sxml_item_heap *item_heap);

/*
 * Allocates an item heap through allocator and initializes it. allocator
 * is stored and must outlive the heap. Returns the failures of
 * sxml_item_heap_initialize and sxml_allocator_allocate.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_item_heap_create(
    sxml_allocator allocator, const sxml_item_heap_configuration *configuration,
    sxml_item_heap **item_heap);

/*
 * Finalizes an item heap made by sxml_item_heap_create and releases its
 * memory. Does nothing when item_heap is NULL.
 */
SXML_EXPORT void sxml_item_heap_destroy(sxml_item_heap *item_heap);

/* The counts so far. */
SXML_EXPORT sxml_item_heap_counts sxml_item_heap_read_counts(
    const sxml_item_heap *item_heap);

/*
 * Issues the next tree identifier: larger than every identifier issued
 * before by this heap. Used by sxml_tree_initialize and for root JNodes.
 */
SXML_EXPORT sxml_tree_identifier sxml_item_heap_issue_tree_identifier(
    sxml_item_heap *item_heap);

/* The name pool and type registry of the configuration. */
SXML_EXPORT sxml_name_pool *sxml_item_heap_get_name_pool(
    const sxml_item_heap *item_heap);
SXML_EXPORT sxml_type_registry *sxml_item_heap_get_type_registry(
    const sxml_item_heap *item_heap);

/* ----------------------------------------------------------------- Items */

/* The kinds of item (XDM 4.0, section 3.1). */
typedef enum sxml_item_kind {
    /* An atomic item: a type annotation and a datum (XDM 4.0, section 5). */
    SXML_ITEM_KIND_ATOMIC,
    /* An XNode, referred to by an sxml_node_reference. */
    SXML_ITEM_KIND_NODE,
    /* A JNode (XDM 4.0, section 8.5). */
    SXML_ITEM_KIND_JSON_NODE,
    /* A function item that is neither a map nor an array. */
    SXML_ITEM_KIND_FUNCTION,
    /* A map; a record is a map whose type annotation is a record type. */
    SXML_ITEM_KIND_MAP,
    /* An array. */
    SXML_ITEM_KIND_ARRAY,
} sxml_item_kind;

/*
 * One item. A plain 16-byte value whose layout is frozen as part of the ABI
 * (D34); its fields are private to sxml: read items through the functions
 * below. A zero-initialized sxml_item is not an item.
 */
typedef struct sxml_item {
    uint8_t representation;
    uint8_t flags;
    uint16_t namespace_number;
    uint32_t type_or_node_index;
    union {
        int64_t integer;
        uint64_t bits;
        double double_number;
        float float_number;
        void *cell;
        sxml_tree *tree;
    } payload;
} sxml_item;

_Static_assert(sizeof(sxml_item) == 16, "sxml_item is 16 bytes (D34)");

/* A read-only list of items. */
typedef struct sxml_item_view {
    const sxml_item *items;
    size_t item_count;
} sxml_item_view;

/* The kind of item. */
SXML_EXPORT sxml_item_kind sxml_item_get_kind(sxml_item item);

/*
 * True for function items, maps and arrays included (XDM 4.0, section 8:
 * maps and arrays are function items).
 */
SXML_EXPORT bool sxml_item_is_function_item(sxml_item item);

/* True for XNodes and JNodes: the generic nodes of XDM 4.0, section 6. */
SXML_EXPORT bool sxml_item_is_generic_node(sxml_item item);

/*
 * The type annotation of an atomic item (its most specific atomic type), of
 * a map (its record type, or SXML_TYPE_MAP), of an array (SXML_TYPE_ARRAY)
 * and of a function item (its signature, a function type). For nodes and
 * JNodes: SXML_TYPE_NONE; a node's annotation is in its description.
 */
SXML_EXPORT sxml_type_identifier sxml_item_get_type_annotation(
    sxml_item item);

/* Adds one reference to item's cell. Does nothing for inline items. */
SXML_EXPORT void sxml_item_retain(sxml_item item);

/*
 * Drops one reference to item's cell, freeing the cell (and releasing what
 * it holds) at 0. A node item releases its tree. Does nothing for inline
 * items.
 */
SXML_EXPORT void sxml_item_release(sxml_item item);

/* ---------------------------------------------------------- Atomic items */

/*
 * The C form of a datum (XDM 4.0, section 5): which member of
 * sxml_atomic_datum holds it. Each primitive type has one datum kind.
 */
typedef enum sxml_datum_kind {
    /* xs:boolean. */
    SXML_DATUM_KIND_BOOLEAN,
    /* xs:integer and the types derived from it, when the value fits in
       int64_t. */
    SXML_DATUM_KIND_INTEGER,
    /* xs:decimal, and xs:integer types beyond int64_t, as decimal text. */
    SXML_DATUM_KIND_DECIMAL,
    /* xs:float. */
    SXML_DATUM_KIND_FLOAT,
    /* xs:double. */
    SXML_DATUM_KIND_DOUBLE,
    /* xs:string, xs:anyURI, xs:untypedAtomic and types derived from them. */
    SXML_DATUM_KIND_STRING,
    /* xs:QName, xs:NOTATION. */
    SXML_DATUM_KIND_QNAME,
    /* xs:dateTime, xs:date, xs:time, xs:gYearMonth, xs:gYear,
       xs:gMonthDay, xs:gMonth, xs:gDay and types derived from them. */
    SXML_DATUM_KIND_DATE_TIME,
    /* xs:duration and the types derived from it. */
    SXML_DATUM_KIND_DURATION,
    /* xs:hexBinary, xs:base64Binary: the octets, not their encoding. */
    SXML_DATUM_KIND_BINARY,
} sxml_datum_kind;

/*
 * The seven components of a date/time datum (XDM 4.0, section 5.4). The
 * type annotation says which are present; absent ones are 0. Components
 * keep their local value, not normalized to UTC; 24:00:00 is already
 * normalized to 00:00:00 of the next day.
 *
 * year uses XSD 1.1 numbering: year 0 is 1 BCE. Seconds are second plus
 * nanosecond / 10^9: sxml keeps nine fractional digits, more than the
 * three XDM 4.0 section 5.1 requires.
 */
typedef struct sxml_date_time {
    int64_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint32_t nanosecond;
    bool has_timezone;
    /* Minutes east of UTC, -840 to 840 (XDM 4.0, section 5.4). */
    int16_t timezone_offset_minutes;
} sxml_date_time;

/*
 * A duration datum: an integer number of months and a decimal number of
 * seconds, here seconds plus nanoseconds (XDM 4.0, section 5.7, note on
 * durations). All three have the same sign; xs:dayTimeDuration has months
 * 0, xs:yearMonthDuration has seconds and nanoseconds 0.
 */
typedef struct sxml_duration {
    int64_t months;
    int64_t seconds;
    int32_t nanoseconds;
} sxml_duration;

/*
 * The datum of an xs:QName or xs:NOTATION: an expanded name and the prefix
 * to show it with (XDM 4.0, section 5.6). The prefix takes no part in
 * comparisons.
 */
typedef struct sxml_qname_datum {
    sxml_name_identifier expanded_name;
    sxml_namespace_prefix_identifier namespace_prefix;
} sxml_qname_datum;

/*
 * A datum. A plain value; views are borrowed: when making an item they need
 * only last for the call (the item copies them), when read from an item
 * they are owned by the item.
 */
typedef struct sxml_atomic_datum {
    sxml_datum_kind datum_kind;
    union {
        bool boolean;
        int64_t integer;
        /* The canonical form of xs:decimal (F&O 4.0, section 23.1.2.1):
           optional "-", digits, and a "." followed by digits only when the
           value is not integral. When making an item, any xs:decimal
           lexical form is accepted. */
        sxml_byte_view decimal_text;
        float float_number;
        double double_number;
        sxml_string string;
        sxml_qname_datum qname;
        sxml_date_time date_time;
        sxml_duration duration;
        sxml_byte_view binary;
    } datum_details;
} sxml_atomic_datum;

/* An atomic item to make: a type annotation and a datum. */
typedef struct sxml_atomic_item_request {
    /* An atomic type: built-in, or (Phase 5) derived in a schema. */
    sxml_type_identifier type;
    sxml_atomic_datum datum;
} sxml_atomic_item_request;

/*
 * Makes an atomic item and writes it, owned, to *item. The datum is
 * checked against the type: its datum kind must be the one of the type's
 * primitive type, and it must be in the type's value space (the range of
 * xs:byte, the syntax of xs:NCName, a timezone for xs:dateTimeStamp, months
 * 0 for xs:dayTimeDuration, ...). Text and octets are copied. An integer
 * that fits in int64_t is kept inline whatever datum kind it was given in.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT when type is not an atomic type or
 * the datum is not in its value space (casting, which must raise FORG0001
 * instead, checks before calling), and the heap's failures.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_atomic_item_make(
    sxml_item_heap *item_heap, const sxml_atomic_item_request *request,
    sxml_item *item);

/* An inline xs:integer item; needs no heap and no release. */
SXML_EXPORT sxml_item sxml_atomic_item_from_integer(int64_t integer);

/* An inline xs:double item; needs no heap and no release. */
SXML_EXPORT sxml_item sxml_atomic_item_from_double(double double_number);

/* The inline xs:boolean items true and false. */
SXML_EXPORT sxml_item sxml_atomic_item_true(void);
SXML_EXPORT sxml_item sxml_atomic_item_false(void);

/*
 * Writes the datum of an atomic item to *datum, borrowed from item. An
 * integer outside int64_t and every xs:decimal give SXML_DATUM_KIND_DECIMAL
 * with the canonical text, which is made on first request and kept in the
 * item's cell.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT when item is not atomic, and the
 * heap's failures for the decimal text.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_atomic_item_get_datum(
    sxml_item item, sxml_atomic_datum *datum);

/*
 * True when first and second are equal under fn:atomic-equal (F&O 4.0,
 * section 2.2.1): the equality of map keys. Never fails, depends on no
 * context, is reflexive (NaN equals NaN), symmetric and transitive. Strings,
 * xs:anyURI and xs:untypedAtomic compare by codepoint; numeric values by
 * exact magnitude across xs:integer, xs:decimal, xs:float and xs:double;
 * date/time values with a timezone never equal values without one; the two
 * binary types compare by octets. False for items that are not atomic.
 */
SXML_EXPORT bool sxml_atomic_item_is_atomic_equal(sxml_item first,
                                                  sxml_item second);

/*
 * A hash consistent with sxml_atomic_item_is_atomic_equal: atomic-equal
 * items have equal hashes, xs:integer 1 and xs:double 1.0e0 included. Never
 * fails. The value is stable for the life of the process only.
 */
SXML_EXPORT uint64_t sxml_atomic_item_hash(sxml_item item);

/* Two atomic items to order by fn:compare. */
typedef struct sxml_atomic_comparison {
    sxml_item first;
    sxml_item second;
    /* The implicit timezone of the dynamic context, used for date/time
       values without a timezone (F&O 4.0, section 2.2.2). */
    int16_t implicit_timezone_offset_minutes;
    /* Receives XPTY0004 for items of different type families; may be
       NULL. */
    sxml_error_record *error_record;
    /* The pool the names of xs:QName and xs:NOTATION items were interned
       in, to order them by namespace URI and local name (F&O 4.0,
       fn:compare). May be NULL when neither item is a QName or NOTATION
       (D68). */
    const sxml_name_pool *name_pool;
} sxml_atomic_comparison;

/*
 * Orders two atomic items as fn:compare does with the Unicode codepoint
 * collation (F&O 4.0, section 2.2.2; XDM 4.0, section 5.7): within a type
 * family, NaN before every other number and equal to itself.
 *
 * xs:QName and xs:NOTATION items order by namespace URI, then local name,
 * in codepoint order, ignoring the prefix.
 *
 * Returns SXML_STATUS_TYPE_ERROR (XPTY0004) when the items belong to
 * different type families or are not atomic, and
 * SXML_STATUS_INVALID_ARGUMENT for two QNames with different expanded
 * names when comparison->name_pool is NULL or does not hold their names.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_atomic_item_compare(
    const sxml_atomic_comparison *comparison, sxml_ordering *ordering);

/* ------------------------------------------------------------ Sequences */

/*
 * A sequence: what status.h declares for error values. A plain 16-byte
 * value with private fields, like sxml_item; read it through the functions
 * below. A zero-initialized sxml_sequence is the empty sequence.
 */
struct sxml_sequence {
    sxml_item content;
};

/* A read-only list of sequences. */
typedef struct sxml_sequence_view {
    const sxml_sequence *sequences;
    size_t sequence_count;
} sxml_sequence_view;

/*
 * How a sequence is stored (D34). Every representation is read the same
 * way, through the functions below; the representation is reported for
 * tests and for the evaluator's fast paths.
 */
typedef enum sxml_sequence_representation {
    /* No items, no cell. */
    SXML_SEQUENCE_REPRESENTATION_EMPTY,
    /* One item, held inline: an item is a singleton sequence (XDM 4.0,
       section 3.2). */
    SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM,
    /* Items in an array in a cell. */
    SXML_SEQUENCE_REPRESENTATION_VECTOR,
    /* The xs:integer items first to last (the range expression), stored as
       the two bounds. */
    SXML_SEQUENCE_REPRESENTATION_INTEGER_RANGE,
    /* Two or more sequences one after the other, without copying them. */
    SXML_SEQUENCE_REPRESENTATION_CONCATENATION,
    /* Items produced on demand by a sequence generator and kept once
       produced. */
    SXML_SEQUENCE_REPRESENTATION_LAZY,
    /* The xs:integer items last to first of a range, stored as its two
       bounds (fn:reverse of a range, D143). */
    SXML_SEQUENCE_REPRESENTATION_DESCENDING_INTEGER_RANGE,
} sxml_sequence_representation;

/* The empty sequence. */
SXML_EXPORT sxml_sequence sxml_sequence_empty(void);

/*
 * The singleton sequence of item. It shares item's reference: whoever owned
 * item now owns the sequence instead, so release one or the other, not
 * both.
 */
SXML_EXPORT sxml_sequence sxml_sequence_from_item(sxml_item item);

/* How sequence is stored (see sxml_sequence_representation). */
SXML_EXPORT sxml_sequence_representation sxml_sequence_get_representation(
    sxml_sequence sequence);

/* Adds one reference to the sequence's cell, if it has one. */
SXML_EXPORT void sxml_sequence_retain(sxml_sequence sequence);

/* Drops one reference to the sequence's cell, if it has one. */
SXML_EXPORT void sxml_sequence_release(sxml_sequence sequence);

/*
 * Makes the sequence of the items in items, in order, and writes it, owned,
 * to *sequence. The items are retained. No items give the empty sequence
 * and one item a single-item sequence, neither allocating.
 *
 * Returns the heap's failures.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_sequence_make_vector(
    sxml_item_heap *item_heap, sxml_item_view items, sxml_sequence *sequence);

/* The bounds of an integer range, both included. */
typedef struct sxml_integer_range {
    int64_t first_integer;
    int64_t last_integer;
} sxml_integer_range;

/*
 * Makes the sequence first_integer to last_integer of xs:integer items
 * (XPath 4.0, section 4.8.2 "Range Expressions"): empty when first_integer
 * is greater than last_integer. O(1) in time and memory. Bounds beyond int64_t are the evaluator's
 * concern (it materializes or raises an error).
 *
 * Returns the heap's failures.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_sequence_make_integer_range(
    sxml_item_heap *item_heap, sxml_integer_range range,
    sxml_sequence *sequence);

/*
 * Makes the sequence last_integer down to first_integer of xs:integer
 * items, the reverse of sxml_sequence_make_integer_range's (fn:reverse,
 * F&O 4.0, section 14.1.12; D143): empty when first_integer is greater
 * than last_integer, one item when they are equal. O(1) in time and
 * memory.
 *
 * Returns the heap's failures.
 */
SXML_NODISCARD SXML_EXPORT sxml_status
sxml_sequence_make_descending_integer_range(sxml_item_heap *item_heap,
                                            sxml_integer_range range,
                                            sxml_sequence *sequence);

/*
 * Makes the concatenation of sequences, in order (dm:sequence-concat, the
 * comma operator; XDM 4.0, section 3.2.2), and writes it, owned, to
 * *sequence. The parts are retained, not copied; empty parts are dropped,
 * and nested concatenations are flattened, so iteration stays O(1) per
 * item.
 *
 * Returns the heap's failures.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_sequence_concatenate(
    sxml_item_heap *item_heap, sxml_sequence_view sequences,
    sxml_sequence *sequence);

/* What one call to an iterator or generator yields. */
typedef struct sxml_iteration_step {
    bool has_item;
    sxml_item item;
} sxml_iteration_step;

/* What a sequence generator is asked for. */
typedef struct sxml_generation_request {
    sxml_item_heap *item_heap;
    /* Receives an error raised while producing the item; may be NULL. */
    sxml_error_record *error_record;
} sxml_generation_request;

/*
 * The operations of a sequence generator, the producer behind a lazy
 * sequence (for example a function call whose result is consumed one item
 * at a time).
 */
typedef struct sxml_sequence_generator_operations {
    /* Produces the next item, owned (the lazy sequence takes the
       reference), in step->item and sets step->has_item; or sets has_item
       false at the end. Called again only while items are left; never
       after an end or an error. An error with a computed description or
       value is raised with sxml_item_heap_raise_error on
       request->item_heap. */
    sxml_status (*next)(void *state, const sxml_generation_request *request,
                        sxml_iteration_step *step);
    /* Releases state; called once, when the lazy sequence's cell is freed.
       May be NULL. */
    void (*release)(void *state);
} sxml_sequence_generator_operations;

typedef struct sxml_sequence_generator {
    void *state;
    const sxml_sequence_generator_operations *operations;
} sxml_sequence_generator;

/*
 * Makes a lazy sequence over generator and writes it, owned, to *sequence.
 * The sequence takes generator's state (released through its release
 * operation). Items are produced when first read, then kept, so every
 * reading sees the same items and the generator runs at most once per
 * item. An error from the generator is reported by the read that needed
 * the item, and by every later one.
 *
 * Returns the heap's failures; generator's state is then not taken.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_sequence_make_lazy(
    sxml_item_heap *item_heap, sxml_sequence_generator generator,
    sxml_sequence *sequence);

/*
 * Writes the number of items to *item_count (dm:count). Reads a lazy
 * sequence to its end. Returns the generator's failures, with
 * error_record (which may be NULL) filled.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_sequence_count(
    sxml_sequence sequence, sxml_error_record *error_record,
    uint64_t *item_count);

/* The outcome of sxml_sequence_test_emptiness. */
typedef struct sxml_emptiness {
    bool is_empty;
} sxml_emptiness;

/*
 * Writes to *emptiness whether sequence has no items (fn:empty, fn:exists).
 * Reads at most one item of a lazy sequence. Returns the generator's
 * failures, with error_record (which may be NULL) filled; *emptiness is
 * then untouched.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_sequence_test_emptiness(
    sxml_sequence sequence, sxml_error_record *error_record,
    sxml_emptiness *emptiness);

/* An item to read from a sequence by position. */
typedef struct sxml_item_request {
    /* From 0. */
    uint64_t item_index;
    /* Receives a generator's error; may be NULL. */
    sxml_error_record *error_record;
} sxml_item_request;

/*
 * Writes the item at request.item_index, borrowed from sequence, to *item.
 * O(1) for every representation except concatenation (O(number of parts))
 * and lazy sequences not yet read that far.
 *
 * Returns SXML_STATUS_INDEX_OUT_OF_RANGE when there are not that many
 * items, and the generator's failures.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_sequence_get_item(
    sxml_sequence sequence, sxml_item_request request, sxml_item *item);

/*
 * Walks the items of a sequence in order (dm:iterate-sequence). A plain
 * value the caller keeps; its fields are private to sxml, and its size is
 * part of the ABI so callers can keep it on the stack. It borrows the
 * sequence, which must outlive it, and the items it yields are borrowed
 * from the sequence.
 */
typedef struct sxml_sequence_iterator {
    sxml_sequence sequence;
    uint64_t next_item_index;
    size_t part_index;
    uint64_t next_index_in_part;
} sxml_sequence_iterator;

/* An iterator at the first item of sequence. */
SXML_EXPORT sxml_sequence_iterator sxml_sequence_iterator_start(
    sxml_sequence sequence);

/*
 * Moves iterator (in/out) to the next item and writes it, borrowed, to
 * *step, or sets step->has_item false at the end. Returns a generator's
 * failures, with error_record (may be NULL) filled; then iterator and
 * *step are untouched.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_sequence_iterator_next(
    sxml_sequence_iterator *iterator, sxml_error_record *error_record,
    sxml_iteration_step *step);

/*
 * Collects items one at a time into a new sequence: how the evaluator
 * builds results whose length it does not know in advance.
 *
 * Thread safety: one thread at a time, the thread of its item heap.
 */
typedef struct sxml_sequence_builder sxml_sequence_builder;

typedef struct sxml_sequence_builder_configuration {
    /* The heap the result is made in; must outlive the builder. */
    sxml_item_heap *item_heap;
} sxml_sequence_builder_configuration;

/*
 * Writes the memory requirements of a sequence builder to *requirements.
 * Returns SXML_STATUS_INVALID_ARGUMENT for an invalid configuration.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_sequence_builder_memory_requirements(
    const sxml_sequence_builder_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places an empty sequence builder in memory and writes its address to
 * *builder. memory must stay valid, unmoved, until
 * sxml_sequence_builder_finalize. Failures as for sxml_item_heap_initialize.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_sequence_builder_initialize(
    sxml_memory_view memory,
    const sxml_sequence_builder_configuration *configuration,
    sxml_sequence_builder **builder);

/*
 * Releases the items collected and not yet finished. Does nothing when
 * builder is NULL.
 */
SXML_EXPORT void sxml_sequence_builder_finalize(
    sxml_sequence_builder *builder);

/*
 * Allocates a sequence builder through allocator and initializes it.
 * allocator is stored and must outlive the builder. Returns the failures of
 * sxml_sequence_builder_initialize and sxml_allocator_allocate.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_sequence_builder_create(
    sxml_allocator allocator,
    const sxml_sequence_builder_configuration *configuration,
    sxml_sequence_builder **builder);

/* Finalizes a builder made by create and releases its memory. Does nothing
   when builder is NULL. */
SXML_EXPORT void sxml_sequence_builder_destroy(sxml_sequence_builder *builder);

/*
 * Appends item, retaining it. Returns the heap's failures; the builder is
 * then unchanged. Amortized O(1).
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_sequence_builder_append_item(
    sxml_sequence_builder *builder, sxml_item item);

/*
 * Appends the items of sequence, retaining what it keeps. Long sequences
 * may be kept as parts of a concatenation rather than copied. Returns the
 * heap's failures and a lazy sequence's, with error_record (may be NULL)
 * filled; the builder is then unchanged.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_sequence_builder_append_sequence(
    sxml_sequence_builder *builder, sxml_sequence sequence,
    sxml_error_record *error_record);

/*
 * Writes the collected items, owned, as a sequence to *sequence, and empties
 * the builder for reuse. Returns the heap's failures; the builder is then
 * unchanged.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_sequence_builder_finish(
    sxml_sequence_builder *builder, sxml_sequence *sequence);

/* -------------------------------------------------------- Raised errors */

/*
 * A specification error whose description or value is computed (fn:error,
 * an extension function, a lazy sequence re-reporting its error), to be
 * kept by the item heap. Errors with only static text need none of this:
 * fill the error record directly (D21).
 */
typedef struct sxml_error_raise_request {
    /* SXML_STATUS_STATIC_ERROR, SXML_STATUS_DYNAMIC_ERROR or
       SXML_STATUS_TYPE_ERROR. */
    sxml_status error_status;
    /* Copied. */
    sxml_error_code code;
    /* UTF-8, copied; may be empty. */
    sxml_byte_view description;
    /* $err:value. When has_value is false the record's value is NULL. */
    bool has_value;
    /* Retained by the heap. */
    sxml_sequence value;
    /* location.source_name is copied. */
    sxml_source_location location;
} sxml_error_raise_request;

/*
 * Keeps a copy of request in item_heap, replacing (and releasing) the error
 * raised on it before, and fills *error_record with views of the copy: the
 * code, description, value and location. They stay valid until the next
 * error raised on the heap, sxml_item_heap_clear_raised_error, or
 * sxml_item_heap_finalize (D41). error_record may be NULL: then nothing is
 * kept.
 *
 * Returns request->error_status once the record is filled, so a function
 * body can end with "return sxml_item_heap_raise_error(...)";
 * SXML_STATUS_INVALID_ARGUMENT when error_status is not one for which
 * sxml_status_has_error_record is true; and the heap's failures
 * (SXML_STATUS_OUT_OF_MEMORY, SXML_STATUS_LIMIT_EXCEEDED), leaving
 * *error_record untouched.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_item_heap_raise_error(
    sxml_item_heap *item_heap, const sxml_error_raise_request *request,
    sxml_error_record *error_record);

/*
 * Releases the error last raised on item_heap. Records filled from it
 * become invalid. Does nothing when none is kept.
 */
SXML_EXPORT void sxml_item_heap_clear_raised_error(sxml_item_heap *item_heap);

/* ------------------------------------------------------------ Node items */

/*
 * The item for node, owned: it retains node.tree. Needs no heap: the
 * reference is held inline.
 */
SXML_EXPORT sxml_item sxml_node_item_make(sxml_node_reference node);

/*
 * Writes the node of a node item to *node, borrowed from item. Returns
 * SXML_STATUS_INVALID_ARGUMENT when item is not an XNode.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_item_get_node(
    sxml_item item, sxml_node_reference *node);

/*
 * Makes dm:string-value of node as an owned xs:string item (XDM 4.0, section
 * 7.6.12): for documents and elements, the concatenated text descendants.
 * Returns the heap's and the tree's failures.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_node_make_string_value(
    sxml_item_heap *item_heap, sxml_node_reference node, sxml_item *item);

/* A node whose typed value is wanted. */
typedef struct sxml_node_value_request {
    sxml_node_reference node;
    /* Receives FOTY0012 when the typed value is absent; may be NULL. */
    sxml_error_record *error_record;
} sxml_node_value_request;

/*
 * Makes dm:typed-value of request->node as an owned sequence (XDM 4.0,
 * section 7.6.14): the string value as xs:untypedAtomic for untyped
 * elements and attributes, as xs:string for comments, processing
 * instructions and namespace nodes; for validated nodes through the
 * tree's get_typed_value, or, when the tree has none, derived from the
 * string value and the type annotation as include/sxml/tree.h describes
 * (D165).
 *
 * Returns SXML_STATUS_TYPE_ERROR (FOTY0012) when the node has no typed
 * value, and the heap's and the tree's failures.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_node_make_typed_value(
    sxml_item_heap *item_heap, const sxml_node_value_request *request,
    sxml_sequence *typed_value);

/*
 * Makes dm:base-uri of request->node as an owned sequence: an xs:anyURI
 * item, or the empty sequence when it is absent. The base URI comes from
 * xml:base attributes on the node and its ancestors, resolved against the
 * tree's base URI (XDM 4.0, section 7.3; RFC 3986, section 5).
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_node_make_base_uri(
    sxml_item_heap *item_heap, const sxml_node_value_request *request,
    sxml_sequence *base_uri);

/* Two items to compare. */
typedef struct sxml_item_pair {
    sxml_item first;
    sxml_item second;
} sxml_item_pair;

/*
 * True when two generic nodes are the same node (XDM 4.0, section 6.1; the
 * "is" operator). Two non-root JNodes are the same when their parents are
 * the same and their keys atomic-equal (XDM 4.0, section 8.5). False when
 * an item is not a generic node.
 */
SXML_EXPORT bool sxml_generic_node_is_same_node(sxml_item_pair pair);

/*
 * Orders two generic nodes in document order (XDM 4.0, section 6.2):
 * nodes of different trees (XTrees or JTrees) by tree identifier, nodes of
 * one XTree as sxml_node_compare_document_order does, nodes of one JTree
 * by their child positions from the root. Returns
 * SXML_STATUS_INVALID_ARGUMENT when an item is not a generic node.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_generic_node_compare_document_order(
    sxml_item_pair pair, sxml_ordering *ordering);

/* -------------------------------------------------------- Function items */

/*
 * The dynamic context of an evaluation (XPath 4.0, section 2.2.2), defined
 * by the evaluator. Function bodies receive it opaque.
 */
typedef struct sxml_dynamic_context sxml_dynamic_context;

/*
 * A function annotation (XDM 4.0, section 8.1): a name and a sequence of
 * atomic items, such as %private or %java:method("x").
 */
typedef struct sxml_function_annotation {
    sxml_name_identifier annotation_name;
    sxml_item_view annotation_values;
} sxml_function_annotation;

typedef struct sxml_function_annotation_view {
    const sxml_function_annotation *function_annotations;
    size_t function_annotation_count;
} sxml_function_annotation_view;

/*
 * What a function body receives. All borrowed for the call. The arguments
 * have already been coerced to the parameter types (XPath 4.0, section 3.4
 * "Coercion Rules") by the caller; there are exactly arity of them.
 */
typedef struct sxml_function_call {
    /* The item being called: a body reads its captured values through
       sxml_function_item_describe. */
    sxml_item function_item;
    sxml_sequence_view arguments;
    /* The caller's dynamic context; NULL for a call from outside an
       evaluation, which bodies that need one reject with
       SXML_STATUS_INVALID_ARGUMENT. */
    sxml_dynamic_context *dynamic_context;
    sxml_item_heap *item_heap;
    /* Receives the error the body raises; may be NULL. */
    sxml_error_record *error_record;
} sxml_function_call;

/*
 * The operations of a function body (D38). Native functions (the F&O
 * library, host extension functions) and functions compiled to the IR use
 * the same interface: the IR's evaluator supplies the operations of its
 * bodies, and their state is opaque here.
 *
 * sxml never recurses in C (D71): inside an evaluation it runs the bodies
 * it knows as steps of its own machine. A host body, like a host sequence
 * generator, resolver or tree builder, is called directly; if it starts
 * another evaluation itself, that evaluation nests on the C stack, and
 * bounding such nesting is the host's (D98).
 */
typedef struct sxml_function_body_operations {
    /* Runs the function and writes its result, owned, to *result. */
    sxml_status (*call)(void *state, const sxml_function_call *call,
                        sxml_sequence *result);
    /* Called when a function item starts and stops using state. Both may
       be NULL when state outlives every function item that uses it, as
       static data or a compiled module the host keeps does. */
    void (*retain)(void *state);
    void (*release)(void *state);
} sxml_function_body_operations;

typedef struct sxml_function_body {
    void *state;
    const sxml_function_body_operations *operations;
} sxml_function_body;

/* A function item to make. */
typedef struct sxml_function_item_request {
    /* The function's name; 0 for an anonymous function. */
    sxml_name_identifier function_name;
    /* A function type registered in the heap's registry: the parameter and
       result types. The arity is its parameter count. */
    sxml_type_identifier signature;
    /* Borrowed: must stay valid as long as body's state. */
    sxml_function_annotation_view function_annotations;
    /* The closure: values of nonlocal variables the body reads, in an
       order the body defines. Retained by the function item. */
    sxml_sequence_view captured_values;
    sxml_function_body body;
} sxml_function_item_request;

/*
 * Makes a function item and writes it, owned, to *item. Retains the body
 * through its retain operation and the captured values.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT when signature is not a function
 * type or body has no call operation, and the heap's failures.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_function_item_make(
    sxml_item_heap *item_heap, const sxml_function_item_request *request,
    sxml_item *item);

/*
 * The properties of a function item (XDM 4.0, section 8.1), borrowed from
 * it. For a map or array: no name, its type annotation as signature, arity
 * 1, no annotations, no captured values and a body that performs map:get
 * or array:get.
 */
typedef struct sxml_function_item_description {
    sxml_name_identifier function_name;
    sxml_type_identifier signature;
    size_t arity;
    sxml_function_annotation_view function_annotations;
    sxml_sequence_view captured_values;
    sxml_function_body body;
} sxml_function_item_description;

/*
 * Writes the properties of a function item to *description. Returns
 * SXML_STATUS_INVALID_ARGUMENT when item is not a function item.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_function_item_describe(
    sxml_item item, sxml_function_item_description *description);

/*
 * Calls call->function_item with call->arguments (dynamic function call,
 * XPath 4.0, section 4.6.3) and writes the result, owned, to *result. Maps
 * and arrays are called as map:get and array:get.
 *
 * The caller coerces the arguments to the parameter types of the signature
 * first (XPath 4.0, section 3.4): coercion needs atomization, casting and
 * function coercion, which belong to the evaluator, not the substrate. The
 * call checks only the number of arguments; the result is not checked
 * against the result type either.
 *
 * Returns SXML_STATUS_TYPE_ERROR (XPTY0004) for a wrong number of
 * arguments, SXML_STATUS_DYNAMIC_ERROR (FOAY0001) for an array index out of
 * bounds, and the body's failures.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_function_item_call(
    const sxml_function_call *call, sxml_sequence *result);

/* ------------------------------------------------------- Maps and records */

/*
 * Maps keep their entries in entry order (XDM 4.0, section 8.2; D7, D37).
 * A map made from a list of entries is one block of entries plus a hash
 * index built on the first lookup; put and remove make a new map that
 * shares the old one's structure, so neither copies the whole map.
 * Entries are addressable by position, which JNode navigation needs.
 */

/* One entry: an atomic key and its value. */
typedef struct sxml_map_entry {
    sxml_item key;
    sxml_sequence value;
} sxml_map_entry;

typedef struct sxml_map_entry_view {
    const sxml_map_entry *map_entries;
    size_t map_entry_count;
} sxml_map_entry_view;

/* What making a map does with keys that are atomic-equal. */
typedef enum sxml_duplicate_key_policy {
    /* Fail with XQDY0137 (map constructors, XPath 4.0, section 4.15.1.1). */
    SXML_DUPLICATE_KEY_POLICY_REJECT,
    /* Keep the first entry with the key. */
    SXML_DUPLICATE_KEY_POLICY_USE_FIRST,
    /* Keep the value of the last entry with the key, at the position of the
       first. */
    SXML_DUPLICATE_KEY_POLICY_USE_LAST,
} sxml_duplicate_key_policy;

typedef struct sxml_map_request {
    /* In entry order. Keys must be atomic items. */
    sxml_map_entry_view map_entries;
    sxml_duplicate_key_policy duplicate_key_policy;
    /* Receives XQDY0137; may be NULL. */
    sxml_error_record *error_record;
} sxml_map_request;

/*
 * Makes a map and writes it, owned, to *map. Keys and values are retained.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT when a key is not atomic,
 * SXML_STATUS_DYNAMIC_ERROR (XQDY0137) for a duplicate under
 * SXML_DUPLICATE_KEY_POLICY_REJECT, and the heap's failures.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_map_make(
    sxml_item_heap *item_heap, const sxml_map_request *request,
    sxml_item *map);

/* Writes the number of entries of a map to *map_entry_count (map:size). */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_map_get_size(
    sxml_item map, size_t *map_entry_count);

/*
 * Writes the entry at entry_index (from 0, in entry order), borrowed from
 * map, to *entry. O(1) for a map that put and remove have not changed,
 * O(log n) otherwise. Returns SXML_STATUS_INDEX_OUT_OF_RANGE past the end
 * and SXML_STATUS_INVALID_ARGUMENT when map is not a map. A record makes
 * its key items when they are first needed (D61), so the first call on a
 * record may also return the heap's failures
 * (SXML_STATUS_OUT_OF_MEMORY, SXML_STATUS_LIMIT_EXCEEDED); *entry is then
 * untouched and a later call may succeed.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_map_get_entry(
    sxml_item map, size_t entry_index, sxml_map_entry *entry);

/* The outcome of looking a key up. */
typedef struct sxml_map_lookup {
    bool is_found;
    size_t entry_index;
    /* Borrowed from the map. */
    sxml_map_entry entry;
} sxml_map_lookup;

/*
 * Looks key up (map:get, map:contains; keys compared by atomic-equal) and
 * writes the outcome to *lookup. O(1) average. May build the map's index on
 * the first lookup, hence the status: returns the heap's failures, and
 * SXML_STATUS_INVALID_ARGUMENT when map is not a map.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_map_find(sxml_item map,
                                                     sxml_item key,
                                                     sxml_map_lookup *lookup);

/* A change to a map. */
typedef struct sxml_map_update {
    sxml_item map;
    sxml_item key;
    /* Ignored by sxml_map_remove. */
    sxml_sequence value;
} sxml_map_update;

/*
 * Makes the map with key set to value (dm:map-put, XDM 4.0, section
 * 8.2.2): an existing entry keeps its position, a new one goes last. The
 * result is a plain map, even when update->map is a record. Writes it,
 * owned, to *map. Returns the heap's failures and
 * SXML_STATUS_INVALID_ARGUMENT for a non-atomic key or a non-map.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_map_put(
    sxml_item_heap *item_heap, const sxml_map_update *update, sxml_item *map);

/*
 * Makes the map without key (map:remove), keeping the order of the other
 * entries, and writes it, owned, to *map. Failures as for sxml_map_put.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_map_remove(
    sxml_item_heap *item_heap, const sxml_map_update *update, sxml_item *map);

/* A record to make: one value per field of record_type, in field order. */
typedef struct sxml_record_request {
    sxml_type_identifier record_type;
    sxml_sequence_view field_values;
} sxml_record_request;

/*
 * Makes a record (dm:make-record, XDM 4.0, section 8.3.1): a map whose keys
 * are the field names of request->record_type, as xs:string, in field
 * order, and whose type annotation is that record type. The values are
 * retained; they must already match the field types (coercion is the
 * caller's). Records of one type share their keys and key index through
 * the type registry.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT when record_type is not a record
 * type with that many fields, and the heap's failures.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_record_make(
    sxml_item_heap *item_heap, const sxml_record_request *request,
    sxml_item *record);

/*
 * Writes the value of the field at field_index, borrowed from record, to
 * *value (dm:record-get-value). O(1). Returns SXML_STATUS_INVALID_ARGUMENT
 * when record is not a record and SXML_STATUS_INDEX_OUT_OF_RANGE past its
 * last field.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_record_get_field_value(
    sxml_item record, size_t field_index, sxml_sequence *value);

/* ----------------------------------------------------------------- Arrays */

/*
 * Arrays (XDM 4.0, section 8.4): members are sequences. Arrays are
 * persistent: append, put, insert, remove and slice share the structure of
 * the array they start from, at O(log n) cost (D37).
 */

/*
 * Makes the array of members, in order, and writes it, owned, to *array.
 * The members are retained. Returns the heap's failures.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_array_make(
    sxml_item_heap *item_heap, sxml_sequence_view members, sxml_item *array);

/* Writes the number of members to *member_count (array:size). Returns
   SXML_STATUS_INVALID_ARGUMENT when array is not an array. */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_array_get_size(
    sxml_item array, size_t *member_count);

/*
 * Writes the member at member_index (from 0), borrowed from array, to
 * *member. O(log n), O(1) for an array made by sxml_array_make. Returns
 * SXML_STATUS_INDEX_OUT_OF_RANGE past the end; raising FOAY0001 is the
 * caller's.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_array_get_member(
    sxml_item array, size_t member_index, sxml_sequence *member);

/* A change to an array. */
typedef struct sxml_array_update {
    sxml_item array;
    /* The member to replace, remove or insert before; ignored by
       sxml_array_append. For sxml_array_insert, the size of the array
       appends. */
    size_t member_index;
    /* Ignored by sxml_array_remove. */
    sxml_sequence member;
} sxml_array_update;

/*
 * Each makes a new array from update->array and writes it, owned, to
 * *array: append adds member last (dm:array-append), put replaces the
 * member at member_index (array:put), insert inserts before it
 * (array:insert-before), remove removes it (array:remove). Return
 * SXML_STATUS_INDEX_OUT_OF_RANGE for a member_index out of range,
 * SXML_STATUS_INVALID_ARGUMENT for a non-array, and the heap's failures.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_array_append(
    sxml_item_heap *item_heap, const sxml_array_update *update,
    sxml_item *array);
SXML_NODISCARD SXML_EXPORT sxml_status sxml_array_put(
    sxml_item_heap *item_heap, const sxml_array_update *update,
    sxml_item *array);
SXML_NODISCARD SXML_EXPORT sxml_status sxml_array_insert(
    sxml_item_heap *item_heap, const sxml_array_update *update,
    sxml_item *array);
SXML_NODISCARD SXML_EXPORT sxml_status sxml_array_remove(
    sxml_item_heap *item_heap, const sxml_array_update *update,
    sxml_item *array);

/* A contiguous part of an array. */
typedef struct sxml_array_slice_request {
    sxml_item array;
    size_t first_member_index;
    size_t member_count;
} sxml_array_slice_request;

/*
 * Makes the array of member_count members from first_member_index
 * (array:subarray) and writes it, owned, to *array. Returns
 * SXML_STATUS_INDEX_OUT_OF_RANGE when the part does not lie within the
 * array, and the heap's failures.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_array_slice(
    sxml_item_heap *item_heap, const sxml_array_slice_request *request,
    sxml_item *array);

/* ---------------------------------------------------------------- JNodes */

/*
 * A JNode (XDM 4.0, section 8.5) is a cell holding its parent JNode, its
 * key, its value, its position among its parent's children and the
 * identifier of its JTree. A root JNode gets a new tree identifier, so it
 * has its own identity and its JTree a place in document order among all
 * trees of the heap. Children are made on demand when navigation reaches
 * them; maps and arrays hold no parent pointers, so one map can sit in
 * many JTrees (the zipper approach of XDM 4.0, section 8.5, note).
 */

/*
 * Makes a root JNode over value (fn:jtree, F&O 4.0, section 16.1.1) and
 * writes it, owned, to *json_node. value is retained. Returns the heap's
 * failures and, because the root counts the items of value, the failures
 * of a lazy value's generator (SXML_STATUS_DYNAMIC_ERROR or
 * SXML_STATUS_TYPE_ERROR without an error record to fill: read value first
 * to see its error; D73).
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_json_node_make_root(
    sxml_item_heap *item_heap, sxml_sequence value, sxml_item *json_node);

/* The properties of a JNode, borrowed from it. */
typedef struct sxml_json_node_description {
    /* ·jparent·; absent for a root. */
    bool has_parent;
    sxml_item parent;
    /* ·jkey·: the map key, or the 1-based position as xs:integer under an
       array or sequence JNode. For a root, which has no key, a
       zero-initialized sxml_item (not an item). */
    sxml_item json_key;
    /* ·jvalue·. */
    sxml_sequence json_value;
    /* The position among the parent's children, from 0. */
    uint64_t child_index;
    /* Number of children (dm:j-children): members of an array, entries of
       a map, items of a sequence of two or more; 0 for a leaf. */
    uint64_t child_count;
    /* The identifier of the JTree, shared by every JNode in it. */
    sxml_tree_identifier json_tree;
} sxml_json_node_description;

/*
 * Writes the properties of a JNode to *description. Returns
 * SXML_STATUS_INVALID_ARGUMENT when json_node is not a JNode.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_json_node_describe(
    sxml_item json_node, sxml_json_node_description *description);

/* A child of a JNode to make. */
typedef struct sxml_json_child_request {
    sxml_item json_node;
    uint64_t child_index;
} sxml_json_child_request;

/*
 * Makes the JNode for child child_index of request.json_node (dm:j-children,
 * XDM 4.0, section 8.5) and writes it, owned, to *child. Making the same
 * child twice gives two cells that are the same node
 * (sxml_generic_node_is_same_node).
 *
 * Returns SXML_STATUS_INDEX_OUT_OF_RANGE past the last child,
 * SXML_STATUS_INVALID_ARGUMENT when json_node is not a JNode, the heap's
 * failures and, because the child counts the items of its value, the
 * failures of a lazy value's generator, as sxml_json_node_make_root (D73).
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_json_node_make_child(
    sxml_item_heap *item_heap, sxml_json_child_request request,
    sxml_item *child);
