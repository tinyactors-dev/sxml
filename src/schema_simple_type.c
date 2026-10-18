/*
 * Compile stage SIMPLE_TYPES: simple type definitions and their facets
 * (src/schema_internal.h, docs/DECISIONS.md D165, D167, D171).
 *
 * Every simple type definition of the schema is checked and registered
 * with sxml_type_registry_register_schema_type in derivation order: the
 * types a definition is made from (its base, item type or member types)
 * first, found on an explicit stack of type indexes (D71), so a cycle is
 * met as a type already on the stack (st-props-correct.2, cos-st-restricts
 * 3.3). Checking one definition (XSD 1.1 Part 1, 3.16.6; Part 2, 4.1.5
 * and 4.3):
 * - its variety from its base (a restriction has its base's), item type
 *   or member types, which are no special types (xs:anySimpleType,
 *   xs:anyAtomicType), lists of atomic or union types without lists, and
 *   not final for the derivation;
 * - each facet applicable to the variety and primitive type, one of each
 *   kind but pattern and enumeration, its value read: counts, keywords,
 *   patterns compiled in the XSD dialect of the regex engine, enumeration
 *   values cast to the base type (so they satisfy the base's facets) and
 *   bounds read in the base's built-in type, all written in canonical
 *   form (types.h, sxml_facet; QNames as EQNames);
 * - each facet against the base's effective facets (its own, the
 *   registered bases' not overlaid, and those built-in types have
 *   implicitly: whiteSpace, fractionDigits 0 of xs:integer, minLength 1
 *   of the built-in lists, explicitTimezone of xs:dateTimeStamp): fixed
 *   facets, the "valid restriction" rule of each facet, and the
 *   consistency of the overlaid set (minLength <= maxLength, the bounds,
 *   fractionDigits <= totalDigits).
 * Casting (src/cast.c, src/cast_facet.c) applies the registered facets
 * to values; the regex engine and the cast engine are the only lexical
 * machinery here.
 *
 * Then the value constraints of attribute declarations and uses are cast
 * to their types (a-props-correct.2, au-props-correct.2); element value
 * constraints are COMPLEX_TYPES' (their type may be complex).
 *
 * A definition that fails is reported and not registered; definitions
 * made from it are then skipped without a report of their own.
 *
 * Golden file: src/cast.c (static errors and a cast step);
 * src/schema_components.c (the store and its reports).
 */
#include "schema_internal.h"

#include "cast_internal.h"
#include "regex_internal.h"
#include "sxml/string.h"
#include "types_internal.h"
#include "xdm_internal.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <string.h>

/* ---------------------------------------------------------- The compiler */

/* What checking the simple types of one compilation uses. */
typedef struct sxml_simple_type_compiler {
    sxml_schema_compilation *compilation;
    sxml_schema_components *components;
    sxml_item_heap *item_heap;
    sxml_type_registry *registry;
    sxml_allocator allocator;
} sxml_simple_type_compiler;

/* An error of a type definition or a facet. */
typedef struct sxml_simple_type_error {
    const char *constraint_name;
    const char *description;
    sxml_schema_position position;
} sxml_simple_type_error;

static sxml_status sxml_simple_type_report(const sxml_simple_type_compiler *compiler,
                                           sxml_simple_type_error error) {
    return sxml_schema_compilation_report(
        compiler->compilation,
        &(sxml_schema_report_request){
            .constraint_name = sxml_byte_view_from_c_string(error.constraint_name),
            .description = sxml_byte_view_from_c_string(error.description),
            .detailed_description = {.bytes = NULL, .byte_count = 0},
            .position = error.position});
}

static sxml_schema_type_definition_index sxml_type_index(uint32_t value) {
    return (sxml_schema_type_definition_index){.value = value};
}

static const sxml_schema_type_definition *
sxml_simple_type_get(const sxml_simple_type_compiler *compiler, uint32_t index) {
    return sxml_schema_get_type_definition(compiler->components, sxml_type_index(index));
}

static sxml_schema_type_definition *
sxml_simple_type_edit(const sxml_simple_type_compiler *compiler, uint32_t index) {
    return sxml_schema_components_edit_entry(
        compiler->components,
        (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_TYPE_DEFINITION, .index = index});
}

static bool sxml_type_definition_is_simple(const sxml_schema_type_definition *definition) {
    return definition->variety != SXML_TYPE_VARIETY_COMPLEX;
}

/* xs:anySimpleType and xs:anyAtomicType: no list item, union member or
   base of a restriction (Part 1, 3.16.6.2; 2.4.4 "special" types). */
static bool sxml_type_index_is_special(uint32_t index) {
    return index == SXML_TYPE_XS_ANY_TYPE || index == SXML_TYPE_XS_ANY_SIMPLE_TYPE ||
           index == SXML_TYPE_XS_ANY_ATOMIC_TYPE;
}

/* ------------------------------------------- What a definition is made of */

/* The base of a definition: base=, else the anonymous base; xs:list and
   xs:union are restrictions of xs:anySimpleType (3.16.2.2). */
static uint32_t sxml_simple_type_base(const sxml_schema_type_definition *definition) {
    if (definition->derivation_method == SXML_SCHEMA_DERIVATION_LIST ||
        definition->derivation_method == SXML_SCHEMA_DERIVATION_UNION) {
        return SXML_TYPE_XS_ANY_SIMPLE_TYPE;
    }
    if (definition->base_type.value != 0) {
        return definition->base_type.value;
    }
    return definition->anonymous_base_type.value;
}

/* The item type of a list made by xs:list. */
static uint32_t sxml_simple_type_item(const sxml_schema_type_definition *definition) {
    return definition->item_type.value != 0 ? definition->item_type.value
                                            : definition->anonymous_item_type.value;
}

/* The member types of a union made by xs:union. */
static sxml_schema_index_view
sxml_simple_type_members(const sxml_simple_type_compiler *compiler,
                         const sxml_schema_type_definition *definition) {
    sxml_schema_list members = definition->member_types.entry_count != 0
                                   ? definition->member_types
                                   : definition->anonymous_member_types;
    return sxml_schema_get_list(compiler->components, members);
}

/* The types index's definition is made from: its base, item type or
   members, in that order; position from 0. */
typedef struct sxml_simple_type_dependency {
    uint32_t type_index;
    size_t position;
} sxml_simple_type_dependency;

static bool sxml_simple_type_dependency_at(const sxml_simple_type_compiler *compiler,
                                           sxml_simple_type_dependency query,
                                           uint32_t *dependency) {
    const sxml_schema_type_definition *definition =
        sxml_simple_type_get(compiler, query.type_index);
    switch (definition->derivation_method) {
    case SXML_SCHEMA_DERIVATION_LIST:
        *dependency = sxml_simple_type_item(definition);
        return query.position == 0;
    case SXML_SCHEMA_DERIVATION_UNION: {
        sxml_schema_index_view members = sxml_simple_type_members(compiler, definition);
        if (query.position >= members.index_count) {
            return false;
        }
        *dependency = members.indexes[query.position];
        return true;
    }
    case SXML_SCHEMA_DERIVATION_NONE:
    case SXML_SCHEMA_DERIVATION_EXTENSION:
    case SXML_SCHEMA_DERIVATION_RESTRICTION:
    case SXML_SCHEMA_DERIVATION_SUBSTITUTION:
        *dependency = sxml_simple_type_base(definition);
        return query.position == 0;
    }
    return false;
}

/* ------------------------------------------------------------ Facets */

/* The constraint names and descriptions of the facets, by sxml_facet_kind
   (Part 2, 4.3, "valid restriction" of each). */
typedef struct sxml_facet_text {
    const char *element_name;
    const char *restriction_constraint;
    const char *restriction_description;
} sxml_facet_text;

static const sxml_facet_text sxml_facet_texts[] = {
    {"length", "length-valid-restriction",
     "a length facet must equal the base type's (XSD 1.1 Part 2, 4.3.1.4)"},
    {"minLength", "minLength-valid-restriction",
     "a minLength facet must not be less than the base type's (XSD 1.1 Part 2, 4.3.2.4)"},
    {"maxLength", "maxLength-valid-restriction",
     "a maxLength facet must not be greater than the base type's (XSD 1.1 Part 2, "
     "4.3.3.4)"},
    {"pattern", "cos-st-restricts.1.3.2",
     "a pattern facet must be a regular expression of XSD 1.1 Part 2, appendix G"},
    {"enumeration", "enumeration-valid-restriction",
     "an enumeration value must be in the value space of the base type (XSD 1.1 Part 2, "
     "4.3.5.5)"},
    {"whiteSpace", "whiteSpace-valid-restriction",
     "a whiteSpace facet must not be less restrictive than the base type's (XSD 1.1 Part "
     "2, 4.3.6.4)"},
    {"maxInclusive", "maxInclusive-valid-restriction",
     "a maxInclusive facet must lie within the base type's bounds (XSD 1.1 Part 2, "
     "4.3.7.4)"},
    {"maxExclusive", "maxExclusive-valid-restriction",
     "a maxExclusive facet must lie within the base type's bounds (XSD 1.1 Part 2, "
     "4.3.8.4)"},
    {"minExclusive", "minExclusive-valid-restriction",
     "a minExclusive facet must lie within the base type's bounds (XSD 1.1 Part 2, "
     "4.3.9.4)"},
    {"minInclusive", "minInclusive-valid-restriction",
     "a minInclusive facet must lie within the base type's bounds (XSD 1.1 Part 2, "
     "4.3.10.4)"},
    {"totalDigits", "totalDigits-valid-restriction",
     "a totalDigits facet must not be greater than the base type's (XSD 1.1 Part 2, "
     "4.3.11.4)"},
    {"fractionDigits", "fractionDigits-valid-restriction",
     "a fractionDigits facet must not be greater than the base type's (XSD 1.1 Part 2, "
     "4.3.12.4)"},
    {"explicitTimezone", "timezone-valid-restriction",
     "an explicitTimezone facet must keep the base type's required or prohibited (XSD "
     "1.1 Part 2, 4.3.14.4)"},
};

#define SXML_FACET_KIND_COUNT ((size_t)SXML_FACET_KIND_EXPLICIT_TIMEZONE + 1U)

static bool sxml_facet_kind_is_bound(sxml_facet_kind kind) {
    return kind == SXML_FACET_KIND_MAX_INCLUSIVE || kind == SXML_FACET_KIND_MAX_EXCLUSIVE ||
           kind == SXML_FACET_KIND_MIN_EXCLUSIVE || kind == SXML_FACET_KIND_MIN_INCLUSIVE;
}

