#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/export.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/status.h"
#include "sxml/view.h"

/*
 * The type registry: schema types, item types and sequence types
 * (docs/DECISIONS.md, D32).
 *
 * Every type sxml knows is named by an sxml_type_identifier, an index into
 * the type registry of a processor:
 *
 * - Built-in types have constant identifiers (sxml_builtin_type): the
 *   schema types of XDM 4.0 section 4.1.2 and 4.1.3, and the generic item
 *   types item(), node(), element(), function(*), map(*), array(*) and so
 *   on. Their descriptions are static const data shared by every registry.
 * - Types registered at run time get identifiers from
 *   SXML_TYPE_FIRST_REGISTERED up: item types written in a query or
 *   stylesheet (element(a), map(xs:string, item()), record(x, y),
 *   enum("a"), function(item()) as item(), (A | B)), nominative record
 *   types, and, from Phase 5, the types of imported schemas.
 *   Structural item types are interned: registering an equal description
 *   twice gives the same identifier, so item type equality is identifier
 *   equality. Nominative record types are never merged.
 *
 * A type identifier is what an atomic item, a node or a record carries as
 * its type annotation, and what the type checker reasons about. Schema types
 * and item types share one identifier space because XDM 4.0 section 4.1.2
 * says they overlap: atomic types are both.
 *
 * A sequence type is not registered: it is the plain value
 * sxml_sequence_type, an item type identifier plus an occurrence.
 *
 * Thread safety: a type registry is used by one thread at a time (D35). The
 * built-in descriptions are immutable static data, and the static inline
 * functions here are safe from any thread.
 */

/* ------------------------------------------------------ Type identifiers */

/*
 * The identifier of a type in a type registry. index 0 (SXML_TYPE_NONE) is
 * never a type; it marks an absent type where one is optional.
 */
typedef struct sxml_type_identifier {
    uint32_t index;
} sxml_type_identifier;

/*
 * The built-in types and their identifiers. The value of each constant is
 * the index of its sxml_type_identifier, the same in every registry and in
 * every version of sxml: the list only grows, at the end.
 *
 * Names follow the specification name, split into words: xs:gYearMonth is
 * SXML_TYPE_XS_G_YEAR_MONTH, xs:NCName is SXML_TYPE_XS_NCNAME.
 */
