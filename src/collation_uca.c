/*
 * The UCA collation feature (src/collation_internal.h, "The UCA collation
 * feature"; include/sxml/processor.h, sxml_uca_collation_feature; packet
 * 7.15a, D213): a collation element table read from the DUCET the host
 * serves as feature data (glossary `collation_element_table`,
 * `feature_data`), and the collation elements of strings over it (UTS #10,
 * Unicode Collation Algorithm, version 16.0.0; UTS #35, part 5, section 3,
 * for the parameters).
 *
 * Golden file: src/optional_feature.c (the test feature: reading feature
 * data once into the processor's slot), src/collation.c (element arrays
 * through an allocator).
 *
 * The table. allkeys.txt (UTS #10, section 9.1) is read in two passes,
 * one that counts and one that fills, into three sorted arrays: the
 * collation elements of every entry, the entries of one codepoint, and the
 * contractions (entries of several codepoints). Each element keeps its
 * 16-bit primary and secondary weights and its tertiary weight; the
 * variable mark "*" decides only the groups of maxVariable (below). Han
 * and unassigned codepoints are not in the file: their implicit weights
 * are computed (UTS #10, section 10.1), with the @implicitweights ranges
 * the file gives.
 *
 * The elements of a string (UTS #10, section 4, steps S1 to S3):
 * 1. The string is put into NFD (S1.1): each codepoint's canonical
 *    decomposition, then the canonical ordering of combining marks; each
 *    character keeps the bytes of the codepoint it came from.
 * 2. At each character, the longest contraction or the single entry that
 *    matches (S2.1), extended by unblocked non-starters that follow it
 *    (S2.1.1 to S2.1.3, discontiguous contractions); else implicit weights
 *    (S2.2). With numeric=yes a run of decimal digits weighs as its value
 *    instead (UTS #35, part 5, section 3.7).
 * 3. Each table element becomes a sxml_collation_element with the
 *    parameters applied: variable weighting (UTS #10, section 4, shifted
 *    and blanked; which primaries are variable follows maxVariable), the
 *    case level and caseFirst from the tertiary weight (UTS #35, part 5,
 *    section 3.14). Primary weights are shifted left by 8 bits, so numeric
 *    weights fit between the primary of digit zero and the next one.
 *
 * No recursion (D71); the table lives in the processor's feature slot and
 * is read only by the processor's thread (D35).
 */
#include "collation_internal.h"

#include "optional_feature_internal.h"
#include "processor_internal.h"
#include "regex_internal.h"
#include "sort_internal.h"
#include "unicode_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------- The table */

/* The longest contraction sxml reads; the DUCET's are at most 3. */
#define SXML_UCA_MAXIMUM_CONTRACTION_LENGTH 8U
/* At most this many elements in one entry; the DUCET's are at most 18. */
#define SXML_UCA_MAXIMUM_ENTRY_ELEMENT_COUNT 32U
/* At most this many @implicitweights lines; the DUCET has 4. */
#define SXML_UCA_MAXIMUM_IMPLICIT_RANGE_COUNT 16U
/* The @version text kept. */
#define SXML_UCA_MAXIMUM_VERSION_BYTE_COUNT 32U

/* The DUCET's common secondary and tertiary weights (UTS #10, section
   10.1, the implicit weights [.AAAA.0020.0002][.BBBB.0000.0000]). */
#define SXML_UCA_COMMON_SECONDARY 0x0020U
#define SXML_UCA_COMMON_TERTIARY 0x0002U

/* The groups of maxVariable a primary weight below digit zero's is in,
   in the order of sxml_collation_variable_group, and a weight that is
   never variable. */
#define SXML_UCA_GROUP_NEVER_VARIABLE 4U
#define SXML_UCA_GROUP_UNKNOWN 0xFFU

/* One collation element of the table. */
typedef struct uca_table_element {
    uint16_t primary;
    uint16_t secondary;
    uint8_t tertiary;
    /* Marked "*" in the file. */
    bool is_variable_marked;
} uca_table_element;

/* An entry of one codepoint. */
typedef struct uca_mapping {
    uint32_t codepoint;
    uint32_t element_offset;
    uint32_t element_count;
} uca_mapping;

/* An entry of several codepoints. */
typedef struct uca_contraction {
    uint32_t codepoints[SXML_UCA_MAXIMUM_CONTRACTION_LENGTH];
    uint32_t codepoint_count;
    uint32_t element_offset;
    uint32_t element_count;
} uca_contraction;

/* One @implicitweights line: codepoints first to last weigh base, with
   their offset from origin, the first codepoint of base's ranges. */
typedef struct uca_implicit_range {
    uint32_t first;
    uint32_t last;
    uint32_t base;
    uint32_t origin;
} uca_implicit_range;

struct sxml_collation_element_table {
    sxml_allocator allocator;
    uca_table_element *elements;
    size_t element_count;
    uca_mapping *mappings;
    size_t mapping_count;
    uca_contraction *contractions;
    size_t contraction_count;
    uca_implicit_range implicit_ranges[SXML_UCA_MAXIMUM_IMPLICIT_RANGE_COUNT];
    size_t implicit_range_count;
    /* The primary of digit zero (U+0030): numeric weights go there, and
       variable primaries are below it. */
    uint32_t digit_zero_primary;
    /* The group of each primary below digit_zero_primary. */
    uint8_t *variable_groups;
    uint8_t version[SXML_UCA_MAXIMUM_VERSION_BYTE_COUNT];
    size_t version_byte_count;
};

/* The feature's state in a processor: the table, or NULL when the
   resolver had no data. */
typedef struct uca_feature_state {
    sxml_collation_element_table *table;
} uca_feature_state;

static void release_table(sxml_collation_element_table *table) {
    if (table == NULL) {
        return;
    }
    const sxml_allocator allocator = table->allocator;
    sxml_allocator_release(allocator, table->elements);
    sxml_allocator_release(allocator, table->mappings);
    sxml_allocator_release(allocator, table->contractions);
    sxml_allocator_release(allocator, table->variable_groups);
    sxml_allocator_release(allocator, table);
}

/* ------------------------------------------------------ Reading the file */

/* A cursor over the text of allkeys.txt. */
typedef struct text_cursor {
    const uint8_t *bytes;
    size_t byte_count;
    size_t byte_offset;
} text_cursor;

