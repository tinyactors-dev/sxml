/*
 * Schema-informed EXI grammars (EXI 1.0, 8.5; D333): the document and
 * fragment grammars (8.5.1, 8.5.2), the element and type grammars of every
 * declaration and type of a compiled schema (8.5.4), their datatypes
 * (7.1, 7.2) and the string table entries a schema adds (7.3.1, Appendix D).
 * Golden: src/exi_grammar.c (the built-in grammars: tails numbered once
 * per stream, event codes of up to three parts).
 *
 * The oracle (EXIficient 1.0.4) builds what 8.5.4.1 to 8.5.4.3 describe
 * with an automaton of each content model, and this file builds the same
 * grammars the same way, so that every event code is the oracle's:
 *
 * - A content model (8.5.4.1.5 to 8.5.4.1.8) becomes a deterministic
 *   automaton: a nondeterministic one of the particles' copies (min and max
 *   occurs expanded) whose subsets are the states, as the normalization of
 *   8.5.4.2 unites non-terminals. Its states are element grammars; their
 *   SE productions follow the schema order of their terms (8.5.4.3: the
 *   first appearance in a depth-first walk with group references in
 *   place), each term's substitution group sorted by local-name then URI
 *   (8.5.4.1.6).
 * - The attribute uses, sorted (8.5.4.1.3.2), make start tags in front of
 *   the content's first state, which a start tag copies; the content index
 *   of 8.5.4.4.1 is that copy, and Element_{i,content2} the first state
 *   itself.
 * - A grammar is one of three kinds by the undeclared productions of
 *   8.5.4.4.1 it has: the first start tag (j = 0), the other start tags
 *   (0 < j <= content), and element grammars (j > content and content2);
 *   those productions are not stored but numbered at each event from the
 *   kind and the options (sxml_exi_schema_undeclared_list).
 * - TypeEmpty (8.5.4.1.3) of a start tag keeps its attribute productions,
 *   each leading to the next start tag's TypeEmpty, and an EE.
 *
 * Every walk uses explicit stacks and work lists (D71).
 */
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

#include "exi_internal.h"
#include "memory_internal.h"
#include "processor_internal.h"
#include "schema_internal.h"

#define SXML_EXI_SCHEMA_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

enum {
    /* Bounds on what one schema may build (8.5.4.1.5 expands occurrences
       into copies). */
    SXML_EXI_MAXIMUM_AUTOMATON_NODES = 1 << 22,
    SXML_EXI_MAXIMUM_AUTOMATON_STATES = 1 << 20,
};

static const sxml_byte_view sxml_exi_xml_schema_uri = SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/2001/XMLSchema");

/* ------------------------------------------------------- Built-in types */

/* What a built-in type of XML Schema 1.0 brings to the datatype of the
   types derived from it (7.1, Table 7-1): its primitive's representation,
   its whiteSpace facet, its own bounds and patterns, its base. */
typedef enum sxml_exi_primitive {
    SXML_EXI_PRIMITIVE_STRING,
    SXML_EXI_PRIMITIVE_BOOLEAN,
    SXML_EXI_PRIMITIVE_DECIMAL,
    SXML_EXI_PRIMITIVE_FLOAT,
    SXML_EXI_PRIMITIVE_BASE64_BINARY,
    SXML_EXI_PRIMITIVE_HEX_BINARY,
    SXML_EXI_PRIMITIVE_DATE_TIME,
    /* xs:QName and xs:NOTATION: strings, never enumerations (7.2). */
    SXML_EXI_PRIMITIVE_QNAME,
    SXML_EXI_PRIMITIVE_LIST,
    SXML_EXI_PRIMITIVE_COMPLEX,
} sxml_exi_primitive;

typedef struct sxml_exi_builtin_type {
    sxml_byte_view name;
    /* Its own minInclusive and maxInclusive, empty when it has none. */
    sxml_byte_view minimum;
    sxml_byte_view maximum;
    sxml_exi_primitive primitive;
    sxml_exi_date_time_kind date_time_kind;
    sxml_exi_whitespace whitespace;
    /* The built-in type it is derived from, an index of this table (its
       own for xs:anyType). */
    uint32_t base;
    /* PRIMITIVE_LIST: the item type. */
    uint32_t item;
    /* Derived from xs:integer (7.1.5). */
    bool is_integer;
    /* It has pattern facets of its own or inherited. */
    bool has_pattern;
} sxml_exi_builtin_type;

/* The indexes of the table below that other entries name. */
enum {
    SXML_EXI_BUILTIN_ANY_TYPE = 0,
    SXML_EXI_BUILTIN_ANY_SIMPLE_TYPE = 1,
    SXML_EXI_BUILTIN_STRING = 2,
    SXML_EXI_BUILTIN_NORMALIZED_STRING = 3,
    SXML_EXI_BUILTIN_TOKEN = 4,
    SXML_EXI_BUILTIN_NAME = 6,
    SXML_EXI_BUILTIN_NCNAME = 7,
    SXML_EXI_BUILTIN_ID_REFERENCE = 9,
    SXML_EXI_BUILTIN_ENTITY = 10,
    SXML_EXI_BUILTIN_NAME_TOKEN = 11,
    SXML_EXI_BUILTIN_DECIMAL = 20,
    SXML_EXI_BUILTIN_INTEGER = 21,
    SXML_EXI_BUILTIN_NON_POSITIVE_INTEGER = 22,
    SXML_EXI_BUILTIN_LONG = 24,
    SXML_EXI_BUILTIN_INT = 25,
    SXML_EXI_BUILTIN_SHORT = 26,
    SXML_EXI_BUILTIN_NON_NEGATIVE_INTEGER = 28,
    SXML_EXI_BUILTIN_UNSIGNED_LONG = 29,
    SXML_EXI_BUILTIN_UNSIGNED_INT = 30,
    SXML_EXI_BUILTIN_UNSIGNED_SHORT = 31,
    SXML_EXI_BUILTIN_COUNT = 46,
};

#define SXML_EXI_NAME(literal) SXML_BYTE_VIEW_INITIALIZER(literal)
#define SXML_EXI_NO_BOUND {.bytes = NULL, .byte_count = 0}