typedef enum sxml_builtin_type {
    SXML_TYPE_NONE = 0,

    /* Complex types (XDM 4.0, sections 4.1.2 and 4.1.3). */
    SXML_TYPE_XS_ANY_TYPE = 1,
    /* Annotation of an unvalidated element; base type xs:anyType. */
    SXML_TYPE_XS_UNTYPED = 2,

    /* The root of the simple types; base type xs:anyType. */
    SXML_TYPE_XS_ANY_SIMPLE_TYPE = 3,
    /* The root of the atomic types; base type xs:anySimpleType. */
    SXML_TYPE_XS_ANY_ATOMIC_TYPE = 4,

    /* The 20 primitive atomic types: the 19 of XSD 1.1 Part 2, section 3.3,
       and xs:untypedAtomic (XDM 4.0, section 5). Base type
       xs:anyAtomicType. */
    SXML_TYPE_XS_UNTYPED_ATOMIC = 5,
    SXML_TYPE_XS_STRING = 6,
    SXML_TYPE_XS_BOOLEAN = 7,
    SXML_TYPE_XS_DECIMAL = 8,
    SXML_TYPE_XS_FLOAT = 9,
    SXML_TYPE_XS_DOUBLE = 10,
    SXML_TYPE_XS_DURATION = 11,
    SXML_TYPE_XS_DATE_TIME = 12,
    SXML_TYPE_XS_TIME = 13,
    SXML_TYPE_XS_DATE = 14,
    SXML_TYPE_XS_G_YEAR_MONTH = 15,
    SXML_TYPE_XS_G_YEAR = 16,
    SXML_TYPE_XS_G_MONTH_DAY = 17,
    SXML_TYPE_XS_G_DAY = 18,
    SXML_TYPE_XS_G_MONTH = 19,
    SXML_TYPE_XS_HEX_BINARY = 20,
    SXML_TYPE_XS_BASE64_BINARY = 21,
    SXML_TYPE_XS_ANY_URI = 22,
    SXML_TYPE_XS_QNAME = 23,
    SXML_TYPE_XS_NOTATION = 24,

    /* Built-in derived atomic types (XSD 1.1 Part 2, section 3.4). The
       base type follows each one. */
    SXML_TYPE_XS_INTEGER = 25,              /* xs:decimal */
    SXML_TYPE_XS_NON_POSITIVE_INTEGER = 26, /* xs:integer */
    SXML_TYPE_XS_NEGATIVE_INTEGER = 27,     /* xs:nonPositiveInteger */
    SXML_TYPE_XS_LONG = 28,                 /* xs:integer */
    SXML_TYPE_XS_INT = 29,                  /* xs:long */
    SXML_TYPE_XS_SHORT = 30,                /* xs:int */
    SXML_TYPE_XS_BYTE = 31,                 /* xs:short */
    SXML_TYPE_XS_NON_NEGATIVE_INTEGER = 32, /* xs:integer */
    SXML_TYPE_XS_UNSIGNED_LONG = 33,        /* xs:nonNegativeInteger */
    SXML_TYPE_XS_UNSIGNED_INT = 34,         /* xs:unsignedLong */
    SXML_TYPE_XS_UNSIGNED_SHORT = 35,       /* xs:unsignedInt */
    SXML_TYPE_XS_UNSIGNED_BYTE = 36,        /* xs:unsignedShort */
    SXML_TYPE_XS_POSITIVE_INTEGER = 37,     /* xs:nonNegativeInteger */
    SXML_TYPE_XS_NORMALIZED_STRING = 38,    /* xs:string */
    SXML_TYPE_XS_TOKEN = 39,                /* xs:normalizedString */
    SXML_TYPE_XS_LANGUAGE = 40,             /* xs:token */
    SXML_TYPE_XS_NMTOKEN = 41,              /* xs:token */
    SXML_TYPE_XS_NAME = 42,                 /* xs:token */
    SXML_TYPE_XS_NCNAME = 43,               /* xs:Name */
    SXML_TYPE_XS_ID = 44,                   /* xs:NCName */
    SXML_TYPE_XS_IDREF = 45,                /* xs:NCName */
    SXML_TYPE_XS_ENTITY = 46,               /* xs:NCName */
    SXML_TYPE_XS_DAY_TIME_DURATION = 47,    /* xs:duration */
    SXML_TYPE_XS_YEAR_MONTH_DURATION = 48,  /* xs:duration */
    SXML_TYPE_XS_DATE_TIME_STAMP = 49,      /* xs:dateTime */

    /* Built-in list types; base type xs:anySimpleType. */
    SXML_TYPE_XS_NMTOKENS = 50, /* list of xs:NMTOKEN */
    SXML_TYPE_XS_IDREFS = 51,   /* list of xs:IDREF */
    SXML_TYPE_XS_ENTITIES = 52, /* list of xs:ENTITY */

    /* Built-in union types; base type xs:anySimpleType. */
    /* xs:double | xs:float | xs:decimal (XDM 4.0, section 4.1.3). */
    SXML_TYPE_XS_NUMERIC = 53,
    /* The union with no members: matches no item (XPath 4.0, 3.2.14). */
    SXML_TYPE_XS_ERROR = 54,

    /* Generic item types (XPath 4.0, section 3.2). They have no name and no
       base type; the subtype relation between them is fixed. */
    SXML_TYPE_ITEM = 55,                   /* item() */
    SXML_TYPE_GENERIC_NODE = 56,           /* node(), = xnode() | jnode() */
    SXML_TYPE_NODE = 57,                   /* xnode() */
    SXML_TYPE_DOCUMENT_NODE = 58,          /* document-node() */
    SXML_TYPE_ELEMENT_NODE = 59,           /* element() */
    SXML_TYPE_ATTRIBUTE_NODE = 60,         /* attribute() */
    SXML_TYPE_TEXT_NODE = 61,              /* text() */
    SXML_TYPE_COMMENT_NODE = 62,           /* comment() */
    SXML_TYPE_PROCESSING_INSTRUCTION_NODE = 63, /* processing-instruction() */
    SXML_TYPE_NAMESPACE_NODE = 64,         /* namespace-node() */
    SXML_TYPE_JSON_NODE = 65,              /* jnode() */
    SXML_TYPE_FUNCTION = 66,               /* function(*) */
    SXML_TYPE_MAP = 67,                    /* map(*) */
    SXML_TYPE_ARRAY = 68,                  /* array(*) */
    SXML_TYPE_RECORD = 69,                 /* record(*) */

    /* One more than the largest built-in identifier in this version. */
    SXML_TYPE_BUILTIN_COUNT = 70,
    /* The first identifier given to a registered type. Identifiers between
       SXML_TYPE_BUILTIN_COUNT and this are reserved for later built-in
       types. */
    SXML_TYPE_FIRST_REGISTERED = 512,
} sxml_builtin_type;

