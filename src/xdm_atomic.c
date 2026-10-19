/*
 * Atomic items: making them, reading their datum, fn:atomic-equal, the
 * matching hash and fn:compare (include/sxml/xdm.h, src/xdm_internal.h;
 * D34, D36, D57, D60).
 *
 * Golden file: src/memory.c.
 *
 * Inline: xs:boolean, every integer type while the value fits int64_t,
 * xs:float, xs:double, xs:QName and xs:NOTATION. In a cell: xs:decimal
 * (always, D36), integers beyond int64_t, strings and their derived types,
 * xs:anyURI, xs:untypedAtomic, the eight date/time types, durations and the
 * two binary types. A decimal cell keeps its canonical text and whether it
 * is exactly a double, made once with the cell, so that atomic-equal, the
 * hash and compare never allocate and never fail.
 *
 * Specification: XDM 4.0, section 5 (atomic items and datums); F&O 4.0,
 * section 2.2.1 (fn:atomic-equal) and 2.2.2 (fn:compare); XSD 1.1 Part 2,
 * section 3 (value spaces of the built-in types); XML 1.0 Fifth Edition,
 * section 2.3 (Name and Nmtoken productions).
 */
#include "xdm_internal.h"

#include "sxml/string.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------ The cells */

/* The decimal of an xs:decimal item or a big integer item. */
typedef struct sxml_decimal_contents {
    sxml_decimal decimal;
    /* F&O 4.0, "Casting to xs:string": no exponent, no "+", no leading or
       trailing zeros. Stored after the cell. */
    sxml_byte_view canonical_text;
    /* The double nearest to the decimal, and whether the decimal equals it
       exactly: what atomic-equal and the hash compare with doubles. */
    double nearest_double;
    bool equals_double;
} sxml_decimal_contents;

typedef struct sxml_atomic_cell {
    sxml_cell cell;
    /* SXML_DATUM_KIND_DECIMAL, _STRING, _DATE_TIME, _DURATION or
       _BINARY. */
    sxml_datum_kind datum_kind;
    union {
        sxml_decimal_contents decimal;
        /* Bytes stored after the cell. */
        sxml_string string;
        sxml_date_time date_time;
        sxml_duration duration;
        /* Octets stored after the cell. */
        sxml_byte_view binary;
    } contents;
    /* Text or octets of the datum. */
    uint8_t trailing_bytes[];
} sxml_atomic_cell;

static void sxml_atomic_cell_release_contents(sxml_cell *cell) {
    sxml_atomic_cell *atomic_cell = (sxml_atomic_cell *)cell;
    if (atomic_cell->datum_kind == SXML_DATUM_KIND_DECIMAL) {
        sxml_decimal_finalize(&atomic_cell->contents.decimal.decimal);
    }
}

static const sxml_cell_class sxml_atomic_cell_class = {
    .class_name = SXML_BYTE_VIEW_INITIALIZER("atomic"),
    .release_contents = sxml_atomic_cell_release_contents,
    .sequence_operations = NULL,
};

static const sxml_atomic_cell *sxml_atomic_cell_of(sxml_item item) {
    return (const sxml_atomic_cell *)sxml_item_get_cell(item);
}