/* XML Schema 1.0's built-in types (Part 2, 3), the 46 names of Table D-5. */
static const sxml_exi_builtin_type sxml_exi_builtin_types[SXML_EXI_BUILTIN_COUNT] = {
    {SXML_EXI_NAME("anyType"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_COMPLEX, 0, SXML_EXI_WHITESPACE_PRESERVE, 0, 0, false, false},
    {SXML_EXI_NAME("anySimpleType"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_STRING, 0, SXML_EXI_WHITESPACE_PRESERVE, 0, 0, false, false},
    {SXML_EXI_NAME("string"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_STRING, 0, SXML_EXI_WHITESPACE_PRESERVE, 1, 0, false, false},
    {SXML_EXI_NAME("normalizedString"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_STRING, 0, SXML_EXI_WHITESPACE_REPLACE, 2, 0, false, false},
    {SXML_EXI_NAME("token"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_STRING, 0, SXML_EXI_WHITESPACE_COLLAPSE, 3, 0, false, false},
    {SXML_EXI_NAME("language"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_STRING, 0, SXML_EXI_WHITESPACE_COLLAPSE, 4, 0, false, true},
    {SXML_EXI_NAME("Name"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_STRING, 0, SXML_EXI_WHITESPACE_COLLAPSE, 4, 0, false, true},
    {SXML_EXI_NAME("NCName"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_STRING, 0, SXML_EXI_WHITESPACE_COLLAPSE, 6, 0, false, true},
    {SXML_EXI_NAME("ID"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_STRING, 0, SXML_EXI_WHITESPACE_COLLAPSE, 7, 0, false, true},
    {SXML_EXI_NAME("IDREF"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_STRING, 0, SXML_EXI_WHITESPACE_COLLAPSE, 7, 0, false, true},
    {SXML_EXI_NAME("ENTITY"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_STRING, 0, SXML_EXI_WHITESPACE_COLLAPSE, 7, 0, false, true},
    {SXML_EXI_NAME("NMTOKEN"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_STRING, 0, SXML_EXI_WHITESPACE_COLLAPSE, 4, 0, false, true},
    {SXML_EXI_NAME("NMTOKENS"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_LIST, 0, SXML_EXI_WHITESPACE_COLLAPSE, 1, SXML_EXI_BUILTIN_NAME_TOKEN, false, false},
    {SXML_EXI_NAME("IDREFS"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_LIST, 0, SXML_EXI_WHITESPACE_COLLAPSE, 1, SXML_EXI_BUILTIN_ID_REFERENCE, false, false},
    {SXML_EXI_NAME("ENTITIES"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_LIST, 0, SXML_EXI_WHITESPACE_COLLAPSE, 1, SXML_EXI_BUILTIN_ENTITY, false, false},
    {SXML_EXI_NAME("boolean"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_BOOLEAN, 0, SXML_EXI_WHITESPACE_COLLAPSE, 1, 0, false, false},
    {SXML_EXI_NAME("base64Binary"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_BASE64_BINARY, 0, SXML_EXI_WHITESPACE_COLLAPSE, 1, 0, false, false},
    {SXML_EXI_NAME("hexBinary"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_HEX_BINARY, 0, SXML_EXI_WHITESPACE_COLLAPSE, 1, 0, false, false},
    {SXML_EXI_NAME("float"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_FLOAT, 0, SXML_EXI_WHITESPACE_COLLAPSE, 1, 0, false, false},
    {SXML_EXI_NAME("double"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_FLOAT, 0, SXML_EXI_WHITESPACE_COLLAPSE, 1, 0, false, false},
    {SXML_EXI_NAME("decimal"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_DECIMAL, 0, SXML_EXI_WHITESPACE_COLLAPSE, 1, 0, false, false},
    {SXML_EXI_NAME("integer"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_DECIMAL, 0, SXML_EXI_WHITESPACE_COLLAPSE, 20, 0, true, false},
    {SXML_EXI_NAME("nonPositiveInteger"), SXML_EXI_NO_BOUND, SXML_EXI_NAME("0"), SXML_EXI_PRIMITIVE_DECIMAL, 0, SXML_EXI_WHITESPACE_COLLAPSE, 21, 0, true, false},
    {SXML_EXI_NAME("negativeInteger"), SXML_EXI_NO_BOUND, SXML_EXI_NAME("-1"), SXML_EXI_PRIMITIVE_DECIMAL, 0, SXML_EXI_WHITESPACE_COLLAPSE, 22, 0, true, false},
    {SXML_EXI_NAME("long"), SXML_EXI_NAME("-9223372036854775808"), SXML_EXI_NAME("9223372036854775807"), SXML_EXI_PRIMITIVE_DECIMAL, 0, SXML_EXI_WHITESPACE_COLLAPSE, 21, 0, true, false},
    {SXML_EXI_NAME("int"), SXML_EXI_NAME("-2147483648"), SXML_EXI_NAME("2147483647"), SXML_EXI_PRIMITIVE_DECIMAL, 0, SXML_EXI_WHITESPACE_COLLAPSE, 24, 0, true, false},
    {SXML_EXI_NAME("short"), SXML_EXI_NAME("-32768"), SXML_EXI_NAME("32767"), SXML_EXI_PRIMITIVE_DECIMAL, 0, SXML_EXI_WHITESPACE_COLLAPSE, 25, 0, true, false},
    {SXML_EXI_NAME("byte"), SXML_EXI_NAME("-128"), SXML_EXI_NAME("127"), SXML_EXI_PRIMITIVE_DECIMAL, 0, SXML_EXI_WHITESPACE_COLLAPSE, 26, 0, true, false},
    {SXML_EXI_NAME("nonNegativeInteger"), SXML_EXI_NAME("0"), SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_DECIMAL, 0, SXML_EXI_WHITESPACE_COLLAPSE, 21, 0, true, false},
    {SXML_EXI_NAME("unsignedLong"), SXML_EXI_NAME("0"), SXML_EXI_NAME("18446744073709551615"), SXML_EXI_PRIMITIVE_DECIMAL, 0, SXML_EXI_WHITESPACE_COLLAPSE, 28, 0, true, false},
    {SXML_EXI_NAME("unsignedInt"), SXML_EXI_NAME("0"), SXML_EXI_NAME("4294967295"), SXML_EXI_PRIMITIVE_DECIMAL, 0, SXML_EXI_WHITESPACE_COLLAPSE, 29, 0, true, false},
    {SXML_EXI_NAME("unsignedShort"), SXML_EXI_NAME("0"), SXML_EXI_NAME("65535"), SXML_EXI_PRIMITIVE_DECIMAL, 0, SXML_EXI_WHITESPACE_COLLAPSE, 30, 0, true, false},
    {SXML_EXI_NAME("unsignedByte"), SXML_EXI_NAME("0"), SXML_EXI_NAME("255"), SXML_EXI_PRIMITIVE_DECIMAL, 0, SXML_EXI_WHITESPACE_COLLAPSE, 31, 0, true, false},
    {SXML_EXI_NAME("positiveInteger"), SXML_EXI_NAME("1"), SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_DECIMAL, 0, SXML_EXI_WHITESPACE_COLLAPSE, 28, 0, true, false},
    {SXML_EXI_NAME("duration"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_STRING, 0, SXML_EXI_WHITESPACE_COLLAPSE, 1, 0, false, false},
    {SXML_EXI_NAME("dateTime"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_DATE_TIME, SXML_EXI_DATE_TIME_DATE_TIME, SXML_EXI_WHITESPACE_COLLAPSE, 1, 0, false, false},
    {SXML_EXI_NAME("time"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_DATE_TIME, SXML_EXI_DATE_TIME_TIME, SXML_EXI_WHITESPACE_COLLAPSE, 1, 0, false, false},
    {SXML_EXI_NAME("date"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_DATE_TIME, SXML_EXI_DATE_TIME_DATE, SXML_EXI_WHITESPACE_COLLAPSE, 1, 0, false, false},
    {SXML_EXI_NAME("gYearMonth"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_DATE_TIME, SXML_EXI_DATE_TIME_G_YEAR_MONTH, SXML_EXI_WHITESPACE_COLLAPSE, 1, 0, false, false},
    {SXML_EXI_NAME("gYear"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_DATE_TIME, SXML_EXI_DATE_TIME_G_YEAR, SXML_EXI_WHITESPACE_COLLAPSE, 1, 0, false, false},
    {SXML_EXI_NAME("gMonthDay"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_DATE_TIME, SXML_EXI_DATE_TIME_G_MONTH_DAY, SXML_EXI_WHITESPACE_COLLAPSE, 1, 0, false, false},
    {SXML_EXI_NAME("gDay"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_DATE_TIME, SXML_EXI_DATE_TIME_G_DAY, SXML_EXI_WHITESPACE_COLLAPSE, 1, 0, false, false},
    {SXML_EXI_NAME("gMonth"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_DATE_TIME, SXML_EXI_DATE_TIME_G_MONTH, SXML_EXI_WHITESPACE_COLLAPSE, 1, 0, false, false},
    {SXML_EXI_NAME("anyURI"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_STRING, 0, SXML_EXI_WHITESPACE_COLLAPSE, 1, 0, false, false},
    {SXML_EXI_NAME("QName"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_QNAME, 0, SXML_EXI_WHITESPACE_COLLAPSE, 1, 0, false, false},
    {SXML_EXI_NAME("NOTATION"), SXML_EXI_NO_BOUND, SXML_EXI_NO_BOUND, SXML_EXI_PRIMITIVE_QNAME, 0, SXML_EXI_WHITESPACE_COLLAPSE, 1, 0, false, false},
};

/* XML Schema 1.1 names the registry has beside them, read as the 1.0 type
   the oracle would have instead (it knows XML Schema 1.0 only). */
typedef struct sxml_exi_builtin_alias {
    sxml_byte_view name;
    uint32_t builtin;
} sxml_exi_builtin_alias;

static const sxml_exi_builtin_alias sxml_exi_builtin_aliases[] = {
    {SXML_EXI_NAME("anyAtomicType"), SXML_EXI_BUILTIN_ANY_SIMPLE_TYPE},
    {SXML_EXI_NAME("untyped"), SXML_EXI_BUILTIN_ANY_TYPE},
    {SXML_EXI_NAME("untypedAtomic"), SXML_EXI_BUILTIN_ANY_SIMPLE_TYPE},
    {SXML_EXI_NAME("dateTimeStamp"), 35},
    {SXML_EXI_NAME("dayTimeDuration"), 34},
    {SXML_EXI_NAME("yearMonthDuration"), 34},
    {SXML_EXI_NAME("numeric"), SXML_EXI_BUILTIN_ANY_SIMPLE_TYPE},
    {SXML_EXI_NAME("error"), SXML_EXI_BUILTIN_ANY_SIMPLE_TYPE},
};

static bool sxml_exi_views_equal(sxml_byte_view first, sxml_byte_view second) {
    return first.byte_count == second.byte_count &&
           (first.byte_count == 0 || memcmp(first.bytes, second.bytes, first.byte_count) == 0);
}

/* Lexicographical order of UTF-8 (the order of code points). */
static int sxml_exi_view_order(sxml_byte_view first, sxml_byte_view second) {
    const size_t common = first.byte_count < second.byte_count ? first.byte_count : second.byte_count;
    const int order = common == 0 ? 0 : memcmp(first.bytes, second.bytes, common);
    if (order != 0) {
        return order;
    }
    return first.byte_count < second.byte_count ? -1 : first.byte_count > second.byte_count ? 1 : 0;
}

/* ------------------------------------------------------------- Buffers */

sxml_status sxml_exi_buffer_append(sxml_exi_buffer *buffer, sxml_byte_view bytes) {
    if (bytes.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    if (buffer->byte_count + bytes.byte_count > buffer->capacity) {
        void *elements = buffer->bytes;
        sxml_status status = sxml_allocator_reserve_array(
            buffer->allocator, &(sxml_array_reservation_request){.elements = &elements,
                                                                 .capacity = &buffer->capacity,
                                                                 .element_count = buffer->byte_count,
                                                                 .required_count = buffer->byte_count + bytes.byte_count,
                                                                 .element_size = 1,
                                                                 .element_alignment = 1,
                                                                 .minimum_capacity = 256});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        buffer->bytes = elements;
    }
    memcpy(buffer->bytes + buffer->byte_count, bytes.bytes, bytes.byte_count);
    buffer->byte_count += bytes.byte_count;
    return SXML_STATUS_OK;
}

void sxml_exi_buffer_release(sxml_exi_buffer *buffer) {
    sxml_allocator_release(buffer->allocator, buffer->bytes);
    *buffer = (sxml_exi_buffer){.allocator = buffer->allocator};
}

/* ------------------------------------------------------------- Builder */

/* A named type of the schema's model: a built-in type (an index of
   sxml_exi_builtin_types) or a type definition of the components. */
typedef struct sxml_exi_type_reference {
    bool is_builtin;
    uint32_t index;
} sxml_exi_type_reference;

/* A qname of the string table. */
typedef struct sxml_exi_name_entry {
    sxml_byte_view uri;
    sxml_byte_view local_name;
} sxml_exi_name_entry;

typedef struct sxml_exi_builder {
    const sxml_exi_schema_build *build;
    sxml_exi_schema_grammars *grammars;
    sxml_exi_string_table *table;
    sxml_allocator allocator;
    /* NULL for the built-in types only. */
    const sxml_schema_components *components;
    const sxml_name_pool *name_pool;
    size_t type_count;
    /* Per type definition: the built-in type it is (SXML_EXI_NONE for a
       schema's own), its datatype and its type grammar. */
    uint32_t *type_builtins;
    uint32_t *type_datatypes;
    uint32_t *type_grammars;
    /* Per type definition: it is the base of a named type (8.5.4.4.2). */
    bool *type_has_named_subtypes;
    uint32_t builtin_datatypes[SXML_EXI_BUILTIN_COUNT];
    uint32_t builtin_grammars[SXML_EXI_BUILTIN_COUNT];
    bool builtin_has_named_subtypes[SXML_EXI_BUILTIN_COUNT];
    /* The productions of the grammar being made. */
    sxml_exi_schema_production *scratch;
    size_t scratch_count;
    size_t scratch_capacity;
    /* String table names collected from the schema (7.3.1). */
    sxml_exi_name_entry *names;
    size_t name_count;
    size_t name_capacity;
    sxml_byte_view failure_description;
} sxml_exi_builder;

#define SXML_EXI_BUILD_FAIL(builder, failure_status, literal) \
    sxml_exi_build_fail((builder), (sxml_exi_build_failure){.status = (failure_status), .description = SXML_EXI_SCHEMA_TEXT(literal)})

typedef struct sxml_exi_build_failure {
    sxml_status status;
    sxml_byte_view description;
} sxml_exi_build_failure;

static sxml_status sxml_exi_build_fail(sxml_exi_builder *builder, sxml_exi_build_failure failure) {
    if (builder->failure_description.byte_count == 0) {
        builder->failure_description = failure.description;
    }
    return failure.status;
}

/* Appends one element to a growable array of the builder's allocator. */
typedef struct sxml_exi_growth {
    void **elements;
    size_t *capacity;
    size_t element_count;
    size_t element_size;
} sxml_exi_growth;

static sxml_status sxml_exi_grow_array(sxml_allocator allocator, const sxml_exi_growth *growth) {
    if (growth->element_count < *growth->capacity) {
        return SXML_STATUS_OK;
    }
    return sxml_allocator_reserve_array(allocator, &(sxml_array_reservation_request){
                                                       .elements = growth->elements,
                                                       .capacity = growth->capacity,
                                                       .element_count = growth->element_count,
                                                       .required_count = growth->element_count + 1,
                                                       .element_size = growth->element_size,
                                                       .element_alignment = alignof(max_align_t),
                                                       .minimum_capacity = 16});
}

static sxml_status sxml_exi_store(sxml_exi_schema_grammars *grammars, sxml_byte_view bytes, sxml_exi_text *text) {
    const size_t offset = grammars->text.byte_count;
    sxml_status status = sxml_exi_buffer_append(&grammars->text, bytes);
    *text = (sxml_exi_text){.offset = offset, .byte_count = (uint32_t)bytes.byte_count};
    return status;
}

/* ---------------------------------------------------------------- Names */

/* The URI and local-name texts of a name of the schema's pool. */
static sxml_status sxml_exi_name_texts(const sxml_exi_builder *builder, sxml_name_identifier name,
                                       sxml_exi_name_entry *entry) {
    sxml_name_parts parts = {0};
    sxml_status status = sxml_name_pool_get_name_parts(builder->name_pool, name, &parts);
    *entry = (sxml_exi_name_entry){.local_name = parts.local_name};
    if (status == SXML_STATUS_OK && parts.namespace_uri.index != 0) {
        status = sxml_name_pool_get_namespace_uri(builder->name_pool, parts.namespace_uri, &entry->uri);
    }
    return status;
}

static sxml_status sxml_exi_namespace_text(const sxml_exi_builder *builder, uint32_t namespace_uri,
                                           sxml_byte_view *text) {
    *text = (sxml_byte_view){0};
    if (namespace_uri == 0) {
        return SXML_STATUS_OK;
    }
    return sxml_name_pool_get_namespace_uri(builder->name_pool, (sxml_namespace_uri_identifier){.index = namespace_uri},
                                            text);
}

static sxml_status sxml_exi_add_name(sxml_exi_builder *builder, sxml_exi_name_entry entry) {
    void *elements = builder->names;
    sxml_status status = sxml_exi_grow_array(builder->allocator, &(sxml_exi_growth){.elements = &elements,
                                                                                    .capacity = &builder->name_capacity,
                                                                                    .element_count = builder->name_count,
                                                                                    .element_size = sizeof(sxml_exi_name_entry)});
    if (status == SXML_STATUS_OK) {
        builder->names = elements;
        builder->names[builder->name_count++] = entry;
    }
    return status;
}

static sxml_status sxml_exi_add_component_name(sxml_exi_builder *builder, sxml_name_identifier name) {
    sxml_exi_name_entry entry = {0};
    sxml_status status = sxml_exi_name_texts(builder, name, &entry);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_add_name(builder, entry);
    }
    return status;
}

/* A URI alone: a partition with no local-name of its own (an empty
   local-name marks it, and is not added). */
static sxml_status sxml_exi_add_namespace_entry(sxml_exi_builder *builder, uint32_t namespace_uri) {
    sxml_exi_name_entry entry = {0};
    sxml_status status = sxml_exi_namespace_text(builder, namespace_uri, &entry.uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_add_name(builder, entry);
    }
    return status;
}

/* The built-in type a type definition of the components is, by name;
   SXML_EXI_NONE for a schema's own. */
static uint32_t sxml_exi_builtin_of(const sxml_exi_builder *builder, uint32_t type) {
    const sxml_schema_type_definition *definition =
        sxml_schema_get_type_definition(builder->components, (sxml_schema_type_definition_index){.value = type});
    if (!definition->is_builtin) {
        return SXML_EXI_NONE;
    }
    sxml_exi_name_entry entry = {0};
    if (definition->type_name.index == 0 ||
        sxml_exi_name_texts(builder, definition->type_name, &entry) != SXML_STATUS_OK) {
        return SXML_EXI_BUILTIN_ANY_SIMPLE_TYPE;
    }
    for (uint32_t builtin_index = 0; builtin_index < SXML_EXI_BUILTIN_COUNT; builtin_index++) {
        if (sxml_exi_views_equal(sxml_exi_builtin_types[builtin_index].name, entry.local_name)) {
            return builtin_index;
        }
    }
    for (size_t alias_index = 0; alias_index < sizeof sxml_exi_builtin_aliases / sizeof sxml_exi_builtin_aliases[0];
         alias_index++) {
        if (sxml_exi_views_equal(sxml_exi_builtin_aliases[alias_index].name, entry.local_name)) {
            return sxml_exi_builtin_aliases[alias_index].builtin;
        }
    }
    return SXML_EXI_BUILTIN_ANY_SIMPLE_TYPE;
}

/* A type definition of the schema's model: a built-in one by its table
   index, else the components'. */
static sxml_exi_type_reference sxml_exi_type_of(const sxml_exi_builder *builder, uint32_t type) {
    if (type == 0) {
        return (sxml_exi_type_reference){.is_builtin = true, .index = SXML_EXI_BUILTIN_ANY_TYPE};
    }
    if (type < builder->type_count && builder->type_builtins[type] != SXML_EXI_NONE) {
        return (sxml_exi_type_reference){.is_builtin = true, .index = builder->type_builtins[type]};
    }
    return (sxml_exi_type_reference){.is_builtin = false, .index = type};
}

/* A work list of type definitions (complex types, for the names of 7.3.1),
   each visited once. */
typedef struct sxml_exi_type_list {
    uint32_t *types;
    size_t type_count;
    size_t type_capacity;
    bool *is_listed;
} sxml_exi_type_list;

typedef struct sxml_exi_type_push {
    sxml_exi_type_list *list;
    uint32_t type;
} sxml_exi_type_push;

static sxml_status sxml_exi_push_type(sxml_exi_builder *builder, const sxml_exi_type_push *push) {
    sxml_exi_type_list *list = push->list;
    if (push->type == 0 || push->type >= builder->type_count || list->is_listed == NULL || list->is_listed[push->type]) {
        return SXML_STATUS_OK;
    }
    void *elements = list->types;
    sxml_status status = sxml_exi_grow_array(builder->allocator, &(sxml_exi_growth){.elements = &elements,
                                                                                    .capacity = &list->type_capacity,
                                                                                    .element_count = list->type_count,
                                                                                    .element_size = sizeof(uint32_t)});
    if (status == SXML_STATUS_OK) {
        list->types = elements;
        list->types[list->type_count++] = push->type;
        list->is_listed[push->type] = true;
    }
    return status;
}

/* An element declaration's names (7.3.1): its own, its type's, and its
   substitution group's members and their types. */
static sxml_status sxml_exi_collect_element(sxml_exi_builder *builder, const sxml_exi_type_push *push) {
    const sxml_schema_components *components = builder->components;
    const sxml_schema_element_declaration *declaration =
        sxml_schema_get_element_declaration(components, (sxml_schema_element_declaration_index){.value = push->type});
    sxml_status status = sxml_exi_add_component_name(builder, declaration->element_name);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_push_type(builder, &(sxml_exi_type_push){.list = push->list,
                                                                   .type = declaration->type_definition.value});
    }
    const sxml_schema_index_view members = sxml_schema_get_list(components, declaration->substitution_group_members);
    for (size_t member_index = 0; member_index < members.index_count && status == SXML_STATUS_OK; member_index++) {
        const sxml_schema_element_declaration *member = sxml_schema_get_element_declaration(
            components, (sxml_schema_element_declaration_index){.value = members.indexes[member_index]});
        status = sxml_exi_add_component_name(builder, member->element_name);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_push_type(builder,
                                        &(sxml_exi_type_push){.list = push->list, .type = member->type_definition.value});
        }
    }
    return status;
}

/* The URIs of a wildcard with a list of namespaces (8.5.4.1.7). */
static sxml_status sxml_exi_collect_wildcard(sxml_exi_builder *builder, uint32_t wildcard) {
    const sxml_schema_wildcard *definition =
        sxml_schema_get_wildcard(builder->components, (sxml_schema_wildcard_index){.value = wildcard});
    if (wildcard == 0 || definition->namespace_constraint_variety != SXML_SCHEMA_NAMESPACE_CONSTRAINT_ENUMERATION) {
        return SXML_STATUS_OK;
    }
    const sxml_schema_index_view namespaces = sxml_schema_get_list(builder->components, definition->namespaces);
    sxml_status status = SXML_STATUS_OK;
    for (size_t namespace_index = 0; namespace_index < namespaces.index_count && status == SXML_STATUS_OK;
         namespace_index++) {
        status = sxml_exi_add_namespace_entry(builder, namespaces.indexes[namespace_index]);
    }
    return status;
}

/* A complex type's names: its attribute uses, its attribute wildcard's
   URIs, and every term of its content (element declarations, wildcards),
   walked with an explicit stack of particles. */
static sxml_status sxml_exi_collect_complex_type(sxml_exi_builder *builder, const sxml_exi_type_push *push) {
    const sxml_schema_components *components = builder->components;
    const sxml_schema_type_definition *definition =
        sxml_schema_get_type_definition(components, (sxml_schema_type_definition_index){.value = push->type});
    sxml_status status = SXML_STATUS_OK;
    const sxml_schema_index_view uses = sxml_schema_get_list(components, definition->attribute_uses);
    for (size_t use_index = 0; use_index < uses.index_count && status == SXML_STATUS_OK; use_index++) {
        const sxml_schema_attribute_use *use =
            sxml_schema_get_attribute_use(components, (sxml_schema_attribute_use_index){.value = uses.indexes[use_index]});
        if (use->use_kind == SXML_SCHEMA_ATTRIBUTE_USE_PROHIBITED) {
            continue;
        }
        const sxml_schema_attribute_declaration *attribute =
            sxml_schema_get_attribute_declaration(components, use->attribute_declaration);
        status = sxml_exi_add_component_name(builder, attribute->attribute_name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_collect_wildcard(builder, definition->attribute_wildcard.value);
    }
    if (definition->content_type_variety == SXML_CONTENT_TYPE_VARIETY_SIMPLE && status == SXML_STATUS_OK) {
        status = sxml_exi_push_type(builder,
                                    &(sxml_exi_type_push){.list = push->list, .type = definition->simple_content_type.value});
    }
    if (definition->particle.value == 0 || status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t *stack = NULL;
    size_t stack_count = 0;
    size_t stack_capacity = 0;
    void *elements = stack;
    status = sxml_exi_grow_array(builder->allocator, &(sxml_exi_growth){.elements = &elements,
                                                                        .capacity = &stack_capacity,
                                                                        .element_count = stack_count,
                                                                        .element_size = sizeof(uint32_t)});
    stack = elements;
    if (status == SXML_STATUS_OK) {
        stack[stack_count++] = definition->particle.value;
    }
    while (status == SXML_STATUS_OK && stack_count > 0) {
        const sxml_schema_particle *particle =
            sxml_schema_get_particle(components, (sxml_schema_particle_index){.value = stack[--stack_count]});
        switch (particle->term_kind) {
        case SXML_SCHEMA_TERM_ELEMENT_DECLARATION:
            status = sxml_exi_collect_element(builder, &(sxml_exi_type_push){.list = push->list, .type = particle->term});
            break;
        case SXML_SCHEMA_TERM_WILDCARD:
            status = sxml_exi_collect_wildcard(builder, particle->term);
            break;
        case SXML_SCHEMA_TERM_MODEL_GROUP: {
            const sxml_schema_model_group *group =
                sxml_schema_get_model_group(components, (sxml_schema_model_group_index){.value = particle->term});
            const sxml_schema_index_view children = sxml_schema_get_list(components, group->particles);
            for (size_t child_index = 0; child_index < children.index_count && status == SXML_STATUS_OK; child_index++) {
                elements = stack;
                status = sxml_exi_grow_array(builder->allocator, &(sxml_exi_growth){.elements = &elements,
                                                                                    .capacity = &stack_capacity,
                                                                                    .element_count = stack_count,
                                                                                    .element_size = sizeof(uint32_t)});
                stack = elements;
                if (status == SXML_STATUS_OK) {
                    stack[stack_count++] = children.indexes[child_index];
                }
            }
            break;
        }
        case SXML_SCHEMA_TERM_NONE:
            break;
        }
    }
    sxml_allocator_release(builder->allocator, stack);
    return status;
}

/* Every name a schema puts in the string table (7.3.1, D.3), as the
   oracle collects them: the target namespaces, the named types, the global
   elements and attributes, and, reached from them, every attribute and
   element declaration (with substitution groups) and every URI of a
   wildcard. */
static sxml_status sxml_exi_collect_names(sxml_exi_builder *builder) {
    const sxml_schema_components *components = builder->components;
    sxml_status status = SXML_STATUS_OK;
    const uint32_t document_count = components->tables[SXML_SCHEMA_TABLE_DOCUMENT].entry_count;
    for (uint32_t document_index = 1; document_index < document_count && status == SXML_STATUS_OK; document_index++) {
        const sxml_schema_document_entry *document =
            sxml_schema_get_document(components, (sxml_schema_document_index){.value = document_index});
        status = sxml_exi_add_namespace_entry(builder, document->target_namespace.index);
    }
    sxml_exi_type_list list = {0};
    if (status == SXML_STATUS_OK && builder->type_count > 0) {
        void *memory = NULL;
        status = sxml_allocator_allocate_array(
            builder->allocator,
            (sxml_array_allocation_request){.element_count = builder->type_count, .element_size = sizeof(bool), .element_alignment = 1},
            &memory);
        list.is_listed = memory;
    }
    for (uint32_t type_index = 1; type_index < builder->type_count && status == SXML_STATUS_OK; type_index++) {
        const sxml_schema_type_definition *definition =
            sxml_schema_get_type_definition(components, (sxml_schema_type_definition_index){.value = type_index});
        if (definition->type_name.index != 0 && !definition->is_builtin) {
            status = sxml_exi_add_component_name(builder, definition->type_name);
            if (status == SXML_STATUS_OK) {
                status = sxml_exi_push_type(builder, &(sxml_exi_type_push){.list = &list, .type = type_index});
            }
        }
    }
    const uint32_t element_count = components->tables[SXML_SCHEMA_TABLE_ELEMENT_DECLARATION].entry_count;
    for (uint32_t element_index = 1; element_index < element_count && status == SXML_STATUS_OK; element_index++) {
        const sxml_schema_element_declaration *declaration =
            sxml_schema_get_element_declaration(components, (sxml_schema_element_declaration_index){.value = element_index});
        if (declaration->scope_variety == SXML_SCHEMA_SCOPE_GLOBAL) {
            status = sxml_exi_collect_element(builder, &(sxml_exi_type_push){.list = &list, .type = element_index});
        }
    }
    const uint32_t attribute_count = components->tables[SXML_SCHEMA_TABLE_ATTRIBUTE_DECLARATION].entry_count;
    /* Attribute declarations 1 to 4 are the xsi: attributes, which the
       oracle's model does not hold. */
    for (uint32_t attribute_index = 5; attribute_index < attribute_count && status == SXML_STATUS_OK; attribute_index++) {
        const sxml_schema_attribute_declaration *declaration = sxml_schema_get_attribute_declaration(
            components, (sxml_schema_attribute_declaration_index){.value = attribute_index});
        if (declaration->scope_variety == SXML_SCHEMA_SCOPE_GLOBAL) {
            status = sxml_exi_add_component_name(builder, declaration->attribute_name);
            if (status == SXML_STATUS_OK) {
                status = sxml_exi_push_type(builder,
                                            &(sxml_exi_type_push){.list = &list, .type = declaration->type_definition.value});
            }
        }
    }
    /* The types reached: named ones give their names, complex ones their
       content's. */
    for (size_t list_index = 0; list_index < list.type_count && status == SXML_STATUS_OK; list_index++) {
        const uint32_t type = list.types[list_index];
        const sxml_schema_type_definition *definition =
            sxml_schema_get_type_definition(components, (sxml_schema_type_definition_index){.value = type});
        if (definition->is_builtin) {
            continue;
        }
        if (definition->type_name.index != 0) {
            status = sxml_exi_add_component_name(builder, definition->type_name);
        }
        if (status == SXML_STATUS_OK && definition->variety == SXML_TYPE_VARIETY_COMPLEX) {
            status = sxml_exi_collect_complex_type(builder, &(sxml_exi_type_push){.list = &list, .type = type});
        }
    }
    sxml_allocator_release(builder->allocator, list.types);
    sxml_allocator_release(builder->allocator, list.is_listed);
    return status;
}

static int sxml_exi_compare_name_entries(const void *first, const void *second) {
    const sxml_exi_name_entry *left = first;
    const sxml_exi_name_entry *right = second;
    const int order = sxml_exi_view_order(left->uri, right->uri);
    return order != 0 ? order : sxml_exi_view_order(left->local_name, right->local_name);
}

/* Initializes the string table with the collected names, one partition
   per URI. */
static sxml_status sxml_exi_initialize_table(sxml_exi_builder *builder) {
    if (builder->name_count > 1) {
        qsort(builder->names, builder->name_count, sizeof builder->names[0], sxml_exi_compare_name_entries);
    }
    size_t partition_count = 0;
    for (size_t name_index = 0; name_index < builder->name_count; name_index++) {
        if (name_index == 0 || !sxml_exi_views_equal(builder->names[name_index].uri, builder->names[name_index - 1].uri)) {
            partition_count++;
        }
    }
    void *memory = NULL;
    sxml_status status = SXML_STATUS_OK;
    size_t local_name_total = builder->name_count;
    status = sxml_allocator_allocate_array(builder->allocator,
                                           (sxml_array_allocation_request){.element_count = partition_count,
                                                                           .element_size = sizeof(sxml_exi_schema_partition),
                                                                           .element_alignment = alignof(sxml_exi_schema_partition)},
                                           &memory);
    sxml_exi_schema_partition *partitions = memory;
    void *name_memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(builder->allocator,
                                               (sxml_array_allocation_request){.element_count = local_name_total,
                                                                               .element_size = sizeof(sxml_byte_view),
                                                                               .element_alignment = alignof(sxml_byte_view)},
                                               &name_memory);
    }
    sxml_byte_view *local_names = name_memory;
    if (status == SXML_STATUS_OK) {
        size_t filled = 0;
        size_t partition_index = 0;
        for (size_t name_index = 0; name_index < builder->name_count; name_index++) {
            const sxml_exi_name_entry *entry = &builder->names[name_index];
            if (name_index == 0 || !sxml_exi_views_equal(entry->uri, builder->names[name_index - 1].uri)) {
                partitions[partition_index++] =
                    (sxml_exi_schema_partition){.uri = entry->uri, .local_names = local_names + filled, .local_name_count = 0};
            }
            if (entry->local_name.byte_count > 0) {
                local_names[filled++] = entry->local_name;
                partitions[partition_index - 1].local_name_count++;
            }
        }
        status = sxml_exi_string_table_initialize(
            builder->table,
            &(sxml_exi_string_table_configuration){
                .allocator = builder->allocator,
                .options = builder->build->options,
                .is_schema_informed = true,
                .schema_partitions = {.partitions = partitions, .partition_count = partition_count}});
    }
    sxml_allocator_release(builder->allocator, partitions);
    sxml_allocator_release(builder->allocator, local_names);
    return status;
}

/* The table's indexes of a qname that must be there (the schema's). */
typedef struct sxml_exi_table_name {
    uint32_t uri;
    uint32_t local_name;
} sxml_exi_table_name;

static sxml_status sxml_exi_find_table_name(sxml_exi_builder *builder, sxml_exi_name_entry entry,
                                            sxml_exi_table_name *name) {
    sxml_exi_lookup uri = {0};
    sxml_exi_lookup local_name = {0};
    sxml_status status = sxml_exi_find_uri(builder->table, &(sxml_exi_string_key){.text = entry.uri}, &uri);
    if (status == SXML_STATUS_OK && uri.is_found) {
        status = sxml_exi_find_local_name(builder->table,
                                          &(sxml_exi_string_key){.text = entry.local_name, .uri = uri.index}, &local_name);
    }
    if (status == SXML_STATUS_OK && (!uri.is_found || !local_name.is_found)) {
        return SXML_EXI_BUILD_FAIL(builder, SXML_STATUS_INVALID_ARGUMENT, "a schema name missing from the string table");
    }
    *name = (sxml_exi_table_name){.uri = uri.index, .local_name = local_name.index};
    return status;
}

static sxml_status sxml_exi_find_component_name(sxml_exi_builder *builder, sxml_name_identifier identifier,
                                                sxml_exi_table_name *name) {
    sxml_exi_name_entry entry = {0};
    sxml_status status = sxml_exi_name_texts(builder, identifier, &entry);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_find_table_name(builder, entry, name);
    }
    return status;
}

static sxml_status sxml_exi_find_uri_index(sxml_exi_builder *builder, sxml_byte_view text, uint32_t *uri) {
    sxml_exi_lookup found = {0};
    sxml_status status = sxml_exi_find_uri(builder->table, &(sxml_exi_string_key){.text = text}, &found);
    if (status == SXML_STATUS_OK && !found.is_found) {
        return SXML_EXI_BUILD_FAIL(builder, SXML_STATUS_INVALID_ARGUMENT, "a schema URI missing from the string table");
    }
    *uri = found.index;
    return status;
}

/* ------------------------------------------------------------ Datatypes */

static sxml_status sxml_exi_add_datatype(sxml_exi_builder *builder, const sxml_exi_datatype *datatype,
                                         uint32_t *index) {
    sxml_exi_schema_grammars *grammars = builder->grammars;
    void *elements = grammars->datatypes;
    sxml_status status = sxml_exi_grow_array(grammars->allocator, &(sxml_exi_growth){.elements = &elements,
                                                                                     .capacity = &grammars->datatype_capacity,
                                                                                     .element_count = grammars->datatype_count,
                                                                                     .element_size = sizeof(sxml_exi_datatype)});
    if (status == SXML_STATUS_OK) {
        grammars->datatypes = elements;
        *index = (uint32_t)grammars->datatype_count;
        grammars->datatypes[grammars->datatype_count++] = *datatype;
    }
    return status;
}

/* What a simple type's chain of restrictions gives its datatype, the
   facets the oracle reads as "defined" (own or inherited, the most derived
   one winning): bounds, whiteSpace, patterns, enumerations. */
typedef struct sxml_exi_type_facts {
    /* The nearest built-in type at or above the type. */
    uint32_t builtin;
    sxml_type_variety variety;
    sxml_exi_integer_bounds bounds;
    bool has_whitespace;
    sxml_exi_whitespace whitespace;
    bool has_pattern;
    /* The most derived step with patterns of its own: a schema's (0 for a
       built-in one). */
    uint32_t pattern_type;
    /* The most derived step with enumerations, and the type above the
       topmost such step (whose datatype the values are of). */
    uint32_t enumeration_type;
    sxml_exi_type_reference enumeration_base;
    /* A list type's item type. */
    bool has_item;
    sxml_exi_type_reference item;
} sxml_exi_type_facts;

static sxml_exi_whitespace sxml_exi_whitespace_of_text(sxml_byte_view text) {
    if (sxml_exi_views_equal(text, SXML_EXI_SCHEMA_TEXT("replace"))) {
        return SXML_EXI_WHITESPACE_REPLACE;
    }
    if (sxml_exi_views_equal(text, SXML_EXI_SCHEMA_TEXT("collapse"))) {
        return SXML_EXI_WHITESPACE_COLLAPSE;
    }
    return SXML_EXI_WHITESPACE_PRESERVE;
}

static bool sxml_exi_is_xml_space(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/* A facet value with XML whitespace trimmed. */
static sxml_byte_view sxml_exi_trimmed(sxml_byte_view text) {
    while (text.byte_count > 0 && sxml_exi_is_xml_space(text.bytes[0])) {
        text.bytes++;
        text.byte_count--;
    }
    while (text.byte_count > 0 && sxml_exi_is_xml_space(text.bytes[text.byte_count - 1])) {
        text.byte_count--;
    }
    return text;
}

/* Reads one restriction step's facets into facts (the first found of a
   kind is the most derived). */
typedef struct sxml_exi_step {
    sxml_exi_type_facts *facts;
    uint32_t type;
} sxml_exi_step;

/* Keeps the most derived value of a bound: the first one read. */
static void sxml_exi_keep_bound(sxml_byte_view *bound, sxml_byte_view value) {
    if (bound->bytes == NULL) {
        *bound = value.bytes == NULL ? SXML_EXI_SCHEMA_TEXT("0") : value;
    }
}

static void sxml_exi_read_step(const sxml_exi_builder *builder, const sxml_exi_step *step) {
    const sxml_schema_components *components = builder->components;
    sxml_exi_type_facts *facts = step->facts;
    const sxml_schema_type_definition *definition =
        sxml_schema_get_type_definition(components, (sxml_schema_type_definition_index){.value = step->type});
    const sxml_schema_index_view facet_list = sxml_schema_get_list(components, definition->facets);
    bool has_own_pattern = false;
    bool has_own_enumeration = false;
    for (size_t facet_index = 0; facet_index < facet_list.index_count; facet_index++) {
        const sxml_schema_facet *facet =
            sxml_schema_get_facet(components, (sxml_schema_facet_index){.value = facet_list.indexes[facet_index]});
        const sxml_byte_view value = sxml_exi_trimmed(sxml_schema_get_text(components, facet->value));
        switch (facet->facet_kind) {
        case SXML_FACET_KIND_MIN_INCLUSIVE:
            sxml_exi_keep_bound(&facts->bounds.minimum_inclusive, value);
            break;
        case SXML_FACET_KIND_MIN_EXCLUSIVE:
            sxml_exi_keep_bound(&facts->bounds.minimum_exclusive, value);
            break;
        case SXML_FACET_KIND_MAX_INCLUSIVE:
            sxml_exi_keep_bound(&facts->bounds.maximum_inclusive, value);
            break;
        case SXML_FACET_KIND_MAX_EXCLUSIVE:
            sxml_exi_keep_bound(&facts->bounds.maximum_exclusive, value);
            break;
        case SXML_FACET_KIND_WHITESPACE:
            if (!facts->has_whitespace) {
                facts->has_whitespace = true;
                facts->whitespace = sxml_exi_whitespace_of_text(value);
            }
            break;
        case SXML_FACET_KIND_PATTERN:
            has_own_pattern = true;
            break;
        case SXML_FACET_KIND_ENUMERATION:
            has_own_enumeration = true;
            break;
        case SXML_FACET_KIND_LENGTH:
        case SXML_FACET_KIND_MIN_LENGTH:
        case SXML_FACET_KIND_MAX_LENGTH:
        case SXML_FACET_KIND_TOTAL_DIGITS:
        case SXML_FACET_KIND_FRACTION_DIGITS:
        case SXML_FACET_KIND_EXPLICIT_TIMEZONE:
            break;
        }
    }
    if (has_own_pattern && !facts->has_pattern) {
        facts->has_pattern = true;
        facts->pattern_type = step->type;
    }
    if (has_own_enumeration) {
        if (facts->enumeration_type == 0) {
            facts->enumeration_type = step->type;
        }
        facts->enumeration_base = sxml_exi_type_of(builder, definition->base_type.value);
    }
    if (!facts->has_item && definition->variety == SXML_TYPE_VARIETY_LIST && definition->item_type.value != 0) {
        facts->has_item = true;
        facts->item = sxml_exi_type_of(builder, definition->item_type.value);
    }
}

/* The facts of a simple type: its restriction steps up to the nearest
   built-in type, then that type's own facets. */
static void sxml_exi_gather_facts(const sxml_exi_builder *builder, sxml_exi_type_reference type,
                                  sxml_exi_type_facts *facts) {
    *facts = (sxml_exi_type_facts){.variety = SXML_TYPE_VARIETY_ATOMIC};
    if (!type.is_builtin) {
        const sxml_schema_type_definition *definition =
            sxml_schema_get_type_definition(builder->components, (sxml_schema_type_definition_index){.value = type.index});
        facts->variety = definition->variety;
    }
    sxml_exi_type_reference step = type;
    /* A chain longer than the table cannot be a derivation (a cycle). */
    for (size_t step_count = 0; !step.is_builtin && step_count <= builder->type_count; step_count++) {
        sxml_exi_read_step(builder, &(sxml_exi_step){.facts = facts, .type = step.index});
        const sxml_schema_type_definition *definition =
            sxml_schema_get_type_definition(builder->components, (sxml_schema_type_definition_index){.value = step.index});
        step = sxml_exi_type_of(builder, definition->base_type.value);
    }
    const uint32_t builtin = step.is_builtin ? step.index : SXML_EXI_BUILTIN_ANY_SIMPLE_TYPE;
    facts->builtin = builtin;
    const sxml_exi_builtin_type *entry = &sxml_exi_builtin_types[builtin];
    if (type.is_builtin) {
        facts->variety = entry->primitive == SXML_EXI_PRIMITIVE_LIST     ? SXML_TYPE_VARIETY_LIST
                         : builtin == SXML_EXI_BUILTIN_ANY_SIMPLE_TYPE ? SXML_TYPE_VARIETY_ANY_SIMPLE
                                                                         : SXML_TYPE_VARIETY_ATOMIC;
    }
    if (facts->bounds.minimum_inclusive.bytes == NULL && facts->bounds.minimum_exclusive.bytes == NULL) {
        facts->bounds.minimum_inclusive = entry->minimum;
    }
    if (facts->bounds.maximum_inclusive.bytes == NULL && facts->bounds.maximum_exclusive.bytes == NULL) {
        facts->bounds.maximum_inclusive = entry->maximum;
    }
    if (!facts->has_whitespace) {
        facts->whitespace = entry->whitespace;
    }
    if (!facts->has_pattern && entry->has_pattern) {
        facts->has_pattern = true;
        facts->pattern_type = 0;
    }
    if (!facts->has_item && entry->primitive == SXML_EXI_PRIMITIVE_LIST) {
        facts->has_item = true;
        facts->item = (sxml_exi_type_reference){.is_builtin = true, .index = entry->item};
    }
}

static uint32_t *sxml_exi_datatype_slot(sxml_exi_builder *builder, sxml_exi_type_reference type) {
    return type.is_builtin ? &builder->builtin_datatypes[type.index] : &builder->type_datatypes[type.index];
}

/* ------------------------------------------- Restricted character sets */

/* A range of code points to add to a set being made. */
typedef struct sxml_exi_character_range {
    uint32_t first;
    uint32_t last;
} sxml_exi_character_range;

typedef struct sxml_exi_character_collection {
    uint32_t *characters;
    size_t character_count;
    size_t character_capacity;
    bool is_restricted;
} sxml_exi_character_collection;

typedef struct sxml_exi_character_add {
    sxml_exi_character_collection *collection;
    sxml_exi_character_range range;
} sxml_exi_character_add;

/* Adds a range to the set (ascending, unique). The oracle: a range wider
   than 255, a character outside the BMP or a set past 255 is no
   restricted set (7.1.10.1 allows fewer than 256). */
static sxml_status sxml_exi_collect_characters(sxml_exi_builder *builder, const sxml_exi_character_add *add) {
    sxml_exi_character_collection *collection = add->collection;
    const sxml_exi_character_range range = add->range;
    if (!collection->is_restricted) {
        return SXML_STATUS_OK;
    }
    if (range.last < range.first || range.last - range.first > 255 || range.last > 0xFFFF) {
        collection->is_restricted = false;
        return SXML_STATUS_OK;
    }
    for (uint32_t codepoint = range.first; codepoint <= range.last; codepoint++) {
        size_t position = 0;
        while (position < collection->character_count && collection->characters[position] < codepoint) {
            position++;
        }
        if (position < collection->character_count && collection->characters[position] == codepoint) {
            continue;
        }
        if (collection->character_count >= 255) {
            collection->is_restricted = false;
            return SXML_STATUS_OK;
        }
        void *elements = collection->characters;
        sxml_status status =
            sxml_exi_grow_array(builder->allocator, &(sxml_exi_growth){.elements = &elements,
                                                                       .capacity = &collection->character_capacity,
                                                                       .element_count = collection->character_count,
                                                                       .element_size = sizeof(uint32_t)});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        collection->characters = elements;
        memmove(collection->characters + position + 1, collection->characters + position,
                (collection->character_count - position) * sizeof(uint32_t));
        collection->characters[position] = codepoint;
        collection->character_count++;
    }
    return SXML_STATUS_OK;
}

/* One character of a pattern at *offset: an escape read as its
   character; \s, or another class (a category or a multi-character
   escape). */
typedef struct sxml_exi_pattern_character {
    uint32_t codepoint;
    bool is_space_class;
    bool is_other_class;
} sxml_exi_pattern_character;

static sxml_exi_pattern_character sxml_exi_read_pattern_character(sxml_byte_view pattern, size_t *offset) {
    uint32_t codepoint = sxml_exi_utf8_next(pattern, offset);
    if (codepoint != '\\' || *offset >= pattern.byte_count) {
        return (sxml_exi_pattern_character){.codepoint = codepoint};
    }
    codepoint = sxml_exi_utf8_next(pattern, offset);
    switch (codepoint) {
    case 'n':
        return (sxml_exi_pattern_character){.codepoint = '\n'};
    case 'r':
        return (sxml_exi_pattern_character){.codepoint = '\r'};
    case 't':
        return (sxml_exi_pattern_character){.codepoint = '\t'};
    case 's':
        return (sxml_exi_pattern_character){.is_space_class = true};
    case 'S':
    case 'i':
    case 'I':
    case 'c':
    case 'C':
    case 'd':
    case 'D':
    case 'w':
    case 'W':
    case 'p':
    case 'P':
        return (sxml_exi_pattern_character){.is_other_class = true};
    default:
        return (sxml_exi_pattern_character){.codepoint = codepoint};
    }
}

/* Adds a pattern character (\s: its four characters) to the set. */
typedef struct sxml_exi_pattern_add {
    sxml_exi_character_collection *collection;
    sxml_exi_pattern_character character;
} sxml_exi_pattern_add;

static sxml_status sxml_exi_collect_pattern_character(sxml_exi_builder *builder, const sxml_exi_pattern_add *add) {
    static const uint32_t spaces[] = {0x9, 0xA, 0xD, 0x20};
    if (add->character.is_other_class) {
        add->collection->is_restricted = false;
        return SXML_STATUS_OK;
    }
    if (!add->character.is_space_class) {
        return sxml_exi_collect_characters(
            builder, &(sxml_exi_character_add){.collection = add->collection,
                                               .range = {.first = add->character.codepoint, .last = add->character.codepoint}});
    }
    sxml_status status = SXML_STATUS_OK;
    for (size_t space_index = 0; space_index < 4 && status == SXML_STATUS_OK; space_index++) {
        status = sxml_exi_collect_characters(
            builder, &(sxml_exi_character_add){.collection = add->collection,
                                               .range = {.first = spaces[space_index], .last = spaces[space_index]}});
    }
    return status;
}

/* A character class [...] from *offset (after its '['): its ranges into
   the set; a complement makes it unrestricted, a subtraction ends it (its
   characters are a subset of what is read). */
typedef struct sxml_exi_class_read {
    sxml_byte_view pattern;
    size_t *offset;
} sxml_exi_class_read;

static sxml_status sxml_exi_collect_class(sxml_exi_builder *builder, const sxml_exi_class_read *read,
                                          sxml_exi_character_collection *collection) {
    const sxml_byte_view pattern = read->pattern;
    size_t offset = *read->offset;
    sxml_status status = SXML_STATUS_OK;
    if (offset < pattern.byte_count && pattern.bytes[offset] == '^') {
        collection->is_restricted = false;
        return SXML_STATUS_OK;
    }
    bool is_first = true;
    while (offset < pattern.byte_count && collection->is_restricted && status == SXML_STATUS_OK) {
        if (pattern.bytes[offset] == ']' && !is_first) {
            offset++;
            break;
        }
        if (pattern.bytes[offset] == '-' && offset + 1 < pattern.byte_count && pattern.bytes[offset + 1] == '[') {
            uint32_t depth = 0;
            while (offset < pattern.byte_count) {
                if (pattern.bytes[offset] == '[') {
                    depth++;
                } else if (pattern.bytes[offset] == ']') {
                    if (depth == 0) {
                        break;
                    }
                    depth--;
                }
                offset++;
            }
            offset++;
            break;
        }
        is_first = false;
        const sxml_exi_pattern_character first = sxml_exi_read_pattern_character(pattern, &offset);
        if (first.is_other_class || first.is_space_class) {
            status = sxml_exi_collect_pattern_character(
                builder, &(sxml_exi_pattern_add){.collection = collection, .character = first});
            continue;
        }
        uint32_t last = first.codepoint;
        if (offset + 1 < pattern.byte_count && pattern.bytes[offset] == '-' && pattern.bytes[offset + 1] != ']' &&
            pattern.bytes[offset + 1] != '[') {
            offset++;
            const sxml_exi_pattern_character end = sxml_exi_read_pattern_character(pattern, &offset);
            if (end.is_other_class || end.is_space_class) {
                collection->is_restricted = false;
                break;
            }
            last = end.codepoint;
        }
        status = sxml_exi_collect_characters(
            builder, &(sxml_exi_character_add){.collection = collection, .range = {.first = first.codepoint, .last = last}});
    }
    *read->offset = offset;
    return status;
}

/* The set of characters of a pattern (Appendix E): the union of its
   atoms' characters, whatever the branches, groups and quantifiers; a
   wildcard, a category or multi-character escape other than \s, a
   complement, or more than 255 characters make it unrestricted. */
typedef struct sxml_exi_pattern_request {
    sxml_byte_view pattern;
    sxml_exi_character_collection *collection;
} sxml_exi_pattern_request;

static sxml_status sxml_exi_pattern_characters(sxml_exi_builder *builder, const sxml_exi_pattern_request *request) {
    const sxml_byte_view pattern = request->pattern;
    sxml_exi_character_collection *collection = request->collection;
    size_t offset = 0;
    sxml_status status = SXML_STATUS_OK;
    while (offset < pattern.byte_count && collection->is_restricted && status == SXML_STATUS_OK) {
        const uint8_t byte = pattern.bytes[offset];
        if (byte == '(' || byte == ')' || byte == '|' || byte == '?' || byte == '*' || byte == '+') {
            offset++;
        } else if (byte == '{') {
            while (offset < pattern.byte_count && pattern.bytes[offset] != '}') {
                offset++;
            }
            offset++;
        } else if (byte == '.') {
            collection->is_restricted = false;
        } else if (byte == '[') {
            offset++;
            status = sxml_exi_collect_class(builder, &(sxml_exi_class_read){.pattern = pattern, .offset = &offset},
                                            collection);
        } else {
            const sxml_exi_pattern_character character = sxml_exi_read_pattern_character(pattern, &offset);
            status = sxml_exi_collect_pattern_character(
                builder, &(sxml_exi_pattern_add){.collection = collection, .character = character});
        }
    }
    return status;
}

/* The restricted character set of a string type whose most derived
   patterns are pattern_type's (7.1.10.1); is_restricted false for none. */
typedef struct sxml_exi_restricted_set {
    bool is_restricted;
    size_t first_character;
    uint32_t character_count;
} sxml_exi_restricted_set;

static sxml_status sxml_exi_restricted_set_of(sxml_exi_builder *builder, uint32_t pattern_type,
                                              sxml_exi_restricted_set *set) {
    const sxml_schema_components *components = builder->components;
    const sxml_schema_type_definition *definition =
        sxml_schema_get_type_definition(components, (sxml_schema_type_definition_index){.value = pattern_type});
    const sxml_schema_index_view facet_list = sxml_schema_get_list(components, definition->facets);
    sxml_exi_character_collection collection = {.is_restricted = true};
    sxml_status status = SXML_STATUS_OK;
    for (size_t facet_index = 0; facet_index < facet_list.index_count && status == SXML_STATUS_OK; facet_index++) {
        const sxml_schema_facet *facet =
            sxml_schema_get_facet(components, (sxml_schema_facet_index){.value = facet_list.indexes[facet_index]});
        if (facet->facet_kind == SXML_FACET_KIND_PATTERN) {
            status = sxml_exi_pattern_characters(
                builder, &(sxml_exi_pattern_request){.pattern = sxml_schema_get_text(components, facet->value),
                                                     .collection = &collection});
        }
    }
    *set = (sxml_exi_restricted_set){.is_restricted = false};
    sxml_exi_schema_grammars *grammars = builder->grammars;
    if (status == SXML_STATUS_OK && collection.is_restricted && collection.character_count > 0) {
        set->is_restricted = true;
        set->first_character = grammars->character_count;
        set->character_count = (uint32_t)collection.character_count;
        for (size_t character_index = 0; character_index < collection.character_count && status == SXML_STATUS_OK;
             character_index++) {
            void *elements = grammars->characters;
            status = sxml_exi_grow_array(grammars->allocator, &(sxml_exi_growth){.elements = &elements,
                                                                                 .capacity = &grammars->character_capacity,
                                                                                 .element_count = grammars->character_count,
                                                                                 .element_size = sizeof(uint32_t)});
            if (status == SXML_STATUS_OK) {
                grammars->characters = elements;
                grammars->characters[grammars->character_count++] = collection.characters[character_index];
            }
        }
    }
    sxml_allocator_release(builder->allocator, collection.characters);
    return status;
}

/* The values of an enumeration (7.2), each the canonical text of its
   value in the base datatype (the values of string-like bases as
   written). */
typedef struct sxml_exi_enumeration_request {
    uint32_t enumeration_type;
    uint32_t base_datatype;
} sxml_exi_enumeration_request;

static sxml_status sxml_exi_add_enumeration_value(sxml_exi_builder *builder, sxml_byte_view text) {
    sxml_exi_schema_grammars *grammars = builder->grammars;
    sxml_exi_text stored = {0};
    sxml_status status = sxml_exi_store(grammars, text, &stored);
    void *elements = grammars->enumeration_values;
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_grow_array(grammars->allocator, &(sxml_exi_growth){.elements = &elements,
                                                                             .capacity = &grammars->enumeration_value_capacity,
                                                                             .element_count = grammars->enumeration_value_count,
                                                                             .element_size = sizeof(sxml_exi_text)});
    }
    if (status == SXML_STATUS_OK) {
        grammars->enumeration_values = elements;
        grammars->enumeration_values[grammars->enumeration_value_count++] = stored;
    }
    return status;
}

static sxml_status sxml_exi_add_enumeration(sxml_exi_builder *builder, const sxml_exi_enumeration_request *request,
                                            sxml_exi_datatype *datatype) {
    const sxml_schema_components *components = builder->components;
    sxml_exi_schema_grammars *grammars = builder->grammars;
    const sxml_schema_type_definition *definition =
        sxml_schema_get_type_definition(components, (sxml_schema_type_definition_index){.value = request->enumeration_type});
    const sxml_schema_index_view facet_list = sxml_schema_get_list(components, definition->facets);
    const sxml_exi_datatype_kind base_kind = grammars->datatypes[request->base_datatype].datatype_kind;
    const bool is_string = base_kind == SXML_EXI_DATATYPE_STRING || base_kind == SXML_EXI_DATATYPE_RESTRICTED_STRING ||
                           base_kind == SXML_EXI_DATATYPE_LIST || base_kind == SXML_EXI_DATATYPE_ENUMERATION;
    datatype->first_value = grammars->enumeration_value_count;
    datatype->value_count = 0;
    sxml_exi_buffer canonical = {.allocator = builder->allocator};
    sxml_status status = SXML_STATUS_OK;
    for (size_t facet_index = 0; facet_index < facet_list.index_count && status == SXML_STATUS_OK; facet_index++) {
        const sxml_schema_facet *facet =
            sxml_schema_get_facet(components, (sxml_schema_facet_index){.value = facet_list.indexes[facet_index]});
        if (facet->facet_kind != SXML_FACET_KIND_ENUMERATION) {
            continue;
        }
        const sxml_byte_view value = sxml_schema_get_text(components, facet->value);
        sxml_byte_view text = value;
        if (!is_string) {
            canonical.byte_count = 0;
            sxml_exi_canonical_output output = {.buffer = &canonical};
            status = sxml_exi_canonicalize_value(
                grammars, (sxml_exi_value_text){.datatype = request->base_datatype, .text = value}, &output);
            if (status == SXML_STATUS_OK && output.is_valid) {
                text = (sxml_byte_view){.bytes = canonical.bytes, .byte_count = canonical.byte_count};
            }
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_add_enumeration_value(builder, text);
        }
        if (status == SXML_STATUS_OK) {
            datatype->value_count++;
        }
    }
    sxml_exi_buffer_release(&canonical);
    datatype->bit_count = sxml_exi_bit_width(datatype->value_count);
    return status;
}

/* The datatype of an atomic type by its built-in ancestry and facts
   (Table 7-1; 7.1.2, 7.1.5, 7.1.10.1). */
static sxml_status sxml_exi_atomic_datatype(sxml_exi_builder *builder, const sxml_exi_type_facts *facts,
                                            sxml_exi_datatype *datatype) {
    const sxml_exi_builtin_type *builtin = &sxml_exi_builtin_types[facts->builtin];
    *datatype = (sxml_exi_datatype){.datatype_kind = SXML_EXI_DATATYPE_STRING,
                                    .whitespace = SXML_EXI_WHITESPACE_COLLAPSE,
                                    .base_datatype = SXML_EXI_NONE};
    switch (builtin->primitive) {
    case SXML_EXI_PRIMITIVE_BOOLEAN:
        datatype->datatype_kind = facts->has_pattern ? SXML_EXI_DATATYPE_PATTERN_BOOLEAN : SXML_EXI_DATATYPE_BOOLEAN;
        return SXML_STATUS_OK;
    case SXML_EXI_PRIMITIVE_FLOAT:
        datatype->datatype_kind = SXML_EXI_DATATYPE_FLOAT;
        return SXML_STATUS_OK;
    case SXML_EXI_PRIMITIVE_BASE64_BINARY:
        datatype->datatype_kind = SXML_EXI_DATATYPE_BASE64_BINARY;
        return SXML_STATUS_OK;
    case SXML_EXI_PRIMITIVE_HEX_BINARY:
        datatype->datatype_kind = SXML_EXI_DATATYPE_HEX_BINARY;
        return SXML_STATUS_OK;
    case SXML_EXI_PRIMITIVE_DATE_TIME:
        datatype->datatype_kind = SXML_EXI_DATATYPE_DATE_TIME;
        datatype->date_time_kind = builtin->date_time_kind;
        return SXML_STATUS_OK;
    case SXML_EXI_PRIMITIVE_DECIMAL: {
        if (!builtin->is_integer) {
            datatype->datatype_kind = SXML_EXI_DATATYPE_DECIMAL;
            return SXML_STATUS_OK;
        }
        sxml_exi_buffer lower_bound = {.allocator = builder->allocator};
        sxml_exi_integer_range range = {.lower_bound = &lower_bound};
        sxml_status status = sxml_exi_integer_range_of(&facts->bounds, &range);
        datatype->datatype_kind = range.datatype_kind;
        if (status == SXML_STATUS_OK && range.datatype_kind == SXML_EXI_DATATYPE_BOUNDED_INTEGER) {
            datatype->value_count = range.value_count;
            datatype->bit_count = sxml_exi_bit_width(range.value_count);
            status = sxml_exi_store(builder->grammars,
                                    (sxml_byte_view){.bytes = lower_bound.bytes, .byte_count = lower_bound.byte_count},
                                    &datatype->lower_bound);
        }
        sxml_exi_buffer_release(&lower_bound);
        return status;
    }
    case SXML_EXI_PRIMITIVE_STRING:
    case SXML_EXI_PRIMITIVE_QNAME:
    case SXML_EXI_PRIMITIVE_LIST:
    case SXML_EXI_PRIMITIVE_COMPLEX:
        break;
    }
    datatype->whitespace = facts->whitespace;
    if (facts->has_pattern && facts->pattern_type != 0) {
        sxml_exi_restricted_set set = {0};
        sxml_status status = sxml_exi_restricted_set_of(builder, facts->pattern_type, &set);
        if (status == SXML_STATUS_OK && set.is_restricted) {
            datatype->datatype_kind = SXML_EXI_DATATYPE_RESTRICTED_STRING;
            datatype->first_value = set.first_character;
            datatype->value_count = set.character_count;
        }
        return status;
    }
    return SXML_STATUS_OK;
}

/* The datatype of a type with its facts, once what it depends on (an
   enumeration's base, a list's item type) has one. */
typedef struct sxml_exi_datatype_making {
    const sxml_exi_type_facts *facts;
    sxml_exi_type_reference type;
    uint32_t dependency_datatype;
} sxml_exi_datatype_making;

static sxml_status sxml_exi_make_datatype(sxml_exi_builder *builder, const sxml_exi_datatype_making *making,
                                          sxml_exi_datatype *made) {
    const sxml_exi_type_facts *facts = making->facts;
    *made = (sxml_exi_datatype){.datatype_kind = SXML_EXI_DATATYPE_STRING,
                                .whitespace = SXML_EXI_WHITESPACE_COLLAPSE,
                                .base_datatype = SXML_EXI_NONE};
    if (facts->enumeration_type != 0) {
        /* 7.2: no enumeration for a union, a list, xs:QName, xs:NOTATION. */
        sxml_exi_type_facts base_facts = {0};
        sxml_exi_gather_facts(builder, facts->enumeration_base, &base_facts);
        const sxml_exi_datatype *base = &builder->grammars->datatypes[making->dependency_datatype];
        if (base_facts.variety == SXML_TYPE_VARIETY_UNION) {
            return SXML_STATUS_OK;
        }
        if (sxml_exi_builtin_types[base_facts.builtin].primitive == SXML_EXI_PRIMITIVE_QNAME) {
            made->whitespace = base->whitespace;
            return SXML_STATUS_OK;
        }
        if (base->datatype_kind == SXML_EXI_DATATYPE_LIST) {
            made->datatype_kind = SXML_EXI_DATATYPE_LIST;
            made->base_datatype = base->base_datatype;
            return SXML_STATUS_OK;
        }
        made->datatype_kind = SXML_EXI_DATATYPE_ENUMERATION;
        made->base_datatype = making->dependency_datatype;
        return sxml_exi_add_enumeration(
            builder,
            &(sxml_exi_enumeration_request){.enumeration_type = facts->enumeration_type,
                                            .base_datatype = making->dependency_datatype},
            made);
    }
    switch (facts->variety) {
    case SXML_TYPE_VARIETY_LIST:
        made->datatype_kind = SXML_EXI_DATATYPE_LIST;
        made->base_datatype = facts->has_item ? making->dependency_datatype : builder->grammars->string_datatype;
        return SXML_STATUS_OK;
    case SXML_TYPE_VARIETY_UNION:
        return SXML_STATUS_OK;
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
        made->whitespace = SXML_EXI_WHITESPACE_PRESERVE;
        return SXML_STATUS_OK;
    case SXML_TYPE_VARIETY_ATOMIC:
        if (making->type.is_builtin && making->type.index == SXML_EXI_BUILTIN_ANY_TYPE) {
            made->whitespace = SXML_EXI_WHITESPACE_PRESERVE;
            return SXML_STATUS_OK;
        }
        return sxml_exi_atomic_datatype(builder, facts, made);
    case SXML_TYPE_VARIETY_COMPLEX:
    case SXML_TYPE_VARIETY_ANY_ITEM:
    case SXML_TYPE_VARIETY_NODE_TEST:
    case SXML_TYPE_VARIETY_JSON_NODE_TEST:
    case SXML_TYPE_VARIETY_FUNCTION:
    case SXML_TYPE_VARIETY_MAP:
    case SXML_TYPE_VARIETY_ARRAY:
    case SXML_TYPE_VARIETY_RECORD:
    case SXML_TYPE_VARIETY_ENUMERATION:
    case SXML_TYPE_VARIETY_CHOICE:
        break;
    }
    made->whitespace = SXML_EXI_WHITESPACE_PRESERVE;
    return SXML_STATUS_OK;
}

/* The datatype of a simple type (the oracle's getDatatype), made once;
   the types it depends on are made first, with an explicit stack. */
static sxml_status sxml_exi_datatype_of(sxml_exi_builder *builder, sxml_exi_type_reference type, uint32_t *datatype) {
    enum { SXML_EXI_DATATYPE_STACK = 64 };
    sxml_exi_type_reference stack[SXML_EXI_DATATYPE_STACK];
    size_t stack_count = 0;
    stack[stack_count++] = type;
    sxml_status status = SXML_STATUS_OK;
    while (stack_count > 0 && status == SXML_STATUS_OK) {
        const sxml_exi_type_reference top = stack[stack_count - 1];
        if (*sxml_exi_datatype_slot(builder, top) != SXML_EXI_NONE) {
            stack_count--;
            continue;
        }
        sxml_exi_type_facts facts = {0};
        sxml_exi_gather_facts(builder, top, &facts);
        const bool needs_base = facts.enumeration_type != 0;
        const bool needs_item = !needs_base && facts.variety == SXML_TYPE_VARIETY_LIST && facts.has_item;
        const sxml_exi_type_reference dependency = needs_base ? facts.enumeration_base : facts.item;
        if ((needs_base || needs_item) && *sxml_exi_datatype_slot(builder, dependency) == SXML_EXI_NONE) {
            if (stack_count == SXML_EXI_DATATYPE_STACK) {
                return SXML_EXI_BUILD_FAIL(builder, SXML_STATUS_LIMIT_EXCEEDED, "simple types nested too deeply");
            }
            stack[stack_count++] = dependency;
            continue;
        }
        sxml_exi_datatype made = {0};
        status = sxml_exi_make_datatype(
            builder,
            &(sxml_exi_datatype_making){.facts = &facts,
                                        .type = top,
                                        .dependency_datatype = (needs_base || needs_item)
                                                                   ? *sxml_exi_datatype_slot(builder, dependency)
                                                                   : SXML_EXI_NONE},
            &made);
        uint32_t index = 0;
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_add_datatype(builder, &made, &index);
        }
        if (status == SXML_STATUS_OK) {
            *sxml_exi_datatype_slot(builder, top) = index;
        }
        stack_count--;
    }
    if (status == SXML_STATUS_OK) {
        *datatype = *sxml_exi_datatype_slot(builder, type);
    }
    return status;
}

/* ------------------------------------------------------------ Grammars */

static sxml_status sxml_exi_new_grammar(sxml_exi_builder *builder, sxml_exi_grammar_kind grammar_kind,
                                        uint32_t *grammar) {
    sxml_exi_schema_grammars *grammars = builder->grammars;
    void *elements = grammars->grammars;
    sxml_status status = sxml_exi_grow_array(grammars->allocator, &(sxml_exi_growth){.elements = &elements,
                                                                                     .capacity = &grammars->grammar_capacity,
                                                                                     .element_count = grammars->grammar_count,
                                                                                     .element_size = sizeof(sxml_exi_schema_grammar)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    grammars->grammars = elements;
    *grammar = (uint32_t)grammars->grammar_count;
    grammars->grammars[grammars->grammar_count++] = (sxml_exi_schema_grammar){.grammar_kind = grammar_kind,
                                                                             .element_content = SXML_EXI_NONE,
                                                                             .type_empty = SXML_EXI_NONE,
                                                                             .least_attribute = SXML_EXI_NONE};
    return SXML_STATUS_OK;
}

/* The order of 8.5.4.3 by terminal, as the oracle sorts its events: AT
   (qname), AT (uri : *), AT (*), SE (qname), SE (uri : *), SE (*), EE,
   typed CH, untyped CH, ED. */
static uint32_t sxml_exi_production_rank(const sxml_exi_schema_production *production) {
    switch (production->event_kind) {
    case SXML_EXI_EVENT_ATTRIBUTE:
        return 0;
    case SXML_EXI_EVENT_ATTRIBUTE_URI:
        return 1;
    case SXML_EXI_EVENT_ATTRIBUTE_ANY:
        return 2;
    case SXML_EXI_EVENT_START_ELEMENT:
        return 3;
    case SXML_EXI_EVENT_START_ELEMENT_URI:
        return 4;
    case SXML_EXI_EVENT_START_ELEMENT_ANY:
        return 5;
    case SXML_EXI_EVENT_END_ELEMENT:
        return 6;
    case SXML_EXI_EVENT_CHARACTERS:
        return production->datatype != SXML_EXI_NONE ? 7 : 8;
    case SXML_EXI_EVENT_END_DOCUMENT:
        return 9;
    case SXML_EXI_EVENT_START_DOCUMENT:
    case SXML_EXI_EVENT_NAMESPACE:
    case SXML_EXI_EVENT_COMMENT:
    case SXML_EXI_EVENT_PROCESSING_INSTRUCTION:
    case SXML_EXI_EVENT_DOCTYPE:
    case SXML_EXI_EVENT_ENTITY_REFERENCE:
    case SXML_EXI_EVENT_SELF_CONTAINED:
        break;
    }
    return 10;
}

static sxml_byte_view sxml_exi_uri_text(const sxml_exi_builder *builder, uint32_t uri) {
    return sxml_exi_string_table_view(builder->table, builder->table->uris[uri].text);
}

static sxml_byte_view sxml_exi_local_text(const sxml_exi_builder *builder, const sxml_exi_schema_production *production) {
    return sxml_exi_string_table_view(builder->table,
                                      builder->table->uris[production->uri].local_names[production->local_name].text);
}

/* Whether production goes after other in a grammar (8.5.4.3): by rank,
   AT (qname) by local-name then URI, AT (uri : *) by URI; SE in the order
   added (schema order). */
static bool sxml_exi_goes_after(const sxml_exi_builder *builder, const sxml_exi_schema_production *production,
                                const sxml_exi_schema_production *other) {
    const uint32_t rank = sxml_exi_production_rank(production);
    const uint32_t other_rank = sxml_exi_production_rank(other);
    if (rank != other_rank) {
        return rank > other_rank;
    }
    if (production->event_kind == SXML_EXI_EVENT_ATTRIBUTE) {
        const int order = sxml_exi_view_order(sxml_exi_local_text(builder, production), sxml_exi_local_text(builder, other));
        if (order != 0) {
            return order > 0;
        }
        return sxml_exi_view_order(sxml_exi_uri_text(builder, production->uri), sxml_exi_uri_text(builder, other->uri)) > 0;
    }
    if (production->event_kind == SXML_EXI_EVENT_ATTRIBUTE_URI) {
        return sxml_exi_view_order(sxml_exi_uri_text(builder, production->uri), sxml_exi_uri_text(builder, other->uri)) > 0;
    }
    return true;
}

static bool sxml_exi_is_same_terminal(const sxml_exi_schema_production *first, const sxml_exi_schema_production *second) {
    if (first->event_kind != second->event_kind) {
        return false;
    }
    switch (first->event_kind) {
    case SXML_EXI_EVENT_ATTRIBUTE:
    case SXML_EXI_EVENT_START_ELEMENT:
        return first->uri == second->uri && first->local_name == second->local_name;
    case SXML_EXI_EVENT_ATTRIBUTE_URI:
    case SXML_EXI_EVENT_START_ELEMENT_URI:
        return first->uri == second->uri;
    case SXML_EXI_EVENT_CHARACTERS:
        return (first->datatype == SXML_EXI_NONE) == (second->datatype == SXML_EXI_NONE);
    case SXML_EXI_EVENT_ATTRIBUTE_ANY:
    case SXML_EXI_EVENT_START_ELEMENT_ANY:
    case SXML_EXI_EVENT_END_ELEMENT:
    case SXML_EXI_EVENT_END_DOCUMENT:
        return true;
    case SXML_EXI_EVENT_START_DOCUMENT:
    case SXML_EXI_EVENT_NAMESPACE:
    case SXML_EXI_EVENT_COMMENT:
    case SXML_EXI_EVENT_PROCESSING_INSTRUCTION:
    case SXML_EXI_EVENT_DOCTYPE:
    case SXML_EXI_EVENT_ENTITY_REFERENCE:
    case SXML_EXI_EVENT_SELF_CONTAINED:
        break;
    }
    return false;
}

/* Adds a production to the grammar being made, in the order of 8.5.4.3;
   a terminal it has already (normalization, 8.5.4.2.2) is not added. */
static sxml_status sxml_exi_add_production(sxml_exi_builder *builder, const sxml_exi_schema_production *production) {
    for (size_t production_index = 0; production_index < builder->scratch_count; production_index++) {
        if (sxml_exi_is_same_terminal(&builder->scratch[production_index], production)) {
            return SXML_STATUS_OK;
        }
    }
    void *elements = builder->scratch;
    sxml_status status = sxml_exi_grow_array(builder->allocator, &(sxml_exi_growth){.elements = &elements,
                                                                                    .capacity = &builder->scratch_capacity,
                                                                                    .element_count = builder->scratch_count,
                                                                                    .element_size = sizeof(sxml_exi_schema_production)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    builder->scratch = elements;
    size_t position = builder->scratch_count;
    while (position > 0 && !sxml_exi_goes_after(builder, production, &builder->scratch[position - 1])) {
        position--;
    }
    memmove(builder->scratch + position + 1, builder->scratch + position,
            (builder->scratch_count - position) * sizeof(sxml_exi_schema_production));
    builder->scratch[position] = *production;
    builder->scratch_count++;
    return SXML_STATUS_OK;
}

/* Gives the productions made to grammar (event codes in order). */
static sxml_status sxml_exi_commit(sxml_exi_builder *builder, uint32_t grammar) {
    sxml_exi_schema_grammars *grammars = builder->grammars;
    if (grammars->production_count + builder->scratch_count > grammars->production_capacity) {
        void *elements = grammars->productions;
        sxml_status status = sxml_allocator_reserve_array(
            grammars->allocator, &(sxml_array_reservation_request){.elements = &elements,
                                                                   .capacity = &grammars->production_capacity,
                                                                   .element_count = grammars->production_count,
                                                                   .required_count = grammars->production_count + builder->scratch_count,
                                                                   .element_size = sizeof(sxml_exi_schema_production),
                                                                   .element_alignment = alignof(sxml_exi_schema_production),
                                                                   .minimum_capacity = 256});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        grammars->productions = elements;
    }
    sxml_exi_schema_grammar *made = &grammars->grammars[grammar];
    made->first_production = (uint32_t)grammars->production_count;
    made->production_count = (uint32_t)builder->scratch_count;
    for (size_t production_index = 0; production_index < builder->scratch_count; production_index++) {
        const sxml_exi_schema_production *production = &builder->scratch[production_index];
        grammars->productions[grammars->production_count++] = *production;
        if (production->event_kind == SXML_EXI_EVENT_END_ELEMENT) {
            made->has_end_element = true;
        }
        if (production->event_kind == SXML_EXI_EVENT_ATTRIBUTE) {
            if (made->least_attribute == SXML_EXI_NONE) {
                made->least_attribute = (uint32_t)production_index;
            }
            made->attribute_count++;
        }
    }
    builder->scratch_count = 0;
    return SXML_STATUS_OK;
}

/* Copies a committed grammar's productions into the grammar being made;
   a production leading back to source leads to redirect instead (unless
   it is SXML_EXI_NONE), and AT (*) and AT (uri : *) are left out when
   skips_wildcards. */
typedef struct sxml_exi_copy {
    uint32_t source;
    uint32_t redirect;
    bool skips_wildcards;
} sxml_exi_copy;

static sxml_status sxml_exi_copy_productions(sxml_exi_builder *builder, const sxml_exi_copy *copy) {
    const sxml_exi_schema_grammar source = builder->grammars->grammars[copy->source];
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t production_index = 0; production_index < source.production_count && status == SXML_STATUS_OK;
         production_index++) {
        sxml_exi_schema_production production = builder->grammars->productions[source.first_production + production_index];
        if (copy->skips_wildcards && (production.event_kind == SXML_EXI_EVENT_ATTRIBUTE_ANY ||
                                      production.event_kind == SXML_EXI_EVENT_ATTRIBUTE_URI)) {
            continue;
        }
        if (copy->redirect != SXML_EXI_NONE && production.next == copy->source) {
            production.next = copy->redirect;
        }
        status = sxml_exi_add_production(builder, &production);
    }
    return status;
}

/* TypeEmpty of a start tag (8.5.4.1.3, made as the oracle makes it from
   the start tag at run time): its attribute productions, each leading to
   the TypeEmpty of where it led, and EE. */
static sxml_status sxml_exi_make_type_empty(sxml_exi_builder *builder, uint32_t grammar) {
    uint32_t type_empty = 0;
    sxml_status status = sxml_exi_new_grammar(builder, builder->grammars->grammars[grammar].grammar_kind, &type_empty);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_exi_schema_grammar source = builder->grammars->grammars[grammar];
    for (uint32_t production_index = 0; production_index < source.production_count && status == SXML_STATUS_OK;
         production_index++) {
        sxml_exi_schema_production production = builder->grammars->productions[source.first_production + production_index];
        const bool is_attribute = production.event_kind == SXML_EXI_EVENT_ATTRIBUTE ||
                                  production.event_kind == SXML_EXI_EVENT_ATTRIBUTE_URI ||
                                  production.event_kind == SXML_EXI_EVENT_ATTRIBUTE_ANY;
        if (is_attribute) {
            production.next =
                production.next == grammar ? type_empty : builder->grammars->grammars[production.next].type_empty;
        } else {
            production = (sxml_exi_schema_production){.event_kind = SXML_EXI_EVENT_END_ELEMENT,
                                                      .datatype = SXML_EXI_NONE,
                                                      .next = SXML_EXI_NONE,
                                                      .element = SXML_EXI_NONE};
        }
        status = sxml_exi_add_production(builder, &production);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_commit(builder, type_empty);
    }
    if (status == SXML_STATUS_OK) {
        sxml_exi_schema_grammar *made = &builder->grammars->grammars[type_empty];
        made->element_content = builder->grammars->empty_content;
        made->type_empty = type_empty;
        builder->grammars->grammars[grammar].type_empty = type_empty;
    }
    return status;
}

/* Commits a start tag and makes its TypeEmpty. */
static sxml_status sxml_exi_commit_start_tag(sxml_exi_builder *builder, uint32_t grammar) {
    sxml_status status = sxml_exi_commit(builder, grammar);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_make_type_empty(builder, grammar);
    }
    return status;
}

/* ------------------------------------------------------ Content models */

/* An edge of the automaton of a content model: to target, on a terminal
   (an index of terminals) or none (SXML_EXI_NONE). */
typedef struct sxml_exi_automaton_edge {
    uint32_t target;
    uint32_t terminal;
    uint32_t next_edge;
} sxml_exi_automaton_edge;

/* A term an edge reads: an element declaration or a wildcard. Terminals
   are numbered in schema order (8.5.4.3). */
typedef struct sxml_exi_terminal {
    sxml_schema_term_kind term_kind;
    uint32_t term;
} sxml_exi_terminal;

/* Work: a particle or a term to build between two nodes (Thompson's
   construction, each particle between fresh nodes of its own). */
typedef struct sxml_exi_automaton_item {
    bool is_particle;
    sxml_schema_term_kind term_kind;
    uint32_t component;
    uint32_t from;
    uint32_t to;
} sxml_exi_automaton_item;

/* A state of the deterministic automaton: a set of nodes (closed under
   edges without terminal) and its grammar. */
typedef struct sxml_exi_automaton_state {
    size_t first_node;
    uint32_t node_count;
    uint32_t grammar;
    uint64_t hash;
} sxml_exi_automaton_state;

/* An edge read from a state: its terminal and target node. */
typedef struct sxml_exi_transition {
    uint32_t terminal;
    uint32_t target;
} sxml_exi_transition;

typedef struct sxml_exi_automaton {
    uint32_t *first_edges;
    size_t node_count;
    size_t node_capacity;
    sxml_exi_automaton_edge *edges;
    size_t edge_count;
    size_t edge_capacity;
    sxml_exi_terminal *terminals;
    size_t terminal_count;
    size_t terminal_capacity;
    sxml_exi_automaton_item *items;
    size_t item_count;
    size_t item_capacity;
    uint32_t *state_nodes;
    size_t state_node_count;
    size_t state_node_capacity;
    sxml_exi_automaton_state *states;
    size_t state_count;
    size_t state_capacity;
    uint32_t *slots;
    size_t slot_count;
    /* Closure scratch: a mark per node, a stack, the set being closed. */
    uint32_t *marks;
    uint32_t mark;
    uint32_t *stack;
    size_t stack_count;
    size_t stack_capacity;
    uint32_t *closure;
    size_t closure_count;
    size_t closure_capacity;
    sxml_exi_transition *transitions;
    size_t transition_count;
    size_t transition_capacity;
    uint32_t final_node;
    bool is_mixed;
} sxml_exi_automaton;

static void sxml_exi_release_automaton(sxml_allocator allocator, sxml_exi_automaton *automaton) {
    sxml_allocator_release(allocator, automaton->first_edges);
    sxml_allocator_release(allocator, automaton->edges);
    sxml_allocator_release(allocator, automaton->terminals);
    sxml_allocator_release(allocator, automaton->items);
    sxml_allocator_release(allocator, automaton->state_nodes);
    sxml_allocator_release(allocator, automaton->states);
    sxml_allocator_release(allocator, automaton->slots);
    sxml_allocator_release(allocator, automaton->marks);
    sxml_allocator_release(allocator, automaton->stack);
    sxml_allocator_release(allocator, automaton->closure);
    sxml_allocator_release(allocator, automaton->transitions);
}

static sxml_status sxml_exi_new_node(sxml_exi_builder *builder, sxml_exi_automaton *automaton, uint32_t *node) {
    if (automaton->node_count >= SXML_EXI_MAXIMUM_AUTOMATON_NODES) {
        return SXML_EXI_BUILD_FAIL(builder, SXML_STATUS_LIMIT_EXCEEDED, "a content model too large to expand");
    }
    void *elements = automaton->first_edges;
    sxml_status status = sxml_exi_grow_array(builder->allocator, &(sxml_exi_growth){.elements = &elements,
                                                                                    .capacity = &automaton->node_capacity,
                                                                                    .element_count = automaton->node_count,
                                                                                    .element_size = sizeof(uint32_t)});
    if (status == SXML_STATUS_OK) {
        automaton->first_edges = elements;
        *node = (uint32_t)automaton->node_count;
        automaton->first_edges[automaton->node_count++] = SXML_EXI_NONE;
    }
    return status;
}

static sxml_status sxml_exi_new_edge(sxml_exi_builder *builder, sxml_exi_automaton *automaton,
                                     const sxml_exi_automaton_item *item) {
    void *elements = automaton->edges;
    sxml_status status = sxml_exi_grow_array(builder->allocator, &(sxml_exi_growth){.elements = &elements,
                                                                                    .capacity = &automaton->edge_capacity,
                                                                                    .element_count = automaton->edge_count,
                                                                                    .element_size = sizeof(sxml_exi_automaton_edge)});
    if (status == SXML_STATUS_OK) {
        automaton->edges = elements;
        automaton->edges[automaton->edge_count] = (sxml_exi_automaton_edge){
            .target = item->to, .terminal = item->component, .next_edge = automaton->first_edges[item->from]};
        automaton->first_edges[item->from] = (uint32_t)automaton->edge_count;
        automaton->edge_count++;
    }
    return status;
}

/* An edge without terminal. */
typedef struct sxml_exi_node_pair {
    uint32_t from;
    uint32_t to;
} sxml_exi_node_pair;

static sxml_status sxml_exi_new_empty_edge(sxml_exi_builder *builder, sxml_exi_automaton *automaton,
                                           sxml_exi_node_pair pair) {
    return sxml_exi_new_edge(builder, automaton,
                             &(sxml_exi_automaton_item){.component = SXML_EXI_NONE, .from = pair.from, .to = pair.to});
}

static sxml_status sxml_exi_push_item(sxml_exi_builder *builder, sxml_exi_automaton *automaton,
                                      const sxml_exi_automaton_item *item) {
    void *elements = automaton->items;
    sxml_status status = sxml_exi_grow_array(builder->allocator, &(sxml_exi_growth){.elements = &elements,
                                                                                    .capacity = &automaton->item_capacity,
                                                                                    .element_count = automaton->item_count,
                                                                                    .element_size = sizeof(sxml_exi_automaton_item)});
    if (status == SXML_STATUS_OK) {
        automaton->items = elements;
        automaton->items[automaton->item_count++] = *item;
    }
    return status;
}

/* The terminal of a term (element declaration or wildcard), numbered at
   its first appearance; terminals are found by search, which a content
   model's few distinct terms make cheap. */
static sxml_status sxml_exi_terminal_of(sxml_exi_builder *builder, sxml_exi_automaton *automaton,
                                        sxml_exi_terminal terminal) {
    for (size_t terminal_index = 0; terminal_index < automaton->terminal_count; terminal_index++) {
        if (automaton->terminals[terminal_index].term_kind == terminal.term_kind &&
            automaton->terminals[terminal_index].term == terminal.term) {
            return SXML_STATUS_OK;
        }
    }
    void *elements = automaton->terminals;
    sxml_status status = sxml_exi_grow_array(builder->allocator, &(sxml_exi_growth){.elements = &elements,
                                                                                    .capacity = &automaton->terminal_capacity,
                                                                                    .element_count = automaton->terminal_count,
                                                                                    .element_size = sizeof(sxml_exi_terminal)});
    if (status == SXML_STATUS_OK) {
        automaton->terminals = elements;
        automaton->terminals[automaton->terminal_count++] = terminal;
    }
    return status;
}

static uint32_t sxml_exi_find_terminal(const sxml_exi_automaton *automaton, sxml_exi_terminal terminal) {
    for (size_t terminal_index = 0; terminal_index < automaton->terminal_count; terminal_index++) {
        if (automaton->terminals[terminal_index].term_kind == terminal.term_kind &&
            automaton->terminals[terminal_index].term == terminal.term) {
            return (uint32_t)terminal_index;
        }
    }
    return SXML_EXI_NONE;
}

/* Numbers the terms of a content model in schema order: a depth-first
   walk of its particles (8.5.4.3). */
static sxml_status sxml_exi_number_terminals(sxml_exi_builder *builder, sxml_exi_automaton *automaton,
                                             uint32_t particle) {
    const sxml_schema_components *components = builder->components;
    automaton->item_count = 0;
    sxml_status status = sxml_exi_push_item(builder, automaton,
                                            &(sxml_exi_automaton_item){.is_particle = true, .component = particle});
    while (status == SXML_STATUS_OK && automaton->item_count > 0) {
        const sxml_exi_automaton_item item = automaton->items[--automaton->item_count];
        const sxml_schema_particle *definition =
            sxml_schema_get_particle(components, (sxml_schema_particle_index){.value = item.component});
        switch (definition->term_kind) {
        case SXML_SCHEMA_TERM_ELEMENT_DECLARATION:
        case SXML_SCHEMA_TERM_WILDCARD:
            status = sxml_exi_terminal_of(
                builder, automaton, (sxml_exi_terminal){.term_kind = definition->term_kind, .term = definition->term});
            break;
        case SXML_SCHEMA_TERM_MODEL_GROUP: {
            const sxml_schema_model_group *group =
                sxml_schema_get_model_group(components, (sxml_schema_model_group_index){.value = definition->term});
            const sxml_schema_index_view children = sxml_schema_get_list(components, group->particles);
            for (size_t child_index = children.index_count; child_index > 0 && status == SXML_STATUS_OK; child_index--) {
                status = sxml_exi_push_item(
                    builder, automaton,
                    &(sxml_exi_automaton_item){.is_particle = true, .component = children.indexes[child_index - 1]});
            }
            break;
        }
        case SXML_SCHEMA_TERM_NONE:
            break;
        }
    }
    return status;
}

/* Builds a particle between two nodes: min occurs copies of its term,
   then max occurs - min occurs optional ones, or one repeated
   (8.5.4.1.5). */
static sxml_status sxml_exi_build_particle(sxml_exi_builder *builder, sxml_exi_automaton *automaton,
                                           const sxml_exi_automaton_item *item) {
    const sxml_schema_particle *definition =
        sxml_schema_get_particle(builder->components, (sxml_schema_particle_index){.value = item->component});
    uint32_t start = 0;
    uint32_t end = 0;
    sxml_status status = sxml_exi_new_node(builder, automaton, &start);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_new_node(builder, automaton, &end);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_new_empty_edge(builder, automaton, (sxml_exi_node_pair){.from = item->from, .to = start});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_new_empty_edge(builder, automaton, (sxml_exi_node_pair){.from = end, .to = item->to});
    }
    const uint64_t minimum = definition->minimum_occurrences;
    const uint64_t maximum = definition->maximum_occurrences;
    const bool is_unbounded = maximum == SXML_SCHEMA_UNBOUNDED;
    if ((minimum > SXML_EXI_MAXIMUM_AUTOMATON_NODES) || (!is_unbounded && maximum > SXML_EXI_MAXIMUM_AUTOMATON_NODES)) {
        return SXML_EXI_BUILD_FAIL(builder, SXML_STATUS_LIMIT_EXCEEDED, "occurrences too many to expand");
    }
    uint32_t cursor = start;
    const sxml_exi_automaton_item term = {
        .is_particle = false, .term_kind = definition->term_kind, .component = definition->term};
    for (uint64_t copy_index = 0; copy_index < minimum && status == SXML_STATUS_OK; copy_index++) {
        uint32_t next = 0;
        status = sxml_exi_new_node(builder, automaton, &next);
        sxml_exi_automaton_item copy = term;
        copy.from = cursor;
        copy.to = next;
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_push_item(builder, automaton, &copy);
        }
        cursor = next;
    }
    if (is_unbounded && status == SXML_STATUS_OK) {
        uint32_t repeated = 0;
        status = sxml_exi_new_node(builder, automaton, &repeated);
        sxml_exi_automaton_item copy = term;
        copy.from = cursor;
        copy.to = repeated;
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_push_item(builder, automaton, &copy);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_new_empty_edge(builder, automaton, (sxml_exi_node_pair){.from = repeated, .to = cursor});
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_new_empty_edge(builder, automaton, (sxml_exi_node_pair){.from = cursor, .to = end});
        }
        return status;
    }
    for (uint64_t copy_index = minimum; copy_index < maximum && status == SXML_STATUS_OK; copy_index++) {
        status = sxml_exi_new_empty_edge(builder, automaton, (sxml_exi_node_pair){.from = cursor, .to = end});
        uint32_t next = 0;
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_new_node(builder, automaton, &next);
        }
        sxml_exi_automaton_item copy = term;
        copy.from = cursor;
        copy.to = next;
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_push_item(builder, automaton, &copy);
        }
        cursor = next;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_new_empty_edge(builder, automaton, (sxml_exi_node_pair){.from = cursor, .to = end});
    }
    return status;
}

/* Builds a term between two nodes: an edge reading an element or a
   wildcard, or the particles of a model group in sequence or as
   alternatives (8.5.4.1.6 to 8.5.4.1.8). */
static sxml_status sxml_exi_build_term(sxml_exi_builder *builder, sxml_exi_automaton *automaton,
                                       const sxml_exi_automaton_item *item) {
    const sxml_schema_components *components = builder->components;
    if (item->term_kind == SXML_SCHEMA_TERM_ELEMENT_DECLARATION || item->term_kind == SXML_SCHEMA_TERM_WILDCARD) {
        const uint32_t terminal =
            sxml_exi_find_terminal(automaton, (sxml_exi_terminal){.term_kind = item->term_kind, .term = item->component});
        return sxml_exi_new_edge(builder, automaton,
                                 &(sxml_exi_automaton_item){.component = terminal, .from = item->from, .to = item->to});
    }
    if (item->term_kind != SXML_SCHEMA_TERM_MODEL_GROUP) {
        return sxml_exi_new_empty_edge(builder, automaton, (sxml_exi_node_pair){.from = item->from, .to = item->to});
    }
    const sxml_schema_model_group *group =
        sxml_schema_get_model_group(components, (sxml_schema_model_group_index){.value = item->component});
    const sxml_schema_index_view children = sxml_schema_get_list(components, group->particles);
    if (children.index_count == 0) {
        return sxml_exi_new_empty_edge(builder, automaton, (sxml_exi_node_pair){.from = item->from, .to = item->to});
    }
    sxml_status status = SXML_STATUS_OK;
    if (group->compositor == SXML_SCHEMA_COMPOSITOR_CHOICE) {
        for (size_t child_index = 0; child_index < children.index_count && status == SXML_STATUS_OK; child_index++) {
            status = sxml_exi_push_item(builder, automaton,
                                        &(sxml_exi_automaton_item){.is_particle = true,
                                                                   .component = children.indexes[child_index],
                                                                   .from = item->from,
                                                                   .to = item->to});
        }
        return status;
    }
    /* A sequence (an all group inside another is read as one too: XML
       Schema 1.0 allows none). */
    uint32_t cursor = item->from;
    for (size_t child_index = 0; child_index < children.index_count && status == SXML_STATUS_OK; child_index++) {
        uint32_t next = item->to;
        if (child_index + 1 < children.index_count) {
            status = sxml_exi_new_node(builder, automaton, &next);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_push_item(builder, automaton,
                                        &(sxml_exi_automaton_item){.is_particle = true,
                                                                   .component = children.indexes[child_index],
                                                                   .from = cursor,
                                                                   .to = next});
        }
        cursor = next;
    }
    return status;
}

/* Builds the nondeterministic automaton of a content particle from node
   0 to final_node. */
static sxml_status sxml_exi_build_automaton(sxml_exi_builder *builder, sxml_exi_automaton *automaton,
                                            uint32_t particle) {
    uint32_t start = 0;
    sxml_status status = sxml_exi_number_terminals(builder, automaton, particle);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_new_node(builder, automaton, &start);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_new_node(builder, automaton, &automaton->final_node);
    }
    automaton->item_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_push_item(builder, automaton,
                                    &(sxml_exi_automaton_item){.is_particle = true,
                                                               .component = particle,
                                                               .from = start,
                                                               .to = automaton->final_node});
    }
    while (status == SXML_STATUS_OK && automaton->item_count > 0) {
        const sxml_exi_automaton_item item = automaton->items[--automaton->item_count];
        status = item.is_particle ? sxml_exi_build_particle(builder, automaton, &item)
                                  : sxml_exi_build_term(builder, automaton, &item);
    }
    return status;
}

/* Closes automaton->closure (seeded) under edges without terminal, and
   sorts it. */
static int sxml_exi_compare_nodes(const void *first, const void *second) {
    const uint32_t left = *(const uint32_t *)first;
    const uint32_t right = *(const uint32_t *)second;
    return left < right ? -1 : left > right ? 1 : 0;
}

static sxml_status sxml_exi_push_closure(sxml_exi_builder *builder, sxml_exi_automaton *automaton, uint32_t node) {
    if (automaton->marks[node] == automaton->mark) {
        return SXML_STATUS_OK;
    }
    automaton->marks[node] = automaton->mark;
    void *elements = automaton->closure;
    sxml_status status = sxml_exi_grow_array(builder->allocator, &(sxml_exi_growth){.elements = &elements,
                                                                                    .capacity = &automaton->closure_capacity,
                                                                                    .element_count = automaton->closure_count,
                                                                                    .element_size = sizeof(uint32_t)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    automaton->closure = elements;
    automaton->closure[automaton->closure_count++] = node;
    elements = automaton->stack;
    status = sxml_exi_grow_array(builder->allocator, &(sxml_exi_growth){.elements = &elements,
                                                                        .capacity = &automaton->stack_capacity,
                                                                        .element_count = automaton->stack_count,
                                                                        .element_size = sizeof(uint32_t)});
    if (status == SXML_STATUS_OK) {
        automaton->stack = elements;
        automaton->stack[automaton->stack_count++] = node;
    }
    return status;
}

static sxml_status sxml_exi_close(sxml_exi_builder *builder, sxml_exi_automaton *automaton) {
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && automaton->stack_count > 0) {
        const uint32_t node = automaton->stack[--automaton->stack_count];
        for (uint32_t edge_index = automaton->first_edges[node]; edge_index != SXML_EXI_NONE && status == SXML_STATUS_OK;
             edge_index = automaton->edges[edge_index].next_edge) {
            if (automaton->edges[edge_index].terminal == SXML_EXI_NONE) {
                status = sxml_exi_push_closure(builder, automaton, automaton->edges[edge_index].target);
            }
        }
    }
    if (automaton->closure_count > 1) {
        qsort(automaton->closure, automaton->closure_count, sizeof(uint32_t), sxml_exi_compare_nodes);
    }
    return status;
}

static uint64_t sxml_exi_hash_nodes(const uint32_t *nodes, size_t node_count) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t node_index = 0; node_index < node_count; node_index++) {
        hash ^= nodes[node_index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/* Grows the state index to twice the states, rehashing. */
static sxml_status sxml_exi_grow_slots(sxml_exi_builder *builder, sxml_exi_automaton *automaton) {
    const size_t slot_count = automaton->slot_count == 0 ? 64 : automaton->slot_count * 2;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        builder->allocator,
        (sxml_array_allocation_request){.element_count = slot_count, .element_size = sizeof(uint32_t), .element_alignment = alignof(uint32_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t *slots = memory;
    for (size_t slot_index = 0; slot_index < slot_count; slot_index++) {
        slots[slot_index] = SXML_EXI_NONE;
    }
    for (size_t state_index = 0; state_index < automaton->state_count; state_index++) {
        size_t slot = (size_t)(automaton->states[state_index].hash & (slot_count - 1));
        while (slots[slot] != SXML_EXI_NONE) {
            slot = (slot + 1) & (slot_count - 1);
        }
        slots[slot] = (uint32_t)state_index;
    }
    sxml_allocator_release(builder->allocator, automaton->slots);
    automaton->slots = slots;
    automaton->slot_count = slot_count;
    return SXML_STATUS_OK;
}

/* The state of the closed set in automaton->closure: found, or added with
   a new element grammar. */
static sxml_status sxml_exi_state_of_closure(sxml_exi_builder *builder, sxml_exi_automaton *automaton,
                                             uint32_t *state) {
    const uint64_t hash = sxml_exi_hash_nodes(automaton->closure, automaton->closure_count);
    if (automaton->slot_count > 0) {
        size_t slot = (size_t)(hash & (automaton->slot_count - 1));
        while (automaton->slots[slot] != SXML_EXI_NONE) {
            const sxml_exi_automaton_state *candidate = &automaton->states[automaton->slots[slot]];
            if (candidate->hash == hash && candidate->node_count == automaton->closure_count &&
                memcmp(automaton->state_nodes + candidate->first_node, automaton->closure,
                       automaton->closure_count * sizeof(uint32_t)) == 0) {
                *state = automaton->slots[slot];
                return SXML_STATUS_OK;
            }
            slot = (slot + 1) & (automaton->slot_count - 1);
        }
    }
    if (automaton->state_count >= SXML_EXI_MAXIMUM_AUTOMATON_STATES) {
        return SXML_EXI_BUILD_FAIL(builder, SXML_STATUS_LIMIT_EXCEEDED, "a content model with too many states");
    }
    sxml_status status = SXML_STATUS_OK;
    if ((automaton->state_count + 1) * 2 > automaton->slot_count) {
        status = sxml_exi_grow_slots(builder, automaton);
    }
    if (status == SXML_STATUS_OK && automaton->state_node_count + automaton->closure_count > automaton->state_node_capacity) {
        void *elements = automaton->state_nodes;
        status = sxml_allocator_reserve_array(
            builder->allocator, &(sxml_array_reservation_request){.elements = &elements,
                                                                  .capacity = &automaton->state_node_capacity,
                                                                  .element_count = automaton->state_node_count,
                                                                  .required_count = automaton->state_node_count + automaton->closure_count,
                                                                  .element_size = sizeof(uint32_t),
                                                                  .element_alignment = alignof(uint32_t),
                                                                  .minimum_capacity = 256});
        automaton->state_nodes = elements;
    }
    void *elements = automaton->states;
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_grow_array(builder->allocator, &(sxml_exi_growth){.elements = &elements,
                                                                            .capacity = &automaton->state_capacity,
                                                                            .element_count = automaton->state_count,
                                                                            .element_size = sizeof(sxml_exi_automaton_state)});
    }
    uint32_t grammar = 0;
    if (status == SXML_STATUS_OK) {
        automaton->states = elements;
        status = sxml_exi_new_grammar(builder, SXML_EXI_GRAMMAR_ELEMENT, &grammar);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memcpy(automaton->state_nodes + automaton->state_node_count, automaton->closure,
           automaton->closure_count * sizeof(uint32_t));
    automaton->states[automaton->state_count] = (sxml_exi_automaton_state){.first_node = automaton->state_node_count,
                                                                          .node_count = (uint32_t)automaton->closure_count,
                                                                          .grammar = grammar,
                                                                          .hash = hash};
    automaton->state_node_count += automaton->closure_count;
    size_t slot = (size_t)(hash & (automaton->slot_count - 1));
    while (automaton->slots[slot] != SXML_EXI_NONE) {
        slot = (slot + 1) & (automaton->slot_count - 1);
    }
    automaton->slots[slot] = (uint32_t)automaton->state_count;
    *state = (uint32_t)automaton->state_count;
    automaton->state_count++;
    return SXML_STATUS_OK;
}

static int sxml_exi_compare_transitions(const void *first, const void *second) {
    const sxml_exi_transition *left = first;
    const sxml_exi_transition *right = second;
    if (left->terminal != right->terminal) {
        return left->terminal < right->terminal ? -1 : 1;
    }
    return left->target < right->target ? -1 : left->target > right->target ? 1 : 0;
}

/* An element declaration and its qname, to sort a substitution group. */
typedef struct sxml_exi_member {
    uint32_t declaration;
    sxml_exi_name_entry name;
} sxml_exi_member;

static int sxml_exi_compare_members(const void *first, const void *second) {
    const sxml_exi_member *left = first;
    const sxml_exi_member *right = second;
    const int order = sxml_exi_view_order(left->name.local_name, right->name.local_name);
    return order != 0 ? order : sxml_exi_view_order(left->name.uri, right->name.uri);
}

/* The productions of a terminal leading to grammar next: SE (qname) for
   the declaration and its substitution group sorted (8.5.4.1.6), SE (*)
   or SE (uri : *) for a wildcard (8.5.4.1.7). */
typedef struct sxml_exi_terminal_productions {
    sxml_exi_terminal terminal;
    uint32_t next;
} sxml_exi_terminal_productions;

static sxml_status sxml_exi_add_terminal_productions(sxml_exi_builder *builder,
                                                     const sxml_exi_terminal_productions *request) {
    const sxml_schema_components *components = builder->components;
    sxml_status status = SXML_STATUS_OK;
    if (request->terminal.term_kind == SXML_SCHEMA_TERM_WILDCARD) {
        const sxml_schema_wildcard *wildcard =
            sxml_schema_get_wildcard(components, (sxml_schema_wildcard_index){.value = request->terminal.term});
        if (wildcard->namespace_constraint_variety != SXML_SCHEMA_NAMESPACE_CONSTRAINT_ENUMERATION) {
            return sxml_exi_add_production(builder, &(sxml_exi_schema_production){.event_kind = SXML_EXI_EVENT_START_ELEMENT_ANY,
                                                                                  .datatype = SXML_EXI_NONE,
                                                                                  .next = request->next,
                                                                                  .element = SXML_EXI_NONE});
        }
        const sxml_schema_index_view namespaces = sxml_schema_get_list(components, wildcard->namespaces);
        for (size_t namespace_index = 0; namespace_index < namespaces.index_count && status == SXML_STATUS_OK;
             namespace_index++) {
            sxml_byte_view text = {0};
            uint32_t uri = 0;
            status = sxml_exi_namespace_text(builder, namespaces.indexes[namespace_index], &text);
            if (status == SXML_STATUS_OK) {
                status = sxml_exi_find_uri_index(builder, text, &uri);
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_exi_add_production(builder, &(sxml_exi_schema_production){.event_kind = SXML_EXI_EVENT_START_ELEMENT_URI,
                                                                                        .uri = uri,
                                                                                        .datatype = SXML_EXI_NONE,
                                                                                        .next = request->next,
                                                                                        .element = SXML_EXI_NONE});
            }
        }
        return status;
    }
    const sxml_schema_element_declaration *declaration = sxml_schema_get_element_declaration(
        components, (sxml_schema_element_declaration_index){.value = request->terminal.term});
    const sxml_schema_index_view members = sxml_schema_get_list(components, declaration->substitution_group_members);
    void *memory = NULL;
    status = sxml_allocator_allocate_array(builder->allocator,
                                           (sxml_array_allocation_request){.element_count = members.index_count + 1,
                                                                           .element_size = sizeof(sxml_exi_member),
                                                                           .element_alignment = alignof(sxml_exi_member)},
                                           &memory);
    sxml_exi_member *sorted = memory;
    size_t member_count = 0;
    if (status == SXML_STATUS_OK) {
        sorted[member_count].declaration = request->terminal.term;
        status = sxml_exi_name_texts(builder, declaration->element_name, &sorted[member_count].name);
        member_count++;
    }
    for (size_t member_index = 0; member_index < members.index_count && status == SXML_STATUS_OK; member_index++) {
        const sxml_schema_element_declaration *member = sxml_schema_get_element_declaration(
            components, (sxml_schema_element_declaration_index){.value = members.indexes[member_index]});
        sorted[member_count].declaration = members.indexes[member_index];
        status = sxml_exi_name_texts(builder, member->element_name, &sorted[member_count].name);
        member_count++;
    }
    if (status == SXML_STATUS_OK && member_count > 1) {
        qsort(sorted, member_count, sizeof sorted[0], sxml_exi_compare_members);
    }
    for (size_t member_index = 0; member_index < member_count && status == SXML_STATUS_OK; member_index++) {
        sxml_exi_table_name name = {0};
        status = sxml_exi_find_table_name(builder, sorted[member_index].name, &name);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_add_production(builder, &(sxml_exi_schema_production){.event_kind = SXML_EXI_EVENT_START_ELEMENT,
                                                                                    .uri = name.uri,
                                                                                    .local_name = name.local_name,
                                                                                    .datatype = SXML_EXI_NONE,
                                                                                    .next = request->next,
                                                                                    .element = sorted[member_index].declaration});
        }
    }
    sxml_allocator_release(builder->allocator, sorted);
    return status;
}

/* The transitions of a state, grouped by terminal: each group's targets
   closed give the next state. */
static sxml_status sxml_exi_expand_state(sxml_exi_builder *builder, sxml_exi_automaton *automaton, uint32_t state) {
    const sxml_exi_automaton_state current = automaton->states[state];
    automaton->transition_count = 0;
    bool is_final = false;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t node_index = 0; node_index < current.node_count && status == SXML_STATUS_OK; node_index++) {
        const uint32_t node = automaton->state_nodes[current.first_node + node_index];
        is_final = is_final || node == automaton->final_node;
        for (uint32_t edge_index = automaton->first_edges[node]; edge_index != SXML_EXI_NONE && status == SXML_STATUS_OK;
             edge_index = automaton->edges[edge_index].next_edge) {
            if (automaton->edges[edge_index].terminal == SXML_EXI_NONE) {
                continue;
            }
            void *elements = automaton->transitions;
            status = sxml_exi_grow_array(builder->allocator, &(sxml_exi_growth){.elements = &elements,
                                                                                .capacity = &automaton->transition_capacity,
                                                                                .element_count = automaton->transition_count,
                                                                                .element_size = sizeof(sxml_exi_transition)});
            if (status == SXML_STATUS_OK) {
                automaton->transitions = elements;
                automaton->transitions[automaton->transition_count++] = (sxml_exi_transition){
                    .terminal = automaton->edges[edge_index].terminal, .target = automaton->edges[edge_index].target};
            }
        }
    }
    if (status == SXML_STATUS_OK && automaton->transition_count > 1) {
        qsort(automaton->transitions, automaton->transition_count, sizeof(sxml_exi_transition),
              sxml_exi_compare_transitions);
    }
    /* Each terminal's next state, closed now, before productions are made
       (making a state needs no scratch). */
    size_t group_start = 0;
    while (group_start < automaton->transition_count && status == SXML_STATUS_OK) {
        const uint32_t terminal = automaton->transitions[group_start].terminal;
        size_t group_end = group_start;
        automaton->mark++;
        automaton->closure_count = 0;
        automaton->stack_count = 0;
        while (group_end < automaton->transition_count && automaton->transitions[group_end].terminal == terminal &&
               status == SXML_STATUS_OK) {
            status = sxml_exi_push_closure(builder, automaton, automaton->transitions[group_end].target);
            group_end++;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_close(builder, automaton);
        }
        uint32_t next = 0;
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_state_of_closure(builder, automaton, &next);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_add_terminal_productions(
                builder, &(sxml_exi_terminal_productions){.terminal = automaton->terminals[terminal],
                                                          .next = automaton->states[next].grammar});
        }
        group_start = group_end;
    }
    const uint32_t grammar = automaton->states[state].grammar;
    if (status == SXML_STATUS_OK && is_final) {
        status = sxml_exi_add_production(builder, &(sxml_exi_schema_production){.event_kind = SXML_EXI_EVENT_END_ELEMENT,
                                                                                .datatype = SXML_EXI_NONE,
                                                                                .next = SXML_EXI_NONE,
                                                                                .element = SXML_EXI_NONE});
    }
    if (status == SXML_STATUS_OK && automaton->is_mixed) {
        status = sxml_exi_add_production(builder, &(sxml_exi_schema_production){.event_kind = SXML_EXI_EVENT_CHARACTERS,
                                                                                .datatype = SXML_EXI_NONE,
                                                                                .next = grammar,
                                                                                .element = SXML_EXI_NONE});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_commit(builder, grammar);
    }
    return status;
}

/* The element grammars of a content particle (0: empty); the first
   state's grammar to *grammar. */
typedef struct sxml_exi_content_request {
    uint32_t particle;
    bool is_mixed;
} sxml_exi_content_request;

static sxml_status sxml_exi_build_content(sxml_exi_builder *builder, const sxml_exi_content_request *request,
                                          uint32_t *grammar) {
    sxml_exi_automaton automaton = {.is_mixed = request->is_mixed};
    sxml_status status = SXML_STATUS_OK;
    if (request->particle != 0) {
        status = sxml_exi_build_automaton(builder, &automaton, request->particle);
    } else {
        uint32_t node = 0;
        status = sxml_exi_new_node(builder, &automaton, &node);
        automaton.final_node = node;
    }
    if (status == SXML_STATUS_OK && automaton.node_count > 0) {
        void *memory = NULL;
        status = sxml_allocator_allocate_array(builder->allocator,
                                               (sxml_array_allocation_request){.element_count = automaton.node_count,
                                                                               .element_size = sizeof(uint32_t),
                                                                               .element_alignment = alignof(uint32_t)},
                                               &memory);
        automaton.marks = memory;
    }
    if (status == SXML_STATUS_OK) {
        automaton.mark = 1;
        automaton.closure_count = 0;
        automaton.stack_count = 0;
        status = sxml_exi_push_closure(builder, &automaton, 0);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_close(builder, &automaton);
    }
    uint32_t first = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_state_of_closure(builder, &automaton, &first);
    }
    for (uint32_t state = 0; state < automaton.state_count && status == SXML_STATUS_OK; state++) {
        status = sxml_exi_expand_state(builder, &automaton, state);
    }
    if (status == SXML_STATUS_OK) {
        *grammar = automaton.states[first].grammar;
    }
    sxml_exi_release_automaton(builder->allocator, &automaton);
    return status;
}

/* The oracle's grammar of an all group (8.5.4.1.8.3): one element grammar
   with EE and an SE (qname) of each element leading back to it. */
static sxml_status sxml_exi_build_all_group(sxml_exi_builder *builder, uint32_t group, uint32_t *grammar) {
    const sxml_schema_components *components = builder->components;
    sxml_status status = sxml_exi_new_grammar(builder, SXML_EXI_GRAMMAR_ELEMENT, grammar);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_add_production(builder, &(sxml_exi_schema_production){.event_kind = SXML_EXI_EVENT_END_ELEMENT,
                                                                                .datatype = SXML_EXI_NONE,
                                                                                .next = SXML_EXI_NONE,
                                                                                .element = SXML_EXI_NONE});
    }
    const sxml_schema_model_group *definition =
        sxml_schema_get_model_group(components, (sxml_schema_model_group_index){.value = group});
    const sxml_schema_index_view children = sxml_schema_get_list(components, definition->particles);
    for (size_t child_index = 0; child_index < children.index_count && status == SXML_STATUS_OK; child_index++) {
        const sxml_schema_particle *particle =
            sxml_schema_get_particle(components, (sxml_schema_particle_index){.value = children.indexes[child_index]});
        if (particle->term_kind != SXML_SCHEMA_TERM_ELEMENT_DECLARATION) {
            continue;
        }
        const sxml_schema_element_declaration *declaration = sxml_schema_get_element_declaration(
            components, (sxml_schema_element_declaration_index){.value = particle->term});
        sxml_exi_table_name name = {0};
        status = sxml_exi_find_component_name(builder, declaration->element_name, &name);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_add_production(builder, &(sxml_exi_schema_production){.event_kind = SXML_EXI_EVENT_START_ELEMENT,
                                                                                    .uri = name.uri,
                                                                                    .local_name = name.local_name,
                                                                                    .datatype = SXML_EXI_NONE,
                                                                                    .next = *grammar,
                                                                                    .element = particle->term});
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_commit(builder, *grammar);
    }
    return status;
}

/* ---------------------------------------------------------- Attributes */

/* An attribute use, sorted by its declaration's local-name then URI
   (8.5.4.1.3.2). */
typedef struct sxml_exi_attribute_entry {
    sxml_exi_name_entry name;
    sxml_exi_table_name table_name;
    uint32_t datatype;
    bool is_required;
} sxml_exi_attribute_entry;

static int sxml_exi_compare_attributes(const void *first, const void *second) {
    const sxml_exi_attribute_entry *left = first;
    const sxml_exi_attribute_entry *right = second;
    const int order = sxml_exi_view_order(left->name.local_name, right->name.local_name);
    return order != 0 ? order : sxml_exi_view_order(left->name.uri, right->name.uri);
}

/* The productions of an attribute wildcard looping to grammar: AT (*), or
   AT (uri : *) for each URI of its list (8.5.4.1.3.2). */
typedef struct sxml_exi_wildcard_loop {
    uint32_t wildcard;
    uint32_t grammar;
} sxml_exi_wildcard_loop;

static sxml_status sxml_exi_add_attribute_wildcard(sxml_exi_builder *builder, const sxml_exi_wildcard_loop *loop) {
    if (loop->wildcard == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_schema_wildcard *wildcard =
        sxml_schema_get_wildcard(builder->components, (sxml_schema_wildcard_index){.value = loop->wildcard});
    if (wildcard->namespace_constraint_variety != SXML_SCHEMA_NAMESPACE_CONSTRAINT_ENUMERATION) {
        return sxml_exi_add_production(builder, &(sxml_exi_schema_production){.event_kind = SXML_EXI_EVENT_ATTRIBUTE_ANY,
                                                                              .datatype = SXML_EXI_NONE,
                                                                              .next = loop->grammar,
                                                                              .element = SXML_EXI_NONE});
    }
    const sxml_schema_index_view namespaces = sxml_schema_get_list(builder->components, wildcard->namespaces);
    sxml_status status = SXML_STATUS_OK;
    for (size_t namespace_index = 0; namespace_index < namespaces.index_count && status == SXML_STATUS_OK;
         namespace_index++) {
        sxml_byte_view text = {0};
        uint32_t uri = 0;
        status = sxml_exi_namespace_text(builder, namespaces.indexes[namespace_index], &text);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_find_uri_index(builder, text, &uri);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_add_production(builder, &(sxml_exi_schema_production){.event_kind = SXML_EXI_EVENT_ATTRIBUTE_URI,
                                                                                    .uri = uri,
                                                                                    .datatype = SXML_EXI_NONE,
                                                                                    .next = loop->grammar,
                                                                                    .element = SXML_EXI_NONE});
        }
    }
    return status;
}

/* What the start tags of a type are made from: the content's first
   grammar, Element_{i,content2}, and the complex type whose attribute
   uses and wildcard they read (0 for a simple type). */
typedef struct sxml_exi_start_tag_request {
    uint32_t content;
    uint32_t element_content;
    uint32_t complex_type;
} sxml_exi_start_tag_request;

/* The sorted attribute uses of a complex type, in memory of the
   builder's allocator. */
typedef struct sxml_exi_attribute_list {
    sxml_exi_attribute_entry *entries;
    size_t entry_count;
} sxml_exi_attribute_list;

static sxml_status sxml_exi_sorted_attributes(sxml_exi_builder *builder, uint32_t complex_type,
                                              sxml_exi_attribute_list *list) {
    const sxml_schema_components *components = builder->components;
    const sxml_schema_type_definition *definition =
        sxml_schema_get_type_definition(components, (sxml_schema_type_definition_index){.value = complex_type});
    const sxml_schema_index_view uses = sxml_schema_get_list(components, definition->attribute_uses);
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(builder->allocator,
                                                       (sxml_array_allocation_request){.element_count = uses.index_count,
                                                                                       .element_size = sizeof(sxml_exi_attribute_entry),
                                                                                       .element_alignment = alignof(sxml_exi_attribute_entry)},
                                                       &memory);
    sxml_exi_attribute_entry *sorted = memory;
    size_t count = 0;
    for (size_t use_index = 0; use_index < uses.index_count && status == SXML_STATUS_OK; use_index++) {
        const sxml_schema_attribute_use *use =
            sxml_schema_get_attribute_use(components, (sxml_schema_attribute_use_index){.value = uses.indexes[use_index]});
        if (use->use_kind == SXML_SCHEMA_ATTRIBUTE_USE_PROHIBITED) {
            continue;
        }
        const sxml_schema_attribute_declaration *attribute =
            sxml_schema_get_attribute_declaration(components, use->attribute_declaration);
        sxml_exi_attribute_entry *entry = &sorted[count];
        *entry = (sxml_exi_attribute_entry){.is_required = use->use_kind == SXML_SCHEMA_ATTRIBUTE_USE_REQUIRED};
        status = sxml_exi_name_texts(builder, attribute->attribute_name, &entry->name);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_find_table_name(builder, entry->name, &entry->table_name);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_datatype_of(builder, sxml_exi_type_of(builder, attribute->type_definition.value),
                                          &entry->datatype);
        }
        count++;
    }
    if (status == SXML_STATUS_OK && count > 1) {
        qsort(sorted, count, sizeof sorted[0], sxml_exi_compare_attributes);
    }
    *list = (sxml_exi_attribute_list){.entries = sorted, .entry_count = count};
    return status;
}

/* The start tags of a type (the oracle's handleAttributes, then its first
   start tag): from the last attribute use back to the first, each with
   AT (qname) to the one after it, the wildcard's loops and, when the use
   is optional, what the one after it has (8.5.4.1.3.2, 8.5.4.1.4). */
static sxml_status sxml_exi_build_start_tags(sxml_exi_builder *builder, const sxml_exi_start_tag_request *request,
                                             uint32_t *first_start_tag) {
    uint32_t wildcard = 0;
    sxml_exi_attribute_list list = {0};
    sxml_status status = SXML_STATUS_OK;
    if (request->complex_type != 0) {
        const sxml_schema_type_definition *definition = sxml_schema_get_type_definition(
            builder->components, (sxml_schema_type_definition_index){.value = request->complex_type});
        wildcard = definition->attribute_wildcard.value;
        status = sxml_exi_sorted_attributes(builder, request->complex_type, &list);
    }
    uint32_t start = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_new_grammar(builder, SXML_EXI_GRAMMAR_START_TAG, &start);
    }
    if (status == SXML_STATUS_OK) {
        builder->grammars->grammars[start].element_content = request->element_content;
        status = sxml_exi_copy_productions(builder, &(sxml_exi_copy){.source = request->content, .redirect = SXML_EXI_NONE});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_add_attribute_wildcard(builder, &(sxml_exi_wildcard_loop){.wildcard = wildcard, .grammar = start});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_commit_start_tag(builder, start);
    }
    for (size_t attribute_index = list.entry_count; attribute_index > 0 && status == SXML_STATUS_OK; attribute_index--) {
        const sxml_exi_attribute_entry *attribute = &list.entries[attribute_index - 1];
        uint32_t current = 0;
        status = sxml_exi_new_grammar(builder, SXML_EXI_GRAMMAR_START_TAG, &current);
        if (status == SXML_STATUS_OK) {
            builder->grammars->grammars[current].element_content = request->element_content;
            status = sxml_exi_add_production(builder, &(sxml_exi_schema_production){.event_kind = SXML_EXI_EVENT_ATTRIBUTE,
                                                                                    .uri = attribute->table_name.uri,
                                                                                    .local_name = attribute->table_name.local_name,
                                                                                    .datatype = attribute->datatype,
                                                                                    .next = start,
                                                                                    .element = SXML_EXI_NONE});
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_add_attribute_wildcard(builder,
                                                     &(sxml_exi_wildcard_loop){.wildcard = wildcard, .grammar = current});
        }
        if (status == SXML_STATUS_OK && !attribute->is_required) {
            status = sxml_exi_copy_productions(
                builder, &(sxml_exi_copy){.source = start, .redirect = SXML_EXI_NONE, .skips_wildcards = true});
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_commit_start_tag(builder, current);
        }
        start = current;
    }
    sxml_allocator_release(builder->allocator, list.entries);
    uint32_t first = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_new_grammar(builder, SXML_EXI_GRAMMAR_FIRST_START_TAG, &first);
    }
    if (status == SXML_STATUS_OK) {
        builder->grammars->grammars[first].element_content = request->element_content;
        status = sxml_exi_copy_productions(builder, &(sxml_exi_copy){.source = start, .redirect = first});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_commit_start_tag(builder, first);
    }
    *first_start_tag = first;
    return status;
}

/* ------------------------------------------------------ Type grammars */

/* The element grammar of a simple content: a typed CH, then EE
   (8.5.4.1.3.1). */
static sxml_status sxml_exi_build_simple_content(sxml_exi_builder *builder, uint32_t datatype, uint32_t *grammar) {
    sxml_status status = sxml_exi_new_grammar(builder, SXML_EXI_GRAMMAR_ELEMENT, grammar);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_add_production(builder, &(sxml_exi_schema_production){.event_kind = SXML_EXI_EVENT_CHARACTERS,
                                                                                .datatype = datatype,
                                                                                .next = builder->grammars->empty_content,
                                                                                .element = SXML_EXI_NONE});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_commit(builder, *grammar);
    }
    return status;
}

/* xs:anyType's grammar (8.5.4.1.3.2 on the complex ur-type): any
   attributes, then mixed content of any elements. */
static sxml_status sxml_exi_build_any_type(sxml_exi_builder *builder, uint32_t *grammar) {
    uint32_t content = 0;
    uint32_t repeated = 0;
    sxml_status status = sxml_exi_new_grammar(builder, SXML_EXI_GRAMMAR_ELEMENT, &content);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_new_grammar(builder, SXML_EXI_GRAMMAR_ELEMENT, &repeated);
    }
    const uint32_t states[2] = {content, repeated};
    for (size_t state_index = 0; state_index < 2 && status == SXML_STATUS_OK; state_index++) {
        status = sxml_exi_add_production(builder, &(sxml_exi_schema_production){.event_kind = SXML_EXI_EVENT_START_ELEMENT_ANY,
                                                                                .datatype = SXML_EXI_NONE,
                                                                                .next = repeated,
                                                                                .element = SXML_EXI_NONE});
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_add_production(builder, &(sxml_exi_schema_production){.event_kind = SXML_EXI_EVENT_END_ELEMENT,
                                                                                    .datatype = SXML_EXI_NONE,
                                                                                    .next = SXML_EXI_NONE,
                                                                                    .element = SXML_EXI_NONE});
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_add_production(builder, &(sxml_exi_schema_production){.event_kind = SXML_EXI_EVENT_CHARACTERS,
                                                                                    .datatype = SXML_EXI_NONE,
                                                                                    .next = states[state_index],
                                                                                    .element = SXML_EXI_NONE});
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_commit(builder, states[state_index]);
        }
    }
    uint32_t start = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_new_grammar(builder, SXML_EXI_GRAMMAR_START_TAG, &start);
    }
    if (status == SXML_STATUS_OK) {
        builder->grammars->grammars[start].element_content = content;
        status = sxml_exi_copy_productions(builder, &(sxml_exi_copy){.source = content, .redirect = SXML_EXI_NONE});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_add_production(builder, &(sxml_exi_schema_production){.event_kind = SXML_EXI_EVENT_ATTRIBUTE_ANY,
                                                                                .datatype = SXML_EXI_NONE,
                                                                                .next = start,
                                                                                .element = SXML_EXI_NONE});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_commit_start_tag(builder, start);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_new_grammar(builder, SXML_EXI_GRAMMAR_FIRST_START_TAG, grammar);
    }
    if (status == SXML_STATUS_OK) {
        builder->grammars->grammars[*grammar].element_content = content;
        status = sxml_exi_copy_productions(builder, &(sxml_exi_copy){.source = start, .redirect = *grammar});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_commit_start_tag(builder, *grammar);
    }
    return status;
}

/* The type grammar Type_i of a type (8.5.4.1.3), made once per type. */
static uint32_t *sxml_exi_grammar_slot(sxml_exi_builder *builder, sxml_exi_type_reference type) {
    return type.is_builtin ? &builder->builtin_grammars[type.index] : &builder->type_grammars[type.index];
}

static sxml_status sxml_exi_type_grammar(sxml_exi_builder *builder, sxml_exi_type_reference type, uint32_t *grammar) {
    uint32_t *slot = sxml_exi_grammar_slot(builder, type);
    if (*slot != SXML_EXI_NONE) {
        *grammar = *slot;
        return SXML_STATUS_OK;
    }
    sxml_status status = SXML_STATUS_OK;
    uint32_t made = 0;
    const sxml_schema_type_definition *definition =
        type.is_builtin ? NULL
                        : sxml_schema_get_type_definition(builder->components,
                                                          (sxml_schema_type_definition_index){.value = type.index});
    if (type.is_builtin && type.index == SXML_EXI_BUILTIN_ANY_TYPE) {
        status = sxml_exi_build_any_type(builder, &made);
    } else if (definition == NULL || definition->variety != SXML_TYPE_VARIETY_COMPLEX) {
        uint32_t datatype = 0;
        uint32_t content = 0;
        status = sxml_exi_datatype_of(builder, type, &datatype);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_build_simple_content(builder, datatype, &content);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_build_start_tags(
                builder, &(sxml_exi_start_tag_request){.content = content, .element_content = content, .complex_type = 0},
                &made);
        }
    } else {
        uint32_t content = 0;
        switch (definition->content_type_variety) {
        case SXML_CONTENT_TYPE_VARIETY_EMPTY:
            status = sxml_exi_build_content(builder, &(sxml_exi_content_request){.particle = 0, .is_mixed = false},
                                            &content);
            break;
        case SXML_CONTENT_TYPE_VARIETY_SIMPLE: {
            uint32_t datatype = 0;
            status = sxml_exi_datatype_of(builder, sxml_exi_type_of(builder, definition->simple_content_type.value),
                                          &datatype);
            if (status == SXML_STATUS_OK) {
                status = sxml_exi_build_simple_content(builder, datatype, &content);
            }
            break;
        }
        case SXML_CONTENT_TYPE_VARIETY_ELEMENT_ONLY:
        case SXML_CONTENT_TYPE_VARIETY_MIXED: {
            const sxml_schema_particle *particle =
                sxml_schema_get_particle(builder->components, definition->particle);
            const sxml_schema_model_group *group =
                particle->term_kind == SXML_SCHEMA_TERM_MODEL_GROUP
                    ? sxml_schema_get_model_group(builder->components, (sxml_schema_model_group_index){.value = particle->term})
                    : NULL;
            if (group != NULL && group->compositor == SXML_SCHEMA_COMPOSITOR_ALL) {
                status = sxml_exi_build_all_group(builder, particle->term, &content);
            } else {
                status = sxml_exi_build_content(
                    builder,
                    &(sxml_exi_content_request){.particle = definition->particle.value,
                                                .is_mixed = definition->content_type_variety == SXML_CONTENT_TYPE_VARIETY_MIXED},
                    &content);
            }
            break;
        }
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_build_start_tags(
                builder,
                &(sxml_exi_start_tag_request){.content = content, .element_content = content, .complex_type = type.index},
                &made);
        }
    }
    if (status == SXML_STATUS_OK) {
        *sxml_exi_grammar_slot(builder, type) = made;
        *grammar = made;
    }
    return status;
}

/* Whether a type has named sub-types or is a union (8.5.4.4.2: the oracle
   gives such types xsi:type with strict). */
static bool sxml_exi_is_type_castable(const sxml_exi_builder *builder, sxml_exi_type_reference type) {
    if (type.is_builtin) {
        return builder->builtin_has_named_subtypes[type.index];
    }
    const sxml_schema_type_definition *definition =
        sxml_schema_get_type_definition(builder->components, (sxml_schema_type_definition_index){.value = type.index});
    return builder->type_has_named_subtypes[type.index] || definition->variety == SXML_TYPE_VARIETY_UNION;
}

static void sxml_exi_mark_named_subtypes(sxml_exi_builder *builder) {
    for (uint32_t builtin_index = 0; builtin_index < SXML_EXI_BUILTIN_COUNT; builtin_index++) {
        builder->builtin_has_named_subtypes[sxml_exi_builtin_types[builtin_index].base] = true;
    }
    for (uint32_t type_index = 1; type_index < builder->type_count; type_index++) {
        const sxml_schema_type_definition *definition =
            sxml_schema_get_type_definition(builder->components, (sxml_schema_type_definition_index){.value = type_index});
        if (definition->is_builtin || definition->type_name.index == 0) {
            continue;
        }
        const sxml_exi_type_reference base = sxml_exi_type_of(builder, definition->base_type.value);
        if (base.is_builtin) {
            builder->builtin_has_named_subtypes[base.index] = true;
        } else {
            builder->type_has_named_subtypes[base.index] = true;
        }
    }
}

/* The grammar of an element declaration (8.5.4.1.2): its type's, with
   strict's xsi:type and xsi:nil productions; a named type's grammar is
   copied when they differ, an anonymous type's given them. */
static sxml_status sxml_exi_declaration_grammar(sxml_exi_builder *builder, uint32_t element, uint32_t *grammar) {
    const sxml_schema_element_declaration *declaration =
        sxml_schema_get_element_declaration(builder->components, (sxml_schema_element_declaration_index){.value = element});
    const sxml_exi_type_reference type = sxml_exi_type_of(builder, declaration->type_definition.value);
    uint32_t type_grammar = 0;
    sxml_status status = sxml_exi_type_grammar(builder, type, &type_grammar);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool is_nillable = declaration->is_nillable;
    const bool is_castable = sxml_exi_is_type_castable(builder, type);
    const bool is_anonymous = !type.is_builtin &&
                              sxml_schema_get_type_definition(builder->components,
                                                              (sxml_schema_type_definition_index){.value = type.index})
                                      ->type_name.index == 0;
    if (is_anonymous || (!is_nillable && !is_castable)) {
        if (is_anonymous) {
            builder->grammars->grammars[type_grammar].is_nillable = is_nillable;
            builder->grammars->grammars[type_grammar].is_type_castable = is_castable;
        }
        *grammar = type_grammar;
        return SXML_STATUS_OK;
    }
    uint32_t copy = 0;
    status = sxml_exi_new_grammar(builder, SXML_EXI_GRAMMAR_FIRST_START_TAG, &copy);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_copy_productions(builder, &(sxml_exi_copy){.source = type_grammar, .redirect = copy});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_commit(builder, copy);
    }
    if (status == SXML_STATUS_OK) {
        sxml_exi_schema_grammar *made = &builder->grammars->grammars[copy];
        const sxml_exi_schema_grammar *source = &builder->grammars->grammars[type_grammar];
        made->element_content = source->element_content;
        made->type_empty = source->type_empty;
        made->is_nillable = is_nillable;
        made->is_type_castable = is_castable;
        *grammar = copy;
    }
    return status;
}

/* --------------------------------------------- Document and fragment */

/* A global element or a declared qname, to sort (8.5.1, 8.5.2). */
static int sxml_exi_compare_declared(const void *first, const void *second) {
    return sxml_exi_compare_members(first, second);
}

/* DocContent (8.5.1): SE (G_i) for the global elements sorted, SE (*);
   DocEnd: ED. FragmentContent (8.5.2): SE (F_i) for the unique qnames of
   every element declaration, SE (*), ED. */
static sxml_status sxml_exi_build_top_grammars(sxml_exi_builder *builder) {
    sxml_exi_schema_grammars *grammars = builder->grammars;
    sxml_status status = sxml_exi_new_grammar(builder, SXML_EXI_GRAMMAR_DOCUMENT_END, &grammars->document_end);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_add_production(builder, &(sxml_exi_schema_production){.event_kind = SXML_EXI_EVENT_END_DOCUMENT,
                                                                                .datatype = SXML_EXI_NONE,
                                                                                .next = SXML_EXI_NONE,
                                                                                .element = SXML_EXI_NONE});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_commit(builder, grammars->document_end);
    }
    const uint32_t element_count =
        builder->components == NULL ? 0 : builder->components->tables[SXML_SCHEMA_TABLE_ELEMENT_DECLARATION].entry_count;
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(builder->allocator,
                                               (sxml_array_allocation_request){.element_count = element_count,
                                                                               .element_size = sizeof(sxml_exi_member),
                                                                               .element_alignment = alignof(sxml_exi_member)},
                                               &memory);
    }
    sxml_exi_member *declared = memory;
    size_t global_count = 0;
    size_t declared_count = 0;
    for (uint32_t pass_index = 0; pass_index < 2 && status == SXML_STATUS_OK; pass_index++) {
        size_t count = 0;
        for (uint32_t element_index = 1; element_index < element_count && status == SXML_STATUS_OK; element_index++) {
            const sxml_schema_element_declaration *declaration = sxml_schema_get_element_declaration(
                builder->components, (sxml_schema_element_declaration_index){.value = element_index});
            if (pass_index == 0 && declaration->scope_variety != SXML_SCHEMA_SCOPE_GLOBAL) {
                continue;
            }
            declared[count].declaration = element_index;
            status = sxml_exi_name_texts(builder, declaration->element_name, &declared[count].name);
            count++;
        }
        if (status == SXML_STATUS_OK && count > 1) {
            qsort(declared, count, sizeof declared[0], sxml_exi_compare_declared);
        }
        const uint32_t top = pass_index == 0 ? SXML_EXI_GRAMMAR_DOCUMENT_CONTENT : SXML_EXI_GRAMMAR_FRAGMENT_CONTENT;
        uint32_t grammar = 0;
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_new_grammar(builder, top, &grammar);
        }
        const uint32_t next = pass_index == 0 ? grammars->document_end : grammar;
        for (size_t declared_index = 0; declared_index < count && status == SXML_STATUS_OK; declared_index++) {
            if (declared_index > 0 && sxml_exi_compare_members(&declared[declared_index], &declared[declared_index - 1]) == 0) {
                continue;
            }
            sxml_exi_table_name name = {0};
            status = sxml_exi_find_table_name(builder, declared[declared_index].name, &name);
            if (status == SXML_STATUS_OK) {
                status = sxml_exi_add_production(builder, &(sxml_exi_schema_production){.event_kind = SXML_EXI_EVENT_START_ELEMENT,
                                                                                        .uri = name.uri,
                                                                                        .local_name = name.local_name,
                                                                                        .datatype = SXML_EXI_NONE,
                                                                                        .next = next,
                                                                                        .element = declared[declared_index].declaration});
            }
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_add_production(builder, &(sxml_exi_schema_production){.event_kind = SXML_EXI_EVENT_START_ELEMENT_ANY,
                                                                                    .datatype = SXML_EXI_NONE,
                                                                                    .next = next,
                                                                                    .element = SXML_EXI_NONE});
        }
        if (status == SXML_STATUS_OK && pass_index == 1) {
            status = sxml_exi_add_production(builder, &(sxml_exi_schema_production){.event_kind = SXML_EXI_EVENT_END_DOCUMENT,
                                                                                    .datatype = SXML_EXI_NONE,
                                                                                    .next = SXML_EXI_NONE,
                                                                                    .element = SXML_EXI_NONE});
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_commit(builder, grammar);
        }
        if (pass_index == 0) {
            grammars->document_content = grammar;
            global_count = count;
        } else {
            grammars->fragment_content = grammar;
            declared_count = count;
        }
    }
    (void)global_count;
    (void)declared_count;
    sxml_allocator_release(builder->allocator, declared);
    return status;
}

/* ------------------------------------------------------------ The build */

/* The global entries of the string table's qnames (8.5.4.4: xsi:type's
   types, SE (*)'s global elements, AT (*)'s global attributes). */
static sxml_status sxml_exi_set_global_entries(sxml_exi_builder *builder) {
    sxml_status status = SXML_STATUS_OK;
    uint32_t xml_schema = 0;
    status = sxml_exi_find_uri_index(builder, sxml_exi_xml_schema_uri, &xml_schema);
    for (uint32_t builtin_index = 0; builtin_index < SXML_EXI_BUILTIN_COUNT && status == SXML_STATUS_OK; builtin_index++) {
        uint32_t grammar = 0;
        status = sxml_exi_type_grammar(builder, (sxml_exi_type_reference){.is_builtin = true, .index = builtin_index},
                                       &grammar);
        sxml_exi_table_name name = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_find_table_name(
                builder, (sxml_exi_name_entry){.uri = sxml_exi_xml_schema_uri, .local_name = sxml_exi_builtin_types[builtin_index].name},
                &name);
        }
        if (status == SXML_STATUS_OK) {
            builder->table->uris[name.uri].local_names[name.local_name].type_grammar = grammar;
        }
    }
    (void)xml_schema;
    if (builder->components == NULL) {
        return status;
    }
    for (uint32_t type_index = 1; type_index < builder->type_count && status == SXML_STATUS_OK; type_index++) {
        const sxml_schema_type_definition *definition =
            sxml_schema_get_type_definition(builder->components, (sxml_schema_type_definition_index){.value = type_index});
        if (definition->is_builtin || definition->type_name.index == 0) {
            continue;
        }
        uint32_t grammar = 0;
        status = sxml_exi_type_grammar(builder, (sxml_exi_type_reference){.is_builtin = false, .index = type_index},
                                       &grammar);
        sxml_exi_table_name name = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_find_component_name(builder, definition->type_name, &name);
        }
        if (status == SXML_STATUS_OK) {
            builder->table->uris[name.uri].local_names[name.local_name].type_grammar = grammar;
        }
    }
    sxml_exi_schema_grammars *grammars = builder->grammars;
    for (uint32_t element_index = 1; element_index < grammars->element_declaration_count && status == SXML_STATUS_OK;
         element_index++) {
        status = sxml_exi_declaration_grammar(builder, element_index, &grammars->element_grammars[element_index]);
        const sxml_schema_element_declaration *declaration = sxml_schema_get_element_declaration(
            builder->components, (sxml_schema_element_declaration_index){.value = element_index});
        if (status == SXML_STATUS_OK && declaration->scope_variety == SXML_SCHEMA_SCOPE_GLOBAL) {
            sxml_exi_table_name name = {0};
            status = sxml_exi_find_component_name(builder, declaration->element_name, &name);
            if (status == SXML_STATUS_OK) {
                builder->table->uris[name.uri].local_names[name.local_name].schema_element_grammar =
                    grammars->element_grammars[element_index];
            }
        }
    }
    const uint32_t attribute_count = builder->components->tables[SXML_SCHEMA_TABLE_ATTRIBUTE_DECLARATION].entry_count;
    for (uint32_t attribute_index = 5; attribute_index < attribute_count && status == SXML_STATUS_OK; attribute_index++) {
        const sxml_schema_attribute_declaration *declaration = sxml_schema_get_attribute_declaration(
            builder->components, (sxml_schema_attribute_declaration_index){.value = attribute_index});
        if (declaration->scope_variety != SXML_SCHEMA_SCOPE_GLOBAL) {
            continue;
        }
        uint32_t datatype = 0;
        status = sxml_exi_datatype_of(builder, sxml_exi_type_of(builder, declaration->type_definition.value), &datatype);
        sxml_exi_table_name name = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_find_component_name(builder, declaration->attribute_name, &name);
        }
        if (status == SXML_STATUS_OK) {
            builder->table->uris[name.uri].local_names[name.local_name].attribute_datatype = datatype;
        }
    }
    return status;
}

/* Allocates an array of count uint32_t set to SXML_EXI_NONE. */
typedef struct sxml_exi_index_array {
    size_t count;
    uint32_t **indexes;
} sxml_exi_index_array;

static sxml_status sxml_exi_allocate_indexes(sxml_allocator allocator, sxml_exi_index_array array) {
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        allocator,
        (sxml_array_allocation_request){.element_count = array.count, .element_size = sizeof(uint32_t), .element_alignment = alignof(uint32_t)},
        &memory);
    uint32_t *indexes = memory;
    for (size_t index = 0; status == SXML_STATUS_OK && index < array.count; index++) {
        indexes[index] = SXML_EXI_NONE;
    }
    *array.indexes = indexes;
    return status;
}

/* The builder's tables, and the grammars every stream has. */
static sxml_status sxml_exi_start_build(sxml_exi_builder *builder) {
    sxml_exi_schema_grammars *grammars = builder->grammars;
    for (uint32_t builtin_index = 0; builtin_index < SXML_EXI_BUILTIN_COUNT; builtin_index++) {
        builder->builtin_datatypes[builtin_index] = SXML_EXI_NONE;
        builder->builtin_grammars[builtin_index] = SXML_EXI_NONE;
    }
    sxml_status status = SXML_STATUS_OK;
    if (builder->components != NULL) {
        builder->type_count = builder->components->tables[SXML_SCHEMA_TABLE_TYPE_DEFINITION].entry_count;
        grammars->element_declaration_count =
            builder->components->tables[SXML_SCHEMA_TABLE_ELEMENT_DECLARATION].entry_count;
        status = sxml_exi_allocate_indexes(builder->allocator,
                                           (sxml_exi_index_array){.count = builder->type_count, .indexes = &builder->type_builtins});
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_allocate_indexes(
                builder->allocator, (sxml_exi_index_array){.count = builder->type_count, .indexes = &builder->type_datatypes});
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_allocate_indexes(
                builder->allocator, (sxml_exi_index_array){.count = builder->type_count, .indexes = &builder->type_grammars});
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_allocate_indexes(grammars->allocator,
                                               (sxml_exi_index_array){.count = grammars->element_declaration_count,
                                                                      .indexes = &grammars->element_grammars});
        }
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate_array(
                builder->allocator,
                (sxml_array_allocation_request){.element_count = builder->type_count, .element_size = sizeof(bool), .element_alignment = 1},
                &memory);
            builder->type_has_named_subtypes = memory;
        }
        for (uint32_t type_index = 1; type_index < builder->type_count && status == SXML_STATUS_OK; type_index++) {
            builder->type_builtins[type_index] = sxml_exi_builtin_of(builder, type_index);
        }
    }
    if (status == SXML_STATUS_OK) {
        sxml_exi_mark_named_subtypes(builder);
        status = sxml_exi_new_grammar(builder, SXML_EXI_GRAMMAR_ELEMENT, &grammars->empty_content);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_add_production(builder, &(sxml_exi_schema_production){.event_kind = SXML_EXI_EVENT_END_ELEMENT,
                                                                                .datatype = SXML_EXI_NONE,
                                                                                .next = SXML_EXI_NONE,
                                                                                .element = SXML_EXI_NONE});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_commit(builder, grammars->empty_content);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_datatype_of(
            builder, (sxml_exi_type_reference){.is_builtin = true, .index = 15 /* boolean */}, &grammars->boolean_datatype);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_datatype_of(builder, (sxml_exi_type_reference){.is_builtin = true, .index = SXML_EXI_BUILTIN_STRING},
                                      &grammars->string_datatype);
    }
    return status;
}

sxml_status sxml_exi_schema_grammars_build(const sxml_exi_schema_build *build, const sxml_exi_schema_setup *setup) {
    sxml_exi_schema_grammars *grammars = setup->grammars;
    *grammars = (sxml_exi_schema_grammars){.allocator = build->allocator,
                                           .options = build->options,
                                           .text = {.allocator = build->allocator},
                                           .document_content = SXML_EXI_NONE,
                                           .document_end = SXML_EXI_NONE,
                                           .fragment_content = SXML_EXI_NONE,
                                           .empty_content = SXML_EXI_NONE};
    sxml_exi_builder builder = {
        .build = build,
        .grammars = grammars,
        .table = setup->table,
        .allocator = build->allocator,
        .components = build->schema != NULL ? &build->schema->components : NULL,
        .name_pool = build->schema != NULL ? build->schema->processor->name_pool : NULL,
    };
    bool has_table = false;
    sxml_status status = SXML_STATUS_OK;
    if (builder.components != NULL) {
        builder.type_count = builder.components->tables[SXML_SCHEMA_TABLE_TYPE_DEFINITION].entry_count;
        status = sxml_exi_allocate_indexes(builder.allocator,
                                           (sxml_exi_index_array){.count = builder.type_count, .indexes = &builder.type_builtins});
        for (uint32_t type_index = 1; type_index < builder.type_count && status == SXML_STATUS_OK; type_index++) {
            builder.type_builtins[type_index] = sxml_exi_builtin_of(&builder, type_index);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_collect_names(&builder);
        }
        sxml_allocator_release(builder.allocator, builder.type_builtins);
        builder.type_builtins = NULL;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_initialize_table(&builder);
        has_table = status == SXML_STATUS_OK;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_start_build(&builder);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_set_global_entries(&builder);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_build_top_grammars(&builder);
    }
    sxml_allocator_release(builder.allocator, builder.type_builtins);
    sxml_allocator_release(builder.allocator, builder.type_datatypes);
    sxml_allocator_release(builder.allocator, builder.type_grammars);
    sxml_allocator_release(builder.allocator, builder.type_has_named_subtypes);
    sxml_allocator_release(builder.allocator, builder.scratch);
    sxml_allocator_release(builder.allocator, builder.names);
    if (status != SXML_STATUS_OK) {
        if (setup->failure_description != NULL) {
            *setup->failure_description = builder.failure_description;
        }
        sxml_exi_schema_grammars_finalize(grammars);
        if (has_table) {
            sxml_exi_string_table_finalize(setup->table);
        }
    }
    return status;
}

void sxml_exi_schema_grammars_finalize(sxml_exi_schema_grammars *grammars) {
    sxml_allocator_release(grammars->allocator, grammars->grammars);
    sxml_allocator_release(grammars->allocator, grammars->productions);
    sxml_allocator_release(grammars->allocator, grammars->datatypes);
    sxml_allocator_release(grammars->allocator, grammars->element_grammars);
    sxml_allocator_release(grammars->allocator, grammars->enumeration_values);
    sxml_allocator_release(grammars->allocator, grammars->characters);
    sxml_exi_buffer_release(&grammars->text);
    *grammars = (sxml_exi_schema_grammars){.allocator = grammars->allocator};
}

/* ---------------------------------------------------------- Event codes */

void sxml_exi_schema_undeclared_list(const sxml_exi_schema_grammars *grammars, uint32_t grammar,
                                     sxml_exi_undeclared_list *list) {
    const sxml_exi_options *options = grammars->options;
    const sxml_exi_schema_grammar *current = &grammars->grammars[grammar];
    const bool has_comments = options->fidelity.preserves_comments || options->fidelity.preserves_processing_instructions;
    *list = (sxml_exi_undeclared_list){0};
    switch (current->grammar_kind) {
    case SXML_EXI_GRAMMAR_DOCUMENT_CONTENT:
        if (options->fidelity.preserves_dtd) {
            list->undeclared[list->undeclared_count++] = SXML_EXI_UNDECLARED_DOCTYPE;
        }
        list->has_third_level = has_comments;
        return;
    case SXML_EXI_GRAMMAR_DOCUMENT_END:
    case SXML_EXI_GRAMMAR_FRAGMENT_CONTENT:
        list->has_third_level = has_comments;
        return;
    case SXML_EXI_GRAMMAR_FIRST_START_TAG:
    case SXML_EXI_GRAMMAR_START_TAG:
    case SXML_EXI_GRAMMAR_ELEMENT:
        break;
    }
    const bool is_first = current->grammar_kind == SXML_EXI_GRAMMAR_FIRST_START_TAG;
    const bool is_start_tag = is_first || current->grammar_kind == SXML_EXI_GRAMMAR_START_TAG;
    if (options->is_strict) {
        /* 8.5.4.4.2, as the oracle has it: xsi:type and xsi:nil both for a
           type with named sub-types or a union, xsi:nil for a nillable
           element. */
        if (is_first && current->is_type_castable) {
            list->undeclared[list->undeclared_count++] = SXML_EXI_UNDECLARED_TYPE;
            list->undeclared[list->undeclared_count++] = SXML_EXI_UNDECLARED_NIL;
        } else if (is_first && current->is_nillable) {
            list->undeclared[list->undeclared_count++] = SXML_EXI_UNDECLARED_NIL;
        }
        return;
    }
    /* 8.5.4.4.1. */
    if (!current->has_end_element) {
        list->undeclared[list->undeclared_count++] = SXML_EXI_UNDECLARED_END_ELEMENT;
    }
    if (is_first) {
        list->undeclared[list->undeclared_count++] = SXML_EXI_UNDECLARED_TYPE;
        list->undeclared[list->undeclared_count++] = SXML_EXI_UNDECLARED_NIL;
    }
    if (is_start_tag) {
        list->undeclared[list->undeclared_count++] = SXML_EXI_UNDECLARED_ATTRIBUTE_ANY;
        list->undeclared[list->undeclared_count++] = SXML_EXI_UNDECLARED_ATTRIBUTE_INVALID;
    }
    if (is_first && options->fidelity.preserves_prefixes) {
        list->undeclared[list->undeclared_count++] = SXML_EXI_UNDECLARED_NAMESPACE;
    }
    if (is_first && options->allows_self_contained) {
        list->undeclared[list->undeclared_count++] = SXML_EXI_UNDECLARED_SELF_CONTAINED;
    }
    list->undeclared[list->undeclared_count++] = SXML_EXI_UNDECLARED_START_ELEMENT_ANY;
    list->undeclared[list->undeclared_count++] = SXML_EXI_UNDECLARED_CHARACTERS;
    if (options->fidelity.preserves_dtd) {
        list->undeclared[list->undeclared_count++] = SXML_EXI_UNDECLARED_ENTITY_REFERENCE;
    }
    list->has_third_level = has_comments;
}

bool sxml_exi_schema_has_undeclared(const sxml_exi_schema_grammars *grammars, sxml_exi_undeclared_query query) {
    sxml_exi_undeclared_list list = {0};
    sxml_exi_schema_undeclared_list(grammars, query.grammar, &list);
    for (uint32_t undeclared_index = 0; undeclared_index < list.undeclared_count; undeclared_index++) {
        if (list.undeclared[undeclared_index] == query.undeclared) {
            return true;
        }
    }
    return false;
}

/* The counts that size a grammar's event code parts. */
typedef struct sxml_exi_code_counts {
    uint32_t first_count;
    uint32_t second_count;
    uint32_t third_count;
} sxml_exi_code_counts;

static sxml_exi_code_counts sxml_exi_code_counts_of(const sxml_exi_schema_grammars *grammars, uint32_t grammar,
                                                    const sxml_exi_undeclared_list *list) {
    const sxml_exi_options *options = grammars->options;
    const uint32_t second_count = list->undeclared_count + (list->has_third_level ? 1U : 0U);
    return (sxml_exi_code_counts){
        .first_count = grammars->grammars[grammar].production_count + (second_count > 0 ? 1U : 0U),
        .second_count = second_count,
        .third_count = (options->fidelity.preserves_comments ? 1U : 0U) +
                       (options->fidelity.preserves_processing_instructions ? 1U : 0U)};
}

sxml_status sxml_exi_schema_write_event_code(const sxml_exi_schema_grammars *grammars,
                                             const sxml_exi_schema_code_write *write, sxml_exi_bit_writer *writer) {
    sxml_exi_undeclared_list list = {0};
    sxml_exi_schema_undeclared_list(grammars, write->grammar, &list);
    const sxml_exi_code_counts counts = sxml_exi_code_counts_of(grammars, write->grammar, &list);
    const uint32_t production_count = grammars->grammars[write->grammar].production_count;
    const uint32_t first_width = sxml_exi_bit_width(counts.first_count);
    if (write->match.match_kind == SXML_EXI_SCHEMA_MATCH_DECLARED) {
        return sxml_exi_write_n_bit(writer, (sxml_exi_bits){.value = write->match.code, .bit_count = first_width});
    }
    uint32_t second = 0;
    uint32_t third = 0;
    bool has_third = false;
    switch (write->match.match_kind) {
    case SXML_EXI_SCHEMA_MATCH_UNDECLARED: {
        bool is_found = false;
        for (uint32_t undeclared_index = 0; undeclared_index < list.undeclared_count; undeclared_index++) {
            if (list.undeclared[undeclared_index] == write->match.undeclared) {
                second = undeclared_index;
                is_found = true;
                break;
            }
        }
        if (!is_found) {
            return SXML_STATUS_DYNAMIC_ERROR;
        }
        break;
    }
    case SXML_EXI_SCHEMA_MATCH_COMMENT:
    case SXML_EXI_SCHEMA_MATCH_PROCESSING_INSTRUCTION:
        if (!list.has_third_level) {
            return SXML_STATUS_DYNAMIC_ERROR;
        }
        second = list.undeclared_count;
        has_third = true;
        third = write->match.match_kind == SXML_EXI_SCHEMA_MATCH_PROCESSING_INSTRUCTION &&
                        grammars->options->fidelity.preserves_comments
                    ? 1U
                    : 0U;
        break;
    case SXML_EXI_SCHEMA_MATCH_DECLARED:
        break;
    }
    sxml_status status = sxml_exi_write_n_bit(writer, (sxml_exi_bits){.value = production_count, .bit_count = first_width});
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_n_bit(writer,
                                      (sxml_exi_bits){.value = second, .bit_count = sxml_exi_bit_width(counts.second_count)});
    }
    if (status == SXML_STATUS_OK && has_third) {
        status = sxml_exi_write_n_bit(writer, (sxml_exi_bits){.value = third, .bit_count = sxml_exi_bit_width(counts.third_count)});
    }
    if (status == SXML_STATUS_OK && write->match.match_kind == SXML_EXI_SCHEMA_MATCH_UNDECLARED &&
        write->match.undeclared == SXML_EXI_UNDECLARED_ATTRIBUTE_INVALID) {
        const uint32_t attribute_count = grammars->grammars[write->grammar].attribute_count;
        status = sxml_exi_write_n_bit(
            writer, (sxml_exi_bits){.value = write->match.attribute, .bit_count = sxml_exi_bit_width(attribute_count + 1)});
    }
    return status;
}

sxml_status sxml_exi_schema_read_event_code(const sxml_exi_schema_grammars *grammars,
                                            const sxml_exi_schema_code_read *read, sxml_exi_schema_match *match) {
    sxml_exi_undeclared_list list = {0};
    sxml_exi_schema_undeclared_list(grammars, read->grammar, &list);
    const sxml_exi_code_counts counts = sxml_exi_code_counts_of(grammars, read->grammar, &list);
    const uint32_t production_count = grammars->grammars[read->grammar].production_count;
    uint32_t first = 0;
    sxml_status status = sxml_exi_read_n_bit(
        read->reader, (sxml_exi_bits_request){.bit_count = sxml_exi_bit_width(counts.first_count)}, &first);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (first < production_count) {
        *match = (sxml_exi_schema_match){.match_kind = SXML_EXI_SCHEMA_MATCH_DECLARED, .code = first};
        return SXML_STATUS_OK;
    }
    if (first > production_count || counts.second_count == 0) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    uint32_t second = 0;
    status = sxml_exi_read_n_bit(read->reader,
                                 (sxml_exi_bits_request){.bit_count = sxml_exi_bit_width(counts.second_count)}, &second);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (second < list.undeclared_count) {
        *match = (sxml_exi_schema_match){.match_kind = SXML_EXI_SCHEMA_MATCH_UNDECLARED,
                                         .undeclared = list.undeclared[second]};
        if (list.undeclared[second] == SXML_EXI_UNDECLARED_ATTRIBUTE_INVALID) {
            const uint32_t attribute_count = grammars->grammars[read->grammar].attribute_count;
            status = sxml_exi_read_n_bit(
                read->reader, (sxml_exi_bits_request){.bit_count = sxml_exi_bit_width(attribute_count + 1)}, &match->attribute);
            if (status == SXML_STATUS_OK && match->attribute > attribute_count) {
                status = SXML_STATUS_MALFORMED_INPUT;
            }
        }
        return status;
    }
    if (second > list.undeclared_count || !list.has_third_level) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    uint32_t third = 0;
    status = sxml_exi_read_n_bit(read->reader,
                                 (sxml_exi_bits_request){.bit_count = sxml_exi_bit_width(counts.third_count)}, &third);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool has_comments = grammars->options->fidelity.preserves_comments;
    if (third >= counts.third_count) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    *match = (sxml_exi_schema_match){.match_kind = (third == 0 && has_comments) ? SXML_EXI_SCHEMA_MATCH_COMMENT
                                                                               : SXML_EXI_SCHEMA_MATCH_PROCESSING_INSTRUCTION};
    return SXML_STATUS_OK;
}
