/*
 * Casting (src/cast_internal.h): which primitive types cast to which, the
 * conversions between them, whitespace processing, and target types that
 * are lists, unions, enumerations and choices.
 *
 * Golden file: src/memory.c.
 *
 * A cast starts from a source: an atomic item, or text that is cast as an
 * xs:string with that text would be (sxml_cast_make_value_from_text and
 * the members of a list). Sources of type xs:string, xs:untypedAtomic and
 * the types derived from xs:string are cast through the lexical form of
 * the target (src/cast_lexical.c); others through the conversion of their
 * datum; casts to the string types through the canonical text.
 *
 * Specification: F&O 4.0, section 19 "Casting": 19.1 (between primitive
 * types, the casting table), 19.2 (from xs:string and xs:untypedAtomic),
 * 19.3 (derived, union and list types); XPath 4.0, section 4.17 (cast and
 * castable, enumeration and choice target types); XSD 1.1 Part 2, section
 * 4.3.6 (whiteSpace).
 */
#include "cast_internal.h"

#include "sxml/string.h"
#include "xml_internal.h"

#include <math.h>
#include <stdalign.h>
#include <string.h>

/* --------------------------------------------------------------- Errors */

static const sxml_static_error sxml_cast_type_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "values of the source type cannot be cast to the target type "
        "(F&O 4.0, section 19.1)"),
};

static const sxml_static_error sxml_cast_cardinality_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "a cast needs one atomic item, or the empty sequence when the target "
        "type allows it (XPath 4.0, section 4.17.2)"),
};

static const sxml_static_error sxml_cast_value_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FORG0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the value is not in the value space of the target type"),
};

static const sxml_static_error sxml_cast_special_number_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOCA0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "NaN and the infinities cannot be cast to xs:decimal or xs:integer"),
};

static const sxml_static_error sxml_cast_target_error = {
    .error_status = SXML_STATUS_STATIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPST0080"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the target type of a cast must be a generalized atomic type other "
        "than xs:NOTATION, xs:anyAtomicType and xs:anySimpleType"),
};

/* Target types nest (a union of unions, a choice of enumerations) at most
   this deep; a bound on hostile types. */
enum { SXML_CAST_MAXIMUM_TARGET_DEPTH = 32 };

/* -------------------------------------------------------------- Sources */

/* What is cast. */
typedef struct sxml_cast_source {
    /* Text cast as an xs:string with that text; otherwise item. */
    bool is_text;
    sxml_byte_view text;
    sxml_item item;
} sxml_cast_source;

/* The primitive type of the source. */
static sxml_builtin_type sxml_source_primitive_type(sxml_cast_source source) {
    if (source.is_text) {
        return SXML_TYPE_XS_STRING;
    }
    return (sxml_builtin_type)sxml_atomic_item_get_primitive_type(source.item)
        .index;
}

/* xs:string (and its derived types) and xs:untypedAtomic: cast through
   the lexical form (F&O 4.0, section 19.2). */
static bool sxml_primitive_is_string_source(sxml_builtin_type primitive_type) {
    return primitive_type == SXML_TYPE_XS_STRING ||
           primitive_type == SXML_TYPE_XS_UNTYPED_ATOMIC;
}