/* Allocates an atomic cell with trailing_byte_count bytes after it. */
static sxml_status sxml_atomic_cell_allocate(sxml_item_heap *item_heap,
                                             size_t trailing_byte_count,
                                             sxml_atomic_cell **atomic_cell) {
    size_t byte_count = 0;
    sxml_status status = sxml_size_add(sizeof(sxml_atomic_cell),
                                       trailing_byte_count, &byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_cell *cell = NULL;
    status = sxml_item_heap_allocate_cell(
        item_heap,
        &(sxml_cell_request){.cell_class = &sxml_atomic_cell_class,
                             .byte_count = byte_count,
                             .alignment = alignof(sxml_atomic_cell)},
        &cell);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *atomic_cell = (sxml_atomic_cell *)cell;
    return SXML_STATUS_OK;
}

/* The item of a filled atomic cell. */
static sxml_item sxml_atomic_cell_item(sxml_atomic_cell *atomic_cell,
                                       sxml_type_identifier type,
                                       sxml_builtin_type primitive_type) {
    return sxml_item_from_cell(&(sxml_cell_item_request){
        .representation = SXML_ITEM_REPRESENTATION_ATOMIC_CELL,
        .type_annotation = type,
        .primitive_type = sxml_builtin_type_identifier(primitive_type),
        .cell = &atomic_cell->cell,
    });
}

/* ---------------------------------------------------------- Inline items */

static sxml_item sxml_inline_atomic_item(sxml_item_representation representation,
                                         sxml_type_identifier type,
                                         sxml_builtin_type primitive_type) {
    return (sxml_item){
        .representation = (uint8_t)representation,
        .flags = (uint8_t)primitive_type,
        .namespace_number = 0,
        .type_or_node_index = type.index,
        .payload = {.bits = 0},
    };
}

static sxml_item sxml_integer_item(sxml_type_identifier type,
                                   int64_t integer) {
    sxml_item item = sxml_inline_atomic_item(SXML_ITEM_REPRESENTATION_INTEGER,
                                             type, SXML_TYPE_XS_DECIMAL);
    item.payload.integer = integer;
    return item;
}

sxml_item sxml_atomic_item_from_integer(int64_t integer) {
    return sxml_integer_item(sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
                             integer);
}

static sxml_item sxml_double_item(sxml_type_identifier type,
                                  double double_number) {
    sxml_item item = sxml_inline_atomic_item(SXML_ITEM_REPRESENTATION_DOUBLE,
                                             type, SXML_TYPE_XS_DOUBLE);
    item.payload.double_number = double_number;
    return item;
}

sxml_item sxml_atomic_item_from_double(double double_number) {
    return sxml_double_item(sxml_builtin_type_identifier(SXML_TYPE_XS_DOUBLE),
                            double_number);
}

/* An xs:boolean value with its type annotation. */
typedef struct sxml_typed_boolean {
    sxml_type_identifier type;
    bool boolean;
} sxml_typed_boolean;

static sxml_item sxml_boolean_item(sxml_typed_boolean value) {
    sxml_item item = sxml_inline_atomic_item(SXML_ITEM_REPRESENTATION_BOOLEAN,
                                             value.type, SXML_TYPE_XS_BOOLEAN);
    item.payload.integer = value.boolean ? 1 : 0;
    return item;
}

sxml_item sxml_atomic_item_true(void) {
    return sxml_boolean_item((sxml_typed_boolean){
        .type = sxml_builtin_type_identifier(SXML_TYPE_XS_BOOLEAN),
        .boolean = true});
}

sxml_item sxml_atomic_item_false(void) {
    return sxml_boolean_item((sxml_typed_boolean){
        .type = sxml_builtin_type_identifier(SXML_TYPE_XS_BOOLEAN),
        .boolean = false});
}

/* ------------------------------------------------------- Type families */

sxml_type_identifier sxml_atomic_item_get_primitive_type(sxml_item item) {
    if (!sxml_item_is_atomic(item)) {
        return sxml_builtin_type_identifier(SXML_TYPE_NONE);
    }
    return (sxml_type_identifier){.index = item.flags};
}

static sxml_builtin_type sxml_atomic_item_primitive(sxml_item item) {
    return (sxml_builtin_type)sxml_atomic_item_get_primitive_type(item).index;
}

bool sxml_atomic_item_is_numeric(sxml_item item) {
    sxml_builtin_type primitive_type = sxml_atomic_item_primitive(item);
    return primitive_type == SXML_TYPE_XS_DECIMAL ||
           primitive_type == SXML_TYPE_XS_FLOAT ||
           primitive_type == SXML_TYPE_XS_DOUBLE;
}

/* True for the primitive types that compare as strings (F&O 4.0, section
   2.2.1: xs:string, xs:anyURI and xs:untypedAtomic compare alike). */
static bool sxml_primitive_is_string_like(sxml_builtin_type primitive_type) {
    return primitive_type == SXML_TYPE_XS_STRING ||
           primitive_type == SXML_TYPE_XS_ANY_URI ||
           primitive_type == SXML_TYPE_XS_UNTYPED_ATOMIC;
}

static bool sxml_primitive_is_date_time(sxml_builtin_type primitive_type) {
    return primitive_type >= SXML_TYPE_XS_DATE_TIME &&
           primitive_type <= SXML_TYPE_XS_G_MONTH;
}

/*
 * The comparison family of a primitive type: items of one family are
 * compared with each other, items of different families are never equal
 * and do not compare (XPTY0004). Numbers, strings and the two binary types
 * each form one family; every other primitive type is a family of its own.
 * Also used as the seed of the hash, so families hash apart.
 */
typedef enum sxml_atomic_family {
    SXML_ATOMIC_FAMILY_NUMERIC = 1000,
    SXML_ATOMIC_FAMILY_STRING = 1001,
    SXML_ATOMIC_FAMILY_BINARY = 1002,
} sxml_atomic_family;

static uint32_t sxml_atomic_item_family(sxml_item item) {
    sxml_builtin_type primitive_type = sxml_atomic_item_primitive(item);
    if (sxml_atomic_item_is_numeric(item)) {
        return SXML_ATOMIC_FAMILY_NUMERIC;
    }
    if (sxml_primitive_is_string_like(primitive_type)) {
        return SXML_ATOMIC_FAMILY_STRING;
    }
    if (primitive_type == SXML_TYPE_XS_HEX_BINARY ||
        primitive_type == SXML_TYPE_XS_BASE64_BINARY) {
        return SXML_ATOMIC_FAMILY_BINARY;
    }
    return (uint32_t)primitive_type;
}

/* --------------------------------------------------------- Type checks */

/* What making an item needs to know about its type. */
typedef struct sxml_atomic_type_facts {
    /* The type itself, or for a registered type the nearest built-in
       type it is derived from, whose value space rules apply. */
    sxml_builtin_type builtin_type;
    sxml_builtin_type primitive_type;
} sxml_atomic_type_facts;

/* Steps through base types before a registered type is given up on: far
   more than any schema derives, and a bound on a broken registry. */
enum { SXML_MAXIMUM_DERIVATION_STEP_COUNT = 1024 };

static sxml_status sxml_atomic_type_resolve(
    const sxml_type_registry *registry, sxml_type_identifier type,
    sxml_atomic_type_facts *facts) {
    sxml_type_description description;
    sxml_status status =
        sxml_type_registry_describe_type(registry, type, &description);
    if (status != SXML_STATUS_OK ||
        description.variety != SXML_TYPE_VARIETY_ATOMIC ||
        description.primitive_type.index == SXML_TYPE_NONE) {
        /* xs:anyAtomicType has no primitive type and no instances of its
           own (XDM 4.0, section 4.1.3). */
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_type_identifier builtin = type;
    for (size_t step_index = 0;
         builtin.index >= SXML_TYPE_BUILTIN_COUNT;
         step_index += 1) {
        sxml_type_description base_description;
        if (step_index == SXML_MAXIMUM_DERIVATION_STEP_COUNT ||
            sxml_type_registry_describe_type(registry, builtin,
                                             &base_description) !=
                SXML_STATUS_OK) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        builtin = base_description.base_type;
    }
    *facts = (sxml_atomic_type_facts){
        .builtin_type = (sxml_builtin_type)builtin.index,
        .primitive_type = (sxml_builtin_type)description.primitive_type.index,
    };
    return SXML_STATUS_OK;
}

/* The value range of a built-in integer type within int64_t (XSD 1.1 Part
   2, sections 3.4.13 to 3.4.25). */
typedef struct sxml_integer_bounds {
    int64_t minimum;
    int64_t maximum;
    /* Values beyond int64_t: allowed below (negative) or above
       (positive). */
    bool allows_beyond_minimum;
    bool allows_beyond_maximum;
} sxml_integer_bounds;

static bool sxml_integer_type_bounds(sxml_builtin_type builtin_type,
                                     sxml_integer_bounds *bounds) {
    /* A switch on the value: -Wswitch-enum would ask for all built-in
       types. */
    switch ((uint32_t)builtin_type) {
    case SXML_TYPE_XS_INTEGER:
        *bounds = (sxml_integer_bounds){INT64_MIN, INT64_MAX, true, true};
        return true;
    case SXML_TYPE_XS_NON_POSITIVE_INTEGER:
        *bounds = (sxml_integer_bounds){INT64_MIN, 0, true, false};
        return true;
    case SXML_TYPE_XS_NEGATIVE_INTEGER:
        *bounds = (sxml_integer_bounds){INT64_MIN, -1, true, false};
        return true;
    case SXML_TYPE_XS_LONG:
        *bounds = (sxml_integer_bounds){INT64_MIN, INT64_MAX, false, false};
        return true;
    case SXML_TYPE_XS_INT:
        *bounds = (sxml_integer_bounds){INT32_MIN, INT32_MAX, false, false};
        return true;
    case SXML_TYPE_XS_SHORT:
        *bounds = (sxml_integer_bounds){INT16_MIN, INT16_MAX, false, false};
        return true;
    case SXML_TYPE_XS_BYTE:
        *bounds = (sxml_integer_bounds){INT8_MIN, INT8_MAX, false, false};
        return true;
    case SXML_TYPE_XS_NON_NEGATIVE_INTEGER:
    case SXML_TYPE_XS_UNSIGNED_LONG:
        /* xs:unsignedLong goes up to 18446744073709551615, checked
           separately beyond int64_t. */
        *bounds = (sxml_integer_bounds){0, INT64_MAX, false, true};
        return true;
    case SXML_TYPE_XS_UNSIGNED_INT:
        *bounds = (sxml_integer_bounds){0, UINT32_MAX, false, false};
        return true;
    case SXML_TYPE_XS_UNSIGNED_SHORT:
        *bounds = (sxml_integer_bounds){0, UINT16_MAX, false, false};
        return true;
    case SXML_TYPE_XS_UNSIGNED_BYTE:
        *bounds = (sxml_integer_bounds){0, UINT8_MAX, false, false};
        return true;
    case SXML_TYPE_XS_POSITIVE_INTEGER:
        *bounds = (sxml_integer_bounds){1, INT64_MAX, false, true};
        return true;
    default:
        return false;
    }
}

/* ---- Names (XML 1.0 Fifth Edition, section 2.3) */

static bool sxml_is_name_start_codepoint(uint32_t codepoint) {
    return codepoint == ':' || (codepoint >= 'A' && codepoint <= 'Z') ||
           codepoint == '_' || (codepoint >= 'a' && codepoint <= 'z') ||
           (codepoint >= 0xC0 && codepoint <= 0xD6) ||
           (codepoint >= 0xD8 && codepoint <= 0xF6) ||
           (codepoint >= 0xF8 && codepoint <= 0x2FF) ||
           (codepoint >= 0x370 && codepoint <= 0x37D) ||
           (codepoint >= 0x37F && codepoint <= 0x1FFF) ||
           (codepoint >= 0x200C && codepoint <= 0x200D) ||
           (codepoint >= 0x2070 && codepoint <= 0x218F) ||
           (codepoint >= 0x2C00 && codepoint <= 0x2FEF) ||
           (codepoint >= 0x3001 && codepoint <= 0xD7FF) ||
           (codepoint >= 0xF900 && codepoint <= 0xFDCF) ||
           (codepoint >= 0xFDF0 && codepoint <= 0xFFFD) ||
           (codepoint >= 0x10000 && codepoint <= 0xEFFFF);
}

static bool sxml_is_name_codepoint(uint32_t codepoint) {
    return sxml_is_name_start_codepoint(codepoint) || codepoint == '-' ||
           codepoint == '.' || (codepoint >= '0' && codepoint <= '9') ||
           codepoint == 0xB7 || (codepoint >= 0x300 && codepoint <= 0x36F) ||
           (codepoint >= 0x203F && codepoint <= 0x2040);
}

/* Which name production a string must match. */
typedef enum sxml_name_production {
    /* Nmtoken: NameChar+. */
    SXML_NAME_PRODUCTION_NMTOKEN,
    /* Name: NameStartChar NameChar*. */
    SXML_NAME_PRODUCTION_NAME,
    /* NCName: a Name without ":" (Namespaces in XML 1.0, section 3). */
    SXML_NAME_PRODUCTION_NCNAME,
} sxml_name_production;

typedef struct sxml_name_check {
    sxml_string string;
    sxml_name_production production;
} sxml_name_check;

static bool sxml_string_matches_name(sxml_name_check check) {
    if (check.string.utf8.byte_count == 0) {
        return false;
    }
    sxml_codepoint_iterator iterator = {.string = check.string,
                                        .byte_offset = 0};
    uint32_t codepoint = 0;
    bool is_first = true;
    while (sxml_codepoint_iterator_next(&iterator, &codepoint)) {
        bool is_allowed = is_first && check.production !=
                                          SXML_NAME_PRODUCTION_NMTOKEN
                              ? sxml_is_name_start_codepoint(codepoint)
                              : sxml_is_name_codepoint(codepoint);
        if (!is_allowed || (check.production == SXML_NAME_PRODUCTION_NCNAME &&
                            codepoint == ':')) {
            return false;
        }
        is_first = false;
    }
    return true;
}

static bool sxml_is_ascii_letter(uint8_t byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
}

/* xs:language: [a-zA-Z]{1,8}(-[a-zA-Z0-9]{1,8})* (XSD 1.1 Part 2,
   section 3.4.3). */
static bool sxml_string_is_language(sxml_byte_view utf8) {
    size_t part_length = 0;
    bool is_first_part = true;
    for (size_t byte_index = 0; byte_index < utf8.byte_count; byte_index += 1) {
        uint8_t byte = utf8.bytes[byte_index];
        if (byte == '-') {
            if (part_length == 0) {
                return false;
            }
            part_length = 0;
            is_first_part = false;
            continue;
        }
        bool is_allowed = sxml_is_ascii_letter(byte) ||
                          (!is_first_part && byte >= '0' && byte <= '9');
        if (!is_allowed || part_length == 8) {
            return false;
        }
        part_length += 1;
    }
    return part_length != 0;
}

/* Text to check for xs:normalizedString or xs:token. */
typedef struct sxml_whitespace_check {
    sxml_byte_view utf8;
    bool is_token;
} sxml_whitespace_check;

/* xs:normalizedString: no tab, line feed or carriage return; xs:token
   also no leading, trailing or doubled space (XSD 1.1 Part 2, 3.4.1 and
   3.4.2). */
static bool sxml_string_is_normalized(sxml_whitespace_check check) {
    sxml_byte_view utf8 = check.utf8;
    bool is_token = check.is_token;
    for (size_t byte_index = 0; byte_index < utf8.byte_count; byte_index += 1) {
        uint8_t byte = utf8.bytes[byte_index];
        if (byte == '\t' || byte == '\n' || byte == '\r') {
            return false;
        }
        if (is_token && byte == ' ' &&
            (byte_index == 0 || byte_index + 1 == utf8.byte_count ||
             utf8.bytes[byte_index + 1] == ' ')) {
            return false;
        }
    }
    return true;
}

static bool sxml_string_is_in_value_space(sxml_builtin_type builtin_type,
                                          sxml_string string) {
    switch ((uint32_t)builtin_type) {
    case SXML_TYPE_XS_NORMALIZED_STRING:
        return sxml_string_is_normalized(
            (sxml_whitespace_check){.utf8 = string.utf8, .is_token = false});
    case SXML_TYPE_XS_TOKEN:
        return sxml_string_is_normalized(
            (sxml_whitespace_check){.utf8 = string.utf8, .is_token = true});
    case SXML_TYPE_XS_LANGUAGE:
        return sxml_string_is_language(string.utf8);
    case SXML_TYPE_XS_NMTOKEN:
        return sxml_string_matches_name((sxml_name_check){
            .string = string, .production = SXML_NAME_PRODUCTION_NMTOKEN});
    case SXML_TYPE_XS_NAME:
        return sxml_string_matches_name((sxml_name_check){
            .string = string, .production = SXML_NAME_PRODUCTION_NAME});
    case SXML_TYPE_XS_NCNAME:
    case SXML_TYPE_XS_ID:
    case SXML_TYPE_XS_IDREF:
    case SXML_TYPE_XS_ENTITY:
        return sxml_string_matches_name((sxml_name_check){
            .string = string, .production = SXML_NAME_PRODUCTION_NCNAME});
    default:
        /* xs:string, xs:anyURI (any string in XSD 1.1) and
           xs:untypedAtomic. */
        return true;
    }
}

/* ---- Dates and durations (XDM 4.0, section 5.4; XSD 1.1 Part 2, D.2) */

static const uint8_t sxml_days_in_month_of_common_year[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static bool sxml_year_is_leap(int64_t year) {
    /* XSD 1.1 numbering: year 0 is 1 BCE, a leap year. */
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static uint8_t sxml_days_in_month(int64_t year, uint8_t month) {
    if (month == 2 && sxml_year_is_leap(year)) {
        return 29;
    }
    return sxml_days_in_month_of_common_year[month - 1];
}

/* Which components a date/time primitive type has. */
typedef struct sxml_date_time_components {
    bool has_year;
    bool has_month;
    bool has_day;
    bool has_time;
} sxml_date_time_components;

static sxml_date_time_components sxml_date_time_components_of(
    sxml_builtin_type primitive_type) {
    switch ((uint32_t)primitive_type) {
    case SXML_TYPE_XS_DATE_TIME:
        return (sxml_date_time_components){true, true, true, true};
    case SXML_TYPE_XS_DATE:
        return (sxml_date_time_components){true, true, true, false};
    case SXML_TYPE_XS_TIME:
        return (sxml_date_time_components){false, false, false, true};
    case SXML_TYPE_XS_G_YEAR_MONTH:
        return (sxml_date_time_components){true, true, false, false};
    case SXML_TYPE_XS_G_YEAR:
        return (sxml_date_time_components){true, false, false, false};
    case SXML_TYPE_XS_G_MONTH_DAY:
        return (sxml_date_time_components){false, true, true, false};
    case SXML_TYPE_XS_G_DAY:
        return (sxml_date_time_components){false, false, true, false};
    case SXML_TYPE_XS_G_MONTH:
        return (sxml_date_time_components){false, true, false, false};
    default:
        return (sxml_date_time_components){false, false, false, false};
    }
}

/* A year in which every gMonthDay exists: 1972, a leap year, is the
   reference year of F&O 4.0, section 9.7 ("Comparison operators on
   duration, date and time values"). */
static const int64_t sxml_reference_year = 1972;

static bool sxml_date_time_is_in_value_space(
    sxml_atomic_type_facts facts, const sxml_date_time *date_time) {
    sxml_date_time_components components =
        sxml_date_time_components_of(facts.primitive_type);
    bool absent_are_zero =
        (components.has_year || date_time->year == 0) &&
        (components.has_month || date_time->month == 0) &&
        (components.has_day || date_time->day == 0) &&
        (components.has_time ||
         (date_time->hour == 0 && date_time->minute == 0 &&
          date_time->second == 0 && date_time->nanosecond == 0));
    if (!absent_are_zero) {
        return false;
    }
    if (components.has_month &&
        (date_time->month < 1 || date_time->month > 12)) {
        return false;
    }
    if (components.has_day) {
        int64_t year =
            components.has_year ? date_time->year : sxml_reference_year;
        uint8_t last_day =
            components.has_month ? sxml_days_in_month(year, date_time->month)
                                 : 31;
        if (date_time->day < 1 || date_time->day > last_day) {
            return false;
        }
    }
    if (date_time->hour > 23 || date_time->minute > 59 ||
        date_time->second > 59 || date_time->nanosecond >= 1000000000U) {
        return false;
    }
    if (date_time->has_timezone &&
        (date_time->timezone_offset_minutes < -840 ||
         date_time->timezone_offset_minutes > 840)) {
        return false;
    }
    if (!date_time->has_timezone && date_time->timezone_offset_minutes != 0) {
        return false;
    }
    /* xs:dateTimeStamp requires a timezone (XSD 1.1 Part 2, 3.4.28). */
    return facts.builtin_type != SXML_TYPE_XS_DATE_TIME_STAMP ||
           date_time->has_timezone;
}

static bool sxml_duration_is_in_value_space(sxml_atomic_type_facts facts,
                                            const sxml_duration *duration) {
    if (duration->nanoseconds <= -1000000000 ||
        duration->nanoseconds >= 1000000000) {
        return false;
    }
    bool has_negative = duration->months < 0 || duration->seconds < 0 ||
                        duration->nanoseconds < 0;
    bool has_positive = duration->months > 0 || duration->seconds > 0 ||
                        duration->nanoseconds > 0;
    if (has_negative && has_positive) {
        return false;
    }
    if (facts.builtin_type == SXML_TYPE_XS_DAY_TIME_DURATION) {
        return duration->months == 0;
    }
    if (facts.builtin_type == SXML_TYPE_XS_YEAR_MONTH_DURATION) {
        return duration->seconds == 0 && duration->nanoseconds == 0;
    }
    return true;
}

/* ------------------------------------------------- Exact decimal texts */

/*
 * Room for the exact decimal text of any double: 2^-1074 has 1074 fraction
 * digits, 2^1023 has 308 integer digits; a sign and "0." on top.
 */
#define SXML_EXACT_DOUBLE_TEXT_CAPACITY 1100

/* Base 10^9 limbs, least significant first: 767 digits need 86. */
enum { SXML_EXACT_LIMB_CAPACITY = 96 };

typedef struct sxml_exact_integer {
    uint32_t limbs[SXML_EXACT_LIMB_CAPACITY];
    size_t limb_count;
} sxml_exact_integer;

static const uint32_t sxml_limb_base = 1000000000U;

static void sxml_exact_integer_multiply(sxml_exact_integer *integer,
                                        uint32_t factor) {
    uint64_t carry = 0;
    for (size_t limb_index = 0; limb_index < integer->limb_count;
         limb_index += 1) {
        /* At most (10^9 - 1) * (2^32 - 1) + carry: below 2^63. */
        uint64_t product =
            (uint64_t)integer->limbs[limb_index] * factor + carry;
        integer->limbs[limb_index] = (uint32_t)(product % sxml_limb_base);
        carry = product / sxml_limb_base;
    }
    while (carry != 0 && integer->limb_count < SXML_EXACT_LIMB_CAPACITY) {
        integer->limbs[integer->limb_count] =
            (uint32_t)(carry % sxml_limb_base);
        integer->limb_count += 1;
        carry /= sxml_limb_base;
    }
}

/* Writes the digits of integer to output (big enough), returns the count. */
static size_t sxml_exact_integer_write_digits(const sxml_exact_integer *integer,
                                              sxml_memory_view output_memory) {
    uint8_t *output = output_memory.bytes;
    size_t digit_count = 0;
    for (size_t limb_index = integer->limb_count; limb_index > 0;
         limb_index -= 1) {
        uint32_t limb = integer->limbs[limb_index - 1];
        uint8_t limb_digits[9];
        for (size_t digit_index = 9; digit_index > 0; digit_index -= 1) {
            limb_digits[digit_index - 1] = (uint8_t)('0' + limb % 10);
            limb /= 10;
        }
        size_t first_digit = 0;
        if (digit_count == 0) {
            /* No leading zeros in the most significant limb. */
            while (first_digit < 8 && limb_digits[first_digit] == '0') {
                first_digit += 1;
            }
        }
        memcpy(output + digit_count, limb_digits + first_digit,
               9 - first_digit);
        digit_count += 9 - first_digit;
    }
    return digit_count;
}

/* The bits of a finite double: value = significand * 2^exponent. */
typedef struct sxml_double_parts {
    bool is_negative;
    uint64_t significand;
    int32_t exponent;
} sxml_double_parts;

static sxml_double_parts sxml_double_split(double double_number) {
    uint64_t bits = 0;
    memcpy(&bits, &double_number, sizeof(bits));
    uint32_t biased_exponent = (uint32_t)((bits >> 52) & 0x7FFU);
    uint64_t fraction = bits & ((UINT64_C(1) << 52) - 1);
    sxml_double_parts parts = {
        .is_negative = (bits >> 63) != 0,
        .significand = fraction,
        .exponent = -1074,
    };
    if (biased_exponent != 0) {
        parts.significand = fraction | (UINT64_C(1) << 52);
        parts.exponent = (int32_t)biased_exponent - 1075;
    }
    return parts;
}

/*
 * Writes the exact value of a finite double as canonical decimal text (the
 * form of sxml_decimal_write_canonical_text) to output, which has
 * SXML_EXACT_DOUBLE_TEXT_CAPACITY bytes, and returns the view. Needs no
 * memory beyond the stack: atomic-equal and compare must not fail.
 */
static sxml_byte_view sxml_double_write_exact_text(double double_number,
                                                   sxml_memory_view output_memory) {
    uint8_t *output = output_memory.bytes;
    sxml_double_parts parts = sxml_double_split(double_number);
    sxml_exact_integer integer = {.limbs = {0}, .limb_count = 0};
    uint64_t significand = parts.significand;
    while (significand != 0) {
        integer.limbs[integer.limb_count] =
            (uint32_t)(significand % sxml_limb_base);
        integer.limb_count += 1;
        significand /= sxml_limb_base;
    }
    size_t fraction_digit_count = 0;
    if (parts.exponent >= 0) {
        for (int32_t remaining = parts.exponent; remaining > 0;
             remaining -= 30) {
            uint32_t shift = remaining > 30 ? 30U : (uint32_t)remaining;
            sxml_exact_integer_multiply(&integer, UINT32_C(1) << shift);
        }
    } else {
        /* significand * 2^-n = significand * 5^n / 10^n. */
        fraction_digit_count = (size_t)(-(int64_t)parts.exponent);
        for (size_t remaining = fraction_digit_count; remaining > 0;) {
            uint32_t factor = 1;
            size_t power_count = 0;
            while (power_count < 13 && power_count < remaining) {
                factor *= 5;
                power_count += 1;
            }
            sxml_exact_integer_multiply(&integer, factor);
            remaining -= power_count;
        }
    }
    uint8_t digits[SXML_EXACT_DOUBLE_TEXT_CAPACITY];
    size_t digit_count = 0;
    if (integer.limb_count != 0) {
        digit_count = sxml_exact_integer_write_digits(
            &integer,
            (sxml_memory_view){.bytes = digits, .byte_count = sizeof(digits)});
    }
    /* Drop fraction digits that are trailing zeros. */
    while (fraction_digit_count > 0 && digit_count > 0 &&
           digits[digit_count - 1] == '0') {
        digit_count -= 1;
        fraction_digit_count -= 1;
    }
    if (digit_count == 0) {
        /* Zero, of either sign, is "0" (D136, Proposed): no fraction. */
        fraction_digit_count = 0;
    }
    size_t output_count = 0;
    if (parts.is_negative && digit_count != 0) {
        output[output_count] = '-';
        output_count += 1;
    }
    if (digit_count <= fraction_digit_count) {
        /* No integer digits: "0", then the fraction with leading zeros. */
        output[output_count] = '0';
        output_count += 1;
        if (fraction_digit_count != 0) {
            output[output_count] = '.';
            output_count += 1;
            size_t zero_count = fraction_digit_count - digit_count;
            memset(output + output_count, '0', zero_count);
            output_count += zero_count;
        }
        memcpy(output + output_count, digits, digit_count);
        output_count += digit_count;
    } else {
        size_t integer_digit_count = digit_count - fraction_digit_count;
        memcpy(output + output_count, digits, integer_digit_count);
        output_count += integer_digit_count;
        if (fraction_digit_count != 0) {
            output[output_count] = '.';
            output_count += 1;
            memcpy(output + output_count, digits + integer_digit_count,
                   fraction_digit_count);
            output_count += fraction_digit_count;
        }
    }
    return (sxml_byte_view){.bytes = output, .byte_count = output_count};
}

/* Room for the text of any int64_t: a sign and 19 digits. */
#define SXML_INTEGER_TEXT_CAPACITY 24

/* Writes integer as canonical decimal text to output. */
static sxml_byte_view sxml_integer_write_text(int64_t integer,
                                              sxml_memory_view output_memory) {
    uint8_t *output = output_memory.bytes;
    uint8_t digits[SXML_INTEGER_TEXT_CAPACITY];
    size_t digit_count = 0;
    /* Negate in unsigned arithmetic so INT64_MIN does not overflow. */
    uint64_t magnitude = integer < 0 ? (uint64_t)0 - (uint64_t)integer
                                     : (uint64_t)integer;
    do {
        digits[digit_count] = (uint8_t)('0' + magnitude % 10);
        digit_count += 1;
        magnitude /= 10;
    } while (magnitude != 0);
    size_t output_count = 0;
    if (integer < 0) {
        output[output_count] = '-';
        output_count += 1;
    }
    while (digit_count > 0) {
        digit_count -= 1;
        output[output_count] = digits[digit_count];
        output_count += 1;
    }
    return (sxml_byte_view){.bytes = output, .byte_count = output_count};
}

/* The length of the integer part of an unsigned canonical text. */
static size_t sxml_canonical_integer_digit_count(sxml_byte_view magnitude) {
    size_t digit_count = 0;
    while (digit_count < magnitude.byte_count &&
           magnitude.bytes[digit_count] != '.') {
        digit_count += 1;
    }
    return digit_count;
}

static sxml_ordering sxml_ordering_from_difference(int difference) {
    if (difference < 0) {
        return SXML_ORDERING_LESS;
    }
    return difference > 0 ? SXML_ORDERING_GREATER : SXML_ORDERING_EQUAL;
}

static sxml_ordering sxml_ordering_reverse(sxml_ordering ordering) {
    return (sxml_ordering)(-(int)ordering);
}

/* Compares bytes as unsigned octets; a proper prefix is less. */
static sxml_ordering sxml_bytes_compare(sxml_byte_view first,
                                        sxml_byte_view second) {
    size_t common_count = first.byte_count < second.byte_count
                              ? first.byte_count
                              : second.byte_count;
    int difference =
        common_count == 0 ? 0 : memcmp(first.bytes, second.bytes, common_count);
    if (difference != 0) {
        return sxml_ordering_from_difference(difference);
    }
    if (first.byte_count == second.byte_count) {
        return SXML_ORDERING_EQUAL;
    }
    return first.byte_count < second.byte_count ? SXML_ORDERING_LESS
                                                : SXML_ORDERING_GREATER;
}

/*
 * Orders two canonical decimal texts by value. Canonical texts have no
 * leading zeros in the integer part (one "0" at least), no trailing zeros
 * in the fraction and no "-0", so magnitudes order by integer digit count,
 * then digit by digit.
 */
static sxml_ordering sxml_canonical_texts_compare(sxml_byte_view first,
                                                  sxml_byte_view second) {
    bool first_is_negative = first.byte_count != 0 && first.bytes[0] == '-';
    bool second_is_negative = second.byte_count != 0 && second.bytes[0] == '-';
    if (first_is_negative != second_is_negative) {
        return first_is_negative ? SXML_ORDERING_LESS : SXML_ORDERING_GREATER;
    }
    sxml_byte_view first_magnitude = first;
    sxml_byte_view second_magnitude = second;
    if (first_is_negative) {
        first_magnitude.bytes += 1;
        first_magnitude.byte_count -= 1;
        second_magnitude.bytes += 1;
        second_magnitude.byte_count -= 1;
    }
    size_t first_integer_count =
        sxml_canonical_integer_digit_count(first_magnitude);
    size_t second_integer_count =
        sxml_canonical_integer_digit_count(second_magnitude);
    sxml_ordering magnitude_ordering = SXML_ORDERING_EQUAL;
    if (first_integer_count != second_integer_count) {
        magnitude_ordering = first_integer_count < second_integer_count
                                 ? SXML_ORDERING_LESS
                                 : SXML_ORDERING_GREATER;
    } else {
        /* Same integer length: the texts align digit for digit, "." at the
           same place, and a longer fraction is larger. */
        magnitude_ordering =
            sxml_bytes_compare(first_magnitude, second_magnitude);
    }
    return first_is_negative ? sxml_ordering_reverse(magnitude_ordering)
                             : magnitude_ordering;
}

/* ------------------------------------------------------- Decimal cells */

/* True when integer is exactly some double. */
static bool sxml_integer_equals_a_double(int64_t integer) {
    double double_number = (double)integer;
    /* 2^63 is the first double above every int64_t. */
    if (double_number >= 9223372036854775808.0) {
        return false;
    }
    return (int64_t)double_number == integer;
}

/* True when integer and double_number are the same number. */
static bool sxml_integer_equals_double(int64_t integer, double double_number) {
    if (!(double_number >= -9223372036854775808.0 &&
          double_number < 9223372036854775808.0)) {
        /* NaN, the infinities and doubles beyond int64_t. */
        return false;
    }
    int64_t truncated = (int64_t)double_number;
    return (double)truncated == double_number && truncated == integer;
}

/*
 * Makes a decimal cell that takes *decimal (on success only): writes its
 * canonical text after the cell and finds whether it is exactly a double.
 */
static sxml_status sxml_decimal_cell_make(sxml_item_heap *item_heap,
                                          sxml_decimal *decimal,
                                          sxml_atomic_cell **atomic_cell) {
    size_t text_byte_count = sxml_decimal_canonical_text_byte_count(decimal);
    sxml_atomic_cell *new_cell = NULL;
    sxml_status status =
        sxml_atomic_cell_allocate(item_heap, text_byte_count, &new_cell);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_byte_view canonical_text = {.bytes = NULL, .byte_count = 0};
    status = sxml_decimal_write_canonical_text(
        decimal,
        (sxml_memory_view){.bytes = new_cell->trailing_bytes,
                           .byte_count = text_byte_count},
        &canonical_text);
    if (status != SXML_STATUS_OK) {
        sxml_item_heap_discard_cell(&new_cell->cell);
        return status;
    }
    double nearest_double = sxml_decimal_to_double(decimal);
    bool equals_double = false;
    /* A double's exact text never exceeds the capacity, so a longer
       decimal cannot equal one. */
    if (nearest_double - nearest_double == 0.0 &&
        canonical_text.byte_count <= SXML_EXACT_DOUBLE_TEXT_CAPACITY) {
        uint8_t exact_text_bytes[SXML_EXACT_DOUBLE_TEXT_CAPACITY];
        sxml_byte_view exact_text =
            sxml_double_write_exact_text(
            nearest_double, (sxml_memory_view){.bytes = exact_text_bytes,
                                  .byte_count = sizeof(exact_text_bytes)});
        equals_double = sxml_byte_view_equals(exact_text, canonical_text);
    }
    new_cell->datum_kind = SXML_DATUM_KIND_DECIMAL;
    new_cell->contents.decimal = (sxml_decimal_contents){
        .decimal = *decimal,
        .canonical_text = canonical_text,
        .nearest_double = nearest_double,
        .equals_double = equals_double,
    };
    *atomic_cell = new_cell;
    return SXML_STATUS_OK;
}

const sxml_decimal *sxml_atomic_item_get_decimal(sxml_item item) {
    if (item.representation != SXML_ITEM_REPRESENTATION_ATOMIC_CELL ||
        sxml_atomic_cell_of(item)->datum_kind != SXML_DATUM_KIND_DECIMAL) {
        return NULL;
    }
    return &sxml_atomic_cell_of(item)->contents.decimal.decimal;
}

/*
 * Whether bounds allow a value beyond int64_t with the sign of decimal
 * (xs:unsignedLong's upper bound is checked on the canonical text).
 */
static bool sxml_big_integer_is_within_bounds(sxml_integer_bounds bounds,
                                              const sxml_decimal *decimal) {
    return sxml_decimal_is_negative(decimal) ? bounds.allows_beyond_minimum
                                             : bounds.allows_beyond_maximum;
}

/* The largest xs:unsignedLong: 2^64 - 1 (XSD 1.1 Part 2, 3.4.21). */
static const sxml_byte_view sxml_unsigned_long_maximum_text =
    SXML_BYTE_VIEW_INITIALIZER("18446744073709551615");

sxml_status sxml_atomic_item_make_taking_decimal(
    sxml_item_heap *item_heap, const sxml_decimal_item_request *request,
    sxml_item *item) {
    sxml_atomic_type_facts facts;
    sxml_status status = sxml_atomic_type_resolve(
        sxml_item_heap_get_type_registry(item_heap), request->type, &facts);
    if (status != SXML_STATUS_OK ||
        facts.primitive_type != SXML_TYPE_XS_DECIMAL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_integer_bounds bounds;
    bool is_integer_type = sxml_integer_type_bounds(facts.builtin_type, &bounds);
    if (is_integer_type) {
        if (!sxml_decimal_is_integer(request->decimal)) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        int64_t integer = 0;
        status = sxml_decimal_to_integer(request->decimal, &integer);
        if (status == SXML_STATUS_OK) {
            if (integer < bounds.minimum || integer > bounds.maximum) {
                return SXML_STATUS_INVALID_ARGUMENT;
            }
            sxml_decimal_finalize(request->decimal);
            *item = sxml_integer_item(request->type, integer);
            return SXML_STATUS_OK;
        }
        if (!sxml_big_integer_is_within_bounds(bounds, request->decimal)) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
    }
    sxml_atomic_cell *atomic_cell = NULL;
    status = sxml_decimal_cell_make(item_heap, request->decimal, &atomic_cell);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (is_integer_type && facts.builtin_type == SXML_TYPE_XS_UNSIGNED_LONG &&
        sxml_canonical_texts_compare(
            atomic_cell->contents.decimal.canonical_text,
            sxml_unsigned_long_maximum_text) == SXML_ORDERING_GREATER) {
        /* Discarding skips release_contents, so the decimal stays with
           the caller, as on every failure. */
        sxml_item_heap_discard_cell(&atomic_cell->cell);
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *item = sxml_atomic_cell_item(atomic_cell, request->type,
                                  SXML_TYPE_XS_DECIMAL);
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Making */

/* What the make functions of each datum kind work with. */
typedef struct sxml_atomic_making {
    sxml_item_heap *item_heap;
    const sxml_atomic_item_request *request;
    sxml_atomic_type_facts facts;
} sxml_atomic_making;

/* An xs:decimal-family item from an integer or decimal datum. */
static sxml_status sxml_atomic_item_make_decimal_family(
    const sxml_atomic_making *making, sxml_item *item) {
    sxml_item_heap *item_heap = making->item_heap;
    const sxml_atomic_item_request *request = making->request;
    sxml_atomic_type_facts facts = making->facts;
    const sxml_decimal_configuration *configuration =
        sxml_item_heap_get_decimal_configuration(item_heap);
    sxml_integer_bounds bounds;
    bool is_integer_type = sxml_integer_type_bounds(facts.builtin_type, &bounds);
    sxml_decimal decimal;
    sxml_status status = SXML_STATUS_OK;
    if (request->datum.datum_kind == SXML_DATUM_KIND_INTEGER) {
        int64_t integer = request->datum.datum_details.integer;
        if (is_integer_type) {
            if (integer < bounds.minimum || integer > bounds.maximum) {
                return SXML_STATUS_INVALID_ARGUMENT;
            }
            *item = sxml_integer_item(request->type, integer);
            return SXML_STATUS_OK;
        }
        /* xs:decimal is always a cell (D36). */
        status = sxml_decimal_from_integer(configuration, integer, &decimal);
    } else if (request->datum.datum_kind == SXML_DATUM_KIND_DECIMAL) {
        status = sxml_decimal_parse(configuration,
                                    request->datum.datum_details.decimal_text,
                                    &decimal);
        if (status == SXML_STATUS_MALFORMED_INPUT) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
    } else {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_atomic_item_make_taking_decimal(
        item_heap,
        &(sxml_decimal_item_request){.type = request->type,
                                     .decimal = &decimal},
        item);
    if (status != SXML_STATUS_OK) {
        sxml_decimal_finalize(&decimal);
    }
    return status;
}

static sxml_status sxml_atomic_item_make_string(
    const sxml_atomic_making *making, sxml_item *item) {
    sxml_item_heap *item_heap = making->item_heap;
    const sxml_atomic_item_request *request = making->request;
    sxml_atomic_type_facts facts = making->facts;
    sxml_byte_view utf8 = request->datum.datum_details.string.utf8;
    /* Revalidated: the counts are recomputed here rather than trusted, and
       ill-formed text never enters the heap. */
    sxml_string string;
    if (sxml_string_from_utf8(utf8, &string) != SXML_STATUS_OK ||
        !sxml_string_is_in_value_space(facts.builtin_type, string)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_atomic_cell *atomic_cell = NULL;
    sxml_status status =
        sxml_atomic_cell_allocate(item_heap, utf8.byte_count, &atomic_cell);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (utf8.byte_count != 0) {
        memcpy(atomic_cell->trailing_bytes, utf8.bytes, utf8.byte_count);
        string.utf8.bytes = atomic_cell->trailing_bytes;
    } else {
        string.utf8.bytes = NULL;
    }
    atomic_cell->datum_kind = SXML_DATUM_KIND_STRING;
    atomic_cell->contents.string = string;
    *item = sxml_atomic_cell_item(atomic_cell, request->type,
                                  facts.primitive_type);
    return SXML_STATUS_OK;
}

static sxml_status sxml_atomic_item_make_binary(
    const sxml_atomic_making *making, sxml_item *item) {
    sxml_item_heap *item_heap = making->item_heap;
    const sxml_atomic_item_request *request = making->request;
    sxml_atomic_type_facts facts = making->facts;
    sxml_byte_view octets = request->datum.datum_details.binary;
    if (octets.bytes == NULL && octets.byte_count != 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_atomic_cell *atomic_cell = NULL;
    sxml_status status =
        sxml_atomic_cell_allocate(item_heap, octets.byte_count, &atomic_cell);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_byte_view copy = {.bytes = NULL, .byte_count = octets.byte_count};
    if (octets.byte_count != 0) {
        memcpy(atomic_cell->trailing_bytes, octets.bytes, octets.byte_count);
        copy.bytes = atomic_cell->trailing_bytes;
    }
    atomic_cell->datum_kind = SXML_DATUM_KIND_BINARY;
    atomic_cell->contents.binary = copy;
    *item = sxml_atomic_cell_item(atomic_cell, request->type,
                                  facts.primitive_type);
    return SXML_STATUS_OK;
}

/* A date/time or duration item: the datum is copied into the cell. */
static sxml_status sxml_atomic_item_make_temporal(
    const sxml_atomic_making *making, sxml_item *item) {
    sxml_item_heap *item_heap = making->item_heap;
    const sxml_atomic_item_request *request = making->request;
    sxml_atomic_type_facts facts = making->facts;
    bool is_duration = facts.primitive_type == SXML_TYPE_XS_DURATION;
    sxml_datum_kind datum_kind =
        is_duration ? SXML_DATUM_KIND_DURATION : SXML_DATUM_KIND_DATE_TIME;
    if (request->datum.datum_kind != datum_kind) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    bool is_in_value_space =
        is_duration ? sxml_duration_is_in_value_space(
                          facts, &request->datum.datum_details.duration)
                    : sxml_date_time_is_in_value_space(
                          facts, &request->datum.datum_details.date_time);
    if (!is_in_value_space) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_atomic_cell *atomic_cell = NULL;
    sxml_status status = sxml_atomic_cell_allocate(item_heap, 0, &atomic_cell);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    atomic_cell->datum_kind = datum_kind;
    if (is_duration) {
        atomic_cell->contents.duration = request->datum.datum_details.duration;
    } else {
        atomic_cell->contents.date_time =
            request->datum.datum_details.date_time;
    }
    *item = sxml_atomic_cell_item(atomic_cell, request->type,
                                  facts.primitive_type);
    return SXML_STATUS_OK;
}

sxml_status sxml_atomic_item_make(sxml_item_heap *item_heap,
                                  const sxml_atomic_item_request *request,
                                  sxml_item *item) {
    sxml_atomic_type_facts facts;
    sxml_status status = sxml_atomic_type_resolve(
        sxml_item_heap_get_type_registry(item_heap), request->type, &facts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_atomic_datum *datum = &request->datum;
    const sxml_atomic_making making = {
        .item_heap = item_heap, .request = request, .facts = facts};
    switch ((uint32_t)facts.primitive_type) {
    case SXML_TYPE_XS_BOOLEAN:
        if (datum->datum_kind != SXML_DATUM_KIND_BOOLEAN) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        *item = sxml_boolean_item((sxml_typed_boolean){
            .type = request->type,
            .boolean = datum->datum_details.boolean});
        return SXML_STATUS_OK;
    case SXML_TYPE_XS_DECIMAL:
        return sxml_atomic_item_make_decimal_family(&making,
                                                    item);
    case SXML_TYPE_XS_FLOAT: {
        if (datum->datum_kind != SXML_DATUM_KIND_FLOAT) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        sxml_item float_item = sxml_inline_atomic_item(
            SXML_ITEM_REPRESENTATION_FLOAT, request->type,
            SXML_TYPE_XS_FLOAT);
        float_item.payload.float_number = datum->datum_details.float_number;
        *item = float_item;
        return SXML_STATUS_OK;
    }
    case SXML_TYPE_XS_DOUBLE:
        if (datum->datum_kind != SXML_DATUM_KIND_DOUBLE) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        *item = sxml_double_item(request->type,
                                 datum->datum_details.double_number);
        return SXML_STATUS_OK;
    case SXML_TYPE_XS_STRING:
    case SXML_TYPE_XS_ANY_URI:
    case SXML_TYPE_XS_UNTYPED_ATOMIC:
        if (datum->datum_kind != SXML_DATUM_KIND_STRING) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        return sxml_atomic_item_make_string(&making, item);
    case SXML_TYPE_XS_QNAME:
    case SXML_TYPE_XS_NOTATION: {
        if (datum->datum_kind != SXML_DATUM_KIND_QNAME ||
            datum->datum_details.qname.expanded_name.index == 0) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        sxml_item qname_item = sxml_inline_atomic_item(
            SXML_ITEM_REPRESENTATION_QNAME, request->type,
            facts.primitive_type);
        qname_item.payload.bits =
            (uint64_t)datum->datum_details.qname.expanded_name.index |
            ((uint64_t)datum->datum_details.qname.namespace_prefix.index
             << 32);
        *item = qname_item;
        return SXML_STATUS_OK;
    }
    case SXML_TYPE_XS_DURATION:
    case SXML_TYPE_XS_DATE_TIME:
    case SXML_TYPE_XS_TIME:
    case SXML_TYPE_XS_DATE:
    case SXML_TYPE_XS_G_YEAR_MONTH:
    case SXML_TYPE_XS_G_YEAR:
    case SXML_TYPE_XS_G_MONTH_DAY:
    case SXML_TYPE_XS_G_DAY:
    case SXML_TYPE_XS_G_MONTH:
        return sxml_atomic_item_make_temporal(&making,
                                              item);
    case SXML_TYPE_XS_HEX_BINARY:
    case SXML_TYPE_XS_BASE64_BINARY:
        if (datum->datum_kind != SXML_DATUM_KIND_BINARY) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        return sxml_atomic_item_make_binary(&making, item);
    default:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
}

/* ------------------------------------------------------------- Datums */

static sxml_qname_datum sxml_qname_item_datum(sxml_item item) {
    return (sxml_qname_datum){
        .expanded_name = {.index = (uint32_t)(item.payload.bits &
                                              UINT32_MAX)},
        .namespace_prefix = {.index = (uint32_t)(item.payload.bits >> 32)},
    };
}

sxml_status sxml_atomic_item_get_datum(sxml_item item,
                                       sxml_atomic_datum *datum) {
    switch (sxml_item_get_representation(item)) {
    case SXML_ITEM_REPRESENTATION_BOOLEAN:
        *datum = (sxml_atomic_datum){
            .datum_kind = SXML_DATUM_KIND_BOOLEAN,
            .datum_details = {.boolean = item.payload.integer != 0}};
        return SXML_STATUS_OK;
    case SXML_ITEM_REPRESENTATION_INTEGER:
        *datum = (sxml_atomic_datum){
            .datum_kind = SXML_DATUM_KIND_INTEGER,
            .datum_details = {.integer = item.payload.integer}};
        return SXML_STATUS_OK;
    case SXML_ITEM_REPRESENTATION_FLOAT:
        *datum = (sxml_atomic_datum){
            .datum_kind = SXML_DATUM_KIND_FLOAT,
            .datum_details = {.float_number = item.payload.float_number}};
        return SXML_STATUS_OK;
    case SXML_ITEM_REPRESENTATION_DOUBLE:
        *datum = (sxml_atomic_datum){
            .datum_kind = SXML_DATUM_KIND_DOUBLE,
            .datum_details = {.double_number = item.payload.double_number}};
        return SXML_STATUS_OK;
    case SXML_ITEM_REPRESENTATION_QNAME:
        *datum = (sxml_atomic_datum){
            .datum_kind = SXML_DATUM_KIND_QNAME,
            .datum_details = {.qname = sxml_qname_item_datum(item)}};
        return SXML_STATUS_OK;
    case SXML_ITEM_REPRESENTATION_ATOMIC_CELL: {
        const sxml_atomic_cell *atomic_cell = sxml_atomic_cell_of(item);
        sxml_atomic_datum cell_datum = {.datum_kind = atomic_cell->datum_kind};
        switch (atomic_cell->datum_kind) {
        case SXML_DATUM_KIND_DECIMAL:
            /* The canonical text was made with the cell, so this read
               never allocates (D36 allows making it lazily). */
            cell_datum.datum_details.decimal_text =
                atomic_cell->contents.decimal.canonical_text;
            break;
        case SXML_DATUM_KIND_STRING:
            cell_datum.datum_details.string = atomic_cell->contents.string;
            break;
        case SXML_DATUM_KIND_DATE_TIME:
            cell_datum.datum_details.date_time =
                atomic_cell->contents.date_time;
            break;
        case SXML_DATUM_KIND_DURATION:
            cell_datum.datum_details.duration =
                atomic_cell->contents.duration;
            break;
        case SXML_DATUM_KIND_BINARY:
            cell_datum.datum_details.binary = atomic_cell->contents.binary;
            break;
        case SXML_DATUM_KIND_BOOLEAN:
        case SXML_DATUM_KIND_INTEGER:
        case SXML_DATUM_KIND_FLOAT:
        case SXML_DATUM_KIND_DOUBLE:
        case SXML_DATUM_KIND_QNAME:
            /* Always inline. */
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        *datum = cell_datum;
        return SXML_STATUS_OK;
    }
    case SXML_ITEM_REPRESENTATION_NONE:
    case SXML_ITEM_REPRESENTATION_NODE:
    case SXML_ITEM_REPRESENTATION_JSON_NODE_CELL:
    case SXML_ITEM_REPRESENTATION_FUNCTION_CELL:
    case SXML_ITEM_REPRESENTATION_MAP_CELL:
    case SXML_ITEM_REPRESENTATION_ARRAY_CELL:
    case SXML_ITEM_REPRESENTATION_VECTOR_CELL:
    case SXML_ITEM_REPRESENTATION_INTEGER_RANGE_CELL:
    case SXML_ITEM_REPRESENTATION_CONCATENATION_CELL:
    case SXML_ITEM_REPRESENTATION_LAZY_CELL:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* ------------------------------------------------------------- Numbers */

/* The three forms a numeric item is compared in; xs:float widens to
   xs:double exactly. */
typedef enum sxml_number_form {
    SXML_NUMBER_FORM_INTEGER,
    SXML_NUMBER_FORM_DOUBLE,
    SXML_NUMBER_FORM_DECIMAL,
} sxml_number_form;

typedef struct sxml_number {
    sxml_number_form number_form;
    int64_t integer;
    double double_number;
    const sxml_decimal_contents *decimal;
} sxml_number;

static sxml_number sxml_number_of(sxml_item item) {
    sxml_number number = {.number_form = SXML_NUMBER_FORM_INTEGER,
                          .integer = 0,
                          .double_number = 0.0,
                          .decimal = NULL};
    switch (sxml_item_get_representation(item)) {
    case SXML_ITEM_REPRESENTATION_INTEGER:
        number.integer = item.payload.integer;
        break;
    case SXML_ITEM_REPRESENTATION_FLOAT:
        number.number_form = SXML_NUMBER_FORM_DOUBLE;
        number.double_number = (double)item.payload.float_number;
        break;
    case SXML_ITEM_REPRESENTATION_DOUBLE:
        number.number_form = SXML_NUMBER_FORM_DOUBLE;
        number.double_number = item.payload.double_number;
        break;
    case SXML_ITEM_REPRESENTATION_ATOMIC_CELL:
        number.number_form = SXML_NUMBER_FORM_DECIMAL;
        number.decimal = &sxml_atomic_cell_of(item)->contents.decimal;
        break;
    case SXML_ITEM_REPRESENTATION_NONE:
    case SXML_ITEM_REPRESENTATION_BOOLEAN:
    case SXML_ITEM_REPRESENTATION_QNAME:
    case SXML_ITEM_REPRESENTATION_NODE:
    case SXML_ITEM_REPRESENTATION_JSON_NODE_CELL:
    case SXML_ITEM_REPRESENTATION_FUNCTION_CELL:
    case SXML_ITEM_REPRESENTATION_MAP_CELL:
    case SXML_ITEM_REPRESENTATION_ARRAY_CELL:
    case SXML_ITEM_REPRESENTATION_VECTOR_CELL:
    case SXML_ITEM_REPRESENTATION_INTEGER_RANGE_CELL:
    case SXML_ITEM_REPRESENTATION_CONCATENATION_CELL:
    case SXML_ITEM_REPRESENTATION_LAZY_CELL:
        /* Callers pass numeric items only. */
        break;
    }
    return number;
}

static bool sxml_double_is_nan(double double_number) {
    return double_number != double_number;
}

/* Orders two doubles; NaN first and equal to itself (F&O 4.0, fn:compare). */
static sxml_ordering sxml_doubles_compare(double first, double second) {
    bool first_is_nan = sxml_double_is_nan(first);
    bool second_is_nan = sxml_double_is_nan(second);
    if (first_is_nan || second_is_nan) {
        if (first_is_nan && second_is_nan) {
            return SXML_ORDERING_EQUAL;
        }
        return first_is_nan ? SXML_ORDERING_LESS : SXML_ORDERING_GREATER;
    }
    if (first < second) {
        return SXML_ORDERING_LESS;
    }
    return first > second ? SXML_ORDERING_GREATER : SXML_ORDERING_EQUAL;
}

static sxml_ordering sxml_integers_compare(int64_t first, int64_t second) {
    if (first < second) {
        return SXML_ORDERING_LESS;
    }
    return first > second ? SXML_ORDERING_GREATER : SXML_ORDERING_EQUAL;
}

/* Orders an integer and a double by exact value; NaN is less. */
static sxml_ordering sxml_integer_double_compare(int64_t integer,
                                                 double double_number) {
    if (sxml_double_is_nan(double_number)) {
        return SXML_ORDERING_GREATER;
    }
    if (double_number >= 9223372036854775808.0) {
        return SXML_ORDERING_LESS;
    }
    if (double_number < -9223372036854775808.0) {
        return SXML_ORDERING_GREATER;
    }
    /* In range, so the conversion truncates toward zero, defined. */
    int64_t truncated = (int64_t)double_number;
    if (integer != truncated) {
        return sxml_integers_compare(integer, truncated);
    }
    /* Equal integer parts: the fraction of the double decides. */
    return sxml_doubles_compare((double)truncated, double_number);
}

/* Orders a decimal and a finite or infinite double by exact value. */
static sxml_ordering sxml_decimal_double_compare(
    const sxml_decimal_contents *decimal, double double_number) {
    if (sxml_double_is_nan(double_number)) {
        return SXML_ORDERING_GREATER;
    }
    if (double_number - double_number != 0.0) {
        /* An infinity. */
        return double_number > 0 ? SXML_ORDERING_LESS : SXML_ORDERING_GREATER;
    }
    uint8_t exact_text_bytes[SXML_EXACT_DOUBLE_TEXT_CAPACITY];
    return sxml_canonical_texts_compare(
        decimal->canonical_text,
        sxml_double_write_exact_text(
            double_number, (sxml_memory_view){.bytes = exact_text_bytes,
                                  .byte_count = sizeof(exact_text_bytes)}));
}

static sxml_ordering sxml_decimal_integer_compare(
    const sxml_decimal_contents *decimal, int64_t integer) {
    uint8_t integer_text_bytes[SXML_INTEGER_TEXT_CAPACITY];
    return sxml_canonical_texts_compare(
        decimal->canonical_text,
        sxml_integer_write_text(
            integer, (sxml_memory_view){.bytes = integer_text_bytes,
                                  .byte_count = sizeof(integer_text_bytes)}));
}

/* sxml_numbers_compare for first.number_form at most second.number_form. */
static sxml_ordering sxml_ordered_forms_compare(sxml_number first,
                                                sxml_number second) {
    switch (first.number_form) {
    case SXML_NUMBER_FORM_INTEGER:
        switch (second.number_form) {
        case SXML_NUMBER_FORM_INTEGER:
            return sxml_integers_compare(first.integer, second.integer);
        case SXML_NUMBER_FORM_DOUBLE:
            return sxml_integer_double_compare(first.integer,
                                               second.double_number);
        case SXML_NUMBER_FORM_DECIMAL:
            return sxml_ordering_reverse(
                sxml_decimal_integer_compare(second.decimal, first.integer));
        }
        break;
    case SXML_NUMBER_FORM_DOUBLE:
        if (second.number_form == SXML_NUMBER_FORM_DOUBLE) {
            return sxml_doubles_compare(first.double_number,
                                        second.double_number);
        }
        return sxml_ordering_reverse(sxml_decimal_double_compare(
            second.decimal, first.double_number));
    case SXML_NUMBER_FORM_DECIMAL:
        return sxml_canonical_texts_compare(first.decimal->canonical_text,
                                            second.decimal->canonical_text);
    }
    return SXML_ORDERING_EQUAL;
}

/*
 * Orders two numbers by exact magnitude across the numeric types (F&O 4.0,
 * section 2.2.1 and fn:compare), NaN first and equal to itself. Never
 * allocates. Swaps the operands instead of recursing (D71).
 */
static sxml_ordering sxml_numbers_compare(sxml_number first,
                                          sxml_number second) {
    if (first.number_form > second.number_form) {
        return sxml_ordering_reverse(
            sxml_ordered_forms_compare(second, first));
    }
    return sxml_ordered_forms_compare(first, second);
}

/* Equality by exact magnitude; NaN equals NaN (fn:atomic-equal). */
static bool sxml_numbers_are_equal(sxml_number first, sxml_number second) {
    if (first.number_form > second.number_form) {
        sxml_number swapped = first;
        first = second;
        second = swapped;
    }
    if (first.number_form == SXML_NUMBER_FORM_INTEGER &&
        second.number_form == SXML_NUMBER_FORM_DOUBLE) {
        return sxml_integer_equals_double(first.integer, second.double_number);
    }
    if (first.number_form == SXML_NUMBER_FORM_DOUBLE &&
        second.number_form == SXML_NUMBER_FORM_DECIMAL) {
        /* equals_double: the decimal is exactly its nearest double, so it
           equals a double exactly when that is the double. */
        return second.decimal->equals_double &&
               second.decimal->nearest_double == first.double_number;
    }
    return sxml_numbers_compare(first, second) == SXML_ORDERING_EQUAL;
}

/* -------------------------------------------------------------- Hashes */

/* The finalizer of splitmix64: spreads every input bit over the output. */
static uint64_t sxml_hash_mix(uint64_t value) {
    value ^= value >> 30;
    value *= UINT64_C(0xBF58476D1CE4E5B9);
    value ^= value >> 27;
    value *= UINT64_C(0x94D049BB133111EB);
    value ^= value >> 31;
    return value;
}

static uint64_t sxml_hash_combine(uint64_t hash, uint64_t value) {
    return sxml_hash_mix(hash ^ (value + UINT64_C(0x9E3779B97F4A7C15) +
                                 (hash << 6) + (hash >> 2)));
}

/* FNV-1a over the bytes, then mixed (unseeded, as D54: hash flooding is
   bounded by the heap's byte limit). */
static uint64_t sxml_hash_bytes(uint64_t seed, sxml_byte_view bytes) {
    uint64_t hash = UINT64_C(0xCBF29CE484222325) ^ seed;
    for (size_t byte_index = 0; byte_index < bytes.byte_count;
         byte_index += 1) {
        hash ^= bytes.bytes[byte_index];
        hash *= UINT64_C(0x100000001B3);
    }
    return sxml_hash_mix(hash);
}

static uint64_t sxml_hash_double(uint64_t seed, double double_number) {
    if (sxml_double_is_nan(double_number)) {
        return sxml_hash_combine(seed, UINT64_C(0x7FF8000000000000));
    }
    /* +0 and -0 are equal, so they hash alike. */
    double normalized = double_number == 0.0 ? 0.0 : double_number;
    uint64_t bits = 0;
    memcpy(&bits, &normalized, sizeof(bits));
    return sxml_hash_combine(seed, bits);
}

/*
 * The hash of a number. A value that is exactly a double hashes as that
 * double; every other value (integers and decimals no double equals)
 * hashes its canonical text. Equal numbers have the same exact value, so
 * they fall in the same case with the same input: 1, 1.0 and 1.0e0 all
 * hash as the double 1.
 */
static uint64_t sxml_number_hash(sxml_number number) {
    uint64_t seed = SXML_ATOMIC_FAMILY_NUMERIC;
    switch (number.number_form) {
    case SXML_NUMBER_FORM_INTEGER: {
        if (sxml_integer_equals_a_double(number.integer)) {
            return sxml_hash_double(seed, (double)number.integer);
        }
        uint8_t integer_text_bytes[SXML_INTEGER_TEXT_CAPACITY];
        return sxml_hash_bytes(
            seed, sxml_integer_write_text(
            number.integer, (sxml_memory_view){.bytes = integer_text_bytes,
                                  .byte_count = sizeof(integer_text_bytes)}));
    }
    case SXML_NUMBER_FORM_DOUBLE:
        return sxml_hash_double(seed, number.double_number);
    case SXML_NUMBER_FORM_DECIMAL:
        if (number.decimal->equals_double) {
            return sxml_hash_double(seed, number.decimal->nearest_double);
        }
        return sxml_hash_bytes(seed, number.decimal->canonical_text);
    }
    return seed;
}

/* ---------------------------------------------------------- Date/time */

/*
 * A date/time value as a point on one time line: year, day of the year
 * (from 1) and nanosecond of the day, after subtracting a timezone offset.
 * Absent components take the values of the reference date 1972-12-31 or
 * the first month or day (F&O 4.0, section 9.7.1, and the "starting
 * instant" of XSD 1.1 Part 2, D.2.1); values of one primitive type always
 * fill the same components, so keys compare and hash consistently.
 */
typedef struct sxml_date_time_key {
    int64_t year;
    uint32_t day_of_year;
    uint64_t nanosecond_of_day;
} sxml_date_time_key;

static const uint64_t sxml_nanoseconds_per_day = UINT64_C(86400000000000);

static uint32_t sxml_days_in_year(int64_t year) {
    return sxml_year_is_leap(year) ? 366U : 365U;
}

/* A date/time datum and the offset (minutes east of UTC) to apply. */
typedef struct sxml_date_time_placement {
    sxml_builtin_type primitive_type;
    const sxml_date_time *date_time;
    int16_t timezone_offset_minutes;
} sxml_date_time_placement;

static sxml_date_time_key sxml_date_time_key_of(
    sxml_date_time_placement placement) {
    const sxml_date_time *date_time = placement.date_time;
    sxml_date_time_components components =
        sxml_date_time_components_of(placement.primitive_type);
    int64_t year = components.has_year ? date_time->year : sxml_reference_year;
    bool is_time = placement.primitive_type == SXML_TYPE_XS_TIME;
    uint8_t month = components.has_month               ? date_time->month
                    : components.has_day || is_time ? 12
                                                       : 1;
    uint8_t day = components.has_day ? date_time->day : is_time ? 31 : 1;
    uint32_t day_of_year = day;
    for (uint8_t month_index = 1; month_index < month; month_index += 1) {
        day_of_year += sxml_days_in_month(year, month_index);
    }
    int64_t nanosecond_of_day =
        (((int64_t)date_time->hour * 60 + date_time->minute) * 60 +
         date_time->second) *
            1000000000 +
        (int64_t)date_time->nanosecond;
    /* Local time minus the offset is UTC; at most 14 hours, so at most one
       day earlier or later. */
    nanosecond_of_day -=
        (int64_t)placement.timezone_offset_minutes * 60 * 1000000000;
    if (nanosecond_of_day < 0) {
        nanosecond_of_day += (int64_t)sxml_nanoseconds_per_day;
        if (day_of_year > 1) {
            day_of_year -= 1;
        } else if (year > INT64_MIN) {
            year -= 1;
            day_of_year = sxml_days_in_year(year);
        }
    } else if (nanosecond_of_day >= (int64_t)sxml_nanoseconds_per_day) {
        nanosecond_of_day -= (int64_t)sxml_nanoseconds_per_day;
        if (day_of_year < sxml_days_in_year(year)) {
            day_of_year += 1;
        } else if (year < INT64_MAX) {
            year += 1;
            day_of_year = 1;
        }
    }
    return (sxml_date_time_key){
        .year = year,
        .day_of_year = day_of_year,
        .nanosecond_of_day = (uint64_t)nanosecond_of_day,
    };
}

static sxml_ordering sxml_date_time_keys_compare(sxml_date_time_key first,
                                                 sxml_date_time_key second) {
    if (first.year != second.year) {
        return sxml_integers_compare(first.year, second.year);
    }
    if (first.day_of_year != second.day_of_year) {
        return first.day_of_year < second.day_of_year ? SXML_ORDERING_LESS
                                                      : SXML_ORDERING_GREATER;
    }
    if (first.nanosecond_of_day != second.nanosecond_of_day) {
        return first.nanosecond_of_day < second.nanosecond_of_day
                   ? SXML_ORDERING_LESS
                   : SXML_ORDERING_GREATER;
    }
    return SXML_ORDERING_EQUAL;
}

/* The key of a date/time item: its own timezone, or offset_minutes. */
static sxml_date_time_key sxml_date_time_item_key(sxml_item item,
                                                  int16_t offset_minutes) {
    const sxml_date_time *date_time =
        &sxml_atomic_cell_of(item)->contents.date_time;
    return sxml_date_time_key_of((sxml_date_time_placement){
        .primitive_type = sxml_atomic_item_primitive(item),
        .date_time = date_time,
        .timezone_offset_minutes =
            (int16_t)(date_time->has_timezone
                          ? date_time->timezone_offset_minutes
                          : offset_minutes),
    });
}

static bool sxml_date_time_item_has_timezone(sxml_item item) {
    return sxml_atomic_cell_of(item)->contents.date_time.has_timezone;
}

/* Durations order by months, then seconds (F&O 4.0, fn:compare). */
static sxml_ordering sxml_durations_compare(const sxml_duration *first,
                                            const sxml_duration *second) {
    if (first->months != second->months) {
        return sxml_integers_compare(first->months, second->months);
    }
    if (first->seconds != second->seconds) {
        return sxml_integers_compare(first->seconds, second->seconds);
    }
    return sxml_integers_compare(first->nanoseconds, second->nanoseconds);
}

/* ------------------------------------------------- Equality and hashing */

static sxml_byte_view sxml_atomic_item_text(sxml_item item) {
    return sxml_atomic_cell_of(item)->contents.string.utf8;
}

static sxml_byte_view sxml_atomic_item_octets(sxml_item item) {
    return sxml_atomic_cell_of(item)->contents.binary;
}

bool sxml_atomic_item_is_atomic_equal(sxml_item first, sxml_item second) {
    if (!sxml_item_is_atomic(first) || !sxml_item_is_atomic(second)) {
        return false;
    }
    uint32_t family = sxml_atomic_item_family(first);
    if (family != sxml_atomic_item_family(second)) {
        return false;
    }
    switch (family) {
    case SXML_ATOMIC_FAMILY_NUMERIC:
        return sxml_numbers_are_equal(sxml_number_of(first),
                                      sxml_number_of(second));
    case SXML_ATOMIC_FAMILY_STRING:
        return sxml_byte_view_equals(sxml_atomic_item_text(first),
                                     sxml_atomic_item_text(second));
    case SXML_ATOMIC_FAMILY_BINARY:
        return sxml_byte_view_equals(sxml_atomic_item_octets(first),
                                     sxml_atomic_item_octets(second));
    default:
        break;
    }
    sxml_builtin_type primitive_type = sxml_atomic_item_primitive(first);
    if (primitive_type == SXML_TYPE_XS_BOOLEAN) {
        return first.payload.integer == second.payload.integer;
    }
    if (primitive_type == SXML_TYPE_XS_QNAME ||
        primitive_type == SXML_TYPE_XS_NOTATION) {
        /* The prefix takes no part (XDM 4.0, section 5.6). */
        return sxml_qname_item_datum(first).expanded_name.index ==
               sxml_qname_item_datum(second).expanded_name.index;
    }
    if (primitive_type == SXML_TYPE_XS_DURATION) {
        return sxml_durations_compare(
                   &sxml_atomic_cell_of(first)->contents.duration,
                   &sxml_atomic_cell_of(second)->contents.duration) ==
               SXML_ORDERING_EQUAL;
    }
    if (sxml_primitive_is_date_time(primitive_type)) {
        /* A value with a timezone never equals one without (F&O 4.0,
           section 2.2.1); otherwise they are compared as instants. */
        if (sxml_date_time_item_has_timezone(first) !=
            sxml_date_time_item_has_timezone(second)) {
            return false;
        }
        return sxml_date_time_keys_compare(
                   sxml_date_time_item_key(first, 0),
                   sxml_date_time_item_key(second, 0)) == SXML_ORDERING_EQUAL;
    }
    return false;
}

uint64_t sxml_atomic_item_hash(sxml_item item) {
    if (!sxml_item_is_atomic(item)) {
        return 0;
    }
    uint32_t family = sxml_atomic_item_family(item);
    switch (family) {
    case SXML_ATOMIC_FAMILY_NUMERIC:
        return sxml_number_hash(sxml_number_of(item));
    case SXML_ATOMIC_FAMILY_STRING:
        return sxml_hash_bytes(family, sxml_atomic_item_text(item));
    case SXML_ATOMIC_FAMILY_BINARY:
        return sxml_hash_bytes(family, sxml_atomic_item_octets(item));
    default:
        break;
    }
    sxml_builtin_type primitive_type = sxml_atomic_item_primitive(item);
    if (primitive_type == SXML_TYPE_XS_BOOLEAN) {
        return sxml_hash_combine(family, (uint64_t)item.payload.integer);
    }
    if (primitive_type == SXML_TYPE_XS_QNAME ||
        primitive_type == SXML_TYPE_XS_NOTATION) {
        return sxml_hash_combine(
            family, sxml_qname_item_datum(item).expanded_name.index);
    }
    if (primitive_type == SXML_TYPE_XS_DURATION) {
        const sxml_duration *duration =
            &sxml_atomic_cell_of(item)->contents.duration;
        uint64_t hash = sxml_hash_combine(family, (uint64_t)duration->months);
        hash = sxml_hash_combine(hash, (uint64_t)duration->seconds);
        return sxml_hash_combine(hash, (uint64_t)duration->nanoseconds);
    }
    sxml_date_time_key key = sxml_date_time_item_key(item, 0);
    uint64_t hash = sxml_hash_combine(
        family, sxml_date_time_item_has_timezone(item) ? 1U : 0U);
    hash = sxml_hash_combine(hash, (uint64_t)key.year);
    hash = sxml_hash_combine(hash, key.day_of_year);
    return sxml_hash_combine(hash, key.nanosecond_of_day);
}

/* ------------------------------------------------------------- Compare */

static const sxml_static_error sxml_incomparable_items_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "fn:compare needs two atomic items of the same type family "
        "(F&O 4.0, fn:compare)"),
};

/* Orders two expanded names by namespace URI, then local name. */
static sxml_status sxml_qname_items_compare(const sxml_name_pool *name_pool,
                                            sxml_item_pair pair,
                                            sxml_ordering *ordering) {
    sxml_name_identifier first = sxml_qname_item_datum(pair.first).expanded_name;
    sxml_name_identifier second =
        sxml_qname_item_datum(pair.second).expanded_name;
    if (first.index == second.index) {
        *ordering = SXML_ORDERING_EQUAL;
        return SXML_STATUS_OK;
    }
    if (name_pool == NULL) {
        return SXML_STATUS_UNSUPPORTED_FEATURE;
    }
    sxml_name_parts first_parts;
    sxml_name_parts second_parts;
    sxml_byte_view first_uri;
    sxml_byte_view second_uri;
    sxml_status status =
        sxml_name_pool_get_name_parts(name_pool, first, &first_parts);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_name_parts(name_pool, second, &second_parts);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_namespace_uri(
            name_pool, first_parts.namespace_uri, &first_uri);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_namespace_uri(
            name_pool, second_parts.namespace_uri, &second_uri);
    }
    if (status != SXML_STATUS_OK) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* Byte order of UTF-8 is codepoint order. */
    sxml_ordering uri_ordering = sxml_bytes_compare(first_uri, second_uri);
    *ordering = uri_ordering != SXML_ORDERING_EQUAL
                    ? uri_ordering
                    : sxml_bytes_compare(first_parts.local_name,
                                         second_parts.local_name);
    return SXML_STATUS_OK;
}

sxml_status sxml_atomic_item_compare_in_heap(
    const sxml_item_heap *item_heap, const sxml_atomic_comparison *comparison,
    sxml_ordering *ordering) {
    sxml_item first = comparison->first;
    sxml_item second = comparison->second;
    if (first.representation == SXML_ITEM_REPRESENTATION_INTEGER &&
        second.representation == SXML_ITEM_REPRESENTATION_INTEGER) {
        /* Two inline integers (xs:integer and the types derived from it):
           their values, as the numeric family below orders them. */
        *ordering = sxml_integers_compare(first.payload.integer, second.payload.integer);
        return SXML_STATUS_OK;
    }
    if (!sxml_item_is_atomic(first) || !sxml_item_is_atomic(second) ||
        sxml_atomic_item_family(first) != sxml_atomic_item_family(second)) {
        return sxml_error_record_report(comparison->error_record,
                                        &sxml_incomparable_items_error);
    }
    uint32_t family = sxml_atomic_item_family(first);
    sxml_builtin_type primitive_type = sxml_atomic_item_primitive(first);
    sxml_ordering result = SXML_ORDERING_EQUAL;
    if (family == SXML_ATOMIC_FAMILY_NUMERIC) {
        result = sxml_numbers_compare(sxml_number_of(first),
                                      sxml_number_of(second));
    } else if (family == SXML_ATOMIC_FAMILY_STRING) {
        result = sxml_bytes_compare(sxml_atomic_item_text(first),
                                    sxml_atomic_item_text(second));
    } else if (family == SXML_ATOMIC_FAMILY_BINARY) {
        result = sxml_bytes_compare(sxml_atomic_item_octets(first),
                                    sxml_atomic_item_octets(second));
    } else if (primitive_type == SXML_TYPE_XS_BOOLEAN) {
        result = sxml_integers_compare(first.payload.integer,
                                       second.payload.integer);
    } else if (primitive_type == SXML_TYPE_XS_QNAME ||
               primitive_type == SXML_TYPE_XS_NOTATION) {
        /* The heap's pool, else the comparison's (D68). */
        sxml_status status = sxml_qname_items_compare(
            item_heap == NULL ? comparison->name_pool
                              : sxml_item_heap_get_name_pool(item_heap),
            (sxml_item_pair){.first = first, .second = second}, &result);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    } else if (primitive_type == SXML_TYPE_XS_DURATION) {
        result = sxml_durations_compare(
            &sxml_atomic_cell_of(first)->contents.duration,
            &sxml_atomic_cell_of(second)->contents.duration);
    } else {
        /* Date/time values without a timezone take the implicit one
           (F&O 4.0, section 9.7.1). */
        int16_t implicit = comparison->implicit_timezone_offset_minutes;
        result = sxml_date_time_keys_compare(
            sxml_date_time_item_key(first, implicit),
            sxml_date_time_item_key(second, implicit));
    }
    *ordering = result;
    return SXML_STATUS_OK;
}

sxml_status sxml_atomic_item_compare(const sxml_atomic_comparison *comparison,
                                     sxml_ordering *ordering) {
    sxml_status status =
        sxml_atomic_item_compare_in_heap(NULL, comparison, ordering);
    /* Without a name pool two different names cannot be ordered: the
       caller left out what xdm.h requires for QNames (D68). */
    return status == SXML_STATUS_UNSUPPORTED_FEATURE
               ? SXML_STATUS_INVALID_ARGUMENT
               : status;
}