static bool sxml_facet_kind_is_count(sxml_facet_kind kind) {
    return kind == SXML_FACET_KIND_LENGTH || kind == SXML_FACET_KIND_MIN_LENGTH ||
           kind == SXML_FACET_KIND_MAX_LENGTH || kind == SXML_FACET_KIND_TOTAL_DIGITS ||
           kind == SXML_FACET_KIND_FRACTION_DIGITS;
}

/* A facet kind as a bit of a set. */
static uint32_t sxml_facet_bit(sxml_facet_kind kind) { return 1U << (uint32_t)kind; }

#define SXML_FACETS_OF_LENGTH                                                                  \
    (sxml_facet_bit(SXML_FACET_KIND_LENGTH) | sxml_facet_bit(SXML_FACET_KIND_MIN_LENGTH) |     \
     sxml_facet_bit(SXML_FACET_KIND_MAX_LENGTH))
#define SXML_FACETS_OF_BOUNDS                                                                  \
    (sxml_facet_bit(SXML_FACET_KIND_MAX_INCLUSIVE) |                                           \
     sxml_facet_bit(SXML_FACET_KIND_MAX_EXCLUSIVE) |                                           \
     sxml_facet_bit(SXML_FACET_KIND_MIN_EXCLUSIVE) | sxml_facet_bit(SXML_FACET_KIND_MIN_INCLUSIVE))
#define SXML_FACETS_OF_LEXICAL                                                                 \
    (sxml_facet_bit(SXML_FACET_KIND_PATTERN) | sxml_facet_bit(SXML_FACET_KIND_WHITESPACE))

/* Applicable Facets (Part 2, 4.1.5): the facets a type of variety and
   primitive type may have. */
typedef struct sxml_facet_owner {
    sxml_type_variety variety;
    sxml_builtin_type primitive_type;
} sxml_facet_owner;

static uint32_t sxml_applicable_facets(sxml_facet_owner owner) {
    const uint32_t enumeration = sxml_facet_bit(SXML_FACET_KIND_ENUMERATION);
    if (owner.variety == SXML_TYPE_VARIETY_LIST) {
        return SXML_FACETS_OF_LENGTH | SXML_FACETS_OF_LEXICAL | enumeration;
    }
    if (owner.variety == SXML_TYPE_VARIETY_UNION) {
        return sxml_facet_bit(SXML_FACET_KIND_PATTERN) | enumeration;
    }
    if (owner.variety != SXML_TYPE_VARIETY_ATOMIC) {
        return 0;
    }
    switch ((uint32_t)owner.primitive_type) {
    case SXML_TYPE_XS_STRING:
    case SXML_TYPE_XS_HEX_BINARY:
    case SXML_TYPE_XS_BASE64_BINARY:
    case SXML_TYPE_XS_ANY_URI:
    case SXML_TYPE_XS_QNAME:
    case SXML_TYPE_XS_NOTATION:
        return SXML_FACETS_OF_LENGTH | SXML_FACETS_OF_LEXICAL | enumeration;
    case SXML_TYPE_XS_BOOLEAN:
        return SXML_FACETS_OF_LEXICAL;
    case SXML_TYPE_XS_FLOAT:
    case SXML_TYPE_XS_DOUBLE:
    case SXML_TYPE_XS_DURATION:
        return SXML_FACETS_OF_LEXICAL | enumeration | SXML_FACETS_OF_BOUNDS;
    case SXML_TYPE_XS_DECIMAL:
        return SXML_FACETS_OF_LEXICAL | enumeration | SXML_FACETS_OF_BOUNDS |
               sxml_facet_bit(SXML_FACET_KIND_TOTAL_DIGITS) |
               sxml_facet_bit(SXML_FACET_KIND_FRACTION_DIGITS);
    case SXML_TYPE_XS_DATE_TIME:
    case SXML_TYPE_XS_TIME:
    case SXML_TYPE_XS_DATE:
    case SXML_TYPE_XS_G_YEAR_MONTH:
    case SXML_TYPE_XS_G_YEAR:
    case SXML_TYPE_XS_G_MONTH_DAY:
    case SXML_TYPE_XS_G_DAY:
    case SXML_TYPE_XS_G_MONTH:
        return SXML_FACETS_OF_LEXICAL | enumeration | SXML_FACETS_OF_BOUNDS |
               sxml_facet_bit(SXML_FACET_KIND_EXPLICIT_TIMEZONE);
    default:
        return 0;
    }
}

/* One facet of a step, as the stage sees it: canonical value, fixed. */
typedef struct sxml_step_facet {
    bool is_present;
    bool is_fixed;
    /* Of the definition being checked, not of a base. */
    bool is_own;
    sxml_byte_view value;
} sxml_step_facet;

/* The effective facets of a type (Part 1, 3.16.6.4: the base's overlaid
   with the own ones), one per kind; pattern and enumeration are not
   compared and stay absent. */
typedef struct sxml_effective_facets {
    sxml_step_facet facets[SXML_FACET_KIND_COUNT];
    /* The built-in type at the end of the chain of restrictions, and the
       variety. */
    sxml_builtin_type builtin_type;
    sxml_type_variety variety;
} sxml_effective_facets;

static void sxml_effective_facets_put(sxml_effective_facets *effective,
                                      sxml_facet_kind kind, sxml_step_facet facet) {
    if (!effective->facets[kind].is_present && kind != SXML_FACET_KIND_PATTERN &&
        kind != SXML_FACET_KIND_ENUMERATION) {
        effective->facets[kind] = facet;
    }
}

static sxml_step_facet sxml_builtin_facet(const char *value, bool is_fixed) {
    return (sxml_step_facet){.is_present = true,
                             .is_fixed = is_fixed,
                             .is_own = false,
                             .value = sxml_byte_view_from_c_string(value)};
}

static bool sxml_builtin_derives_from(const sxml_simple_type_compiler *compiler,
                                      sxml_builtin_type derived, sxml_builtin_type base) {
    return sxml_type_registry_is_derived_from(
        compiler->registry, (sxml_type_pair){.derived_type = sxml_builtin_type_identifier(derived),
                                             .base_type = sxml_builtin_type_identifier(base)});
}

/* The facets built-in types have implicitly (Part 2, 3.3 and 3.4; 4.1.6:
   whiteSpace collapse, fixed, but for the string types). */
static void sxml_effective_facets_add_builtin(const sxml_simple_type_compiler *compiler,
                                              sxml_effective_facets *effective,
                                              sxml_builtin_type builtin_type) {
    effective->builtin_type = builtin_type;
    if (effective->variety == SXML_TYPE_VARIETY_LIST) {
        sxml_effective_facets_put(effective, SXML_FACET_KIND_WHITESPACE,
                                  sxml_builtin_facet("collapse", true));
        if (builtin_type == SXML_TYPE_XS_NMTOKENS || builtin_type == SXML_TYPE_XS_IDREFS ||
            builtin_type == SXML_TYPE_XS_ENTITIES) {
            sxml_effective_facets_put(effective, SXML_FACET_KIND_MIN_LENGTH,
                                      sxml_builtin_facet("1", false));
        }
        return;
    }
    if (effective->variety != SXML_TYPE_VARIETY_ATOMIC) {
        return;
    }
    if (sxml_builtin_derives_from(compiler, builtin_type, SXML_TYPE_XS_TOKEN)) {
        sxml_effective_facets_put(effective, SXML_FACET_KIND_WHITESPACE,
                                  sxml_builtin_facet("collapse", false));
    } else if (sxml_builtin_derives_from(compiler, builtin_type,
                                         SXML_TYPE_XS_NORMALIZED_STRING)) {
        sxml_effective_facets_put(effective, SXML_FACET_KIND_WHITESPACE,
                                  sxml_builtin_facet("replace", false));
    } else if (builtin_type == SXML_TYPE_XS_STRING) {
        sxml_effective_facets_put(effective, SXML_FACET_KIND_WHITESPACE,
                                  sxml_builtin_facet("preserve", false));
    } else {
        sxml_effective_facets_put(effective, SXML_FACET_KIND_WHITESPACE,
                                  sxml_builtin_facet("collapse", true));
    }
    if (sxml_builtin_derives_from(compiler, builtin_type, SXML_TYPE_XS_INTEGER)) {
        sxml_effective_facets_put(effective, SXML_FACET_KIND_FRACTION_DIGITS,
                                  sxml_builtin_facet("0", true));
    }
    if (builtin_type == SXML_TYPE_XS_DATE_TIME_STAMP) {
        sxml_effective_facets_put(effective, SXML_FACET_KIND_EXPLICIT_TIMEZONE,
                                  sxml_builtin_facet("required", true));
    }
}

/* More restriction steps than any schema derives: a bound on a broken
   store. */
enum { SXML_SIMPLE_TYPE_MAXIMUM_STEP_COUNT = 4096 };

/* The effective facets of a registered type definition: its own, then
   its bases', then the built-in ones. The registered facets are the
   definition's in order, with canonical values. */
static void sxml_effective_facets_of(const sxml_simple_type_compiler *compiler,
                                     uint32_t type_index, sxml_effective_facets *effective) {
    uint32_t current = type_index;
    for (size_t step_index = 0; step_index < SXML_SIMPLE_TYPE_MAXIMUM_STEP_COUNT;
         step_index += 1) {
        const sxml_schema_type_definition *definition = sxml_simple_type_get(compiler, current);
        if (definition->is_builtin) {
            sxml_effective_facets_add_builtin(compiler, effective, (sxml_builtin_type)current);
            return;
        }
        sxml_type_description description;
        if (sxml_type_registry_describe_type(compiler->registry, definition->registered_type,
                                             &description) != SXML_STATUS_OK) {
            return;
        }
        sxml_schema_index_view facet_indexes =
            sxml_schema_get_list(compiler->components, definition->facets);
        for (size_t facet_index = 0;
             facet_index < description.facets.facet_count &&
             facet_index < facet_indexes.index_count;
             facet_index += 1) {
            const sxml_facet *facet = &description.facets.facets[facet_index];
            const sxml_schema_facet *component = sxml_schema_get_facet(
                compiler->components,
                (sxml_schema_facet_index){.value = facet_indexes.indexes[facet_index]});
            sxml_effective_facets_put(effective, facet->facet_kind,
                                      (sxml_step_facet){.is_present = true,
                                                        .is_fixed = component->is_fixed,
                                                        .is_own = false,
                                                        .value = facet->value});
        }
        if (definition->derivation_method != SXML_SCHEMA_DERIVATION_RESTRICTION) {
            /* Made by xs:list or xs:union: a list's whiteSpace is collapse,
               fixed (Part 2, 4.1.2.2). */
            sxml_effective_facets_add_builtin(compiler, effective, SXML_TYPE_XS_ANY_SIMPLE_TYPE);
            return;
        }
        current = sxml_simple_type_base(definition);
    }
}

