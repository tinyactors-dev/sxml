/*
 * The UCA collation feature (src/collation_internal.h, "The UCA collation
 * feature"; include/sxml/processor.h, sxml_uca_collation_feature; packets
 * 7.15a and 7.15b, D213, D220): a root collation element table read from
 * the CLDR root collation (or the DUCET) the host serves as feature data
 * (glossary `collation_element_table`, `feature_data`), the reorder groups
 * and reorderings over it (glossary `reorder_group`), and the collation
 * elements of strings over a table or a tailoring of it
 * (src/collation_tailoring.c; UTS #10, Unicode Collation Algorithm,
 * version 16.0.0; UTS #35, part 5, section 3, for the parameters).
 *
 * Golden file: src/optional_feature.c (the test feature: reading feature
 * data once into the processor's slot), src/collation.c (element arrays
 * through an allocator).
 *
 * The table. allkeys.txt (UTS #10, section 9.1; CLDR's allkeys_CLDR.txt
 * has the same form) is read in two passes, one that counts and one that
 * fills, into three sorted arrays: the collation elements of every entry,
 * the entries of one codepoint, and the contractions (entries of several
 * codepoints). Each element keeps its 16-bit primary and secondary
 * weights, its tertiary weight and its case; the variable mark "*"
 * decides only the groups of maxVariable of the DUCET (below). Han and
 * unassigned codepoints are not in the file: their implicit weights are
 * computed (UTS #10, section 10.1), with the @implicitweights ranges the
 * file gives, else those of UTS #10 16.0.0. A tailoring is a table of the
 * same form over its parent (src/collation_uca_internal.h); lookups try
 * the chain from the tailoring to the root.
 *
 * Reordering (UTS #35, part 5, section 3.13): the root's primaries fall
 * into the reorder groups of src/collation_tables.c, each starting at the
 * primary of its first codepoint; a reordering moves whole groups, so a
 * primary weighs as its group's new start plus its distance from the old
 * one.
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
 *    section 3.14), the reordering of primaries. Weights are shifted left
 *    by 8 bits, so numeric weights fit between the primary of digit zero
 *    and the next one, and a tailoring's between the root's weights.
 *
 * No recursion (D71); the table lives in the processor's feature slot and
 * is read only by the processor's thread (D35).
 */
#include "collation_uca_internal.h"

#include "memory_internal.h"
#include "optional_feature_internal.h"
#include "processor_internal.h"
#include "regex_internal.h"
#include "sort_internal.h"
#include "unicode_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------- The table */

/* At most this many elements in one entry; the DUCET's are at most 18. */
#define SXML_UCA_MAXIMUM_ENTRY_ELEMENT_COUNT 32U

/* The DUCET's common secondary and tertiary weights (UTS #10, section
   10.1, the implicit weights [.AAAA.0020.0002][.BBBB.0000.0000]). */
#define SXML_UCA_COMMON_SECONDARY 0x0020U
#define SXML_UCA_COMMON_TERTIARY 0x0002U

/* The groups of maxVariable a primary weight below digit zero's is in,
   in the order of sxml_collation_variable_group, and a weight that is
   never variable. */
#define SXML_UCA_GROUP_NEVER_VARIABLE 4U
#define SXML_UCA_GROUP_UNKNOWN 0xFFU

/* The base of the implicit weights of unassigned codepoints (UTS #10,
   section 10.1.2). */
#define SXML_UCA_UNASSIGNED_BASE 0xFBC0U

