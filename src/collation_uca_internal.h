#pragma once

/*
 * The insides of the UCA collation feature, shared by its files
 * (src/collation_uca.c: the collation element table, the elements of
 * strings, the feature; src/collation_tailoring.c: tailorings from CLDR
 * rules; src/collation_tables.c: the reorder groups, generated). Internal
 * to the feature: nothing outside those files includes this header; the
 * core reaches the feature through sxml_uca_collation_operations
 * (src/collation_internal.h).
 *
 * A collation element table (glossary `collation_element_table`) is
 * either a root table, read from allkeys.txt (CLDR's allkeys_CLDR.txt or
 * the DUCET; UTS #10, section 9.1), or a tailoring (glossary `tailoring`;
 * UTS #35, Part 5): entries of its own over a parent table, which are
 * looked up first, the settings its rules give, and a reordering. A table
 * and its parents form a chain that ends at the root; lookups walk it in
 * a loop (D71).
 */
#include "collation_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------ Elements */

/* The flags of a table element. */
enum {
    /* Marked "*" in allkeys.txt (a variable element of the DUCET). */
    SXML_UCA_ELEMENT_FLAG_VARIABLE_MARKED = 0x01U,
    /* An upper-case element (UTS #35, part 5, section 3.14): from the
       tertiary weight in a root table, from the characters of the
       tailored string in a tailoring. */
    SXML_UCA_ELEMENT_FLAG_UPPER = 0x02U,
    /* The second element of implicit weights (UTS #10, section 10.1,
       [.BBBB.0000.0000]): its primary is no primary of the table and is
       never reordered. */
    SXML_UCA_ELEMENT_FLAG_CONTINUATION = 0x04U,
};

/*
 * One collation element of a table. The weights of a root table are
 * those of allkeys.txt; a tailoring puts its elements between them with
 * the offsets (glossary `tailoring`): the weight of a level is the root
 * weight shifted left by 8 bits plus the offset, so up to 255 tailored
 * weights fit after each root weight and before the next.
 */
typedef struct sxml_uca_element {
    uint16_t primary;
    uint16_t secondary;
    uint8_t tertiary;
    uint8_t primary_offset;
    uint8_t secondary_offset;
    uint8_t tertiary_offset;
    uint8_t flags;
} sxml_uca_element;

/* The elements of one entry, borrowed from the table that has them. */
typedef struct sxml_uca_entry {
    const sxml_uca_element *elements;
    uint32_t element_count;
} sxml_uca_entry;

/* An entry of one codepoint: elements at element_offset of its table. */
typedef struct sxml_uca_mapping {
    uint32_t codepoint;
    uint32_t element_offset;
    uint32_t element_count;
} sxml_uca_mapping;

/* The longest contraction sxml reads; the DUCET's are at most 3. */
#define SXML_UCA_MAXIMUM_CONTRACTION_LENGTH 8U

/* An entry of several codepoints. */
typedef struct sxml_uca_contraction {
    uint32_t codepoints[SXML_UCA_MAXIMUM_CONTRACTION_LENGTH];
    uint32_t codepoint_count;
    uint32_t element_offset;
    uint32_t element_count;
} sxml_uca_contraction;

/* At most this many @implicitweights lines; the DUCET has 4. */
#define SXML_UCA_MAXIMUM_IMPLICIT_RANGE_COUNT 16U
/* The @version text kept. */
#define SXML_UCA_MAXIMUM_VERSION_BYTE_COUNT 32U

/* One @implicitweights line: codepoints first to last weigh base, with
   their offset from origin, the first codepoint of base's ranges. */
typedef struct sxml_uca_implicit_range {
    uint32_t first;
    uint32_t last;
    uint32_t base;
    uint32_t origin;
} sxml_uca_implicit_range;

/* ------------------------------------------------------- Reorder groups */