/* ---------------------------------------------------- Reading values */

/* Reads a non-negative integer facet value; false for none. */
static bool sxml_read_count(sxml_byte_view text, uint64_t *count) {
    size_t start = 0;
    size_t end = text.byte_count;
    while (start < end && sxml_xml_byte_is_whitespace(text.bytes[start])) {
        start += 1;
    }
    while (end > start && sxml_xml_byte_is_whitespace(text.bytes[end - 1])) {
        end -= 1;
    }
    if (start < end && text.bytes[start] == '+') {
        start += 1;
    }
    if (start == end) {
        return false;
    }
    uint64_t number = 0;
    for (size_t byte_index = start; byte_index < end; byte_index += 1) {
        uint8_t byte = text.bytes[byte_index];
        if (byte < '0' || byte > '9') {
            return false;
        }
        number = number > (UINT64_MAX - 9U) / 10U ? UINT64_MAX
                                                   : number * 10U + (uint64_t)(byte - '0');
    }
    *count = number;
    return true;
}

/* The text with XML whitespace around it removed (the facet values'
   whiteSpace collapse, Part 1, 3.16.2). */
static sxml_byte_view sxml_trim(sxml_byte_view text) {
    size_t start = 0;
    size_t end = text.byte_count;
    while (start < end && sxml_xml_byte_is_whitespace(text.bytes[start])) {
        start += 1;
    }
    while (end > start && sxml_xml_byte_is_whitespace(text.bytes[end - 1])) {
        end -= 1;
    }
    return (sxml_byte_view){.bytes = text.bytes + start, .byte_count = end - start};
}

static bool sxml_text_is(sxml_byte_view text, const char *expected) {
    return sxml_byte_view_equals(text, sxml_byte_view_from_c_string(expected));
}

/* Appends the canonical text of an item, an EQName for xs:QName and
   xs:NOTATION (types.h), to *text in the text table. */
typedef struct sxml_canonical_append {
    sxml_item item;
    /* Text so far; empty at first. */
    sxml_schema_text *text;
    bool is_first;
} sxml_canonical_append;