static bool cursor_is_at_end(const text_cursor *cursor) {
    return cursor->byte_offset >= cursor->byte_count;
}

static uint8_t cursor_peek(const text_cursor *cursor) {
    return cursor_is_at_end(cursor) ? 0 : cursor->bytes[cursor->byte_offset];
}

/* Skips spaces and tabs (not line ends). */
static void skip_blanks(text_cursor *cursor) {
    while (cursor_peek(cursor) == ' ' || cursor_peek(cursor) == '\t') {
        cursor->byte_offset += 1;
    }
}

/* Skips to the start of the next line. */
static void skip_line(text_cursor *cursor) {
    while (!cursor_is_at_end(cursor) && cursor_peek(cursor) != '\n') {
        cursor->byte_offset += 1;
    }
    if (!cursor_is_at_end(cursor)) {
        cursor->byte_offset += 1;
    }
}

static bool is_line_end(const text_cursor *cursor) {
    const uint8_t byte = cursor_peek(cursor);
    return cursor_is_at_end(cursor) || byte == '\n' || byte == '\r' || byte == '#';
}

static int hex_digit_value(uint8_t byte) {
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'A' && byte <= 'F') {
        return byte - 'A' + 10;
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    return -1;
}

/* A hexadecimal number of at most maximum_digit_count digits. */
typedef struct hex_request {
    size_t maximum_digit_count;
    uint32_t maximum_value;
} hex_request;

static bool read_hex(text_cursor *cursor, hex_request request, uint32_t *value) {
    uint32_t number = 0;
    size_t digit_count = 0;
    while (hex_digit_value(cursor_peek(cursor)) >= 0) {
        if (digit_count == request.maximum_digit_count) {
            return false;
        }
        number = number * 16U + (uint32_t)hex_digit_value(cursor_peek(cursor));
        digit_count += 1;
        cursor->byte_offset += 1;
    }
    if (digit_count == 0 || number > request.maximum_value) {
        return false;
    }
    *value = number;
    return true;
}

static const hex_request codepoint_hex = {.maximum_digit_count = 6, .maximum_value = 0x10FFFFU};
static const hex_request weight_hex = {.maximum_digit_count = 4, .maximum_value = 0xFFFFU};

static bool read_byte(text_cursor *cursor, uint8_t expected) {
    if (cursor_peek(cursor) != expected || cursor_is_at_end(cursor)) {
        return false;
    }
    cursor->byte_offset += 1;
    return true;
}

/* One entry line as read. */
typedef struct entry_line {
    uint32_t codepoints[SXML_UCA_MAXIMUM_CONTRACTION_LENGTH];
    size_t codepoint_count;
    uca_table_element elements[SXML_UCA_MAXIMUM_ENTRY_ELEMENT_COUNT];
    size_t element_count;
} entry_line;

/* Reads "cp cp ; [.pppp.ssss.tttt][*pppp.ssss.tttt]..." (UTS #10, section
   9.1); the cursor is at the first codepoint. */
static bool read_entry_line(text_cursor *cursor, entry_line *line) {
    line->codepoint_count = 0;
    line->element_count = 0;
    while (true) {
        skip_blanks(cursor);
        if (cursor_peek(cursor) == ';') {
            cursor->byte_offset += 1;
            break;
        }
        uint32_t codepoint = 0;
        if (line->codepoint_count == SXML_UCA_MAXIMUM_CONTRACTION_LENGTH ||
            !read_hex(cursor, codepoint_hex, &codepoint)) {
            return false;
        }
        line->codepoints[line->codepoint_count] = codepoint;
        line->codepoint_count += 1;
    }
    if (line->codepoint_count == 0) {
        return false;
    }
    while (true) {
        skip_blanks(cursor);
        if (is_line_end(cursor)) {
            break;
        }
        uca_table_element element = {0};
        uint32_t primary = 0;
        uint32_t secondary = 0;
        uint32_t tertiary = 0;
        if (line->element_count == SXML_UCA_MAXIMUM_ENTRY_ELEMENT_COUNT ||
            !read_byte(cursor, '[')) {
            return false;
        }
        element.is_variable_marked = cursor_peek(cursor) == '*';
        if (!(read_byte(cursor, '.') || read_byte(cursor, '*')) ||
            !read_hex(cursor, weight_hex, &primary) || !read_byte(cursor, '.') ||
            !read_hex(cursor, weight_hex, &secondary) || !read_byte(cursor, '.') ||
            !read_hex(cursor, weight_hex, &tertiary) || tertiary > 0xFFU) {
            return false;
        }
        /* A fourth weight, as older files had, is not used (UTS #10,
           section 9.1). */
        if (read_byte(cursor, '.')) {
            uint32_t ignored = 0;
            if (!read_hex(cursor, weight_hex, &ignored)) {
                return false;
            }
        }
        if (!read_byte(cursor, ']')) {
            return false;
        }
        element.primary = (uint16_t)primary;
        element.secondary = (uint16_t)secondary;
        element.tertiary = (uint8_t)tertiary;
        line->elements[line->element_count] = element;
        line->element_count += 1;
    }
    return true;
}

/* Whether the cursor's next bytes are text. */
static bool read_word(text_cursor *cursor, const char *text) {
    const size_t byte_count = strlen(text);
    if (cursor->byte_count - cursor->byte_offset < byte_count ||
        memcmp(cursor->bytes + cursor->byte_offset, text, byte_count) != 0) {
        return false;
    }
    cursor->byte_offset += byte_count;
    return true;
}

/* Reads "@implicitweights first..last; base" into the table. */
static bool read_implicit_weights(text_cursor *cursor, sxml_collation_element_table *table) {
    uca_implicit_range range = {0};
    skip_blanks(cursor);
    if (!read_hex(cursor, codepoint_hex, &range.first) || !read_word(cursor, "..") ||
        !read_hex(cursor, codepoint_hex, &range.last) || range.last < range.first) {
        return false;
    }
    skip_blanks(cursor);
    if (!read_byte(cursor, ';')) {
        return false;
    }
    skip_blanks(cursor);
    if (!read_hex(cursor, weight_hex, &range.base) ||
        table->implicit_range_count == SXML_UCA_MAXIMUM_IMPLICIT_RANGE_COUNT) {
        return false;
    }
    range.origin = range.first;
    for (size_t range_index = 0; range_index < table->implicit_range_count; range_index += 1) {
        uca_implicit_range *earlier = &table->implicit_ranges[range_index];
        if (earlier->base == range.base) {
            /* UTS #10, section 10.1.3: the offset is from the first range
               of the base (Tangut and its supplement share FB00). */
            const uint32_t origin = earlier->origin < range.first ? earlier->origin : range.first;
            range.origin = origin;
            earlier->origin = origin;
        }
    }
    for (size_t range_index = 0; range_index < table->implicit_range_count; range_index += 1) {
        if (table->implicit_ranges[range_index].base == range.base) {
            table->implicit_ranges[range_index].origin = range.origin;
        }
    }
    table->implicit_ranges[table->implicit_range_count] = range;
    table->implicit_range_count += 1;
    return true;
}

