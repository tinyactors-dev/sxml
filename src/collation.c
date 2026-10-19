/*
 * Collations (src/collation_internal.h, docs/DECISIONS.md D97, D116,
 * D213): finding the collation a URI names, comparing strings, collation
 * keys and substring matching (F&O 4.0, section 5.2 "Collations" and
 * section 5.4 "Functions based on substring matching").
 *
 * Golden file: src/unicode.c (growing arrays through an allocator, tables
 * as lookups), src/memory.c (overflow checks).
 *
 * Every collation is computed from the collation elements of a string
 * (UTS #10, section 3.2): five weights, primary, secondary, case, tertiary
 * and quaternary, each zero when the element is ignorable at that level,
 * and the bytes of the input they came from. The UCA collation feature
 * makes the elements of SXML_COLLATION_KIND_UCA (src/collation_uca.c);
 * make_elements here makes the others, one codepoint at a time. The
 * codepoint, Unicode case-insensitive and HTML ASCII case-insensitive
 * collations give primary weights only (the codepoint, mapped, plus one).
 * The UCA fallback gives the weights described in the header. A sort key
 * (UTS #10, section 7.3) lists the nonzero weights of each compared level,
 * three bytes each, big-endian, with three zero bytes between levels, so
 * keys compare as bytes; strings compare by their keys. Substring matching
 * compares collation units, the elements that are not ignorable on every
 * compared level, and accepts a match only when it starts and ends at the
 * boundary of a group of elements (a codepoint's, a contraction's).
 */
#include "collation_internal.h"

#include "function_library_core_internal.h"
#include "optional_feature_internal.h"
#include "regex_internal.h"
#include "sxml/xpath.h"
#include "unicode_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------------ URIs */

/* F&O 4.0, sections 5.2.3, 5.2.5, 5.2.6 and 5.2.4. */
static const sxml_byte_view codepoint_uri = SXML_BYTE_VIEW_INITIALIZER(
    "http://www.w3.org/2005/xpath-functions/collation/codepoint");
static const sxml_byte_view unicode_case_insensitive_uri = SXML_BYTE_VIEW_INITIALIZER(
    "http://www.w3.org/2005/xpath-functions/collation/unicode-case-insensitive");
static const sxml_byte_view html_ascii_case_insensitive_uri = SXML_BYTE_VIEW_INITIALIZER(
    "http://www.w3.org/2005/xpath-functions/collation/html-ascii-case-insensitive");
static const sxml_byte_view uca_uri =
    SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/2013/collation/UCA");

sxml_collation sxml_collation_codepoint(void) {
    return (sxml_collation){.collation_kind = SXML_COLLATION_KIND_CODEPOINT,
                            .strength = SXML_COLLATION_STRENGTH_TERTIARY,
                            .alternate = SXML_COLLATION_ALTERNATE_NON_IGNORABLE,
                            .maximum_variable = SXML_COLLATION_VARIABLE_GROUP_PUNCTUATION,
                            .case_first = SXML_COLLATION_CASE_FIRST_OFF,
                            .has_case_level = false,
                            .is_backwards = false,
                            .is_numeric = false,
                            .is_fallback_allowed = true,
                            .has_unsupported_parameter = false,
                            .explicit_parameters = 0,
                            .version = {.bytes = NULL, .byte_count = 0},
                            .language = {.bytes = NULL, .byte_count = 0},
                            .reordering = {.bytes = NULL, .byte_count = 0},
                            .uca_operations = NULL,
                            .element_table = NULL};
}

sxml_byte_view sxml_collation_codepoint_uri(void) { return codepoint_uri; }

/* One keyword=value parameter of a UCA URI's query. */
typedef struct uca_parameter {
    sxml_byte_view keyword;
    sxml_byte_view value;
} uca_parameter;

/* Whether a view holds the text of a string literal. */
#define SXML_COLLATION_TEXT_IS(view, literal)                                 \
    sxml_byte_view_equals((view), SXML_BYTE_VIEW_FROM_LITERAL(literal))

/* A yes/no value as read: is_valid is false when it is neither. */
typedef struct yes_no_value {
    bool is_valid;
    bool is_yes;
} yes_no_value;

static yes_no_value read_yes_no(sxml_byte_view value) {
    const bool is_yes = SXML_COLLATION_TEXT_IS(value, "yes");
    return (yes_no_value){.is_valid = is_yes || SXML_COLLATION_TEXT_IS(value, "no"),
                          .is_yes = is_yes};
}

/* The strength values of F&O 4.0, section 5.2.4, in order. */
static const sxml_byte_view strength_names[] = {
    SXML_BYTE_VIEW_INITIALIZER("primary"), SXML_BYTE_VIEW_INITIALIZER("secondary"),
    SXML_BYTE_VIEW_INITIALIZER("tertiary"), SXML_BYTE_VIEW_INITIALIZER("quaternary"),
    SXML_BYTE_VIEW_INITIALIZER("identical")};
static const sxml_byte_view strength_numbers[] = {
    SXML_BYTE_VIEW_INITIALIZER("1"), SXML_BYTE_VIEW_INITIALIZER("2"),
    SXML_BYTE_VIEW_INITIALIZER("3"), SXML_BYTE_VIEW_INITIALIZER("4"),
    SXML_BYTE_VIEW_INITIALIZER("5")};

/* The lang values the root collation serves without a tailoring and
   without asking for CLDR data (CLDR has none for en, und and root): BCP
   47 tags compared without case. */
static bool is_root_language(sxml_byte_view value) {
    uint8_t lowered[16];
    if (value.byte_count >= sizeof(lowered)) {
        return false;
    }
    for (size_t byte_index = 0; byte_index < value.byte_count; byte_index += 1) {
        const uint8_t byte = value.bytes[byte_index];
        lowered[byte_index] = byte >= 'A' && byte <= 'Z' ? (uint8_t)(byte + 0x20U) : byte;
    }
    const sxml_byte_view tag = {.bytes = lowered, .byte_count = value.byte_count};
    if (tag.byte_count == 0 || SXML_COLLATION_TEXT_IS(tag, "root") ||
        SXML_COLLATION_TEXT_IS(tag, "und") || SXML_COLLATION_TEXT_IS(tag, "en")) {
        return true;
    }
    /* en with a region, but not en-US-POSIX, which CLDR tailors. */
    return tag.byte_count == 5 && memcmp(tag.bytes, "en-", 3) == 0;
}

/* Reads a value from names; false when it is none of them. */
typedef struct value_choice {
    const char *const *names;
    size_t name_count;
} value_choice;

