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

/* The whiteSpace facet (XSD 1.1 Part 2, section 4.3.6). */
typedef enum sxml_whitespace_facet {
    SXML_WHITESPACE_FACET_PRESERVE,
    SXML_WHITESPACE_FACET_REPLACE,
    SXML_WHITESPACE_FACET_COLLAPSE,
} sxml_whitespace_facet;

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

static bool sxml_is_whitespace(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
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
            bytes[byte_count] = sxml_is_whitespace(byte) ? ' ' : byte;
            byte_count += 1;
            continue;
        }
        if (sxml_is_whitespace(byte)) {
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

/*
 * Resolves an atomic type to its nearest built-in ancestor and primitive
 * type. Returns SXML_STATUS_INVALID_ARGUMENT when type is not atomic.
 */
static sxml_status
sxml_resolve_atomic_target(const sxml_type_registry *registry,
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

static sxml_status
sxml_write_canonical_scratch(sxml_item_heap *item_heap, sxml_item item,
                             sxml_canonical_scratch *canonical) {
    canonical->scratch =
        (sxml_scratch_text){.text = {.bytes = NULL, .byte_count = 0},
                            .bytes = {.memory = NULL, .byte_count = 0}};
    sxml_canonical_text text;
    sxml_status status = sxml_cast_write_canonical_text(
        item_heap,
        &(sxml_canonical_text_request){
            .item = item,
            .output = {.bytes = canonical->bytes,
                       .byte_count = sizeof(canonical->bytes)}},
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
                           .byte_count = text.required_byte_count}},
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
    sxml_scratch_text processed;
    sxml_status status = sxml_apply_whitespace_facet(
        item_heap,
        (sxml_whitespace_request){
            .text = text,
            .facet = sxml_whitespace_facet_of(cast->target.builtin_type)},
        &processed);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_cast_make_item_from_lexical_form(
        item_heap,
        &(sxml_lexical_form_request){.lexical_form = processed.text,
                                     .target = cast->target,
                                     .namespace_bindings =
                                         cast->namespace_bindings,
                                     .error_record = cast->error_record},
        result);
    sxml_scratch_text_release(item_heap, &processed);
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
        sxml_status status =
            sxml_write_canonical_scratch(item_heap, source.item, &canonical);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        status = sxml_cast_text(cast, canonical.scratch.text, result);
        sxml_scratch_text_release(item_heap, &canonical.scratch);
        return status;
    }
    return sxml_cast_datum(cast, result);
}

/* ---------------------------------------------------------- Any target */

/* A source to cast to any target type. */
typedef struct sxml_target_cast {
    sxml_item_heap *item_heap;
    sxml_cast_source source;
    sxml_type_identifier target_type;
    sxml_namespace_binding_view namespace_bindings;
    sxml_error_record *error_record;
    /* Nesting of union, choice and list targets so far. */
    uint32_t depth;
} sxml_target_cast;

static sxml_status sxml_cast_to_target(const sxml_target_cast *cast,
                                       sxml_sequence *result);

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
 * A union or a choice (F&O 4.0, section 19.3.5 "Casting to union types";
 * XPath 4.0, section 4.17.1): a source that is already an instance of an
 * atomic member is returned unchanged; otherwise the first member the cast
 * to which succeeds gives the result. When none does: XPTY0004 when every
 * member refused the source type, otherwise FORG0001 (xs:error, the union
 * without members, always FORG0001).
 */