/* Reads "@version text". */
static bool read_version(text_cursor *cursor, sxml_collation_element_table *table) {
    skip_blanks(cursor);
    table->version_byte_count = 0;
    while (!is_line_end(cursor) && cursor_peek(cursor) != ' ' && cursor_peek(cursor) != '\t') {
        if (table->version_byte_count == SXML_UCA_MAXIMUM_VERSION_BYTE_COUNT) {
            return false;
        }
        table->version[table->version_byte_count] = cursor_peek(cursor);
        table->version_byte_count += 1;
        cursor->byte_offset += 1;
    }
    return table->version_byte_count > 0;
}

/* What one pass over the file does: count, or fill the arrays. */
typedef struct reading_pass {
    sxml_collation_element_table *table;
    bool is_filling;
} reading_pass;

/* Reads every line once: counts the entries and elements, or (when
   filling) stores them and the directives. */
static sxml_status read_lines(reading_pass pass, sxml_byte_view content) {
    sxml_collation_element_table *table = pass.table;
    text_cursor cursor = {.bytes = content.bytes, .byte_count = content.byte_count, .byte_offset = 0};
    size_t element_count = 0;
    size_t mapping_count = 0;
    size_t contraction_count = 0;
    entry_line line;
    while (!cursor_is_at_end(&cursor)) {
        skip_blanks(&cursor);
        if (is_line_end(&cursor)) {
            skip_line(&cursor);
            continue;
        }
        if (read_byte(&cursor, '@')) {
            bool is_valid = true;
            if (read_word(&cursor, "version")) {
                is_valid = !pass.is_filling || read_version(&cursor, table);
            } else if (read_word(&cursor, "implicitweights")) {
                is_valid = !pass.is_filling || read_implicit_weights(&cursor, table);
            }
            /* Other directives are not used. */
            if (!is_valid) {
                return SXML_STATUS_MALFORMED_INPUT;
            }
            skip_line(&cursor);
            continue;
        }
        if (!read_entry_line(&cursor, &line) || element_count > UINT32_MAX - line.element_count) {
            return SXML_STATUS_MALFORMED_INPUT;
        }
        if (pass.is_filling) {
            if (element_count + line.element_count > table->element_count ||
                (line.element_count > 0 && table->elements == NULL) ||
                (line.codepoint_count == 1 &&
                 (mapping_count == table->mapping_count || table->mappings == NULL)) ||
                (line.codepoint_count > 1 && (contraction_count == table->contraction_count ||
                                              table->contractions == NULL))) {
                return SXML_STATUS_MALFORMED_INPUT;
            }
            if (line.element_count > 0) {
                memcpy(&table->elements[element_count], line.elements,
                       line.element_count * sizeof(uca_table_element));
            }
            if (line.codepoint_count == 1) {
                table->mappings[mapping_count] =
                    (uca_mapping){.codepoint = line.codepoints[0],
                                  .element_offset = (uint32_t)element_count,
                                  .element_count = (uint32_t)line.element_count};
            } else if (table->contractions != NULL) {
                uca_contraction *contraction = &table->contractions[contraction_count];
                memset(contraction, 0, sizeof(*contraction));
                memcpy(contraction->codepoints, line.codepoints,
                       line.codepoint_count * sizeof(uint32_t));
                contraction->codepoint_count = (uint32_t)line.codepoint_count;
                contraction->element_offset = (uint32_t)element_count;
                contraction->element_count = (uint32_t)line.element_count;
            }
        }
        element_count += line.element_count;
        if (line.codepoint_count == 1) {
            mapping_count += 1;
        } else {
            contraction_count += 1;
        }
        skip_line(&cursor);
    }
    if (!pass.is_filling) {
        table->element_count = element_count;
        table->mapping_count = mapping_count;
        table->contraction_count = contraction_count;
    }
    return SXML_STATUS_OK;
}

/* Allocates an array of element_count elements of element_size bytes;
   NULL for none. */
typedef struct array_request {
    size_t element_count;
    size_t element_size;
    size_t alignment;
} array_request;

static sxml_status allocate_array(sxml_allocator allocator, array_request request,
                                  void **memory) {
    *memory = NULL;
    if (request.element_count == 0) {
        return SXML_STATUS_OK;
    }
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(request.element_count, request.element_size, &byte_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = byte_count, .alignment = request.alignment},
            memory);
    }
    return status;
}

static bool mapping_is_before(const void *state, sxml_sort_pair pair) {
    (void)state;
    return ((const uca_mapping *)pair.first)->codepoint <
           ((const uca_mapping *)pair.second)->codepoint;
}

/* A codepoint sequence. */
typedef struct codepoint_sequence {
    const uint32_t *codepoints;
    size_t codepoint_count;
} codepoint_sequence;

/* Orders sequences lexicographically, a proper prefix first. */
static int compare_sequences(codepoint_sequence first, codepoint_sequence second) {
    const size_t common =
        first.codepoint_count < second.codepoint_count ? first.codepoint_count : second.codepoint_count;
    for (size_t codepoint_index = 0; codepoint_index < common; codepoint_index += 1) {
        if (first.codepoints[codepoint_index] != second.codepoints[codepoint_index]) {
            return first.codepoints[codepoint_index] < second.codepoints[codepoint_index] ? -1 : 1;
        }
    }
    if (first.codepoint_count == second.codepoint_count) {
        return 0;
    }
    return first.codepoint_count < second.codepoint_count ? -1 : 1;
}

static codepoint_sequence contraction_sequence(const uca_contraction *contraction) {
    return (codepoint_sequence){.codepoints = contraction->codepoints,
                                .codepoint_count = contraction->codepoint_count};
}