/* The identifier of a built-in type. */
static inline sxml_type_identifier sxml_builtin_type_identifier(
    sxml_builtin_type builtin_type) {
    return (sxml_type_identifier){.index = (uint32_t)builtin_type};
}

/* True when first and second name the same type. */
static inline bool sxml_type_identifier_equals(sxml_type_identifier first,
                                               sxml_type_identifier second) {
    return first.index == second.index;
}

/* --------------------------------------------------------- Sequence types */

/*
 * How many items a sequence type allows (XPath 4.0, section 3.1). The
 * values are bit sets of the allowed counts: bit 0 "no item", bit 1 "one
 * item", bit 2 "more than one item", so a subtype test on occurrences is a
 * subset test.
 */
typedef enum sxml_occurrence {
    /* empty-sequence(): the item type is ignored. */
    SXML_OCCURRENCE_ZERO = 1,
    /* T */
    SXML_OCCURRENCE_EXACTLY_ONE = 2,
    /* T? */
    SXML_OCCURRENCE_ZERO_OR_ONE = 3,
    /* T+ */
    SXML_OCCURRENCE_ONE_OR_MORE = 6,
    /* T* */
    SXML_OCCURRENCE_ZERO_OR_MORE = 7,
} sxml_occurrence;

/*
 * A sequence type: an item type and an occurrence. A plain value, valid with
 * the registry that issued item_type.
 */
typedef struct sxml_sequence_type {
    sxml_type_identifier item_type;
    sxml_occurrence occurrence;
} sxml_sequence_type;

/* ---------------------------------------------------- Type descriptions */

/* What kind of type an identifier names. */
typedef enum sxml_type_variety {
    /* A complex schema type: xs:anyType, xs:untyped, or one from a schema. */
    SXML_TYPE_VARIETY_COMPLEX,
    /* xs:anySimpleType, the one simple type that is neither atomic, list
       nor union. */
    SXML_TYPE_VARIETY_ANY_SIMPLE,
    /* An atomic type: xs:anyAtomicType, a primitive type, or one derived
       from them by restriction. Atomic types are also item types. */
    SXML_TYPE_VARIETY_ATOMIC,
    /* A list type (a schema type, not an item type). */
    SXML_TYPE_VARIETY_LIST,
    /* A union type. A union of atomic types is also an item type
       (XPath 4.0, section 3.2.3). */
    SXML_TYPE_VARIETY_UNION,
    /* item() */
    SXML_TYPE_VARIETY_ANY_ITEM,
    /* node(), xnode() or a node test such as element(a, xs:int). */
    SXML_TYPE_VARIETY_NODE_TEST,
    /* jnode(key, value type). */
    SXML_TYPE_VARIETY_JSON_NODE_TEST,
    /* function(*) or function(T1, T2) as R. */
    SXML_TYPE_VARIETY_FUNCTION,
    /* map(*) or map(K, V). */
    SXML_TYPE_VARIETY_MAP,
    /* array(*) or array(T). */
    SXML_TYPE_VARIETY_ARRAY,
    /* record(*), a structural or a nominative record type. */
    SXML_TYPE_VARIETY_RECORD,
    /* enum("a", "b"). */
    SXML_TYPE_VARIETY_ENUMERATION,
    /* (A | B | C). */
    SXML_TYPE_VARIETY_CHOICE,
} sxml_type_variety;