static sxml_status sxml_cast_to_members(const sxml_target_cast *cast,
                                        sxml_type_identifier_view members,
                                        sxml_sequence *result) {
    sxml_item_heap *item_heap = cast->item_heap;
    const sxml_type_registry *registry =
        sxml_item_heap_get_type_registry(item_heap);
    for (size_t member_index = 0;
         !cast->source.is_text && member_index < members.type_identifier_count;
         member_index += 1) {
        sxml_type_identifier member = members.type_identifiers[member_index];
        sxml_type_description description;
        if (sxml_type_registry_describe_type(registry, member, &description) ==
                SXML_STATUS_OK &&
            description.variety == SXML_TYPE_VARIETY_ATOMIC &&
            sxml_source_is_derived_from(item_heap, cast->source, member)) {
            sxml_item_retain(cast->source.item);
            *result = sxml_sequence_from_item(cast->source.item);
            return SXML_STATUS_OK;
        }
    }
    bool is_every_refusal_a_type_error =
        members.type_identifier_count != 0 &&
        !sxml_primitive_is_string_source(
            sxml_source_primitive_type(cast->source));
    for (size_t member_index = 0; member_index < members.type_identifier_count;
         member_index += 1) {
        sxml_error_record member_error;
        sxml_target_cast member_cast = *cast;
        member_cast.target_type = members.type_identifiers[member_index];
        member_cast.error_record = &member_error;
        member_cast.depth = cast->depth + 1U;
        sxml_status status = sxml_cast_to_target(&member_cast, result);
        if (!sxml_status_is_cast_error(status)) {
            return status;
        }
        if (status != SXML_STATUS_TYPE_ERROR) {
            is_every_refusal_a_type_error = false;
        }
    }
    return sxml_target_fail(cast, is_every_refusal_a_type_error
                                      ? &sxml_cast_type_error
                                      : &sxml_cast_value_error);
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

/* Casts the tokens of collapsed text to the item type, one item each. */
typedef struct sxml_list_request {
    sxml_byte_view text;
    sxml_type_identifier item_type;
} sxml_list_request;

static sxml_status sxml_cast_tokens(const sxml_target_cast *cast,
                                    sxml_list_request request,
                                    sxml_list_items *list) {
    size_t token_offset = 0;
    sxml_byte_view text = request.text;
    for (size_t byte_index = 0; byte_index <= text.byte_count;
         byte_index += 1) {
        if (byte_index != text.byte_count && text.bytes[byte_index] != ' ') {
            continue;
        }
        sxml_target_cast token_cast = *cast;
        token_cast.source = (sxml_cast_source){
            .is_text = true,
            .text = {.bytes = text.bytes + token_offset,
                     .byte_count = byte_index - token_offset},
            .item = {.representation = 0}};
        token_cast.target_type = request.item_type;
        token_cast.depth = cast->depth + 1U;
        sxml_sequence token_value = sxml_sequence_empty();
        sxml_status status = sxml_cast_to_target(&token_cast, &token_value);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (sxml_sequence_get_representation(token_value) !=
            SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM) {
            /* An item type that is itself a list: refused (XSD 1.1 Part 2,
               section 2.4.1.2, list item types are atomic or unions). */
            sxml_sequence_release(token_value);
            return sxml_target_fail(cast, &sxml_cast_target_error);
        }
        list->items[list->item_count] = token_value.content;
        list->item_count += 1;
        token_offset = byte_index + 1;
    }
    return SXML_STATUS_OK;
}

/*
 * A list type such as xs:NMTOKENS (F&O 4.0, section 19.3.6 "Casting to
 * list types"): only from the string types; the collapsed text is split at
 * spaces and each token cast to the item type. Built-in list types need at
 * least one token (minLength 1, XSD 1.1 Part 2, section 3.4.5 for
 * xs:NMTOKENS).
 */
static sxml_status sxml_cast_to_list(const sxml_target_cast *cast,
                                     sxml_type_identifier item_type,
                                     sxml_sequence *result) {
    sxml_item_heap *item_heap = cast->item_heap;
    if (!sxml_primitive_is_string_source(
            sxml_source_primitive_type(cast->source))) {
        return sxml_target_fail(cast, &sxml_cast_type_error);
    }
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_source_text(cast->source, &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_scratch_text collapsed;
    status = sxml_apply_whitespace_facet(
        item_heap,
        (sxml_whitespace_request){.text = text,
                                  .facet = SXML_WHITESPACE_FACET_COLLAPSE},
        &collapsed);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t token_count = sxml_count_tokens(collapsed.text);
    if (token_count == 0) {
        sxml_scratch_text_release(item_heap, &collapsed);
        return sxml_target_fail(cast, &sxml_cast_value_error);
    }
    sxml_list_items list = {
        .items = NULL, .item_count = 0, .capacity = token_count};
    size_t byte_count = 0;
    status = sxml_size_multiply(token_count, sizeof(sxml_item), &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_item_heap_allocate_bytes(
            item_heap,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(sxml_item)},
            &memory);
    }
    if (status == SXML_STATUS_OK) {
        list.items = memory;
        status = sxml_cast_tokens(
            cast,
            (sxml_list_request){.text = collapsed.text, .item_type = item_type},
            &list);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_make_vector(
            item_heap,
            (sxml_item_view){.items = list.items,
                             .item_count = list.item_count},
            result);
    }
    if (memory != NULL) {
        sxml_list_items_release(item_heap, &list);
    }
    sxml_scratch_text_release(item_heap, &collapsed);
    return status;
}

static sxml_status sxml_cast_to_target(const sxml_target_cast *cast,
                                       sxml_sequence *result) {
    sxml_item_heap *item_heap = cast->item_heap;
    if (cast->depth > SXML_CAST_MAXIMUM_TARGET_DEPTH) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    const sxml_type_registry *registry =
        sxml_item_heap_get_type_registry(item_heap);
    sxml_type_description description;
    if (sxml_type_registry_describe_type(registry, cast->target_type,
                                         &description) != SXML_STATUS_OK) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    switch (description.variety) {
    case SXML_TYPE_VARIETY_ATOMIC: {
        sxml_atomic_target target;
        sxml_status status =
            sxml_resolve_atomic_target(registry, cast->target_type, &target);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!sxml_atomic_target_is_allowed(target)) {
            return sxml_target_fail(cast, &sxml_cast_target_error);
        }
        sxml_item item = {.representation = 0};
        status = sxml_cast_to_atomic_target(
            &(sxml_atomic_cast){.item_heap = item_heap,
                                .source = cast->source,
                                .target = target,
                                .namespace_bindings = cast->namespace_bindings,
                                .error_record = cast->error_record},
            &item);
        if (status == SXML_STATUS_OK) {
            *result = sxml_sequence_from_item(item);
        }
        return status;
    }
    case SXML_TYPE_VARIETY_UNION:
    case SXML_TYPE_VARIETY_CHOICE:
        return sxml_cast_to_members(
            cast, description.variety_details.member_types, result);
    case SXML_TYPE_VARIETY_ENUMERATION:
        return sxml_cast_to_enumeration(
            cast, description.variety_details.enumeration_values, result);
    case SXML_TYPE_VARIETY_LIST:
        return sxml_cast_to_list(
            cast, description.variety_details.list_item_type, result);
    case SXML_TYPE_VARIETY_COMPLEX:
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_ANY_ITEM:
    case SXML_TYPE_VARIETY_NODE_TEST:
    case SXML_TYPE_VARIETY_JSON_NODE_TEST:
    case SXML_TYPE_VARIETY_FUNCTION:
    case SXML_TYPE_VARIETY_MAP:
    case SXML_TYPE_VARIETY_ARRAY:
    case SXML_TYPE_VARIETY_RECORD:
        return sxml_target_fail(cast, &sxml_cast_target_error);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
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
                            .error_record = request->error_record,
                            .depth = 0},
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
                            .error_record = request->error_record,
                            .depth = 0},
        result);
}