/* What a reorder group is (glossary `reorder_group`). */
typedef enum sxml_collation_reorder_group_kind {
    /* space, punct, symbol, currency or digit (UTS #35, part 5, section
       3.13): kept at the start when a reordering does not name it. */
    SXML_COLLATION_REORDER_GROUP_KIND_SPECIAL,
    /* One or more scripts, by ISO 15924 code. */
    SXML_COLLATION_REORDER_GROUP_KIND_SCRIPT,
    /* The unassigned codepoints (implicit weights from FBC0, UTS #10,
       section 10.1.2): named by no code, moved with "others". */
    SXML_COLLATION_REORDER_GROUP_KIND_UNASSIGNED,
} sxml_collation_reorder_group_kind;

/* A reorder group as src/collation_tables.c lists it. */
typedef struct sxml_collation_reorder_group_definition {
    /* The reorder codes, separated by spaces ("Hira Hrkt Kana"). */
    const char *codes;
    /* The first codepoint of the group in the root order; 0 for the
       unassigned group. */
    uint32_t first_codepoint;
    sxml_collation_reorder_group_kind group_kind;
} sxml_collation_reorder_group_definition;

/* src/collation_tables.c: the groups in the root order. */
extern const sxml_collation_reorder_group_definition sxml_collation_reorder_groups[];
extern const size_t sxml_collation_reorder_group_count;

/* One range of primaries a reordering moves: the primaries from first
   to end (exclusive) weigh shift more. */
typedef struct sxml_uca_reorder_range {
    uint32_t first;
    uint32_t end;
    int32_t shift;
} sxml_uca_reorder_range;

/* --------------------------------------------------------------- Tables */

struct sxml_collation_element_table {
    sxml_allocator allocator;
    /* The table this one tailors; NULL for a root table. */
    const sxml_collation_element_table *parent;
    /* The root table of the chain (this one for a root table). */
    const sxml_collation_element_table *root;
    sxml_uca_element *elements;
    size_t element_count;
    sxml_uca_mapping *mappings;
    size_t mapping_count;
    sxml_uca_contraction *contractions;
    size_t contraction_count;
    /* The settings the rules of a tailoring give ([strength 2] and the
       like; UTS #35, part 5, section 3.4); settings.explicit_parameters
       says which. A parameter of the collation URI wins over them. */
    sxml_collation settings;
    /* The reordering of a tailoring, or none (count 0, then a parent's
       applies). */
    sxml_uca_reorder_range *reorder_ranges;
    size_t reorder_range_count;
    bool has_reordering;
    /* Root tables only. */
    sxml_uca_implicit_range implicit_ranges[SXML_UCA_MAXIMUM_IMPLICIT_RANGE_COUNT];
    size_t implicit_range_count;
    /* The primary of digit zero (U+0030): numeric weights go there, and
       variable primaries are below it. */
    uint32_t digit_zero_primary;
    /* The group of each primary below digit_zero_primary. */
    uint8_t *variable_groups;
    /* The primary where each reorder group starts, and after the last
       the end of the reorderable primaries; NULL when the table's
       groups are not in the order of the root collation. */
    uint32_t *group_starts;
    uint8_t version[SXML_UCA_MAXIMUM_VERSION_BYTE_COUNT];
    size_t version_byte_count;
};

/* Releases a table made by the feature (a root table or a tailoring),
   not its parents. Does nothing for NULL. */
void sxml_uca_release_table(sxml_collation_element_table *table);

/* The entry of codepoint in the chain of table; false when none has
   one. */
bool sxml_uca_find_mapping(const sxml_collation_element_table *table, uint32_t codepoint,
                           sxml_uca_entry *entry);

/* A codepoint sequence, borrowed. */
typedef struct sxml_uca_codepoints {
    const uint32_t *codepoints;
    size_t codepoint_count;
} sxml_uca_codepoints;

/* The contraction of exactly sequence in the chain of table; false when
   none has it. */