/* A read-only list of type identifiers. */
typedef struct sxml_type_identifier_view {
    const sxml_type_identifier *type_identifiers;
    size_t type_identifier_count;
} sxml_type_identifier_view;

/* A read-only list of sequence types. */
typedef struct sxml_sequence_type_view {
    const sxml_sequence_type *sequence_types;
    size_t sequence_type_count;
} sxml_sequence_type_view;

/* A read-only list of strings, each a UTF-8 byte view. */
typedef struct sxml_byte_view_list {
    const sxml_byte_view *byte_views;
    size_t byte_view_count;
} sxml_byte_view_list;

/*
 * Node kinds, as matched by node tests and reported by trees (XDM 4.0,
 * section 7.5). SXML_NODE_KIND_ANY is used only in node tests, for node()
 * and xnode().
 */
typedef enum sxml_node_kind {
    SXML_NODE_KIND_DOCUMENT,
    SXML_NODE_KIND_ELEMENT,
    SXML_NODE_KIND_ATTRIBUTE,
    SXML_NODE_KIND_TEXT,
    SXML_NODE_KIND_COMMENT,
    SXML_NODE_KIND_PROCESSING_INSTRUCTION,
    SXML_NODE_KIND_NAMESPACE,
    SXML_NODE_KIND_ANY,
} sxml_node_kind;

/*
 * Which names a name test matches (XPath 4.0, NameTest and Wildcard in
 * section 4.7 "Path Expressions"; the same NameTest is used in the element
 * and attribute tests of section 3.2.7 and in the NameTestUnion of
 * element(a | p:*)).
 */
typedef enum sxml_name_test_kind {
    /* "*": any name. */
    SXML_NAME_TEST_KIND_ANY_NAME,
    /* An EQName: exactly expanded_name. */
    SXML_NAME_TEST_KIND_EXPANDED_NAME,
    /* "prefix:*" or "Q{uri}*": any local name in namespace_uri
       (SXML_NAMESPACE_NONE for "Q{}*"). */
    SXML_NAME_TEST_KIND_NAMESPACE_WILDCARD,
    /* "*:local": local_name in any namespace or in none. */
    SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD,
} sxml_name_test_kind;

/*
 * A name test. Only the fields its kind names are meaningful. A plain
 * value; when it is part of a registered item type, the registry copies
 * local_name.
 */
typedef struct sxml_name_test {
    sxml_name_test_kind name_test_kind;
    sxml_name_identifier expanded_name;
    sxml_namespace_uri_identifier namespace_uri;
    /* For SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD: the local name, an
       NCName in UTF-8. */
    sxml_byte_view local_name;
} sxml_name_test;

/*
 * The parts of a node test (XPath 4.0, section 3.2.7):
 * element(name, type?), attribute(name, type), document-node(element(...)),
 * processing-instruction(name), schema-element(name), and the simple kind
 * tests. A NameTestUnion such as element(a | b) is registered as a choice of
 * node tests.
 */
typedef struct sxml_node_test {
    sxml_node_kind node_kind;
    /* The names the node may have; SXML_NAME_TEST_KIND_ANY_NAME for a test
       without a name. A processing-instruction test with a target is an
       SXML_NAME_TEST_KIND_EXPANDED_NAME test of a name in no namespace
       whose local name is the target. A namespace test may have any name
       test (namespace::x on the namespace axis): a namespace node's name
       is its prefix, in no namespace (D77). */
    sxml_name_test name_test;
    /* The required type annotation (the node must be annotated with it or a
       type derived from it); SXML_TYPE_NONE when unconstrained. */
    sxml_type_identifier type_annotation;
    /* element(N, T?): nilled elements match too. */
    bool allows_nilled;
    /* schema-element(N) or schema-attribute(N): matched through the
       substitution group of the declaration named by name_test, which is
       then an SXML_NAME_TEST_KIND_EXPANDED_NAME test (Phase 5). */
    bool is_schema_declaration;
    /* For SXML_NODE_KIND_DOCUMENT: a node test for the document element,
       SXML_TYPE_NONE when unconstrained. */
    sxml_type_identifier document_element_test;
} sxml_node_test;

