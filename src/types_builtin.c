/*
 * The built-in types of the type registry (include/sxml/types.h,
 * docs/DECISIONS.md D32): static, immutable descriptions shared by every
 * registry, indexed by sxml_builtin_type.
 *
 * Imitates the static tables of the golden module src/name.c (its
 * well-known namespaces). The hierarchy follows XDM 4.0 section 4.1.2
 * ("Schema Type Hierarchy") and XSD 1.1 Part 2 sections 3.3 and 3.4; the
 * generic item types follow XPath 4.0 section 3.2.
 *
 * The descriptions of map(*), array(*), function(*) and jnode(*) also fill
 * the fields their is_any flag makes meaningless with the equivalent
 * explicit types (map(xs:anyAtomicType, item()*), array(item()*),
 * function() as item()*, jnode(*, item()*)), the normal form the registry
 * compares registered descriptions in (src/types.c).
 */
#include "types_internal.h"

/* ------------------------------------------------------ Initializer parts */

#define SXML_TYPE_REFERENCE(builtin_type) {.index = (uint32_t)(builtin_type)}

/* item()*: the sequence type of anything. */
#define SXML_TYPE_ANY_SEQUENCE                                                 \
    {.item_type = SXML_TYPE_REFERENCE(SXML_TYPE_ITEM),                         \
     .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}

/* A schema type without variety details: the simple ur-type, and the
   placeholder at SXML_TYPE_NONE. */
#define SXML_TYPE_SCHEMA_TYPE(type_variety, base)                              \
    {.variety = (type_variety),                                                \
     .type_name = {.index = 0},                                                \
     .base_type = SXML_TYPE_REFERENCE(base),                                   \
     .primitive_type = SXML_TYPE_REFERENCE(SXML_TYPE_NONE),                    \
     .is_builtin = true,                                                       \
     .variety_details = {.list_item_type =                                     \
                             SXML_TYPE_REFERENCE(SXML_TYPE_NONE)}}

/* A built-in complex type: xs:anyType and xs:untyped allow any content,
   so their content type is mixed (XSD 1.1 Part 1, 3.4.7; XDM 4.0,
   section 4.1.2; D165). */
#define SXML_TYPE_COMPLEX(base)                                                \
    {.variety = SXML_TYPE_VARIETY_COMPLEX,                                     \
     .type_name = {.index = 0},                                                \
     .base_type = SXML_TYPE_REFERENCE(base),                                   \
     .primitive_type = SXML_TYPE_REFERENCE(SXML_TYPE_NONE),                    \
     .is_builtin = true,                                                       \
     .variety_details = {                                                      \
         .complex_type = {.content_type_variety =                              \
                              SXML_CONTENT_TYPE_VARIETY_MIXED,                 \
                          .simple_content_type =                               \
                              SXML_TYPE_REFERENCE(SXML_TYPE_NONE)}}}

/* An atomic type with its base type and primitive type. */
#define SXML_TYPE_ATOMIC(base, primitive)                                      \
    {.variety = SXML_TYPE_VARIETY_ATOMIC,                                      \
     .type_name = {.index = 0},                                                \
     .base_type = SXML_TYPE_REFERENCE(base),                                   \
     .primitive_type = SXML_TYPE_REFERENCE(primitive),                         \
     .is_builtin = true,                                                       \
     .variety_details = {.list_item_type =                                     \
                             SXML_TYPE_REFERENCE(SXML_TYPE_NONE)}}

/* A primitive atomic type: derived from xs:anyAtomicType, its own
   primitive type (XSD 1.1 Part 2, section 3.3; XDM 4.0, section 4.1.2). */
#define SXML_TYPE_PRIMITIVE(primitive)                                         \
    SXML_TYPE_ATOMIC(SXML_TYPE_XS_ANY_ATOMIC_TYPE, primitive)

/* A built-in list type of the given item type. */
#define SXML_TYPE_LIST(item_type)                                              \
    {.variety = SXML_TYPE_VARIETY_LIST,                                        \
     .type_name = {.index = 0},                                                \
     .base_type = SXML_TYPE_REFERENCE(SXML_TYPE_XS_ANY_SIMPLE_TYPE),           \
     .primitive_type = SXML_TYPE_REFERENCE(SXML_TYPE_NONE),                    \
     .is_builtin = true,                                                       \
     .variety_details = {.list_item_type = SXML_TYPE_REFERENCE(item_type)}}