bool sxml_uca_find_contraction(const sxml_collation_element_table *table,
                               sxml_uca_codepoints sequence, sxml_uca_entry *entry);

/* Whether a contraction of the chain of table starts with codepoint. */
bool sxml_uca_starts_contraction(const sxml_collation_element_table *table, uint32_t codepoint);

/* The two implicit elements of a codepoint without an entry (UTS #10,
   section 10.1), from the root table's ranges. */
void sxml_uca_make_implicit_elements(const sxml_collation_element_table *root,
                                     uint32_t codepoint, sxml_uca_element elements[2]);

/* The array of allocator memory sxml_uca_allocate_array makes. */
typedef struct sxml_uca_array_request {
    size_t element_count;
    size_t element_size;
    size_t alignment;
} sxml_uca_array_request;

/* Allocates an array; *memory is NULL for zero elements. Returns the
   allocator's failures and SXML_STATUS_ARITHMETIC_OVERFLOW. */
SXML_NODISCARD sxml_status sxml_uca_allocate_array(sxml_allocator allocator,
                                                   sxml_uca_array_request request,
                                                   void **memory);

/* Sorts the mappings and contractions of a table by codepoints. */
void sxml_uca_sort_entries(sxml_collation_element_table *table);

/*
 * Writes the reorder ranges of codes to table (UTS #35, part 5, section
 * 3.13): codes is a list of reorder codes separated by commas, spaces or
 * hyphens ("Grek,digit"; "others" or "Zzzz" for every group not named;
 * "default" alone for none), compared without case. Writes
 * *is_honoured false, leaving table unchanged, for an unknown or
 * repeated code or a root table without groups. Returns the allocator's
 * failures.
 */
typedef struct sxml_uca_reordering_request {
    sxml_collation_element_table *table;
    sxml_byte_view codes;
} sxml_uca_reordering_request;

SXML_NODISCARD sxml_status sxml_uca_set_reordering(const sxml_uca_reordering_request *request,
                                                   bool *is_honoured);

/* ---------------------------------------------------------- Tailorings */

/* At most this many tailorings a processor keeps (also the answers "no
   tailoring"); past them, a new language or reordering is not honoured,
   so queries cannot grow the state without bound. */
#define SXML_UCA_MAXIMUM_TAILORING_COUNT 64U
/* The longest key of a kept tailoring: the language tag and the reorder
   codes. */
#define SXML_UCA_MAXIMUM_TAILORING_KEY_BYTE_COUNT 96U

/* A tailoring the feature keeps. */
typedef struct sxml_uca_kept_tailoring {
    uint8_t key[SXML_UCA_MAXIMUM_TAILORING_KEY_BYTE_COUNT];
    size_t key_byte_count;
    /* The table to use; NULL for the root table. */
    const sxml_collation_element_table *table;
    /* The table the feature made for this key, released with the state;
       NULL when it made none (the root table, or the language's table
       for a reordering that is not honoured). */
    sxml_collation_element_table *made_table;
    bool is_honoured;
} sxml_uca_kept_tailoring;

/* The feature's state in a processor. */
typedef struct sxml_uca_feature_state {
    /* The root table, or NULL when the resolver had no data. */
    sxml_collation_element_table *table;
    sxml_uca_kept_tailoring tailorings[SXML_UCA_MAXIMUM_TAILORING_COUNT];
    size_t tailoring_count;
} sxml_uca_feature_state;

/*
 * src/collation_tailoring.c: makes the tailoring of request->language
 * and request->reordering over state->table (see
 * sxml_uca_collation_operations.find_tailoring) and keeps it in state.
 */
typedef struct sxml_uca_tailoring_build {
    sxml_processor *processor;
    sxml_uca_feature_state *state;
    const sxml_uca_tailoring_request *request;
} sxml_uca_tailoring_build;

SXML_NODISCARD sxml_status sxml_uca_build_tailoring(const sxml_uca_tailoring_build *build,
                                                    sxml_uca_tailoring *tailoring);