static bool choose_value(sxml_byte_view value, value_choice choice, size_t *chosen_index) {
    for (size_t name_index = 0; name_index < choice.name_count; name_index += 1) {
        if (sxml_byte_view_equals(value, sxml_byte_view_from_c_string(choice.names[name_index]))) {
            *chosen_index = name_index;
            return true;
        }
    }
    return false;
}

static const char *const alternate_names[] = {"non-ignorable", "shifted", "blanked"};
static const char *const variable_group_names[] = {"space", "punct", "symbol", "currency"};
static const char *const case_first_names[] = {"off", "lower", "upper"};

/* The keywords of F&O 4.0, section 5.2.4, by what they set. */
typedef enum uca_keyword {
    SXML_UCA_KEYWORD_STRENGTH,
    SXML_UCA_KEYWORD_ALTERNATE,
    SXML_UCA_KEYWORD_MAXIMUM_VARIABLE,
    SXML_UCA_KEYWORD_CASE_FIRST,
    SXML_UCA_KEYWORD_CASE_LEVEL,
    SXML_UCA_KEYWORD_BACKWARDS,
    SXML_UCA_KEYWORD_NUMERIC,
    SXML_UCA_KEYWORD_NORMALIZATION,
    SXML_UCA_KEYWORD_LANGUAGE,
    SXML_UCA_KEYWORD_VERSION,
    SXML_UCA_KEYWORD_REORDER,
    SXML_UCA_KEYWORD_UNKNOWN,
} uca_keyword;

static uca_keyword read_keyword(sxml_byte_view keyword) {
    /* The hyphenated spellings are those of earlier drafts. */
    static const char *const keyword_names[] = {
        "strength",  "alternate", "maxVariable",   "caseFirst", "caseLevel", "backwards",
        "numeric",   "normalization", "lang",      "version",   "reorder",
        "max-variable", "case-first", "case-level"};
    static const uca_keyword keyword_values[] = {
        SXML_UCA_KEYWORD_STRENGTH,      SXML_UCA_KEYWORD_ALTERNATE,
        SXML_UCA_KEYWORD_MAXIMUM_VARIABLE, SXML_UCA_KEYWORD_CASE_FIRST,
        SXML_UCA_KEYWORD_CASE_LEVEL,    SXML_UCA_KEYWORD_BACKWARDS,
        SXML_UCA_KEYWORD_NUMERIC,       SXML_UCA_KEYWORD_NORMALIZATION,
        SXML_UCA_KEYWORD_LANGUAGE,      SXML_UCA_KEYWORD_VERSION,
        SXML_UCA_KEYWORD_REORDER,       SXML_UCA_KEYWORD_MAXIMUM_VARIABLE,
        SXML_UCA_KEYWORD_CASE_FIRST,    SXML_UCA_KEYWORD_CASE_LEVEL};
    size_t keyword_index = 0;
    if (choose_value(keyword, (value_choice){.names = keyword_names,
                                             .name_count = sizeof(keyword_names) /
                                                           sizeof(keyword_names[0])},
                     &keyword_index)) {
        return keyword_values[keyword_index];
    }
    return SXML_UCA_KEYWORD_UNKNOWN;
}

/* Applies one parameter. An unknown keyword or value leaves the collation
   as it was and marks it, which fallback=yes allows and fallback=no does
   not (F&O 4.0, section 5.2.4); fallback itself is read by the caller. */
static void apply_parameter(sxml_collation *collation, uca_parameter parameter) {
    const sxml_byte_view value = parameter.value;
    const yes_no_value yes_no = read_yes_no(value);
    size_t chosen_index = 0;
    bool is_honoured = true;
    const uca_keyword keyword = read_keyword(parameter.keyword);
    switch (keyword) {
    case SXML_UCA_KEYWORD_STRENGTH:
        is_honoured = false;
        for (size_t strength_index = 0; strength_index < 5; strength_index += 1) {
            if (sxml_byte_view_equals(value, strength_names[strength_index]) ||
                sxml_byte_view_equals(value, strength_numbers[strength_index])) {
                collation->strength = (sxml_collation_strength)(strength_index + 1);
                is_honoured = true;
            }
        }
        break;
    case SXML_UCA_KEYWORD_ALTERNATE:
        is_honoured = choose_value(value, (value_choice){.names = alternate_names, .name_count = 3},
                                   &chosen_index);
        if (is_honoured) {
            collation->alternate = (sxml_collation_alternate)chosen_index;
        }
        break;
    case SXML_UCA_KEYWORD_MAXIMUM_VARIABLE:
        is_honoured = choose_value(
            value, (value_choice){.names = variable_group_names, .name_count = 4}, &chosen_index);
        if (is_honoured) {
            collation->maximum_variable = (sxml_collation_variable_group)chosen_index;
        }
        break;
    case SXML_UCA_KEYWORD_CASE_FIRST:
        is_honoured = choose_value(value, (value_choice){.names = case_first_names, .name_count = 3},
                                   &chosen_index);
        if (is_honoured) {
            collation->case_first = (sxml_collation_case_first)chosen_index;
        }
        break;
    case SXML_UCA_KEYWORD_CASE_LEVEL:
        is_honoured = yes_no.is_valid;
        collation->has_case_level = is_honoured ? yes_no.is_yes : collation->has_case_level;
        break;
    case SXML_UCA_KEYWORD_BACKWARDS:
        is_honoured = yes_no.is_valid;
        collation->is_backwards = is_honoured ? yes_no.is_yes : collation->is_backwards;
        break;
    case SXML_UCA_KEYWORD_NUMERIC:
        is_honoured = yes_no.is_valid;
        collation->is_numeric = is_honoured ? yes_no.is_yes : collation->is_numeric;
        break;
    case SXML_UCA_KEYWORD_NORMALIZATION:
        /* The UCA kind always normalizes to NFD (UTS #10, step S1.1),
           which gives the same results for every string normalization=no
           is meant for (FCD strings, UTS #10, section 6.5). */
        is_honoured = yes_no.is_valid;
        break;
    case SXML_UCA_KEYWORD_LANGUAGE:
        /* Other languages are tailorings, which sxml_collation_resolve
           looks up (packet 7.15b). */
        collation->language = is_root_language(value)
                                  ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
                                  : value;
        break;
    case SXML_UCA_KEYWORD_VERSION:
        /* Compared with the table's version by sxml_collation_resolve. */
        collation->version = value;
        break;
    case SXML_UCA_KEYWORD_REORDER:
        /* A reordering is a tailoring, which sxml_collation_resolve looks
           up (packet 7.15b). */
        collation->reordering = value;
        break;
    case SXML_UCA_KEYWORD_UNKNOWN:
        is_honoured = false;
        break;
    }
    if (!is_honoured) {
        collation->has_unsupported_parameter = true;
        return;
    }
    /* The parameters a tailoring's settings do not override (UTS #35,
       part 5, section 3.4). */
    static const uint32_t keyword_parameters[] = {
        [SXML_UCA_KEYWORD_STRENGTH] = SXML_COLLATION_PARAMETER_STRENGTH,
        [SXML_UCA_KEYWORD_ALTERNATE] = SXML_COLLATION_PARAMETER_ALTERNATE,
        [SXML_UCA_KEYWORD_MAXIMUM_VARIABLE] = SXML_COLLATION_PARAMETER_MAXIMUM_VARIABLE,
        [SXML_UCA_KEYWORD_CASE_FIRST] = SXML_COLLATION_PARAMETER_CASE_FIRST,
        [SXML_UCA_KEYWORD_CASE_LEVEL] = SXML_COLLATION_PARAMETER_CASE_LEVEL,
        [SXML_UCA_KEYWORD_BACKWARDS] = SXML_COLLATION_PARAMETER_BACKWARDS,
        [SXML_UCA_KEYWORD_NUMERIC] = SXML_COLLATION_PARAMETER_NUMERIC,
        [SXML_UCA_KEYWORD_NORMALIZATION] = 0,
        [SXML_UCA_KEYWORD_LANGUAGE] = 0,
        [SXML_UCA_KEYWORD_VERSION] = 0,
        [SXML_UCA_KEYWORD_REORDER] = 0,
        [SXML_UCA_KEYWORD_UNKNOWN] = 0};
    collation->explicit_parameters |= keyword_parameters[keyword];
}