/*
 * jnode(key, value type) (XPath 4.0, section 3.2.12). The key constraint
 * is a constant atomic item, which the type registry cannot hold without
 * the item heap; it is kept as the type registry's own copy of its
 * lexical form and type.
 */
typedef enum sxml_json_key_constraint {
    /* jnode(*): any key, or none. */
    SXML_JSON_KEY_CONSTRAINT_ANY,
    /* jnode(()): the root of a JTree, which has no key. */
    SXML_JSON_KEY_CONSTRAINT_ROOT,
    /* jnode(constant): the key is atomic-equal to the constant. */
    SXML_JSON_KEY_CONSTRAINT_CONSTANT,
} sxml_json_key_constraint;

typedef struct sxml_json_node_test {
    sxml_json_key_constraint key_constraint;
    /* For SXML_JSON_KEY_CONSTRAINT_CONSTANT: the atomic type of the
       constant (xs:string, xs:integer, xs:decimal, xs:double, xs:QName or
       xs:boolean) and its canonical lexical form. A QName is given as an
       EQName, Q{uri}local. */
    sxml_type_identifier key_type;
    sxml_byte_view key_lexical_form;
    /* Whether value_type constrains ·jvalue·. */
    bool has_value_type;
    sxml_sequence_type value_type;
} sxml_json_node_test;

/* function(parameter types) as result type; function(*) has
   is_any_function set. */
typedef struct sxml_function_type {
    bool is_any_function;
    sxml_sequence_type_view parameter_types;
    sxml_sequence_type result_type;
} sxml_function_type;

/* map(key type, value type); map(*) has is_any_map set. */
typedef struct sxml_map_type {
    bool is_any_map;
    /* An atomic or union type identifier. */
    sxml_type_identifier key_type;
    sxml_sequence_type value_type;
} sxml_map_type;

/* array(member type); array(*) has is_any_array set. */
typedef struct sxml_array_type {
    bool is_any_array;
    sxml_sequence_type member_type;
} sxml_array_type;

/*
 * One field of a record type: a name (any string) and a sequence type. A
 * field declared without "as" has sequence type item()*.
 */
typedef struct sxml_record_field {
    sxml_byte_view field_name;
    sxml_sequence_type field_type;
} sxml_record_field;

typedef struct sxml_record_field_view {
    const sxml_record_field *record_fields;
    size_t record_field_count;
} sxml_record_field_view;

/*
 * A record type (XPath 4.0, section 3.2.10). record(*) has is_any_record
 * set. A nominative record type has a name and is_nominative set; a
 * structural one has neither.
 */
typedef struct sxml_record_type {
    bool is_any_record;
    bool is_nominative;
    /* In the order of declaration, which is the entry order of every record
       of this type (XDM 4.0, section 8.3). */
    sxml_record_field_view record_fields;
} sxml_record_type;

/*
 * Everything the registry knows about one type. Views point into the
 * registry (or into static data for built-in types) and are valid until the
 * registry is finalized. Only the part named by variety is meaningful.
 */
typedef struct sxml_type_description {
    sxml_type_variety variety;
    /* The type's expanded name; 0 for an anonymous schema type and for an
       item type written in place (element(a), map(*)). */
    sxml_name_identifier type_name;
    /* For schema types: the type this one is derived from; SXML_TYPE_NONE
       for xs:anyType and for item types that are not schema types. */
    sxml_type_identifier base_type;
    /* For atomic types: the primitive type (itself for a primitive type;
       SXML_TYPE_NONE for xs:anyAtomicType). */
    sxml_type_identifier primitive_type;
    /* True for a built-in type. */
    bool is_builtin;
    union {
        /* SXML_TYPE_VARIETY_LIST: the item type of the list. */
        sxml_type_identifier list_item_type;
        /* SXML_TYPE_VARIETY_UNION and SXML_TYPE_VARIETY_CHOICE: the
           members, in order. */
        sxml_type_identifier_view member_types;
        sxml_node_test node_test;
        sxml_json_node_test json_node_test;
        sxml_function_type function_type;
        sxml_map_type map_type;
        sxml_array_type array_type;
        sxml_record_type record_type;
        /* SXML_TYPE_VARIETY_ENUMERATION: the strings, in the order written,
           without duplicates. */
        sxml_byte_view_list enumeration_values;
    } variety_details;
} sxml_type_description;