void sxml_uca_release_table(sxml_collation_element_table *table) {
    if (table == NULL) {
        return;
    }
    const sxml_allocator allocator = table->allocator;
    sxml_allocator_release(allocator, table->elements);
    sxml_allocator_release(allocator, table->mappings);
    sxml_allocator_release(allocator, table->contractions);
    sxml_allocator_release(allocator, table->variable_groups);
    sxml_allocator_release(allocator, table->group_starts);
    sxml_allocator_release(allocator, table->reorder_ranges);
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

/* One entry line as read. */
typedef struct entry_line {
    uint32_t codepoints[SXML_UCA_MAXIMUM_CONTRACTION_LENGTH];
    size_t codepoint_count;
    sxml_uca_element elements[SXML_UCA_MAXIMUM_ENTRY_ELEMENT_COUNT];
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
        sxml_uca_element element = {0};
        uint32_t primary = 0;
        uint32_t secondary = 0;
        uint32_t tertiary = 0;
        if (line->element_count == SXML_UCA_MAXIMUM_ENTRY_ELEMENT_COUNT ||
            !read_byte(cursor, '[')) {
            return false;
        }
        const bool is_variable_marked = cursor_peek(cursor) == '*';
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
        element.flags = (uint8_t)((is_variable_marked ? SXML_UCA_ELEMENT_FLAG_VARIABLE_MARKED : 0U) |
                                  (is_upper_tertiary(tertiary) ? SXML_UCA_ELEMENT_FLAG_UPPER : 0U));
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
    sxml_uca_implicit_range range = {0};
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
        sxml_uca_implicit_range *earlier = &table->implicit_ranges[range_index];
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
                       line.element_count * sizeof(sxml_uca_element));
            }
            if (line.codepoint_count == 1) {
                table->mappings[mapping_count] =
                    (sxml_uca_mapping){.codepoint = line.codepoints[0],
                                  .element_offset = (uint32_t)element_count,
                                  .element_count = (uint32_t)line.element_count};
            } else if (table->contractions != NULL) {
                sxml_uca_contraction *contraction = &table->contractions[contraction_count];
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

static bool mapping_is_before(const void *state, sxml_sort_pair pair) {
    (void)state;
    return ((const sxml_uca_mapping *)pair.first)->codepoint <
           ((const sxml_uca_mapping *)pair.second)->codepoint;
}

/* Orders sequences lexicographically, a proper prefix first. */
static int compare_sequences(sxml_uca_codepoints first, sxml_uca_codepoints second) {
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

static sxml_uca_codepoints contraction_sequence(const sxml_uca_contraction *contraction) {
    return (sxml_uca_codepoints){.codepoints = contraction->codepoints,
                                 .codepoint_count = contraction->codepoint_count};
}

static bool contraction_is_before(const void *state, sxml_sort_pair pair) {
    (void)state;
    return compare_sequences(contraction_sequence(pair.first),
                             contraction_sequence(pair.second)) < 0;
}

void sxml_uca_sort_entries(sxml_collation_element_table *table) {
    sxml_sort_in_place(&(sxml_sort_request){.elements = table->mappings,
                                            .element_count = table->mapping_count,
                                            .element_size = sizeof(sxml_uca_mapping),
                                            .is_before = mapping_is_before,
                                            .state = NULL});
    sxml_sort_in_place(&(sxml_sort_request){.elements = table->contractions,
                                            .element_count = table->contraction_count,
                                            .element_size = sizeof(sxml_uca_contraction),
                                            .is_before = contraction_is_before,
                                            .state = NULL});
}

/* The entry of codepoint in table alone, or NULL. */
static const sxml_uca_mapping *find_own_mapping(const sxml_collation_element_table *table,
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

bool sxml_uca_find_mapping(const sxml_collation_element_table *table, uint32_t codepoint,
                           sxml_uca_entry *entry) {
    for (const sxml_collation_element_table *chain_table = table; chain_table != NULL;
         chain_table = chain_table->parent) {
        const sxml_uca_mapping *mapping = find_own_mapping(chain_table, codepoint);
        if (mapping != NULL) {
            *entry = (sxml_uca_entry){.elements = chain_table->elements + mapping->element_offset,
                                      .element_count = mapping->element_count};
            return true;
        }
    }
    return false;
}

/* The index of the first contraction of table alone not before
   sequence. */
static size_t lower_bound_contraction(const sxml_collation_element_table *table,
                                      sxml_uca_codepoints sequence) {
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

bool sxml_uca_find_contraction(const sxml_collation_element_table *table,
                               sxml_uca_codepoints sequence, sxml_uca_entry *entry) {
    for (const sxml_collation_element_table *chain_table = table; chain_table != NULL;
         chain_table = chain_table->parent) {
        const size_t found = lower_bound_contraction(chain_table, sequence);
        if (found < chain_table->contraction_count &&
            compare_sequences(contraction_sequence(&chain_table->contractions[found]), sequence) == 0) {
            const sxml_uca_contraction *contraction = &chain_table->contractions[found];
            *entry = (sxml_uca_entry){.elements = chain_table->elements + contraction->element_offset,
                                      .element_count = contraction->element_count};
            return true;
        }
    }
    return false;
}

bool sxml_uca_starts_contraction(const sxml_collation_element_table *table, uint32_t codepoint) {
    for (const sxml_collation_element_table *chain_table = table; chain_table != NULL;
         chain_table = chain_table->parent) {
        const size_t found = lower_bound_contraction(
            chain_table, (sxml_uca_codepoints){.codepoints = &codepoint, .codepoint_count = 1});
        if (found < chain_table->contraction_count && chain_table->contractions[found].codepoints[0] == codepoint) {
            return true;
        }
    }
    return false;
}

/* The maxVariable group of a variable character (UTS #35, part 5,
   section 3.6, by general category), or SXML_UCA_GROUP_NEVER_VARIABLE
   for a character that is neither space, punctuation nor a symbol. */
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
    if ((category_bit & ((1U << (uint32_t)SXML_REGEX_GENERAL_CATEGORY_SM) |
                         (1U << (uint32_t)SXML_REGEX_GENERAL_CATEGORY_SK) |
                         (1U << (uint32_t)SXML_REGEX_GENERAL_CATEGORY_SO))) != 0) {
        return (uint8_t)SXML_COLLATION_VARIABLE_GROUP_SYMBOL;
    }
    return SXML_UCA_GROUP_NEVER_VARIABLE;
}

/*
 * The group of every primary below digit zero's.
 * - The CLDR root: by the reorder groups space, punct, symbol and
 *   currency (UTS #35, part 5, section 3.6: maxVariable names the last
 *   group that is variable); a primary below the space group (U+FFFE's)
 *   is never variable.
 * - The DUCET: a primary takes the lowest group of the characters whose
 *   first element it is: the variable ("*") elements are space,
 *   punctuation and symbols, and the currency symbols follow them; a
 *   primary that no single character starts with takes the group of the
 *   primary before it.
 */
static void make_root_variable_groups(sxml_collation_element_table *table) {
    const uint32_t *starts = table->group_starts;
    for (size_t primary = 0; primary < table->digit_zero_primary; primary += 1) {
        uint8_t group = SXML_UCA_GROUP_NEVER_VARIABLE;
        for (size_t group_index = 0; group_index <= SXML_COLLATION_VARIABLE_GROUP_CURRENCY;
             group_index += 1) {
            if (primary >= starts[group_index] && primary < starts[group_index + 1]) {
                group = (uint8_t)group_index;
            }
        }
        table->variable_groups[primary] = group;
    }
}

/* Whether the first reorder groups are the four of maxVariable, in its
   order (src/collation_tables.c). */
static bool has_variable_reorder_groups(void) {
    static const char *const names[] = {"space", "punct", "symbol", "currency"};
    for (size_t group_index = 0; group_index < 4; group_index += 1) {
        if (sxml_collation_reorder_group_count <= group_index ||
            strcmp(sxml_collation_reorder_groups[group_index].codes, names[group_index]) != 0) {
            return false;
        }
    }
    return true;
}

static sxml_status make_variable_groups(sxml_collation_element_table *table, bool is_cldr_root) {
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        table->allocator,
        (sxml_array_allocation_request){.element_count = table->digit_zero_primary,
                                 .element_size = sizeof(uint8_t),
                                 .element_alignment = alignof(uint8_t)},
        &memory);
    if (status != SXML_STATUS_OK || memory == NULL) {
        return status;
    }
    table->variable_groups = memory;
    if (is_cldr_root && table->group_starts != NULL && has_variable_reorder_groups()) {
        make_root_variable_groups(table);
        return SXML_STATUS_OK;
    }
    memset(table->variable_groups, SXML_UCA_GROUP_UNKNOWN, table->digit_zero_primary);
    for (size_t mapping_index = 0; mapping_index < table->mapping_count; mapping_index += 1) {
        const sxml_uca_mapping *mapping = &table->mappings[mapping_index];
        if (mapping->element_count == 0) {
            continue;
        }
        const sxml_uca_element *first = &table->elements[mapping->element_offset];
        if (first->primary == 0 || first->primary >= table->digit_zero_primary) {
            continue;
        }
        const bool is_marked = (first->flags & SXML_UCA_ELEMENT_FLAG_VARIABLE_MARKED) != 0;
        uint8_t group = group_of_category(mapping->codepoint);
        if (is_marked && group == SXML_UCA_GROUP_NEVER_VARIABLE) {
            group = (uint8_t)SXML_COLLATION_VARIABLE_GROUP_SYMBOL;
        } else if (!is_marked && group != SXML_COLLATION_VARIABLE_GROUP_CURRENCY) {
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

/* The implicit weights of UTS #10 version 16.0.0, section 10.1.3 (Table
   16), for a root table whose file has no @implicitweights lines
   (allkeys_CLDR.txt has none; the CLDR root has the same weights). */
static void set_default_implicit_ranges(sxml_collation_element_table *table) {
    static const sxml_uca_implicit_range ranges[] = {
        {.first = 0x17000U, .last = 0x18AFFU, .base = 0xFB00U, .origin = 0x17000U},
        {.first = 0x18B00U, .last = 0x18CFFU, .base = 0xFB02U, .origin = 0x18B00U},
        {.first = 0x18D00U, .last = 0x18D7FU, .base = 0xFB00U, .origin = 0x17000U},
        {.first = 0x1B170U, .last = 0x1B2FFU, .base = 0xFB01U, .origin = 0x1B170U},
    };
    memcpy(table->implicit_ranges, ranges, sizeof(ranges));
    table->implicit_range_count = sizeof(ranges) / sizeof(ranges[0]);
}

/* The first primary of codepoint in a root table: its entry's, or its
   implicit weight's. */
static uint32_t first_primary(const sxml_collation_element_table *root, uint32_t codepoint) {
    sxml_uca_entry entry;
    if (sxml_uca_find_mapping(root, codepoint, &entry) && entry.element_count > 0) {
        return entry.elements[0].primary;
    }
    sxml_uca_element implicit[2];
    sxml_uca_make_implicit_elements(root, codepoint, implicit);
    return implicit[0].primary;
}

/*
 * Where each reorder group starts in a root table (UTS #35, part 5,
 * section 3.13), from the first codepoint src/collation_tables.c gives,
 * and after the last group the primary of U+FFFD, where CLDR's trailing
 * weights start. Leaves group_starts NULL when they are not ascending (a
 * table not in the root order), so reorder is then not honoured.
 */
static sxml_status make_group_starts(sxml_collation_element_table *table) {
    void *memory = NULL;
    const size_t start_count = sxml_collation_reorder_group_count + 1;
    sxml_status status = sxml_allocator_allocate_array(
        table->allocator,
        (sxml_array_allocation_request){.element_count = start_count,
                                 .element_size = sizeof(uint32_t),
                                 .element_alignment = alignof(uint32_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t *starts = memory;
    if (starts == NULL) {
        return SXML_STATUS_OK;
    }
    for (size_t group_index = 0; group_index < sxml_collation_reorder_group_count;
         group_index += 1) {
        const sxml_collation_reorder_group_definition *group =
            &sxml_collation_reorder_groups[group_index];
        starts[group_index] = group->group_kind == SXML_COLLATION_REORDER_GROUP_KIND_UNASSIGNED
                                  ? SXML_UCA_UNASSIGNED_BASE
                                  : first_primary(table, group->first_codepoint);
    }
    starts[start_count - 1] = first_primary(table, 0xFFFDU);
    for (size_t start_index = 1; start_index < start_count; start_index += 1) {
        if (starts[start_index] <= starts[start_index - 1]) {
            sxml_allocator_release(table->allocator, starts);
            return SXML_STATUS_OK;
        }
    }
    table->group_starts = starts;
    return SXML_STATUS_OK;
}

/* The data of a root table: its text, and whether it is the CLDR root
   (else the DUCET). */
typedef struct table_source {
    sxml_byte_view content;
    bool is_cldr_root;
} table_source;

/* Reads source into a new table in *table (NULL on failure). */
static sxml_status read_table(sxml_allocator allocator, table_source source,
                              sxml_collation_element_table **made) {
    const sxml_byte_view content = source.content;
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
    table->root = table;
    status = read_lines((reading_pass){.table = table, .is_filling = false}, content);
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(allocator,
                                         (sxml_array_allocation_request){.element_count = table->element_count,
                                                                  .element_size = sizeof(sxml_uca_element),
                                                                  .element_alignment = alignof(sxml_uca_element)},
                                         &memory);
        table->elements = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(allocator,
                                         (sxml_array_allocation_request){.element_count = table->mapping_count,
                                                                  .element_size = sizeof(sxml_uca_mapping),
                                                                  .element_alignment = alignof(sxml_uca_mapping)},
                                         &memory);
        table->mappings = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(
            allocator,
            (sxml_array_allocation_request){.element_count = table->contraction_count,
                                     .element_size = sizeof(sxml_uca_contraction),
                                     .element_alignment = alignof(sxml_uca_contraction)},
            &memory);
        table->contractions = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = read_lines((reading_pass){.table = table, .is_filling = true}, content);
    }
    if (status == SXML_STATUS_OK) {
        sxml_uca_sort_entries(table);
        if (table->implicit_range_count == 0) {
            set_default_implicit_ranges(table);
        }
        /* Digit zero's primary bounds the variable weights and places the
           numeric ones; a table without it is not a DUCET. */
        const sxml_uca_mapping *zero = find_own_mapping(table, '0');
        if (zero == NULL || zero->element_count == 0 || table->elements == NULL ||
            table->elements[zero->element_offset].primary == 0 || table->version_byte_count == 0) {
            status = SXML_STATUS_MALFORMED_INPUT;
        } else {
            table->digit_zero_primary = table->elements[zero->element_offset].primary;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = make_group_starts(table);
    }
    if (status == SXML_STATUS_OK) {
        status = make_variable_groups(table, source.is_cldr_root);
    }
    if (status != SXML_STATUS_OK) {
        sxml_uca_release_table(table);
        return status;
    }
    *made = table;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Reordering */

/* Whether a reorder code is one of a group's codes, without ASCII
   case. */
static bool group_has_code(const sxml_collation_reorder_group_definition *group,
                           sxml_byte_view code) {
    const char *codes = group->codes;
    size_t position = 0;
    while (codes[position] != '\0') {
        size_t end = position;
        while (codes[end] != '\0' && codes[end] != ' ') {
            end += 1;
        }
        bool is_equal = end - position == code.byte_count;
        for (size_t byte_index = 0; is_equal && byte_index < code.byte_count; byte_index += 1) {
            uint8_t first = (uint8_t)codes[position + byte_index];
            uint8_t second = code.bytes[byte_index];
            first = first >= 'A' && first <= 'Z' ? (uint8_t)(first + 0x20U) : first;
            second = second >= 'A' && second <= 'Z' ? (uint8_t)(second + 0x20U) : second;
            is_equal = first == second;
        }
        if (is_equal) {
            return true;
        }
        position = codes[end] == ' ' ? end + 1 : end;
    }
    return false;
}

static bool code_is(sxml_byte_view code, const char *text) {
    const size_t byte_count = strlen(text);
    if (code.byte_count != byte_count) {
        return false;
    }
    for (size_t byte_index = 0; byte_index < byte_count; byte_index += 1) {
        uint8_t byte = code.bytes[byte_index];
        byte = byte >= 'A' && byte <= 'Z' ? (uint8_t)(byte + 0x20U) : byte;
        if (byte != (uint8_t)text[byte_index]) {
            return false;
        }
    }
    return true;
}

static bool is_code_separator(uint8_t byte) {
    return byte == ',' || byte == ' ' || byte == '-' || byte == '_' || byte == '\t' ||
           byte == '\n' || byte == '\r';
}

/* At most this many groups (src/collation_tables.c has fewer). */
#define SXML_UCA_MAXIMUM_GROUP_COUNT 256U

/* No group: the end of the order. */
#define SXML_UCA_NO_GROUP 0xFFFFU

/*
 * UTS #35, part 5, section 3.13: the special groups the codes do not
 * name stay at the start in their order; then the groups named, in the
 * codes' order, with "others" (or "Zzzz") standing for every script
 * group not named, in the root order; when "others" is not named, those
 * follow at the end.
 */
sxml_status sxml_uca_set_reordering(const sxml_uca_reordering_request *request,
                                    bool *is_honoured) {
    sxml_collation_element_table *table = request->table;
    const sxml_collation_element_table *root = table->root;
    *is_honoured = false;
    const size_t group_count = sxml_collation_reorder_group_count;
    if (root->group_starts == NULL || group_count > SXML_UCA_MAXIMUM_GROUP_COUNT) {
        return SXML_STATUS_OK;
    }
    /* The groups named, in order; SXML_UCA_NO_GROUP stands for others. */
    uint16_t named[SXML_UCA_MAXIMUM_GROUP_COUNT + 1];
    size_t named_count = 0;
    bool is_named[SXML_UCA_MAXIMUM_GROUP_COUNT] = {false};
    bool has_others = false;
    bool has_default = false;
    size_t code_count = 0;
    const sxml_byte_view codes = request->codes;
    size_t position = 0;
    while (position < codes.byte_count) {
        if (is_code_separator(codes.bytes[position])) {
            position += 1;
            continue;
        }
        size_t end = position;
        while (end < codes.byte_count && !is_code_separator(codes.bytes[end])) {
            end += 1;
        }
        const sxml_byte_view code = {.bytes = codes.bytes + position, .byte_count = end - position};
        position = end;
        code_count += 1;
        if (code_is(code, "default")) {
            has_default = true;
            continue;
        }
        if (code_is(code, "others") || code_is(code, "zzzz")) {
            if (has_others) {
                return SXML_STATUS_OK;
            }
            has_others = true;
            named[named_count] = SXML_UCA_NO_GROUP;
            named_count += 1;
            continue;
        }
        size_t found = group_count;
        for (size_t group_index = 0; group_index < group_count; group_index += 1) {
            if (sxml_collation_reorder_groups[group_index].group_kind !=
                    SXML_COLLATION_REORDER_GROUP_KIND_UNASSIGNED &&
                group_has_code(&sxml_collation_reorder_groups[group_index], code)) {
                found = group_index;
                break;
            }
        }
        if (found == group_count || is_named[found] || named_count == SXML_UCA_MAXIMUM_GROUP_COUNT) {
            return SXML_STATUS_OK;
        }
        is_named[found] = true;
        named[named_count] = (uint16_t)found;
        named_count += 1;
    }
    if (has_default && code_count > 1) {
        return SXML_STATUS_OK;
    }
    /* The new order of the groups. */
    uint16_t order[SXML_UCA_MAXIMUM_GROUP_COUNT];
    size_t order_count = 0;
    for (size_t group_index = 0; group_index < group_count; group_index += 1) {
        if (sxml_collation_reorder_groups[group_index].group_kind ==
                SXML_COLLATION_REORDER_GROUP_KIND_SPECIAL &&
            !is_named[group_index]) {
            order[order_count] = (uint16_t)group_index;
            order_count += 1;
        }
    }
    for (size_t named_index = 0; named_index <= named_count; named_index += 1) {
        const bool is_end = named_index == named_count;
        if (!is_end && named[named_index] != SXML_UCA_NO_GROUP) {
            order[order_count] = named[named_index];
            order_count += 1;
            continue;
        }
        if (is_end && has_others) {
            break;
        }
        /* "others" here, or at the end when it is not named. */
        for (size_t group_index = 0; group_index < group_count; group_index += 1) {
            if (sxml_collation_reorder_groups[group_index].group_kind !=
                    SXML_COLLATION_REORDER_GROUP_KIND_SPECIAL &&
                !is_named[group_index]) {
                order[order_count] = (uint16_t)group_index;
                order_count += 1;
            }
        }
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        table->allocator,
        (sxml_array_allocation_request){.element_count = group_count,
                                 .element_size = sizeof(sxml_uca_reorder_range),
                                 .element_alignment = alignof(sxml_uca_reorder_range)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_uca_reorder_range *ranges = memory;
    uint32_t next_start = root->group_starts[0];
    for (size_t order_index = 0; order_index < order_count; order_index += 1) {
        const size_t group_index = order[order_index];
        const uint32_t first = root->group_starts[group_index];
        const uint32_t end = root->group_starts[group_index + 1];
        ranges[group_index] = (sxml_uca_reorder_range){
            .first = first, .end = end, .shift = (int32_t)next_start - (int32_t)first};
        next_start += end - first;
    }
    sxml_allocator_release(table->allocator, table->reorder_ranges);
    table->reorder_ranges = ranges;
    table->reorder_range_count = group_count;
    table->has_reordering = true;
    *is_honoured = true;
    return SXML_STATUS_OK;
}

/* A primary after the reordering of the chain of table (the nearest
   tailoring that has one). */
static uint32_t reorder_primary(const sxml_collation_element_table *table, uint32_t primary) {
    for (const sxml_collation_element_table *chain_table = table; chain_table != NULL;
         chain_table = chain_table->parent) {
        if (!chain_table->has_reordering) {
            continue;
        }
        /* The ranges are in the root order of their groups. */
        size_t low = 0;
        size_t high = chain_table->reorder_range_count;
        while (low < high) {
            const size_t middle = low + ((high - low) / 2);
            const sxml_uca_reorder_range *range = &chain_table->reorder_ranges[middle];
            if (primary < range->first) {
                high = middle;
            } else if (primary >= range->end) {
                low = middle + 1;
            } else {
                return (uint32_t)((int32_t)primary + range->shift);
            }
        }
        return primary;
    }
    return primary;
}

/* ---------------------------------------------------- The feature's slot */

/* The reader of the feature data: reads the table into *state. */
typedef struct table_reading {
    sxml_allocator allocator;
    bool is_cldr_root;
    sxml_collation_element_table *table;
} table_reading;

static sxml_status read_table_data(void *state, sxml_byte_view content) {
    table_reading *reading = state;
    return read_table(reading->allocator,
                      (table_source){.content = content, .is_cldr_root = reading->is_cldr_root},
                      &reading->table);
}

static const sxml_feature_data_reader_operations table_reader_operations = {
    .read = read_table_data,
};

static void finalize_feature_state(void *state, sxml_allocator allocator) {
    sxml_uca_feature_state *feature_state = state;
    for (size_t tailoring_index = 0; tailoring_index < feature_state->tailoring_count;
         tailoring_index += 1) {
        sxml_uca_release_table(feature_state->tailorings[tailoring_index].made_table);
    }
    sxml_uca_release_table(feature_state->table);
    sxml_allocator_release(allocator, feature_state);
}

/* The root tables the feature asks for, in order: CLDR's root collation
   (UTS #35, part 5, section 2.3, allkeys_CLDR.txt), then the DUCET of UTS
   #10 version 16.0.0, sxml's Unicode version, whose order differs from
   it only in U+FFFE and U+FFFF, the Tibetan contractions of U+0FB2 and
   U+0FB3, and which symbols are marked variable. */
static const sxml_byte_view root_table_uris[] = {
    SXML_BYTE_VIEW_INITIALIZER("urn:sxml:collation-data:uca/allkeys_CLDR.txt"),
    SXML_BYTE_VIEW_INITIALIZER("http://www.unicode.org/Public/UCA/16.0.0/allkeys.txt"),
};

static sxml_status find_table(sxml_processor *processor,
                              const sxml_collation_element_table **table) {
    *table = NULL;
    const sxml_uca_feature_state *kept = sxml_processor_get_optional_feature_state(
        processor, SXML_OPTIONAL_FEATURE_KIND_UCA_COLLATION);
    if (kept != NULL) {
        *table = kept->table;
        return SXML_STATUS_OK;
    }
    table_reading reading = {.allocator = processor->allocator, .is_cldr_root = false, .table = NULL};
    sxml_status status = SXML_STATUS_OK;
    for (size_t uri_index = 0; status == SXML_STATUS_OK && reading.table == NULL &&
                               uri_index < sizeof(root_table_uris) / sizeof(root_table_uris[0]);
         uri_index += 1) {
        sxml_feature_data_outcome outcome = {.is_available = false};
        reading.is_cldr_root = uri_index == 0;
        status = sxml_optional_feature_read_data(
            &(sxml_feature_data_request){
                .processor = processor,
                .resource_kind = SXML_RESOURCE_KIND_COLLATION_DATA,
                .uri = root_table_uris[uri_index],
                .reader = {.state = &reading, .operations = &table_reader_operations}},
            &outcome);
    }
    if (status != SXML_STATUS_OK) {
        sxml_uca_release_table(reading.table);
        return status;
    }
    /* The answer is kept, also "no data": the resolver is asked once. */
    void *memory = NULL;
    status = sxml_allocator_allocate(
        processor->allocator,
        (sxml_allocation_request){.byte_count = sizeof(sxml_uca_feature_state),
                                  .alignment = alignof(sxml_uca_feature_state)},
        &memory);
    if (status != SXML_STATUS_OK) {
        sxml_uca_release_table(reading.table);
        return status;
    }
    sxml_uca_feature_state *state = memory;
    memset(state, 0, sizeof(*state));
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

static sxml_status find_tailoring(const sxml_uca_tailoring_request *request,
                                  sxml_uca_tailoring *tailoring) {
    tailoring->table = request->table;
    tailoring->is_honoured = false;
    sxml_uca_feature_state *state = sxml_processor_get_optional_feature_state(
        request->processor, SXML_OPTIONAL_FEATURE_KIND_UCA_COLLATION);
    if (state == NULL || state->table == NULL) {
        return SXML_STATUS_OK;
    }
    return sxml_uca_build_tailoring(
        &(sxml_uca_tailoring_build){
            .processor = request->processor, .state = state, .request = request},
        tailoring);
}

static void apply_settings(const sxml_collation_element_table *table, sxml_collation *collation) {
    /* The nearest table of the chain that sets a parameter decides it. */
    uint32_t decided = collation->explicit_parameters;
    for (const sxml_collation_element_table *chain_table = table; chain_table != NULL;
         chain_table = chain_table->parent) {
        const sxml_collation *settings = &chain_table->settings;
        const uint32_t applied = settings->explicit_parameters & ~decided;
        if ((applied & SXML_COLLATION_PARAMETER_STRENGTH) != 0) {
            collation->strength = settings->strength;
        }
        if ((applied & SXML_COLLATION_PARAMETER_ALTERNATE) != 0) {
            collation->alternate = settings->alternate;
        }
        if ((applied & SXML_COLLATION_PARAMETER_MAXIMUM_VARIABLE) != 0) {
            collation->maximum_variable = settings->maximum_variable;
        }
        if ((applied & SXML_COLLATION_PARAMETER_CASE_FIRST) != 0) {
            collation->case_first = settings->case_first;
        }
        if ((applied & SXML_COLLATION_PARAMETER_CASE_LEVEL) != 0) {
            collation->has_case_level = settings->has_case_level;
        }
        if ((applied & SXML_COLLATION_PARAMETER_BACKWARDS) != 0) {
            collation->is_backwards = settings->is_backwards;
        }
        if ((applied & SXML_COLLATION_PARAMETER_NUMERIC) != 0) {
            collation->is_numeric = settings->is_numeric;
        }
        decided |= applied;
    }
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
    void *characters = array->characters;
    const sxml_status status = sxml_allocator_reserve_array(
        array->allocator,
        &(sxml_array_reservation_request){.elements = &characters,
                                          .capacity = &array->character_capacity,
                                          .element_count = array->character_count,
                                          .required_count = array->character_count + 1,
                                          .element_size = sizeof(uca_character),
                                          .element_alignment = alignof(uca_character),
                                          .minimum_capacity = 32});
    array->characters = characters;
    if (status != SXML_STATUS_OK) {
        return status;
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

static bool is_variable_primary(const element_writer *writer, uint32_t primary) {
    const sxml_collation_element_table *root = writer->table->root;
    return primary != 0 && primary < root->digit_zero_primary &&
           root->variable_groups[primary] <= (uint8_t)writer->collation->maximum_variable;
}

/* Appends one element of the table (or computed) with the parameters
   applied: each level's weight is the table weight shifted left by 8
   bits plus the tailoring's offset (src/collation_uca_internal.h), the
   primary after the reordering. */
static sxml_status write_element(element_writer *writer, sxml_uca_element element) {
    const sxml_collation *collation = writer->collation;
    uint32_t primary = element.primary;
    if (primary != 0 && (element.flags & SXML_UCA_ELEMENT_FLAG_CONTINUATION) == 0) {
        primary = reorder_primary(writer->table, primary);
    }
    const uint32_t primary_weight = (primary << 8U) | element.primary_offset;
    const uint32_t secondary_weight = ((uint32_t)element.secondary << 8U) | element.secondary_offset;
    const uint32_t tertiary_weight = ((uint32_t)element.tertiary << 8U) | element.tertiary_offset;
    uint32_t weights[SXML_COLLATION_ELEMENT_LEVEL_COUNT] = {0};
    const bool is_variable = collation->alternate != SXML_COLLATION_ALTERNATE_NON_IGNORABLE &&
                             (element.flags & SXML_UCA_ELEMENT_FLAG_CONTINUATION) == 0 &&
                             is_variable_primary(writer, element.primary);
    const bool is_ignorable = primary_weight == 0 && secondary_weight == 0 && tertiary_weight == 0;
    if (is_variable) {
        /* Shifted: [.0000.0000.0000.pppp]; blanked: ignorable. */
        if (collation->alternate == SXML_COLLATION_ALTERNATE_SHIFTED) {
            weights[SXML_COLLATION_LEVEL_QUATERNARY] = primary_weight;
        }
        writer->is_after_variable = true;
    } else if (is_ignorable ||
               (primary_weight == 0 && writer->is_after_variable &&
                collation->alternate != SXML_COLLATION_ALTERNATE_NON_IGNORABLE)) {
        /* Completely ignorable, or ignorable after a variable element. */
    } else {
        if (primary_weight != 0) {
            writer->is_after_variable = false;
        }
        const bool is_upper = (element.flags & SXML_UCA_ELEMENT_FLAG_UPPER) != 0;
        const bool is_upper_first = collation->case_first == SXML_COLLATION_CASE_FIRST_UPPER;
        const uint32_t case_rank = is_upper == is_upper_first ? 1U : 2U;
        weights[SXML_COLLATION_LEVEL_PRIMARY] = primary_weight;
        weights[SXML_COLLATION_LEVEL_SECONDARY] = secondary_weight;
        weights[SXML_COLLATION_LEVEL_CASE] = primary_weight != 0 ? case_rank : 0;
        weights[SXML_COLLATION_LEVEL_TERTIARY] = tertiary_weight;
        if (collation->case_first != SXML_COLLATION_CASE_FIRST_OFF && tertiary_weight != 0) {
            /* The case bits above the tertiary weight. */
            weights[SXML_COLLATION_LEVEL_TERTIARY] = (case_rank << 16U) | tertiary_weight;
        }
        weights[SXML_COLLATION_LEVEL_QUATERNARY] = 0xFFFFFFU;
    }
    sxml_collation_element made = {.byte_offset = writer->byte_offset, .byte_end = writer->byte_end};
    memcpy(made.weights, weights, sizeof(weights));
    return sxml_collation_element_array_append(writer->elements, &made);
}

static sxml_status write_entry_elements(element_writer *writer, sxml_uca_entry entry) {
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t element_index = 0; status == SXML_STATUS_OK && element_index < entry.element_count;
         element_index += 1) {
        status = write_element(writer, entry.elements[element_index]);
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
void sxml_uca_make_implicit_elements(const sxml_collation_element_table *root,
                                     uint32_t codepoint, sxml_uca_element elements[2]) {
    uint32_t base = 0;
    uint32_t second = 0;
    bool is_in_range = false;
    for (size_t range_index = 0; range_index < root->implicit_range_count; range_index += 1) {
        const sxml_uca_implicit_range *range = &root->implicit_ranges[range_index];
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
            base = SXML_UCA_UNASSIGNED_BASE;
        }
        base += codepoint >> 15U;
        second = (codepoint & 0x7FFFU) | 0x8000U;
    }
    elements[0] = (sxml_uca_element){.primary = (uint16_t)base,
                                     .secondary = SXML_UCA_COMMON_SECONDARY,
                                     .tertiary = SXML_UCA_COMMON_TERTIARY,
                                     .flags = 0};
    elements[1] = (sxml_uca_element){.primary = (uint16_t)second,
                                     .secondary = 0,
                                     .tertiary = 0,
                                     .flags = SXML_UCA_ELEMENT_FLAG_CONTINUATION};
}

static sxml_status write_implicit_elements(element_writer *writer, uint32_t codepoint) {
    sxml_uca_element elements[2];
    sxml_uca_make_implicit_elements(writer->table->root, codepoint, elements);
    return write_entry_elements(writer, (sxml_uca_entry){.elements = elements, .element_count = 2});
}

/* The value of a decimal digit (general category Nd), whose primary the
   DUCET gives by value from digit zero's; -1 for any other character. */
static int digit_value(const sxml_collation_element_table *table, uint32_t codepoint) {
    if (sxml_regex_get_general_category(codepoint) != SXML_REGEX_GENERAL_CATEGORY_ND) {
        return -1;
    }
    const sxml_collation_element_table *root = table->root;
    const sxml_uca_mapping *mapping = find_own_mapping(root, codepoint);
    if (mapping == NULL || mapping->element_count != 1) {
        return -1;
    }
    const uint32_t primary = root->elements[mapping->element_offset].primary;
    if (primary < root->digit_zero_primary || primary > root->digit_zero_primary + 9U) {
        return -1;
    }
    return (int)(primary - root->digit_zero_primary);
}

/* Appends a numeric element: digit zero's primary (shifted, after the
   reordering) plus offset, common secondary and tertiary. */
static sxml_status write_numeric_element(element_writer *writer, uint32_t offset) {
    const uint32_t base = reorder_primary(writer->table, writer->table->root->digit_zero_primary)
                          << 8U;
    const bool is_upper_first = writer->collation->case_first == SXML_COLLATION_CASE_FIRST_UPPER;
    const uint32_t case_rank = is_upper_first ? 2U : 1U;
    const uint32_t common_tertiary = SXML_UCA_COMMON_TERTIARY << 8U;
    sxml_collation_element made = {
        .weights = {base + offset, SXML_UCA_COMMON_SECONDARY << 8U, case_rank,
                    writer->collation->case_first == SXML_COLLATION_CASE_FIRST_OFF
                        ? common_tertiary
                        : (case_rank << 16U) | common_tertiary,
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
    sxml_uca_entry entry;
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
    if (sxml_uca_starts_contraction(table, characters[position.start].codepoint)) {
        size_t index = position.start;
        while (sequence_count < SXML_UCA_MAXIMUM_CONTRACTION_LENGTH &&
               index < position.characters->character_count) {
            positions[sequence_count] = index;
            sequence[sequence_count] = characters[index].codepoint;
            sequence_count += 1;
            index = next_unconsumed(position.characters, index + 1);
        }
        for (size_t length = sequence_count; length >= 2; length -= 1) {
            if (sxml_uca_find_contraction(
                    table, (sxml_uca_codepoints){.codepoints = sequence, .codepoint_count = length},
                    &match.entry)) {
                memcpy(match.codepoints, sequence, length * sizeof(uint32_t));
                match.codepoint_count = length;
                match.is_found = true;
                *end = positions[length - 1] + 1;
                return match;
            }
        }
    }
    match.codepoints[0] = characters[position.start].codepoint;
    match.codepoint_count = 1;
    *end = position.start + 1;
    match.is_found = sxml_uca_find_mapping(table, characters[position.start].codepoint, &match.entry);
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
            sxml_uca_entry entry;
            if (sxml_uca_find_contraction(
                    extension.table,
                    (sxml_uca_codepoints){.codepoints = match->codepoints,
                                          .codepoint_count = match->codepoint_count + 1},
                    &entry)) {
                match->codepoint_count += 1;
                match->entry = entry;
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
            status = write_entry_elements(&writer, match.entry);
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
    .find_tailoring = find_tailoring,
    .apply_settings = apply_settings,
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