/* Splits text at the first separator byte: *part before it, *rest after
   it (empty when there is none). */
typedef struct split_text {
    sxml_byte_view part;
    sxml_byte_view rest;
    bool has_separator;
} split_text;

static split_text split_at(sxml_byte_view text, uint8_t separator) {
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        if (text.bytes[byte_index] == separator) {
            return (split_text){
                .part = {.bytes = text.bytes, .byte_count = byte_index},
                .rest = {.bytes = text.bytes + byte_index + 1,
                         .byte_count = text.byte_count - byte_index - 1},
                .has_separator = true};
        }
    }
    return (split_text){.part = text,
                        .rest = {.bytes = NULL, .byte_count = 0},
                        .has_separator = false};
}

/* Reads the query of a UCA URI (F&O 4.0, section 5.2.4): semicolon-
   separated keyword=value pairs, the last one of a keyword winning.
   False for a fallback value other than yes and no. */
static bool read_uca_query(sxml_byte_view query, sxml_collation *collation) {
    sxml_byte_view rest = query;
    while (rest.byte_count > 0) {
        const split_text pair = split_at(rest, ';');
        rest = pair.rest;
        if (pair.part.byte_count == 0) {
            continue;
        }
        const split_text parameter = split_at(pair.part, '=');
        if (!parameter.has_separator) {
            /* Not keyword=value: an unrecognized parameter. */
            collation->has_unsupported_parameter = true;
            continue;
        }
        if (SXML_COLLATION_TEXT_IS(parameter.part, "fallback")) {
            const yes_no_value fallback = read_yes_no(parameter.rest);
            if (!fallback.is_valid) {
                return false;
            }
            collation->is_fallback_allowed = fallback.is_yes;
            continue;
        }
        apply_parameter(collation,
                        (uca_parameter){.keyword = parameter.part, .value = parameter.rest});
    }
    return true;
}

bool sxml_collation_find(sxml_byte_view collation_uri, sxml_collation *collation) {
    sxml_collation found = sxml_collation_codepoint();
    if (sxml_byte_view_equals(collation_uri, codepoint_uri)) {
        *collation = found;
        return true;
    }
    if (sxml_byte_view_equals(collation_uri, unicode_case_insensitive_uri)) {
        found.collation_kind = SXML_COLLATION_KIND_UNICODE_CASE_INSENSITIVE;
        *collation = found;
        return true;
    }
    if (sxml_byte_view_equals(collation_uri, html_ascii_case_insensitive_uri)) {
        found.collation_kind = SXML_COLLATION_KIND_HTML_ASCII_CASE_INSENSITIVE;
        *collation = found;
        return true;
    }
    if (collation_uri.byte_count < uca_uri.byte_count ||
        memcmp(collation_uri.bytes, uca_uri.bytes, uca_uri.byte_count) != 0) {
        return false;
    }
    const sxml_byte_view rest = {.bytes = collation_uri.bytes + uca_uri.byte_count,
                                 .byte_count = collation_uri.byte_count - uca_uri.byte_count};
    if (rest.byte_count > 0 && rest.bytes[0] != '?') {
        return false;
    }
    found.collation_kind = SXML_COLLATION_KIND_UCA_FALLBACK;
    if (rest.byte_count > 0 &&
        !read_uca_query((sxml_byte_view){.bytes = rest.bytes + 1, .byte_count = rest.byte_count - 1},
                        &found)) {
        return false;
    }
    *collation = found;
    return true;
}

bool sxml_collation_is_supported(sxml_byte_view collation_uri) {
    /* Without a processor there is no collation element table: a UCA URI
       with fallback=no is not supported (include/sxml/xpath.h). */
    sxml_collation collation;
    return sxml_collation_find(collation_uri, &collation) && collation.is_fallback_allowed;
}