/* The text of a string source, borrowed from it. */
static sxml_status sxml_source_text(sxml_cast_source source,
                                    sxml_byte_view *text) {
    if (source.is_text) {
        *text = source.text;
        return SXML_STATUS_OK;
    }
    sxml_atomic_datum datum;
    sxml_status status = sxml_atomic_item_get_datum(source.item, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *text = datum.datum_details.string.utf8;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- Heap scratch */

/* Text that may live in heap memory released after use. */
typedef struct sxml_scratch_text {
    sxml_byte_view text;
    sxml_heap_bytes bytes;
} sxml_scratch_text;

static void sxml_scratch_text_release(sxml_item_heap *item_heap,
                                      sxml_scratch_text *scratch) {
    sxml_item_heap_release_bytes(item_heap, scratch->bytes);
    scratch->bytes = (sxml_heap_bytes){.memory = NULL, .byte_count = 0};
}

/* ----------------------------------------------------------- Whitespace */

/* xs:string and xs:untypedAtomic preserve, xs:normalizedString replaces,
   every other atomic type collapses. */
static sxml_whitespace_facet
sxml_whitespace_facet_of(sxml_builtin_type builtin_type) {
    switch ((uint32_t)builtin_type) {
    case SXML_TYPE_XS_STRING:
    case SXML_TYPE_XS_UNTYPED_ATOMIC:
        return SXML_WHITESPACE_FACET_PRESERVE;
    case SXML_TYPE_XS_NORMALIZED_STRING:
        return SXML_WHITESPACE_FACET_REPLACE;
    default:
        return SXML_WHITESPACE_FACET_COLLAPSE;
    }
}

/* Text and the facet to apply to it. */
typedef struct sxml_whitespace_request {
    sxml_byte_view text;
    sxml_whitespace_facet facet;
} sxml_whitespace_request;

/* True when applying the facet would change text. */
static bool sxml_whitespace_needs_change(sxml_whitespace_request request) {
    sxml_byte_view text = request.text;
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        uint8_t byte = text.bytes[byte_index];
        if (byte == '\t' || byte == '\n' || byte == '\r') {
            return true;
        }
        if (request.facet == SXML_WHITESPACE_FACET_COLLAPSE && byte == ' ' &&
            (byte_index == 0 || byte_index + 1 == text.byte_count ||
             text.bytes[byte_index + 1] == ' ')) {
            return true;
        }
    }
    return false;
}

/*
 * Applies request.facet to request.text: the text itself when nothing
 * changes, otherwise a copy in heap memory (released with
 * sxml_scratch_text_release).
 */
static sxml_status sxml_apply_whitespace_facet(sxml_item_heap *item_heap,
                                               sxml_whitespace_request request,
                                               sxml_scratch_text *scratch) {
    *scratch = (sxml_scratch_text){.text = request.text,
                                   .bytes = {.memory = NULL, .byte_count = 0}};
    if (request.facet == SXML_WHITESPACE_FACET_PRESERVE ||
        !sxml_whitespace_needs_change(request)) {
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    sxml_status status = sxml_item_heap_allocate_bytes(
        item_heap,
        (sxml_allocation_request){.byte_count = request.text.byte_count,
                                  .alignment = 1},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *bytes = memory;
    size_t byte_count = 0;
    bool is_pending_space = false;
    for (size_t byte_index = 0; byte_index < request.text.byte_count;
         byte_index += 1) {
        uint8_t byte = request.text.bytes[byte_index];
        if (request.facet == SXML_WHITESPACE_FACET_REPLACE) {
            bytes[byte_count] = sxml_xml_byte_is_whitespace(byte) ? ' ' : byte;
            byte_count += 1;
            continue;
        }
        if (sxml_xml_byte_is_whitespace(byte)) {
            is_pending_space = byte_count != 0;
            continue;
        }
        if (is_pending_space) {
            bytes[byte_count] = ' ';
            byte_count += 1;
            is_pending_space = false;
        }
        bytes[byte_count] = byte;
        byte_count += 1;
    }
    *scratch = (sxml_scratch_text){
        .text = {.bytes = bytes, .byte_count = byte_count},
        .bytes = {.memory = memory, .byte_count = request.text.byte_count}};
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------- Targets */

/* More base type steps than any schema derives: a bound on a broken
   registry. */
enum { SXML_CAST_MAXIMUM_DERIVATION_STEP_COUNT = 1024 };

/* Types a schema registered may have facets; built-in types have theirs
   in their identifiers (include/sxml/types.h, D165). */
static bool sxml_type_is_registered(sxml_type_identifier type) {
    return type.index >= SXML_TYPE_FIRST_REGISTERED;
}

sxml_status sxml_cast_resolve_atomic_target(const sxml_type_registry *registry,
                                            sxml_type_identifier type,
                                            sxml_atomic_target *target) {
    sxml_type_description description;
    sxml_status status =
        sxml_type_registry_describe_type(registry, type, &description);
    if (status != SXML_STATUS_OK ||
        description.variety != SXML_TYPE_VARIETY_ATOMIC) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_type_identifier builtin = type;
    for (size_t step_index = 0; builtin.index >= SXML_TYPE_BUILTIN_COUNT;
         step_index += 1) {
        sxml_type_description base_description;
        if (step_index == SXML_CAST_MAXIMUM_DERIVATION_STEP_COUNT ||
            sxml_type_registry_describe_type(
                registry, builtin, &base_description) != SXML_STATUS_OK) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        builtin = base_description.base_type;
    }
    *target = (sxml_atomic_target){
        .type = type,
        .builtin_type = (sxml_builtin_type)builtin.index,
        .primitive_type = (sxml_builtin_type)description.primitive_type.index,
    };
    return SXML_STATUS_OK;
}

/* xs:anyAtomicType has no primitive type; xs:NOTATION is abstract (XPath
   4.0, section 4.17.1, XPST0080). */
static bool sxml_atomic_target_is_allowed(sxml_atomic_target target) {
    return target.primitive_type != SXML_TYPE_NONE &&
           target.type.index != SXML_TYPE_XS_NOTATION;
}

static bool sxml_target_is_integer_type(sxml_atomic_target target) {
    return target.builtin_type >= SXML_TYPE_XS_INTEGER &&
           target.builtin_type <= SXML_TYPE_XS_POSITIVE_INTEGER;
}

/* --------------------------------------------- The casting table (19.1) */

static bool sxml_primitive_is_numeric(sxml_builtin_type primitive_type) {
    return primitive_type == SXML_TYPE_XS_DECIMAL ||
           primitive_type == SXML_TYPE_XS_FLOAT ||
           primitive_type == SXML_TYPE_XS_DOUBLE;
}

static bool sxml_primitive_is_date_time(sxml_builtin_type primitive_type) {
    return primitive_type >= SXML_TYPE_XS_DATE_TIME &&
           primitive_type <= SXML_TYPE_XS_G_MONTH;
}

static bool sxml_primitive_is_binary(sxml_builtin_type primitive_type) {
    return primitive_type == SXML_TYPE_XS_HEX_BINARY ||
           primitive_type == SXML_TYPE_XS_BASE64_BINARY;
}

static bool sxml_primitive_is_qname_like(sxml_builtin_type primitive_type) {
    return primitive_type == SXML_TYPE_XS_QNAME ||
           primitive_type == SXML_TYPE_XS_NOTATION;
}

/* Primitive source and target types. */
typedef struct sxml_primitive_pair {
    sxml_builtin_type source;
    sxml_builtin_type target;
} sxml_primitive_pair;

/*
 * The casting table of F&O 4.0, section 19.1, for sources that are not
 * strings: every type casts to xs:string and xs:untypedAtomic; numbers and
 * booleans to each other; durations to durations; xs:dateTime to every
 * date/time type, xs:date to all but xs:time, the others to themselves; the
 * binary types to each other; xs:anyURI to itself; xs:QName and
 * xs:NOTATION to each other.
 */
static bool sxml_cast_is_allowed(sxml_primitive_pair pair) {
    if (sxml_primitive_is_string_source(pair.target)) {
        return true;
    }
    if (pair.source == SXML_TYPE_XS_BOOLEAN ||
        sxml_primitive_is_numeric(pair.source)) {
        return pair.target == SXML_TYPE_XS_BOOLEAN ||
               sxml_primitive_is_numeric(pair.target);
    }
    if (sxml_primitive_is_date_time(pair.source)) {
        if (!sxml_primitive_is_date_time(pair.target)) {
            return false;
        }
        if (pair.source == SXML_TYPE_XS_DATE_TIME ||
            pair.source == pair.target) {
            return true;
        }
        return pair.source == SXML_TYPE_XS_DATE &&
               pair.target != SXML_TYPE_XS_TIME;
    }
    if (sxml_primitive_is_binary(pair.source)) {
        return sxml_primitive_is_binary(pair.target);
    }
    if (sxml_primitive_is_qname_like(pair.source)) {
        return sxml_primitive_is_qname_like(pair.target);
    }
    /* xs:duration and xs:anyURI cast to their own primitive type. */
    return pair.source == pair.target;
}

/* ------------------------------------------------------ The conversions */

/* An atomic item to cast to an atomic target. */
typedef struct sxml_atomic_cast {
    /* The heap the result is made in. */
    sxml_item_heap *item_heap;
    sxml_cast_source source;
    sxml_atomic_target target;
    sxml_namespace_binding_view namespace_bindings;
    sxml_error_record *error_record;
} sxml_atomic_cast;

static sxml_status sxml_cast_fail(const sxml_atomic_cast *cast,
                                  const sxml_static_error *error) {
    return sxml_error_record_report(cast->error_record, error);
}

/* Makes the target item from a datum; outside the value space is
   FORG0001 (F&O 4.0, section 19.3.1, facets of the target). */
static sxml_status sxml_cast_make_datum(const sxml_atomic_cast *cast,
                                        sxml_atomic_datum datum,
                                        sxml_item *result) {
    sxml_item_heap *item_heap = cast->item_heap;
    sxml_status status = sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){.type = cast->target.type, .datum = datum},
        result);
    if (status == SXML_STATUS_INVALID_ARGUMENT) {
        return sxml_cast_fail(cast, &sxml_cast_value_error);
    }
    return status;
}

/* Makes the target item from a computed decimal, which it takes; finalizes
   it on failure. */
static sxml_status sxml_cast_make_decimal(const sxml_atomic_cast *cast,
                                          sxml_decimal *decimal,
                                          sxml_item *result) {
    sxml_item_heap *item_heap = cast->item_heap;
    sxml_status status = sxml_atomic_item_make_taking_decimal(
        item_heap,
        &(sxml_decimal_item_request){.type = cast->target.type,
                                     .decimal = decimal},
        result);
    if (status != SXML_STATUS_OK) {
        sxml_decimal_finalize(decimal);
        if (status == SXML_STATUS_INVALID_ARGUMENT) {
            return sxml_cast_fail(cast, &sxml_cast_value_error);
        }
    }
    return status;
}

/* A number or boolean source, read once. */
typedef enum sxml_number_kind {
    SXML_NUMBER_KIND_INTEGER,
    SXML_NUMBER_KIND_DECIMAL,
    SXML_NUMBER_KIND_FLOAT,
    SXML_NUMBER_KIND_DOUBLE,
} sxml_number_kind;

typedef struct sxml_number {
    sxml_number_kind number_kind;
    int64_t integer;
    /* Borrowed from the item. */
    const sxml_decimal *decimal;
    double double_number;
} sxml_number;

/* Booleans count as the integers 1 and 0 (F&O 4.0, section 19.1.3). */
static sxml_number sxml_number_of(sxml_item item) {
    sxml_number number = {.number_kind = SXML_NUMBER_KIND_INTEGER,
                          .integer = 0,
                          .decimal = NULL,
                          .double_number = 0.0};
    switch (sxml_item_get_representation(item)) {
    case SXML_ITEM_REPRESENTATION_BOOLEAN:
    case SXML_ITEM_REPRESENTATION_INTEGER:
        number.integer = item.payload.integer;
        return number;
    case SXML_ITEM_REPRESENTATION_FLOAT:
        number.number_kind = SXML_NUMBER_KIND_FLOAT;
        number.double_number = (double)item.payload.float_number;
        return number;
    case SXML_ITEM_REPRESENTATION_DOUBLE:
        number.number_kind = SXML_NUMBER_KIND_DOUBLE;
        number.double_number = item.payload.double_number;
        return number;
    case SXML_ITEM_REPRESENTATION_ATOMIC_CELL:
        number.number_kind = SXML_NUMBER_KIND_DECIMAL;
        number.decimal = sxml_atomic_item_get_decimal(item);
        return number;
    case SXML_ITEM_REPRESENTATION_NONE:
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
        return number;
    }
    return number;
}

/* xs:boolean: false for 0, -0 and NaN (F&O 4.0, section 19.1.6). */
static bool sxml_number_is_true(sxml_number number) {
    switch (number.number_kind) {
    case SXML_NUMBER_KIND_INTEGER:
        return number.integer != 0;
    case SXML_NUMBER_KIND_DECIMAL:
        return number.decimal != NULL && !sxml_decimal_is_zero(number.decimal);
    case SXML_NUMBER_KIND_FLOAT:
    case SXML_NUMBER_KIND_DOUBLE:
        return number.double_number != 0.0 && !isnan(number.double_number);
    }
    return false;
}

/* The nearest double (F&O 4.0, section 19.1.3 "Casting to numeric
   types"). */
static double sxml_number_to_double(sxml_number number) {
    switch (number.number_kind) {
    case SXML_NUMBER_KIND_INTEGER:
        return (double)number.integer;
    case SXML_NUMBER_KIND_DECIMAL:
        return number.decimal == NULL ? 0.0
                                      : sxml_decimal_to_double(number.decimal);
    case SXML_NUMBER_KIND_FLOAT:
    case SXML_NUMBER_KIND_DOUBLE:
        return number.double_number;
    }
    return 0.0;
}

/* The nearest float, rounded once. */
static float sxml_number_to_float(sxml_number number) {
    switch (number.number_kind) {
    case SXML_NUMBER_KIND_INTEGER:
        return (float)number.integer;
    case SXML_NUMBER_KIND_DECIMAL:
        return number.decimal == NULL ? 0.0F
                                      : sxml_decimal_to_float(number.decimal);
    case SXML_NUMBER_KIND_FLOAT:
    case SXML_NUMBER_KIND_DOUBLE:
        return (float)number.double_number;
    }
    return 0.0F;
}

/* 2^63 as a double: the doubles below it in magnitude truncate into
   int64_t. */
static const double sxml_two_to_the_63 = 9223372036854775808.0;

/*
 * To xs:integer and its derived types: the value with its fraction
 * discarded (F&O 4.0, section 19.1.3, casting to xs:integer); NaN and
 * the infinities are FOCA0002, a value outside the target type FORG0001.
 */
static sxml_status sxml_cast_number_to_integer(const sxml_atomic_cast *cast,
                                               sxml_number number,
                                               sxml_item *result) {
    sxml_item_heap *item_heap = cast->item_heap;
    const sxml_decimal_configuration *configuration =
        sxml_item_heap_get_decimal_configuration(item_heap);
    switch (number.number_kind) {
    case SXML_NUMBER_KIND_INTEGER:
        return sxml_cast_make_datum(
            cast,
            (sxml_atomic_datum){.datum_kind = SXML_DATUM_KIND_INTEGER,
                                .datum_details = {.integer = number.integer}},
            result);
    case SXML_NUMBER_KIND_DECIMAL: {
        sxml_decimal truncated;
        sxml_status status = sxml_decimal_round(
            configuration,
            &(sxml_decimal_rounding){.decimal = number.decimal,
                                     .precision = 0,
                                     .rounding_mode =
                                         SXML_ROUNDING_MODE_TOWARD_ZERO},
            &truncated);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_cast_make_decimal(cast, &truncated, result);
    }
    case SXML_NUMBER_KIND_FLOAT:
    case SXML_NUMBER_KIND_DOUBLE: {
        double value = number.double_number;
        if (isnan(value) || isinf(value)) {
            return sxml_cast_fail(cast, &sxml_cast_special_number_error);
        }
        double truncated = trunc(value);
        if (truncated >= -sxml_two_to_the_63 &&
            truncated < sxml_two_to_the_63) {
            return sxml_cast_make_datum(
                cast,
                (sxml_atomic_datum){
                    .datum_kind = SXML_DATUM_KIND_INTEGER,
                    .datum_details = {.integer = (int64_t)truncated}},
                result);
        }
        /* Beyond 2^63 a double is an integer: its exact value. */
        sxml_decimal exact;
        sxml_status status =
            sxml_decimal_from_double_exactly(configuration, truncated, &exact);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_cast_make_decimal(cast, &exact, result);
    }
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/*
 * To xs:decimal (F&O 4.0, section 19.1.3, casting to xs:decimal): a
 * float or double becomes the shortest decimal that reads back as it
 * (D57); NaN and the infinities are FOCA0002.
 */
static sxml_status sxml_cast_number_to_decimal(const sxml_atomic_cast *cast,
                                               sxml_number number,
                                               sxml_item *result) {
    sxml_item_heap *item_heap = cast->item_heap;
    const sxml_decimal_configuration *configuration =
        sxml_item_heap_get_decimal_configuration(item_heap);
    sxml_decimal decimal;
    sxml_status status = SXML_STATUS_OK;
    switch (number.number_kind) {
    case SXML_NUMBER_KIND_INTEGER:
        status =
            sxml_decimal_from_integer(configuration, number.integer, &decimal);
        break;
    case SXML_NUMBER_KIND_DECIMAL:
        status = sxml_decimal_copy(configuration, number.decimal, &decimal);
        break;
    case SXML_NUMBER_KIND_FLOAT:
    case SXML_NUMBER_KIND_DOUBLE:
        if (isnan(number.double_number) || isinf(number.double_number)) {
            return sxml_cast_fail(cast, &sxml_cast_special_number_error);
        }
        status = number.number_kind == SXML_NUMBER_KIND_FLOAT
                     ? sxml_decimal_from_float_shortest(
                           configuration, (float)number.double_number, &decimal)
                     : sxml_decimal_from_double_shortest(
                           configuration, number.double_number, &decimal);
        break;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_cast_make_decimal(cast, &decimal, result);
}

/* Numbers and booleans to numbers and booleans (F&O 4.0, sections 19.1.3
   and 19.1.6). */
static sxml_status sxml_cast_number(const sxml_atomic_cast *cast,
                                    sxml_item *result) {
    sxml_number number = sxml_number_of(cast->source.item);
    switch ((uint32_t)cast->target.primitive_type) {
    case SXML_TYPE_XS_BOOLEAN:
        return sxml_cast_make_datum(
            cast,
            (sxml_atomic_datum){
                .datum_kind = SXML_DATUM_KIND_BOOLEAN,
                .datum_details = {.boolean = sxml_number_is_true(number)}},
            result);
    case SXML_TYPE_XS_DOUBLE:
        return sxml_cast_make_datum(
            cast,
            (sxml_atomic_datum){
                .datum_kind = SXML_DATUM_KIND_DOUBLE,
                .datum_details = {.double_number =
                                      sxml_number_to_double(number)}},
            result);
    case SXML_TYPE_XS_FLOAT:
        return sxml_cast_make_datum(
            cast,
            (sxml_atomic_datum){
                .datum_kind = SXML_DATUM_KIND_FLOAT,
                .datum_details = {.float_number =
                                      sxml_number_to_float(number)}},
            result);
    default:
        if (sxml_target_is_integer_type(cast->target)) {
            return sxml_cast_number_to_integer(cast, number, result);
        }
        return sxml_cast_number_to_decimal(cast, number, result);
    }
}

/*
 * Between date/time types (F&O 4.0, section 19.1.5 "Casting to date and
 * time types"): the components the target has are kept, with the
 * timezone; those it lacks are dropped; xs:date to xs:dateTime adds
 * 00:00:00.
 */
static sxml_date_time
sxml_date_time_for_target(const sxml_date_time *source,
                          sxml_builtin_type target_primitive) {
    sxml_date_time result = {.year = 0,
                             .month = 0,
                             .day = 0,
                             .hour = 0,
                             .minute = 0,
                             .second = 0,
                             .nanosecond = 0,
                             .has_timezone = source->has_timezone,
                             .timezone_offset_minutes =
                                 source->timezone_offset_minutes};
    bool has_year = false;
    bool has_month = false;
    bool has_day = false;
    bool has_time = false;
    switch ((uint32_t)target_primitive) {
    case SXML_TYPE_XS_DATE_TIME:
        has_year = has_month = has_day = has_time = true;
        break;
    case SXML_TYPE_XS_DATE:
        has_year = has_month = has_day = true;
        break;
    case SXML_TYPE_XS_TIME:
        has_time = true;
        break;
    case SXML_TYPE_XS_G_YEAR_MONTH:
        has_year = has_month = true;
        break;
    case SXML_TYPE_XS_G_YEAR:
        has_year = true;
        break;
    case SXML_TYPE_XS_G_MONTH_DAY:
        has_month = has_day = true;
        break;
    case SXML_TYPE_XS_G_DAY:
        has_day = true;
        break;
    case SXML_TYPE_XS_G_MONTH:
        has_month = true;
        break;
    default:
        break;
    }
    if (has_year) {
        result.year = source->year;
    }
    if (has_month) {
        result.month = source->month;
    }
    if (has_day) {
        result.day = source->day;
    }
    if (has_time) {
        result.hour = source->hour;
        result.minute = source->minute;
        result.second = source->second;
        result.nanosecond = source->nanosecond;
    }
    return result;
}

/* Casts a source that is not a string to a target of another family than
   the string types. */
static sxml_status sxml_cast_datum(const sxml_atomic_cast *cast,
                                   sxml_item *result) {
    sxml_builtin_type target = cast->target.primitive_type;
    if (target == SXML_TYPE_XS_BOOLEAN || sxml_primitive_is_numeric(target)) {
        return sxml_cast_number(cast, result);
    }
    sxml_atomic_datum datum;
    sxml_status status = sxml_atomic_item_get_datum(cast->source.item, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    switch (datum.datum_kind) {
    case SXML_DATUM_KIND_DATE_TIME:
        datum.datum_details.date_time =
            sxml_date_time_for_target(&datum.datum_details.date_time, target);
        break;
    case SXML_DATUM_KIND_DURATION:
        /* F&O 4.0, section 19.1.4 "Casting to duration types": the part
           the target keeps. */
        if (cast->target.builtin_type == SXML_TYPE_XS_DAY_TIME_DURATION) {
            datum.datum_details.duration.months = 0;
        } else if (cast->target.builtin_type ==
                   SXML_TYPE_XS_YEAR_MONTH_DURATION) {
            datum.datum_details.duration.seconds = 0;
            datum.datum_details.duration.nanoseconds = 0;
        }
        break;
    case SXML_DATUM_KIND_BOOLEAN:
    case SXML_DATUM_KIND_INTEGER:
    case SXML_DATUM_KIND_DECIMAL:
    case SXML_DATUM_KIND_FLOAT:
    case SXML_DATUM_KIND_DOUBLE:
    case SXML_DATUM_KIND_STRING:
    case SXML_DATUM_KIND_QNAME:
    case SXML_DATUM_KIND_BINARY:
        /* xs:anyURI, the QName types and the binary types keep their datum
           (F&O 4.0, sections 19.1.7 to 19.1.9). */
        break;
    }
    return sxml_cast_make_datum(cast, datum, result);
}

/* The canonical text of an item, in stack memory or, when longer, heap
   memory (released with sxml_scratch_text_release). */
typedef struct sxml_canonical_scratch {
    uint8_t bytes[SXML_CAST_CANONICAL_TEXT_CAPACITY];
    sxml_scratch_text scratch;
} sxml_canonical_scratch;

/* An item whose canonical text is wanted, and which mapping
   (sxml_canonical_text_request). */
typedef struct sxml_canonical_scratch_request {
    sxml_item item;
    bool uses_schema_mapping;
} sxml_canonical_scratch_request;

static sxml_status
sxml_write_canonical_scratch(sxml_item_heap *item_heap, sxml_canonical_scratch_request request,
                             sxml_canonical_scratch *canonical) {
    const sxml_item item = request.item;
    canonical->scratch =
        (sxml_scratch_text){.text = {.bytes = NULL, .byte_count = 0},
                            .bytes = {.memory = NULL, .byte_count = 0}};
    sxml_canonical_text text;
    sxml_status status = sxml_cast_write_canonical_text(
        item_heap,
        &(sxml_canonical_text_request){
            .item = item,
            .output = {.bytes = canonical->bytes,
                       .byte_count = sizeof(canonical->bytes)},
            .uses_schema_mapping = request.uses_schema_mapping},
        &text);
    if (status == SXML_STATUS_CAPACITY_EXCEEDED) {
        void *memory = NULL;
        status = sxml_item_heap_allocate_bytes(
            item_heap,
            (sxml_allocation_request){.byte_count = text.required_byte_count,
                                      .alignment = 1},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        canonical->scratch.bytes = (sxml_heap_bytes){
            .memory = memory, .byte_count = text.required_byte_count};
        status = sxml_cast_write_canonical_text(
            item_heap,
            &(sxml_canonical_text_request){
                .item = item,
                .output = {.bytes = memory,
                           .byte_count = text.required_byte_count},
                .uses_schema_mapping = request.uses_schema_mapping},
            &text);
        if (status != SXML_STATUS_OK) {
            sxml_scratch_text_release(item_heap, &canonical->scratch);
            return status;
        }
    }
    if (status == SXML_STATUS_OK) {
        canonical->scratch.text = text.text;
    }
    return status;
}

/* Parses text, after the target's whitespace facet, as the target's
   lexical form. */
static sxml_status sxml_cast_text(const sxml_atomic_cast *cast,
                                  sxml_byte_view text, sxml_item *result) {
    sxml_item_heap *item_heap = cast->item_heap;
    sxml_whitespace_facet facet =
        sxml_whitespace_facet_of(cast->target.builtin_type);
    if (sxml_type_is_registered(cast->target.type)) {
        facet = sxml_cast_find_whitespace_facet(
            sxml_item_heap_get_type_registry(item_heap), cast->target.type,
            facet);
    }
    sxml_scratch_text processed;
    sxml_status status = sxml_apply_whitespace_facet(
        item_heap, (sxml_whitespace_request){.text = text, .facet = facet},
        &processed);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item item = {.representation = 0};
    status = sxml_cast_make_item_from_lexical_form(
        item_heap,
        &(sxml_lexical_form_request){.lexical_form = processed.text,
                                     .target = cast->target,
                                     .namespace_bindings =
                                         cast->namespace_bindings,
                                     .error_record = cast->error_record},
        &item);
    if (status == SXML_STATUS_OK && sxml_type_is_registered(cast->target.type)) {
        /* The facets of a schema type (F&O 4.0, section 19.3.1). */
        status = sxml_cast_check_facets(
            item_heap, &(sxml_facet_check_request){
                           .type = cast->target.type,
                           .lexical_form = processed.text,
                           .items = {.items = &item, .item_count = 1},
                           .error_record = cast->error_record});
        if (status != SXML_STATUS_OK) {
            sxml_item_release(item);
        }
    }
    sxml_scratch_text_release(item_heap, &processed);
    if (status == SXML_STATUS_OK) {
        *result = item;
    }
    return status;
}

/* Checks the facets of a schema type against an item cast from another
   type than the string types, matching patterns against its canonical
   text, XSD's canonical mapping (F&O 4.0, 23.3.5: "the canonical
   representation of the value, as defined for the source type");
   releases the item on failure. */
static sxml_status sxml_cast_check_item_facets(sxml_item_heap *item_heap,
                                               const sxml_facet_check_request *request) {
    sxml_canonical_scratch canonical;
    sxml_status status = sxml_write_canonical_scratch(
        item_heap,
        (sxml_canonical_scratch_request){.item = request->items.items[0],
                                         .uses_schema_mapping = true},
        &canonical);
    if (status == SXML_STATUS_OK) {
        sxml_facet_check_request check = *request;
        check.lexical_form = canonical.scratch.text;
        status = sxml_cast_check_facets(item_heap, &check);
        sxml_scratch_text_release(item_heap, &canonical.scratch);
    }
    if (status != SXML_STATUS_OK) {
        sxml_item_release(request->items.items[0]);
    }
    return status;
}

/*
 * Casts cast->source to an atomic target: unchanged when it already has
 * the target's annotation; through the lexical form from the string
 * types; through the canonical text to the string types; through the
 * datum otherwise.
 */
static sxml_status sxml_cast_to_atomic_target(const sxml_atomic_cast *cast,
                                              sxml_item *result) {
    sxml_item_heap *item_heap = cast->item_heap;
    sxml_cast_source source = cast->source;
    if (!source.is_text &&
        sxml_type_identifier_equals(sxml_item_get_type_annotation(source.item),
                                    cast->target.type)) {
        sxml_item_retain(source.item);
        *result = source.item;
        return SXML_STATUS_OK;
    }
    sxml_builtin_type source_primitive = sxml_source_primitive_type(source);
    sxml_builtin_type target_primitive = cast->target.primitive_type;
    if (sxml_primitive_is_string_source(source_primitive)) {
        sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
        sxml_status status = sxml_source_text(source, &text);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_cast_text(cast, text, result);
    }
    if (!sxml_cast_is_allowed((sxml_primitive_pair){
            .source = source_primitive, .target = target_primitive})) {
        return sxml_cast_fail(cast, &sxml_cast_type_error);
    }
    if (sxml_primitive_is_string_source(target_primitive)) {
        /* F&O 4.0, section 19.1.2, then the facets of a derived string
           type (19.3.1). */
        sxml_canonical_scratch canonical;
        sxml_status status = sxml_write_canonical_scratch(
            item_heap,
            (sxml_canonical_scratch_request){.item = source.item,
                                             .uses_schema_mapping = false},
            &canonical);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        status = sxml_cast_text(cast, canonical.scratch.text, result);
        sxml_scratch_text_release(item_heap, &canonical.scratch);
        return status;
    }
    sxml_item item = {.representation = 0};
    sxml_status status = sxml_cast_datum(cast, &item);
    if (status == SXML_STATUS_OK && sxml_type_is_registered(cast->target.type)) {
        status = sxml_cast_check_item_facets(
            item_heap, &(sxml_facet_check_request){
                           .type = cast->target.type,
                           .lexical_form = {.bytes = NULL, .byte_count = 0},
                           .items = {.items = &item, .item_count = 1},
                           .error_record = cast->error_record});
    }
    if (status == SXML_STATUS_OK) {
        *result = item;
    }
    return status;
}

/* ---------------------------------------------------------- Any target */

/*
 * A cast to any target type. Union, choice and list targets cast their
 * source again to members or tokens, which may be unions, choices or lists
 * themselves; those casts are made on an explicit stack of cast frames in
 * heap memory (D71), at most SXML_CAST_MAXIMUM_TARGET_DEPTH deep.
 */

/* A source to cast to a target type. */
typedef struct sxml_target_cast {
    sxml_item_heap *item_heap;
    sxml_cast_source source;
    sxml_type_identifier target_type;
    sxml_namespace_binding_view namespace_bindings;
    sxml_error_record *error_record;
} sxml_target_cast;

static sxml_status sxml_target_fail(const sxml_target_cast *cast,
                                    const sxml_static_error *error) {
    return sxml_error_record_report(cast->error_record, error);
}

/* True for the statuses that report an error of the cast itself, which a
   union or castable turns into an answer. */
static bool sxml_status_is_cast_error(sxml_status status) {
    return status == SXML_STATUS_DYNAMIC_ERROR ||
           status == SXML_STATUS_TYPE_ERROR;
}

/* The source as an item of its own annotation, for subtype tests. */
static bool sxml_source_is_derived_from(sxml_item_heap *item_heap,
                                        sxml_cast_source source,
                                        sxml_type_identifier type) {
    sxml_type_identifier annotation =
        source.is_text ? sxml_builtin_type_identifier(SXML_TYPE_XS_STRING)
                       : sxml_item_get_type_annotation(source.item);
    return sxml_type_registry_is_derived_from(
        sxml_item_heap_get_type_registry(item_heap),
        (sxml_type_pair){.derived_type = annotation, .base_type = type});
}

/*
 * An enumeration type (XPath 4.0, section 3.2 "Enumeration Types"): the
 * source cast to xs:string must be one of the values; the result is that
 * xs:string, which is an instance of the enumeration.
 */
static sxml_status sxml_cast_to_enumeration(const sxml_target_cast *cast,
                                            sxml_byte_view_list values,
                                            sxml_sequence *result) {
    sxml_item_heap *item_heap = cast->item_heap;
    sxml_item string_item = {.representation = 0};
    sxml_status status = sxml_cast_to_atomic_target(
        &(sxml_atomic_cast){.item_heap = item_heap,
                            .source = cast->source,
                            .target = {.type = sxml_builtin_type_identifier(
                                           SXML_TYPE_XS_STRING),
                                       .builtin_type = SXML_TYPE_XS_STRING,
                                       .primitive_type = SXML_TYPE_XS_STRING},
                            .namespace_bindings = cast->namespace_bindings,
                            .error_record = cast->error_record},
        &string_item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(string_item, &datum);
    bool is_member = false;
    for (size_t value_index = 0; status == SXML_STATUS_OK && !is_member &&
                                 value_index < values.byte_view_count;
         value_index += 1) {
        is_member = sxml_byte_view_equals(datum.datum_details.string.utf8,
                                          values.byte_views[value_index]);
    }
    if (status == SXML_STATUS_OK && !is_member) {
        status = sxml_target_fail(cast, &sxml_cast_value_error);
    }
    if (status != SXML_STATUS_OK) {
        sxml_item_release(string_item);
        return status;
    }
    *result = sxml_sequence_from_item(string_item);
    return SXML_STATUS_OK;
}

/* Counts the space-separated tokens of collapsed text. */
static size_t sxml_count_tokens(sxml_byte_view text) {
    size_t token_count = text.byte_count == 0 ? 0 : 1;
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        if (text.bytes[byte_index] == ' ') {
            token_count += 1;
        }
    }
    return token_count;
}

/* The items of a list cast so far, in heap memory. */
typedef struct sxml_list_items {
    sxml_item *items;
    size_t item_count;
    size_t capacity;
} sxml_list_items;

static void sxml_list_items_release(sxml_item_heap *item_heap,
                                    sxml_list_items *list) {
    for (size_t item_index = 0; item_index < list->item_count;
         item_index += 1) {
        sxml_item_release(list->items[item_index]);
    }
    sxml_item_heap_release_bytes(
        item_heap,
        (sxml_heap_bytes){.memory = list->items,
                          .byte_count = list->capacity * sizeof(sxml_item)});
}

/* -------------------------------------------------- Any target: frames */

/* What a cast frame waits for. */
typedef enum sxml_cast_frame_kind {
    /* A union or choice target: the cast to a member. */
    SXML_CAST_FRAME_MEMBERS,
    /* A list target: the cast of a token to the item type. */
    SXML_CAST_FRAME_LIST
} sxml_cast_frame_kind;

/* error_frame_index of a cast that reports to the caller's error record. */
#define SXML_CAST_CALLER_ERROR_RECORD SIZE_MAX

/* A cast to a union, choice or list target, waiting for the cast of a
   member or token. */
typedef struct sxml_cast_frame {
    sxml_cast_frame_kind kind;
    /* The union, choice or list target type. */
    sxml_type_identifier target_type;
    sxml_cast_source source;
    /* The frame whose member_error this cast reports to, or
       SXML_CAST_CALLER_ERROR_RECORD. */
    size_t error_frame_index;
    /* SXML_CAST_FRAME_MEMBERS: the members, the next one to try, whether
       every member so far refused the source type, and the error record
       of the member casts, whose errors are dropped. */
    sxml_type_identifier_view members;
    size_t next_member_index;
    bool is_every_refusal_a_type_error;
    sxml_error_record member_error;
    /* SXML_CAST_FRAME_LIST: the item type, the collapsed text, the items
       cast so far and where the next token starts (past the end when
       there is none). */
    sxml_type_identifier item_type;
    sxml_scratch_text collapsed;
    sxml_list_items list;
    size_t next_token_offset;
} sxml_cast_frame;

/* The explicit stack of one cast to a target. */
typedef struct sxml_cast_machine {
    /* The cast asked for: heap, namespace bindings, caller's error record. */
    const sxml_target_cast *cast;
    /* frame_capacity frames in heap memory; NULL while it is 0. */
    sxml_cast_frame *frames;
    size_t frame_count;
    size_t frame_capacity;
} sxml_cast_machine;

/* A cast that the top frame, or the caller, asks for. Its target is
   nested as deep as the machine has frames. */
typedef struct sxml_cast_step {
    sxml_cast_source source;
    sxml_type_identifier target_type;
    size_t error_frame_index;
} sxml_cast_step;

/* What a finished cast gave; value only with SXML_STATUS_OK. */
typedef struct sxml_cast_outcome {
    sxml_status status;
    sxml_sequence value;
} sxml_cast_outcome;

static sxml_error_record *sxml_cast_machine_error_record(
    const sxml_cast_machine *machine, size_t error_frame_index) {
    if (error_frame_index == SXML_CAST_CALLER_ERROR_RECORD) {
        return machine->cast->error_record;
    }
    return &machine->frames[error_frame_index].member_error;
}

/* Reports error to the error record of a frame's cast; the status. */
static sxml_status sxml_cast_frame_fail(const sxml_cast_machine *machine,
                                        const sxml_static_error *error) {
    const sxml_cast_frame *top = &machine->frames[machine->frame_count - 1];
    return sxml_error_record_report(
        sxml_cast_machine_error_record(machine, top->error_frame_index),
        error);
}

/* Pushes a copy of frame, growing the stack up to the most frames a cast
   can nest. */
static sxml_status sxml_cast_machine_push(sxml_cast_machine *machine,
                                          const sxml_cast_frame *frame) {
    if (machine->frame_count == machine->frame_capacity) {
        size_t capacity =
            machine->frame_capacity == 0 ? 4U : machine->frame_capacity * 2U;
        if (capacity > (size_t)SXML_CAST_MAXIMUM_TARGET_DEPTH + 1U) {
            capacity = (size_t)SXML_CAST_MAXIMUM_TARGET_DEPTH + 1U;
        }
        void *memory = NULL;
        sxml_status status = sxml_item_heap_allocate_bytes(
            machine->cast->item_heap,
            (sxml_allocation_request){.byte_count =
                                          capacity * sizeof(sxml_cast_frame),
                                      .alignment = alignof(sxml_cast_frame)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (machine->frame_count != 0) {
            memcpy(memory, machine->frames,
                   machine->frame_count * sizeof(sxml_cast_frame));
        }
        sxml_item_heap_release_bytes(
            machine->cast->item_heap,
            (sxml_heap_bytes){.memory = machine->frames,
                              .byte_count = machine->frame_capacity *
                                            sizeof(sxml_cast_frame)});
        machine->frames = memory;
        machine->frame_capacity = capacity;
    }
    machine->frames[machine->frame_count] = *frame;
    machine->frame_count += 1;
    return SXML_STATUS_OK;
}

/* Releases what the top frame holds and pops it. */
static void sxml_cast_machine_pop(sxml_cast_machine *machine) {
    sxml_cast_frame *top = &machine->frames[machine->frame_count - 1];
    if (top->kind == SXML_CAST_FRAME_LIST) {
        sxml_list_items_release(machine->cast->item_heap, &top->list);
        sxml_scratch_text_release(machine->cast->item_heap, &top->collapsed);
    }
    machine->frame_count -= 1;
}

/*
 * The facets of a union type a schema derived by restriction (pattern
 * and enumeration, XSD 1.1 Part 2, 4.1.5) on the value a member gave:
 * patterns match the collapsed text cast from, or the canonical text of
 * the value. Releases the value on failure.
 */
static sxml_status sxml_cast_check_union_facets(const sxml_cast_machine *machine,
                                                const sxml_cast_frame *frame,
                                                sxml_item value) {
    sxml_item_heap *item_heap = machine->cast->item_heap;
    sxml_facet_check_request request = {
        .type = frame->target_type,
        .lexical_form = {.bytes = NULL, .byte_count = 0},
        .items = {.items = &value, .item_count = 1},
        .error_record =
            sxml_cast_machine_error_record(machine, frame->error_frame_index)};
    if (!sxml_primitive_is_string_source(sxml_source_primitive_type(frame->source))) {
        return sxml_cast_check_item_facets(item_heap, &request);
    }
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    sxml_scratch_text collapsed = {.text = {.bytes = NULL, .byte_count = 0},
                                   .bytes = {.memory = NULL, .byte_count = 0}};
    sxml_status status = sxml_source_text(frame->source, &text);
    if (status == SXML_STATUS_OK) {
        status = sxml_apply_whitespace_facet(
            item_heap,
            (sxml_whitespace_request){.text = text,
                                      .facet = SXML_WHITESPACE_FACET_COLLAPSE},
            &collapsed);
    }
    if (status == SXML_STATUS_OK) {
        request.lexical_form = collapsed.text;
        status = sxml_cast_check_facets(item_heap, &request);
        sxml_scratch_text_release(item_heap, &collapsed);
    }
    if (status != SXML_STATUS_OK) {
        sxml_item_release(value);
    }
    return status;
}

/*
 * A union or a choice (F&O 4.0, section 19.3.5 "Casting to union types";
 * XPath 4.0, section 4.17.1): a source that is already an instance of an
 * atomic member is returned unchanged; otherwise the first member the cast
 * to which succeeds gives the result. When none does: XPTY0004 when every
 * member refused the source type, otherwise FORG0001 (xs:error, the union
 * without members, always FORG0001). Returns true when it pushed frame to
 * try the members, else writes *outcome.
 */
static bool sxml_cast_begin_members(sxml_cast_machine *machine,
                                    sxml_cast_frame *frame,
                                    sxml_cast_outcome *outcome) {
    sxml_item_heap *item_heap = machine->cast->item_heap;
    const sxml_type_registry *registry =
        sxml_item_heap_get_type_registry(item_heap);
    sxml_type_identifier_view members = frame->members;
    /* F&O 4.0, 23.3.7: the first condition that applies decides. An
       xs:string or xs:untypedAtomic is cast from its text (23.2), even when
       its type is a member; any other instance of a member stays as it
       is. */
    const bool is_string_source =
        frame->source.is_text ||
        sxml_primitive_is_string_source(sxml_source_primitive_type(frame->source));
    for (size_t member_index = 0;
         !is_string_source && member_index < members.type_identifier_count;
         member_index += 1) {
        sxml_type_identifier member = members.type_identifiers[member_index];
        sxml_type_description description;
        if (sxml_type_registry_describe_type(registry, member, &description) ==
                SXML_STATUS_OK &&
            description.variety == SXML_TYPE_VARIETY_ATOMIC &&
            sxml_source_is_derived_from(item_heap, frame->source, member)) {
            sxml_item_retain(frame->source.item);
            if (sxml_type_is_registered(frame->target_type)) {
                outcome->status = sxml_cast_check_union_facets(
                    machine, frame, frame->source.item);
                if (outcome->status != SXML_STATUS_OK) {
                    return false;
                }
            }
            outcome->value = sxml_sequence_from_item(frame->source.item);
            return false;
        }
    }
    frame->next_member_index = 0;
    frame->is_every_refusal_a_type_error =
        members.type_identifier_count != 0 &&
        !sxml_primitive_is_string_source(
            sxml_source_primitive_type(frame->source));
    outcome->status = sxml_cast_machine_push(machine, frame);
    return outcome->status == SXML_STATUS_OK;
}

/*
 * A list type such as xs:NMTOKENS (F&O 4.0, section 19.3.6 "Casting to
 * list types"): only from the string types; the collapsed text is split at
 * spaces and each token cast to the item type. Built-in list types need at
 * least one token (minLength 1, XSD 1.1 Part 2, section 3.4.5 for
 * xs:NMTOKENS). Returns true when it pushed frame to cast the tokens, else
 * writes *outcome.
 */
static bool sxml_cast_begin_list(sxml_cast_machine *machine,
                                 sxml_cast_frame *frame,
                                 sxml_cast_outcome *outcome) {
    sxml_item_heap *item_heap = machine->cast->item_heap;
    sxml_error_record *error_record =
        sxml_cast_machine_error_record(machine, frame->error_frame_index);
    if (!sxml_primitive_is_string_source(
            sxml_source_primitive_type(frame->source))) {
        outcome->status =
            sxml_error_record_report(error_record, &sxml_cast_type_error);
        return false;
    }
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    outcome->status = sxml_source_text(frame->source, &text);
    if (outcome->status != SXML_STATUS_OK) {
        return false;
    }
    outcome->status = sxml_apply_whitespace_facet(
        item_heap,
        (sxml_whitespace_request){.text = text,
                                  .facet = SXML_WHITESPACE_FACET_COLLAPSE},
        &frame->collapsed);
    if (outcome->status != SXML_STATUS_OK) {
        return false;
    }
    size_t token_count = sxml_count_tokens(frame->collapsed.text);
    if (token_count == 0) {
        /* A list type a schema made from xs:list may be empty (XSD 1.1
           Part 2, 4.1.4), with the facets of its restrictions. */
        if (sxml_type_is_registered(frame->target_type) &&
            sxml_cast_list_allows_no_items(
                sxml_item_heap_get_type_registry(item_heap),
                frame->target_type)) {
            outcome->status = sxml_cast_check_facets(
                item_heap, &(sxml_facet_check_request){
                               .type = frame->target_type,
                               .lexical_form = frame->collapsed.text,
                               .items = {.items = NULL, .item_count = 0},
                               .error_record = error_record});
        } else {
            outcome->status =
                sxml_error_record_report(error_record, &sxml_cast_value_error);
        }
        sxml_scratch_text_release(item_heap, &frame->collapsed);
        return false;
    }
    size_t byte_count = 0;
    void *memory = NULL;
    outcome->status =
        sxml_size_multiply(token_count, sizeof(sxml_item), &byte_count);
    if (outcome->status == SXML_STATUS_OK) {
        outcome->status = sxml_item_heap_allocate_bytes(
            item_heap,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(sxml_item)},
            &memory);
    }
    if (outcome->status != SXML_STATUS_OK) {
        sxml_scratch_text_release(item_heap, &frame->collapsed);
        return false;
    }
    frame->list = (sxml_list_items){
        .items = memory, .item_count = 0, .capacity = token_count};
    frame->next_token_offset = 0;
    outcome->status = sxml_cast_machine_push(machine, frame);
    if (outcome->status != SXML_STATUS_OK) {
        sxml_list_items_release(item_heap, &frame->list);
        sxml_scratch_text_release(item_heap, &frame->collapsed);
        return false;
    }
    return true;
}

/*
 * Begins the cast of step, nested as deep as the machine has frames: an
 * atomic or enumeration target, or a failure, is cast at once and writes
 * *outcome (returns false); a union, choice or list target pushes a frame
 * (returns true).
 */
static bool sxml_cast_begin(sxml_cast_machine *machine, sxml_cast_step step,
                            sxml_cast_outcome *outcome) {
    *outcome = (sxml_cast_outcome){.status = SXML_STATUS_OK,
                                   .value = sxml_sequence_empty()};
    if (machine->frame_count > SXML_CAST_MAXIMUM_TARGET_DEPTH) {
        outcome->status = SXML_STATUS_LIMIT_EXCEEDED;
        return false;
    }
    sxml_target_cast cast = *machine->cast;
    cast.source = step.source;
    cast.target_type = step.target_type;
    cast.error_record =
        sxml_cast_machine_error_record(machine, step.error_frame_index);
    sxml_item_heap *item_heap = cast.item_heap;
    const sxml_type_registry *registry =
        sxml_item_heap_get_type_registry(item_heap);
    sxml_type_description description;
    if (sxml_type_registry_describe_type(registry, cast.target_type,
                                         &description) != SXML_STATUS_OK) {
        outcome->status = SXML_STATUS_INVALID_ARGUMENT;
        return false;
    }
    sxml_cast_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.target_type = step.target_type;
    frame.source = step.source;
    frame.error_frame_index = step.error_frame_index;
    switch (description.variety) {
    case SXML_TYPE_VARIETY_ATOMIC: {
        sxml_atomic_target target;
        outcome->status =
            sxml_cast_resolve_atomic_target(registry, cast.target_type, &target);
        if (outcome->status != SXML_STATUS_OK) {
            return false;
        }
        if (!sxml_atomic_target_is_allowed(target)) {
            outcome->status = sxml_target_fail(&cast, &sxml_cast_target_error);
            return false;
        }
        sxml_item item = {.representation = 0};
        outcome->status = sxml_cast_to_atomic_target(
            &(sxml_atomic_cast){.item_heap = item_heap,
                                .source = cast.source,
                                .target = target,
                                .namespace_bindings = cast.namespace_bindings,
                                .error_record = cast.error_record},
            &item);
        if (outcome->status == SXML_STATUS_OK) {
            outcome->value = sxml_sequence_from_item(item);
        }
        return false;
    }
    case SXML_TYPE_VARIETY_UNION:
    case SXML_TYPE_VARIETY_CHOICE:
        frame.kind = SXML_CAST_FRAME_MEMBERS;
        frame.members = description.variety_details.member_types;
        return sxml_cast_begin_members(machine, &frame, outcome);
    case SXML_TYPE_VARIETY_ENUMERATION:
        outcome->status = sxml_cast_to_enumeration(
            &cast, description.variety_details.enumeration_values,
            &outcome->value);
        return false;
    case SXML_TYPE_VARIETY_LIST:
        frame.kind = SXML_CAST_FRAME_LIST;
        frame.item_type = description.variety_details.list_item_type;
        return sxml_cast_begin_list(machine, &frame, outcome);
    case SXML_TYPE_VARIETY_COMPLEX:
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_ANY_ITEM:
    case SXML_TYPE_VARIETY_NODE_TEST:
    case SXML_TYPE_VARIETY_JSON_NODE_TEST:
    case SXML_TYPE_VARIETY_FUNCTION:
    case SXML_TYPE_VARIETY_MAP:
    case SXML_TYPE_VARIETY_ARRAY:
    case SXML_TYPE_VARIETY_RECORD:
        outcome->status = sxml_target_fail(&cast, &sxml_cast_target_error);
        return false;
    }
    outcome->status = SXML_STATUS_INVALID_ARGUMENT;
    return false;
}

/*
 * The top frame begins the cast of its next member or token, or, when it
 * has none left, finishes and is popped. Returns true with *outcome for the
 * frame now on top (the cast finished at once, or the top frame did), false
 * when a frame was pushed.
 */
static bool sxml_cast_advance(sxml_cast_machine *machine,
                              sxml_cast_outcome *outcome) {
    size_t top_index = machine->frame_count - 1;
    sxml_cast_frame *top = &machine->frames[top_index];
    sxml_cast_step step;
    if (top->kind == SXML_CAST_FRAME_MEMBERS) {
        if (top->next_member_index == top->members.type_identifier_count) {
            *outcome = (sxml_cast_outcome){
                .status = sxml_cast_frame_fail(
                    machine, top->is_every_refusal_a_type_error
                                 ? &sxml_cast_type_error
                                 : &sxml_cast_value_error),
                .value = sxml_sequence_empty()};
            sxml_cast_machine_pop(machine);
            return true;
        }
        step = (sxml_cast_step){
            .source = top->source,
            .target_type =
                top->members.type_identifiers[top->next_member_index],
            .error_frame_index = top_index};
        top->next_member_index += 1;
    } else {
        sxml_byte_view text = top->collapsed.text;
        size_t token_offset = top->next_token_offset;
        if (token_offset > text.byte_count) {
            *outcome = (sxml_cast_outcome){.status = SXML_STATUS_OK,
                                           .value = sxml_sequence_empty()};
            if (sxml_type_is_registered(top->target_type)) {
                /* The facets of a list type (XSD 1.1 Part 2, 4.1.4). */
                outcome->status = sxml_cast_check_facets(
                    machine->cast->item_heap,
                    &(sxml_facet_check_request){
                        .type = top->target_type,
                        .lexical_form = text,
                        .items = {.items = top->list.items,
                                  .item_count = top->list.item_count},
                        .error_record = sxml_cast_machine_error_record(
                            machine, top->error_frame_index)});
                if (outcome->status != SXML_STATUS_OK) {
                    sxml_cast_machine_pop(machine);
                    return true;
                }
            }
            outcome->status = sxml_sequence_make_vector(
                machine->cast->item_heap,
                (sxml_item_view){.items = top->list.items,
                                 .item_count = top->list.item_count},
                &outcome->value);
            sxml_cast_machine_pop(machine);
            return true;
        }
        size_t token_end = token_offset;
        while (token_end < text.byte_count && text.bytes[token_end] != ' ') {
            token_end += 1;
        }
        step = (sxml_cast_step){
            .source = {.is_text = true,
                       .text = {.bytes = text.bytes + token_offset,
                                .byte_count = token_end - token_offset},
                       .item = {.representation = 0}},
            .target_type = top->item_type,
            .error_frame_index = top->error_frame_index};
        top->next_token_offset = token_end + 1;
    }
    return !sxml_cast_begin(machine, step, outcome);
}

/*
 * Gives the top frame the outcome of the cast it began. Returns true when
 * that finished the frame, which is popped and whose outcome replaces
 * *outcome, false when the frame goes on.
 */
static bool sxml_cast_deliver(sxml_cast_machine *machine,
                              sxml_cast_outcome *outcome) {
    sxml_cast_frame *top = &machine->frames[machine->frame_count - 1];
    if (top->kind == SXML_CAST_FRAME_MEMBERS) {
        if (outcome->status == SXML_STATUS_OK &&
            sxml_type_is_registered(top->target_type) &&
            sxml_sequence_get_representation(outcome->value) ==
                SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM) {
            /* A member's value is the union's when the union's own facets
               hold; no other member is tried. */
            outcome->status = sxml_cast_check_union_facets(
                machine, top, outcome->value.content);
            if (outcome->status != SXML_STATUS_OK) {
                outcome->value = sxml_sequence_empty();
            }
            sxml_cast_machine_pop(machine);
            return true;
        }
        if (!sxml_status_is_cast_error(outcome->status)) {
            sxml_cast_machine_pop(machine);
            return true;
        }
        if (outcome->status != SXML_STATUS_TYPE_ERROR) {
            top->is_every_refusal_a_type_error = false;
        }
        return false;
    }
    if (outcome->status != SXML_STATUS_OK) {
        sxml_cast_machine_pop(machine);
        return true;
    }
    if (sxml_sequence_get_representation(outcome->value) !=
        SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM) {
        /* An item type that is itself a list: refused (XSD 1.1 Part 2,
           section 2.4.1.2, list item types are atomic or unions). */
        sxml_sequence_release(outcome->value);
        *outcome = (sxml_cast_outcome){
            .status = sxml_cast_frame_fail(machine, &sxml_cast_target_error),
            .value = sxml_sequence_empty()};
        sxml_cast_machine_pop(machine);
        return true;
    }
    top->list.items[top->list.item_count] = outcome->value.content;
    top->list.item_count += 1;
    return false;
}

static sxml_status sxml_cast_to_target(const sxml_target_cast *cast,
                                       sxml_sequence *result) {
    sxml_cast_machine machine = {
        .cast = cast, .frames = NULL, .frame_count = 0, .frame_capacity = 0};
    sxml_cast_outcome outcome;
    bool has_outcome = !sxml_cast_begin(
        &machine,
        (sxml_cast_step){.source = cast->source,
                         .target_type = cast->target_type,
                         .error_frame_index = SXML_CAST_CALLER_ERROR_RECORD},
        &outcome);
    while (machine.frame_count != 0) {
        if (has_outcome) {
            has_outcome = sxml_cast_deliver(&machine, &outcome);
        } else {
            has_outcome = sxml_cast_advance(&machine, &outcome);
        }
    }
    sxml_item_heap_release_bytes(
        cast->item_heap,
        (sxml_heap_bytes){.memory = machine.frames,
                          .byte_count = machine.frame_capacity *
                                        sizeof(sxml_cast_frame)});
    if (outcome.status == SXML_STATUS_OK) {
        *result = outcome.value;
    }
    return outcome.status;
}

/* ------------------------------------------------------------ Entries */

/*
 * The single item of request->value: writes has_item false for the empty
 * sequence. More than one item, or one that is not atomic, is XPTY0004
 * (XPath 4.0, section 4.17.2).
 */
typedef struct sxml_cast_operand {
    bool has_item;
    sxml_item item;
} sxml_cast_operand;

static sxml_status sxml_cast_read_operand(const sxml_cast_request *request,
                                          sxml_cast_operand *operand) {
    uint64_t item_count = 0;
    sxml_status status =
        sxml_sequence_count(request->value, request->error_record, &item_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (item_count == 0) {
        if (!request->allows_empty) {
            return sxml_error_record_report(request->error_record,
                                            &sxml_cast_cardinality_error);
        }
        *operand = (sxml_cast_operand){.has_item = false,
                                       .item = {.representation = 0}};
        return SXML_STATUS_OK;
    }
    sxml_item item = {.representation = 0};
    if (item_count == 1) {
        status = sxml_sequence_get_item(
            request->value,
            (sxml_item_request){.item_index = 0,
                                .error_record = request->error_record},
            &item);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (item_count != 1 || !sxml_item_is_atomic(item)) {
        return sxml_error_record_report(request->error_record,
                                        &sxml_cast_cardinality_error);
    }
    *operand = (sxml_cast_operand){.has_item = true, .item = item};
    return SXML_STATUS_OK;
}

sxml_status sxml_cast_make_value(sxml_item_heap *item_heap,
                                 const sxml_cast_request *request,
                                 sxml_sequence *result) {
    if (item_heap == NULL || request == NULL || result == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_cast_operand operand = {.has_item = false,
                                 .item = {.representation = 0}};
    sxml_status status = sxml_cast_read_operand(request, &operand);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!operand.has_item) {
        *result = sxml_sequence_empty();
        return SXML_STATUS_OK;
    }
    return sxml_cast_to_target(
        &(sxml_target_cast){.item_heap = item_heap,
                            .source = {.is_text = false,
                                       .text = {.bytes = NULL, .byte_count = 0},
                                       .item = operand.item},
                            .target_type = request->target_type,
                            .namespace_bindings = request->namespace_bindings,
                            .error_record = request->error_record},
        result);
}

sxml_status sxml_cast_test_castability(sxml_item_heap *item_heap,
                                       const sxml_cast_request *request,
                                       sxml_castability *castability) {
    if (item_heap == NULL || request == NULL || castability == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* An error reading the operand is not an answer: count it with the
       caller's error record first. */
    uint64_t item_count = 0;
    sxml_status count_status =
        sxml_sequence_count(request->value, request->error_record, &item_count);
    if (count_status != SXML_STATUS_OK) {
        return count_status;
    }
    sxml_error_record cast_error;
    sxml_cast_request trial = *request;
    trial.error_record = &cast_error;
    sxml_sequence value = sxml_sequence_empty();
    sxml_status status = sxml_cast_make_value(item_heap, &trial, &value);
    if (status == SXML_STATUS_OK) {
        sxml_sequence_release(value);
        *castability = (sxml_castability){.is_castable = true};
        return SXML_STATUS_OK;
    }
    if (sxml_status_is_cast_error(status)) {
        *castability = (sxml_castability){.is_castable = false};
        return SXML_STATUS_OK;
    }
    if (status == SXML_STATUS_STATIC_ERROR && request->error_record != NULL) {
        *request->error_record = cast_error;
    }
    return status;
}

sxml_status
sxml_cast_make_value_from_text(sxml_item_heap *item_heap,
                               const sxml_text_cast_request *request,
                               sxml_sequence *result) {
    if (item_heap == NULL || request == NULL || result == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_utf8_validation validation;
    if (sxml_utf8_validate(request->text, &validation) != SXML_STATUS_OK) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_cast_to_target(
        &(sxml_target_cast){.item_heap = item_heap,
                            .source = {.is_text = true,
                                       .text = request->text,
                                       .item = {.representation = 0}},
                            .target_type = request->target_type,
                            .namespace_bindings = request->namespace_bindings,
                            .error_record = request->error_record},
        result);
}