/* A built-in union type with the given members. */
#define SXML_TYPE_UNION(members, member_count)                                 \
    {.variety = SXML_TYPE_VARIETY_UNION,                                       \
     .type_name = {.index = 0},                                                \
     .base_type = SXML_TYPE_REFERENCE(SXML_TYPE_XS_ANY_SIMPLE_TYPE),           \
     .primitive_type = SXML_TYPE_REFERENCE(SXML_TYPE_NONE),                    \
     .is_builtin = true,                                                       \
     .variety_details = {.member_types = {.type_identifiers = (members),       \
                                          .type_identifier_count =             \
                                              (member_count)}}}

/* A generic item type: no name, no base type. The details are variadic
   because their braces hold commas. */
#define SXML_TYPE_GENERIC(type_variety, ...)                                   \
    {.variety = (type_variety),                                                \
     .type_name = {.index = 0},                                                \
     .base_type = SXML_TYPE_REFERENCE(SXML_TYPE_NONE),                         \
     .primitive_type = SXML_TYPE_REFERENCE(SXML_TYPE_NONE),                    \
     .is_builtin = true,                                                       \
     .variety_details = __VA_ARGS__}

/* The node test of a node kind without name, annotation or content. */
#define SXML_TYPE_NODE_KIND_TEST(kind)                                         \
    SXML_TYPE_GENERIC(                                                         \
        SXML_TYPE_VARIETY_NODE_TEST,                                           \
        {.node_test = {                                                        \
             .node_kind = (kind),                                              \
             .name_test = {.name_test_kind = SXML_NAME_TEST_KIND_ANY_NAME,     \
                           .expanded_name = {.index = 0},                      \
                           .namespace_uri = {.index = 0},                      \
                           .local_name = {.bytes = NULL, .byte_count = 0}},    \
             .type_annotation = SXML_TYPE_REFERENCE(SXML_TYPE_NONE),           \
             .allows_nilled = false,                                           \
             .is_schema_declaration = false,                                   \
             .document_element_test = SXML_TYPE_REFERENCE(SXML_TYPE_NONE)}})

/* --------------------------------------------------------- Union members */

/* xs:numeric is xs:double | xs:float | xs:decimal, in that order (XDM 4.0,
   section 4.1.3; the order decides casting). */
static const sxml_type_identifier sxml_type_numeric_members[] = {
    SXML_TYPE_REFERENCE(SXML_TYPE_XS_DOUBLE),
    SXML_TYPE_REFERENCE(SXML_TYPE_XS_FLOAT),
    SXML_TYPE_REFERENCE(SXML_TYPE_XS_DECIMAL),
};

/* ---------------------------------------------------------- Descriptions */