static sxml_status sxml_append_text(const sxml_simple_type_compiler *compiler,
                                    sxml_schema_text *text, sxml_byte_view bytes) {
    /* Runs of the text table are contiguous when appended back to back,
       and nothing else adds text during one facet. */
    sxml_schema_text added = {.byte_offset = 0, .byte_count = 0};
    if (bytes.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    sxml_status status = sxml_schema_components_add_text(compiler->components, bytes, &added);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (text->byte_count == 0) {
        *text = added;
    } else {
        text->byte_count += added.byte_count;
    }
    return SXML_STATUS_OK;
}

static sxml_status sxml_append_canonical(const sxml_simple_type_compiler *compiler,
                                         sxml_canonical_append request) {
    sxml_status status = SXML_STATUS_OK;
    if (!request.is_first) {
        status = sxml_append_text(compiler, request.text, SXML_BYTE_VIEW_FROM_LITERAL(" "));
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_builtin_type primitive_type =
        (sxml_builtin_type)sxml_atomic_item_get_primitive_type(request.item).index;
    if (primitive_type == SXML_TYPE_XS_QNAME || primitive_type == SXML_TYPE_XS_NOTATION) {
        sxml_atomic_datum datum;
        sxml_name_parts parts;
        sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
        const sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(compiler->item_heap);
        status = sxml_atomic_item_get_datum(request.item, &datum);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_name_parts(
                name_pool, datum.datum_details.qname.expanded_name, &parts);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_namespace_uri(name_pool, parts.namespace_uri,
                                                      &namespace_uri);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_append_text(compiler, request.text, SXML_BYTE_VIEW_FROM_LITERAL("Q{"));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_append_text(compiler, request.text, namespace_uri);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_append_text(compiler, request.text, SXML_BYTE_VIEW_FROM_LITERAL("}"));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_append_text(compiler, request.text, parts.local_name);
        }
        return status;
    }
    uint8_t bytes[SXML_CAST_CANONICAL_TEXT_CAPACITY];
    sxml_canonical_text canonical;
    status = sxml_cast_write_canonical_text(
        compiler->item_heap,
        &(sxml_canonical_text_request){.item = request.item,
                                       .output = {.bytes = bytes, .byte_count = sizeof(bytes)}},
        &canonical);
    if (status == SXML_STATUS_CAPACITY_EXCEEDED) {
        void *memory = NULL;
        status = sxml_allocator_allocate(
            compiler->allocator,
            (sxml_allocation_request){.byte_count = canonical.required_byte_count,
                                      .alignment = 1},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        status = sxml_cast_write_canonical_text(
            compiler->item_heap,
            &(sxml_canonical_text_request){
                .item = request.item,
                .output = {.bytes = memory, .byte_count = canonical.required_byte_count}},
            &canonical);
        if (status == SXML_STATUS_OK) {
            status = sxml_append_text(compiler, request.text, canonical.text);
        }
        sxml_allocator_release(compiler->allocator, memory);
        return status;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_append_text(compiler, request.text, canonical.text);
    }
    return status;
}

/* The canonical text of a value (one item or a list's items), added to
   the text table. */
static sxml_status sxml_canonical_value(const sxml_simple_type_compiler *compiler,
                                        sxml_sequence value, sxml_schema_text *text) {
    *text = (sxml_schema_text){.byte_offset = 0, .byte_count = 0};
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(value);
    sxml_status status = SXML_STATUS_OK;
    for (bool is_first = true; status == SXML_STATUS_OK; is_first = false) {
        sxml_iteration_step step;
        status = sxml_sequence_iterator_next(&iterator, NULL, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        status = sxml_append_canonical(
            compiler,
            (sxml_canonical_append){.item = step.item, .text = text, .is_first = is_first});
    }
    return status;
}

/* Text cast to a type with the namespaces of a facet or value
   constraint. */
typedef struct sxml_simple_cast {
    sxml_byte_view text;
    sxml_type_identifier target_type;
    sxml_schema_namespace_context_index namespace_context;
} sxml_simple_cast;

/* Casts; a cast error gives *is_valid false, not a failure. */
typedef struct sxml_simple_cast_outcome {
    bool is_valid;
    sxml_sequence value;
} sxml_simple_cast_outcome;

static sxml_status sxml_simple_type_cast(const sxml_simple_type_compiler *compiler,
                                         sxml_simple_cast cast,
                                         sxml_simple_cast_outcome *outcome) {
    const sxml_schema_namespace_context *context =
        sxml_schema_get_namespace_context(compiler->components, cast.namespace_context);
    sxml_error_record ignored;
    *outcome = (sxml_simple_cast_outcome){.is_valid = false, .value = sxml_sequence_empty()};
    sxml_status status = sxml_cast_make_value_from_text(
        compiler->item_heap,
        &(sxml_text_cast_request){.text = cast.text,
                                  .target_type = cast.target_type,
                                  .namespace_bindings = sxml_schema_get_namespace_bindings(
                                      compiler->components, context->namespace_bindings),
                                  .error_record = &ignored},
        &outcome->value);
    if (status == SXML_STATUS_OK) {
        outcome->is_valid = true;
        return SXML_STATUS_OK;
    }
    if (status == SXML_STATUS_DYNAMIC_ERROR || status == SXML_STATUS_TYPE_ERROR ||
        status == SXML_STATUS_STATIC_ERROR || status == SXML_STATUS_INVALID_ARGUMENT) {
        return SXML_STATUS_OK;
    }
    return status;
}

/* ------------------------------------------------ Checking one definition */

/* One definition under check. */
typedef struct sxml_simple_type_check {
    uint32_t type_index;
    sxml_schema_type_definition definition;
    uint32_t base_index;
    sxml_type_variety variety;
    /* ATOMIC: the base's primitive and built-in types. */
    sxml_builtin_type primitive_type;
    sxml_builtin_type builtin_type;
    /* The facets of the step: kinds, canonical values (in the text table)
       and positions, in document order. */
    sxml_facet *facets;
    sxml_schema_text *canonical_values;
    size_t facet_count;
    sxml_effective_facets base_facets;
    sxml_effective_facets facets_after;
} sxml_simple_type_check;

static sxml_status sxml_check_report(const sxml_simple_type_compiler *compiler,
                                     const sxml_simple_type_check *check,
                                     sxml_simple_type_error error) {
    if (error.position.line_number == 0) {
        error.position = check->definition.position;
    }
    return sxml_simple_type_report(compiler, error);
}

/* The position of facet facet_index of the step. */
static sxml_schema_position sxml_facet_position(const sxml_simple_type_compiler *compiler,
                                                const sxml_simple_type_check *check,
                                                size_t facet_index) {
    sxml_schema_index_view indexes =
        sxml_schema_get_list(compiler->components, check->definition.facets);
    return sxml_schema_get_facet(compiler->components,
                                 (sxml_schema_facet_index){.value = indexes.indexes[facet_index]})
        ->position;
}

/* Reads one facet's value into its canonical text; *is_valid false when
   it is not a value of the facet (reported by the caller). */
typedef struct sxml_facet_reading {
    const sxml_simple_type_check *check;
    const sxml_schema_facet *facet;
    sxml_schema_text *canonical_value;
} sxml_facet_reading;

static sxml_status sxml_read_count_value(const sxml_simple_type_compiler *compiler,
                                         sxml_facet_reading reading, bool *is_valid) {
    uint64_t count = 0;
    *is_valid = sxml_read_count(sxml_schema_get_text(compiler->components, reading.facet->value),
                                &count);
    if (*is_valid && reading.facet->facet_kind == SXML_FACET_KIND_TOTAL_DIGITS && count == 0) {
        /* totalDigits is a positiveInteger (Part 2, 4.3.11.1). */
        *is_valid = false;
    }
    if (!*is_valid) {
        return SXML_STATUS_OK;
    }
    /* The canonical form: decimal digits without leading zeros. */
    uint8_t digits[24];
    size_t digit_offset = sizeof(digits);
    do {
        digit_offset -= 1;
        digits[digit_offset] = (uint8_t)('0' + count % 10U);
        count /= 10U;
    } while (count != 0);
    return sxml_append_text(compiler, reading.canonical_value,
                            (sxml_byte_view){.bytes = digits + digit_offset,
                                             .byte_count = sizeof(digits) - digit_offset});
}

static sxml_status sxml_read_keyword_value(const sxml_simple_type_compiler *compiler,
                                           sxml_facet_reading reading, bool *is_valid) {
    sxml_byte_view value =
        sxml_trim(sxml_schema_get_text(compiler->components, reading.facet->value));
    if (reading.facet->facet_kind == SXML_FACET_KIND_WHITESPACE) {
        *is_valid = sxml_text_is(value, "preserve") || sxml_text_is(value, "replace") ||
                    sxml_text_is(value, "collapse");
    } else {
        *is_valid = sxml_text_is(value, "required") || sxml_text_is(value, "prohibited") ||
                    sxml_text_is(value, "optional");
    }
    if (*is_valid) {
        /* The trimmed run of the value as written (the text table may move
           while it grows, so it is not copied from itself). */
        sxml_byte_view written = sxml_schema_get_text(compiler->components, reading.facet->value);
        *reading.canonical_value = (sxml_schema_text){
            .byte_offset = reading.facet->value.byte_offset +
                           (uint32_t)(value.bytes - written.bytes),
            .byte_count = (uint32_t)value.byte_count};
    }
    return SXML_STATUS_OK;
}

/* A pattern compiles in the XSD dialect (Part 2, appendix G); it is kept
   as written. */
static sxml_status sxml_read_pattern_value(const sxml_simple_type_compiler *compiler,
                                           sxml_facet_reading reading, bool *is_valid) {
    sxml_byte_view value = sxml_schema_get_text(compiler->components, reading.facet->value);
    sxml_string pattern;
    *is_valid = false;
    if (sxml_string_from_utf8(value, &pattern) != SXML_STATUS_OK) {
        return SXML_STATUS_OK;
    }
    sxml_error_record ignored;
    sxml_memory_requirements requirements;
    sxml_status status = sxml_regex_memory_requirements(
        &(sxml_regex_configuration){.regex_text = pattern,
                                    .flags = {.is_xsd_pattern = true},
                                    .maximum_instruction_count = 0,
                                    .maximum_group_depth = 0,
                                    .stack_allocator = compiler->allocator,
                                    .error_record = &ignored},
        &requirements);
    if (status == SXML_STATUS_DYNAMIC_ERROR) {
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *is_valid = true;
    *reading.canonical_value = reading.facet->value;
    return SXML_STATUS_OK;
}

/* An enumeration value is in the base's value space: cast to the base
   (Part 2, 4.3.5.5). A bound is a value of the base's built-in type
   (4.3.7 to 4.3.10), compared with the base's bounds afterwards. */
static sxml_status sxml_read_typed_value(const sxml_simple_type_compiler *compiler,
                                         sxml_facet_reading reading, bool *is_valid) {
    const sxml_simple_type_check *check = reading.check;
    sxml_type_identifier target =
        sxml_simple_type_get(compiler, check->base_index)->registered_type;
    if (sxml_facet_kind_is_bound(reading.facet->facet_kind)) {
        target = sxml_builtin_type_identifier(check->builtin_type);
    } else if (target.index == SXML_TYPE_XS_NOTATION) {
        /* xs:NOTATION cannot be cast to (XPST0080); its enumeration values
           are QNames (Part 2, 3.3.19), written as EQNames either way. */
        target = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME);
    }
    sxml_simple_cast_outcome outcome;
    sxml_status status = sxml_simple_type_cast(
        compiler,
        (sxml_simple_cast){
            .text = sxml_schema_get_text(compiler->components, reading.facet->value),
            .target_type = target,
            .namespace_context = reading.facet->namespace_context},
        &outcome);
    *is_valid = outcome.is_valid;
    if (status != SXML_STATUS_OK || !outcome.is_valid) {
        return status;
    }
    status = sxml_canonical_value(compiler, outcome.value, reading.canonical_value);
    sxml_sequence_release(outcome.value);
    return status;
}

static sxml_status sxml_read_facet_value(const sxml_simple_type_compiler *compiler,
                                         sxml_facet_reading reading, bool *is_valid) {
    sxml_facet_kind kind = reading.facet->facet_kind;
    if (sxml_facet_kind_is_count(kind)) {
        return sxml_read_count_value(compiler, reading, is_valid);
    }
    if (kind == SXML_FACET_KIND_WHITESPACE || kind == SXML_FACET_KIND_EXPLICIT_TIMEZONE) {
        return sxml_read_keyword_value(compiler, reading, is_valid);
    }
    if (kind == SXML_FACET_KIND_PATTERN) {
        return sxml_read_pattern_value(compiler, reading, is_valid);
    }
    return sxml_read_typed_value(compiler, reading, is_valid);
}

/* Reads the step's facets: applicable, one per kind (st-props-correct.4),
   values valid. *is_valid false when one was reported. */
static sxml_status sxml_read_step_facets(const sxml_simple_type_compiler *compiler,
                                         sxml_simple_type_check *check, bool *is_valid) {
    sxml_schema_index_view indexes =
        sxml_schema_get_list(compiler->components, check->definition.facets);
    uint32_t applicable = sxml_applicable_facets(
        (sxml_facet_owner){.variety = check->variety, .primitive_type = check->primitive_type});
    uint32_t seen = 0;
    *is_valid = true;
    for (size_t facet_index = 0; facet_index < indexes.index_count; facet_index += 1) {
        const sxml_schema_facet *facet = sxml_schema_get_facet(
            compiler->components, (sxml_schema_facet_index){.value = indexes.indexes[facet_index]});
        sxml_facet_kind kind = facet->facet_kind;
        sxml_simple_type_error error = {.constraint_name = NULL,
                                        .description = NULL,
                                        .position = facet->position};
        bool is_value_valid = true;
        sxml_status status = SXML_STATUS_OK;
        if ((applicable & sxml_facet_bit(kind)) == 0) {
            error.constraint_name = "cos-applicable-facets";
            error.description = "the facet does not apply to the base type's variety and "
                                "primitive type (XSD 1.1 Part 2, 4.1.5)";
        } else if ((seen & sxml_facet_bit(kind)) != 0 && kind != SXML_FACET_KIND_PATTERN &&
                   kind != SXML_FACET_KIND_ENUMERATION) {
            error.constraint_name = "st-props-correct.4";
            error.description = "a restriction step has at most one facet of each kind (XSD "
                                "1.1 Part 1, 3.16.6.1)";
        } else {
            status = sxml_read_facet_value(
                compiler,
                (sxml_facet_reading){.check = check,
                                     .facet = facet,
                                     .canonical_value = &check->canonical_values[facet_index]},
                &is_value_valid);
            if (status == SXML_STATUS_OK && !is_value_valid) {
                error.constraint_name = sxml_facet_texts[kind].restriction_constraint;
                error.description = kind == SXML_FACET_KIND_ENUMERATION || sxml_facet_kind_is_bound(kind)
                                        ? "the facet's value is not a value of the base type "
                                          "(XSD 1.1 Part 2, 4.3)"
                                        : "the facet's value is not one the facet allows "
                                          "(XSD 1.1 Part 2, 4.3)";
            }
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        seen |= sxml_facet_bit(kind);
        check->facets[facet_index].facet_kind = kind;
        if (error.constraint_name != NULL) {
            *is_valid = false;
            status = sxml_check_report(compiler, check, error);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
    }
    if ((seen & sxml_facet_bit(SXML_FACET_KIND_MAX_INCLUSIVE)) != 0 &&
        (seen & sxml_facet_bit(SXML_FACET_KIND_MAX_EXCLUSIVE)) != 0) {
        *is_valid = false;
        return sxml_check_report(
            compiler, check,
            (sxml_simple_type_error){.constraint_name = "maxInclusive-maxExclusive",
                                     .description = "maxInclusive and maxExclusive in one "
                                                    "derivation step (XSD 1.1 Part 2, 4.3.8.4)"});
    }
    if ((seen & sxml_facet_bit(SXML_FACET_KIND_MIN_INCLUSIVE)) != 0 &&
        (seen & sxml_facet_bit(SXML_FACET_KIND_MIN_EXCLUSIVE)) != 0) {
        *is_valid = false;
        return sxml_check_report(
            compiler, check,
            (sxml_simple_type_error){.constraint_name = "minInclusive-minExclusive",
                                     .description = "minInclusive and minExclusive in one "
                                                    "derivation step (XSD 1.1 Part 2, 4.3.9.4)"});
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------ Facets against the base */

/* Orders two bound values, read in the type's built-in type. */
typedef struct sxml_bound_pair {
    sxml_byte_view first;
    sxml_byte_view second;
} sxml_bound_pair;

static sxml_status sxml_order_bounds(const sxml_simple_type_compiler *compiler,
                                     const sxml_simple_type_check *check, sxml_bound_pair pair,
                                     sxml_value_order *order) {
    sxml_type_identifier builtin = sxml_builtin_type_identifier(check->builtin_type);
    sxml_simple_cast_outcome first;
    sxml_simple_cast_outcome second = {.is_valid = false, .value = sxml_sequence_empty()};
    *order = SXML_VALUE_ORDER_INCOMPARABLE;
    sxml_status status = sxml_simple_type_cast(
        compiler, (sxml_simple_cast){.text = pair.first, .target_type = builtin}, &first);
    if (status == SXML_STATUS_OK && first.is_valid) {
        status = sxml_simple_type_cast(
            compiler, (sxml_simple_cast){.text = pair.second, .target_type = builtin}, &second);
    }
    if (status == SXML_STATUS_OK && first.is_valid && second.is_valid) {
        status = sxml_cast_order_values(
            compiler->item_heap,
            (sxml_item_pair){.first = first.value.content, .second = second.value.content},
            order);
    }
    sxml_sequence_release(first.value);
    sxml_sequence_release(second.value);
    return status;
}

/* What a facet of the step may not be relative to a facet of the base,
   for the bound facets (Part 2, 4.3.7.4 to 4.3.10.4): orders of
   own-to-base that are errors, as bits of sxml_value_order. */
static uint32_t sxml_order_bit(sxml_value_order order) { return 1U << (uint32_t)order; }

typedef struct sxml_bound_rule {
    sxml_facet_kind own_kind;
    sxml_facet_kind base_kind;
    uint32_t error_orders;
} sxml_bound_rule;

#define SXML_ORDERS_ABOVE (1U << SXML_VALUE_ORDER_GREATER)
#define SXML_ORDERS_AT_OR_ABOVE ((1U << SXML_VALUE_ORDER_GREATER) | (1U << SXML_VALUE_ORDER_EQUAL))
#define SXML_ORDERS_BELOW (1U << SXML_VALUE_ORDER_LESS)
#define SXML_ORDERS_AT_OR_BELOW ((1U << SXML_VALUE_ORDER_LESS) | (1U << SXML_VALUE_ORDER_EQUAL))

static const sxml_bound_rule sxml_bound_rules[] = {
    {SXML_FACET_KIND_MAX_INCLUSIVE, SXML_FACET_KIND_MAX_INCLUSIVE, SXML_ORDERS_ABOVE},
    {SXML_FACET_KIND_MAX_INCLUSIVE, SXML_FACET_KIND_MAX_EXCLUSIVE, SXML_ORDERS_AT_OR_ABOVE},
    {SXML_FACET_KIND_MAX_INCLUSIVE, SXML_FACET_KIND_MIN_INCLUSIVE, SXML_ORDERS_BELOW},
    {SXML_FACET_KIND_MAX_INCLUSIVE, SXML_FACET_KIND_MIN_EXCLUSIVE, SXML_ORDERS_AT_OR_BELOW},
    {SXML_FACET_KIND_MAX_EXCLUSIVE, SXML_FACET_KIND_MAX_EXCLUSIVE, SXML_ORDERS_ABOVE},
    {SXML_FACET_KIND_MAX_EXCLUSIVE, SXML_FACET_KIND_MAX_INCLUSIVE, SXML_ORDERS_ABOVE},
    {SXML_FACET_KIND_MAX_EXCLUSIVE, SXML_FACET_KIND_MIN_INCLUSIVE, SXML_ORDERS_AT_OR_BELOW},
    {SXML_FACET_KIND_MAX_EXCLUSIVE, SXML_FACET_KIND_MIN_EXCLUSIVE, SXML_ORDERS_AT_OR_BELOW},
    {SXML_FACET_KIND_MIN_EXCLUSIVE, SXML_FACET_KIND_MIN_EXCLUSIVE, SXML_ORDERS_BELOW},
    {SXML_FACET_KIND_MIN_EXCLUSIVE, SXML_FACET_KIND_MIN_INCLUSIVE, SXML_ORDERS_BELOW},
    {SXML_FACET_KIND_MIN_EXCLUSIVE, SXML_FACET_KIND_MAX_INCLUSIVE, SXML_ORDERS_AT_OR_ABOVE},
    {SXML_FACET_KIND_MIN_EXCLUSIVE, SXML_FACET_KIND_MAX_EXCLUSIVE, SXML_ORDERS_AT_OR_ABOVE},
    {SXML_FACET_KIND_MIN_INCLUSIVE, SXML_FACET_KIND_MIN_INCLUSIVE, SXML_ORDERS_BELOW},
    {SXML_FACET_KIND_MIN_INCLUSIVE, SXML_FACET_KIND_MAX_INCLUSIVE, SXML_ORDERS_ABOVE},
    {SXML_FACET_KIND_MIN_INCLUSIVE, SXML_FACET_KIND_MIN_EXCLUSIVE, SXML_ORDERS_AT_OR_BELOW},
    {SXML_FACET_KIND_MIN_INCLUSIVE, SXML_FACET_KIND_MAX_EXCLUSIVE, SXML_ORDERS_AT_OR_ABOVE},
};

/* The consistency of the overlaid bounds (Part 2, 4.3.7.4 to 4.3.10.4):
   lower to upper orders that are errors. */
typedef struct sxml_bound_consistency {
    sxml_facet_kind lower_kind;
    sxml_facet_kind upper_kind;
    uint32_t error_orders;
    const char *constraint_name;
} sxml_bound_consistency;

static const sxml_bound_consistency sxml_bound_consistencies[] = {
    {SXML_FACET_KIND_MIN_INCLUSIVE, SXML_FACET_KIND_MAX_INCLUSIVE, SXML_ORDERS_ABOVE,
     "minInclusive-less-than-equal-to-maxInclusive"},
    {SXML_FACET_KIND_MIN_INCLUSIVE, SXML_FACET_KIND_MAX_EXCLUSIVE, SXML_ORDERS_AT_OR_ABOVE,
     "minInclusive-less-than-maxExclusive"},
    {SXML_FACET_KIND_MIN_EXCLUSIVE, SXML_FACET_KIND_MAX_INCLUSIVE, SXML_ORDERS_AT_OR_ABOVE,
     "minExclusive-less-than-maxInclusive"},
    {SXML_FACET_KIND_MIN_EXCLUSIVE, SXML_FACET_KIND_MAX_EXCLUSIVE, SXML_ORDERS_ABOVE,
     "minExclusive-less-than-equal-to-maxExclusive"},
};

/* The count value of an effective facet. */
static uint64_t sxml_step_facet_count(sxml_step_facet facet) {
    uint64_t count = 0;
    (void)sxml_read_count(facet.value, &count);
    return count;
}

/* Whether a facet of the step breaks its base's facet of the same kind
   (fixed, and the "valid restriction" rules but those of the bounds). */
static bool sxml_breaks_base_facet(sxml_facet_kind kind, sxml_step_facet own,
                                   sxml_step_facet base) {
    if (!base.is_present) {
        return false;
    }
    uint64_t own_count = sxml_step_facet_count(own);
    uint64_t base_count = sxml_step_facet_count(base);
    switch (kind) {
    case SXML_FACET_KIND_LENGTH:
        return own_count != base_count;
    case SXML_FACET_KIND_MIN_LENGTH:
        return own_count < base_count || (base.is_fixed && own_count != base_count);
    case SXML_FACET_KIND_MAX_LENGTH:
    case SXML_FACET_KIND_TOTAL_DIGITS:
    case SXML_FACET_KIND_FRACTION_DIGITS:
        return own_count > base_count || (base.is_fixed && own_count != base_count);
    case SXML_FACET_KIND_WHITESPACE:
        /* preserve < replace < collapse in restrictiveness (4.3.6.4). */
        return (sxml_text_is(base.value, "collapse") && !sxml_text_is(own.value, "collapse")) ||
               (sxml_text_is(base.value, "replace") && sxml_text_is(own.value, "preserve")) ||
               (base.is_fixed && !sxml_byte_view_equals(own.value, base.value));
    case SXML_FACET_KIND_EXPLICIT_TIMEZONE:
        return (!sxml_text_is(base.value, "optional") || base.is_fixed) &&
               !sxml_byte_view_equals(own.value, base.value);
    case SXML_FACET_KIND_PATTERN:
    case SXML_FACET_KIND_ENUMERATION:
    case SXML_FACET_KIND_MAX_INCLUSIVE:
    case SXML_FACET_KIND_MAX_EXCLUSIVE:
    case SXML_FACET_KIND_MIN_EXCLUSIVE:
    case SXML_FACET_KIND_MIN_INCLUSIVE:
        return false;
    }
    return false;
}

/* The bound rules for one own bound facet against the base's bounds. */
static sxml_status sxml_check_bound_against_base(const sxml_simple_type_compiler *compiler,
                                                 const sxml_simple_type_check *check,
                                                 size_t facet_index, bool *is_valid) {
    sxml_facet_kind kind = check->facets[facet_index].facet_kind;
    sxml_byte_view own =
        sxml_schema_get_text(compiler->components, check->canonical_values[facet_index]);
    for (size_t rule_index = 0; rule_index < sizeof(sxml_bound_rules) / sizeof(sxml_bound_rules[0]);
         rule_index += 1) {
        const sxml_bound_rule *rule = &sxml_bound_rules[rule_index];
        sxml_step_facet base = check->base_facets.facets[rule->base_kind];
        if (rule->own_kind != kind || !base.is_present) {
            continue;
        }
        sxml_value_order order = SXML_VALUE_ORDER_INCOMPARABLE;
        sxml_status status = sxml_order_bounds(
            compiler, check, (sxml_bound_pair){.first = own, .second = base.value}, &order);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        bool is_broken = (rule->error_orders & sxml_order_bit(order)) != 0 ||
                         (rule->base_kind == kind && base.is_fixed &&
                          order != SXML_VALUE_ORDER_EQUAL);
        if (is_broken) {
            *is_valid = false;
            return sxml_check_report(
                compiler, check,
                (sxml_simple_type_error){
                    .constraint_name = sxml_facet_texts[kind].restriction_constraint,
                    .description = sxml_facet_texts[kind].restriction_description,
                    .position = sxml_facet_position(compiler, check, facet_index)});
        }
    }
    return SXML_STATUS_OK;
}

/* The own facets against the base's, then the overlaid set's
   consistency. */
static sxml_status sxml_check_against_base(const sxml_simple_type_compiler *compiler,
                                           sxml_simple_type_check *check, bool *is_valid) {
    check->facets_after = check->base_facets;
    for (size_t facet_index = 0; facet_index < check->facet_count; facet_index += 1) {
        sxml_facet_kind kind = check->facets[facet_index].facet_kind;
        sxml_step_facet own = {
            .is_present = true,
            .is_fixed = false,
            .is_own = true,
            .value = sxml_schema_get_text(compiler->components, check->canonical_values[facet_index])};
        sxml_status status = SXML_STATUS_OK;
        if (sxml_facet_kind_is_bound(kind)) {
            status = sxml_check_bound_against_base(compiler, check, facet_index, is_valid);
        } else if (sxml_breaks_base_facet(kind, own, check->base_facets.facets[kind])) {
            *is_valid = false;
            status = sxml_check_report(
                compiler, check,
                (sxml_simple_type_error){
                    .constraint_name = sxml_facet_texts[kind].restriction_constraint,
                    .description = sxml_facet_texts[kind].restriction_description,
                    .position = sxml_facet_position(compiler, check, facet_index)});
        }
        if (status != SXML_STATUS_OK || !*is_valid) {
            return status;
        }
        if (kind != SXML_FACET_KIND_PATTERN && kind != SXML_FACET_KIND_ENUMERATION) {
            check->facets_after.facets[kind] = own;
        }
    }
    const sxml_step_facet *after = check->facets_after.facets;
    /* Lengths (4.3.1.4, 4.3.2.4) and digits (4.3.12.4). */
    sxml_step_facet length = after[SXML_FACET_KIND_LENGTH];
    sxml_step_facet minimum_length = after[SXML_FACET_KIND_MIN_LENGTH];
    sxml_step_facet maximum_length = after[SXML_FACET_KIND_MAX_LENGTH];
    const char *constraint_name = NULL;
    if (length.is_present && minimum_length.is_present &&
        (sxml_step_facet_count(minimum_length) > sxml_step_facet_count(length) ||
         (length.is_own && minimum_length.is_own))) {
        constraint_name = "length-minLength-maxLength.1";
    } else if (length.is_present && maximum_length.is_present &&
               (sxml_step_facet_count(length) > sxml_step_facet_count(maximum_length) ||
                (length.is_own && maximum_length.is_own))) {
        constraint_name = "length-minLength-maxLength.2";
    } else if (minimum_length.is_present && maximum_length.is_present &&
               sxml_step_facet_count(minimum_length) > sxml_step_facet_count(maximum_length)) {
        constraint_name = "minLength-less-than-equal-to-maxLength";
    } else if (after[SXML_FACET_KIND_FRACTION_DIGITS].is_present &&
               after[SXML_FACET_KIND_TOTAL_DIGITS].is_present &&
               sxml_step_facet_count(after[SXML_FACET_KIND_FRACTION_DIGITS]) >
                   sxml_step_facet_count(after[SXML_FACET_KIND_TOTAL_DIGITS])) {
        constraint_name = "fractionDigits-totalDigits";
    }
    for (size_t rule_index = 0;
         constraint_name == NULL &&
         rule_index < sizeof(sxml_bound_consistencies) / sizeof(sxml_bound_consistencies[0]);
         rule_index += 1) {
        const sxml_bound_consistency *rule = &sxml_bound_consistencies[rule_index];
        sxml_step_facet lower = after[rule->lower_kind];
        sxml_step_facet upper = after[rule->upper_kind];
        if (!lower.is_present || !upper.is_present || (!lower.is_own && !upper.is_own)) {
            continue;
        }
        sxml_value_order order = SXML_VALUE_ORDER_INCOMPARABLE;
        sxml_status status = sxml_order_bounds(
            compiler, check, (sxml_bound_pair){.first = lower.value, .second = upper.value},
            &order);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if ((rule->error_orders & sxml_order_bit(order)) != 0) {
            constraint_name = rule->constraint_name;
        }
    }
    if (constraint_name == NULL) {
        return SXML_STATUS_OK;
    }
    *is_valid = false;
    return sxml_check_report(
        compiler, check,
        (sxml_simple_type_error){.constraint_name = constraint_name,
                                 .description = "the facets of the type contradict each other "
                                                "(XSD 1.1 Part 2, 4.3)"});
}

/* ------------------------------------------------- Variety and members */

/* Whether a union's transitive members include a list (Part 1, 3.16.6.2,
   2.1), by the registry's descriptions, on a bounded stack. */
static bool sxml_union_has_list_member(const sxml_simple_type_compiler *compiler,
                                       sxml_type_identifier union_type) {
    enum { SXML_UNION_STACK_CAPACITY = 64 };
    sxml_type_identifier stack[SXML_UNION_STACK_CAPACITY];
    size_t stack_count = 1;
    stack[0] = union_type;
    while (stack_count != 0) {
        stack_count -= 1;
        sxml_type_description description;
        if (sxml_type_registry_describe_type(compiler->registry, stack[stack_count],
                                             &description) != SXML_STATUS_OK) {
            return true;
        }
        if (description.variety == SXML_TYPE_VARIETY_LIST) {
            return true;
        }
        if (description.variety != SXML_TYPE_VARIETY_UNION) {
            continue;
        }
        sxml_type_identifier_view members = description.variety_details.member_types;
        for (size_t member_index = 0; member_index < members.type_identifier_count;
             member_index += 1) {
            if (stack_count == SXML_UNION_STACK_CAPACITY) {
                return true;
            }
            stack[stack_count] = members.type_identifiers[member_index];
            stack_count += 1;
        }
    }
    return false;
}

/* The variety of the definition from what it is made of, and the checks
   of Part 1, 3.16.6.1 and 3.16.6.2 that need no facets. *is_valid false
   when one was reported; *is_skipped when a type it is made of failed
   (already reported). */
typedef struct sxml_variety_outcome {
    bool is_valid;
    bool is_skipped;
} sxml_variety_outcome;

static sxml_status sxml_check_made_of(const sxml_simple_type_compiler *compiler,
                                      sxml_simple_type_check *check,
                                      sxml_variety_outcome *outcome) {
    const sxml_schema_type_definition *definition = &check->definition;
    sxml_simple_type_error error = {.constraint_name = NULL,
                                    .description = NULL,
                                    .position = definition->position};
    *outcome = (sxml_variety_outcome){.is_valid = true, .is_skipped = false};
    check->base_index = sxml_simple_type_base(definition);
    const sxml_schema_type_definition *base = sxml_simple_type_get(compiler, check->base_index);
    if (definition->derivation_method == SXML_SCHEMA_DERIVATION_LIST) {
        uint32_t item_index = sxml_simple_type_item(definition);
        const sxml_schema_type_definition *item = sxml_simple_type_get(compiler, item_index);
        check->variety = SXML_TYPE_VARIETY_LIST;
        if (item_index == 0 || (item->registered_type.index == SXML_TYPE_NONE &&
                                sxml_type_definition_is_simple(item))) {
            outcome->is_skipped = true;
            return SXML_STATUS_OK;
        }
        if (!sxml_type_definition_is_simple(item) || sxml_type_index_is_special(item_index) ||
            (item->variety != SXML_TYPE_VARIETY_ATOMIC &&
             item->variety != SXML_TYPE_VARIETY_UNION) ||
            (item->variety == SXML_TYPE_VARIETY_UNION &&
             sxml_union_has_list_member(compiler, item->registered_type))) {
            error.constraint_name = "cos-st-restricts.2.1";
            error.description = "the item type of a list must be an atomic type or a union "
                                "without lists (XSD 1.1 Part 1, 3.16.6.2)";
        } else if ((item->final_derivations & SXML_SCHEMA_DERIVATION_LIST) != 0) {
            error.constraint_name = "cos-st-restricts.2.2.1.1";
            error.description = "the item type's final contains list (XSD 1.1 Part 1, 3.16.6.2)";
        }
    } else if (definition->derivation_method == SXML_SCHEMA_DERIVATION_UNION) {
        sxml_schema_index_view members = sxml_simple_type_members(compiler, definition);
        check->variety = SXML_TYPE_VARIETY_UNION;
        for (size_t member_index = 0;
             error.constraint_name == NULL && member_index < members.index_count;
             member_index += 1) {
            uint32_t member_type = members.indexes[member_index];
            const sxml_schema_type_definition *member = sxml_simple_type_get(compiler, member_type);
            if (member_type == 0 || (member->registered_type.index == SXML_TYPE_NONE &&
                                     sxml_type_definition_is_simple(member))) {
                outcome->is_skipped = true;
                return SXML_STATUS_OK;
            }
            if (!sxml_type_definition_is_simple(member) || sxml_type_index_is_special(member_type)) {
                error.constraint_name = "cos-st-restricts.3.1";
                error.description = "a member type of a union must be a simple type other than "
                                    "a special one (XSD 1.1 Part 1, 3.16.6.2)";
            } else if ((member->final_derivations & SXML_SCHEMA_DERIVATION_UNION) != 0) {
                error.constraint_name = "cos-st-restricts.3.2.1.1";
                error.description = "a member type's final contains union (XSD 1.1 Part 1, "
                                    "3.16.6.2)";
            }
        }
    } else {
        if (check->base_index == 0 || (base->registered_type.index == SXML_TYPE_NONE &&
                                       sxml_type_definition_is_simple(base))) {
            outcome->is_skipped = true;
            return SXML_STATUS_OK;
        }
        check->variety = base->variety;
        if (!sxml_type_definition_is_simple(base) || sxml_type_index_is_special(check->base_index)) {
            error.constraint_name = "cos-st-restricts.1.1";
            error.description = "the base of a restriction must be a simple type other than "
                                "xs:anySimpleType and xs:anyAtomicType (XSD 1.1 Part 1, "
                                "3.16.6.2)";
        } else if ((base->final_derivations & SXML_SCHEMA_DERIVATION_RESTRICTION) != 0) {
            error.constraint_name = "st-props-correct.3";
            error.description = "the base type's final contains restriction (XSD 1.1 Part 1, "
                                "3.16.6.1)";
        } else if (check->variety == SXML_TYPE_VARIETY_ATOMIC) {
            sxml_atomic_target target;
            if (sxml_cast_resolve_atomic_target(compiler->registry, base->registered_type,
                                                &target) != SXML_STATUS_OK) {
                return SXML_STATUS_INVALID_ARGUMENT;
            }
            check->primitive_type = target.primitive_type;
            check->builtin_type = target.builtin_type;
        }
    }
    if (error.constraint_name == NULL) {
        return SXML_STATUS_OK;
    }
    outcome->is_valid = false;
    return sxml_simple_type_report(compiler, error);
}

/* ----------------------------------------------------------- Registering */

/* The effective assertions: the base's, then the own (Part 2, 4.3.13). */
static sxml_status sxml_combine_assertions(const sxml_simple_type_compiler *compiler,
                                           const sxml_simple_type_check *check,
                                           sxml_schema_list *assertions) {
    const sxml_schema_type_definition *base = sxml_simple_type_get(compiler, check->base_index);
    *assertions = check->definition.assertions;
    if (check->definition.derivation_method != SXML_SCHEMA_DERIVATION_RESTRICTION ||
        base->assertions.entry_count == 0) {
        return SXML_STATUS_OK;
    }
    if (check->definition.assertions.entry_count == 0) {
        *assertions = base->assertions;
        return SXML_STATUS_OK;
    }
    size_t count = (size_t)base->assertions.entry_count + check->definition.assertions.entry_count;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        compiler->allocator,
        (sxml_allocation_request){.byte_count = count * sizeof(uint32_t),
                                  .alignment = alignof(uint32_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t *entries = memory;
    sxml_schema_index_view base_entries = sxml_schema_get_list(compiler->components, base->assertions);
    sxml_schema_index_view own_entries =
        sxml_schema_get_list(compiler->components, check->definition.assertions);
    memcpy(entries, base_entries.indexes, base_entries.index_count * sizeof(uint32_t));
    memcpy(entries + base_entries.index_count, own_entries.indexes,
           own_entries.index_count * sizeof(uint32_t));
    status = sxml_schema_components_add_list(
        compiler->components, (sxml_schema_index_view){.indexes = entries, .index_count = count},
        assertions);
    sxml_allocator_release(compiler->allocator, memory);
    return status;
}

/* The registered identifiers of a union's members, in compiler memory
   the caller releases. */
static sxml_status sxml_member_identifiers(const sxml_simple_type_compiler *compiler,
                                           const sxml_simple_type_check *check,
                                           sxml_type_identifier_view *members) {
    sxml_schema_index_view indexes = sxml_simple_type_members(compiler, &check->definition);
    *members = (sxml_type_identifier_view){.type_identifiers = NULL, .type_identifier_count = 0};
    if (indexes.index_count == 0) {
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        compiler->allocator,
        (sxml_allocation_request){.byte_count = indexes.index_count * sizeof(sxml_type_identifier),
                                  .alignment = alignof(sxml_type_identifier)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_type_identifier *identifiers = memory;
    for (size_t member_index = 0; member_index < indexes.index_count; member_index += 1) {
        identifiers[member_index] =
            sxml_simple_type_get(compiler, indexes.indexes[member_index])->registered_type;
    }
    *members = (sxml_type_identifier_view){.type_identifiers = identifiers,
                                           .type_identifier_count = indexes.index_count};
    return SXML_STATUS_OK;
}

/* Registers the checked definition and writes its properties. */
static sxml_status sxml_register_simple_type(const sxml_simple_type_compiler *compiler,
                                             sxml_simple_type_check *check) {
    const sxml_schema_type_definition *base = sxml_simple_type_get(compiler, check->base_index);
    sxml_type_description description = {
        .variety = check->variety,
        .type_name = check->definition.type_name,
        .base_type = base->registered_type,
        .facets = {.facets = check->facets, .facet_count = check->facet_count},
    };
    for (size_t facet_index = 0; facet_index < check->facet_count; facet_index += 1) {
        check->facets[facet_index].value =
            sxml_schema_get_text(compiler->components, check->canonical_values[facet_index]);
    }
    sxml_type_identifier_view members = {.type_identifiers = NULL, .type_identifier_count = 0};
    sxml_status status = SXML_STATUS_OK;
    sxml_type_description base_description;
    if (check->definition.derivation_method == SXML_SCHEMA_DERIVATION_LIST) {
        description.variety_details.list_item_type =
            sxml_simple_type_get(compiler, sxml_simple_type_item(&check->definition))->registered_type;
    } else if (check->definition.derivation_method == SXML_SCHEMA_DERIVATION_UNION) {
        status = sxml_member_identifiers(compiler, check, &members);
        description.variety_details.member_types = members;
    } else if (check->variety != SXML_TYPE_VARIETY_ATOMIC) {
        /* A restriction of a list or union keeps its item or member types
           (Part 2, 4.1.2.1). */
        status = sxml_type_registry_describe_type(compiler->registry, base->registered_type,
                                                  &base_description);
        if (status == SXML_STATUS_OK) {
            description.variety_details = base_description.variety_details;
        }
    }
    sxml_type_identifier registered = {.index = SXML_TYPE_NONE};
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_register_schema_type(compiler->registry, &description,
                                                         &registered);
    }
    if (members.type_identifiers != NULL) {
        sxml_allocator_release(compiler->allocator, (void *)members.type_identifiers);
    }
    sxml_schema_list assertions = {.first_entry_index = 0, .entry_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_combine_assertions(compiler, check, &assertions);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_schema_type_definition *entry = sxml_simple_type_edit(compiler, check->type_index);
    if (entry == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    entry->variety = check->variety;
    entry->registered_type = registered;
    entry->assertions = assertions;
    if (entry->base_type.value == 0) {
        entry->base_type = sxml_type_index(check->base_index);
    }
    if (check->variety == SXML_TYPE_VARIETY_ATOMIC) {
        /* Built-in components have their type's index (D167). */
        entry->primitive_type = sxml_type_index((uint32_t)check->primitive_type);
    }
    if (check->definition.derivation_method == SXML_SCHEMA_DERIVATION_RESTRICTION) {
        entry->item_type = base->item_type;
        entry->member_types = base->member_types;
    } else if (check->definition.derivation_method == SXML_SCHEMA_DERIVATION_LIST) {
        entry->item_type = sxml_type_index(sxml_simple_type_item(&check->definition));
    } else if (entry->member_types.entry_count == 0) {
        entry->member_types = check->definition.anonymous_member_types;
    }
    return SXML_STATUS_OK;
}

/* Allocates the step's facet arrays. */
static sxml_status sxml_allocate_step_facets(const sxml_simple_type_compiler *compiler,
                                             sxml_simple_type_check *check) {
    check->facet_count = check->definition.facets.entry_count;
    check->facets = NULL;
    check->canonical_values = NULL;
    if (check->facet_count == 0) {
        return SXML_STATUS_OK;
    }
    size_t entry_byte_count = sizeof(sxml_facet) + sizeof(sxml_schema_text);
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        compiler->allocator,
        (sxml_allocation_request){.byte_count = check->facet_count * entry_byte_count,
                                  .alignment = alignof(sxml_facet)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(memory, 0, check->facet_count * entry_byte_count);
    check->facets = memory;
    check->canonical_values = (sxml_schema_text *)(void *)(check->facets + check->facet_count);
    return SXML_STATUS_OK;
}

/* Checks and registers one definition whose dependencies are done. */
static sxml_status sxml_compile_one(const sxml_simple_type_compiler *compiler,
                                    uint32_t type_index) {
    sxml_simple_type_check check;
    memset(&check, 0, sizeof(check));
    check.type_index = type_index;
    check.definition = *sxml_simple_type_get(compiler, type_index);
    if (check.definition.is_builtin ||
        check.definition.registered_type.index != SXML_TYPE_NONE ||
        !sxml_type_definition_is_simple(&check.definition)) {
        return SXML_STATUS_OK;
    }
    sxml_variety_outcome made_of;
    sxml_status status = sxml_check_made_of(compiler, &check, &made_of);
    if (status != SXML_STATUS_OK || !made_of.is_valid || made_of.is_skipped) {
        return status;
    }
    status = sxml_allocate_step_facets(compiler, &check);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    bool is_valid = true;
    if (check.facet_count != 0 &&
        check.definition.derivation_method != SXML_SCHEMA_DERIVATION_RESTRICTION) {
        /* xs:list and xs:union have no facets of their own (3.16.6.2,
           2.2.1.2 and 3.2.1.2). */
        is_valid = false;
        status = sxml_check_report(
            compiler, &check,
            (sxml_simple_type_error){.constraint_name = "cos-st-restricts.2.2.1.2",
                                     .description = "a list or union made from its item or "
                                                    "member types has no facets (XSD 1.1 Part "
                                                    "1, 3.16.6.2)"});
    }
    if (status == SXML_STATUS_OK && is_valid) {
        status = sxml_read_step_facets(compiler, &check, &is_valid);
    }
    if (status == SXML_STATUS_OK && is_valid) {
        check.base_facets.variety = check.variety;
        if (check.definition.derivation_method == SXML_SCHEMA_DERIVATION_RESTRICTION) {
            sxml_effective_facets_of(compiler, check.base_index, &check.base_facets);
        } else {
            sxml_effective_facets_add_builtin(compiler, &check.base_facets,
                                              SXML_TYPE_XS_ANY_SIMPLE_TYPE);
        }
        status = sxml_check_against_base(compiler, &check, &is_valid);
    }
    if (status == SXML_STATUS_OK && is_valid) {
        status = sxml_register_simple_type(compiler, &check);
    }
    if (check.facets != NULL) {
        sxml_allocator_release(compiler->allocator, check.facets);
    }
    return status;
}

/* ------------------------------------------------------ Derivation order */

/* The state of each type definition in the walk. */
enum {
    SXML_SIMPLE_TYPE_UNVISITED = 0,
    SXML_SIMPLE_TYPE_ON_STACK,
    SXML_SIMPLE_TYPE_DONE,
};

/* A type on the walk's stack and its next dependency. */
typedef struct sxml_simple_type_frame {
    uint32_t type_index;
    size_t next_dependency;
} sxml_simple_type_frame;

/* The walk's memory: a state per type definition and the stack. */
typedef struct sxml_simple_type_walk {
    uint8_t *states;
    sxml_simple_type_frame *frames;
    size_t frame_count;
    uint32_t type_count;
} sxml_simple_type_walk;

static bool sxml_needs_compiling(const sxml_simple_type_compiler *compiler, uint32_t type_index) {
    const sxml_schema_type_definition *definition = sxml_simple_type_get(compiler, type_index);
    return type_index != 0 && !definition->is_builtin &&
           definition->registered_type.index == SXML_TYPE_NONE &&
           sxml_type_definition_is_simple(definition);
}

/* Compiles type_index after the types it is made of, depth first on the
   walk's stack; a type met again while on the stack is circular. */
static sxml_status sxml_walk_from(const sxml_simple_type_compiler *compiler,
                                  sxml_simple_type_walk *walk, uint32_t type_index) {
    walk->frames[0] = (sxml_simple_type_frame){.type_index = type_index, .next_dependency = 0};
    walk->frame_count = 1;
    walk->states[type_index] = SXML_SIMPLE_TYPE_ON_STACK;
    while (walk->frame_count != 0) {
        sxml_simple_type_frame *top = &walk->frames[walk->frame_count - 1];
        uint32_t dependency = 0;
        if (sxml_simple_type_dependency_at(
                compiler,
                (sxml_simple_type_dependency){.type_index = top->type_index,
                                              .position = top->next_dependency},
                &dependency)) {
            top->next_dependency += 1;
            if (dependency >= walk->type_count || !sxml_needs_compiling(compiler, dependency) ||
                walk->states[dependency] == SXML_SIMPLE_TYPE_DONE) {
                continue;
            }
            if (walk->states[dependency] == SXML_SIMPLE_TYPE_ON_STACK) {
                /* Reported once, at the type that closes the cycle; the
                   types on it stay unregistered. */
                walk->states[top->type_index] = SXML_SIMPLE_TYPE_DONE;
                walk->frame_count -= 1;
                sxml_status status = sxml_simple_type_report(
                    compiler,
                    (sxml_simple_type_error){
                        .constraint_name = "st-props-correct.2",
                        .description = "a simple type is derived from itself, or is a member "
                                       "of its own union (XSD 1.1 Part 1, 3.16.6.1, 3.16.6.2)",
                        .position = sxml_simple_type_get(compiler, top->type_index)->position});
                if (status != SXML_STATUS_OK) {
                    return status;
                }
                continue;
            }
            walk->states[dependency] = SXML_SIMPLE_TYPE_ON_STACK;
            walk->frames[walk->frame_count] =
                (sxml_simple_type_frame){.type_index = dependency, .next_dependency = 0};
            walk->frame_count += 1;
            continue;
        }
        uint32_t done = top->type_index;
        walk->frame_count -= 1;
        if (walk->states[done] == SXML_SIMPLE_TYPE_DONE) {
            continue;
        }
        walk->states[done] = SXML_SIMPLE_TYPE_DONE;
        sxml_status status = sxml_compile_one(compiler, done);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------- Value constraints */

/* A value constraint and the type and position it is checked with. */
typedef struct sxml_value_constraint_check {
    sxml_schema_value_constraint value_constraint;
    uint32_t type_index;
    sxml_schema_position position;
    const char *constraint_name;
} sxml_value_constraint_check;

/* Casts a value constraint to its type; *value owned on success. */
static sxml_status sxml_cast_value_constraint(const sxml_simple_type_compiler *compiler,
                                              sxml_value_constraint_check check,
                                              sxml_simple_cast_outcome *outcome) {
    return sxml_simple_type_cast(
        compiler,
        (sxml_simple_cast){
            .text = sxml_schema_get_text(compiler->components, check.value_constraint.lexical_form),
            .target_type = sxml_simple_type_get(compiler, check.type_index)->registered_type,
            .namespace_context = check.value_constraint.namespace_context},
        outcome);
}

/* Simple Default Valid (Part 1, 3.2.6.2): the value constraint is a value
   of a registered simple type. */
static sxml_status sxml_check_value_constraint(const sxml_simple_type_compiler *compiler,
                                               sxml_value_constraint_check check) {
    const sxml_schema_type_definition *type = sxml_simple_type_get(compiler, check.type_index);
    if (check.value_constraint.variety == SXML_SCHEMA_VALUE_CONSTRAINT_NONE ||
        !sxml_type_definition_is_simple(type) || type->registered_type.index == SXML_TYPE_NONE) {
        return SXML_STATUS_OK;
    }
    sxml_simple_cast_outcome outcome;
    sxml_status status = sxml_cast_value_constraint(compiler, check, &outcome);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (outcome.is_valid) {
        sxml_sequence_release(outcome.value);
        return SXML_STATUS_OK;
    }
    return sxml_simple_type_report(
        compiler, (sxml_simple_type_error){.constraint_name = check.constraint_name,
                                           .description = "the default or fixed value is not "
                                                          "a value of the type (XSD 1.1 Part "
                                                          "1, 3.2.6.2)",
                                           .position = check.position});
}

/* Whether two values are equal item by item (Part 1, 3.5.6.1, 2). */
static sxml_status sxml_values_equal(sxml_sequence first, sxml_sequence second,
                                     bool *is_equal) {
    uint64_t first_count = 0;
    uint64_t second_count = 0;
    sxml_status status = sxml_sequence_count(first, NULL, &first_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_count(second, NULL, &second_count);
    }
    *is_equal = status == SXML_STATUS_OK && first_count == second_count;
    for (uint64_t item_index = 0; status == SXML_STATUS_OK && *is_equal && item_index < first_count;
         item_index += 1) {
        sxml_item first_item;
        sxml_item second_item;
        status = sxml_sequence_get_item(
            first, (sxml_item_request){.item_index = item_index, .error_record = NULL}, &first_item);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_get_item(
                second, (sxml_item_request){.item_index = item_index, .error_record = NULL},
                &second_item);
        }
        if (status == SXML_STATUS_OK) {
            *is_equal = sxml_atomic_item_is_atomic_equal(first_item, second_item);
        }
    }
    return status;
}

/* au-props-correct.2: a use of a declaration with a fixed value is fixed
   with an equal value. */
static sxml_status sxml_check_attribute_use(const sxml_simple_type_compiler *compiler,
                                            const sxml_schema_attribute_use *use) {
    const sxml_schema_attribute_declaration *declaration =
        sxml_schema_get_attribute_declaration(compiler->components, use->attribute_declaration);
    sxml_value_constraint_check own = {.value_constraint = use->value_constraint,
                                       .type_index = declaration->type_definition.value,
                                       .position = use->position,
                                       .constraint_name = "au-props-correct.2"};
    sxml_status status = sxml_check_value_constraint(compiler, own);
    if (status != SXML_STATUS_OK ||
        declaration->value_constraint.variety != SXML_SCHEMA_VALUE_CONSTRAINT_FIXED ||
        use->value_constraint.variety == SXML_SCHEMA_VALUE_CONSTRAINT_NONE) {
        return status;
    }
    bool is_equal = false;
    if (use->value_constraint.variety == SXML_SCHEMA_VALUE_CONSTRAINT_FIXED) {
        sxml_value_constraint_check fixed = own;
        fixed.value_constraint = declaration->value_constraint;
        sxml_simple_cast_outcome first;
        sxml_simple_cast_outcome second = {.is_valid = false, .value = sxml_sequence_empty()};
        status = sxml_cast_value_constraint(compiler, own, &first);
        if (status == SXML_STATUS_OK) {
            status = sxml_cast_value_constraint(compiler, fixed, &second);
        }
        if (status == SXML_STATUS_OK && first.is_valid && second.is_valid) {
            status = sxml_values_equal(first.value, second.value, &is_equal);
        } else if (status == SXML_STATUS_OK) {
            /* An invalid value was reported already, or the type is
               complex: nothing to compare. */
            is_equal = true;
        }
        sxml_sequence_release(first.value);
        sxml_sequence_release(second.value);
    }
    if (status != SXML_STATUS_OK || is_equal) {
        return status;
    }
    return sxml_simple_type_report(
        compiler,
        (sxml_simple_type_error){.constraint_name = "au-props-correct.2",
                                 .description = "the attribute declaration's value is fixed, "
                                                "so the use's must be fixed and equal (XSD 1.1 "
                                                "Part 1, 3.5.6.1)",
                                 .position = use->position});
}

static sxml_status sxml_check_attribute_values(const sxml_simple_type_compiler *compiler) {
    const sxml_schema_components *components = compiler->components;
    uint32_t declaration_count = components->tables[SXML_SCHEMA_TABLE_ATTRIBUTE_DECLARATION].entry_count;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t index = 1; status == SXML_STATUS_OK && index < declaration_count; index += 1) {
        const sxml_schema_attribute_declaration *declaration = sxml_schema_get_attribute_declaration(
            components, (sxml_schema_attribute_declaration_index){.value = index});
        status = sxml_check_value_constraint(
            compiler, (sxml_value_constraint_check){.value_constraint = declaration->value_constraint,
                                                    .type_index = declaration->type_definition.value,
                                                    .position = declaration->position,
                                                    .constraint_name = "a-props-correct.2"});
    }
    uint32_t use_count = components->tables[SXML_SCHEMA_TABLE_ATTRIBUTE_USE].entry_count;
    for (uint32_t index = 1; status == SXML_STATUS_OK && index < use_count; index += 1) {
        status = sxml_check_attribute_use(
            compiler, sxml_schema_get_attribute_use(
                          components, (sxml_schema_attribute_use_index){.value = index}));
    }
    return status;
}

/* ---------------------------------------------------------------- Stage */

static sxml_simple_type_compiler sxml_simple_type_compiler_of(sxml_schema_compilation *compilation) {
    sxml_processor *processor = compilation->components->processor;
    return (sxml_simple_type_compiler){
        .compilation = compilation,
        .components = compilation->components,
        .item_heap = sxml_processor_get_item_heap(processor),
        .registry = sxml_processor_get_type_registry(processor),
        .allocator = compilation->components->allocator,
    };
}

sxml_status sxml_schema_compile_simple_types(sxml_schema_compilation *compilation) {
    if (compilation == NULL || compilation->components == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_simple_type_compiler compiler = sxml_simple_type_compiler_of(compilation);
    uint32_t type_count =
        compilation->components->tables[SXML_SCHEMA_TABLE_TYPE_DEFINITION].entry_count;
    sxml_status status = SXML_STATUS_OK;
    if (type_count > 1) {
        /* A state per type definition, then a frame per type definition:
           a dependency chain visits each at most once. */
        size_t frames_offset = ((size_t)type_count + alignof(sxml_simple_type_frame) - 1U) /
                               alignof(sxml_simple_type_frame) * alignof(sxml_simple_type_frame);
        void *memory = NULL;
        status = sxml_allocator_allocate(
            compiler.allocator,
            (sxml_allocation_request){
                .byte_count = frames_offset + (size_t)type_count * sizeof(sxml_simple_type_frame),
                .alignment = alignof(sxml_simple_type_frame)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        memset(memory, 0, type_count);
        sxml_simple_type_walk walk = {
            .states = memory,
            .frames = (sxml_simple_type_frame *)(void *)((uint8_t *)memory + frames_offset),
            .frame_count = 0,
            .type_count = type_count};
        for (uint32_t type_index = 1; status == SXML_STATUS_OK && type_index < type_count;
             type_index += 1) {
            if (walk.states[type_index] == SXML_SIMPLE_TYPE_UNVISITED &&
                sxml_needs_compiling(&compiler, type_index)) {
                status = sxml_walk_from(&compiler, &walk, type_index);
            }
        }
        sxml_allocator_release(compiler.allocator, memory);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_check_attribute_values(&compiler);
    }
    return status;
}

sxml_status sxml_schema_compile_simple_type(sxml_schema_compilation *compilation,
                                            sxml_schema_type_definition_index simple_type) {
    if (compilation == NULL || compilation->components == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_simple_type_compiler compiler = sxml_simple_type_compiler_of(compilation);
    return sxml_compile_one(&compiler, simple_type.value);
}