/* ------------------------------------------------------------- Registry */

typedef struct sxml_type_registry sxml_type_registry;

typedef struct sxml_type_registry_configuration {
    /* Provides memory for registered types. The null allocator gives a
       registry of the built-in types only, where registering fails with
       SXML_STATUS_CAPACITY_EXCEEDED. Must outlive the registry. */
    sxml_allocator allocator;
    /* The pool type names are interned into. The registry keeps a
       reference; name_pool must outlive it. The built-in type names are
       interned at initialization. */
    sxml_name_pool *name_pool;
    /* Most types that may be registered, a bound for untrusted input; 0
       means no bound. Reaching it gives SXML_STATUS_LIMIT_EXCEEDED. */
    uint32_t maximum_registered_type_count;
} sxml_type_registry_configuration;

/*
 * Writes the memory requirements of a type registry to *requirements. They
 * do not include memory for registered types. Returns
 * SXML_STATUS_INVALID_ARGUMENT for an invalid configuration (no name pool).
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_type_registry_memory_requirements(
    const sxml_type_registry_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places a type registry in memory and writes its address to *registry. It
 * knows every built-in type. memory must stay valid, unmoved, until
 * sxml_type_registry_finalize.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for an invalid configuration,
 * SXML_STATUS_INSUFFICIENT_MEMORY or SXML_STATUS_MISALIGNED_MEMORY when
 * memory does not meet the memory requirements, and the failures of
 * sxml_name_pool_intern_name for the built-in type names.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_type_registry_initialize(
    sxml_memory_view memory,
    const sxml_type_registry_configuration *configuration,
    sxml_type_registry **registry);

/*
 * Releases the memory of registered types. Every identifier from
 * SXML_TYPE_FIRST_REGISTERED up, and every view the registry handed out,
 * becomes invalid. Does nothing when registry is NULL.
 */
SXML_EXPORT void sxml_type_registry_finalize(sxml_type_registry *registry);

/*
 * Allocates a type registry through allocator and initializes it. allocator
 * is stored and must outlive the registry. Returns the failures of
 * sxml_type_registry_initialize and sxml_allocator_allocate.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_type_registry_create(
    sxml_allocator allocator,
    const sxml_type_registry_configuration *configuration,
    sxml_type_registry **registry);

/*
 * Finalizes a registry made by sxml_type_registry_create and releases its
 * memory. Does nothing when registry is NULL.
 */
SXML_EXPORT void sxml_type_registry_destroy(sxml_type_registry *registry);

/*
 * Writes the description of type to *description. Returns
 * SXML_STATUS_INDEX_OUT_OF_RANGE for an identifier the registry did not
 * issue (SXML_TYPE_NONE included). O(1).
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_type_registry_describe_type(
    const sxml_type_registry *registry, sxml_type_identifier type,
    sxml_type_description *description);

/*
 * Finds the named schema type, named item type or nominative record type
 * type_name and writes its identifier to *type. Returns false, leaving
 * *type untouched, when there is none. O(1) average.
 */
SXML_EXPORT bool sxml_type_registry_find_type(
    const sxml_type_registry *registry, sxml_name_identifier type_name,
    sxml_type_identifier *type);

/* A question about two types. */
typedef struct sxml_type_pair {
    sxml_type_identifier derived_type;
    sxml_type_identifier base_type;
} sxml_type_pair;

/*
 * True when pair.derived_type is pair.base_type or is derived from it,
 * directly or through a chain of base types, by restriction, list or
 * union (XSD 1.1 Part 1, "Type Derivation OK (Complex)" and "Type
 * Derivation OK (Simple)"). Also true when base_type is a union that has
 * derived_type among its transitive members. This is the
 * relation that type annotations are tested with, for example
 * instance of xs:integer on an xs:short item.
 *
 * False when either identifier was not issued by the registry. O(depth of
 * the type hierarchy).
 */