static const sxml_type_description
    sxml_type_builtin_descriptions[SXML_TYPE_BUILTIN_COUNT] = {
        [SXML_TYPE_NONE] = SXML_TYPE_SCHEMA_TYPE(SXML_TYPE_VARIETY_COMPLEX,
                                                 SXML_TYPE_NONE),

        [SXML_TYPE_XS_ANY_TYPE] = SXML_TYPE_COMPLEX(SXML_TYPE_NONE),
        [SXML_TYPE_XS_UNTYPED] = SXML_TYPE_COMPLEX(SXML_TYPE_XS_ANY_TYPE),
        [SXML_TYPE_XS_ANY_SIMPLE_TYPE] = SXML_TYPE_SCHEMA_TYPE(
            SXML_TYPE_VARIETY_ANY_SIMPLE, SXML_TYPE_XS_ANY_TYPE),
        [SXML_TYPE_XS_ANY_ATOMIC_TYPE] = SXML_TYPE_ATOMIC(
            SXML_TYPE_XS_ANY_SIMPLE_TYPE, SXML_TYPE_NONE),

        [SXML_TYPE_XS_UNTYPED_ATOMIC] =
            SXML_TYPE_PRIMITIVE(SXML_TYPE_XS_UNTYPED_ATOMIC),
        [SXML_TYPE_XS_STRING] = SXML_TYPE_PRIMITIVE(SXML_TYPE_XS_STRING),
        [SXML_TYPE_XS_BOOLEAN] = SXML_TYPE_PRIMITIVE(SXML_TYPE_XS_BOOLEAN),
        [SXML_TYPE_XS_DECIMAL] = SXML_TYPE_PRIMITIVE(SXML_TYPE_XS_DECIMAL),
        [SXML_TYPE_XS_FLOAT] = SXML_TYPE_PRIMITIVE(SXML_TYPE_XS_FLOAT),
        [SXML_TYPE_XS_DOUBLE] = SXML_TYPE_PRIMITIVE(SXML_TYPE_XS_DOUBLE),
        [SXML_TYPE_XS_DURATION] = SXML_TYPE_PRIMITIVE(SXML_TYPE_XS_DURATION),
        [SXML_TYPE_XS_DATE_TIME] = SXML_TYPE_PRIMITIVE(SXML_TYPE_XS_DATE_TIME),
        [SXML_TYPE_XS_TIME] = SXML_TYPE_PRIMITIVE(SXML_TYPE_XS_TIME),
        [SXML_TYPE_XS_DATE] = SXML_TYPE_PRIMITIVE(SXML_TYPE_XS_DATE),
        [SXML_TYPE_XS_G_YEAR_MONTH] =
            SXML_TYPE_PRIMITIVE(SXML_TYPE_XS_G_YEAR_MONTH),
        [SXML_TYPE_XS_G_YEAR] = SXML_TYPE_PRIMITIVE(SXML_TYPE_XS_G_YEAR),
        [SXML_TYPE_XS_G_MONTH_DAY] =
            SXML_TYPE_PRIMITIVE(SXML_TYPE_XS_G_MONTH_DAY),
        [SXML_TYPE_XS_G_DAY] = SXML_TYPE_PRIMITIVE(SXML_TYPE_XS_G_DAY),
        [SXML_TYPE_XS_G_MONTH] = SXML_TYPE_PRIMITIVE(SXML_TYPE_XS_G_MONTH),
        [SXML_TYPE_XS_HEX_BINARY] = SXML_TYPE_PRIMITIVE(SXML_TYPE_XS_HEX_BINARY),
        [SXML_TYPE_XS_BASE64_BINARY] =
            SXML_TYPE_PRIMITIVE(SXML_TYPE_XS_BASE64_BINARY),
        [SXML_TYPE_XS_ANY_URI] = SXML_TYPE_PRIMITIVE(SXML_TYPE_XS_ANY_URI),
        [SXML_TYPE_XS_QNAME] = SXML_TYPE_PRIMITIVE(SXML_TYPE_XS_QNAME),
        [SXML_TYPE_XS_NOTATION] = SXML_TYPE_PRIMITIVE(SXML_TYPE_XS_NOTATION),

        /* XSD 1.1 Part 2, section 3.4: the built-in derived types. */
        [SXML_TYPE_XS_INTEGER] =
            SXML_TYPE_ATOMIC(SXML_TYPE_XS_DECIMAL, SXML_TYPE_XS_DECIMAL),
        [SXML_TYPE_XS_NON_POSITIVE_INTEGER] =
            SXML_TYPE_ATOMIC(SXML_TYPE_XS_INTEGER, SXML_TYPE_XS_DECIMAL),
        [SXML_TYPE_XS_NEGATIVE_INTEGER] = SXML_TYPE_ATOMIC(
            SXML_TYPE_XS_NON_POSITIVE_INTEGER, SXML_TYPE_XS_DECIMAL),
        [SXML_TYPE_XS_LONG] =
            SXML_TYPE_ATOMIC(SXML_TYPE_XS_INTEGER, SXML_TYPE_XS_DECIMAL),
        [SXML_TYPE_XS_INT] =
            SXML_TYPE_ATOMIC(SXML_TYPE_XS_LONG, SXML_TYPE_XS_DECIMAL),
        [SXML_TYPE_XS_SHORT] =
            SXML_TYPE_ATOMIC(SXML_TYPE_XS_INT, SXML_TYPE_XS_DECIMAL),
        [SXML_TYPE_XS_BYTE] =
            SXML_TYPE_ATOMIC(SXML_TYPE_XS_SHORT, SXML_TYPE_XS_DECIMAL),
        [SXML_TYPE_XS_NON_NEGATIVE_INTEGER] =
            SXML_TYPE_ATOMIC(SXML_TYPE_XS_INTEGER, SXML_TYPE_XS_DECIMAL),
        [SXML_TYPE_XS_UNSIGNED_LONG] = SXML_TYPE_ATOMIC(
            SXML_TYPE_XS_NON_NEGATIVE_INTEGER, SXML_TYPE_XS_DECIMAL),
        [SXML_TYPE_XS_UNSIGNED_INT] =
            SXML_TYPE_ATOMIC(SXML_TYPE_XS_UNSIGNED_LONG, SXML_TYPE_XS_DECIMAL),
        [SXML_TYPE_XS_UNSIGNED_SHORT] =
            SXML_TYPE_ATOMIC(SXML_TYPE_XS_UNSIGNED_INT, SXML_TYPE_XS_DECIMAL),
        [SXML_TYPE_XS_UNSIGNED_BYTE] = SXML_TYPE_ATOMIC(
            SXML_TYPE_XS_UNSIGNED_SHORT, SXML_TYPE_XS_DECIMAL),
        [SXML_TYPE_XS_POSITIVE_INTEGER] = SXML_TYPE_ATOMIC(
            SXML_TYPE_XS_NON_NEGATIVE_INTEGER, SXML_TYPE_XS_DECIMAL),
        [SXML_TYPE_XS_NORMALIZED_STRING] =
            SXML_TYPE_ATOMIC(SXML_TYPE_XS_STRING, SXML_TYPE_XS_STRING),
        [SXML_TYPE_XS_TOKEN] =
            SXML_TYPE_ATOMIC(SXML_TYPE_XS_NORMALIZED_STRING, SXML_TYPE_XS_STRING),
        [SXML_TYPE_XS_LANGUAGE] =
            SXML_TYPE_ATOMIC(SXML_TYPE_XS_TOKEN, SXML_TYPE_XS_STRING),
        [SXML_TYPE_XS_NMTOKEN] =
            SXML_TYPE_ATOMIC(SXML_TYPE_XS_TOKEN, SXML_TYPE_XS_STRING),
        [SXML_TYPE_XS_NAME] =
            SXML_TYPE_ATOMIC(SXML_TYPE_XS_TOKEN, SXML_TYPE_XS_STRING),
        [SXML_TYPE_XS_NCNAME] =
            SXML_TYPE_ATOMIC(SXML_TYPE_XS_NAME, SXML_TYPE_XS_STRING),
        [SXML_TYPE_XS_ID] =
            SXML_TYPE_ATOMIC(SXML_TYPE_XS_NCNAME, SXML_TYPE_XS_STRING),
        [SXML_TYPE_XS_IDREF] =
            SXML_TYPE_ATOMIC(SXML_TYPE_XS_NCNAME, SXML_TYPE_XS_STRING),
        [SXML_TYPE_XS_ENTITY] =
            SXML_TYPE_ATOMIC(SXML_TYPE_XS_NCNAME, SXML_TYPE_XS_STRING),
        [SXML_TYPE_XS_DAY_TIME_DURATION] =
            SXML_TYPE_ATOMIC(SXML_TYPE_XS_DURATION, SXML_TYPE_XS_DURATION),
        [SXML_TYPE_XS_YEAR_MONTH_DURATION] =
            SXML_TYPE_ATOMIC(SXML_TYPE_XS_DURATION, SXML_TYPE_XS_DURATION),
        [SXML_TYPE_XS_DATE_TIME_STAMP] =
            SXML_TYPE_ATOMIC(SXML_TYPE_XS_DATE_TIME, SXML_TYPE_XS_DATE_TIME),

        [SXML_TYPE_XS_NMTOKENS] = SXML_TYPE_LIST(SXML_TYPE_XS_NMTOKEN),
        [SXML_TYPE_XS_IDREFS] = SXML_TYPE_LIST(SXML_TYPE_XS_IDREF),
        [SXML_TYPE_XS_ENTITIES] = SXML_TYPE_LIST(SXML_TYPE_XS_ENTITY),

        [SXML_TYPE_XS_NUMERIC] = SXML_TYPE_UNION(
            sxml_type_numeric_members,
            sizeof(sxml_type_numeric_members) /
                sizeof(sxml_type_numeric_members[0])),
        /* XPath 4.0, section 3.2.14: the union with no member types. */
        [SXML_TYPE_XS_ERROR] = SXML_TYPE_UNION(NULL, 0),

        [SXML_TYPE_ITEM] = SXML_TYPE_GENERIC(
            SXML_TYPE_VARIETY_ANY_ITEM,
            {.list_item_type = SXML_TYPE_REFERENCE(SXML_TYPE_NONE)}),
        /* node() and xnode() have the same description; node() also
           matches JNodes and is told apart by its identifier. */
        [SXML_TYPE_GENERIC_NODE] = SXML_TYPE_NODE_KIND_TEST(SXML_NODE_KIND_ANY),
        [SXML_TYPE_NODE] = SXML_TYPE_NODE_KIND_TEST(SXML_NODE_KIND_ANY),
        [SXML_TYPE_DOCUMENT_NODE] =
            SXML_TYPE_NODE_KIND_TEST(SXML_NODE_KIND_DOCUMENT),
        [SXML_TYPE_ELEMENT_NODE] =
            SXML_TYPE_NODE_KIND_TEST(SXML_NODE_KIND_ELEMENT),
        [SXML_TYPE_ATTRIBUTE_NODE] =
            SXML_TYPE_NODE_KIND_TEST(SXML_NODE_KIND_ATTRIBUTE),
        [SXML_TYPE_TEXT_NODE] = SXML_TYPE_NODE_KIND_TEST(SXML_NODE_KIND_TEXT),
        [SXML_TYPE_COMMENT_NODE] =
            SXML_TYPE_NODE_KIND_TEST(SXML_NODE_KIND_COMMENT),
        [SXML_TYPE_PROCESSING_INSTRUCTION_NODE] =
            SXML_TYPE_NODE_KIND_TEST(SXML_NODE_KIND_PROCESSING_INSTRUCTION),
        [SXML_TYPE_NAMESPACE_NODE] =
            SXML_TYPE_NODE_KIND_TEST(SXML_NODE_KIND_NAMESPACE),
        [SXML_TYPE_JSON_NODE] = SXML_TYPE_GENERIC(
            SXML_TYPE_VARIETY_JSON_NODE_TEST,
            {.json_node_test = {
                 .key_constraint = SXML_JSON_KEY_CONSTRAINT_ANY,
                 .key_type = SXML_TYPE_REFERENCE(SXML_TYPE_NONE),
                 .key_lexical_form = {.bytes = NULL, .byte_count = 0},
                 .has_value_type = false,
                 .value_type = SXML_TYPE_ANY_SEQUENCE}}),
        [SXML_TYPE_FUNCTION] = SXML_TYPE_GENERIC(
            SXML_TYPE_VARIETY_FUNCTION,
            {.function_type = {.is_any_function = true,
                               .parameter_types = {.sequence_types = NULL,
                                                   .sequence_type_count = 0},
                               .result_type = SXML_TYPE_ANY_SEQUENCE}}),
        [SXML_TYPE_MAP] = SXML_TYPE_GENERIC(
            SXML_TYPE_VARIETY_MAP,
            {.map_type = {.is_any_map = true,
                          .key_type = SXML_TYPE_REFERENCE(
                              SXML_TYPE_XS_ANY_ATOMIC_TYPE),
                          .value_type = SXML_TYPE_ANY_SEQUENCE}}),
        [SXML_TYPE_ARRAY] = SXML_TYPE_GENERIC(
            SXML_TYPE_VARIETY_ARRAY,
            {.array_type = {.is_any_array = true,
                            .member_type = SXML_TYPE_ANY_SEQUENCE}}),
        [SXML_TYPE_RECORD] = SXML_TYPE_GENERIC(
            SXML_TYPE_VARIETY_RECORD,
            {.record_type = {.is_any_record = true,
                             .is_nominative = false,
                             .record_fields = {.record_fields = NULL,
                                               .record_field_count = 0}}}),
};