static bool contraction_is_before(const void *state, sxml_sort_pair pair) {
    (void)state;
    return compare_sequences(contraction_sequence(pair.first),
                             contraction_sequence(pair.second)) < 0;
}

/* The entry of codepoint, or NULL. */
static const uca_mapping *find_mapping(const sxml_collation_element_table *table,
                                       uint32_t codepoint) {
    size_t low = 0;
    size_t high = table->mapping_count;
    while (low < high) {
        const size_t middle = low + ((high - low) / 2);
        const uint32_t found = table->mappings[middle].codepoint;
        if (found == codepoint) {
            return &table->mappings[middle];
        }
        if (found < codepoint) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return NULL;
}

/* The index of the first contraction not before sequence. */
static size_t lower_bound_contraction(const sxml_collation_element_table *table,
                                      codepoint_sequence sequence) {
    size_t low = 0;
    size_t high = table->contraction_count;
    while (low < high) {
        const size_t middle = low + ((high - low) / 2);
        if (compare_sequences(contraction_sequence(&table->contractions[middle]), sequence) < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

/* The contraction of exactly sequence, or NULL. */
static const uca_contraction *find_contraction(const sxml_collation_element_table *table,
                                               codepoint_sequence sequence) {
    const size_t found = lower_bound_contraction(table, sequence);
    if (found < table->contraction_count &&
        compare_sequences(contraction_sequence(&table->contractions[found]), sequence) == 0) {
        return &table->contractions[found];
    }
    return NULL;
}

/* Whether some contraction starts with codepoint. */
static bool starts_contraction(const sxml_collation_element_table *table, uint32_t codepoint) {
    const size_t found = lower_bound_contraction(
        table, (codepoint_sequence){.codepoints = &codepoint, .codepoint_count = 1});
    return found < table->contraction_count &&
           table->contractions[found].codepoints[0] == codepoint;
}

/* The maxVariable group of a variable character (UTS #35, part 5,
   section 3.6, by general category). */
static uint8_t group_of_category(uint32_t codepoint) {
    const uint32_t category_bit = 1U << (uint32_t)sxml_regex_get_general_category(codepoint);
    if ((category_bit & (SXML_REGEX_CATEGORY_MASK_Z |
                         (1U << (uint32_t)SXML_REGEX_GENERAL_CATEGORY_CC))) != 0) {
        return (uint8_t)SXML_COLLATION_VARIABLE_GROUP_SPACE;
    }
    if ((category_bit & SXML_REGEX_CATEGORY_MASK_P) != 0) {
        return (uint8_t)SXML_COLLATION_VARIABLE_GROUP_PUNCTUATION;
    }
    if ((category_bit & (1U << (uint32_t)SXML_REGEX_GENERAL_CATEGORY_SC)) != 0) {
        return (uint8_t)SXML_COLLATION_VARIABLE_GROUP_CURRENCY;
    }
    return (uint8_t)SXML_COLLATION_VARIABLE_GROUP_SYMBOL;
}

/*
 * The group of every primary below digit zero's. A primary takes the
 * lowest group of the characters whose first element it is: the DUCET's
 * variable ("*") elements are space, punctuation and symbols, and the
 * currency symbols follow them; a primary that no single character starts
 * with takes the group of the primary before it.
 */
static sxml_status make_variable_groups(sxml_collation_element_table *table) {
    void *memory = NULL;
    sxml_status status = allocate_array(
        table->allocator,
        (array_request){.element_count = table->digit_zero_primary,
                        .element_size = sizeof(uint8_t),
                        .alignment = alignof(uint8_t)},
        &memory);
    if (status != SXML_STATUS_OK || memory == NULL) {
        return status;
    }
    table->variable_groups = memory;
    memset(table->variable_groups, SXML_UCA_GROUP_UNKNOWN, table->digit_zero_primary);
    for (size_t mapping_index = 0; mapping_index < table->mapping_count; mapping_index += 1) {
        const uca_mapping *mapping = &table->mappings[mapping_index];
        if (mapping->element_count == 0) {
            continue;
        }
        const uca_table_element *first = &table->elements[mapping->element_offset];
        if (first->primary == 0 || first->primary >= table->digit_zero_primary) {
            continue;
        }
        uint8_t group = group_of_category(mapping->codepoint);
        if (!first->is_variable_marked && group != SXML_COLLATION_VARIABLE_GROUP_CURRENCY) {
            group = SXML_UCA_GROUP_NEVER_VARIABLE;
        }
        if (group < table->variable_groups[first->primary]) {
            table->variable_groups[first->primary] = group;
        }
    }
    uint8_t previous = (uint8_t)SXML_COLLATION_VARIABLE_GROUP_SPACE;
    for (size_t primary = 0; primary < table->digit_zero_primary; primary += 1) {
        if (table->variable_groups[primary] == SXML_UCA_GROUP_UNKNOWN) {
            table->variable_groups[primary] = previous;
        }
        previous = table->variable_groups[primary];
    }
    return SXML_STATUS_OK;
}

/* Reads content into a new table in *table (NULL on failure). */
static sxml_status read_table(sxml_allocator allocator, sxml_byte_view content,
                              sxml_collation_element_table **made) {
    *made = NULL;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = sizeof(sxml_collation_element_table),
                                  .alignment = alignof(sxml_collation_element_table)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_collation_element_table *table = memory;
    memset(table, 0, sizeof(*table));
    table->allocator = allocator;
    status = read_lines((reading_pass){.table = table, .is_filling = false}, content);
    if (status == SXML_STATUS_OK) {
        status = allocate_array(allocator,
                                (array_request){.element_count = table->element_count,
                                                .element_size = sizeof(uca_table_element),
                                                .alignment = alignof(uca_table_element)},
                                &memory);
        table->elements = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_array(allocator,
                                (array_request){.element_count = table->mapping_count,
                                                .element_size = sizeof(uca_mapping),
                                                .alignment = alignof(uca_mapping)},
                                &memory);
        table->mappings = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_array(allocator,
                                (array_request){.element_count = table->contraction_count,
                                                .element_size = sizeof(uca_contraction),
                                                .alignment = alignof(uca_contraction)},
                                &memory);
        table->contractions = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = read_lines((reading_pass){.table = table, .is_filling = true}, content);
    }
    if (status == SXML_STATUS_OK) {
        sxml_sort_in_place(&(sxml_sort_request){.elements = table->mappings,
                                                .element_count = table->mapping_count,
                                                .element_size = sizeof(uca_mapping),
                                                .is_before = mapping_is_before,
                                                .state = NULL});
        sxml_sort_in_place(&(sxml_sort_request){.elements = table->contractions,
                                                .element_count = table->contraction_count,
                                                .element_size = sizeof(uca_contraction),
                                                .is_before = contraction_is_before,
                                                .state = NULL});
        /* Digit zero's primary bounds the variable weights and places the
           numeric ones; a table without it is not a DUCET. */
        const uca_mapping *zero = find_mapping(table, '0');
        if (zero == NULL || zero->element_count == 0 || table->elements == NULL ||
            table->elements[zero->element_offset].primary == 0 || table->version_byte_count == 0) {
            status = SXML_STATUS_MALFORMED_INPUT;
        } else {
            table->digit_zero_primary = table->elements[zero->element_offset].primary;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = make_variable_groups(table);
    }
    if (status != SXML_STATUS_OK) {
        release_table(table);
        return status;
    }
    *made = table;
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------- The feature's slot */

/* The reader of the feature data: reads the table into *state. */
typedef struct table_reading {
    sxml_allocator allocator;
    sxml_collation_element_table *table;
} table_reading;

static sxml_status read_table_data(void *state, sxml_byte_view content) {
    table_reading *reading = state;
    return read_table(reading->allocator, content, &reading->table);
}

static const sxml_feature_data_reader_operations table_reader_operations = {
    .read = read_table_data,
};

static void finalize_feature_state(void *state, sxml_allocator allocator) {
    uca_feature_state *feature_state = state;
    release_table(feature_state->table);
    sxml_allocator_release(allocator, feature_state);
}

/* The DUCET of UTS #10 version 16.0.0, sxml's Unicode version. */
static const sxml_byte_view ducet_uri =
    SXML_BYTE_VIEW_INITIALIZER("http://www.unicode.org/Public/UCA/16.0.0/allkeys.txt");

static sxml_status find_table(sxml_processor *processor,
                              const sxml_collation_element_table **table) {
    *table = NULL;
    const uca_feature_state *kept = sxml_processor_get_optional_feature_state(
        processor, SXML_OPTIONAL_FEATURE_KIND_UCA_COLLATION);
    if (kept != NULL) {
        *table = kept->table;
        return SXML_STATUS_OK;
    }
    table_reading reading = {.allocator = processor->allocator, .table = NULL};
    sxml_feature_data_outcome outcome = {.is_available = false};
    sxml_status status = sxml_optional_feature_read_data(
        &(sxml_feature_data_request){
            .processor = processor,
            .resource_kind = SXML_RESOURCE_KIND_COLLATION_DATA,
            .uri = ducet_uri,
            .reader = {.state = &reading, .operations = &table_reader_operations}},
        &outcome);
    if (status != SXML_STATUS_OK) {
        release_table(reading.table);
        return status;
    }
    /* The answer is kept, also "no data": the resolver is asked once. */
    void *memory = NULL;
    status = sxml_allocator_allocate(
        processor->allocator,
        (sxml_allocation_request){.byte_count = sizeof(uca_feature_state),
                                  .alignment = alignof(uca_feature_state)},
        &memory);
    if (status != SXML_STATUS_OK) {
        release_table(reading.table);
        return status;
    }
    uca_feature_state *state = memory;
    state->table = reading.table;
    status = sxml_processor_set_optional_feature_state(
        processor, SXML_OPTIONAL_FEATURE_KIND_UCA_COLLATION, state);
    if (status != SXML_STATUS_OK) {
        finalize_feature_state(state, processor->allocator);
        return status;
    }
    *table = state->table;
    return SXML_STATUS_OK;
}

static sxml_byte_view get_version(const sxml_collation_element_table *table) {
    return (sxml_byte_view){.bytes = table->version, .byte_count = table->version_byte_count};
}

/* --------------------------------------------------------- NFD characters */

/* One character of the NFD form of the input. */
typedef struct uca_character {
    uint32_t codepoint;
    uint8_t combining_class;
    /* Taken into a discontiguous contraction already. */
    bool is_consumed;
    /* The bytes of the input codepoint it came from. */
    size_t byte_offset;
    size_t byte_end;
} uca_character;

typedef struct character_array {
    sxml_allocator allocator;
    uca_character *characters;
    size_t character_count;
    size_t character_capacity;
} character_array;

static sxml_status append_character(character_array *array, const uca_character *character) {
    if (array->character_count == array->character_capacity) {
        size_t new_capacity = array->character_capacity < 32 ? 32 : array->character_capacity;
        sxml_status status = SXML_STATUS_OK;
        if (array->character_capacity >= 32) {
            status = sxml_size_multiply(new_capacity, 2, &new_capacity);
        }
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = allocate_array(array->allocator,
                                    (array_request){.element_count = new_capacity,
                                                    .element_size = sizeof(uca_character),
                                                    .alignment = alignof(uca_character)},
                                    &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (array->character_count > 0) {
            memcpy(memory, array->characters, array->character_count * sizeof(uca_character));
        }
        sxml_allocator_release(array->allocator, array->characters);
        array->characters = memory;
        array->character_capacity = new_capacity;
    }
    array->characters[array->character_count] = *character;
    array->character_count += 1;
    return SXML_STATUS_OK;
}

/* Writes the NFD characters of input (UAX #15: full canonical
   decomposition, then the canonical ordering algorithm). */
static sxml_status make_characters(sxml_string input, character_array *array) {
    sxml_codepoint_iterator iterator = {.string = input, .byte_offset = 0};
    uint32_t codepoint = 0;
    size_t byte_offset = 0;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && sxml_codepoint_iterator_next(&iterator, &codepoint)) {
        sxml_unicode_decomposition decomposition;
        sxml_unicode_decompose(
            (sxml_unicode_decomposition_request){
                .codepoint = codepoint,
                .decomposition_kind = SXML_UNICODE_DECOMPOSITION_KIND_CANONICAL},
            &decomposition);
        for (size_t part_index = 0;
             status == SXML_STATUS_OK && part_index < decomposition.codepoint_count;
             part_index += 1) {
            const uint32_t part = decomposition.codepoints[part_index];
            status = append_character(
                array, &(uca_character){.codepoint = part,
                                        .combining_class = sxml_unicode_get_combining_class(part),
                                        .is_consumed = false,
                                        .byte_offset = byte_offset,
                                        .byte_end = iterator.byte_offset});
        }
        byte_offset = iterator.byte_offset;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Canonical ordering: a stable sort of each run of non-starters by
       combining class (The Unicode Standard 16.0, section 3.11, D109). */
    uca_character *characters = array->characters;
    for (size_t character_index = 1; character_index < array->character_count;
         character_index += 1) {
        size_t position = character_index;
        while (position > 0 && characters[position].combining_class != 0 &&
               characters[position - 1].combining_class > characters[position].combining_class) {
            const uca_character moved = characters[position];
            characters[position] = characters[position - 1];
            characters[position - 1] = moved;
            position -= 1;
        }
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------ Weighing elements */

/* What turns table elements into sxml_collation_elements for one
   string. */
typedef struct element_writer {
    const sxml_collation *collation;
    const sxml_collation_element_table *table;
    sxml_collation_element_array *elements;
    /* The last element with a primary weight was variable (UTS #10,
       section 4: an ignorable after it is ignored too). */
    bool is_after_variable;
    /* The bytes of the characters being weighed. */
    size_t byte_offset;
    size_t byte_end;
} element_writer;

/* The tertiary weights of upper-case variants (UTS #10, section 10.3.1,
   the tertiary weight table; UTS #35, part 5, section 3.14). */
static bool is_upper_tertiary(uint32_t tertiary) {
    switch (tertiary) {
    case 0x08U:
    case 0x09U:
    case 0x0AU:
    case 0x0BU:
    case 0x0CU:
    case 0x0EU:
    case 0x11U:
    case 0x12U:
    case 0x1DU:
        return true;
    default:
        return false;
    }
}

static bool is_variable_primary(const element_writer *writer, uint32_t primary) {
    const sxml_collation_element_table *table = writer->table;
    return primary != 0 && primary < table->digit_zero_primary &&
           table->variable_groups[primary] <= (uint8_t)writer->collation->maximum_variable;
}

/* Appends one element of the table (or computed) with the parameters
   applied. */
static sxml_status write_element(element_writer *writer, uca_table_element element) {
    const sxml_collation *collation = writer->collation;
    const uint32_t primary = element.primary;
    uint32_t weights[SXML_COLLATION_ELEMENT_LEVEL_COUNT] = {0};
    const bool is_variable = collation->alternate != SXML_COLLATION_ALTERNATE_NON_IGNORABLE &&
                             is_variable_primary(writer, primary);
    const bool is_ignorable =
        primary == 0 && element.secondary == 0 && element.tertiary == 0;
    if (is_variable) {
        /* Shifted: [.0000.0000.0000.pppp]; blanked: ignorable. */
        if (collation->alternate == SXML_COLLATION_ALTERNATE_SHIFTED) {
            weights[SXML_COLLATION_LEVEL_QUATERNARY] = primary << 8U;
        }
        writer->is_after_variable = true;
    } else if (is_ignorable ||
               (primary == 0 && writer->is_after_variable &&
                collation->alternate != SXML_COLLATION_ALTERNATE_NON_IGNORABLE)) {
        /* Completely ignorable, or ignorable after a variable element. */
    } else {
        if (primary != 0) {
            writer->is_after_variable = false;
        }
        const bool is_upper = is_upper_tertiary(element.tertiary);
        const bool is_upper_first = collation->case_first == SXML_COLLATION_CASE_FIRST_UPPER;
        const uint32_t case_rank = is_upper == is_upper_first ? 1U : 2U;
        weights[SXML_COLLATION_LEVEL_PRIMARY] = primary << 8U;
        weights[SXML_COLLATION_LEVEL_SECONDARY] = element.secondary;
        weights[SXML_COLLATION_LEVEL_CASE] = primary != 0 ? case_rank : 0;
        weights[SXML_COLLATION_LEVEL_TERTIARY] = element.tertiary;
        if (collation->case_first != SXML_COLLATION_CASE_FIRST_OFF && element.tertiary != 0) {
            /* The case bits above the tertiary weight. */
            weights[SXML_COLLATION_LEVEL_TERTIARY] = (case_rank << 8U) | element.tertiary;
        }
        weights[SXML_COLLATION_LEVEL_QUATERNARY] = 0xFFFFFFU;
    }
    sxml_collation_element made = {.byte_offset = writer->byte_offset, .byte_end = writer->byte_end};
    memcpy(made.weights, weights, sizeof(weights));
    return sxml_collation_element_array_append(writer->elements, &made);
}

static sxml_status write_table_elements(element_writer *writer, uint32_t element_offset,
                                        uint32_t element_count) {
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t element_index = 0; status == SXML_STATUS_OK && element_index < element_count;
         element_index += 1) {
        status = write_element(writer, writer->table->elements[element_offset + element_index]);
    }
    return status;
}

/* The ranges of Unified_Ideograph (Unicode 16.0, PropList.txt). */
typedef struct codepoint_range {
    uint32_t first;
    uint32_t last;
} codepoint_range;

static const codepoint_range unified_ideographs[] = {
    {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},   {0xFA0E, 0xFA0F},   {0xFA11, 0xFA11},
    {0xFA13, 0xFA14},   {0xFA1F, 0xFA1F},   {0xFA21, 0xFA21},   {0xFA23, 0xFA24},
    {0xFA27, 0xFA29},   {0x20000, 0x2A6DF}, {0x2A700, 0x2B739}, {0x2B740, 0x2B81D},
    {0x2B820, 0x2CEA1}, {0x2CEB0, 0x2EBE0}, {0x2EBF0, 0x2EE5D}, {0x30000, 0x3134A},
    {0x31350, 0x323AF},
};

static bool is_unified_ideograph(uint32_t codepoint) {
    for (size_t range_index = 0;
         range_index < sizeof(unified_ideographs) / sizeof(unified_ideographs[0]);
         range_index += 1) {
        if (codepoint >= unified_ideographs[range_index].first &&
            codepoint <= unified_ideographs[range_index].last) {
            return true;
        }
    }
    return false;
}

/* The implicit weights of a codepoint without an entry (UTS #10, section
   10.1): [.AAAA.0020.0002][.BBBB.0000.0000]. */
static sxml_status write_implicit_elements(element_writer *writer, uint32_t codepoint) {
    const sxml_collation_element_table *table = writer->table;
    uint32_t base = 0;
    uint32_t second = 0;
    bool is_in_range = false;
    for (size_t range_index = 0; range_index < table->implicit_range_count; range_index += 1) {
        const uca_implicit_range *range = &table->implicit_ranges[range_index];
        if (codepoint >= range->first && codepoint <= range->last) {
            /* Section 10.1.3: Tangut, Nushu, Khitan. */
            base = range->base;
            second = ((codepoint - range->origin) & 0x7FFFU) | 0x8000U;
            is_in_range = true;
            break;
        }
    }
    if (!is_in_range) {
        /* Sections 10.1.1 and 10.1.2: core Han (the CJK Unified
           Ideographs and CJK Compatibility Ideographs blocks), other Han,
           and everything else. */
        if (is_unified_ideograph(codepoint)) {
            base = (codepoint >= 0x4E00U && codepoint <= 0x9FFFU) ||
                           (codepoint >= 0xF900U && codepoint <= 0xFAFFU)
                       ? 0xFB40U
                       : 0xFB80U;
        } else {
            base = 0xFBC0U;
        }
        base += codepoint >> 15U;
        second = (codepoint & 0x7FFFU) | 0x8000U;
    }
    sxml_status status = write_element(
        writer, (uca_table_element){.primary = (uint16_t)base,
                                    .secondary = SXML_UCA_COMMON_SECONDARY,
                                    .tertiary = SXML_UCA_COMMON_TERTIARY,
                                    .is_variable_marked = false});
    if (status == SXML_STATUS_OK) {
        status = write_element(writer, (uca_table_element){.primary = (uint16_t)second,
                                                           .secondary = 0,
                                                           .tertiary = 0,
                                                           .is_variable_marked = false});
    }
    return status;
}

/* The value of a decimal digit (general category Nd), whose primary the
   DUCET gives by value from digit zero's; -1 for any other character. */
static int digit_value(const sxml_collation_element_table *table, uint32_t codepoint) {
    if (sxml_regex_get_general_category(codepoint) != SXML_REGEX_GENERAL_CATEGORY_ND) {
        return -1;
    }
    const uca_mapping *mapping = find_mapping(table, codepoint);
    if (mapping == NULL || mapping->element_count != 1) {
        return -1;
    }
    const uint32_t primary = table->elements[mapping->element_offset].primary;
    if (primary < table->digit_zero_primary || primary > table->digit_zero_primary + 9U) {
        return -1;
    }
    return (int)(primary - table->digit_zero_primary);
}

/* Appends a numeric element: digit zero's primary (shifted) plus
   offset, common secondary and tertiary. */
static sxml_status write_numeric_element(element_writer *writer, uint32_t offset) {
    const uint32_t base = writer->table->digit_zero_primary << 8U;
    const bool is_upper_first = writer->collation->case_first == SXML_COLLATION_CASE_FIRST_UPPER;
    const uint32_t case_rank = is_upper_first ? 2U : 1U;
    sxml_collation_element made = {
        .weights = {base + offset, SXML_UCA_COMMON_SECONDARY, case_rank,
                    writer->collation->case_first == SXML_COLLATION_CASE_FIRST_OFF
                        ? SXML_UCA_COMMON_TERTIARY
                        : (case_rank << 8U) | SXML_UCA_COMMON_TERTIARY,
                    0xFFFFFFU},
        .byte_offset = writer->byte_offset,
        .byte_end = writer->byte_end};
    writer->is_after_variable = false;
    return sxml_collation_element_array_append(writer->elements, &made);
}

/*
 * numeric=yes (UTS #35, part 5, section 3.7): the digits from first to
 * last weigh as their value. Leading zeros are ignored; the value weighs
 * as its count of significant digits, then each digit, all in the 255
 * weights after digit zero's primary, so a longer number sorts after a
 * shorter one and equal lengths compare digit by digit.
 */
typedef struct digit_run {
    const character_array *characters;
    size_t first;
    size_t end;
} digit_run;

static sxml_status write_numeric_elements(element_writer *writer, digit_run run) {
    const uca_character *characters = run.characters->characters;
    size_t significant = run.first;
    while (significant + 1 < run.end &&
           digit_value(writer->table, characters[significant].codepoint) == 0) {
        significant += 1;
    }
    const size_t digit_count = run.end - significant;
    sxml_status status = SXML_STATUS_OK;
    if (digit_count < 0xFFU) {
        status = write_numeric_element(writer, (uint32_t)digit_count);
    } else {
        /* Very long numbers: 0xFF, then the count in three weights. */
        const uint32_t count = digit_count > 0xFFFFFFU ? 0xFFFFFFU : (uint32_t)digit_count;
        status = write_numeric_element(writer, 0xFFU);
        for (uint32_t shift = 24U; status == SXML_STATUS_OK && shift > 0; shift -= 8U) {
            status = write_numeric_element(writer, 1U + ((count >> (shift - 8U)) & 0xFFU) % 0xFFU);
        }
    }
    for (size_t digit_index = significant; status == SXML_STATUS_OK && digit_index < run.end;
         digit_index += 1) {
        status = write_numeric_element(
            writer, 1U + (uint32_t)digit_value(writer->table, characters[digit_index].codepoint));
    }
    return status;
}

/* ------------------------------------------------------ Matching entries */

/* What matched at one position: a contraction, an entry or nothing. */
typedef struct entry_match {
    uint32_t codepoints[SXML_UCA_MAXIMUM_CONTRACTION_LENGTH];
    size_t codepoint_count;
    uint32_t element_offset;
    uint32_t element_count;
    bool is_found;
} entry_match;

/* The next character at or after index that is not consumed, or count. */
static size_t next_unconsumed(const character_array *characters, size_t index) {
    while (index < characters->character_count && characters->characters[index].is_consumed) {
        index += 1;
    }
    return index;
}

/*
 * The longest contiguous match at start (UTS #10, step S2.1): a
 * contraction of start and the characters after it, else start's own
 * entry. *end is set after the last character taken.
 */
typedef struct match_position {
    const sxml_collation_element_table *table;
    const character_array *characters;
    size_t start;
} match_position;

static entry_match find_longest_match(match_position position, size_t *end) {
    const sxml_collation_element_table *table = position.table;
    const uca_character *characters = position.characters->characters;
    entry_match match = {.codepoint_count = 0, .is_found = false};
    size_t positions[SXML_UCA_MAXIMUM_CONTRACTION_LENGTH];
    uint32_t sequence[SXML_UCA_MAXIMUM_CONTRACTION_LENGTH];
    size_t sequence_count = 0;
    if (starts_contraction(table, characters[position.start].codepoint)) {
        size_t index = position.start;
        while (sequence_count < SXML_UCA_MAXIMUM_CONTRACTION_LENGTH &&
               index < position.characters->character_count) {
            positions[sequence_count] = index;
            sequence[sequence_count] = characters[index].codepoint;
            sequence_count += 1;
            index = next_unconsumed(position.characters, index + 1);
        }
        for (size_t length = sequence_count; length >= 2; length -= 1) {
            const uca_contraction *contraction = find_contraction(
                table, (codepoint_sequence){.codepoints = sequence, .codepoint_count = length});
            if (contraction != NULL) {
                memcpy(match.codepoints, sequence, length * sizeof(uint32_t));
                match.codepoint_count = length;
                match.element_offset = contraction->element_offset;
                match.element_count = contraction->element_count;
                match.is_found = true;
                *end = positions[length - 1] + 1;
                return match;
            }
        }
    }
    const uca_mapping *mapping = find_mapping(table, characters[position.start].codepoint);
    match.codepoints[0] = characters[position.start].codepoint;
    match.codepoint_count = 1;
    *end = position.start + 1;
    if (mapping != NULL) {
        match.element_offset = mapping->element_offset;
        match.element_count = mapping->element_count;
        match.is_found = true;
    }
    return match;
}

/*
 * Extends match by the unblocked non-starters after end (UTS #10, steps
 * S2.1.1 to S2.1.3): a non-starter C is unblocked when no character
 * between the match and C has a combining class of 0 or of at least C's;
 * when the match plus C is a contraction, C joins it and is consumed.
 * Widens writer's bytes to the characters taken.
 */
typedef struct discontiguous_extension {
    const sxml_collation_element_table *table;
    character_array *characters;
    size_t end;
} discontiguous_extension;

static void extend_match(discontiguous_extension extension, entry_match *match,
                         size_t *byte_end) {
    uca_character *characters = extension.characters->characters;
    uint8_t highest_skipped_class = 0;
    bool has_skipped = false;
    for (size_t index = next_unconsumed(extension.characters, extension.end);
         index < extension.characters->character_count &&
         match->codepoint_count < SXML_UCA_MAXIMUM_CONTRACTION_LENGTH;
         index = next_unconsumed(extension.characters, index + 1)) {
        const uint8_t combining_class = characters[index].combining_class;
        if (combining_class == 0) {
            return;
        }
        const bool is_unblocked = !has_skipped || highest_skipped_class < combining_class;
        if (is_unblocked) {
            match->codepoints[match->codepoint_count] = characters[index].codepoint;
            const uca_contraction *contraction = find_contraction(
                extension.table, (codepoint_sequence){.codepoints = match->codepoints,
                                                      .codepoint_count = match->codepoint_count + 1});
            if (contraction != NULL) {
                match->codepoint_count += 1;
                match->element_offset = contraction->element_offset;
                match->element_count = contraction->element_count;
                characters[index].is_consumed = true;
                if (characters[index].byte_end > *byte_end) {
                    *byte_end = characters[index].byte_end;
                }
                continue;
            }
        }
        has_skipped = true;
        if (combining_class > highest_skipped_class) {
            highest_skipped_class = combining_class;
        }
    }
}

/* ------------------------------------------------------ The elements */

static sxml_status append_elements(const sxml_collation_element_request *request,
                                   sxml_collation_element_array *elements) {
    const sxml_collation_element_table *table = request->collation->element_table;
    character_array characters = {.allocator = request->allocator, .characters = NULL};
    sxml_status status = make_characters(request->input, &characters);
    element_writer writer = {.collation = request->collation,
                             .table = table,
                             .elements = elements,
                             .is_after_variable = false};
    size_t index = next_unconsumed(&characters, 0);
    while (status == SXML_STATUS_OK && index < characters.character_count) {
        const uca_character *character = &characters.characters[index];
        writer.byte_offset = character->byte_offset;
        writer.byte_end = character->byte_end;
        if (request->collation->is_numeric && digit_value(table, character->codepoint) >= 0) {
            size_t end = index + 1;
            while (end < characters.character_count && !characters.characters[end].is_consumed &&
                   digit_value(table, characters.characters[end].codepoint) >= 0) {
                writer.byte_end = characters.characters[end].byte_end;
                end += 1;
            }
            status = write_numeric_elements(
                &writer, (digit_run){.characters = &characters, .first = index, .end = end});
            index = next_unconsumed(&characters, end);
            continue;
        }
        size_t end = index + 1;
        entry_match match = find_longest_match(
            (match_position){.table = table, .characters = &characters, .start = index}, &end);
        for (size_t taken = index; taken < end; taken += 1) {
            if (characters.characters[taken].byte_end > writer.byte_end) {
                writer.byte_end = characters.characters[taken].byte_end;
            }
        }
        if (match.is_found) {
            extend_match(
                (discontiguous_extension){.table = table, .characters = &characters, .end = end},
                &match, &writer.byte_end);
            status = write_table_elements(&writer, match.element_offset, match.element_count);
        } else {
            status = write_implicit_elements(&writer, character->codepoint);
        }
        index = next_unconsumed(&characters, end);
    }
    sxml_allocator_release(characters.allocator, characters.characters);
    return status;
}

/* ------------------------------------------------------------ The feature */

static const sxml_uca_collation_operations uca_collation_operations = {
    .find_table = find_table,
    .get_version = get_version,
    .append_elements = append_elements,
};

const sxml_optional_feature_definition sxml_uca_collation_feature_definition = {
    .feature_kind = SXML_OPTIONAL_FEATURE_KIND_UCA_COLLATION,
    .feature_name = SXML_BYTE_VIEW_INITIALIZER("uca-collation"),
    .operations = &uca_collation_operations,
    .finalize_state = finalize_feature_state,
    .library_functions = NULL,
};

sxml_optional_feature sxml_uca_collation_feature(void) {
    return (sxml_optional_feature){.definition = &sxml_uca_collation_feature_definition};
}