SXML_EXPORT bool sxml_type_registry_is_derived_from(
    const sxml_type_registry *registry, sxml_type_pair pair);

/*
 * True when pair.derived_type is a subtype of pair.base_type in the sense
 * of XPath 4.0, section 3.3.2 "Subtypes of Item Types": the general rules,
 * choice types, atomic and union types, enumerations, node tests, JNode
 * tests, function types (contravariant parameters), maps, arrays and
 * records. Never allocates.
 *
 * False when either identifier was not issued by the registry.
 */
SXML_EXPORT bool sxml_type_registry_is_subtype(
    const sxml_type_registry *registry, sxml_type_pair pair);

/* A question about two sequence types. */
typedef struct sxml_sequence_type_pair {
    sxml_sequence_type derived_type;
    sxml_sequence_type base_type;
} sxml_sequence_type_pair;

/*
 * True when pair.derived_type is a subtype of pair.base_type (XPath 4.0,
 * section 3.3.1 "Subtypes of Sequence Types"): the occurrence of the first
 * allows a subset of the counts of the second and, unless it is
 * SXML_OCCURRENCE_ZERO, its item type is a subtype.
 * False when an identifier was not issued by the registry.
 */
SXML_EXPORT bool sxml_type_registry_is_sequence_subtype(
    const sxml_type_registry *registry, sxml_sequence_type_pair pair);

/*
 * Registers the item type description describes and writes its identifier
 * to *type. variety must be one of the item type varieties
 * (SXML_TYPE_VARIETY_NODE_TEST up to SXML_TYPE_VARIETY_CHOICE). type_name,
 * base_type, primitive_type and is_builtin are ignored. Views in description
 * are copied.
 *
 * Structural item types are interned: an equal description gives the
 * identifier registered first, and a description equal to a built-in type
 * (node test element() for example) gives the built-in identifier. A record
 * type with is_nominative set is rejected here; use
 * sxml_type_registry_declare_nominative_record_type.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for a description that is not an
 * item type or names identifiers the registry did not issue,
 * SXML_STATUS_CAPACITY_EXCEEDED for a registry without an allocator,
 * SXML_STATUS_LIMIT_EXCEEDED at maximum_registered_type_count, and the
 * failures of the allocator.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_type_registry_register_item_type(
    sxml_type_registry *registry, const sxml_type_description *description,
    sxml_type_identifier *type);

/*
 * Declares a nominative record type named type_name, without fields yet,
 * and writes its identifier to *type, so that field types, its own
 * included, can refer to it (recursive record types, XPath 4.0, section
 * 3.2.10.2.1). Complete it with sxml_type_registry_define_record_fields
 * before using it for anything else.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT when type_name already names a type
 * in this registry, and the failures of
 * sxml_type_registry_register_item_type.
 */
SXML_NODISCARD SXML_EXPORT sxml_status
sxml_type_registry_declare_nominative_record_type(
    sxml_type_registry *registry, sxml_name_identifier type_name,
    sxml_type_identifier *type);

/* The fields that complete a declared nominative record type. */
typedef struct sxml_record_definition {
    sxml_type_identifier record_type;
    /* Copied. Field names must be distinct. */
    sxml_record_field_view record_fields;
} sxml_record_definition;

/*
 * Gives a declared nominative record type its fields. Returns
 * SXML_STATUS_INVALID_ARGUMENT when record_type is not a declared and not
 * yet defined nominative record type, when two fields share a name, or when
 * a field type names an identifier the registry did not issue; and the
 * failures of the allocator.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_type_registry_define_record_fields(
    sxml_type_registry *registry, const sxml_record_definition *definition);

/* A field to look up in a record type. */
typedef struct sxml_record_field_query {
    sxml_type_identifier record_type;
    sxml_byte_view field_name;
} sxml_record_field_query;

/*
 * Finds the field named query.field_name in query.record_type and writes its
 * position to *field_index. Returns false, leaving *field_index untouched,
 * when record_type is not a record type with such a field. O(1) average:
 * records of one type share this lookup, the key "shape" of D7.
 */
SXML_EXPORT bool sxml_type_registry_find_record_field(
    const sxml_type_registry *registry, sxml_record_field_query query,
    size_t *field_index);