/* ----------------------------------------------------------------- Names */

/* Local names in the XML Schema namespace; empty for unnamed types. */
static const sxml_byte_view
    sxml_type_builtin_local_names[SXML_TYPE_BUILTIN_COUNT] = {
        [SXML_TYPE_XS_ANY_TYPE] = SXML_BYTE_VIEW_INITIALIZER("anyType"),
        [SXML_TYPE_XS_UNTYPED] = SXML_BYTE_VIEW_INITIALIZER("untyped"),
        [SXML_TYPE_XS_ANY_SIMPLE_TYPE] =
            SXML_BYTE_VIEW_INITIALIZER("anySimpleType"),
        [SXML_TYPE_XS_ANY_ATOMIC_TYPE] =
            SXML_BYTE_VIEW_INITIALIZER("anyAtomicType"),
        [SXML_TYPE_XS_UNTYPED_ATOMIC] =
            SXML_BYTE_VIEW_INITIALIZER("untypedAtomic"),
        [SXML_TYPE_XS_STRING] = SXML_BYTE_VIEW_INITIALIZER("string"),
        [SXML_TYPE_XS_BOOLEAN] = SXML_BYTE_VIEW_INITIALIZER("boolean"),
        [SXML_TYPE_XS_DECIMAL] = SXML_BYTE_VIEW_INITIALIZER("decimal"),
        [SXML_TYPE_XS_FLOAT] = SXML_BYTE_VIEW_INITIALIZER("float"),
        [SXML_TYPE_XS_DOUBLE] = SXML_BYTE_VIEW_INITIALIZER("double"),
        [SXML_TYPE_XS_DURATION] = SXML_BYTE_VIEW_INITIALIZER("duration"),
        [SXML_TYPE_XS_DATE_TIME] = SXML_BYTE_VIEW_INITIALIZER("dateTime"),
        [SXML_TYPE_XS_TIME] = SXML_BYTE_VIEW_INITIALIZER("time"),
        [SXML_TYPE_XS_DATE] = SXML_BYTE_VIEW_INITIALIZER("date"),
        [SXML_TYPE_XS_G_YEAR_MONTH] = SXML_BYTE_VIEW_INITIALIZER("gYearMonth"),
        [SXML_TYPE_XS_G_YEAR] = SXML_BYTE_VIEW_INITIALIZER("gYear"),
        [SXML_TYPE_XS_G_MONTH_DAY] = SXML_BYTE_VIEW_INITIALIZER("gMonthDay"),
        [SXML_TYPE_XS_G_DAY] = SXML_BYTE_VIEW_INITIALIZER("gDay"),
        [SXML_TYPE_XS_G_MONTH] = SXML_BYTE_VIEW_INITIALIZER("gMonth"),
        [SXML_TYPE_XS_HEX_BINARY] = SXML_BYTE_VIEW_INITIALIZER("hexBinary"),
        [SXML_TYPE_XS_BASE64_BINARY] =
            SXML_BYTE_VIEW_INITIALIZER("base64Binary"),
        [SXML_TYPE_XS_ANY_URI] = SXML_BYTE_VIEW_INITIALIZER("anyURI"),
        [SXML_TYPE_XS_QNAME] = SXML_BYTE_VIEW_INITIALIZER("QName"),
        [SXML_TYPE_XS_NOTATION] = SXML_BYTE_VIEW_INITIALIZER("NOTATION"),
        [SXML_TYPE_XS_INTEGER] = SXML_BYTE_VIEW_INITIALIZER("integer"),
        [SXML_TYPE_XS_NON_POSITIVE_INTEGER] =
            SXML_BYTE_VIEW_INITIALIZER("nonPositiveInteger"),
        [SXML_TYPE_XS_NEGATIVE_INTEGER] =
            SXML_BYTE_VIEW_INITIALIZER("negativeInteger"),
        [SXML_TYPE_XS_LONG] = SXML_BYTE_VIEW_INITIALIZER("long"),
        [SXML_TYPE_XS_INT] = SXML_BYTE_VIEW_INITIALIZER("int"),
        [SXML_TYPE_XS_SHORT] = SXML_BYTE_VIEW_INITIALIZER("short"),
        [SXML_TYPE_XS_BYTE] = SXML_BYTE_VIEW_INITIALIZER("byte"),
        [SXML_TYPE_XS_NON_NEGATIVE_INTEGER] =
            SXML_BYTE_VIEW_INITIALIZER("nonNegativeInteger"),
        [SXML_TYPE_XS_UNSIGNED_LONG] =
            SXML_BYTE_VIEW_INITIALIZER("unsignedLong"),
        [SXML_TYPE_XS_UNSIGNED_INT] = SXML_BYTE_VIEW_INITIALIZER("unsignedInt"),
        [SXML_TYPE_XS_UNSIGNED_SHORT] =
            SXML_BYTE_VIEW_INITIALIZER("unsignedShort"),
        [SXML_TYPE_XS_UNSIGNED_BYTE] =
            SXML_BYTE_VIEW_INITIALIZER("unsignedByte"),
        [SXML_TYPE_XS_POSITIVE_INTEGER] =
            SXML_BYTE_VIEW_INITIALIZER("positiveInteger"),
        [SXML_TYPE_XS_NORMALIZED_STRING] =
            SXML_BYTE_VIEW_INITIALIZER("normalizedString"),
        [SXML_TYPE_XS_TOKEN] = SXML_BYTE_VIEW_INITIALIZER("token"),
        [SXML_TYPE_XS_LANGUAGE] = SXML_BYTE_VIEW_INITIALIZER("language"),
        [SXML_TYPE_XS_NMTOKEN] = SXML_BYTE_VIEW_INITIALIZER("NMTOKEN"),
        [SXML_TYPE_XS_NAME] = SXML_BYTE_VIEW_INITIALIZER("Name"),
        [SXML_TYPE_XS_NCNAME] = SXML_BYTE_VIEW_INITIALIZER("NCName"),
        [SXML_TYPE_XS_ID] = SXML_BYTE_VIEW_INITIALIZER("ID"),
        [SXML_TYPE_XS_IDREF] = SXML_BYTE_VIEW_INITIALIZER("IDREF"),
        [SXML_TYPE_XS_ENTITY] = SXML_BYTE_VIEW_INITIALIZER("ENTITY"),
        [SXML_TYPE_XS_DAY_TIME_DURATION] =
            SXML_BYTE_VIEW_INITIALIZER("dayTimeDuration"),
        [SXML_TYPE_XS_YEAR_MONTH_DURATION] =
            SXML_BYTE_VIEW_INITIALIZER("yearMonthDuration"),
        [SXML_TYPE_XS_DATE_TIME_STAMP] =
            SXML_BYTE_VIEW_INITIALIZER("dateTimeStamp"),
        [SXML_TYPE_XS_NMTOKENS] = SXML_BYTE_VIEW_INITIALIZER("NMTOKENS"),
        [SXML_TYPE_XS_IDREFS] = SXML_BYTE_VIEW_INITIALIZER("IDREFS"),
        [SXML_TYPE_XS_ENTITIES] = SXML_BYTE_VIEW_INITIALIZER("ENTITIES"),
        [SXML_TYPE_XS_NUMERIC] = SXML_BYTE_VIEW_INITIALIZER("numeric"),
        [SXML_TYPE_XS_ERROR] = SXML_BYTE_VIEW_INITIALIZER("error"),
};

/* ---------------------------------------------------------------- Access */

static bool sxml_type_is_builtin_index(sxml_builtin_type builtin_type) {
    return builtin_type > SXML_TYPE_NONE &&
           builtin_type < SXML_TYPE_BUILTIN_COUNT;
}

const sxml_type_description *sxml_type_builtin_description(
    sxml_builtin_type builtin_type) {
    if (!sxml_type_is_builtin_index(builtin_type)) {
        return NULL;
    }
    return &sxml_type_builtin_descriptions[builtin_type];
}

sxml_byte_view sxml_type_builtin_local_name(sxml_builtin_type builtin_type) {
    if (!sxml_type_is_builtin_index(builtin_type)) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return sxml_type_builtin_local_names[builtin_type];
}