sxml_status sxml_collation_resolve(const sxml_collation_resolution_request *request,
                                   sxml_collation_resolution *resolution) {
    resolution->is_supported = false;
    resolution->collation = sxml_collation_codepoint();
    sxml_collation collation;
    if (!sxml_collation_find(request->collation_uri, &collation)) {
        return SXML_STATUS_OK;
    }
    if (collation.collation_kind != SXML_COLLATION_KIND_UCA_FALLBACK) {
        resolution->is_supported = true;
        resolution->collation = collation;
        return SXML_STATUS_OK;
    }
    /* The feature's D162 place (D189): without it, the fallback. */
    const sxml_uca_collation_operations *operations = sxml_processor_find_optional_feature(
        request->processor, SXML_OPTIONAL_FEATURE_KIND_UCA_COLLATION);
    const sxml_collation_element_table *table = NULL;
    if (operations != NULL) {
        const sxml_status status = operations->find_table(request->processor, &table);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (table != NULL) {
        collation.collation_kind = SXML_COLLATION_KIND_UCA;
        collation.uca_operations = operations;
        collation.element_table = table;
        if (collation.version.byte_count > 0 &&
            !sxml_byte_view_equals(collation.version, operations->get_version(table))) {
            collation.has_unsupported_parameter = true;
        }
        if (collation.language.byte_count > 0 || collation.reordering.byte_count > 0) {
            /* UTS #35, part 5: the language's tailoring and the
               reordering; the root order when they cannot be honoured. */
            sxml_uca_tailoring tailoring = {.table = table, .is_honoured = false};
            const sxml_status status = operations->find_tailoring(
                &(sxml_uca_tailoring_request){.processor = request->processor,
                                              .table = table,
                                              .language = collation.language,
                                              .reordering = collation.reordering},
                &tailoring);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            collation.element_table = tailoring.table;
            if (!tailoring.is_honoured) {
                collation.has_unsupported_parameter = true;
            }
        }
        operations->apply_settings(collation.element_table, &collation);
    } else if (collation.language.byte_count > 0 || collation.reordering.byte_count > 0) {
        /* The fallback has no tailorings. */
        collation.has_unsupported_parameter = true;
    }
    /* The views were borrowed from the URI for these lookups only. */
    collation.version = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    collation.language = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    collation.reordering = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    /* F&O 4.0, section 5.2.4: with fallback=no, a collation that does not
       honour every parameter (the fallback honours none of UTS #10's
       weights) is not supported. */
    resolution->is_supported =
        collation.is_fallback_allowed ||
        (table != NULL && !collation.has_unsupported_parameter);
    resolution->collation = collation;
    return SXML_STATUS_OK;
}

bool sxml_collation_is_codepoint_order(const sxml_collation *collation) {
    return collation->collation_kind == SXML_COLLATION_KIND_CODEPOINT;
}

/* --------------------------------------------------------- Element arrays */

sxml_status sxml_collation_element_array_append(sxml_collation_element_array *array,
                                                const sxml_collation_element *element) {
    if (array->element_count == array->element_capacity) {
        size_t new_capacity = array->element_capacity < 32 ? 32 : array->element_capacity;
        sxml_status status = SXML_STATUS_OK;
        if (array->element_capacity >= 32) {
            status = sxml_size_multiply(new_capacity, 2, &new_capacity);
        }
        size_t byte_count = 0;
        if (status == SXML_STATUS_OK) {
            status = sxml_size_multiply(new_capacity, sizeof(sxml_collation_element), &byte_count);
        }
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                array->allocator,
                (sxml_allocation_request){.byte_count = byte_count,
                                          .alignment = alignof(sxml_collation_element)},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (array->element_count > 0) {
            memcpy(memory, array->elements, array->element_count * sizeof(sxml_collation_element));
        }
        sxml_allocator_release(array->allocator, array->elements);
        array->elements = memory;
        array->element_capacity = new_capacity;
    }
    array->elements[array->element_count] = *element;
    array->element_count += 1;
    return SXML_STATUS_OK;
}

void sxml_collation_element_array_release(sxml_collation_element_array *array) {
    sxml_allocator_release(array->allocator, array->elements);
    array->elements = NULL;
    array->element_count = 0;
    array->element_capacity = 0;
}

/* ------------------------------------- Elements of the other collations */

/* One element made by make_elements: primary, secondary, tertiary and
   quaternary weights, zero where it is ignorable. */
typedef struct codepoint_element {
    uint32_t weights[4];
} codepoint_element;

/* The secondary weight of a base character, below every mark's. */
#define SXML_COLLATION_COMMON_SECONDARY 1U
/* The quaternary weight of an element that is not variable (UTS #10,
   section 4, "Variable Weighting", shifted: FFFF). */
#define SXML_COLLATION_HIGHEST_QUATERNARY 0xFFFFFFU
/* At most the elements of a full decomposition whose codepoints each
   lower-case to three. */
#define SXML_COLLATION_MAXIMUM_ELEMENT_COUNT                                  \
    ((size_t)SXML_UNICODE_MAXIMUM_DECOMPOSITION_LENGTH * 3U)

/* The elements of one codepoint. */
typedef struct element_buffer {
    codepoint_element elements[SXML_COLLATION_MAXIMUM_ELEMENT_COUNT];
    size_t element_count;
} element_buffer;

static void add_element(element_buffer *buffer, codepoint_element element) {
    if (buffer->element_count < SXML_COLLATION_MAXIMUM_ELEMENT_COUNT) {
        buffer->elements[buffer->element_count] = element;
        buffer->element_count += 1;
    }
}

/* A primary-only element for the codepoint-like collations. */
static void add_primary(element_buffer *buffer, uint32_t codepoint) {
    add_element(buffer, (codepoint_element){.weights = {codepoint + 1U, 0, 0, 0}});
}

/* Whether codepoint is variable under collation (UTS #10, section 4;
   UTS #35, part 5, maxVariable). */
static bool is_variable(const sxml_collation *collation, uint32_t codepoint) {
    if (codepoint >= 0x09U && codepoint <= 0x0DU) {
        return true;
    }
    const uint32_t category_bit = 1U << (uint32_t)sxml_regex_get_general_category(codepoint);
    uint32_t mask = SXML_REGEX_CATEGORY_MASK_Z;
    if (collation->maximum_variable >= SXML_COLLATION_VARIABLE_GROUP_PUNCTUATION) {
        mask |= SXML_REGEX_CATEGORY_MASK_P;
    }
    if (collation->maximum_variable >= SXML_COLLATION_VARIABLE_GROUP_SYMBOL) {
        mask |= (1U << (uint32_t)SXML_REGEX_GENERAL_CATEGORY_SM) |
                (1U << (uint32_t)SXML_REGEX_GENERAL_CATEGORY_SK) |
                (1U << (uint32_t)SXML_REGEX_GENERAL_CATEGORY_SO);
    }
    if (collation->maximum_variable >= SXML_COLLATION_VARIABLE_GROUP_CURRENCY) {
        mask |= 1U << (uint32_t)SXML_REGEX_GENERAL_CATEGORY_SC;
    }
    return (category_bit & mask) != 0;
}

/* A mark (combining class above 0): ignorable on the primary level. */
static codepoint_element mark_element(uint32_t codepoint) {
    return (codepoint_element){
        .weights = {0, SXML_COLLATION_COMMON_SECONDARY + 1U + codepoint, 1U,
                    SXML_COLLATION_HIGHEST_QUATERNARY}};
}

/* What decides the tertiary weight of a base character. */
typedef struct tertiary_traits {
    bool is_upper;
    bool is_compatibility_variant;
} tertiary_traits;

/* Lower case first (caseFirst=lower or off, the default): 1 for lower, 2
   for upper; compatibility variants (a ligature, a full-width form) above
   both. */
static uint32_t tertiary_weight(const sxml_collation *collation, tertiary_traits traits) {
    const bool is_upper_first = collation->case_first == SXML_COLLATION_CASE_FIRST_UPPER;
    const bool is_second = is_upper_first ? !traits.is_upper : traits.is_upper;
    return (is_second ? 2U : 1U) + (traits.is_compatibility_variant ? 2U : 0U);
}

/* The case part of a fallback tertiary weight: 1 or 2. */
static uint32_t case_weight(uint32_t tertiary) { return ((tertiary - 1U) % 2U) + 1U; }

/* The UCA fallback's elements of a base character or mark that its
   decomposition gave. */
typedef struct decomposed_character {
    uint32_t codepoint;
    bool is_compatibility_variant;
} decomposed_character;

static void add_uca_character(const sxml_collation *collation, decomposed_character character,
                              element_buffer *buffer) {
    if (sxml_unicode_get_combining_class(character.codepoint) > 0) {
        add_element(buffer, mark_element(character.codepoint));
        return;
    }
    sxml_case_mapping mapping;
    sxml_function_library_map_case(
        (sxml_case_mapping_request){.codepoint = character.codepoint,
                                    .mapping_kind = SXML_CASE_MAPPING_KIND_LOWER},
        &mapping);
    const bool is_upper =
        !(mapping.codepoint_count == 1 && mapping.codepoints[0] == character.codepoint);
    for (size_t mapped_index = 0; mapped_index < mapping.codepoint_count; mapped_index += 1) {
        const uint32_t mapped = mapping.codepoints[mapped_index];
        if (sxml_unicode_get_combining_class(mapped) > 0) {
            add_element(buffer, mark_element(mapped));
            continue;
        }
        add_element(buffer,
                    (codepoint_element){
                        .weights = {mapped + 1U, SXML_COLLATION_COMMON_SECONDARY,
                                    tertiary_weight(collation,
                                                    (tertiary_traits){
                                                        .is_upper = is_upper,
                                                        .is_compatibility_variant =
                                                            character.is_compatibility_variant}),
                                    SXML_COLLATION_HIGHEST_QUATERNARY}});
    }
}

static bool decompositions_differ(const sxml_unicode_decomposition *first,
                                  const sxml_unicode_decomposition *second) {
    return first->codepoint_count != second->codepoint_count ||
           memcmp(first->codepoints, second->codepoints,
                  first->codepoint_count * sizeof(uint32_t)) != 0;
}

/* The UCA fallback's elements of one codepoint. */
static void add_uca_elements(const sxml_collation *collation, uint32_t codepoint,
                             element_buffer *buffer) {
    if (collation->alternate != SXML_COLLATION_ALTERNATE_NON_IGNORABLE &&
        is_variable(collation, codepoint)) {
        if (collation->alternate == SXML_COLLATION_ALTERNATE_SHIFTED) {
            add_element(buffer, (codepoint_element){.weights = {0, 0, 0, codepoint + 1U}});
        }
        /* Blanked: completely ignorable. */
        return;
    }
    sxml_unicode_decomposition compatibility;
    sxml_unicode_decomposition canonical;
    sxml_unicode_decompose(
        (sxml_unicode_decomposition_request){
            .codepoint = codepoint,
            .decomposition_kind = SXML_UNICODE_DECOMPOSITION_KIND_COMPATIBILITY},
        &compatibility);
    sxml_unicode_decompose(
        (sxml_unicode_decomposition_request){
            .codepoint = codepoint,
            .decomposition_kind = SXML_UNICODE_DECOMPOSITION_KIND_CANONICAL},
        &canonical);
    const bool is_compatibility_variant = decompositions_differ(&compatibility, &canonical);
    for (size_t part_index = 0; part_index < compatibility.codepoint_count; part_index += 1) {
        add_uca_character(collation,
                          (decomposed_character){
                              .codepoint = compatibility.codepoints[part_index],
                              .is_compatibility_variant = is_compatibility_variant},
                          buffer);
    }
}

/* Writes the elements of codepoint under collation (not the UCA kind) to
   *buffer. */
static void make_elements(const sxml_collation *collation, uint32_t codepoint,
                          element_buffer *buffer) {
    buffer->element_count = 0;
    switch (collation->collation_kind) {
    case SXML_COLLATION_KIND_CODEPOINT:
        add_primary(buffer, codepoint);
        return;
    case SXML_COLLATION_KIND_HTML_ASCII_CASE_INSENSITIVE:
        /* F&O 4.0, section 5.2.6: translate A-Z to a-z. */
        add_primary(buffer, codepoint >= 'A' && codepoint <= 'Z' ? codepoint + 0x20U : codepoint);
        return;
    case SXML_COLLATION_KIND_UNICODE_CASE_INSENSITIVE: {
        /* F&O 4.0, section 5.2.5: the codepoints of fn:lower-case. */
        sxml_case_mapping mapping;
        sxml_function_library_map_case(
            (sxml_case_mapping_request){.codepoint = codepoint,
                                        .mapping_kind = SXML_CASE_MAPPING_KIND_LOWER},
            &mapping);
        for (size_t mapped_index = 0; mapped_index < mapping.codepoint_count;
             mapped_index += 1) {
            add_primary(buffer, mapping.codepoints[mapped_index]);
        }
        return;
    }
    case SXML_COLLATION_KIND_UCA_FALLBACK:
    case SXML_COLLATION_KIND_UCA:
        add_uca_elements(collation, codepoint, buffer);
        return;
    }
}

/* Writes the collation elements of request->input to *elements. */
static sxml_status make_string_elements(const sxml_collation_element_request *request,
                                        sxml_collation_element_array *elements) {
    const sxml_collation *collation = request->collation;
    if (collation->collation_kind == SXML_COLLATION_KIND_UCA) {
        return collation->uca_operations->append_elements(request, elements);
    }
    sxml_codepoint_iterator iterator = {.string = request->input, .byte_offset = 0};
    uint32_t codepoint = 0;
    element_buffer buffer;
    sxml_status status = SXML_STATUS_OK;
    size_t byte_offset = 0;
    while (status == SXML_STATUS_OK && sxml_codepoint_iterator_next(&iterator, &codepoint)) {
        make_elements(collation, codepoint, &buffer);
        for (size_t element_index = 0;
             status == SXML_STATUS_OK && element_index < buffer.element_count;
             element_index += 1) {
            const codepoint_element made = buffer.elements[element_index];
            const sxml_collation_element element = {
                .weights = {made.weights[0], made.weights[1],
                            made.weights[0] != 0 && made.weights[2] != 0
                                ? case_weight(made.weights[2])
                                : 0,
                            made.weights[2], made.weights[3]},
                .byte_offset = byte_offset,
                .byte_end = iterator.byte_offset};
            status = sxml_collation_element_array_append(elements, &element);
        }
        byte_offset = iterator.byte_offset;
    }
    return status;
}

/* Whether level is compared under collation. */
static bool is_level_compared(const sxml_collation *collation, sxml_collation_level level) {
    if (collation->collation_kind != SXML_COLLATION_KIND_UCA_FALLBACK &&
        collation->collation_kind != SXML_COLLATION_KIND_UCA) {
        return level == SXML_COLLATION_LEVEL_PRIMARY;
    }
    switch (level) {
    case SXML_COLLATION_LEVEL_PRIMARY:
        return true;
    case SXML_COLLATION_LEVEL_SECONDARY:
        return collation->strength >= SXML_COLLATION_STRENGTH_SECONDARY;
    case SXML_COLLATION_LEVEL_CASE:
        return collation->has_case_level;
    case SXML_COLLATION_LEVEL_TERTIARY:
        return collation->strength >= SXML_COLLATION_STRENGTH_TERTIARY;
    case SXML_COLLATION_LEVEL_QUATERNARY:
        /* Without shifted every quaternary weight is the same. */
        return collation->strength >= SXML_COLLATION_STRENGTH_QUATERNARY &&
               collation->alternate == SXML_COLLATION_ALTERNATE_SHIFTED;
    case SXML_COLLATION_LEVEL_IDENTICAL:
        return collation->strength >= SXML_COLLATION_STRENGTH_IDENTICAL;
    }
    return false;
}

/* ------------------------------------------------------- Collation keys */

#define SXML_COLLATION_WEIGHT_BYTE_COUNT ((size_t)3)

static sxml_status reserve_key(sxml_collation_key *key, size_t additional_byte_count) {
    size_t needed = 0;
    sxml_status status = sxml_size_add(key->byte_count, additional_byte_count, &needed);
    if (status != SXML_STATUS_OK || needed <= key->byte_capacity) {
        return status;
    }
    size_t new_capacity = key->byte_capacity < 64 ? 64 : key->byte_capacity;
    while (new_capacity < needed) {
        status = sxml_size_multiply(new_capacity, 2, &new_capacity);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(
        key->allocator,
        (sxml_allocation_request){.byte_count = new_capacity, .alignment = alignof(uint8_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (key->byte_count > 0) {
        memcpy(memory, key->bytes, key->byte_count);
    }
    sxml_allocator_release(key->allocator, key->bytes);
    key->bytes = memory;
    key->byte_capacity = new_capacity;
    return SXML_STATUS_OK;
}

/* Appends a weight below 2^24 as three bytes, big-endian. */
static sxml_status append_weight(sxml_collation_key *key, uint32_t weight) {
    sxml_status status = reserve_key(key, SXML_COLLATION_WEIGHT_BYTE_COUNT);
    if (status == SXML_STATUS_OK && key->bytes == NULL) {
        /* reserve_key always leaves room for a weight. */
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        key->bytes[key->byte_count] = (uint8_t)(weight >> 16U);
        key->bytes[key->byte_count + 1] = (uint8_t)(weight >> 8U);
        key->bytes[key->byte_count + 2] = (uint8_t)weight;
        key->byte_count += SXML_COLLATION_WEIGHT_BYTE_COUNT;
    }
    return status;
}

/* Reverses the order of the weights from byte_offset to the end
   (backwards=yes, UTS #10, section 7.3). */
static void reverse_weights(sxml_collation_key *key, size_t byte_offset) {
    if (key->bytes == NULL) {
        return;
    }
    size_t low = byte_offset;
    size_t high = key->byte_count;
    while (high - low >= 2 * SXML_COLLATION_WEIGHT_BYTE_COUNT) {
        high -= SXML_COLLATION_WEIGHT_BYTE_COUNT;
        for (size_t byte_index = 0; byte_index < SXML_COLLATION_WEIGHT_BYTE_COUNT;
             byte_index += 1) {
            const uint8_t byte = key->bytes[low + byte_index];
            key->bytes[low + byte_index] = key->bytes[high + byte_index];
            key->bytes[high + byte_index] = byte;
        }
        low += SXML_COLLATION_WEIGHT_BYTE_COUNT;
    }
}

/* One level of a key to append. */
typedef struct level_request {
    const sxml_collation_key_request *key_request;
    const sxml_collation_element_array *elements;
    sxml_collation_level level;
} level_request;

static sxml_status append_level(level_request request, sxml_collation_key *key) {
    const size_t level_offset = key->byte_count;
    sxml_status status = SXML_STATUS_OK;
    if (request.level == SXML_COLLATION_LEVEL_IDENTICAL) {
        /* UTS #10, section 7.4: the codepoints of the NFD form under the
           UCA; the input's own under the fallback. */
        sxml_string identical_input = request.key_request->input;
        sxml_unicode_normalized_text normalized = {
            .allocator = key->allocator, .bytes = NULL, .byte_count = 0};
        if (request.key_request->collation->collation_kind == SXML_COLLATION_KIND_UCA) {
            status = sxml_unicode_normalize(
                &(sxml_unicode_normalization_request){
                    .allocator = key->allocator,
                    .normalization_form = SXML_UNICODE_NORMALIZATION_FORM_NFD,
                    .input = identical_input},
                &normalized);
            if (status == SXML_STATUS_OK) {
                status = sxml_string_from_utf8(sxml_unicode_normalized_text_view(&normalized),
                                               &identical_input);
            }
        }
        sxml_codepoint_iterator iterator = {.string = identical_input, .byte_offset = 0};
        uint32_t codepoint = 0;
        while (status == SXML_STATUS_OK && sxml_codepoint_iterator_next(&iterator, &codepoint)) {
            status = append_weight(key, codepoint + 1U);
        }
        sxml_unicode_normalized_text_release(&normalized);
        return status;
    }
    const sxml_collation_element_array *elements = request.elements;
    for (size_t element_index = 0;
         status == SXML_STATUS_OK && element_index < elements->element_count;
         element_index += 1) {
        const uint32_t weight = elements->elements[element_index].weights[request.level];
        if (weight != 0) {
            status = append_weight(key, weight);
        }
    }
    if (status == SXML_STATUS_OK && request.level == SXML_COLLATION_LEVEL_SECONDARY &&
        request.key_request->collation->is_backwards) {
        reverse_weights(key, level_offset);
    }
    return status;
}

sxml_status sxml_collation_make_key(const sxml_collation_key_request *request,
                                    sxml_collation_key *key) {
    key->byte_count = 0;
    sxml_collation_element_array elements = {.allocator = key->allocator, .elements = NULL};
    sxml_status status = make_string_elements(
        &(sxml_collation_element_request){
            .collation = request->collation, .input = request->input, .allocator = key->allocator},
        &elements);
    bool is_first_level = true;
    for (uint32_t level = 0; status == SXML_STATUS_OK && level <= SXML_COLLATION_LEVEL_IDENTICAL;
         level += 1) {
        if (!is_level_compared(request->collation, (sxml_collation_level)level)) {
            continue;
        }
        if (!is_first_level) {
            /* The level separator, below every weight. */
            status = append_weight(key, 0);
        }
        is_first_level = false;
        if (status == SXML_STATUS_OK) {
            status = append_level((level_request){.key_request = request,
                                                  .elements = &elements,
                                                  .level = (sxml_collation_level)level},
                                  key);
        }
    }
    sxml_collation_element_array_release(&elements);
    if (status != SXML_STATUS_OK) {
        sxml_collation_key_release(key);
    }
    return status;
}

sxml_byte_view sxml_collation_key_view(const sxml_collation_key *key) {
    return (sxml_byte_view){.bytes = key->bytes, .byte_count = key->byte_count};
}

void sxml_collation_key_release(sxml_collation_key *key) {
    sxml_allocator_release(key->allocator, key->bytes);
    key->bytes = NULL;
    key->byte_count = 0;
    key->byte_capacity = 0;
}

/* ------------------------------------------------------------ Comparing */

static sxml_ordering compare_bytes(sxml_byte_view first, sxml_byte_view second) {
    const size_t common = first.byte_count < second.byte_count ? first.byte_count
                                                               : second.byte_count;
    const int difference = common == 0 ? 0 : memcmp(first.bytes, second.bytes, common);
    if (difference != 0) {
        return difference < 0 ? SXML_ORDERING_LESS : SXML_ORDERING_GREATER;
    }
    if (first.byte_count == second.byte_count) {
        return SXML_ORDERING_EQUAL;
    }
    return first.byte_count < second.byte_count ? SXML_ORDERING_LESS : SXML_ORDERING_GREATER;
}

sxml_status sxml_collation_compare(const sxml_collation_comparison *comparison,
                                   sxml_ordering *ordering) {
    if (sxml_collation_is_codepoint_order(comparison->collation)) {
        /* F&O 4.0, section 5.2.3: UTF-8 bytes are in codepoint order. */
        *ordering = sxml_string_compare(comparison->first, comparison->second);
        return SXML_STATUS_OK;
    }
    sxml_collation_key first = {.allocator = comparison->allocator, .bytes = NULL};
    sxml_collation_key second = {.allocator = comparison->allocator, .bytes = NULL};
    sxml_status status = sxml_collation_make_key(
        &(sxml_collation_key_request){.collation = comparison->collation,
                                      .input = comparison->first},
        &first);
    if (status == SXML_STATUS_OK) {
        status = sxml_collation_make_key(
            &(sxml_collation_key_request){.collation = comparison->collation,
                                          .input = comparison->second},
            &second);
    }
    if (status == SXML_STATUS_OK) {
        *ordering = compare_bytes(sxml_collation_key_view(&first), sxml_collation_key_view(&second));
    }
    sxml_collation_key_release(&first);
    sxml_collation_key_release(&second);
    return status;
}

/* ------------------------------------------------------------- Matching */

/* One collation unit: the weights of an element on every compared level
   up to the quaternary (zero on the others), and the bytes it came from.
   The identical level compares the matched bytes themselves. */
typedef struct collation_unit {
    uint32_t weights[SXML_COLLATION_ELEMENT_LEVEL_COUNT];
    size_t byte_offset;
    size_t byte_end;
} collation_unit;

/* The collation units of a string, in allocator memory. */
typedef struct unit_array {
    sxml_allocator allocator;
    collation_unit *units;
    size_t unit_count;
    size_t unit_capacity;
} unit_array;

static sxml_status append_unit(unit_array *array, const collation_unit *unit) {
    if (array->unit_count == array->unit_capacity) {
        size_t new_capacity = array->unit_capacity < 32 ? 32 : array->unit_capacity;
        sxml_status status = SXML_STATUS_OK;
        if (array->unit_capacity >= 32) {
            status = sxml_size_multiply(new_capacity, 2, &new_capacity);
        }
        size_t byte_count = 0;
        if (status == SXML_STATUS_OK) {
            status = sxml_size_multiply(new_capacity, sizeof(collation_unit), &byte_count);
        }
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                array->allocator,
                (sxml_allocation_request){.byte_count = byte_count,
                                          .alignment = alignof(collation_unit)},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (array->unit_count > 0) {
            memcpy(memory, array->units, array->unit_count * sizeof(collation_unit));
        }
        sxml_allocator_release(array->allocator, array->units);
        array->units = memory;
        array->unit_capacity = new_capacity;
    }
    array->units[array->unit_count] = *unit;
    array->unit_count += 1;
    return SXML_STATUS_OK;
}

static void release_units(unit_array *array) {
    sxml_allocator_release(array->allocator, array->units);
    array->units = NULL;
    array->unit_count = 0;
    array->unit_capacity = 0;
}

/* The units of one string. */
typedef struct unit_request {
    const sxml_collation *collation;
    sxml_string input;
} unit_request;

static sxml_status make_units(unit_request request, unit_array *array) {
    sxml_collation_element_array elements = {.allocator = array->allocator, .elements = NULL};
    sxml_status status = make_string_elements(
        &(sxml_collation_element_request){
            .collation = request.collation, .input = request.input, .allocator = array->allocator},
        &elements);
    for (size_t element_index = 0;
         status == SXML_STATUS_OK && element_index < elements.element_count;
         element_index += 1) {
        const sxml_collation_element *element = &elements.elements[element_index];
        collation_unit unit = {
            .weights = {0}, .byte_offset = element->byte_offset, .byte_end = element->byte_end};
        bool is_significant = false;
        for (uint32_t level = 0; level < SXML_COLLATION_ELEMENT_LEVEL_COUNT; level += 1) {
            if (is_level_compared(request.collation, (sxml_collation_level)level)) {
                unit.weights[level] = element->weights[level];
                is_significant = is_significant || unit.weights[level] != 0;
            }
        }
        if (is_significant) {
            status = append_unit(array, &unit);
        }
    }
    sxml_collation_element_array_release(&elements);
    return status;
}

/* The bytes units first to first + count - 1 of input came from: all of
   them, since canonical reordering may put a later codepoint's unit
   first. */
typedef struct unit_range {
    const unit_array *units;
    size_t first;
    size_t count;
} unit_range;

static sxml_collation_match bytes_of_units(unit_range range) {
    const collation_unit *units = range.units->units;
    size_t byte_offset = units[range.first].byte_offset;
    size_t byte_end = units[range.first].byte_end;
    for (size_t unit_index = range.first; unit_index < range.first + range.count;
         unit_index += 1) {
        byte_offset = units[unit_index].byte_offset < byte_offset ? units[unit_index].byte_offset
                                                                  : byte_offset;
        byte_end = units[unit_index].byte_end > byte_end ? units[unit_index].byte_end : byte_end;
    }
    return (sxml_collation_match){
        .is_found = true, .byte_offset = byte_offset, .byte_count = byte_end - byte_offset};
}

/* Whether the units of search match those of input from start_index on,
   starting and ending at boundaries of element groups. */
typedef struct match_candidate {
    const unit_array *input;
    const unit_array *search;
    sxml_string input_text;
    sxml_string search_text;
    bool is_identical_compared;
    size_t start_index;
} match_candidate;

static bool is_match(match_candidate candidate) {
    const collation_unit *input = candidate.input->units;
    const size_t input_count = candidate.input->unit_count;
    const size_t search_count = candidate.search->unit_count;
    const size_t start = candidate.start_index;
    const size_t end = start + search_count;
    if (start > 0 && input[start - 1].byte_offset == input[start].byte_offset) {
        return false;
    }
    if (end < input_count && input[end].byte_offset == input[end - 1].byte_offset) {
        return false;
    }
    for (size_t unit_index = 0; unit_index < search_count; unit_index += 1) {
        if (memcmp(input[start + unit_index].weights,
                   candidate.search->units[unit_index].weights,
                   sizeof(input[0].weights)) != 0) {
            return false;
        }
    }
    if (candidate.is_identical_compared) {
        /* The identical level: the matched bytes are the search's. */
        const sxml_collation_match bytes = bytes_of_units(
            (unit_range){.units = candidate.input, .first = start, .count = search_count});
        return bytes.byte_count == candidate.search_text.utf8.byte_count &&
               memcmp(candidate.input_text.utf8.bytes + bytes.byte_offset,
                      candidate.search_text.utf8.bytes, bytes.byte_count) == 0;
    }
    return true;
}

/* Byte search for the codepoint collation: UTF-8 is self-synchronizing,
   so a byte match is a codepoint match. */
static sxml_collation_match find_bytes(const sxml_collation_match_request *request) {
    const sxml_byte_view input = request->input.utf8;
    const sxml_byte_view search = request->search.utf8;
    sxml_collation_match match = {.is_found = false, .byte_offset = 0, .byte_count = 0};
    if (search.byte_count > input.byte_count) {
        return match;
    }
    const size_t last_offset = input.byte_count - search.byte_count;
    size_t first = 0;
    size_t last = last_offset;
    switch (request->match_kind) {
    case SXML_COLLATION_MATCH_KIND_START:
        last = 0;
        break;
    case SXML_COLLATION_MATCH_KIND_END:
        first = last_offset;
        break;
    case SXML_COLLATION_MATCH_KIND_FIRST:
    case SXML_COLLATION_MATCH_KIND_LAST:
        break;
    }
    const bool is_from_end = request->match_kind == SXML_COLLATION_MATCH_KIND_LAST;
    for (size_t attempt = 0; attempt <= last - first; attempt += 1) {
        const size_t offset = is_from_end ? last - attempt : first + attempt;
        if (search.byte_count == 0 ||
            memcmp(input.bytes + offset, search.bytes, search.byte_count) == 0) {
            return (sxml_collation_match){
                .is_found = true, .byte_offset = offset, .byte_count = search.byte_count};
        }
    }
    return match;
}

sxml_status sxml_collation_find_match(const sxml_collation_match_request *request,
                                      sxml_collation_match *match) {
    if (sxml_collation_is_codepoint_order(request->collation)) {
        *match = find_bytes(request);
        return SXML_STATUS_OK;
    }
    unit_array input = {.allocator = request->allocator, .units = NULL};
    unit_array search = {.allocator = request->allocator, .units = NULL};
    sxml_status status = make_units(
        (unit_request){.collation = request->collation, .input = request->input}, &input);
    if (status == SXML_STATUS_OK) {
        status = make_units(
            (unit_request){.collation = request->collation, .input = request->search}, &search);
    }
    if (status == SXML_STATUS_OK) {
        const bool is_at_end = request->match_kind == SXML_COLLATION_MATCH_KIND_LAST ||
                               request->match_kind == SXML_COLLATION_MATCH_KIND_END;
        sxml_collation_match found = {.is_found = false, .byte_offset = 0, .byte_count = 0};
        if (search.unit_count == 0) {
            found = (sxml_collation_match){
                .is_found = true,
                .byte_offset = is_at_end ? request->input.utf8.byte_count : 0,
                .byte_count = 0};
        } else if (search.unit_count <= input.unit_count) {
            const size_t last_start = input.unit_count - search.unit_count;
            size_t first = 0;
            size_t last = last_start;
            if (request->match_kind == SXML_COLLATION_MATCH_KIND_START) {
                last = 0;
            } else if (request->match_kind == SXML_COLLATION_MATCH_KIND_END) {
                first = last_start;
            }
            const bool is_from_end = request->match_kind == SXML_COLLATION_MATCH_KIND_LAST;
            const bool is_identical_compared =
                is_level_compared(request->collation, SXML_COLLATION_LEVEL_IDENTICAL);
            for (size_t attempt = 0; !found.is_found && attempt <= last - first; attempt += 1) {
                const size_t start = is_from_end ? last - attempt : first + attempt;
                if (is_match((match_candidate){.input = &input,
                                               .search = &search,
                                               .input_text = request->input,
                                               .search_text = request->search,
                                               .is_identical_compared = is_identical_compared,
                                               .start_index = start})) {
                    found = bytes_of_units((unit_range){
                        .units = &input, .first = start, .count = search.unit_count});
                }
            }
        }
        *match = found;
    }
    release_units(&input);
    release_units(&search);
    return status;
}
