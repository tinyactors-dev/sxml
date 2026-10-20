/*
 * Compile stage READ, the reading half: one schema document's elements
 * into schema components "as read" (src/schema_read_internal.h,
 * src/schema_internal.h; docs/DECISIONS.md D165, D167, D170).
 *
 * Golden file: src/xslt_read.c (a table of element rules, attributes
 * collected and checked per element, the tree walked without recursion).
 *
 * Specification: XSD 1.1 Part 1 (W3C Recommendation, 5 April 2012), the
 * "XML Representation" sections of 3.2 to 3.17, the Schema Representation
 * Constraints of appendix B.3 (src-*), the schema for schema documents
 * (appendix A), 4.2.2 "Conditional inclusion", 3.17.6.1 "Schema
 * Properties Correct" (sch-props-correct.2, duplicate names) and 3.17.6.2
 * "QName resolution (Schema Document)" (src-resolve).
 *
 * The table. Every element of a schema document has a role (glossary
 * schema_element_role) that its parent's child slots (glossary
 * child_slot) give it: an xs:element child of xs:schema is a global
 * element declaration, one of xs:sequence a local one with a particle.
 * The role's rule lists the attributes the schema for schema documents
 * allows (unqualified ones; attributes in other namespaces than XSD's are
 * always allowed, 3.1.? "openAttrs"), which of them are required, and the
 * slots its children fill in order. What breaks a rule is reported with
 * the role's representation constraint (src-element for an element
 * declaration, src-ct for the parts of a complex type, ...; where the
 * specification has none, the component's properties constraint), since
 * each "Representation OK" constraint begins "in addition to the
 * conditions imposed by the schema for schema documents" (D170).
 *
 * The walk. Nodes are visited in document order, by index, from the
 * element to read to its last descendant; an element's children get their
 * roles when the element itself is read, so an element without a role
 * (ignored by conditional inclusion, not allowed where it is, or inside an
 * annotation) is skipped with its subtree. Each element is read into its
 * components at once, linked to its parent's component by index; lists of
 * children (a model group's particles, a type's attribute uses and
 * facets, ...) are written in a second pass, when every child exists, so
 * each list is written once, whole (sxml_schema_components_add_list).
 * Components are added in document order of their start tags, a particle
 * before its term and an attribute use before its declaration.
 */
#include "schema_read_internal.h"

#include "cast_internal.h"
#include "evaluator_internal.h"
#include "processor_internal.h"
#include "tree_internal.h"
#include "xdm_internal.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* ---------------------------------------------------------------- Roles */

/* The roles an element of a schema document can have (glossary
   schema_element_role). At most 64, so that a child slot is a bit set. */
typedef enum read_role {
    SXML_READ_ROLE_NONE = 0,
    SXML_READ_ROLE_SCHEMA,
    SXML_READ_ROLE_INCLUDE,
    SXML_READ_ROLE_IMPORT,
    SXML_READ_ROLE_REDEFINE,
    SXML_READ_ROLE_OVERRIDE,
    SXML_READ_ROLE_ANNOTATION,
    SXML_READ_ROLE_APPINFO,
    SXML_READ_ROLE_DOCUMENTATION,
    SXML_READ_ROLE_DEFAULT_OPEN_CONTENT,
    SXML_READ_ROLE_GLOBAL_SIMPLE_TYPE,
    SXML_READ_ROLE_LOCAL_SIMPLE_TYPE,
    SXML_READ_ROLE_SIMPLE_RESTRICTION,
    SXML_READ_ROLE_LIST,
    SXML_READ_ROLE_UNION,
    SXML_READ_ROLE_MIN_EXCLUSIVE,
    SXML_READ_ROLE_MIN_INCLUSIVE,
    SXML_READ_ROLE_MAX_EXCLUSIVE,
    SXML_READ_ROLE_MAX_INCLUSIVE,
    SXML_READ_ROLE_TOTAL_DIGITS,
    SXML_READ_ROLE_FRACTION_DIGITS,
    SXML_READ_ROLE_LENGTH,
    SXML_READ_ROLE_MIN_LENGTH,
    SXML_READ_ROLE_MAX_LENGTH,
    SXML_READ_ROLE_ENUMERATION,
    SXML_READ_ROLE_WHITE_SPACE,
    SXML_READ_ROLE_PATTERN,
    SXML_READ_ROLE_EXPLICIT_TIMEZONE,
    SXML_READ_ROLE_ASSERTION,
    SXML_READ_ROLE_GLOBAL_COMPLEX_TYPE,
    SXML_READ_ROLE_LOCAL_COMPLEX_TYPE,
    SXML_READ_ROLE_SIMPLE_CONTENT,
    SXML_READ_ROLE_COMPLEX_CONTENT,
    SXML_READ_ROLE_SIMPLE_CONTENT_RESTRICTION,
    SXML_READ_ROLE_SIMPLE_CONTENT_EXTENSION,
    SXML_READ_ROLE_COMPLEX_CONTENT_RESTRICTION,
    SXML_READ_ROLE_COMPLEX_CONTENT_EXTENSION,
    SXML_READ_ROLE_OPEN_CONTENT,
    SXML_READ_ROLE_GLOBAL_GROUP,
    SXML_READ_ROLE_GROUP_REFERENCE,
    SXML_READ_ROLE_GROUP_ALL,
    SXML_READ_ROLE_GROUP_CHOICE,
    SXML_READ_ROLE_GROUP_SEQUENCE,
    SXML_READ_ROLE_ALL,
    SXML_READ_ROLE_CHOICE,
    SXML_READ_ROLE_SEQUENCE,
    SXML_READ_ROLE_GLOBAL_ELEMENT,
    SXML_READ_ROLE_LOCAL_ELEMENT,
    SXML_READ_ROLE_GLOBAL_ATTRIBUTE,
    SXML_READ_ROLE_LOCAL_ATTRIBUTE,
    SXML_READ_ROLE_GLOBAL_ATTRIBUTE_GROUP,
    SXML_READ_ROLE_ATTRIBUTE_GROUP_REFERENCE,
    SXML_READ_ROLE_ANY,
    SXML_READ_ROLE_ANY_ATTRIBUTE,
    SXML_READ_ROLE_OPEN_CONTENT_ANY,
    SXML_READ_ROLE_ALTERNATIVE,
    SXML_READ_ROLE_ASSERT,
    SXML_READ_ROLE_UNIQUE,
    SXML_READ_ROLE_KEY,
    SXML_READ_ROLE_KEYREF,
    SXML_READ_ROLE_SELECTOR,
    SXML_READ_ROLE_FIELD,
    SXML_READ_ROLE_NOTATION,
    SXML_READ_ROLE_COUNT,
} read_role;

#define ROLE_BIT(role) (UINT64_C(1) << (unsigned)(role))

/* ----------------------------------------------------------- Attributes */

/* The unqualified attributes of the schema for schema documents. At most
   64, so that a rule's attributes are a bit set. */
typedef enum read_attribute {
    SXML_READ_ATTRIBUTE_ID = 0,
    SXML_READ_ATTRIBUTE_NAME,
    SXML_READ_ATTRIBUTE_REF,
    SXML_READ_ATTRIBUTE_TYPE,
    SXML_READ_ATTRIBUTE_BASE,
    SXML_READ_ATTRIBUTE_ITEM_TYPE,
    SXML_READ_ATTRIBUTE_MEMBER_TYPES,
    SXML_READ_ATTRIBUTE_SUBSTITUTION_GROUP,
    SXML_READ_ATTRIBUTE_DEFAULT,
    SXML_READ_ATTRIBUTE_FIXED,
    SXML_READ_ATTRIBUTE_NILLABLE,
    SXML_READ_ATTRIBUTE_ABSTRACT,
    SXML_READ_ATTRIBUTE_FINAL,
    SXML_READ_ATTRIBUTE_BLOCK,
    SXML_READ_ATTRIBUTE_MIXED,
    SXML_READ_ATTRIBUTE_FORM,
    SXML_READ_ATTRIBUTE_MIN_OCCURS,
    SXML_READ_ATTRIBUTE_MAX_OCCURS,
    SXML_READ_ATTRIBUTE_USE,
    SXML_READ_ATTRIBUTE_NAMESPACE,
    SXML_READ_ATTRIBUTE_NOT_NAMESPACE,
    SXML_READ_ATTRIBUTE_NOT_QNAME,
    SXML_READ_ATTRIBUTE_PROCESS_CONTENTS,
    SXML_READ_ATTRIBUTE_SCHEMA_LOCATION,
    SXML_READ_ATTRIBUTE_TARGET_NAMESPACE,
    SXML_READ_ATTRIBUTE_ELEMENT_FORM_DEFAULT,
    SXML_READ_ATTRIBUTE_ATTRIBUTE_FORM_DEFAULT,
    SXML_READ_ATTRIBUTE_BLOCK_DEFAULT,
    SXML_READ_ATTRIBUTE_FINAL_DEFAULT,
    SXML_READ_ATTRIBUTE_DEFAULT_ATTRIBUTES,
    SXML_READ_ATTRIBUTE_XPATH_DEFAULT_NAMESPACE,
    SXML_READ_ATTRIBUTE_VERSION,
    SXML_READ_ATTRIBUTE_DEFAULT_ATTRIBUTES_APPLY,
    SXML_READ_ATTRIBUTE_INHERITABLE,
    SXML_READ_ATTRIBUTE_MODE,
    SXML_READ_ATTRIBUTE_APPLIES_TO_EMPTY,
    SXML_READ_ATTRIBUTE_TEST,
    SXML_READ_ATTRIBUTE_XPATH,
    SXML_READ_ATTRIBUTE_REFER,
    SXML_READ_ATTRIBUTE_VALUE,
    SXML_READ_ATTRIBUTE_PUBLIC,
    SXML_READ_ATTRIBUTE_SYSTEM,
    SXML_READ_ATTRIBUTE_SOURCE,
    SXML_READ_ATTRIBUTE_COUNT,
} read_attribute;

#define ATTRIBUTE_BIT(attribute) (UINT64_C(1) << (unsigned)(attribute))

/* How an attribute's value is checked before its role reads it (the
   simple types of the schema for schema documents). */
typedef enum value_kind {
    /* Any string: xs:string, xs:anyURI (which accepts nearly any text in
       XSD 1.1, Part 2 3.3.17), xs:token. */
    SXML_VALUE_KIND_TEXT = 0,
    /* xs:NCName (and xs:ID). */
    SXML_VALUE_KIND_NCNAME,
    /* xs:QName and a list of them. */
    SXML_VALUE_KIND_QNAME,
    SXML_VALUE_KIND_QNAME_LIST,
    SXML_VALUE_KIND_BOOLEAN,
    /* xs:nonNegativeInteger; allNNI (also "unbounded"). */
    SXML_VALUE_KIND_NON_NEGATIVE_INTEGER,
    SXML_VALUE_KIND_ALL_NNI,
    /* qualified or unqualified. */
    SXML_VALUE_KIND_FORM,
    /* optional, prohibited or required. */
    SXML_VALUE_KIND_USE,
    /* skip, lax or strict. */
    SXML_VALUE_KIND_PROCESS_CONTENTS,
    /* Read by the role, which knows the allowed values: final, block,
       finalDefault, blockDefault, mode, value, fixed, namespace,
       notNamespace, notQName, xpathDefaultNamespace. */
    SXML_VALUE_KIND_BY_ROLE,
} value_kind;

typedef struct attribute_rule {
    sxml_byte_view local_name;
    value_kind kind;
} attribute_rule;

static const attribute_rule attribute_rules[SXML_READ_ATTRIBUTE_COUNT] = {
    [SXML_READ_ATTRIBUTE_ID] = {SXML_BYTE_VIEW_INITIALIZER("id"), SXML_VALUE_KIND_NCNAME},
    [SXML_READ_ATTRIBUTE_NAME] = {SXML_BYTE_VIEW_INITIALIZER("name"), SXML_VALUE_KIND_NCNAME},
    [SXML_READ_ATTRIBUTE_REF] = {SXML_BYTE_VIEW_INITIALIZER("ref"), SXML_VALUE_KIND_QNAME},
    [SXML_READ_ATTRIBUTE_TYPE] = {SXML_BYTE_VIEW_INITIALIZER("type"), SXML_VALUE_KIND_QNAME},
    [SXML_READ_ATTRIBUTE_BASE] = {SXML_BYTE_VIEW_INITIALIZER("base"), SXML_VALUE_KIND_QNAME},
    [SXML_READ_ATTRIBUTE_ITEM_TYPE] = {SXML_BYTE_VIEW_INITIALIZER("itemType"),
                                       SXML_VALUE_KIND_QNAME},
    [SXML_READ_ATTRIBUTE_MEMBER_TYPES] = {SXML_BYTE_VIEW_INITIALIZER("memberTypes"),
                                          SXML_VALUE_KIND_QNAME_LIST},
    [SXML_READ_ATTRIBUTE_SUBSTITUTION_GROUP] = {SXML_BYTE_VIEW_INITIALIZER("substitutionGroup"),
                                                SXML_VALUE_KIND_QNAME_LIST},
    [SXML_READ_ATTRIBUTE_DEFAULT] = {SXML_BYTE_VIEW_INITIALIZER("default"), SXML_VALUE_KIND_TEXT},
    [SXML_READ_ATTRIBUTE_FIXED] = {SXML_BYTE_VIEW_INITIALIZER("fixed"), SXML_VALUE_KIND_BY_ROLE},
    [SXML_READ_ATTRIBUTE_NILLABLE] = {SXML_BYTE_VIEW_INITIALIZER("nillable"),
                                      SXML_VALUE_KIND_BOOLEAN},
    [SXML_READ_ATTRIBUTE_ABSTRACT] = {SXML_BYTE_VIEW_INITIALIZER("abstract"),
                                      SXML_VALUE_KIND_BOOLEAN},
    [SXML_READ_ATTRIBUTE_FINAL] = {SXML_BYTE_VIEW_INITIALIZER("final"), SXML_VALUE_KIND_BY_ROLE},
    [SXML_READ_ATTRIBUTE_BLOCK] = {SXML_BYTE_VIEW_INITIALIZER("block"), SXML_VALUE_KIND_BY_ROLE},
    [SXML_READ_ATTRIBUTE_MIXED] = {SXML_BYTE_VIEW_INITIALIZER("mixed"), SXML_VALUE_KIND_BOOLEAN},
    [SXML_READ_ATTRIBUTE_FORM] = {SXML_BYTE_VIEW_INITIALIZER("form"), SXML_VALUE_KIND_FORM},
    [SXML_READ_ATTRIBUTE_MIN_OCCURS] = {SXML_BYTE_VIEW_INITIALIZER("minOccurs"),
                                        SXML_VALUE_KIND_NON_NEGATIVE_INTEGER},
    [SXML_READ_ATTRIBUTE_MAX_OCCURS] = {SXML_BYTE_VIEW_INITIALIZER("maxOccurs"),
                                        SXML_VALUE_KIND_ALL_NNI},
    [SXML_READ_ATTRIBUTE_USE] = {SXML_BYTE_VIEW_INITIALIZER("use"), SXML_VALUE_KIND_USE},
    [SXML_READ_ATTRIBUTE_NAMESPACE] = {SXML_BYTE_VIEW_INITIALIZER("namespace"),
                                       SXML_VALUE_KIND_BY_ROLE},
    [SXML_READ_ATTRIBUTE_NOT_NAMESPACE] = {SXML_BYTE_VIEW_INITIALIZER("notNamespace"),
                                           SXML_VALUE_KIND_BY_ROLE},
    [SXML_READ_ATTRIBUTE_NOT_QNAME] = {SXML_BYTE_VIEW_INITIALIZER("notQName"),
                                       SXML_VALUE_KIND_BY_ROLE},
    [SXML_READ_ATTRIBUTE_PROCESS_CONTENTS] = {SXML_BYTE_VIEW_INITIALIZER("processContents"),
                                              SXML_VALUE_KIND_PROCESS_CONTENTS},
    [SXML_READ_ATTRIBUTE_SCHEMA_LOCATION] = {SXML_BYTE_VIEW_INITIALIZER("schemaLocation"),
                                             SXML_VALUE_KIND_TEXT},
    [SXML_READ_ATTRIBUTE_TARGET_NAMESPACE] = {SXML_BYTE_VIEW_INITIALIZER("targetNamespace"),
                                              SXML_VALUE_KIND_TEXT},
    [SXML_READ_ATTRIBUTE_ELEMENT_FORM_DEFAULT] = {SXML_BYTE_VIEW_INITIALIZER("elementFormDefault"),
                                                  SXML_VALUE_KIND_FORM},
    [SXML_READ_ATTRIBUTE_ATTRIBUTE_FORM_DEFAULT] = {SXML_BYTE_VIEW_INITIALIZER(
                                                        "attributeFormDefault"),
                                                    SXML_VALUE_KIND_FORM},
    [SXML_READ_ATTRIBUTE_BLOCK_DEFAULT] = {SXML_BYTE_VIEW_INITIALIZER("blockDefault"),
                                           SXML_VALUE_KIND_BY_ROLE},
    [SXML_READ_ATTRIBUTE_FINAL_DEFAULT] = {SXML_BYTE_VIEW_INITIALIZER("finalDefault"),
                                           SXML_VALUE_KIND_BY_ROLE},
    [SXML_READ_ATTRIBUTE_DEFAULT_ATTRIBUTES] = {SXML_BYTE_VIEW_INITIALIZER("defaultAttributes"),
                                                SXML_VALUE_KIND_QNAME},
    [SXML_READ_ATTRIBUTE_XPATH_DEFAULT_NAMESPACE] = {SXML_BYTE_VIEW_INITIALIZER(
                                                         "xpathDefaultNamespace"),
                                                     SXML_VALUE_KIND_BY_ROLE},
    [SXML_READ_ATTRIBUTE_VERSION] = {SXML_BYTE_VIEW_INITIALIZER("version"), SXML_VALUE_KIND_TEXT},
    [SXML_READ_ATTRIBUTE_DEFAULT_ATTRIBUTES_APPLY] = {SXML_BYTE_VIEW_INITIALIZER(
                                                          "defaultAttributesApply"),
                                                      SXML_VALUE_KIND_BOOLEAN},
    [SXML_READ_ATTRIBUTE_INHERITABLE] = {SXML_BYTE_VIEW_INITIALIZER("inheritable"),
                                         SXML_VALUE_KIND_BOOLEAN},
    [SXML_READ_ATTRIBUTE_MODE] = {SXML_BYTE_VIEW_INITIALIZER("mode"), SXML_VALUE_KIND_BY_ROLE},
    [SXML_READ_ATTRIBUTE_APPLIES_TO_EMPTY] = {SXML_BYTE_VIEW_INITIALIZER("appliesToEmpty"),
                                              SXML_VALUE_KIND_BOOLEAN},
    [SXML_READ_ATTRIBUTE_TEST] = {SXML_BYTE_VIEW_INITIALIZER("test"), SXML_VALUE_KIND_TEXT},
    [SXML_READ_ATTRIBUTE_XPATH] = {SXML_BYTE_VIEW_INITIALIZER("xpath"), SXML_VALUE_KIND_TEXT},
    [SXML_READ_ATTRIBUTE_REFER] = {SXML_BYTE_VIEW_INITIALIZER("refer"), SXML_VALUE_KIND_QNAME},
    [SXML_READ_ATTRIBUTE_VALUE] = {SXML_BYTE_VIEW_INITIALIZER("value"), SXML_VALUE_KIND_BY_ROLE},
    [SXML_READ_ATTRIBUTE_PUBLIC] = {SXML_BYTE_VIEW_INITIALIZER("public"), SXML_VALUE_KIND_TEXT},
    [SXML_READ_ATTRIBUTE_SYSTEM] = {SXML_BYTE_VIEW_INITIALIZER("system"), SXML_VALUE_KIND_TEXT},
    [SXML_READ_ATTRIBUTE_SOURCE] = {SXML_BYTE_VIEW_INITIALIZER("source"), SXML_VALUE_KIND_TEXT},
};

/* ------------------------------------------------------------ Rules */

/* One place in a role's content (glossary child_slot). */
typedef struct child_slot {
    /* The roles allowed here, as ROLE_BIT bits. */
    uint64_t roles;
    /* At least one child here. */
    bool is_required;
    /* Any number of children here, not just one. */
    bool repeats;
    /* Nothing may follow a child here: the slot is one branch of a
       choice whose other branch is the slots after it (complexType's
       simpleContent | complexContent | the rest). */
    bool ends_content;
} child_slot;

#define MAXIMUM_SLOT_COUNT 7U

/* The facets a restriction of a simple type or of simple content may
   have (Part 2, 4.3; the facet group of the schema for schema
   documents). */
#define FACET_ROLES                                                                                \
    (ROLE_BIT(SXML_READ_ROLE_MIN_EXCLUSIVE) | ROLE_BIT(SXML_READ_ROLE_MIN_INCLUSIVE) |             \
     ROLE_BIT(SXML_READ_ROLE_MAX_EXCLUSIVE) | ROLE_BIT(SXML_READ_ROLE_MAX_INCLUSIVE) |             \
     ROLE_BIT(SXML_READ_ROLE_TOTAL_DIGITS) | ROLE_BIT(SXML_READ_ROLE_FRACTION_DIGITS) |            \
     ROLE_BIT(SXML_READ_ROLE_LENGTH) | ROLE_BIT(SXML_READ_ROLE_MIN_LENGTH) |                       \
     ROLE_BIT(SXML_READ_ROLE_MAX_LENGTH) | ROLE_BIT(SXML_READ_ROLE_ENUMERATION) |                  \
     ROLE_BIT(SXML_READ_ROLE_WHITE_SPACE) | ROLE_BIT(SXML_READ_ROLE_PATTERN) |                     \
     ROLE_BIT(SXML_READ_ROLE_EXPLICIT_TIMEZONE) | ROLE_BIT(SXML_READ_ROLE_ASSERTION))

/* The particles of a sequence or choice (nestedParticle). */
#define NESTED_PARTICLE_ROLES                                                                      \
    (ROLE_BIT(SXML_READ_ROLE_LOCAL_ELEMENT) | ROLE_BIT(SXML_READ_ROLE_GROUP_REFERENCE) |           \
     ROLE_BIT(SXML_READ_ROLE_CHOICE) | ROLE_BIT(SXML_READ_ROLE_SEQUENCE) |                         \
     ROLE_BIT(SXML_READ_ROLE_ANY))

/* The particles of an all group (XSD 1.1: elements, wildcards, group
   references). */
#define ALL_PARTICLE_ROLES                                                                         \
    (ROLE_BIT(SXML_READ_ROLE_LOCAL_ELEMENT) | ROLE_BIT(SXML_READ_ROLE_ANY) |                       \
     ROLE_BIT(SXML_READ_ROLE_GROUP_REFERENCE))

/* The explicit content of a complex type (typeDefParticle). */
#define CONTENT_PARTICLE_ROLES                                                                     \
    (ROLE_BIT(SXML_READ_ROLE_GROUP_REFERENCE) | ROLE_BIT(SXML_READ_ROLE_ALL) |                     \
     ROLE_BIT(SXML_READ_ROLE_CHOICE) | ROLE_BIT(SXML_READ_ROLE_SEQUENCE))

#define ATTRIBUTE_ROLES                                                                            \
    (ROLE_BIT(SXML_READ_ROLE_LOCAL_ATTRIBUTE) | ROLE_BIT(SXML_READ_ROLE_ATTRIBUTE_GROUP_REFERENCE))

#define IDENTITY_CONSTRAINT_ROLES                                                                  \
    (ROLE_BIT(SXML_READ_ROLE_UNIQUE) | ROLE_BIT(SXML_READ_ROLE_KEY) |                              \
     ROLE_BIT(SXML_READ_ROLE_KEYREF))

#define LOCAL_TYPE_ROLES                                                                           \
    (ROLE_BIT(SXML_READ_ROLE_LOCAL_SIMPLE_TYPE) | ROLE_BIT(SXML_READ_ROLE_LOCAL_COMPLEX_TYPE))

#define REDEFINABLE_ROLES                                                                          \
    (ROLE_BIT(SXML_READ_ROLE_GLOBAL_SIMPLE_TYPE) | ROLE_BIT(SXML_READ_ROLE_GLOBAL_COMPLEX_TYPE) |  \
     ROLE_BIT(SXML_READ_ROLE_GLOBAL_GROUP) | ROLE_BIT(SXML_READ_ROLE_GLOBAL_ATTRIBUTE_GROUP))

#define OVERRIDABLE_ROLES                                                                          \
    (REDEFINABLE_ROLES | ROLE_BIT(SXML_READ_ROLE_GLOBAL_ELEMENT) |                                 \
     ROLE_BIT(SXML_READ_ROLE_GLOBAL_ATTRIBUTE) | ROLE_BIT(SXML_READ_ROLE_NOTATION))

#define ANNOTATION_ROLE ROLE_BIT(SXML_READ_ROLE_ANNOTATION)

/* Slot shorthands: optional and once, optional and repeated, required and
   once. */
#define OPTIONAL_SLOT(role_bits)                                                                   \
    { (role_bits), false, false, false }
#define REPEATED_SLOT(role_bits)                                                                   \
    { (role_bits), false, true, false }
#define REQUIRED_SLOT(role_bits)                                                                   \
    { (role_bits), true, false, false }
#define ANNOTATION_SLOT OPTIONAL_SLOT(ANNOTATION_ROLE)

/* The attribute groups every rule draws from. */
#define ATTRIBUTES_ID ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_ID)
#define ATTRIBUTES_OCCURS                                                                          \
    (ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_MIN_OCCURS) | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_MAX_OCCURS))
#define ATTRIBUTES_WILDCARD                                                                        \
    (ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NAMESPACE) |                                \
     ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NOT_NAMESPACE) |                                            \
     ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NOT_QNAME) |                                                \
     ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_PROCESS_CONTENTS))
#define ATTRIBUTES_FACET                                                                           \
    (ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_VALUE) |                                    \
     ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_FIXED))
#define ATTRIBUTES_NO_FIXED_FACET (ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_VALUE))
#define ATTRIBUTES_XPATH_DEFAULT ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_XPATH_DEFAULT_NAMESPACE)
#define ATTRIBUTES_IDENTITY                                                                        \
    (ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NAME) |                                     \
     ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_REF))

/* What one role is: its element's local name, the constraint its errors
   carry, its attributes and its child slots. */
typedef struct role_rule {
    sxml_byte_view local_name;
    sxml_byte_view constraint_name;
    uint64_t attributes;
    uint64_t required_attributes;
    child_slot slots[MAXIMUM_SLOT_COUNT];
    uint32_t slot_count;
    /* The content is not read: appinfo and documentation (any content),
       and the roles whose children are read later. */
    bool skips_content;
    /* Elements in namespaces other than XSD's are allowed among the
       children (the implementation-defined facets of a restriction). */
    bool allows_foreign_children;
    /* For a facet role: its kind. */
    sxml_facet_kind facet_kind;
} role_rule;

#define CONSTRAINT(literal) SXML_BYTE_VIEW_INITIALIZER(literal)
#define NAME(literal) SXML_BYTE_VIEW_INITIALIZER(literal)

/* A facet's rule. */
#define FACET_RULE(name_literal, kind, attribute_bits)                                             \
    {                                                                                              \
        .local_name = NAME(name_literal), .constraint_name = CONSTRAINT("src-simple-type"),        \
        .attributes = (attribute_bits),                                                            \
        .required_attributes = ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_VALUE),                           \
        .slots = {ANNOTATION_SLOT}, .slot_count = 1, .facet_kind = (kind)                          \
    }

/* The rule of a role whose content is an annotation alone. */
#define ANNOTATED_RULE(name_literal, constraint_literal, attribute_bits, required_bits)            \
    {                                                                                              \
        .local_name = NAME(name_literal), .constraint_name = CONSTRAINT(constraint_literal),       \
        .attributes = (attribute_bits), .required_attributes = (required_bits),                    \
        .slots = {ANNOTATION_SLOT}, .slot_count = 1                                                \
    }

/* The content of a complex type after its annotation, and of
   complexContent's restriction and extension: open content, a particle,
   attributes, an attribute wildcard, assertions. */
#define COMPLEX_CONTENT_SLOTS                                                                      \
    OPTIONAL_SLOT(ROLE_BIT(SXML_READ_ROLE_OPEN_CONTENT)), OPTIONAL_SLOT(CONTENT_PARTICLE_ROLES),   \
        REPEATED_SLOT(ATTRIBUTE_ROLES), OPTIONAL_SLOT(ROLE_BIT(SXML_READ_ROLE_ANY_ATTRIBUTE)),     \
        REPEATED_SLOT(ROLE_BIT(SXML_READ_ROLE_ASSERT))

/* The attributes of a model group in content (with occurrences) or in a
   group definition (without). */
#define MODEL_GROUP_RULE(name_literal, attribute_bits, particle_roles)                             \
    {                                                                                              \
        .local_name = NAME(name_literal), .constraint_name = CONSTRAINT("mg-props-correct"),       \
        .attributes = (attribute_bits), .slots = {ANNOTATION_SLOT, REPEATED_SLOT(particle_roles)}, \
        .slot_count = 2                                                                            \
    }

static const role_rule role_rules[SXML_READ_ROLE_COUNT] = {
    [SXML_READ_ROLE_SCHEMA] =
        {.local_name = NAME("schema"),
         .constraint_name = CONSTRAINT("sch-props-correct"),
         .attributes = ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_TARGET_NAMESPACE) |
                       ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_VERSION) |
                       ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_FINAL_DEFAULT) |
                       ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_BLOCK_DEFAULT) |
                       ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_ATTRIBUTE_FORM_DEFAULT) |
                       ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_ELEMENT_FORM_DEFAULT) |
                       ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_DEFAULT_ATTRIBUTES) |
                       ATTRIBUTES_XPATH_DEFAULT,
         .slots = {REPEATED_SLOT(ROLE_BIT(SXML_READ_ROLE_INCLUDE) |
                                 ROLE_BIT(SXML_READ_ROLE_IMPORT) |
                                 ROLE_BIT(SXML_READ_ROLE_REDEFINE) |
                                 ROLE_BIT(SXML_READ_ROLE_OVERRIDE) | ANNOTATION_ROLE),
                   OPTIONAL_SLOT(ROLE_BIT(SXML_READ_ROLE_DEFAULT_OPEN_CONTENT)),
                   REPEATED_SLOT(OVERRIDABLE_ROLES | ANNOTATION_ROLE)},
         .slot_count = 3},
    [SXML_READ_ROLE_INCLUDE] =
        ANNOTATED_RULE("include", "src-include",
                       ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_SCHEMA_LOCATION),
                       ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_SCHEMA_LOCATION)),
    [SXML_READ_ROLE_IMPORT] =
        ANNOTATED_RULE("import", "src-import",
                       ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NAMESPACE) |
                           ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_SCHEMA_LOCATION),
                       0),
    [SXML_READ_ROLE_REDEFINE] = {.local_name = NAME("redefine"),
                                 .constraint_name = CONSTRAINT("src-redefine"),
                                 .attributes = ATTRIBUTES_ID |
                                               ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_SCHEMA_LOCATION),
                                 .required_attributes =
                                     ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_SCHEMA_LOCATION),
                                 .slots = {REPEATED_SLOT(REDEFINABLE_ROLES | ANNOTATION_ROLE)},
                                 .slot_count = 1},
    [SXML_READ_ROLE_OVERRIDE] = {.local_name = NAME("override"),
                                 .constraint_name = CONSTRAINT("src-override"),
                                 .attributes = ATTRIBUTES_ID |
                                               ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_SCHEMA_LOCATION),
                                 .required_attributes =
                                     ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_SCHEMA_LOCATION),
                                 .slots = {REPEATED_SLOT(OVERRIDABLE_ROLES | ANNOTATION_ROLE)},
                                 .slot_count = 1},
    [SXML_READ_ROLE_ANNOTATION] = {.local_name = NAME("annotation"),
                                   .constraint_name = CONSTRAINT("an-props-correct"),
                                   .attributes = ATTRIBUTES_ID,
                                   .slots = {REPEATED_SLOT(ROLE_BIT(SXML_READ_ROLE_APPINFO) |
                                                           ROLE_BIT(SXML_READ_ROLE_DOCUMENTATION))},
                                   .slot_count = 1},
    [SXML_READ_ROLE_APPINFO] = {.local_name = NAME("appinfo"),
                                .constraint_name = CONSTRAINT("an-props-correct"),
                                .attributes = ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_SOURCE),
                                .skips_content = true},
    [SXML_READ_ROLE_DOCUMENTATION] = {.local_name = NAME("documentation"),
                                      .constraint_name = CONSTRAINT("an-props-correct"),
                                      .attributes = ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_SOURCE),
                                      .skips_content = true},
    [SXML_READ_ROLE_DEFAULT_OPEN_CONTENT] =
        {.local_name = NAME("defaultOpenContent"),
         .constraint_name = CONSTRAINT("src-ct"),
         .attributes = ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_APPLIES_TO_EMPTY) |
                       ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_MODE),
         .slots = {ANNOTATION_SLOT, REQUIRED_SLOT(ROLE_BIT(SXML_READ_ROLE_OPEN_CONTENT_ANY))},
         .slot_count = 2},
    [SXML_READ_ROLE_GLOBAL_SIMPLE_TYPE] =
        {.local_name = NAME("simpleType"),
         .constraint_name = CONSTRAINT("src-simple-type"),
         .attributes = ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NAME) |
                       ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_FINAL),
         .required_attributes = ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NAME),
         .slots = {ANNOTATION_SLOT,
                   REQUIRED_SLOT(ROLE_BIT(SXML_READ_ROLE_SIMPLE_RESTRICTION) |
                                 ROLE_BIT(SXML_READ_ROLE_LIST) | ROLE_BIT(SXML_READ_ROLE_UNION))},
         .slot_count = 2},
    [SXML_READ_ROLE_LOCAL_SIMPLE_TYPE] =
        {.local_name = NAME("simpleType"),
         .constraint_name = CONSTRAINT("src-simple-type"),
         .attributes = ATTRIBUTES_ID,
         .slots = {ANNOTATION_SLOT,
                   REQUIRED_SLOT(ROLE_BIT(SXML_READ_ROLE_SIMPLE_RESTRICTION) |
                                 ROLE_BIT(SXML_READ_ROLE_LIST) | ROLE_BIT(SXML_READ_ROLE_UNION))},
         .slot_count = 2},
    [SXML_READ_ROLE_SIMPLE_RESTRICTION] =
        {.local_name = NAME("restriction"),
         .constraint_name = CONSTRAINT("src-simple-type"),
         .attributes = ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_BASE),
         .slots = {ANNOTATION_SLOT, OPTIONAL_SLOT(ROLE_BIT(SXML_READ_ROLE_LOCAL_SIMPLE_TYPE)),
                   REPEATED_SLOT(FACET_ROLES)},
         .slot_count = 3,
         .allows_foreign_children = true},
    [SXML_READ_ROLE_LIST] = {.local_name = NAME("list"),
                             .constraint_name = CONSTRAINT("src-simple-type"),
                             .attributes =
                                 ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_ITEM_TYPE),
                             .slots = {ANNOTATION_SLOT,
                                       OPTIONAL_SLOT(ROLE_BIT(SXML_READ_ROLE_LOCAL_SIMPLE_TYPE))},
                             .slot_count = 2},
    [SXML_READ_ROLE_UNION] = {.local_name = NAME("union"),
                              .constraint_name = CONSTRAINT("src-simple-type"),
                              .attributes =
                                  ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_MEMBER_TYPES),
                              .slots = {ANNOTATION_SLOT,
                                        REPEATED_SLOT(ROLE_BIT(SXML_READ_ROLE_LOCAL_SIMPLE_TYPE))},
                              .slot_count = 2},
    [SXML_READ_ROLE_MIN_EXCLUSIVE] =
        FACET_RULE("minExclusive", SXML_FACET_KIND_MIN_EXCLUSIVE, ATTRIBUTES_FACET),
    [SXML_READ_ROLE_MIN_INCLUSIVE] =
        FACET_RULE("minInclusive", SXML_FACET_KIND_MIN_INCLUSIVE, ATTRIBUTES_FACET),
    [SXML_READ_ROLE_MAX_EXCLUSIVE] =
        FACET_RULE("maxExclusive", SXML_FACET_KIND_MAX_EXCLUSIVE, ATTRIBUTES_FACET),
    [SXML_READ_ROLE_MAX_INCLUSIVE] =
        FACET_RULE("maxInclusive", SXML_FACET_KIND_MAX_INCLUSIVE, ATTRIBUTES_FACET),
    [SXML_READ_ROLE_TOTAL_DIGITS] =
        FACET_RULE("totalDigits", SXML_FACET_KIND_TOTAL_DIGITS, ATTRIBUTES_FACET),
    [SXML_READ_ROLE_FRACTION_DIGITS] =
        FACET_RULE("fractionDigits", SXML_FACET_KIND_FRACTION_DIGITS, ATTRIBUTES_FACET),
    [SXML_READ_ROLE_LENGTH] = FACET_RULE("length", SXML_FACET_KIND_LENGTH, ATTRIBUTES_FACET),
    [SXML_READ_ROLE_MIN_LENGTH] =
        FACET_RULE("minLength", SXML_FACET_KIND_MIN_LENGTH, ATTRIBUTES_FACET),
    [SXML_READ_ROLE_MAX_LENGTH] =
        FACET_RULE("maxLength", SXML_FACET_KIND_MAX_LENGTH, ATTRIBUTES_FACET),
    [SXML_READ_ROLE_ENUMERATION] =
        FACET_RULE("enumeration", SXML_FACET_KIND_ENUMERATION, ATTRIBUTES_NO_FIXED_FACET),
    [SXML_READ_ROLE_WHITE_SPACE] =
        FACET_RULE("whiteSpace", SXML_FACET_KIND_WHITESPACE, ATTRIBUTES_FACET),
    [SXML_READ_ROLE_PATTERN] =
        FACET_RULE("pattern", SXML_FACET_KIND_PATTERN, ATTRIBUTES_NO_FIXED_FACET),
    [SXML_READ_ROLE_EXPLICIT_TIMEZONE] =
        FACET_RULE("explicitTimezone", SXML_FACET_KIND_EXPLICIT_TIMEZONE, ATTRIBUTES_FACET),
    [SXML_READ_ROLE_ASSERTION] = ANNOTATED_RULE(
        "assertion", "as-props-correct",
        ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_TEST) | ATTRIBUTES_XPATH_DEFAULT, 0),
    [SXML_READ_ROLE_GLOBAL_COMPLEX_TYPE] =
        {.local_name = NAME("complexType"),
         .constraint_name = CONSTRAINT("src-ct"),
         .attributes = ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NAME) |
                       ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_MIXED) |
                       ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_ABSTRACT) |
                       ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_FINAL) |
                       ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_BLOCK) |
                       ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_DEFAULT_ATTRIBUTES_APPLY),
         .required_attributes = ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NAME),
         .slots = {ANNOTATION_SLOT,
                   {ROLE_BIT(SXML_READ_ROLE_SIMPLE_CONTENT) |
                        ROLE_BIT(SXML_READ_ROLE_COMPLEX_CONTENT),
                    false, false, true},
                   COMPLEX_CONTENT_SLOTS},
         .slot_count = 7},
    [SXML_READ_ROLE_LOCAL_COMPLEX_TYPE] =
        {.local_name = NAME("complexType"),
         .constraint_name = CONSTRAINT("src-ct"),
         .attributes = ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_MIXED) |
                       ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_DEFAULT_ATTRIBUTES_APPLY),
         .slots = {ANNOTATION_SLOT,
                   {ROLE_BIT(SXML_READ_ROLE_SIMPLE_CONTENT) |
                        ROLE_BIT(SXML_READ_ROLE_COMPLEX_CONTENT),
                    false, false, true},
                   COMPLEX_CONTENT_SLOTS},
         .slot_count = 7},
    [SXML_READ_ROLE_SIMPLE_CONTENT] =
        {.local_name = NAME("simpleContent"),
         .constraint_name = CONSTRAINT("src-ct"),
         .attributes = ATTRIBUTES_ID,
         .slots = {ANNOTATION_SLOT,
                   REQUIRED_SLOT(ROLE_BIT(SXML_READ_ROLE_SIMPLE_CONTENT_RESTRICTION) |
                                 ROLE_BIT(SXML_READ_ROLE_SIMPLE_CONTENT_EXTENSION))},
         .slot_count = 2},
    [SXML_READ_ROLE_COMPLEX_CONTENT] =
        {.local_name = NAME("complexContent"),
         .constraint_name = CONSTRAINT("src-ct"),
         .attributes = ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_MIXED),
         .slots = {ANNOTATION_SLOT,
                   REQUIRED_SLOT(ROLE_BIT(SXML_READ_ROLE_COMPLEX_CONTENT_RESTRICTION) |
                                 ROLE_BIT(SXML_READ_ROLE_COMPLEX_CONTENT_EXTENSION))},
         .slot_count = 2},
    [SXML_READ_ROLE_SIMPLE_CONTENT_RESTRICTION] =
        {.local_name = NAME("restriction"),
         .constraint_name = CONSTRAINT("src-ct"),
         .attributes = ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_BASE),
         .required_attributes = ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_BASE),
         .slots = {ANNOTATION_SLOT, OPTIONAL_SLOT(ROLE_BIT(SXML_READ_ROLE_LOCAL_SIMPLE_TYPE)),
                   REPEATED_SLOT(FACET_ROLES), REPEATED_SLOT(ATTRIBUTE_ROLES),
                   OPTIONAL_SLOT(ROLE_BIT(SXML_READ_ROLE_ANY_ATTRIBUTE)),
                   REPEATED_SLOT(ROLE_BIT(SXML_READ_ROLE_ASSERT))},
         .slot_count = 6,
         .allows_foreign_children = true},
    [SXML_READ_ROLE_SIMPLE_CONTENT_EXTENSION] =
        {.local_name = NAME("extension"),
         .constraint_name = CONSTRAINT("src-ct"),
         .attributes = ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_BASE),
         .required_attributes = ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_BASE),
         .slots = {ANNOTATION_SLOT, REPEATED_SLOT(ATTRIBUTE_ROLES),
                   OPTIONAL_SLOT(ROLE_BIT(SXML_READ_ROLE_ANY_ATTRIBUTE)),
                   REPEATED_SLOT(ROLE_BIT(SXML_READ_ROLE_ASSERT))},
         .slot_count = 4},
    [SXML_READ_ROLE_COMPLEX_CONTENT_RESTRICTION] =
        {.local_name = NAME("restriction"),
         .constraint_name = CONSTRAINT("src-ct"),
         .attributes = ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_BASE),
         .required_attributes = ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_BASE),
         .slots = {ANNOTATION_SLOT, COMPLEX_CONTENT_SLOTS},
         .slot_count = 6},
    [SXML_READ_ROLE_COMPLEX_CONTENT_EXTENSION] =
        {.local_name = NAME("extension"),
         .constraint_name = CONSTRAINT("src-ct"),
         .attributes = ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_BASE),
         .required_attributes = ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_BASE),
         .slots = {ANNOTATION_SLOT, COMPLEX_CONTENT_SLOTS},
         .slot_count = 6},
    [SXML_READ_ROLE_OPEN_CONTENT] =
        {.local_name = NAME("openContent"),
         .constraint_name = CONSTRAINT("src-ct"),
         .attributes = ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_MODE),
         .slots = {ANNOTATION_SLOT, OPTIONAL_SLOT(ROLE_BIT(SXML_READ_ROLE_OPEN_CONTENT_ANY))},
         .slot_count = 2},
    [SXML_READ_ROLE_GLOBAL_GROUP] =
        {.local_name = NAME("group"),
         .constraint_name = CONSTRAINT("mgd-props-correct"),
         .attributes = ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NAME),
         .required_attributes = ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NAME),
         .slots = {ANNOTATION_SLOT, REQUIRED_SLOT(ROLE_BIT(SXML_READ_ROLE_GROUP_ALL) |
                                                  ROLE_BIT(SXML_READ_ROLE_GROUP_CHOICE) |
                                                  ROLE_BIT(SXML_READ_ROLE_GROUP_SEQUENCE))},
         .slot_count = 2},
    [SXML_READ_ROLE_GROUP_REFERENCE] =
        ANNOTATED_RULE("group", "mgd-props-correct",
                       ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_REF) | ATTRIBUTES_OCCURS,
                       ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_REF)),
    [SXML_READ_ROLE_GROUP_ALL] = MODEL_GROUP_RULE("all", ATTRIBUTES_ID, ALL_PARTICLE_ROLES),
    [SXML_READ_ROLE_GROUP_CHOICE] =
        MODEL_GROUP_RULE("choice", ATTRIBUTES_ID, NESTED_PARTICLE_ROLES),
    [SXML_READ_ROLE_GROUP_SEQUENCE] =
        MODEL_GROUP_RULE("sequence", ATTRIBUTES_ID, NESTED_PARTICLE_ROLES),
    [SXML_READ_ROLE_ALL] =
        MODEL_GROUP_RULE("all", ATTRIBUTES_ID | ATTRIBUTES_OCCURS, ALL_PARTICLE_ROLES),
    [SXML_READ_ROLE_CHOICE] =
        MODEL_GROUP_RULE("choice", ATTRIBUTES_ID | ATTRIBUTES_OCCURS, NESTED_PARTICLE_ROLES),
    [SXML_READ_ROLE_SEQUENCE] =
        MODEL_GROUP_RULE("sequence", ATTRIBUTES_ID | ATTRIBUTES_OCCURS, NESTED_PARTICLE_ROLES),
    [SXML_READ_ROLE_GLOBAL_ELEMENT] =
        {.local_name = NAME("element"),
         .constraint_name = CONSTRAINT("src-element"),
         .attributes =
             ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NAME) |
             ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_TYPE) |
             ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_SUBSTITUTION_GROUP) |
             ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_DEFAULT) | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_FIXED) |
             ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NILLABLE) |
             ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_ABSTRACT) |
             ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_FINAL) | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_BLOCK),
         .required_attributes = ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NAME),
         .slots = {ANNOTATION_SLOT, OPTIONAL_SLOT(LOCAL_TYPE_ROLES),
                   REPEATED_SLOT(ROLE_BIT(SXML_READ_ROLE_ALTERNATIVE)),
                   REPEATED_SLOT(IDENTITY_CONSTRAINT_ROLES)},
         .slot_count = 4},
    [SXML_READ_ROLE_LOCAL_ELEMENT] =
        {.local_name = NAME("element"),
         .constraint_name = CONSTRAINT("src-element"),
         .attributes =
             ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NAME) |
             ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_REF) | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_TYPE) |
             ATTRIBUTES_OCCURS | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_DEFAULT) |
             ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_FIXED) |
             ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NILLABLE) |
             ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_BLOCK) | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_FORM) |
             ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_TARGET_NAMESPACE),
         .slots = {ANNOTATION_SLOT, OPTIONAL_SLOT(LOCAL_TYPE_ROLES),
                   REPEATED_SLOT(ROLE_BIT(SXML_READ_ROLE_ALTERNATIVE)),
                   REPEATED_SLOT(IDENTITY_CONSTRAINT_ROLES)},
         .slot_count = 4},
    [SXML_READ_ROLE_GLOBAL_ATTRIBUTE] =
        {.local_name = NAME("attribute"),
         .constraint_name = CONSTRAINT("src-attribute"),
         .attributes = ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NAME) |
                       ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_TYPE) |
                       ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_DEFAULT) |
                       ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_FIXED) |
                       ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_INHERITABLE),
         .required_attributes = ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NAME),
         .slots = {ANNOTATION_SLOT, OPTIONAL_SLOT(ROLE_BIT(SXML_READ_ROLE_LOCAL_SIMPLE_TYPE))},
         .slot_count = 2},
    [SXML_READ_ROLE_LOCAL_ATTRIBUTE] =
        {.local_name = NAME("attribute"),
         .constraint_name = CONSTRAINT("src-attribute"),
         .attributes =
             ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NAME) |
             ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_REF) |
             ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_TYPE) | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_USE) |
             ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_DEFAULT) | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_FIXED) |
             ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_FORM) |
             ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_TARGET_NAMESPACE) |
             ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_INHERITABLE),
         .slots = {ANNOTATION_SLOT, OPTIONAL_SLOT(ROLE_BIT(SXML_READ_ROLE_LOCAL_SIMPLE_TYPE))},
         .slot_count = 2},
    [SXML_READ_ROLE_GLOBAL_ATTRIBUTE_GROUP] =
        {.local_name = NAME("attributeGroup"),
         .constraint_name = CONSTRAINT("src-attribute_group"),
         .attributes = ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NAME),
         .required_attributes = ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NAME),
         .slots = {ANNOTATION_SLOT, REPEATED_SLOT(ATTRIBUTE_ROLES),
                   OPTIONAL_SLOT(ROLE_BIT(SXML_READ_ROLE_ANY_ATTRIBUTE))},
         .slot_count = 3},
    [SXML_READ_ROLE_ATTRIBUTE_GROUP_REFERENCE] =
        ANNOTATED_RULE("attributeGroup", "src-attribute_group",
                       ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_REF),
                       ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_REF)),
    [SXML_READ_ROLE_ANY] =
        ANNOTATED_RULE("any", "src-wildcard", ATTRIBUTES_WILDCARD | ATTRIBUTES_OCCURS, 0),
    [SXML_READ_ROLE_ANY_ATTRIBUTE] =
        ANNOTATED_RULE("anyAttribute", "src-wildcard", ATTRIBUTES_WILDCARD, 0),
    [SXML_READ_ROLE_OPEN_CONTENT_ANY] =
        ANNOTATED_RULE("any", "src-wildcard", ATTRIBUTES_WILDCARD, 0),
    [SXML_READ_ROLE_ALTERNATIVE] = {.local_name = NAME("alternative"),
                                    .constraint_name = CONSTRAINT("src-ta"),
                                    .attributes = ATTRIBUTES_ID |
                                                  ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_TEST) |
                                                  ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_TYPE) |
                                                  ATTRIBUTES_XPATH_DEFAULT,
                                    .slots = {ANNOTATION_SLOT, OPTIONAL_SLOT(LOCAL_TYPE_ROLES)},
                                    .slot_count = 2},
    [SXML_READ_ROLE_ASSERT] = ANNOTATED_RULE(
        "assert", "as-props-correct",
        ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_TEST) | ATTRIBUTES_XPATH_DEFAULT, 0),
    [SXML_READ_ROLE_UNIQUE] = {.local_name = NAME("unique"),
                               .constraint_name = CONSTRAINT("src-identity-constraint"),
                               .attributes = ATTRIBUTES_IDENTITY,
                               .slots = {ANNOTATION_SLOT,
                                         OPTIONAL_SLOT(ROLE_BIT(SXML_READ_ROLE_SELECTOR)),
                                         REPEATED_SLOT(ROLE_BIT(SXML_READ_ROLE_FIELD))},
                               .slot_count = 3},
    [SXML_READ_ROLE_KEY] = {.local_name = NAME("key"),
                            .constraint_name = CONSTRAINT("src-identity-constraint"),
                            .attributes = ATTRIBUTES_IDENTITY,
                            .slots = {ANNOTATION_SLOT,
                                      OPTIONAL_SLOT(ROLE_BIT(SXML_READ_ROLE_SELECTOR)),
                                      REPEATED_SLOT(ROLE_BIT(SXML_READ_ROLE_FIELD))},
                            .slot_count = 3},
    [SXML_READ_ROLE_KEYREF] = {.local_name = NAME("keyref"),
                               .constraint_name = CONSTRAINT("src-identity-constraint"),
                               .attributes =
                                   ATTRIBUTES_IDENTITY | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_REFER),
                               .slots = {ANNOTATION_SLOT,
                                         OPTIONAL_SLOT(ROLE_BIT(SXML_READ_ROLE_SELECTOR)),
                                         REPEATED_SLOT(ROLE_BIT(SXML_READ_ROLE_FIELD))},
                               .slot_count = 3},
    [SXML_READ_ROLE_SELECTOR] = ANNOTATED_RULE(
        "selector", "src-identity-constraint",
        ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_XPATH) | ATTRIBUTES_XPATH_DEFAULT,
        ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_XPATH)),
    [SXML_READ_ROLE_FIELD] = ANNOTATED_RULE(
        "field", "src-identity-constraint",
        ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_XPATH) | ATTRIBUTES_XPATH_DEFAULT,
        ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_XPATH)),
    [SXML_READ_ROLE_NOTATION] = ANNOTATED_RULE(
        "notation", "n-props-correct",
        ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NAME) |
            ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_PUBLIC) | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_SYSTEM),
        ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_NAME)),
};

/* ------------------------------------------------------------ The reader */

/* The lists of a component that its children's components join, written
   once every child is read. */
typedef enum list_field {
    SXML_LIST_FIELD_NONE = 0,
    SXML_LIST_FIELD_PARTICLES,
    SXML_LIST_FIELD_ATTRIBUTE_USES,
    SXML_LIST_FIELD_ATTRIBUTE_GROUP_REFERENCES,
    SXML_LIST_FIELD_FACETS,
    SXML_LIST_FIELD_ASSERTIONS,
    SXML_LIST_FIELD_FACET_ASSERTIONS,
    SXML_LIST_FIELD_MEMBER_TYPES,
    SXML_LIST_FIELD_ALTERNATIVES,
    SXML_LIST_FIELD_IDENTITY_CONSTRAINTS,
    SXML_LIST_FIELD_FIELDS,
    SXML_LIST_FIELD_COUNT,
} list_field;

/* A component: its table and index; table TYPE_DEFINITION with index 0
   is "none". */
typedef struct component_key {
    sxml_schema_table table;
    uint32_t index;
} component_key;

static const component_key no_component = {.table = SXML_SCHEMA_TABLE_TYPE_DEFINITION, .index = 0};

/* What the reader knows about one node of the range it reads, by node
   index from the first. */
typedef struct node_record {
    /* Given by the parent's child slots; NONE: not read. */
    read_role role;
    /* The list of the parent's owner that primary joins. */
    list_field joined_list;
    /* The element or an ancestor has xml:base. */
    bool has_xml_base;
    /* The component the element's children attach to. */
    component_key owner;
    /* The component the element gives its parent (a particle, an
       attribute use, a facet, a reference, ...). */
    uint32_t primary;
    /* The complex type, model group definition or attribute group the
       element is in, the {parent} of local declarations. */
    component_key scope;
} node_record;

/* The element being read. */
typedef struct element_state {
    sxml_node_index node;
    sxml_node_description description;
    read_role role;
    const role_rule *rule;
    node_record *record;
    const node_record *parent_record;
    read_role parent_role;
    sxml_schema_position position;
    /* Attribute values by read_attribute, and which are present. */
    sxml_byte_view values[SXML_READ_ATTRIBUTE_COUNT];
    uint64_t present_attributes;
    /* The roles of its children that are read. */
    uint64_t child_roles;
    /* The element's in-scope namespaces, owned by the tree. */
    sxml_namespace_binding_view bindings;
    /* Its subtree is not walked: its content is not read here. */
    bool skips_subtree;
} element_state;

/* The global component a redefine's or override's child replaces. */
typedef struct redefinition_state {
    bool is_active;
    sxml_schema_symbol_space symbol_space;
    sxml_name_identifier name;
    uint32_t original;
    /* The component that replaces it, once read. */
    uint32_t replacement;
    /* The last node of the child's subtree, and where it starts. */
    uint32_t last_node;
    sxml_schema_position position;
    /* What xs:redefine requires (Part 1, 4.2.4, src-redefine.5 to 7): a
       type derived from itself; at most one reference of a group or
       attribute group to itself, a group's with minOccurs and maxOccurs
       1. */
    bool has_self_derivation;
    uint32_t self_reference_count;
    bool has_counted_self_reference;
} redefinition_state;

/* A growing array of the reader's (sxml_evaluator_grow). */
typedef struct scratch_array {
    void *elements;
    size_t element_count;
    size_t element_capacity;
} scratch_array;

typedef struct schema_reader {
    sxml_schema_assembly *assembly;
    sxml_schema_compilation *compilation;
    sxml_schema_components *components;
    sxml_name_pool *name_pool;
    sxml_allocator allocator;
    const sxml_schema_read_request *request;
    sxml_schema_document_defaults *defaults;
    sxml_tree *tree;
    uint32_t first_node;
    uint32_t last_node;
    /* node_record, one per node of the range. */
    scratch_array records;
    /* sxml_namespace_uri_identifier: the namespaces the document's
       xs:import elements name (SXML_NAMESPACE_NONE for one without a
       namespace attribute). */
    scratch_array imported_namespaces;
    /* uint32_t: indexes a list is gathered in. */
    scratch_array list_indexes;
    /* The versioning namespace (vc:), when the name pool knows it. */
    bool knows_versioning_namespace;
    sxml_namespace_uri_identifier versioning_namespace;
    /* Computed base URIs. */
    sxml_xml_buffer base_uri;
    sxml_schema_namespace_context_index last_context;
    redefinition_state redefinition;
    /* The mixed attribute of the xs:complexType last read, for its
       xs:complexContent child (src-ct). */
    bool has_complex_type_mixed;
    bool complex_type_mixed;
    element_state element;
} schema_reader;

static sxml_status grow_scratch(schema_reader *reader, scratch_array *array,
                                sxml_evaluator_growth growth) {
    growth.elements = &array->elements;
    growth.element_capacity = &array->element_capacity;
    growth.element_count = array->element_count;
    return sxml_evaluator_grow(reader->allocator, &growth);
}

static sxml_status append_list_index(schema_reader *reader, uint32_t index) {
    sxml_status status = grow_scratch(
        reader, &reader->list_indexes,
        (sxml_evaluator_growth){.required_count = reader->list_indexes.element_count + 1U,
                                .element_byte_count = sizeof(uint32_t),
                                .alignment = alignof(uint32_t)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t *indexes = reader->list_indexes.elements;
    indexes[reader->list_indexes.element_count] = index;
    reader->list_indexes.element_count += 1;
    return SXML_STATUS_OK;
}

static node_record *record_of(const schema_reader *reader, uint32_t node) {
    node_record *records = reader->records.elements;
    return &records[node - reader->first_node];
}

static bool is_in_range(const schema_reader *reader, uint32_t node) {
    return node >= reader->first_node && node <= reader->last_node;
}

/* ------------------------------------------------------------ Text */

/* [7] QName: prefix and local part split at the colon. */
typedef struct qname_parts {
    bool is_valid;
    sxml_byte_view prefix;
    sxml_byte_view local_name;
} qname_parts;

static qname_parts split_qname(sxml_byte_view text) {
    qname_parts parts = {
        .is_valid = false, .prefix = {.bytes = NULL, .byte_count = 0}, .local_name = text};
    const uint8_t *colon = text.byte_count == 0 ? NULL : memchr(text.bytes, ':', text.byte_count);
    if (colon != NULL) {
        const size_t prefix_count = (size_t)(colon - text.bytes);
        parts.prefix = (sxml_byte_view){.bytes = text.bytes, .byte_count = prefix_count};
        parts.local_name =
            (sxml_byte_view){.bytes = colon + 1, .byte_count = text.byte_count - prefix_count - 1U};
        parts.is_valid = sxml_xml_text_is_ncname(parts.prefix) && sxml_xml_text_is_ncname(parts.local_name);
    } else {
        parts.is_valid = sxml_xml_text_is_ncname(text);
    }
    return parts;
}

/* An xs:boolean value read (Part 2, 3.3.2), whitespace collapsed. */
typedef sxml_lexical_boolean boolean_value_text;

static boolean_value_text parse_boolean(sxml_byte_view text) {
    return sxml_lexical_read_boolean(sxml_xml_text_trim_whitespace(text));
}

/* xs:nonNegativeInteger (Part 2, 3.4.20); a value above
   SXML_SCHEMA_UNBOUNDED - 1 is read as that. */
static bool parse_non_negative_integer(sxml_byte_view text, uint64_t *value) {
    sxml_byte_view digits = sxml_xml_text_trim_whitespace(text);
    bool is_negative = false;
    if (digits.byte_count > 0 && (digits.bytes[0] == '+' || digits.bytes[0] == '-')) {
        is_negative = digits.bytes[0] == '-';
        digits.bytes += 1;
        digits.byte_count -= 1;
    }
    if (digits.byte_count == 0) {
        return false;
    }
    for (size_t digit_index = 0; digit_index < digits.byte_count; digit_index += 1) {
        const uint8_t byte = digits.bytes[digit_index];
        if (byte < '0' || byte > '9' || (is_negative && byte != '0')) {
            /* Not digits, or below zero (-0 is zero). */
            return false;
        }
    }
    if (!sxml_lexical_read_digits(
            (sxml_digit_value_request){.digits = digits, .maximum = SXML_SCHEMA_UNBOUNDED - 1U},
            value)) {
        *value = SXML_SCHEMA_UNBOUNDED - 1U;
    }
    return true;
}

/* allNNI: a nonNegativeInteger or "unbounded". */
static bool parse_all_nni(sxml_byte_view text, uint64_t *value) {
    if (sxml_byte_view_equals(sxml_xml_text_trim_whitespace(text), SXML_TEXT("unbounded"))) {
        *value = SXML_SCHEMA_UNBOUNDED;
        return true;
    }
    return parse_non_negative_integer(text, value);
}

/* One of a list of keywords, by position. */
typedef struct keyword_list {
    const sxml_byte_view *keywords;
    size_t keyword_count;
} keyword_list;

static bool find_keyword(keyword_list list, sxml_byte_view text, size_t *position) {
    const sxml_byte_view token = sxml_xml_text_trim_whitespace(text);
    for (size_t keyword_index = 0; keyword_index < list.keyword_count; keyword_index += 1) {
        if (sxml_byte_view_equals(token, list.keywords[keyword_index])) {
            *position = keyword_index;
            return true;
        }
    }
    return false;
}

static const sxml_byte_view form_keywords[] = {SXML_BYTE_VIEW_INITIALIZER("unqualified"),
                                               SXML_BYTE_VIEW_INITIALIZER("qualified")};
static const sxml_byte_view use_keywords[] = {SXML_BYTE_VIEW_INITIALIZER("optional"),
                                              SXML_BYTE_VIEW_INITIALIZER("required"),
                                              SXML_BYTE_VIEW_INITIALIZER("prohibited")};
static const sxml_byte_view process_contents_keywords[] = {SXML_BYTE_VIEW_INITIALIZER("strict"),
                                                           SXML_BYTE_VIEW_INITIALIZER("lax"),
                                                           SXML_BYTE_VIEW_INITIALIZER("skip")};

#define KEYWORDS(array)                                                                            \
    ((keyword_list){.keywords = (array), .keyword_count = sizeof(array) / sizeof((array)[0])})

/* ------------------------------------------------------------ Tree */

static sxml_node_reference node_reference(const schema_reader *reader, uint32_t node) {
    return (sxml_node_reference){
        .tree = reader->tree, .node_index = {.index = node}, .namespace_number = 0};
}

static sxml_status describe_node(const schema_reader *reader, uint32_t node,
                                 sxml_node_description *description) {
    memset(description, 0, sizeof(*description));
    return sxml_node_describe(node_reference(reader, node), description);
}

static sxml_schema_position position_of_node(const schema_reader *reader, uint32_t node) {
    sxml_source_location location = {.source_name = {.bytes = NULL, .byte_count = 0},
                                     .line_number = 0,
                                     .column_number = 0,
                                     .byte_offset = 0};
    if (reader->tree->operations->get_location == NULL ||
        reader->tree->operations->get_location(
            reader->tree->state, (sxml_node_index){.index = node}, &location) != SXML_STATUS_OK) {
        location.line_number = 0;
        location.column_number = 0;
    }
    return (sxml_schema_position){.document = reader->request->document,
                                  .line_number = location.line_number,
                                  .column_number = location.column_number,
                                  .byte_offset = location.byte_offset};
}

/* The namespace and local name of a named node. */
static sxml_name_parts name_parts_of(const schema_reader *reader, sxml_name_identifier name) {
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    if (sxml_name_pool_get_name_parts(reader->name_pool, name, &parts) != SXML_STATUS_OK) {
        parts.local_name = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return parts;
}

static bool is_schema_namespace(sxml_namespace_uri_identifier uri) {
    return uri.index == (uint32_t)SXML_NAMESPACE_XML_SCHEMA;
}

/* ------------------------------------------------------------ Reports */

/* An error found at the current element. */
typedef struct read_error {
    /* The constraint name; empty for the role's. */
    sxml_byte_view constraint_name;
    /* Static text. */
    const char *description;
    /* Shown after the description in the report sink's text: the name or
       value involved; may be empty. */
    sxml_byte_view subject;
} read_error;

static sxml_status report_at(schema_reader *reader, read_error error,
                             sxml_schema_position position) {
    const sxml_byte_view description = sxml_byte_view_from_c_string(error.description);
    sxml_byte_view constraint_name = error.constraint_name;
    if (constraint_name.byte_count == 0) {
        constraint_name = reader->element.rule != NULL ? reader->element.rule->constraint_name
                                                       : SXML_TEXT("sch-props-correct");
    }
    return sxml_schema_compilation_report(
        reader->compilation, &(sxml_schema_report_request){
                                 .constraint_name = constraint_name,
                                 .description = description,
                                 .subject = error.subject,
                                 .position = position});
}

static sxml_status report(schema_reader *reader, read_error error) {
    return report_at(reader, error, reader->element.position);
}

/* ------------------------------------------------------------ Names */

static sxml_status intern_name(schema_reader *reader, sxml_name_request request,
                               sxml_name_identifier *name) {
    return sxml_name_pool_intern_name(reader->name_pool, &request, name);
}

static sxml_status intern_uri(schema_reader *reader, sxml_byte_view uri,
                              sxml_namespace_uri_identifier *identifier) {
    return sxml_name_pool_intern_namespace_uri(reader->name_pool, uri, identifier);
}

/* The namespace a prefix is bound to among the current element's in-scope
   namespaces; the empty prefix gives the default namespace (none when
   undeclared). */
typedef struct prefix_lookup {
    bool is_bound;
    sxml_namespace_uri_identifier namespace_uri;
} prefix_lookup;

static sxml_status lookup_prefix(schema_reader *reader, sxml_byte_view prefix,
                                 prefix_lookup *lookup) {
    *lookup = (prefix_lookup){.is_bound = prefix.byte_count == 0,
                              .namespace_uri = {.index = SXML_NAMESPACE_NONE}};
    sxml_namespace_prefix_identifier prefix_identifier = {.index = 0};
    if (prefix.byte_count != 0) {
        sxml_status status =
            sxml_name_pool_intern_namespace_prefix(reader->name_pool, prefix, &prefix_identifier);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    const sxml_namespace_binding_view bindings = reader->element.bindings;
    for (size_t binding_index = 0; binding_index < bindings.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding binding = bindings.namespace_bindings[binding_index];
        if (binding.namespace_prefix.index == prefix_identifier.index) {
            lookup->is_bound = true;
            lookup->namespace_uri = binding.namespace_uri;
        }
    }
    return SXML_STATUS_OK;
}

/* A QName value resolved to an expanded name (Part 1, 3.17.6.2,
   src-resolve.1 to 3; F.1 for a chameleon inclusion). */
typedef struct qname_resolution {
    bool is_resolved;
    sxml_name_identifier name;
    sxml_namespace_uri_identifier namespace_uri;
} qname_resolution;

static sxml_status resolve_qname(schema_reader *reader, sxml_byte_view text,
                                 qname_resolution *resolution) {
    *resolution = (qname_resolution){
        .is_resolved = false, .name = {.index = 0}, .namespace_uri = {.index = 0}};
    const qname_parts parts = split_qname(sxml_xml_text_trim_whitespace(text));
    if (!parts.is_valid) {
        return report(reader, (read_error){.constraint_name = SXML_TEXT("src-resolve"),
                                           .description = "A QName value is not a QName",
                                           .subject = sxml_xml_text_trim_whitespace(text)});
    }
    prefix_lookup lookup = {.is_bound = false, .namespace_uri = {.index = 0}};
    sxml_status status = lookup_prefix(reader, parts.prefix, &lookup);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!lookup.is_bound) {
        return report(reader,
                      (read_error){.constraint_name = SXML_TEXT("src-resolve"),
                                   .description = "The prefix of a QName value is not declared",
                                   .subject = parts.prefix});
    }
    if (lookup.namespace_uri.index == SXML_NAMESPACE_NONE && reader->request->is_chameleon) {
        lookup.namespace_uri = reader->request->target_namespace;
    }
    status = intern_name(
        reader,
        (sxml_name_request){.namespace_uri = lookup.namespace_uri, .local_name = parts.local_name},
        &resolution->name);
    if (status == SXML_STATUS_OK) {
        resolution->is_resolved = true;
        resolution->namespace_uri = lookup.namespace_uri;
    }
    return status;
}

/* True when the document may refer to components of namespace_uri: its
   target namespace, XSD's (the built-in components), xsi's, or one an
   xs:import of the document names (Part 1, 3.17.6.2, src-resolve.4). */
static bool may_refer_to_namespace(const schema_reader *reader,
                                   sxml_namespace_uri_identifier namespace_uri) {
    if (namespace_uri.index == reader->request->target_namespace.index ||
        is_schema_namespace(namespace_uri) ||
        namespace_uri.index == (uint32_t)SXML_NAMESPACE_XML_SCHEMA_INSTANCE) {
        return true;
    }
    const sxml_namespace_uri_identifier *imported = reader->imported_namespaces.elements;
    for (size_t import_index = 0; import_index < reader->imported_namespaces.element_count;
         import_index += 1) {
        if (imported[import_index].index == namespace_uri.index) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------ Components */

/* One component to add to its table. */
typedef struct component_entry {
    sxml_schema_table table;
    const void *entry;
} component_entry;

static sxml_status add_component(schema_reader *reader, component_entry request, uint32_t *index) {
    return sxml_schema_components_add_entries(reader->components,
                                              &(sxml_schema_entry_request){.table = request.table,
                                                                           .entries = request.entry,
                                                                           .entry_count = 1},
                                              index);
}

static void *edit_component(schema_reader *reader, component_key key) {
    return sxml_schema_components_edit_entry(
        reader->components, (sxml_schema_entry_key){.table = key.table, .index = key.index});
}

static sxml_schema_type_definition *edit_type(schema_reader *reader, uint32_t index) {
    return edit_component(
        reader, (component_key){.table = SXML_SCHEMA_TABLE_TYPE_DEFINITION, .index = index});
}

/* A QName attribute as a schema reference. */
typedef struct reference_request {
    sxml_schema_symbol_space symbol_space;
    sxml_byte_view text;
    /* A redefinition's base or self-reference may name the original. */
    bool may_name_original;
} reference_request;

static sxml_status add_reference(schema_reader *reader, reference_request request,
                                 sxml_schema_reference_index *reference) {
    reference->value = 0;
    qname_resolution resolution = {
        .is_resolved = false, .name = {.index = 0}, .namespace_uri = {.index = 0}};
    sxml_status status = resolve_qname(reader, request.text, &resolution);
    if (status != SXML_STATUS_OK || !resolution.is_resolved) {
        return status;
    }
    if (!may_refer_to_namespace(reader, resolution.namespace_uri)) {
        status = report(reader, (read_error){.constraint_name = SXML_TEXT("src-resolve.4.2"),
                                             .description = "A QName value names a component "
                                                            "of a namespace the schema "
                                                            "document does not import",
                                             .subject = sxml_xml_text_trim_whitespace(request.text)});
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_schema_reference entry = {.symbol_space = request.symbol_space,
                                   .name = resolution.name,
                                   .position = reader->element.position,
                                   .component = 0};
    const redefinition_state *redefinition = &reader->redefinition;
    if (request.may_name_original && redefinition->is_active &&
        reader->request->read_kind == SXML_SCHEMA_READ_KIND_REDEFINE &&
        redefinition->symbol_space == request.symbol_space &&
        redefinition->name.index == resolution.name.index) {
        /* Part 1, 4.2.4: the redefinition's reference to its own name is
           the original. */
        entry.component = redefinition->original;
        reader->redefinition.self_reference_count += 1;
    }
    return add_component(reader,
                         (component_entry){.table = SXML_SCHEMA_TABLE_REFERENCE, .entry = &entry},
                         &reference->value);
}

/* A QName list attribute as a list of reference indexes. */
static sxml_status add_reference_list(schema_reader *reader, reference_request request,
                                      sxml_schema_list *list) {
    const size_t first_index = reader->list_indexes.element_count;
    sxml_byte_view rest = request.text;
    sxml_byte_view token = {.bytes = NULL, .byte_count = 0};
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && sxml_xml_text_next_token(&rest, &token)) {
        sxml_schema_reference_index reference = {.value = 0};
        status = add_reference(reader,
                               (reference_request){.symbol_space = request.symbol_space,
                                                   .text = token,
                                                   .may_name_original = false},
                               &reference);
        if (status == SXML_STATUS_OK && reference.value != 0) {
            status = append_list_index(reader, reference.value);
        }
    }
    if (status == SXML_STATUS_OK) {
        const uint32_t *indexes = reader->list_indexes.elements;
        status = sxml_schema_components_add_list(
            reader->components,
            (sxml_schema_index_view){.indexes = indexes == NULL ? NULL : indexes + first_index,
                                     .index_count =
                                         reader->list_indexes.element_count - first_index},
            list);
    }
    reader->list_indexes.element_count = first_index;
    return status;
}

/* ------------------------------------------------------- Attribute values */

static bool has_attribute(const schema_reader *reader, read_attribute attribute) {
    return (reader->element.present_attributes & ATTRIBUTE_BIT(attribute)) != 0;
}

static sxml_byte_view attribute_value(const schema_reader *reader, read_attribute attribute) {
    return reader->element.values[attribute];
}

static sxml_status report_attribute_value(schema_reader *reader, read_attribute attribute) {
    return report(reader, (read_error){.constraint_name = {.bytes = NULL, .byte_count = 0},
                                       .description = "An attribute's value is not allowed",
                                       .subject = attribute_rules[attribute].local_name});
}

/* A boolean attribute, or default_value when it is absent (or invalid,
   which collect_attributes reported). */
typedef struct boolean_request {
    read_attribute attribute;
    bool default_value;
} boolean_request;

static bool boolean_value(const schema_reader *reader, boolean_request request) {
    if (!has_attribute(reader, request.attribute)) {
        return request.default_value;
    }
    const boolean_value_text text = parse_boolean(attribute_value(reader, request.attribute));
    return text.is_valid ? text.value : request.default_value;
}

/* minOccurs and maxOccurs (default 1, Part 1, 3.9.2). */
typedef struct occurrences {
    uint64_t minimum;
    uint64_t maximum;
} occurrences;

static occurrences occurrences_of(const schema_reader *reader) {
    occurrences result = {.minimum = 1, .maximum = 1};
    if (has_attribute(reader, SXML_READ_ATTRIBUTE_MIN_OCCURS) &&
        !parse_non_negative_integer(attribute_value(reader, SXML_READ_ATTRIBUTE_MIN_OCCURS),
                                    &result.minimum)) {
        result.minimum = 1;
    }
    if (has_attribute(reader, SXML_READ_ATTRIBUTE_MAX_OCCURS) &&
        !parse_all_nni(attribute_value(reader, SXML_READ_ATTRIBUTE_MAX_OCCURS), &result.maximum)) {
        result.maximum = 1;
    }
    return result;
}

/* A derivation set attribute: #all, or a list of the tokens allowed
   (Part 1, 3.3.2, 3.4.2, 3.16.2; derivationSet, blockSet, fullDerivationSet
   and simpleDerivationSet of the schema for schema documents). */
typedef struct derivation_set_request {
    read_attribute attribute;
    sxml_schema_derivation_set allowed;
    /* When the attribute is absent: the schema document's default, masked
       by allowed. */
    sxml_schema_derivation_set default_set;
} derivation_set_request;

static const struct {
    sxml_byte_view token;
    sxml_schema_derivation derivation;
} derivation_tokens[] = {
    {SXML_BYTE_VIEW_INITIALIZER("extension"), SXML_SCHEMA_DERIVATION_EXTENSION},
    {SXML_BYTE_VIEW_INITIALIZER("restriction"), SXML_SCHEMA_DERIVATION_RESTRICTION},
    {SXML_BYTE_VIEW_INITIALIZER("list"), SXML_SCHEMA_DERIVATION_LIST},
    {SXML_BYTE_VIEW_INITIALIZER("union"), SXML_SCHEMA_DERIVATION_UNION},
    {SXML_BYTE_VIEW_INITIALIZER("substitution"), SXML_SCHEMA_DERIVATION_SUBSTITUTION},
};

static sxml_status read_derivation_set(schema_reader *reader, derivation_set_request request,
                                       sxml_schema_derivation_set *set) {
    if (!has_attribute(reader, request.attribute)) {
        *set = request.default_set & request.allowed;
        return SXML_STATUS_OK;
    }
    const sxml_byte_view text = attribute_value(reader, request.attribute);
    if (sxml_byte_view_equals(sxml_xml_text_trim_whitespace(text), SXML_TEXT("#all"))) {
        *set = request.allowed;
        return SXML_STATUS_OK;
    }
    sxml_schema_derivation_set result = 0;
    sxml_byte_view rest = text;
    sxml_byte_view token = {.bytes = NULL, .byte_count = 0};
    bool is_valid = true;
    while (sxml_xml_text_next_token(&rest, &token)) {
        bool is_known = false;
        for (size_t token_index = 0;
             token_index < sizeof(derivation_tokens) / sizeof(derivation_tokens[0]);
             token_index += 1) {
            if (sxml_byte_view_equals(token, derivation_tokens[token_index].token) &&
                (request.allowed & (uint32_t)derivation_tokens[token_index].derivation) != 0) {
                result |= (uint32_t)derivation_tokens[token_index].derivation;
                is_known = true;
            }
        }
        is_valid = is_valid && is_known;
    }
    *set = result;
    return is_valid ? SXML_STATUS_OK : report_attribute_value(reader, request.attribute);
}

#define DERIVATIONS_EXTENSION_RESTRICTION                                                          \
    ((uint32_t)SXML_SCHEMA_DERIVATION_EXTENSION | (uint32_t)SXML_SCHEMA_DERIVATION_RESTRICTION)
#define DERIVATIONS_BLOCK                                                                          \
    (DERIVATIONS_EXTENSION_RESTRICTION | (uint32_t)SXML_SCHEMA_DERIVATION_SUBSTITUTION)
#define DERIVATIONS_FULL                                                                           \
    (DERIVATIONS_EXTENSION_RESTRICTION | (uint32_t)SXML_SCHEMA_DERIVATION_LIST |                   \
     (uint32_t)SXML_SCHEMA_DERIVATION_UNION)

/* ------------------------------------------------------- Collecting */

/* Checks a value of a kind the schema for schema documents fixes. */
static bool is_valid_value(value_kind kind, sxml_byte_view value) {
    uint64_t number = 0;
    size_t position = 0;
    switch (kind) {
    case SXML_VALUE_KIND_NCNAME:
        return sxml_xml_text_is_ncname(sxml_xml_text_trim_whitespace(value));
    case SXML_VALUE_KIND_QNAME:
        return split_qname(sxml_xml_text_trim_whitespace(value)).is_valid;
    case SXML_VALUE_KIND_QNAME_LIST: {
        sxml_byte_view rest = value;
        sxml_byte_view token = {.bytes = NULL, .byte_count = 0};
        while (sxml_xml_text_next_token(&rest, &token)) {
            if (!split_qname(token).is_valid) {
                return false;
            }
        }
        return true;
    }
    case SXML_VALUE_KIND_BOOLEAN:
        return parse_boolean(value).is_valid;
    case SXML_VALUE_KIND_NON_NEGATIVE_INTEGER:
        return parse_non_negative_integer(value, &number);
    case SXML_VALUE_KIND_ALL_NNI:
        return parse_all_nni(value, &number);
    case SXML_VALUE_KIND_FORM:
        return find_keyword(KEYWORDS(form_keywords), value, &position);
    case SXML_VALUE_KIND_USE:
        return find_keyword(KEYWORDS(use_keywords), value, &position);
    case SXML_VALUE_KIND_PROCESS_CONTENTS:
        return find_keyword(KEYWORDS(process_contents_keywords), value, &position);
    case SXML_VALUE_KIND_TEXT:
    case SXML_VALUE_KIND_BY_ROLE:
        return true;
    }
    return true;
}

static bool find_rule_attribute(const role_rule *rule, sxml_byte_view local_name,
                                read_attribute *attribute) {
    for (uint32_t attribute_index = 0; attribute_index < (uint32_t)SXML_READ_ATTRIBUTE_COUNT;
         attribute_index += 1) {
        if ((rule->attributes & ATTRIBUTE_BIT(attribute_index)) != 0 &&
            sxml_byte_view_equals(attribute_rules[attribute_index].local_name, local_name)) {
            *attribute = (read_attribute)attribute_index;
            return true;
        }
    }
    return false;
}

/* Reads the current element's attributes into element.values, reporting
   those the schema for schema documents does not allow (unqualified ones
   not in the rule, any in XSD's namespace), invalid values and missing
   required ones. */
static sxml_status collect_attributes(schema_reader *reader) {
    element_state *element = &reader->element;
    element->present_attributes = 0;
    memset(element->values, 0, sizeof(element->values));
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t attribute_index = 1;
         status == SXML_STATUS_OK && attribute_index <= element->description.attribute_count;
         attribute_index += 1) {
        const uint32_t attribute_node = element->node.index + attribute_index;
        sxml_node_description description;
        status = describe_node(reader, attribute_node, &description);
        sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_node_get_content(node_reference(reader, attribute_node), &value);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const sxml_name_parts parts = name_parts_of(reader, description.node_name);
        if (parts.namespace_uri.index == (uint32_t)SXML_NAMESPACE_XML &&
            sxml_byte_view_equals(parts.local_name, SXML_TEXT("base"))) {
            element->record->has_xml_base = true;
            continue;
        }
        if (parts.namespace_uri.index != SXML_NAMESPACE_NONE &&
            !is_schema_namespace(parts.namespace_uri)) {
            /* openAttrs: attributes of other namespaces are allowed. */
            continue;
        }
        read_attribute attribute = SXML_READ_ATTRIBUTE_ID;
        if (is_schema_namespace(parts.namespace_uri) ||
            !find_rule_attribute(element->rule, parts.local_name, &attribute)) {
            status =
                report(reader, (read_error){.constraint_name = {.bytes = NULL, .byte_count = 0},
                                            .description = "An attribute is not allowed on "
                                                           "this element of the schema "
                                                           "document",
                                            .subject = parts.local_name});
            continue;
        }
        element->values[attribute] = value;
        element->present_attributes |= ATTRIBUTE_BIT(attribute);
        if (!is_valid_value(attribute_rules[attribute].kind, value)) {
            status = report_attribute_value(reader, attribute);
        }
    }
    const uint64_t missing = element->rule->required_attributes & ~element->present_attributes;
    for (uint32_t attribute_index = 0;
         status == SXML_STATUS_OK && attribute_index < (uint32_t)SXML_READ_ATTRIBUTE_COUNT;
         attribute_index += 1) {
        if ((missing & ATTRIBUTE_BIT(attribute_index)) != 0) {
            status = report(reader,
                            (read_error){.constraint_name = {.bytes = NULL, .byte_count = 0},
                                         .description = "A required attribute is missing",
                                         .subject = attribute_rules[attribute_index].local_name});
        }
    }
    return status;
}

/* ------------------------------------------------- Conditional inclusion */

/* "1.1" compared with a decimal (Part 1, 4.2.2: vc:minVersion and
   vc:maxVersion are xs:decimal). Writes -1, 0 or 1 as 1.1 is less than,
   equal to or greater than text; false when text is not a decimal. */
static bool compare_version(sxml_byte_view text, int *comparison) {
    sxml_byte_view digits = sxml_xml_text_trim_whitespace(text);
    bool is_negative = false;
    if (digits.byte_count > 0 && (digits.bytes[0] == '+' || digits.bytes[0] == '-')) {
        is_negative = digits.bytes[0] == '-';
        digits.bytes += 1;
        digits.byte_count -= 1;
    }
    /* The integer part without leading zeros, and the fraction part
       without trailing zeros. */
    size_t point = 0;
    while (point < digits.byte_count && digits.bytes[point] != '.') {
        point += 1;
    }
    size_t integer_start = 0;
    while (integer_start < point && digits.bytes[integer_start] == '0') {
        integer_start += 1;
    }
    size_t fraction_end = digits.byte_count;
    const size_t fraction_start = point < digits.byte_count ? point + 1U : digits.byte_count;
    while (fraction_end > fraction_start && digits.bytes[fraction_end - 1] == '0') {
        fraction_end -= 1;
    }
    bool has_digit = false;
    for (size_t digit_index = 0; digit_index < digits.byte_count; digit_index += 1) {
        const uint8_t byte = digits.bytes[digit_index];
        if (digit_index == point) {
            continue;
        }
        if (byte < '0' || byte > '9') {
            return false;
        }
        has_digit = true;
    }
    if (!has_digit) {
        return false;
    }
    const bool is_zero = integer_start == point && fraction_end == fraction_start;
    if (is_negative && !is_zero) {
        *comparison = 1;
        return true;
    }
    /* 1.1 has the integer part "1" and the fraction "1". */
    const size_t integer_count = point - integer_start;
    if (integer_count != 1) {
        *comparison = integer_count > 1 ? -1 : 1;
        return true;
    }
    if (digits.bytes[integer_start] != '1') {
        *comparison = digits.bytes[integer_start] > '1' ? -1 : 1;
        return true;
    }
    if (fraction_end == fraction_start) {
        *comparison = 1;
        return true;
    }
    if (digits.bytes[fraction_start] != '1') {
        *comparison = digits.bytes[fraction_start] > '1' ? -1 : 1;
        return true;
    }
    *comparison = fraction_end - fraction_start > 1 ? -1 : 0;
    return true;
}

/* The facets this processor knows, by the local name of their elements in
   XSD's namespace (Part 1, 4.2.2: vc:facetAvailable). */
static bool is_known_facet(sxml_byte_view local_name) {
    for (uint32_t role_index = (uint32_t)SXML_READ_ROLE_MIN_EXCLUSIVE;
         role_index <= (uint32_t)SXML_READ_ROLE_ASSERTION; role_index += 1) {
        if (sxml_byte_view_equals(role_rules[role_index].local_name, local_name)) {
            return true;
        }
    }
    return false;
}

/* Whether every name a vc:typeAvailable or vc:facetAvailable list gives
   is available, and whether any is not. */
typedef struct availability {
    bool is_valid;
    bool are_all_available;
    bool is_any_unavailable;
} availability;

typedef struct availability_request {
    sxml_byte_view names;
    bool are_facets;
} availability_request;

static sxml_status check_availability(schema_reader *reader, availability_request request,
                                      availability *result) {
    *result =
        (availability){.is_valid = true, .are_all_available = true, .is_any_unavailable = false};
    sxml_byte_view rest = request.names;
    sxml_byte_view token = {.bytes = NULL, .byte_count = 0};
    while (sxml_xml_text_next_token(&rest, &token)) {
        const qname_parts parts = split_qname(token);
        prefix_lookup lookup = {.is_bound = false, .namespace_uri = {.index = 0}};
        if (parts.is_valid) {
            sxml_status status = lookup_prefix(reader, parts.prefix, &lookup);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        if (!parts.is_valid || !lookup.is_bound) {
            result->is_valid = false;
            return SXML_STATUS_OK;
        }
        bool is_available = false;
        if (is_schema_namespace(lookup.namespace_uri)) {
            if (request.are_facets) {
                is_available = is_known_facet(parts.local_name);
            } else {
                sxml_name_identifier name = {.index = 0};
                uint32_t component = 0;
                /* Types "automatically known": the built-in ones. */
                is_available =
                    sxml_name_pool_find_name(
                        reader->name_pool,
                        &(sxml_name_request){.namespace_uri = lookup.namespace_uri,
                                             .local_name = parts.local_name},
                        &name) &&
                    sxml_schema_components_find_name(
                        reader->components,
                        (sxml_schema_name_key){
                            .symbol_space = SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION, .name = name},
                        &component) &&
                    sxml_schema_get_type_definition(
                        reader->components, (sxml_schema_type_definition_index){.value = component})
                        ->is_builtin;
            }
        }
        result->are_all_available = result->are_all_available && is_available;
        result->is_any_unavailable = result->is_any_unavailable || !is_available;
    }
    return SXML_STATUS_OK;
}

/* Decides whether child, an element, is ignored by conditional inclusion
   (Part 1, 4.2.2), reporting src-cip for vc: values of the wrong type.
   The current element's bindings are swapped for the child's while its
   QNames are read. */
/* Whether conditional inclusion ignores an element. */
typedef struct inclusion_decision {
    bool is_excluded;
} inclusion_decision;

static sxml_status decide_inclusion(schema_reader *reader, uint32_t child,
                                    inclusion_decision *decision) {
    bool *excluded = &decision->is_excluded;
    *excluded = false;
    if (!reader->knows_versioning_namespace) {
        return SXML_STATUS_OK;
    }
    sxml_node_description description;
    sxml_status status = describe_node(reader, child, &description);
    const sxml_namespace_binding_view parent_bindings = reader->element.bindings;
    bool has_child_bindings = false;
    for (uint32_t attribute_index = 1;
         status == SXML_STATUS_OK && attribute_index <= description.attribute_count;
         attribute_index += 1) {
        const uint32_t attribute_node = child + attribute_index;
        sxml_node_description attribute;
        status = describe_node(reader, attribute_node, &attribute);
        if (status != SXML_STATUS_OK) {
            break;
        }
        const sxml_name_parts parts = name_parts_of(reader, attribute.node_name);
        if (parts.namespace_uri.index != reader->versioning_namespace.index) {
            continue;
        }
        sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
        status = sxml_node_get_content(node_reference(reader, attribute_node), &value);
        if (status != SXML_STATUS_OK) {
            break;
        }
        const bool is_minimum = sxml_byte_view_equals(parts.local_name, SXML_TEXT("minVersion"));
        const bool is_maximum = sxml_byte_view_equals(parts.local_name, SXML_TEXT("maxVersion"));
        const bool is_type_available = sxml_byte_view_equals(parts.local_name, SXML_TEXT("typeAvailable"));
        const bool is_type_unavailable =
            sxml_byte_view_equals(parts.local_name, SXML_TEXT("typeUnavailable"));
        const bool is_facet_available = sxml_byte_view_equals(parts.local_name, SXML_TEXT("facetAvailable"));
        const bool is_facet_unavailable =
            sxml_byte_view_equals(parts.local_name, SXML_TEXT("facetUnavailable"));
        const sxml_schema_position position = position_of_node(reader, child);
        if (is_minimum || is_maximum) {
            int comparison = 0;
            if (!compare_version(value, &comparison)) {
                status = report_at(reader,
                                   (read_error){.constraint_name = SXML_TEXT("src-cip.1"),
                                                .description = "vc:minVersion or vc:maxVersion "
                                                               "is not a decimal",
                                                .subject = value},
                                   position);
                continue;
            }
            /* Included when minVersion <= 1.1 < maxVersion. */
            if ((is_minimum && comparison < 0) || (is_maximum && comparison >= 0)) {
                *excluded = true;
            }
        } else if (is_type_available || is_type_unavailable || is_facet_available ||
                   is_facet_unavailable) {
            if (!has_child_bindings) {
                status = sxml_node_get_in_scope_namespaces(node_reference(reader, child),
                                                           &reader->element.bindings);
                has_child_bindings = status == SXML_STATUS_OK;
            }
            availability result = {
                .is_valid = true, .are_all_available = true, .is_any_unavailable = false};
            if (status == SXML_STATUS_OK) {
                status = check_availability(
                    reader,
                    (availability_request){
                        .names = value, .are_facets = is_facet_available || is_facet_unavailable},
                    &result);
            }
            if (status == SXML_STATUS_OK && !result.is_valid) {
                status = report_at(reader,
                                   (read_error){.constraint_name = SXML_TEXT("src-cip.2"),
                                                .description = "A vc: availability attribute is "
                                                               "not a list of QNames",
                                                .subject = value},
                                   position);
                continue;
            }
            if (((is_type_available || is_facet_available) && !result.are_all_available) ||
                ((is_type_unavailable || is_facet_unavailable) && !result.is_any_unavailable)) {
                *excluded = true;
            }
        }
    }
    reader->element.bindings = parent_bindings;
    return status;
}

/* ------------------------------------------------------------ Children */

/* The role of the child named local_name in slot, or NONE. */
static read_role role_in_slot(const child_slot *slot, sxml_byte_view local_name) {
    for (uint32_t role_index = 1; role_index < (uint32_t)SXML_READ_ROLE_COUNT; role_index += 1) {
        if ((slot->roles & ROLE_BIT(role_index)) != 0 &&
            sxml_byte_view_equals(role_rules[role_index].local_name, local_name)) {
            return (read_role)role_index;
        }
    }
    return SXML_READ_ROLE_NONE;
}

static sxml_status report_missing_child(schema_reader *reader, const child_slot *slot) {
    sxml_byte_view expected = {.bytes = NULL, .byte_count = 0};
    for (uint32_t role_index = 1; role_index < (uint32_t)SXML_READ_ROLE_COUNT; role_index += 1) {
        if ((slot->roles & ROLE_BIT(role_index)) != 0) {
            expected = role_rules[role_index].local_name;
            break;
        }
    }
    return report(reader, (read_error){.constraint_name = {.bytes = NULL, .byte_count = 0},
                                       .description = "A required child element is missing",
                                       .subject = expected});
}

/* Where matching children against the slots has reached. */
typedef struct slot_cursor {
    uint32_t slot_index;
    uint32_t count_in_slot;
    bool has_ended;
    /* Out: the role of the child last matched; NONE when not allowed. */
    read_role matched_role;
} slot_cursor;

/* Gives one child element its role from the current element's slots, or
   reports it (Part 1, appendix A: the content models of the schema for
   schema documents, all deterministic, so the first slot that takes a
   name is the one). */
static sxml_status match_child(schema_reader *reader, slot_cursor *cursor,
                               sxml_byte_view local_name) {
    const role_rule *rule = reader->element.rule;
    read_role *role = &cursor->matched_role;
    *role = SXML_READ_ROLE_NONE;
    uint32_t found_slot = rule->slot_count;
    if (!cursor->has_ended) {
        for (uint32_t slot_index = cursor->slot_index; slot_index < rule->slot_count;
             slot_index += 1) {
            const child_slot *slot = &rule->slots[slot_index];
            const read_role candidate = role_in_slot(slot, local_name);
            const bool is_full =
                slot_index == cursor->slot_index && cursor->count_in_slot > 0 && !slot->repeats;
            if (candidate != SXML_READ_ROLE_NONE && !is_full) {
                *role = candidate;
                found_slot = slot_index;
                break;
            }
        }
    }
    if (*role == SXML_READ_ROLE_NONE) {
        return report(reader, (read_error){.constraint_name = {.bytes = NULL, .byte_count = 0},
                                           .description = "A child element is not allowed here "
                                                          "in the schema document",
                                           .subject = local_name});
    }
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t slot_index = cursor->slot_index;
         status == SXML_STATUS_OK && slot_index < found_slot; slot_index += 1) {
        const bool is_filled = slot_index == cursor->slot_index && cursor->count_in_slot > 0;
        if (rule->slots[slot_index].is_required && !is_filled) {
            status = report_missing_child(reader, &rule->slots[slot_index]);
        }
    }
    if (found_slot != cursor->slot_index) {
        cursor->slot_index = found_slot;
        cursor->count_in_slot = 0;
    }
    cursor->count_in_slot += 1;
    if (rule->slots[found_slot].ends_content) {
        cursor->has_ended = true;
    }
    return status;
}

/* Gives the current element's children their roles (an ignored or
   misplaced one keeps NONE) and reports what its slots do not allow:
   misplaced or missing children, text in element-only content. */
static sxml_status assign_child_roles(schema_reader *reader) {
    element_state *element = &reader->element;
    element->child_roles = 0;
    if (element->rule->skips_content) {
        return SXML_STATUS_OK;
    }
    slot_cursor cursor = {.slot_index = 0,
                          .count_in_slot = 0,
                          .has_ended = false,
                          .matched_role = SXML_READ_ROLE_NONE};
    sxml_status status = SXML_STATUS_OK;
    bool has_child = element->description.has_children;
    uint32_t child = element->description.first_child.index;
    while (status == SXML_STATUS_OK && has_child) {
        sxml_node_description description;
        status = describe_node(reader, child, &description);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (description.node_kind == SXML_NODE_KIND_TEXT) {
            sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
            status = sxml_node_get_content(node_reference(reader, child), &text);
            if (status == SXML_STATUS_OK && !sxml_xml_text_is_whitespace(text)) {
                status =
                    report(reader, (read_error){.constraint_name = {.bytes = NULL, .byte_count = 0},
                                                .description = "Text is not allowed in "
                                                               "this element of the "
                                                               "schema document",
                                                .subject = sxml_xml_text_trim_whitespace(text)});
            }
        } else if (description.node_kind == SXML_NODE_KIND_ELEMENT) {
            inclusion_decision inclusion = {.is_excluded = false};
            status = decide_inclusion(reader, child, &inclusion);
            const bool excluded = inclusion.is_excluded;
            const sxml_name_parts parts = name_parts_of(reader, description.node_name);
            read_role role = SXML_READ_ROLE_NONE;
            if (status == SXML_STATUS_OK && !excluded) {
                if (!is_schema_namespace(parts.namespace_uri)) {
                    if (!element->rule->allows_foreign_children) {
                        status =
                            report(reader,
                                   (read_error){.constraint_name = {.bytes = NULL, .byte_count = 0},
                                                .description = "An element of another "
                                                               "namespace is not allowed "
                                                               "here in the schema document",
                                                .subject = parts.local_name});
                    }
                } else {
                    status = match_child(reader, &cursor, parts.local_name);
                    role = cursor.matched_role;
                }
            }
            if (is_in_range(reader, child)) {
                node_record *record = record_of(reader, child);
                record->role = role;
                record->has_xml_base = element->record->has_xml_base;
            }
            element->child_roles |= role == SXML_READ_ROLE_NONE ? 0 : ROLE_BIT(role);
        }
        has_child = description.has_next_sibling;
        child = description.next_sibling.index;
    }
    for (uint32_t slot_index = cursor.slot_index;
         status == SXML_STATUS_OK && !cursor.has_ended && slot_index < element->rule->slot_count;
         slot_index += 1) {
        const bool is_filled = slot_index == cursor.slot_index && cursor.count_in_slot > 0;
        if (element->rule->slots[slot_index].is_required && !is_filled) {
            status = report_missing_child(reader, &element->rule->slots[slot_index]);
        }
    }
    return status;
}

static bool has_child_role(const schema_reader *reader, uint64_t roles) {
    return (reader->element.child_roles & roles) != 0;
}

/* ------------------------------------------------------- Base URIs */

/* The base URI of the current element: the document's, with the xml:base
   attributes of the element and its ancestors resolved against it (XDM
   4.0, 7.3; RFC 3986, 5). Valid until the next call. */
static sxml_status current_base_uri(schema_reader *reader, sxml_byte_view *base_uri) {
    *base_uri = reader->request->document_uri;
    if (!reader->element.record->has_xml_base) {
        return SXML_STATUS_OK;
    }
    /* The xml:base values from the element up, applied outermost
       first. */
    sxml_xml_buffer *buffer = &reader->base_uri;
    buffer->byte_count = 0;
    sxml_status status = sxml_xml_buffer_append(buffer, reader->request->document_uri);
    uint32_t ancestors[64];
    size_t ancestor_count = 0;
    uint32_t node = reader->element.node.index;
    bool has_node = true;
    while (status == SXML_STATUS_OK && has_node && ancestor_count < 64U) {
        ancestors[ancestor_count] = node;
        ancestor_count += 1;
        sxml_node_description description;
        status = describe_node(reader, node, &description);
        has_node = description.has_parent;
        node = description.parent.index;
    }
    for (size_t ancestor_index = ancestor_count; status == SXML_STATUS_OK && ancestor_index > 0;
         ancestor_index -= 1) {
        const uint32_t element = ancestors[ancestor_index - 1];
        sxml_node_description description;
        status = describe_node(reader, element, &description);
        for (uint32_t attribute_index = 1;
             status == SXML_STATUS_OK && description.node_kind == SXML_NODE_KIND_ELEMENT &&
             attribute_index <= description.attribute_count;
             attribute_index += 1) {
            sxml_node_description attribute;
            status = describe_node(reader, element + attribute_index, &attribute);
            const sxml_name_parts parts = name_parts_of(reader, attribute.node_name);
            if (status != SXML_STATUS_OK ||
                parts.namespace_uri.index != (uint32_t)SXML_NAMESPACE_XML ||
                !sxml_byte_view_equals(parts.local_name, SXML_TEXT("base"))) {
                continue;
            }
            sxml_byte_view reference = {.bytes = NULL, .byte_count = 0};
            status = sxml_node_get_content(node_reference(reader, element + attribute_index),
                                           &reference);
            sxml_uri_resolution resolution = {.uri = {.bytes = NULL, .byte_count = 0},
                                              .memory = NULL};
            if (status == SXML_STATUS_OK) {
                status = sxml_uri_resolve(
                    &(sxml_uri_resolution_request){
                        .base_uri = {.bytes = buffer->bytes, .byte_count = buffer->byte_count},
                        .reference = sxml_xml_text_trim_whitespace(reference),
                        .allocator = reader->allocator},
                    &resolution);
            }
            if (status == SXML_STATUS_OK) {
                sxml_xml_buffer resolved = sxml_xml_buffer_empty(reader->allocator);
                status = sxml_xml_buffer_append(&resolved, resolution.uri);
                sxml_allocator_release(reader->allocator, resolution.memory);
                if (status == SXML_STATUS_OK) {
                    sxml_xml_buffer_release(buffer);
                    *buffer = resolved;
                } else {
                    sxml_xml_buffer_release(&resolved);
                }
            }
        }
    }
    if (status == SXML_STATUS_OK) {
        *base_uri = (sxml_byte_view){.bytes = buffer->bytes, .byte_count = buffer->byte_count};
    }
    return status;
}

/* ------------------------------------------------ Namespace contexts */

/* xpathDefaultNamespace resolved for the current element (Part 1,
   3.13.2): ##defaultNamespace, ##targetNamespace, ##local or a URI. */
static sxml_status read_xpath_default_namespace(schema_reader *reader, sxml_byte_view text,
                                                sxml_namespace_uri_identifier *namespace_uri) {
    const sxml_byte_view token = sxml_xml_text_trim_whitespace(text);
    if (sxml_byte_view_equals(token, SXML_TEXT("##defaultNamespace"))) {
        prefix_lookup lookup = {.is_bound = false, .namespace_uri = {.index = 0}};
        sxml_status status =
            lookup_prefix(reader, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, &lookup);
        *namespace_uri = lookup.namespace_uri;
        return status;
    }
    if (sxml_byte_view_equals(token, SXML_TEXT("##targetNamespace"))) {
        *namespace_uri = reader->request->target_namespace;
        return SXML_STATUS_OK;
    }
    if (sxml_byte_view_equals(token, SXML_TEXT("##local"))) {
        *namespace_uri = (sxml_namespace_uri_identifier){.index = SXML_NAMESPACE_NONE};
        return SXML_STATUS_OK;
    }
    return intern_uri(reader, token, namespace_uri);
}

static bool bindings_equal(sxml_namespace_binding_view first, sxml_namespace_binding_view second) {
    if (first.namespace_binding_count != second.namespace_binding_count) {
        return false;
    }
    for (size_t binding_index = 0; binding_index < first.namespace_binding_count;
         binding_index += 1) {
        const sxml_namespace_binding one = first.namespace_bindings[binding_index];
        const sxml_namespace_binding other = second.namespace_bindings[binding_index];
        if (one.namespace_prefix.index != other.namespace_prefix.index ||
            one.namespace_uri.index != other.namespace_uri.index) {
            return false;
        }
    }
    return true;
}

/* The namespace context of the current element (glossary
   namespace_context): its in-scope namespaces, its xpathDefaultNamespace
   (else the document's) and its base URI; the last one added when it is
   the same. */
static sxml_status current_namespace_context(schema_reader *reader,
                                             sxml_schema_namespace_context_index *context) {
    sxml_namespace_uri_identifier default_namespace = reader->defaults->xpath_default_namespace;
    sxml_status status = SXML_STATUS_OK;
    if (has_attribute(reader, SXML_READ_ATTRIBUTE_XPATH_DEFAULT_NAMESPACE)) {
        status = read_xpath_default_namespace(
            reader, attribute_value(reader, SXML_READ_ATTRIBUTE_XPATH_DEFAULT_NAMESPACE),
            &default_namespace);
    } else if (reader->defaults->xpath_default_is_default_namespace) {
        /* 3.13.2: ##defaultNamespace of xs:schema is the default namespace
           in scope on the host element. */
        status = read_xpath_default_namespace(reader, SXML_TEXT("##defaultNamespace"),
                                              &default_namespace);
    }
    sxml_byte_view base_uri = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = current_base_uri(reader, &base_uri);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_schema_namespace_context *last =
        sxml_schema_get_namespace_context(reader->components, reader->last_context);
    if (reader->last_context.value != 0 &&
        last->default_namespace.index == default_namespace.index &&
        sxml_byte_view_equals(sxml_schema_get_text(reader->components, last->base_uri), base_uri) &&
        bindings_equal(
            sxml_schema_get_namespace_bindings(reader->components, last->namespace_bindings),
            reader->element.bindings)) {
        *context = reader->last_context;
        return SXML_STATUS_OK;
    }
    sxml_schema_namespace_context entry = {.default_namespace = default_namespace};
    status = sxml_schema_components_add_namespace_bindings(
        reader->components, reader->element.bindings, &entry.namespace_bindings);
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_components_add_text(reader->components, base_uri, &entry.base_uri);
    }
    if (status == SXML_STATUS_OK) {
        status = add_component(
            reader,
            (component_entry){.table = SXML_SCHEMA_TABLE_NAMESPACE_CONTEXT, .entry = &entry},
            &reader->last_context.value);
    }
    if (status == SXML_STATUS_OK) {
        *context = reader->last_context;
    }
    return status;
}

/* An XPath record of the current element (glossary xpath_record) for
   the attribute's expression. */
static sxml_status add_xpath_record(schema_reader *reader, read_attribute attribute,
                                    sxml_schema_xpath_record_index *record) {
    sxml_schema_xpath_record entry = {.position = reader->element.position};
    sxml_status status = current_namespace_context(reader, &entry.namespace_context);
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_components_add_text(
            reader->components, attribute_value(reader, attribute), &entry.expression);
    }
    if (status == SXML_STATUS_OK) {
        status = add_component(
            reader, (component_entry){.table = SXML_SCHEMA_TABLE_XPATH_RECORD, .entry = &entry},
            &record->value);
    }
    return status;
}

/* The default= or fixed= of the current element as a value constraint;
   both is src-element.1 or src-attribute.1. */
static sxml_status read_value_constraint(schema_reader *reader,
                                         sxml_schema_value_constraint *constraint) {
    *constraint = (sxml_schema_value_constraint){.variety = SXML_SCHEMA_VALUE_CONSTRAINT_NONE};
    const bool has_default = has_attribute(reader, SXML_READ_ATTRIBUTE_DEFAULT);
    const bool has_fixed = has_attribute(reader, SXML_READ_ATTRIBUTE_FIXED);
    if (!has_default && !has_fixed) {
        return SXML_STATUS_OK;
    }
    sxml_status status = SXML_STATUS_OK;
    if (has_default && has_fixed) {
        status =
            report(reader, (read_error){.constraint_name = {.bytes = NULL, .byte_count = 0},
                                        .description = "default and fixed must not both be present",
                                        .subject = {.bytes = NULL, .byte_count = 0}});
    }
    const read_attribute attribute =
        has_default ? SXML_READ_ATTRIBUTE_DEFAULT : SXML_READ_ATTRIBUTE_FIXED;
    constraint->variety =
        has_default ? SXML_SCHEMA_VALUE_CONSTRAINT_DEFAULT : SXML_SCHEMA_VALUE_CONSTRAINT_FIXED;
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_components_add_text(
            reader->components, attribute_value(reader, attribute), &constraint->lexical_form);
    }
    if (status == SXML_STATUS_OK) {
        status = current_namespace_context(reader, &constraint->namespace_context);
    }
    return status;
}

/* ------------------------------------------------------- Global names */

/* True for a child of the xs:redefine or xs:override element a REDEFINE
   or OVERRIDE read starts at: a component that replaces another. */
static bool is_redefined_child(const schema_reader *reader) {
    return reader->request->read_kind != SXML_SCHEMA_READ_KIND_DOCUMENT &&
           reader->element.node.index != reader->first_node &&
           reader->element.parent_record == record_of(reader, reader->first_node);
}

/* Enters a global component in the name index (Part 1, 3.17.6.1,
   sch-props-correct.2: no two of one name in one symbol space); a
   redefine's or override's child replaces the original's entry. */
typedef struct global_entry {
    sxml_schema_symbol_space symbol_space;
    sxml_name_identifier name;
    uint32_t component;
} global_entry;

static sxml_status enter_global(schema_reader *reader, global_entry entry) {
    const sxml_schema_name_entry name_entry = {
        .symbol_space = entry.symbol_space, .name = entry.name, .component = entry.component};
    if (entry.name.index == 0 || entry.component == 0) {
        return SXML_STATUS_OK;
    }
    if (is_redefined_child(reader)) {
        reader->redefinition.replacement = entry.component;
        return sxml_schema_components_replace_name(reader->components, &name_entry);
    }
    uint32_t existing = 0;
    sxml_status status =
        sxml_schema_components_enter_name(reader->components, &name_entry, &existing);
    if (status != SXML_STATUS_OK || existing == 0) {
        return status;
    }
    if (reader->request->read_kind != SXML_SCHEMA_READ_KIND_DOCUMENT &&
        entry.symbol_space == SXML_SCHEMA_SYMBOL_SPACE_IDENTITY_CONSTRAINT) {
        /* The identity constraints of a redefinition stand for the
           original's. */
        return sxml_schema_components_replace_name(reader->components, &name_entry);
    }
    if (entry.symbol_space == SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION &&
        sxml_schema_get_type_definition(reader->components,
                                        (sxml_schema_type_definition_index){.value = existing})
            ->is_builtin) {
        /* A schema document for the XSD namespace (the schema for schema
           documents, say) restating a built-in type: every schema has the
           built-in components (4.2.1, "schema(D)"), which a processor
           knows without reading (C.2.1, hard-coded schemas), and two
           top-level type definitions of one name are one component (3.4.6.5,
           the note on component identity); its own definition is left
           unnamed, so sch-props-correct.2 does not hold against it. */
        return SXML_STATUS_OK;
    }
    return report(reader, (read_error){.constraint_name = SXML_TEXT("sch-props-correct.2"),
                                       .description = "Two global components of one kind have "
                                                      "the same name",
                                       .subject = name_parts_of(reader, entry.name).local_name});
}

/* The name attribute of a global component, in the target namespace. */
static sxml_status global_name(schema_reader *reader, sxml_name_identifier *name) {
    name->index = 0;
    if (!has_attribute(reader, SXML_READ_ATTRIBUTE_NAME)) {
        return SXML_STATUS_OK;
    }
    const sxml_byte_view local_name = sxml_xml_text_trim_whitespace(attribute_value(reader, SXML_READ_ATTRIBUTE_NAME));
    if (!sxml_xml_text_is_ncname(local_name)) {
        return SXML_STATUS_OK;
    }
    return intern_name(reader,
                       (sxml_name_request){.namespace_uri = reader->request->target_namespace,
                                           .local_name = local_name},
                       name);
}

/* The symbol space of a role's global component; NONE for the others. */
static const sxml_schema_symbol_space role_symbol_spaces[SXML_READ_ROLE_COUNT] = {
    [SXML_READ_ROLE_GLOBAL_SIMPLE_TYPE] = SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION,
    [SXML_READ_ROLE_GLOBAL_COMPLEX_TYPE] = SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION,
    [SXML_READ_ROLE_GLOBAL_GROUP] = SXML_SCHEMA_SYMBOL_SPACE_MODEL_GROUP_DEFINITION,
    [SXML_READ_ROLE_GLOBAL_ATTRIBUTE_GROUP] = SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_GROUP,
    [SXML_READ_ROLE_GLOBAL_ELEMENT] = SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION,
    [SXML_READ_ROLE_GLOBAL_ATTRIBUTE] = SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_DECLARATION,
    [SXML_READ_ROLE_NOTATION] = SXML_SCHEMA_SYMBOL_SPACE_NOTATION_DECLARATION,
};

/* The table of a symbol space's components. */
static sxml_schema_table table_of_symbol_space(sxml_schema_symbol_space symbol_space) {
    static const sxml_schema_table tables[] = {
        [SXML_SCHEMA_SYMBOL_SPACE_NONE] = SXML_SCHEMA_TABLE_TYPE_DEFINITION,
        [SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION] = SXML_SCHEMA_TABLE_TYPE_DEFINITION,
        [SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION] = SXML_SCHEMA_TABLE_ELEMENT_DECLARATION,
        [SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_DECLARATION] = SXML_SCHEMA_TABLE_ATTRIBUTE_DECLARATION,
        [SXML_SCHEMA_SYMBOL_SPACE_MODEL_GROUP_DEFINITION] =
            SXML_SCHEMA_TABLE_MODEL_GROUP_DEFINITION,
        [SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_GROUP] = SXML_SCHEMA_TABLE_ATTRIBUTE_GROUP,
        [SXML_SCHEMA_SYMBOL_SPACE_IDENTITY_CONSTRAINT] = SXML_SCHEMA_TABLE_IDENTITY_CONSTRAINT,
        [SXML_SCHEMA_SYMBOL_SPACE_NOTATION_DECLARATION] = SXML_SCHEMA_TABLE_NOTATION_DECLARATION,
    };
    return tables[symbol_space];
}

/* The document a global component was read from (its position). */
static sxml_schema_document_index component_document(const schema_reader *reader,
                                                     component_key key) {
    const sxml_schema_components *components = reader->components;
    switch (key.table) {
    case SXML_SCHEMA_TABLE_TYPE_DEFINITION:
        return sxml_schema_get_type_definition(
                   components, (sxml_schema_type_definition_index){.value = key.index})
            ->position.document;
    case SXML_SCHEMA_TABLE_ELEMENT_DECLARATION:
        return sxml_schema_get_element_declaration(
                   components, (sxml_schema_element_declaration_index){.value = key.index})
            ->position.document;
    case SXML_SCHEMA_TABLE_ATTRIBUTE_DECLARATION:
        return sxml_schema_get_attribute_declaration(
                   components, (sxml_schema_attribute_declaration_index){.value = key.index})
            ->position.document;
    case SXML_SCHEMA_TABLE_MODEL_GROUP_DEFINITION:
        return sxml_schema_get_model_group_definition(
                   components, (sxml_schema_model_group_definition_index){.value = key.index})
            ->position.document;
    case SXML_SCHEMA_TABLE_ATTRIBUTE_GROUP:
        return sxml_schema_get_attribute_group(
                   components, (sxml_schema_attribute_group_index){.value = key.index})
            ->position.document;
    case SXML_SCHEMA_TABLE_IDENTITY_CONSTRAINT:
        return sxml_schema_get_identity_constraint(
                   components, (sxml_schema_identity_constraint_index){.value = key.index})
            ->position.document;
    case SXML_SCHEMA_TABLE_NOTATION_DECLARATION:
        return sxml_schema_get_notation_declaration(
                   components, (sxml_schema_notation_declaration_index){.value = key.index})
            ->position.document;
    case SXML_SCHEMA_TABLE_ATTRIBUTE_USE:
    case SXML_SCHEMA_TABLE_MODEL_GROUP:
    case SXML_SCHEMA_TABLE_PARTICLE:
    case SXML_SCHEMA_TABLE_WILDCARD:
    case SXML_SCHEMA_TABLE_TYPE_ALTERNATIVE:
    case SXML_SCHEMA_TABLE_ASSERTION:
    case SXML_SCHEMA_TABLE_FACET:
    case SXML_SCHEMA_TABLE_XPATH_RECORD:
    case SXML_SCHEMA_TABLE_NAMESPACE_CONTEXT:
    case SXML_SCHEMA_TABLE_REFERENCE:
    case SXML_SCHEMA_TABLE_DOCUMENT:
    case SXML_SCHEMA_TABLE_LIST_ENTRY:
    case SXML_SCHEMA_TABLE_NAMESPACE_BINDING:
    case SXML_SCHEMA_TABLE_TEXT_BYTE:
    case SXML_SCHEMA_TABLE_COUNT:
        break;
    }
    return (sxml_schema_document_index){.value = 0};
}

/* A child of xs:redefine or xs:override: finds the component it replaces
   (Part 1, 4.2.4, src-redefine; 4.2.5: one that replaces nothing is not
   read). */
static sxml_status begin_redefinition(schema_reader *reader) {
    reader->redefinition.is_active = false;
    const sxml_schema_symbol_space symbol_space = role_symbol_spaces[reader->element.role];
    sxml_name_identifier name = {.index = 0};
    sxml_status status = global_name(reader, &name);
    if (status != SXML_STATUS_OK || name.index == 0 ||
        symbol_space == SXML_SCHEMA_SYMBOL_SPACE_NONE) {
        return status;
    }
    uint32_t original = 0;
    if (!sxml_schema_components_find_name(
            reader->components, (sxml_schema_name_key){.symbol_space = symbol_space, .name = name},
            &original)) {
        reader->element.skips_subtree = true;
        if (reader->request->read_kind == SXML_SCHEMA_READ_KIND_OVERRIDE) {
            return SXML_STATUS_OK;
        }
        return report(reader, (read_error){.constraint_name = SXML_TEXT("src-redefine.2"),
                                           .description = "A redefinition names no component "
                                                          "of the redefined schema document",
                                           .subject = name_parts_of(reader, name).local_name});
    }
    const bool is_redefine = reader->request->read_kind == SXML_SCHEMA_READ_KIND_REDEFINE;
    sxml_schema_redefinition_note note = {.is_first = true};
    status = sxml_schema_assembly_note_redefinition(
        reader->assembly,
        (sxml_schema_redefinition_key){.name = {.symbol_space = symbol_space, .name = name},
                                       .redefined_document = reader->request->redefined_document},
        &note);
    if (status == SXML_STATUS_OK && !note.is_first) {
        reader->element.skips_subtree = true;
        return report(reader,
                      (read_error){.constraint_name = is_redefine ? SXML_TEXT("src-redefine")
                                                                  : SXML_TEXT("src-override"),
                                   .description = "A component is redefined or "
                                                  "overridden twice",
                                   .subject = name_parts_of(reader, name).local_name});
    }
    if (status == SXML_STATUS_OK && is_redefine &&
        component_document(reader, (component_key){.table = table_of_symbol_space(symbol_space),
                                                   .index = original})
                .value == reader->request->document.value) {
        /* The original is the redefining document's own, not the
           redefined one's (src-redefine.6.2.1, 7.2.1). */
        reader->element.skips_subtree = true;
        return report(reader, (read_error){.constraint_name = SXML_TEXT("src-redefine.2"),
                                           .description = "A redefinition names no component "
                                                          "of the redefined schema document",
                                           .subject = name_parts_of(reader, name).local_name});
    }
    reader->redefinition =
        (redefinition_state){.is_active = true,
                             .symbol_space = symbol_space,
                             .name = name,
                             .original = original,
                             .replacement = 0,
                             .last_node = reader->element.description.last_descendant.index,
                             .position = reader->element.position,
                             .has_self_derivation = false,
                             .self_reference_count = 0,
                             .has_counted_self_reference = false};
    return status;
}

/* A pre-resolved reference to a component (sxml_schema_reference). */
typedef struct resolved_reference {
    sxml_schema_symbol_space symbol_space;
    uint32_t component;
} resolved_reference;

static sxml_status add_resolved_reference(schema_reader *reader, resolved_reference request,
                                          uint32_t *reference) {
    const sxml_schema_reference entry = {.symbol_space = request.symbol_space,
                                         .name = reader->redefinition.name,
                                         .position = reader->redefinition.position,
                                         .component = request.component};
    return add_component(reader,
                         (component_entry){.table = SXML_SCHEMA_TABLE_REFERENCE, .entry = &entry},
                         reference);
}

/* One of the two complex types a redefinition's restriction check is made
   of: for a group, its content the group's model group; for an attribute
   group, its attributes the group's. */
typedef struct restriction_side {
    uint32_t group;
    /* The base type (0: xs:anyType, the base side). */
    uint32_t base_type;
} restriction_side;

static sxml_status add_restriction_side(schema_reader *reader, restriction_side side,
                                        uint32_t *type) {
    const redefinition_state *redefinition = &reader->redefinition;
    sxml_schema_type_definition definition = {
        .variety = SXML_TYPE_VARIETY_COMPLEX,
        .position = redefinition->position,
        .derivation_method = SXML_SCHEMA_DERIVATION_RESTRICTION};
    sxml_status status = SXML_STATUS_OK;
    if (side.base_type != 0) {
        status = add_resolved_reference(
            reader,
            (resolved_reference){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION,
                                 .component = side.base_type},
            &definition.base_type_reference.value);
    }
    const bool is_group =
        redefinition->symbol_space == SXML_SCHEMA_SYMBOL_SPACE_MODEL_GROUP_DEFINITION;
    const uint32_t model_group =
        is_group ? sxml_schema_get_model_group_definition(
                       reader->components,
                       (sxml_schema_model_group_definition_index){.value = side.group})
                       ->model_group.value
                 : 0;
    if (status == SXML_STATUS_OK && is_group && model_group != 0) {
        const sxml_schema_particle particle = {.position = redefinition->position,
                                               .minimum_occurrences = 1,
                                               .maximum_occurrences = 1,
                                               .term_kind = SXML_SCHEMA_TERM_MODEL_GROUP,
                                               .term = model_group};
        status = add_component(reader,
                               (component_entry){.table = SXML_SCHEMA_TABLE_PARTICLE,
                                                 .entry = &particle},
                               &definition.explicit_content.value);
    } else if (status == SXML_STATUS_OK && !is_group) {
        uint32_t reference = 0;
        status = add_resolved_reference(
            reader,
            (resolved_reference){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_GROUP,
                                 .component = side.group},
            &reference);
        if (status == SXML_STATUS_OK) {
            status = sxml_schema_components_add_list(
                reader->components,
                (sxml_schema_index_view){.indexes = &reference, .index_count = 1},
                &definition.attribute_group_references);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = add_component(reader,
                               (component_entry){.table = SXML_SCHEMA_TABLE_TYPE_DEFINITION,
                                                 .entry = &definition},
                               type);
    }
    return status;
}

/* src-redefine.6.2.2 and 7.2.2 (Part 1, 4.2.4): a group or attribute
   group redefined without a reference to itself must be a valid
   restriction of the original. The check is a restriction of complex
   types, so COMPLEX_TYPES makes it with its own rules
   (cos-content-act-restrict for the content, derivation-ok-restriction
   clauses 2 to 4 for the attributes, as 6.2.2 and 7.2.2 say): an
   unnamed type of the original's content or attributes, and an unnamed
   restriction of it of the redefinition's. */
static sxml_status add_restriction_check(schema_reader *reader) {
    uint32_t base_type = 0;
    sxml_status status = add_restriction_side(
        reader, (restriction_side){.group = reader->redefinition.original, .base_type = 0},
        &base_type);
    uint32_t derived_type = 0;
    if (status == SXML_STATUS_OK) {
        status = add_restriction_side(
            reader,
            (restriction_side){.group = reader->redefinition.replacement, .base_type = base_type},
            &derived_type);
    }
    return status;
}

/* The end of an xs:redefine child (Part 1, 4.2.4): a type derives from
   itself (src-redefine.5); a group or attribute group refers to itself at
   most once (src-redefine.6.1.1, 7.1), a group exactly once (6.1.2), and
   without a reference to itself restricts the original (6.2.2, 7.2.2;
   add_restriction_check). */
static sxml_status end_redefinition(schema_reader *reader) {
    redefinition_state *redefinition = &reader->redefinition;
    if (!redefinition->is_active) {
        return SXML_STATUS_OK;
    }
    redefinition->is_active = false;
    if (reader->request->read_kind != SXML_SCHEMA_READ_KIND_REDEFINE) {
        return SXML_STATUS_OK;
    }
    const char *description = NULL;
    sxml_byte_view constraint_name = {.bytes = NULL, .byte_count = 0};
    if (redefinition->symbol_space == SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION &&
        !redefinition->has_self_derivation) {
        constraint_name = SXML_TEXT("src-redefine.5");
        description = "A redefined type must be derived from itself";
    } else if (redefinition->symbol_space != SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION &&
               redefinition->self_reference_count > 1U) {
        constraint_name = redefinition->symbol_space == SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_GROUP
                              ? SXML_TEXT("src-redefine.7.1")
                              : SXML_TEXT("src-redefine.6.1.1");
        description = "A redefinition refers to itself more than once";
    } else if (redefinition->has_counted_self_reference) {
        constraint_name = SXML_TEXT("src-redefine.6.1.2");
        description = "A redefined group's reference to itself has minOccurs and maxOccurs 1";
    }
    if (description == NULL && redefinition->self_reference_count == 0 &&
        redefinition->symbol_space != SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION &&
        redefinition->replacement != 0) {
        return add_restriction_check(reader);
    }
    if (description == NULL) {
        return SXML_STATUS_OK;
    }
    return report_at(reader,
                     (read_error){.constraint_name = constraint_name,
                                  .description = description,
                                  .subject = name_parts_of(reader, redefinition->name).local_name},
                     redefinition->position);
}

/* --------------------------------------------------------- Attaching */

/* The component of the parent the current element attaches to. */
static component_key parent_owner(const schema_reader *reader) {
    return reader->element.parent_record->owner;
}

/* The current element joins a list of its parent's owner. */
static void join_list(schema_reader *reader, list_field field, uint32_t primary) {
    reader->element.record->joined_list = field;
    reader->element.record->primary = primary;
}

/* Gives a local type to the parent's owner: an element declaration, an
   attribute declaration, a type alternative, a simple type (base or item
   type) or a union (a member type). */
static sxml_status attach_local_type(schema_reader *reader, uint32_t type) {
    const component_key owner = parent_owner(reader);
    const read_role parent_role = reader->element.parent_role;
    if (parent_role == SXML_READ_ROLE_UNION) {
        join_list(reader, SXML_LIST_FIELD_MEMBER_TYPES, type);
        return SXML_STATUS_OK;
    }
    if (parent_role == SXML_READ_ROLE_SIMPLE_RESTRICTION ||
        parent_role == SXML_READ_ROLE_SIMPLE_CONTENT_RESTRICTION ||
        parent_role == SXML_READ_ROLE_LIST) {
        sxml_schema_type_definition *definition = edit_type(reader, owner.index);
        if (definition != NULL && parent_role == SXML_READ_ROLE_LIST) {
            definition->anonymous_item_type.value = type;
        } else if (definition != NULL) {
            definition->anonymous_base_type.value = type;
        }
        return SXML_STATUS_OK;
    }
    if (owner.table == SXML_SCHEMA_TABLE_ELEMENT_DECLARATION) {
        sxml_schema_element_declaration *declaration = edit_component(reader, owner);
        if (declaration != NULL) {
            declaration->anonymous_type.value = type;
        }
    } else if (owner.table == SXML_SCHEMA_TABLE_ATTRIBUTE_DECLARATION) {
        sxml_schema_attribute_declaration *declaration = edit_component(reader, owner);
        if (declaration != NULL) {
            declaration->anonymous_type.value = type;
        }
    } else if (owner.table == SXML_SCHEMA_TABLE_TYPE_ALTERNATIVE) {
        sxml_schema_type_alternative *alternative = edit_component(reader, owner);
        if (alternative != NULL) {
            alternative->anonymous_type.value = type;
        }
    }
    return SXML_STATUS_OK;
}

/* Gives a particle to the parent: a model group's particles, or a complex
   type's explicit content unless that is empty (Part 1, 3.4.2.3.3,
   clause 2.1). */
/* A particle to give the parent, and whether it is empty content. */
typedef struct particle_attachment {
    uint32_t particle;
    bool is_empty;
} particle_attachment;

static void attach_particle(schema_reader *reader, particle_attachment attachment) {
    const uint32_t particle = attachment.particle;
    const bool is_empty = attachment.is_empty;
    const component_key owner = parent_owner(reader);
    if (owner.table == SXML_SCHEMA_TABLE_MODEL_GROUP && owner.index != 0) {
        join_list(reader, SXML_LIST_FIELD_PARTICLES, particle);
    } else if (owner.table == SXML_SCHEMA_TABLE_TYPE_DEFINITION && owner.index != 0 && !is_empty) {
        sxml_schema_type_definition *definition = edit_type(reader, owner.index);
        if (definition != NULL) {
            definition->explicit_content.value = particle;
        }
    }
}

/* ------------------------------------------------------- Schema element */

static sxml_status read_schema_defaults(schema_reader *reader) {
    sxml_schema_document_defaults *defaults = reader->defaults;
    if (has_attribute(reader, SXML_READ_ATTRIBUTE_TARGET_NAMESPACE) &&
        sxml_xml_text_trim_whitespace(attribute_value(reader, SXML_READ_ATTRIBUTE_TARGET_NAMESPACE)).byte_count == 0) {
        /* Part 1, 3.17.2: an absent target namespace has no attribute;
           the empty one is not a namespace name. */
        sxml_status status =
            report(reader, (read_error){.constraint_name = {.bytes = NULL, .byte_count = 0},
                                        .description = "targetNamespace is empty",
                                        .subject = {.bytes = NULL, .byte_count = 0}});
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    size_t position = 0;
    defaults->qualifies_elements =
        has_attribute(reader, SXML_READ_ATTRIBUTE_ELEMENT_FORM_DEFAULT) &&
        find_keyword(KEYWORDS(form_keywords),
                     attribute_value(reader, SXML_READ_ATTRIBUTE_ELEMENT_FORM_DEFAULT),
                     &position) &&
        position == 1;
    position = 0;
    defaults->qualifies_attributes =
        has_attribute(reader, SXML_READ_ATTRIBUTE_ATTRIBUTE_FORM_DEFAULT) &&
        find_keyword(KEYWORDS(form_keywords),
                     attribute_value(reader, SXML_READ_ATTRIBUTE_ATTRIBUTE_FORM_DEFAULT),
                     &position) &&
        position == 1;
    sxml_status status =
        read_derivation_set(reader,
                            (derivation_set_request){.attribute = SXML_READ_ATTRIBUTE_BLOCK_DEFAULT,
                                                     .allowed = DERIVATIONS_BLOCK,
                                                     .default_set = 0},
                            &defaults->block_default);
    if (status == SXML_STATUS_OK) {
        status = read_derivation_set(
            reader,
            (derivation_set_request){.attribute = SXML_READ_ATTRIBUTE_FINAL_DEFAULT,
                                     .allowed = DERIVATIONS_FULL,
                                     .default_set = 0},
            &defaults->final_default);
    }
    if (status == SXML_STATUS_OK &&
        has_attribute(reader, SXML_READ_ATTRIBUTE_XPATH_DEFAULT_NAMESPACE)) {
        status = read_xpath_default_namespace(
            reader, attribute_value(reader, SXML_READ_ATTRIBUTE_XPATH_DEFAULT_NAMESPACE),
            &defaults->xpath_default_namespace);
        defaults->xpath_default_is_default_namespace =
            sxml_byte_view_equals(sxml_xml_text_trim_whitespace(attribute_value(reader, SXML_READ_ATTRIBUTE_XPATH_DEFAULT_NAMESPACE)),
                        SXML_TEXT("##defaultNamespace"));
    }
    if (status == SXML_STATUS_OK && has_attribute(reader, SXML_READ_ATTRIBUTE_DEFAULT_ATTRIBUTES)) {
        status = add_reference(
            reader,
            (reference_request){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_GROUP,
                                .text =
                                    attribute_value(reader, SXML_READ_ATTRIBUTE_DEFAULT_ATTRIBUTES),
                                .may_name_original = false},
            &defaults->default_attributes);
    }
    /* The namespace context of the xs:schema element, which the text
       form's names take their prefixes from. */
    sxml_schema_namespace_context_index context = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = current_namespace_context(reader, &context);
    }
    defaults->is_read = true;
    return status;
}

/* An xs:include, xs:import, xs:redefine or xs:override (Part 1, 4.2.3 to
   4.2.6). */
static sxml_status read_document_reference(schema_reader *reader,
                                           sxml_schema_document_relation relation) {
    sxml_schema_document_reference reference = {
        .relation = relation,
        .referrer = reader->request,
        .element = reader->element.node,
        .position = reader->element.position,
        .schema_location = sxml_xml_text_trim_whitespace(attribute_value(reader, SXML_READ_ATTRIBUTE_SCHEMA_LOCATION)),
        .base_uri = {.bytes = NULL, .byte_count = 0},
        .imported_namespace = {.index = SXML_NAMESPACE_NONE}};
    sxml_status status = SXML_STATUS_OK;
    if (relation == SXML_SCHEMA_DOCUMENT_RELATION_IMPORT) {
        const bool has_namespace = has_attribute(reader, SXML_READ_ATTRIBUTE_NAMESPACE);
        if (has_namespace) {
            status = intern_uri(reader, attribute_value(reader, SXML_READ_ATTRIBUTE_NAMESPACE),
                                &reference.imported_namespace);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        /* src-import.1.1 and 1.2. */
        if (has_namespace &&
            reference.imported_namespace.index == reader->request->target_namespace.index) {
            return report(reader, (read_error){.constraint_name = SXML_TEXT("src-import.1.1"),
                                               .description = "An xs:import names the target "
                                                              "namespace of its own document",
                                               .subject = {.bytes = NULL, .byte_count = 0}});
        }
        if (!has_namespace && reader->request->target_namespace.index == SXML_NAMESPACE_NONE &&
            !reader->request->is_chameleon) {
            return report(reader, (read_error){.constraint_name = SXML_TEXT("src-import.1.2"),
                                               .description = "An xs:import without a namespace "
                                                              "is in a document without a "
                                                              "targetNamespace",
                                               .subject = {.bytes = NULL, .byte_count = 0}});
        }
    }
    if (relation == SXML_SCHEMA_DOCUMENT_RELATION_REDEFINE ||
        relation == SXML_SCHEMA_DOCUMENT_RELATION_OVERRIDE) {
        reader->element.skips_subtree = true;
    }
    status = current_base_uri(reader, &reference.base_uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_assembly_add_reference(reader->assembly, &reference);
    }
    return status;
}

/* ------------------------------------------------------- Simple types */

static sxml_status read_simple_type(schema_reader *reader) {
    sxml_schema_type_definition definition = {.position = reader->element.position};
    if (has_child_role(reader, ROLE_BIT(SXML_READ_ROLE_LIST))) {
        definition.variety = SXML_TYPE_VARIETY_LIST;
        definition.derivation_method = SXML_SCHEMA_DERIVATION_LIST;
    } else if (has_child_role(reader, ROLE_BIT(SXML_READ_ROLE_UNION))) {
        definition.variety = SXML_TYPE_VARIETY_UNION;
        definition.derivation_method = SXML_SCHEMA_DERIVATION_UNION;
    } else {
        /* RESOLVE gives a restriction its base's variety. */
        definition.variety = SXML_TYPE_VARIETY_ATOMIC;
        definition.derivation_method = SXML_SCHEMA_DERIVATION_RESTRICTION;
    }
    const bool is_global = reader->element.role == SXML_READ_ROLE_GLOBAL_SIMPLE_TYPE;
    sxml_status status = SXML_STATUS_OK;
    if (is_global) {
        status = global_name(reader, &definition.type_name);
        if (status == SXML_STATUS_OK) {
            status = read_derivation_set(
                reader,
                (derivation_set_request){.attribute = SXML_READ_ATTRIBUTE_FINAL,
                                         .allowed = DERIVATIONS_FULL,
                                         .default_set = reader->defaults->final_default},
                &definition.final_derivations);
        }
    }
    uint32_t index = 0;
    if (status == SXML_STATUS_OK) {
        status = add_component(
            reader,
            (component_entry){.table = SXML_SCHEMA_TABLE_TYPE_DEFINITION, .entry = &definition},
            &index);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    reader->element.record->owner =
        (component_key){.table = SXML_SCHEMA_TABLE_TYPE_DEFINITION, .index = index};
    if (is_global) {
        return enter_global(reader,
                            (global_entry){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION,
                                           .name = definition.type_name,
                                           .component = index});
    }
    return attach_local_type(reader, index);
}

/* True while reading the type an xs:redefine child redefines itself. */
static bool is_redefining_type(const schema_reader *reader) {
    return reader->redefinition.is_active &&
           reader->request->read_kind == SXML_SCHEMA_READ_KIND_REDEFINE &&
           reader->redefinition.symbol_space == SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION;
}

/* A base= of a restriction or extension; in a redefinition it must be the
   type's own name and is the original (src-redefine.5). */
static sxml_status read_base(schema_reader *reader, sxml_schema_type_definition_index type) {
    if (!has_attribute(reader, SXML_READ_ATTRIBUTE_BASE)) {
        return SXML_STATUS_OK;
    }
    const bool is_redefining =
        is_redefining_type(reader) && reader->redefinition.replacement == type.value;
    sxml_schema_reference_index reference = {.value = 0};
    sxml_status status =
        add_reference(reader,
                      (reference_request){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION,
                                          .text = attribute_value(reader, SXML_READ_ATTRIBUTE_BASE),
                                          .may_name_original = is_redefining},
                      &reference);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (is_redefining && reference.value != 0 &&
        sxml_schema_get_reference(reader->components, reference)->name.index ==
            reader->redefinition.name.index) {
        reader->redefinition.has_self_derivation = true;
    } else if (is_redefining && reference.value != 0) {
        status = report(reader,
                        (read_error){.constraint_name = SXML_TEXT("src-redefine.5"),
                                     .description = "A redefined type must be derived "
                                                    "from itself",
                                     .subject = attribute_value(reader, SXML_READ_ATTRIBUTE_BASE)});
    }
    sxml_schema_type_definition *definition = edit_type(reader, type.value);
    if (definition != NULL) {
        definition->base_type_reference = reference;
    }
    return status;
}

/* xs:restriction, xs:list, xs:union of a simple type (Part 1, 3.16.2;
   src-simple-type.2 to 4). */
static sxml_status read_simple_derivation(schema_reader *reader) {
    const component_key owner = parent_owner(reader);
    reader->element.record->owner = owner;
    const sxml_schema_type_definition_index type = {.value = owner.index};
    sxml_status status = SXML_STATUS_OK;
    const bool has_local_type = has_child_role(reader, ROLE_BIT(SXML_READ_ROLE_LOCAL_SIMPLE_TYPE));
    const read_role role = reader->element.role;
    if (role == SXML_READ_ROLE_SIMPLE_RESTRICTION) {
        if (has_attribute(reader, SXML_READ_ATTRIBUTE_BASE) == has_local_type) {
            status = report(reader, (read_error){.constraint_name = SXML_TEXT("src-simple-type.2"),
                                                 .description = "A restriction has either a base "
                                                                "attribute or a simpleType child",
                                                 .subject = {.bytes = NULL, .byte_count = 0}});
        }
        if (status == SXML_STATUS_OK) {
            status = read_base(reader, type);
        }
    } else if (role == SXML_READ_ROLE_LIST) {
        if (has_attribute(reader, SXML_READ_ATTRIBUTE_ITEM_TYPE) == has_local_type) {
            status = report(reader, (read_error){.constraint_name = SXML_TEXT("src-simple-type.3"),
                                                 .description = "A list has either an itemType "
                                                                "attribute or a simpleType child",
                                                 .subject = {.bytes = NULL, .byte_count = 0}});
        }
        if (status == SXML_STATUS_OK && has_attribute(reader, SXML_READ_ATTRIBUTE_ITEM_TYPE)) {
            sxml_schema_reference_index reference = {.value = 0};
            status = add_reference(
                reader,
                (reference_request){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION,
                                    .text = attribute_value(reader, SXML_READ_ATTRIBUTE_ITEM_TYPE),
                                    .may_name_original = false},
                &reference);
            sxml_schema_type_definition *definition = edit_type(reader, type.value);
            if (status == SXML_STATUS_OK && definition != NULL) {
                definition->item_type_reference = reference;
            }
        }
    } else if (role == SXML_READ_ROLE_UNION) {
        sxml_schema_list references = {.first_entry_index = 0, .entry_count = 0};
        if (has_attribute(reader, SXML_READ_ATTRIBUTE_MEMBER_TYPES)) {
            status = add_reference_list(
                reader,
                (reference_request){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION,
                                    .text =
                                        attribute_value(reader, SXML_READ_ATTRIBUTE_MEMBER_TYPES),
                                    .may_name_original = false},
                &references);
        }
        sxml_schema_type_definition *definition = edit_type(reader, type.value);
        if (status == SXML_STATUS_OK && definition != NULL) {
            definition->member_type_references = references;
        }
        if (status == SXML_STATUS_OK && references.entry_count == 0 && !has_local_type) {
            status = report(reader, (read_error){.constraint_name = SXML_TEXT("src-simple-type.4"),
                                                 .description = "A union has member types in its "
                                                                "memberTypes attribute or its "
                                                                "simpleType children",
                                                 .subject = {.bytes = NULL, .byte_count = 0}});
        }
    }
    return status;
}

/* A facet's value as the schema for schema documents types it (Part 2,
   4.3: the numeric facets nonNegativeInteger, totalDigits positiveInteger,
   whiteSpace and explicitTimezone keywords). */
static bool is_valid_facet_value(sxml_facet_kind kind, sxml_byte_view value) {
    uint64_t number = 0;
    size_t position = 0;
    static const sxml_byte_view whitespace_keywords[] = {SXML_BYTE_VIEW_INITIALIZER("preserve"),
                                                         SXML_BYTE_VIEW_INITIALIZER("replace"),
                                                         SXML_BYTE_VIEW_INITIALIZER("collapse")};
    static const sxml_byte_view timezone_keywords[] = {SXML_BYTE_VIEW_INITIALIZER("optional"),
                                                       SXML_BYTE_VIEW_INITIALIZER("required"),
                                                       SXML_BYTE_VIEW_INITIALIZER("prohibited")};
    if (kind == SXML_FACET_KIND_LENGTH || kind == SXML_FACET_KIND_MIN_LENGTH ||
        kind == SXML_FACET_KIND_MAX_LENGTH || kind == SXML_FACET_KIND_FRACTION_DIGITS) {
        return parse_non_negative_integer(value, &number);
    }
    if (kind == SXML_FACET_KIND_TOTAL_DIGITS) {
        return parse_non_negative_integer(value, &number) && number > 0;
    }
    if (kind == SXML_FACET_KIND_WHITESPACE) {
        return find_keyword(KEYWORDS(whitespace_keywords), value, &position);
    }
    if (kind == SXML_FACET_KIND_EXPLICIT_TIMEZONE) {
        return find_keyword(KEYWORDS(timezone_keywords), value, &position);
    }
    return true;
}

static sxml_status read_facet(schema_reader *reader) {
    const sxml_facet_kind kind = reader->element.rule->facet_kind;
    sxml_schema_facet facet = {.facet_kind = kind, .position = reader->element.position};
    sxml_status status = SXML_STATUS_OK;
    const sxml_byte_view value = attribute_value(reader, SXML_READ_ATTRIBUTE_VALUE);
    if (has_attribute(reader, SXML_READ_ATTRIBUTE_VALUE) && !is_valid_facet_value(kind, value)) {
        status = report_attribute_value(reader, SXML_READ_ATTRIBUTE_VALUE);
    }
    if (status == SXML_STATUS_OK && has_attribute(reader, SXML_READ_ATTRIBUTE_FIXED)) {
        const boolean_value_text fixed =
            parse_boolean(attribute_value(reader, SXML_READ_ATTRIBUTE_FIXED));
        facet.is_fixed = fixed.value;
        if (!fixed.is_valid) {
            status = report_attribute_value(reader, SXML_READ_ATTRIBUTE_FIXED);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_components_add_text(reader->components, value, &facet.value);
    }
    if (status == SXML_STATUS_OK) {
        status = current_namespace_context(reader, &facet.namespace_context);
    }
    uint32_t index = 0;
    if (status == SXML_STATUS_OK) {
        status = add_component(
            reader, (component_entry){.table = SXML_SCHEMA_TABLE_FACET, .entry = &facet}, &index);
    }
    if (status == SXML_STATUS_OK) {
        join_list(reader, SXML_LIST_FIELD_FACETS, index);
    }
    return status;
}

/* xs:assertion (a facet) and xs:assert (Part 1, 3.13.2). */
static sxml_status read_assertion(schema_reader *reader) {
    sxml_schema_assertion assertion = {.position = reader->element.position};
    sxml_status status = add_xpath_record(reader, SXML_READ_ATTRIBUTE_TEST, &assertion.test);
    uint32_t index = 0;
    if (status == SXML_STATUS_OK) {
        status = add_component(
            reader, (component_entry){.table = SXML_SCHEMA_TABLE_ASSERTION, .entry = &assertion},
            &index);
    }
    if (status == SXML_STATUS_OK) {
        /* An xs:assertion facet of a simpleContent restriction belongs to
           the simple content type, xs:assert to the complex type
           (3.4.2.3.3; Part 2, 4.3.13.2). */
        join_list(reader,
                  reader->element.role == SXML_READ_ROLE_ASSERTION &&
                          reader->element.parent_role == SXML_READ_ROLE_SIMPLE_CONTENT_RESTRICTION
                      ? SXML_LIST_FIELD_FACET_ASSERTIONS
                      : SXML_LIST_FIELD_ASSERTIONS,
                  index);
    }
    return status;
}

/* ------------------------------------------------------- Complex types */

/* Applies the schema document's xs:defaultOpenContent where it may apply
   (Part 1, 3.4.2.3.3, clause 6): to complex content, unless
   appliesToEmpty is false and the type has no particle of its own or
   through complexContent (COMPLEX_TYPES drops it too when the explicit
   content type turns out empty); xs:openContent replaces it. */
static void apply_default_open_content(const schema_reader *reader,
                                       sxml_schema_type_definition *definition) {
    const sxml_schema_document_defaults *defaults = reader->defaults;
    if (defaults->open_content_mode == SXML_SCHEMA_OPEN_CONTENT_NONE ||
        has_child_role(reader, ROLE_BIT(SXML_READ_ROLE_SIMPLE_CONTENT))) {
        return;
    }
    const bool may_have_content =
        has_child_role(reader, CONTENT_PARTICLE_ROLES | ROLE_BIT(SXML_READ_ROLE_COMPLEX_CONTENT));
    if (!defaults->open_content_applies_to_empty && !may_have_content) {
        return;
    }
    definition->open_content_mode = defaults->open_content_mode;
    definition->open_content_wildcard = defaults->open_content_wildcard;
    definition->open_content_skips_empty = !defaults->open_content_applies_to_empty;
}

static sxml_status read_complex_type(schema_reader *reader) {
    const bool is_global = reader->element.role == SXML_READ_ROLE_GLOBAL_COMPLEX_TYPE;
    sxml_schema_type_definition definition = {
        .variety = SXML_TYPE_VARIETY_COMPLEX,
        .position = reader->element.position,
        .derivation_method = SXML_SCHEMA_DERIVATION_RESTRICTION,
        .is_abstract =
            boolean_value(reader, (boolean_request){.attribute = SXML_READ_ATTRIBUTE_ABSTRACT,
                                                    .default_value = false}),
        .is_mixed = boolean_value(reader, (boolean_request){.attribute = SXML_READ_ATTRIBUTE_MIXED,
                                                            .default_value = false}),
        .has_simple_content = has_child_role(reader, ROLE_BIT(SXML_READ_ROLE_SIMPLE_CONTENT))};
    /* Part 1, 3.4.2: defaultAttributesApply (default true) with the
       document's defaultAttributes. */
    definition.applies_default_attributes =
        reader->defaults->default_attributes.value != 0 &&
        boolean_value(reader,
                      (boolean_request){.attribute = SXML_READ_ATTRIBUTE_DEFAULT_ATTRIBUTES_APPLY,
                                        .default_value = true});
    if (definition.applies_default_attributes) {
        definition.default_attribute_group_reference = reader->defaults->default_attributes;
    }
    apply_default_open_content(reader, &definition);
    reader->has_complex_type_mixed = has_attribute(reader, SXML_READ_ATTRIBUTE_MIXED);
    reader->complex_type_mixed = definition.is_mixed;
    sxml_status status = SXML_STATUS_OK;
    if (is_global) {
        status = global_name(reader, &definition.type_name);
        if (status == SXML_STATUS_OK) {
            status = read_derivation_set(
                reader,
                (derivation_set_request){.attribute = SXML_READ_ATTRIBUTE_FINAL,
                                         .allowed = DERIVATIONS_EXTENSION_RESTRICTION,
                                         .default_set = reader->defaults->final_default},
                &definition.final_derivations);
        }
        if (status == SXML_STATUS_OK) {
            status = read_derivation_set(
                reader,
                (derivation_set_request){.attribute = SXML_READ_ATTRIBUTE_BLOCK,
                                         .allowed = DERIVATIONS_EXTENSION_RESTRICTION,
                                         .default_set = reader->defaults->block_default},
                &definition.prohibited_substitutions);
        }
    } else {
        definition.final_derivations = 0;
        definition.prohibited_substitutions =
            reader->defaults->block_default & DERIVATIONS_EXTENSION_RESTRICTION;
    }
    uint32_t index = 0;
    if (status == SXML_STATUS_OK) {
        status = add_component(
            reader,
            (component_entry){.table = SXML_SCHEMA_TABLE_TYPE_DEFINITION, .entry = &definition},
            &index);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const component_key key = {.table = SXML_SCHEMA_TABLE_TYPE_DEFINITION, .index = index};
    reader->element.record->owner = key;
    reader->element.record->scope = key;
    if (is_global) {
        return enter_global(reader,
                            (global_entry){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION,
                                           .name = definition.type_name,
                                           .component = index});
    }
    return attach_local_type(reader, index);
}

/* xs:simpleContent and xs:complexContent: their mixed (Part 1,
   3.4.2.3.1: complexContent's wins). */
static sxml_status read_content(schema_reader *reader) {
    const component_key owner = parent_owner(reader);
    reader->element.record->owner = owner;
    sxml_schema_type_definition *definition = edit_type(reader, owner.index);
    if (definition != NULL && reader->element.role == SXML_READ_ROLE_COMPLEX_CONTENT &&
        has_attribute(reader, SXML_READ_ATTRIBUTE_MIXED)) {
        const bool is_mixed =
            boolean_value(reader, (boolean_request){.attribute = SXML_READ_ATTRIBUTE_MIXED,
                                                    .default_value = false});
        definition->is_mixed = is_mixed;
        if (reader->has_complex_type_mixed && reader->complex_type_mixed != is_mixed) {
            /* Part 1, 3.4.3 (src-ct): mixed on both xs:complexType and
               xs:complexContent must agree (XSD 1.1 bug 16786). */
            return report(reader,
                          (read_error){.constraint_name = SXML_TEXT("src-ct"),
                                       .description = "The mixed attributes of xs:complexType "
                                                      "and xs:complexContent disagree",
                                       .subject = {.bytes = NULL, .byte_count = 0}});
        }
    }
    return SXML_STATUS_OK;
}

/* The restriction and extension of simple and complex content. */
static sxml_status read_content_derivation(schema_reader *reader) {
    const component_key owner = parent_owner(reader);
    reader->element.record->owner = owner;
    sxml_schema_type_definition *definition = edit_type(reader, owner.index);
    if (definition != NULL) {
        const bool is_extension = reader->element.role == SXML_READ_ROLE_SIMPLE_CONTENT_EXTENSION ||
                                  reader->element.role == SXML_READ_ROLE_COMPLEX_CONTENT_EXTENSION;
        definition->derivation_method =
            is_extension ? SXML_SCHEMA_DERIVATION_EXTENSION : SXML_SCHEMA_DERIVATION_RESTRICTION;
    }
    if (reader->element.role == SXML_READ_ROLE_COMPLEX_CONTENT_RESTRICTION &&
        has_child_role(reader, ROLE_BIT(SXML_READ_ROLE_OPEN_CONTENT)) &&
        !has_child_role(reader, CONTENT_PARTICLE_ROLES)) {
        /* The schema for schema documents (complexRestrictionType, XSD
           1.1 bug 16786): xs:openContent in a restriction is followed by
           its particle. */
        const sxml_status status = report(
            reader, (read_error){.constraint_name = SXML_TEXT("src-ct"),
                                 .description = "Open content in a restriction of complex content "
                                                "needs a particle after it",
                                 .subject = {.bytes = NULL, .byte_count = 0}});
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return read_base(reader, (sxml_schema_type_definition_index){.value = owner.index});
}

static sxml_status read_open_content(schema_reader *reader) {
    const component_key owner = parent_owner(reader);
    reader->element.record->owner = owner;
    sxml_schema_open_content_mode mode = SXML_SCHEMA_OPEN_CONTENT_INTERLEAVE;
    static const sxml_byte_view mode_keywords[] = {SXML_BYTE_VIEW_INITIALIZER("none"),
                                                   SXML_BYTE_VIEW_INITIALIZER("interleave"),
                                                   SXML_BYTE_VIEW_INITIALIZER("suffix")};
    sxml_status status = SXML_STATUS_OK;
    if (has_attribute(reader, SXML_READ_ATTRIBUTE_MODE)) {
        size_t position = 0;
        if (find_keyword(KEYWORDS(mode_keywords), attribute_value(reader, SXML_READ_ATTRIBUTE_MODE),
                         &position)) {
            mode = (sxml_schema_open_content_mode)position;
        } else {
            status = report_attribute_value(reader, SXML_READ_ATTRIBUTE_MODE);
        }
    }
    if (status == SXML_STATUS_OK && mode == SXML_SCHEMA_OPEN_CONTENT_NONE &&
        has_child_role(reader, ROLE_BIT(SXML_READ_ROLE_OPEN_CONTENT_ANY))) {
        /* Part 1, 3.4.3, src-ct.6 (XSD 1.1 bug 7069): mode="none" has no
           wildcard. */
        status = report(reader, (read_error){.constraint_name = SXML_TEXT("src-ct.6"),
                                             .description = "Open content of mode none has no "
                                                            "xs:any child",
                                             .subject = {.bytes = NULL, .byte_count = 0}});
    }
    if (status == SXML_STATUS_OK && mode != SXML_SCHEMA_OPEN_CONTENT_NONE &&
        !has_child_role(reader, ROLE_BIT(SXML_READ_ROLE_OPEN_CONTENT_ANY))) {
        status = report(reader, (read_error){.constraint_name = SXML_TEXT("src-ct.6"),
                                             .description = "Open content other than mode none "
                                                            "needs an xs:any child",
                                             .subject = {.bytes = NULL, .byte_count = 0}});
    }
    sxml_schema_type_definition *definition = edit_type(reader, owner.index);
    if (definition != NULL) {
        definition->open_content_mode = mode;
        definition->open_content_wildcard.value = 0;
        definition->open_content_skips_empty = false;
    }
    return status;
}

static sxml_status read_default_open_content(schema_reader *reader) {
    reader->element.record->owner = no_component;
    sxml_schema_document_defaults *defaults = reader->defaults;
    defaults->open_content_mode = SXML_SCHEMA_OPEN_CONTENT_INTERLEAVE;
    defaults->open_content_applies_to_empty =
        boolean_value(reader, (boolean_request){.attribute = SXML_READ_ATTRIBUTE_APPLIES_TO_EMPTY,
                                                .default_value = false});
    if (has_attribute(reader, SXML_READ_ATTRIBUTE_MODE)) {
        const sxml_byte_view mode = sxml_xml_text_trim_whitespace(attribute_value(reader, SXML_READ_ATTRIBUTE_MODE));
        if (sxml_byte_view_equals(mode, SXML_TEXT("suffix"))) {
            defaults->open_content_mode = SXML_SCHEMA_OPEN_CONTENT_SUFFIX;
        } else if (!sxml_byte_view_equals(mode, SXML_TEXT("interleave"))) {
            return report_attribute_value(reader, SXML_READ_ATTRIBUTE_MODE);
        }
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- Wildcards */

/* The namespaces of a namespace or notNamespace attribute (Part 1,
   3.10.2.2): ##targetNamespace, ##local and URIs; ##any and ##other only
   alone in namespace. */
typedef struct namespace_list_request {
    read_attribute attribute;
    sxml_schema_wildcard *wildcard;
} namespace_list_request;

static sxml_status read_namespace_list(schema_reader *reader, namespace_list_request request) {
    const sxml_byte_view text = attribute_value(reader, request.attribute);
    const sxml_namespace_uri_identifier target = reader->request->target_namespace;
    const size_t first_index = reader->list_indexes.element_count;
    sxml_status status = SXML_STATUS_OK;
    bool is_valid = true;
    const bool is_namespace = request.attribute == SXML_READ_ATTRIBUTE_NAMESPACE;
    if (is_namespace && sxml_byte_view_equals(sxml_xml_text_trim_whitespace(text), SXML_TEXT("##any"))) {
        request.wildcard->namespace_constraint_variety = SXML_SCHEMA_NAMESPACE_CONSTRAINT_ANY;
    } else if (is_namespace && sxml_byte_view_equals(sxml_xml_text_trim_whitespace(text), SXML_TEXT("##other"))) {
        request.wildcard->namespace_constraint_variety = SXML_SCHEMA_NAMESPACE_CONSTRAINT_NOT;
        status = append_list_index(reader, target.index);
        if (status == SXML_STATUS_OK && target.index != SXML_NAMESPACE_NONE) {
            status = append_list_index(reader, SXML_NAMESPACE_NONE);
        }
    } else {
        request.wildcard->namespace_constraint_variety =
            is_namespace ? SXML_SCHEMA_NAMESPACE_CONSTRAINT_ENUMERATION
                         : SXML_SCHEMA_NAMESPACE_CONSTRAINT_NOT;
        sxml_byte_view rest = text;
        sxml_byte_view token = {.bytes = NULL, .byte_count = 0};
        while (status == SXML_STATUS_OK && sxml_xml_text_next_token(&rest, &token)) {
            sxml_namespace_uri_identifier uri = {.index = SXML_NAMESPACE_NONE};
            if (sxml_byte_view_equals(token, SXML_TEXT("##targetNamespace"))) {
                uri = target;
            } else if (sxml_byte_view_equals(token, SXML_TEXT("##local"))) {
                uri.index = SXML_NAMESPACE_NONE;
            } else if (token.byte_count > 1 && token.bytes[0] == '#' && token.bytes[1] == '#') {
                is_valid = false;
                continue;
            } else {
                status = intern_uri(reader, token, &uri);
            }
            if (status == SXML_STATUS_OK) {
                status = append_list_index(reader, uri.index);
            }
        }
    }
    if (status == SXML_STATUS_OK) {
        const uint32_t *indexes = reader->list_indexes.elements;
        status = sxml_schema_components_add_list(
            reader->components,
            (sxml_schema_index_view){.indexes = indexes == NULL ? NULL : indexes + first_index,
                                     .index_count =
                                         reader->list_indexes.element_count - first_index},
            &request.wildcard->namespaces);
    }
    reader->list_indexes.element_count = first_index;
    if (status == SXML_STATUS_OK && !is_valid) {
        status = report_attribute_value(reader, request.attribute);
    }
    return status;
}

/* notQName (Part 1, 3.10.2.2): QNames, ##defined, and for element
   wildcards ##definedSibling. */
static sxml_status read_disallowed_names(schema_reader *reader, sxml_schema_wildcard *wildcard) {
    const size_t first_index = reader->list_indexes.element_count;
    sxml_byte_view rest = attribute_value(reader, SXML_READ_ATTRIBUTE_NOT_QNAME);
    sxml_byte_view token = {.bytes = NULL, .byte_count = 0};
    sxml_status status = SXML_STATUS_OK;
    const bool is_attribute_wildcard = reader->element.role == SXML_READ_ROLE_ANY_ATTRIBUTE;
    while (status == SXML_STATUS_OK && sxml_xml_text_next_token(&rest, &token)) {
        if (sxml_byte_view_equals(token, SXML_TEXT("##defined"))) {
            wildcard->disallows_defined = true;
        } else if (sxml_byte_view_equals(token, SXML_TEXT("##definedSibling")) && !is_attribute_wildcard) {
            wildcard->disallows_sibling = true;
        } else if (token.byte_count > 0 && token.bytes[0] == '#') {
            status = report_attribute_value(reader, SXML_READ_ATTRIBUTE_NOT_QNAME);
        } else {
            qname_resolution resolution = {
                .is_resolved = false, .name = {.index = 0}, .namespace_uri = {.index = 0}};
            status = resolve_qname(reader, token, &resolution);
            if (status == SXML_STATUS_OK && resolution.is_resolved) {
                status = append_list_index(reader, resolution.name.index);
            }
        }
    }
    if (status == SXML_STATUS_OK) {
        const uint32_t *indexes = reader->list_indexes.elements;
        status = sxml_schema_components_add_list(
            reader->components,
            (sxml_schema_index_view){.indexes = indexes == NULL ? NULL : indexes + first_index,
                                     .index_count =
                                         reader->list_indexes.element_count - first_index},
            &wildcard->disallowed_names);
    }
    reader->list_indexes.element_count = first_index;
    return status;
}

/* Wildcard Properties Correct clause 4 (Part 1, 3.10.6.1): the namespace
   of each disallowed name is one the namespace constraint allows
   (3.10.4.3). */
static sxml_status check_disallowed_names(schema_reader *reader,
                                          const sxml_schema_wildcard *wildcard) {
    const sxml_schema_index_view names =
        sxml_schema_get_list(reader->components, wildcard->disallowed_names);
    const sxml_schema_index_view namespaces =
        sxml_schema_get_list(reader->components, wildcard->namespaces);
    for (size_t name_index = 0; name_index < names.index_count; name_index += 1) {
        const sxml_name_parts parts =
            name_parts_of(reader, (sxml_name_identifier){.index = names.indexes[name_index]});
        bool is_listed = false;
        for (size_t namespace_index = 0; namespace_index < namespaces.index_count;
             namespace_index += 1) {
            is_listed =
                is_listed || namespaces.indexes[namespace_index] == parts.namespace_uri.index;
        }
        const bool is_allowed =
            wildcard->namespace_constraint_variety == SXML_SCHEMA_NAMESPACE_CONSTRAINT_ANY ||
            (wildcard->namespace_constraint_variety == SXML_SCHEMA_NAMESPACE_CONSTRAINT_ENUMERATION
                 ? is_listed
                 : !is_listed);
        if (!is_allowed) {
            return report(reader, (read_error){.constraint_name = SXML_TEXT("w-props-correct.4"),
                                               .description = "A disallowed name is in a "
                                                              "namespace the wildcard does not "
                                                              "allow",
                                               .subject = parts.local_name});
        }
    }
    return SXML_STATUS_OK;
}

static sxml_status add_wildcard(schema_reader *reader, uint32_t *index) {
    sxml_schema_wildcard wildcard = {.position = reader->element.position,
                                     .namespace_constraint_variety =
                                         SXML_SCHEMA_NAMESPACE_CONSTRAINT_ANY,
                                     .process_contents = SXML_SCHEMA_PROCESS_CONTENTS_STRICT};
    sxml_status status = SXML_STATUS_OK;
    const bool has_namespace = has_attribute(reader, SXML_READ_ATTRIBUTE_NAMESPACE);
    const bool has_not_namespace = has_attribute(reader, SXML_READ_ATTRIBUTE_NOT_NAMESPACE);
    if (has_namespace && has_not_namespace) {
        status = report(reader, (read_error){.constraint_name = SXML_TEXT("src-wildcard.1"),
                                             .description = "namespace and notNamespace must "
                                                            "not both be present",
                                             .subject = {.bytes = NULL, .byte_count = 0}});
    }
    if (status == SXML_STATUS_OK && (has_namespace || has_not_namespace)) {
        status = read_namespace_list(
            reader,
            (namespace_list_request){.attribute = has_namespace ? SXML_READ_ATTRIBUTE_NAMESPACE
                                                                : SXML_READ_ATTRIBUTE_NOT_NAMESPACE,
                                     .wildcard = &wildcard});
    }
    if (status == SXML_STATUS_OK && has_attribute(reader, SXML_READ_ATTRIBUTE_NOT_QNAME)) {
        status = read_disallowed_names(reader, &wildcard);
    }
    if (status == SXML_STATUS_OK) {
        status = check_disallowed_names(reader, &wildcard);
    }
    size_t position = 0;
    if (has_attribute(reader, SXML_READ_ATTRIBUTE_PROCESS_CONTENTS) &&
        find_keyword(KEYWORDS(process_contents_keywords),
                     attribute_value(reader, SXML_READ_ATTRIBUTE_PROCESS_CONTENTS), &position)) {
        wildcard.process_contents = (sxml_schema_process_contents)position;
    }
    if (status == SXML_STATUS_OK) {
        status = add_component(
            reader, (component_entry){.table = SXML_SCHEMA_TABLE_WILDCARD, .entry = &wildcard},
            index);
    }
    return status;
}

static sxml_status read_wildcard(schema_reader *reader) {
    reader->element.record->owner = no_component;
    if (reader->element.role == SXML_READ_ROLE_ANY) {
        /* A particle, then its wildcard term. */
        const occurrences counts = occurrences_of(reader);
        sxml_schema_particle particle = {.position = reader->element.position,
                                         .minimum_occurrences = counts.minimum,
                                         .maximum_occurrences = counts.maximum,
                                         .term_kind = SXML_SCHEMA_TERM_WILDCARD};
        uint32_t particle_index = 0;
        sxml_status status = add_component(
            reader, (component_entry){.table = SXML_SCHEMA_TABLE_PARTICLE, .entry = &particle},
            &particle_index);
        uint32_t wildcard_index = 0;
        if (status == SXML_STATUS_OK) {
            status = add_wildcard(reader, &wildcard_index);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_schema_particle *entry = edit_component(
            reader, (component_key){.table = SXML_SCHEMA_TABLE_PARTICLE, .index = particle_index});
        if (entry != NULL) {
            entry->term = wildcard_index;
        }
        attach_particle(reader, (particle_attachment){.particle = particle_index,
                                                      .is_empty = counts.maximum == 0});
        return SXML_STATUS_OK;
    }
    uint32_t wildcard_index = 0;
    sxml_status status = add_wildcard(reader, &wildcard_index);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const component_key owner = parent_owner(reader);
    if (reader->element.role == SXML_READ_ROLE_OPEN_CONTENT_ANY) {
        if (reader->element.parent_role == SXML_READ_ROLE_DEFAULT_OPEN_CONTENT) {
            reader->defaults->open_content_wildcard.value = wildcard_index;
        } else {
            sxml_schema_type_definition *definition = edit_type(reader, owner.index);
            if (definition != NULL) {
                definition->open_content_wildcard.value = wildcard_index;
            }
        }
    } else if (owner.table == SXML_SCHEMA_TABLE_TYPE_DEFINITION) {
        sxml_schema_type_definition *definition = edit_type(reader, owner.index);
        if (definition != NULL) {
            definition->attribute_wildcard.value = wildcard_index;
        }
    } else if (owner.table == SXML_SCHEMA_TABLE_ATTRIBUTE_GROUP) {
        sxml_schema_attribute_group *group = edit_component(reader, owner);
        if (group != NULL) {
            group->attribute_wildcard.value = wildcard_index;
        }
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- Model groups */

static sxml_schema_compositor compositor_of_role(read_role role) {
    if (role == SXML_READ_ROLE_ALL || role == SXML_READ_ROLE_GROUP_ALL) {
        return SXML_SCHEMA_COMPOSITOR_ALL;
    }
    if (role == SXML_READ_ROLE_CHOICE || role == SXML_READ_ROLE_GROUP_CHOICE) {
        return SXML_SCHEMA_COMPOSITOR_CHOICE;
    }
    return SXML_SCHEMA_COMPOSITOR_SEQUENCE;
}

/* xs:all, xs:choice, xs:sequence: in content a particle and its model
   group; in a group definition its model group (Part 1, 3.8.2). */
static sxml_status read_model_group(schema_reader *reader) {
    const read_role role = reader->element.role;
    const bool has_particle = role == SXML_READ_ROLE_ALL || role == SXML_READ_ROLE_CHOICE ||
                              role == SXML_READ_ROLE_SEQUENCE;
    const occurrences counts = occurrences_of(reader);
    sxml_status status = SXML_STATUS_OK;
    if (role == SXML_READ_ROLE_ALL && (counts.minimum > 1 || counts.maximum > 1)) {
        /* The schema for schema documents: an all group has minOccurs
           and maxOccurs 0 or 1 (Part 1, 3.8.2, cos-all-limited). */
        status = report(reader, (read_error){.constraint_name = SXML_TEXT("cos-all-limited.1.2"),
                                             .description = "An all group has minOccurs 0 or 1 "
                                                            "and maxOccurs 0 or 1",
                                             .subject = {.bytes = NULL, .byte_count = 0}});
    }
    uint32_t particle_index = 0;
    if (status == SXML_STATUS_OK && has_particle) {
        const sxml_schema_particle particle = {.position = reader->element.position,
                                               .minimum_occurrences = counts.minimum,
                                               .maximum_occurrences = counts.maximum,
                                               .term_kind = SXML_SCHEMA_TERM_MODEL_GROUP};
        status = add_component(
            reader, (component_entry){.table = SXML_SCHEMA_TABLE_PARTICLE, .entry = &particle},
            &particle_index);
    }
    const sxml_schema_model_group group = {.compositor = compositor_of_role(role),
                                           .position = reader->element.position};
    uint32_t group_index = 0;
    if (status == SXML_STATUS_OK) {
        status = add_component(
            reader, (component_entry){.table = SXML_SCHEMA_TABLE_MODEL_GROUP, .entry = &group},
            &group_index);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const component_key owner = parent_owner(reader);
    reader->element.record->owner =
        (component_key){.table = SXML_SCHEMA_TABLE_MODEL_GROUP, .index = group_index};
    if (!has_particle) {
        sxml_schema_model_group_definition *definition = edit_component(reader, owner);
        if (owner.table == SXML_SCHEMA_TABLE_MODEL_GROUP_DEFINITION && definition != NULL) {
            definition->model_group.value = group_index;
        }
        return SXML_STATUS_OK;
    }
    sxml_schema_particle *particle = edit_component(
        reader, (component_key){.table = SXML_SCHEMA_TABLE_PARTICLE, .index = particle_index});
    if (particle != NULL) {
        particle->term = group_index;
    }
    /* Part 1, 3.4.2.3.3, clause 2.1: an empty sequence or all, an empty
       choice with minOccurs 0, or maxOccurs 0 is no explicit content. */
    const bool has_particles = has_child_role(reader, NESTED_PARTICLE_ROLES | ALL_PARTICLE_ROLES);
    const bool is_empty =
        counts.maximum == 0 ||
        (!has_particles && (role != SXML_READ_ROLE_CHOICE || counts.minimum == 0));
    attach_particle(reader,
                    (particle_attachment){.particle = particle_index, .is_empty = is_empty});
    return SXML_STATUS_OK;
}

static sxml_status read_group_definition(schema_reader *reader) {
    sxml_schema_model_group_definition definition = {.position = reader->element.position};
    sxml_status status = global_name(reader, &definition.group_name);
    uint32_t index = 0;
    if (status == SXML_STATUS_OK) {
        status = add_component(reader,
                               (component_entry){.table = SXML_SCHEMA_TABLE_MODEL_GROUP_DEFINITION,
                                                 .entry = &definition},
                               &index);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const component_key key = {.table = SXML_SCHEMA_TABLE_MODEL_GROUP_DEFINITION, .index = index};
    reader->element.record->owner = key;
    reader->element.record->scope = key;
    return enter_global(
        reader, (global_entry){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_MODEL_GROUP_DEFINITION,
                               .name = definition.group_name,
                               .component = index});
}

static sxml_status read_group_reference(schema_reader *reader) {
    reader->element.record->owner = no_component;
    const occurrences counts = occurrences_of(reader);
    sxml_schema_particle particle = {.position = reader->element.position,
                                     .minimum_occurrences = counts.minimum,
                                     .maximum_occurrences = counts.maximum,
                                     .term_kind = SXML_SCHEMA_TERM_MODEL_GROUP};
    const uint32_t self_reference_count = reader->redefinition.self_reference_count;
    sxml_status status = add_reference(
        reader,
        (reference_request){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_MODEL_GROUP_DEFINITION,
                            .text = attribute_value(reader, SXML_READ_ATTRIBUTE_REF),
                            .may_name_original = true},
        &particle.term_reference);
    if (reader->redefinition.self_reference_count != self_reference_count &&
        (counts.minimum != 1 || counts.maximum != 1)) {
        reader->redefinition.has_counted_self_reference = true;
    }
    uint32_t index = 0;
    if (status == SXML_STATUS_OK) {
        status = add_component(
            reader, (component_entry){.table = SXML_SCHEMA_TABLE_PARTICLE, .entry = &particle},
            &index);
    }
    if (status == SXML_STATUS_OK) {
        attach_particle(reader,
                        (particle_attachment){.particle = index, .is_empty = counts.maximum == 0});
    }
    return status;
}

/* ------------------------------------------------------- Declarations */

/* A local declaration whose targetNamespace is not the document's
   (Part 1, 3.3.3 clause 4.3 and 3.2.3 clause 6.3): it is inside a
   restriction of complex content, between it and its nearest complex
   type, whose base is not xs:anyType. */
static sxml_status check_foreign_namespace(schema_reader *reader) {
    uint32_t node = reader->element.node.index;
    bool is_in_restriction = false;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK) {
        sxml_node_description description;
        status = describe_node(reader, node, &description);
        if (status != SXML_STATUS_OK || !description.has_parent ||
            !is_in_range(reader, description.parent.index)) {
            break;
        }
        node = description.parent.index;
        const node_record *record = record_of(reader, node);
        if (record->role == SXML_READ_ROLE_COMPLEX_CONTENT_RESTRICTION ||
            record->role == SXML_READ_ROLE_SIMPLE_CONTENT_RESTRICTION) {
            const sxml_schema_type_definition *definition = sxml_schema_get_type_definition(
                reader->components,
                (sxml_schema_type_definition_index){.value = record->owner.index});
            const sxml_name_parts base =
                name_parts_of(reader, sxml_schema_get_reference(reader->components,
                                                                definition->base_type_reference)
                                          ->name);
            is_in_restriction = definition->base_type_reference.value != 0 &&
                                !(is_schema_namespace(base.namespace_uri) &&
                                  sxml_byte_view_equals(base.local_name, SXML_TEXT("anyType")));
        }
        if (record->role == SXML_READ_ROLE_GLOBAL_COMPLEX_TYPE ||
            record->role == SXML_READ_ROLE_LOCAL_COMPLEX_TYPE) {
            break;
        }
    }
    if (status != SXML_STATUS_OK || is_in_restriction) {
        return status;
    }
    return report(reader, (read_error){.constraint_name = {.bytes = NULL, .byte_count = 0},
                                       .description = "A local declaration in another "
                                                      "namespace is allowed only in a "
                                                      "restriction of a complex type other "
                                                      "than xs:anyType",
                                       .subject = {.bytes = NULL, .byte_count = 0}});
}

/* The {target namespace} of a local declaration (Part 1, 3.3.2.3 and
   3.2.2.2): its targetNamespace, else the document's when form (else the
   document's form default) is qualified, else none. */
typedef struct local_name_request {
    bool qualifies_by_default;
} local_name_request;

static sxml_status local_declaration_name(schema_reader *reader, local_name_request request,
                                          sxml_name_identifier *name) {
    name->index = 0;
    const sxml_byte_view local_name = sxml_xml_text_trim_whitespace(attribute_value(reader, SXML_READ_ATTRIBUTE_NAME));
    if (!sxml_xml_text_is_ncname(local_name)) {
        return SXML_STATUS_OK;
    }
    sxml_namespace_uri_identifier namespace_uri = {.index = SXML_NAMESPACE_NONE};
    sxml_status status = SXML_STATUS_OK;
    if (has_attribute(reader, SXML_READ_ATTRIBUTE_TARGET_NAMESPACE)) {
        if (has_attribute(reader, SXML_READ_ATTRIBUTE_FORM)) {
            status =
                report(reader, (read_error){.constraint_name = {.bytes = NULL, .byte_count = 0},
                                            .description = "form and targetNamespace must not "
                                                           "both be present",
                                            .subject = {.bytes = NULL, .byte_count = 0}});
        }
        if (status == SXML_STATUS_OK) {
            status = intern_uri(
                reader, sxml_xml_text_trim_whitespace(attribute_value(reader, SXML_READ_ATTRIBUTE_TARGET_NAMESPACE)),
                &namespace_uri);
        }
        if (status == SXML_STATUS_OK &&
            namespace_uri.index != reader->request->target_namespace.index) {
            status = check_foreign_namespace(reader);
        }
    } else {
        bool qualifies = request.qualifies_by_default;
        size_t position = 0;
        if (has_attribute(reader, SXML_READ_ATTRIBUTE_FORM) &&
            find_keyword(KEYWORDS(form_keywords), attribute_value(reader, SXML_READ_ATTRIBUTE_FORM),
                         &position)) {
            qualifies = position == 1;
        }
        if (qualifies) {
            namespace_uri = reader->request->target_namespace;
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return intern_name(
        reader, (sxml_name_request){.namespace_uri = namespace_uri, .local_name = local_name},
        name);
}

/* The parent of a local declaration: the complex type or group (model
   group or attribute group) it is in. */
static void set_local_scope(const schema_reader *reader, sxml_schema_type_definition_index *type,
                            uint32_t *group) {
    const component_key scope = reader->element.parent_record->scope;
    type->value = scope.table == SXML_SCHEMA_TABLE_TYPE_DEFINITION ? scope.index : 0;
    *group = scope.table != SXML_SCHEMA_TABLE_TYPE_DEFINITION ? scope.index : 0;
}

/* An element with ref= may have only minOccurs, maxOccurs, id and an
   annotation (Part 1, 3.3.3, src-element.2.2). */
#define ELEMENT_REFERENCE_ATTRIBUTES                                                               \
    (ATTRIBUTES_ID | ATTRIBUTES_OCCURS | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_REF))

static sxml_status read_element_declaration(schema_reader *reader) {
    const bool is_global = reader->element.role == SXML_READ_ROLE_GLOBAL_ELEMENT;
    const bool has_reference = has_attribute(reader, SXML_READ_ATTRIBUTE_REF);
    const bool has_name = has_attribute(reader, SXML_READ_ATTRIBUTE_NAME);
    sxml_status status = SXML_STATUS_OK;
    reader->element.record->owner = no_component;
    if (!is_global && has_reference == has_name) {
        status = report(reader, (read_error){.constraint_name = SXML_TEXT("src-element.2.1"),
                                             .description = "A local element has either name "
                                                            "or ref",
                                             .subject = {.bytes = NULL, .byte_count = 0}});
    }
    if (status == SXML_STATUS_OK && !is_global && has_reference &&
        ((reader->element.present_attributes & ~ELEMENT_REFERENCE_ATTRIBUTES) != 0 ||
         (reader->element.child_roles & ~ANNOTATION_ROLE) != 0)) {
        status = report(reader, (read_error){.constraint_name = SXML_TEXT("src-element.2.2"),
                                             .description = "An element reference has only "
                                                            "minOccurs, maxOccurs, id and an "
                                                            "annotation",
                                             .subject = {.bytes = NULL, .byte_count = 0}});
    }
    if (status == SXML_STATUS_OK && has_attribute(reader, SXML_READ_ATTRIBUTE_TYPE) &&
        has_child_role(reader, LOCAL_TYPE_ROLES)) {
        status = report(reader, (read_error){.constraint_name = SXML_TEXT("src-element.3"),
                                             .description = "An element has either a type "
                                                            "attribute or a type child",
                                             .subject = {.bytes = NULL, .byte_count = 0}});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t particle_index = 0;
    occurrences counts = {.minimum = 1, .maximum = 1};
    if (!is_global) {
        counts = occurrences_of(reader);
        sxml_schema_particle particle = {.position = reader->element.position,
                                         .minimum_occurrences = counts.minimum,
                                         .maximum_occurrences = counts.maximum,
                                         .term_kind = SXML_SCHEMA_TERM_ELEMENT_DECLARATION};
        if (has_reference) {
            status = add_reference(
                reader,
                (reference_request){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION,
                                    .text = attribute_value(reader, SXML_READ_ATTRIBUTE_REF),
                                    .may_name_original = false},
                &particle.term_reference);
        }
        if (status == SXML_STATUS_OK) {
            status = add_component(
                reader, (component_entry){.table = SXML_SCHEMA_TABLE_PARTICLE, .entry = &particle},
                &particle_index);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        attach_particle(reader, (particle_attachment){.particle = particle_index,
                                                      .is_empty = counts.maximum == 0});
        if (has_reference) {
            return SXML_STATUS_OK;
        }
    }
    sxml_schema_element_declaration declaration = {
        .position = reader->element.position,
        .scope_variety = is_global ? SXML_SCHEMA_SCOPE_GLOBAL : SXML_SCHEMA_SCOPE_LOCAL,
        .is_nillable =
            boolean_value(reader, (boolean_request){.attribute = SXML_READ_ATTRIBUTE_NILLABLE,
                                                    .default_value = false}),
        .is_abstract =
            boolean_value(reader, (boolean_request){.attribute = SXML_READ_ATTRIBUTE_ABSTRACT,
                                                    .default_value = false})};
    if (is_global) {
        status = global_name(reader, &declaration.element_name);
    } else {
        uint32_t group = 0;
        set_local_scope(reader, &declaration.parent_type, &group);
        declaration.parent_group.value = group;
        status = local_declaration_name(
            reader,
            (local_name_request){.qualifies_by_default = reader->defaults->qualifies_elements},
            &declaration.element_name);
    }
    if (status == SXML_STATUS_OK && has_attribute(reader, SXML_READ_ATTRIBUTE_TYPE)) {
        status = add_reference(
            reader,
            (reference_request){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION,
                                .text = attribute_value(reader, SXML_READ_ATTRIBUTE_TYPE),
                                .may_name_original = false},
            &declaration.type_reference);
    }
    if (status == SXML_STATUS_OK && has_attribute(reader, SXML_READ_ATTRIBUTE_SUBSTITUTION_GROUP)) {
        status = add_reference_list(
            reader,
            (reference_request){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION,
                                .text =
                                    attribute_value(reader, SXML_READ_ATTRIBUTE_SUBSTITUTION_GROUP),
                                .may_name_original = false},
            &declaration.substitution_group_references);
    }
    if (status == SXML_STATUS_OK) {
        status = read_value_constraint(reader, &declaration.value_constraint);
    }
    if (status == SXML_STATUS_OK && is_global) {
        status = read_derivation_set(
            reader,
            (derivation_set_request){.attribute = SXML_READ_ATTRIBUTE_FINAL,
                                     .allowed = DERIVATIONS_EXTENSION_RESTRICTION,
                                     .default_set = reader->defaults->final_default},
            &declaration.substitution_group_exclusions);
    }
    if (status == SXML_STATUS_OK) {
        status = read_derivation_set(
            reader,
            (derivation_set_request){.attribute = SXML_READ_ATTRIBUTE_BLOCK,
                                     .allowed = DERIVATIONS_BLOCK,
                                     .default_set = reader->defaults->block_default},
            &declaration.disallowed_substitutions);
    }
    uint32_t index = 0;
    if (status == SXML_STATUS_OK) {
        status = add_component(reader,
                               (component_entry){.table = SXML_SCHEMA_TABLE_ELEMENT_DECLARATION,
                                                 .entry = &declaration},
                               &index);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    reader->element.record->owner =
        (component_key){.table = SXML_SCHEMA_TABLE_ELEMENT_DECLARATION, .index = index};
    if (is_global) {
        return enter_global(
            reader, (global_entry){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION,
                                   .name = declaration.element_name,
                                   .component = index});
    }
    sxml_schema_particle *particle = edit_component(
        reader, (component_key){.table = SXML_SCHEMA_TABLE_PARTICLE, .index = particle_index});
    if (particle != NULL) {
        particle->term = index;
    }
    return SXML_STATUS_OK;
}

/* The checks of attribute declarations (Part 1, 3.2.3: src-attribute;
   3.2.6.3 no-xmlns and 3.2.6.4 no-xsi). */
static sxml_status check_attribute_name(schema_reader *reader, sxml_name_identifier name) {
    const sxml_name_parts parts = name_parts_of(reader, name);
    if (name.index != 0 && parts.namespace_uri.index == SXML_NAMESPACE_NONE &&
        sxml_byte_view_equals(parts.local_name, SXML_TEXT("xmlns"))) {
        return report(reader, (read_error){.constraint_name = SXML_TEXT("no-xmlns"),
                                           .description = "An attribute declaration may not be "
                                                          "named xmlns",
                                           .subject = {.bytes = NULL, .byte_count = 0}});
    }
    if (name.index != 0 &&
        parts.namespace_uri.index == (uint32_t)SXML_NAMESPACE_XML_SCHEMA_INSTANCE) {
        return report(reader, (read_error){.constraint_name = SXML_TEXT("no-xsi"),
                                           .description = "An attribute declaration may not be "
                                                          "in the xsi namespace",
                                           .subject = parts.local_name});
    }
    return SXML_STATUS_OK;
}

static sxml_status read_global_attribute(schema_reader *reader) {
    sxml_schema_attribute_declaration declaration = {
        .position = reader->element.position,
        .scope_variety = SXML_SCHEMA_SCOPE_GLOBAL,
        .is_inheritable =
            boolean_value(reader, (boolean_request){.attribute = SXML_READ_ATTRIBUTE_INHERITABLE,
                                                    .default_value = false})};
    sxml_status status = global_name(reader, &declaration.attribute_name);
    if (status == SXML_STATUS_OK) {
        status = check_attribute_name(reader, declaration.attribute_name);
    }
    if (status == SXML_STATUS_OK && has_attribute(reader, SXML_READ_ATTRIBUTE_TYPE) &&
        has_child_role(reader, ROLE_BIT(SXML_READ_ROLE_LOCAL_SIMPLE_TYPE))) {
        status = report(reader, (read_error){.constraint_name = SXML_TEXT("src-attribute.4"),
                                             .description = "An attribute has either a type "
                                                            "attribute or a simpleType child",
                                             .subject = {.bytes = NULL, .byte_count = 0}});
    }
    if (status == SXML_STATUS_OK && has_attribute(reader, SXML_READ_ATTRIBUTE_TYPE)) {
        status = add_reference(
            reader,
            (reference_request){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION,
                                .text = attribute_value(reader, SXML_READ_ATTRIBUTE_TYPE),
                                .may_name_original = false},
            &declaration.type_reference);
    }
    if (status == SXML_STATUS_OK) {
        status = read_value_constraint(reader, &declaration.value_constraint);
    }
    uint32_t index = 0;
    if (status == SXML_STATUS_OK) {
        status = add_component(reader,
                               (component_entry){.table = SXML_SCHEMA_TABLE_ATTRIBUTE_DECLARATION,
                                                 .entry = &declaration},
                               &index);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    reader->element.record->owner =
        (component_key){.table = SXML_SCHEMA_TABLE_ATTRIBUTE_DECLARATION, .index = index};
    return enter_global(
        reader, (global_entry){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_DECLARATION,
                               .name = declaration.attribute_name,
                               .component = index});
}

/* An attribute with ref= may have only use, default, fixed, inheritable,
   id and an annotation (Part 1, 3.2.3, src-attribute.3.2). */
#define ATTRIBUTE_REFERENCE_ATTRIBUTES                                                             \
    (ATTRIBUTES_ID | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_REF) |                                      \
     ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_USE) | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_DEFAULT) |         \
     ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_FIXED) | ATTRIBUTE_BIT(SXML_READ_ATTRIBUTE_INHERITABLE))

/* A local xs:attribute: an attribute use, then its local declaration
   unless it refers to a global one (Part 1, 3.2.2.2, 3.5.2). */
static sxml_status read_local_attribute(schema_reader *reader) {
    reader->element.record->owner = no_component;
    const bool has_reference = has_attribute(reader, SXML_READ_ATTRIBUTE_REF);
    const bool has_name = has_attribute(reader, SXML_READ_ATTRIBUTE_NAME);
    sxml_status status = SXML_STATUS_OK;
    if (has_reference == has_name) {
        status = report(reader, (read_error){.constraint_name = SXML_TEXT("src-attribute.3.1"),
                                             .description = "A local attribute has either name "
                                                            "or ref",
                                             .subject = {.bytes = NULL, .byte_count = 0}});
    } else if (has_reference &&
               ((reader->element.present_attributes & ~ATTRIBUTE_REFERENCE_ATTRIBUTES) != 0 ||
                (reader->element.child_roles & ~ANNOTATION_ROLE) != 0)) {
        status = report(reader, (read_error){.constraint_name = SXML_TEXT("src-attribute.3.2"),
                                             .description = "An attribute reference has no "
                                                            "form, type or simpleType",
                                             .subject = {.bytes = NULL, .byte_count = 0}});
    } else if (has_attribute(reader, SXML_READ_ATTRIBUTE_TYPE) &&
               has_child_role(reader, ROLE_BIT(SXML_READ_ROLE_LOCAL_SIMPLE_TYPE))) {
        status = report(reader, (read_error){.constraint_name = SXML_TEXT("src-attribute.4"),
                                             .description = "An attribute has either a type "
                                                            "attribute or a simpleType child",
                                             .subject = {.bytes = NULL, .byte_count = 0}});
    }
    sxml_schema_attribute_use use = {
        .position = reader->element.position,
        .use_kind = SXML_SCHEMA_ATTRIBUTE_USE_OPTIONAL,
        .is_inheritable =
            boolean_value(reader, (boolean_request){.attribute = SXML_READ_ATTRIBUTE_INHERITABLE,
                                                    .default_value = false}),
        .has_inheritable = has_attribute(reader, SXML_READ_ATTRIBUTE_INHERITABLE)};
    size_t position = 0;
    if (has_attribute(reader, SXML_READ_ATTRIBUTE_USE) &&
        find_keyword(KEYWORDS(use_keywords), attribute_value(reader, SXML_READ_ATTRIBUTE_USE),
                     &position)) {
        use.use_kind = (sxml_schema_attribute_use_kind)position;
    }
    if (status == SXML_STATUS_OK && has_attribute(reader, SXML_READ_ATTRIBUTE_DEFAULT) &&
        use.use_kind != SXML_SCHEMA_ATTRIBUTE_USE_OPTIONAL) {
        status = report(reader, (read_error){.constraint_name = SXML_TEXT("src-attribute.2"),
                                             .description = "An attribute with a default is "
                                                            "optional",
                                             .subject = {.bytes = NULL, .byte_count = 0}});
    }
    if (status == SXML_STATUS_OK) {
        status = read_value_constraint(reader, &use.value_constraint);
    }
    if (status == SXML_STATUS_OK && has_reference) {
        status = add_reference(
            reader,
            (reference_request){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_DECLARATION,
                                .text = attribute_value(reader, SXML_READ_ATTRIBUTE_REF),
                                .may_name_original = false},
            &use.attribute_declaration_reference);
    }
    uint32_t use_index = 0;
    if (status == SXML_STATUS_OK) {
        status = add_component(
            reader, (component_entry){.table = SXML_SCHEMA_TABLE_ATTRIBUTE_USE, .entry = &use},
            &use_index);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    join_list(reader, SXML_LIST_FIELD_ATTRIBUTE_USES, use_index);
    if (has_reference || !has_name) {
        return SXML_STATUS_OK;
    }
    sxml_schema_attribute_declaration declaration = {.position = reader->element.position,
                                                     .scope_variety = SXML_SCHEMA_SCOPE_LOCAL,
                                                     .is_inheritable = use.is_inheritable};
    uint32_t group = 0;
    set_local_scope(reader, &declaration.parent_type, &group);
    declaration.parent_attribute_group.value = group;
    status = local_declaration_name(
        reader,
        (local_name_request){.qualifies_by_default = reader->defaults->qualifies_attributes},
        &declaration.attribute_name);
    if (status == SXML_STATUS_OK) {
        status = check_attribute_name(reader, declaration.attribute_name);
    }
    if (status == SXML_STATUS_OK && has_attribute(reader, SXML_READ_ATTRIBUTE_TYPE)) {
        status = add_reference(
            reader,
            (reference_request){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION,
                                .text = attribute_value(reader, SXML_READ_ATTRIBUTE_TYPE),
                                .may_name_original = false},
            &declaration.type_reference);
    }
    uint32_t declaration_index = 0;
    if (status == SXML_STATUS_OK) {
        status = add_component(reader,
                               (component_entry){.table = SXML_SCHEMA_TABLE_ATTRIBUTE_DECLARATION,
                                                 .entry = &declaration},
                               &declaration_index);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    reader->element.record->owner = (component_key){
        .table = SXML_SCHEMA_TABLE_ATTRIBUTE_DECLARATION, .index = declaration_index};
    sxml_schema_attribute_use *entry = edit_component(
        reader, (component_key){.table = SXML_SCHEMA_TABLE_ATTRIBUTE_USE, .index = use_index});
    if (entry != NULL) {
        entry->local_attribute_declaration.value = declaration_index;
    }
    return SXML_STATUS_OK;
}

static sxml_status read_attribute_group(schema_reader *reader) {
    sxml_schema_attribute_group group = {.position = reader->element.position};
    sxml_status status = global_name(reader, &group.group_name);
    uint32_t index = 0;
    if (status == SXML_STATUS_OK) {
        status = add_component(
            reader, (component_entry){.table = SXML_SCHEMA_TABLE_ATTRIBUTE_GROUP, .entry = &group},
            &index);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const component_key key = {.table = SXML_SCHEMA_TABLE_ATTRIBUTE_GROUP, .index = index};
    reader->element.record->owner = key;
    reader->element.record->scope = key;
    return enter_global(reader,
                        (global_entry){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_GROUP,
                                       .name = group.group_name,
                                       .component = index});
}

static sxml_status read_attribute_group_reference(schema_reader *reader) {
    reader->element.record->owner = no_component;
    sxml_schema_reference_index reference = {.value = 0};
    sxml_status status =
        add_reference(reader,
                      (reference_request){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_GROUP,
                                          .text = attribute_value(reader, SXML_READ_ATTRIBUTE_REF),
                                          .may_name_original = true},
                      &reference);
    if (status == SXML_STATUS_OK && reference.value != 0) {
        join_list(reader, SXML_LIST_FIELD_ATTRIBUTE_GROUP_REFERENCES, reference.value);
    }
    return status;
}

/* ------------------------------------------------- Alternatives, keys */

/* xs:alternative (Part 1, 3.12.3, src-ta: a type attribute or a type
   child, not both). */
static sxml_status read_alternative(schema_reader *reader) {
    sxml_schema_type_alternative alternative = {.position = reader->element.position};
    sxml_status status = SXML_STATUS_OK;
    const bool has_type = has_attribute(reader, SXML_READ_ATTRIBUTE_TYPE);
    if (has_type == has_child_role(reader, LOCAL_TYPE_ROLES)) {
        status = report(reader, (read_error){.constraint_name = {.bytes = NULL, .byte_count = 0},
                                             .description = "A type alternative has either a "
                                                            "type attribute or a type child",
                                             .subject = {.bytes = NULL, .byte_count = 0}});
    }
    if (status == SXML_STATUS_OK && has_attribute(reader, SXML_READ_ATTRIBUTE_TEST)) {
        status = add_xpath_record(reader, SXML_READ_ATTRIBUTE_TEST, &alternative.test);
    }
    if (status == SXML_STATUS_OK && has_type) {
        status = add_reference(
            reader,
            (reference_request){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION,
                                .text = attribute_value(reader, SXML_READ_ATTRIBUTE_TYPE),
                                .may_name_original = false},
            &alternative.type_reference);
    }
    uint32_t index = 0;
    if (status == SXML_STATUS_OK) {
        status = add_component(
            reader,
            (component_entry){.table = SXML_SCHEMA_TABLE_TYPE_ALTERNATIVE, .entry = &alternative},
            &index);
    }
    if (status == SXML_STATUS_OK) {
        reader->element.record->owner =
            (component_key){.table = SXML_SCHEMA_TABLE_TYPE_ALTERNATIVE, .index = index};
        join_list(reader, SXML_LIST_FIELD_ALTERNATIVES, index);
    }
    return status;
}

/* xs:unique, xs:key, xs:keyref (Part 1, 3.11.3, src-identity-constraint:
   a name with a selector and fields, or a ref alone). */
static sxml_status read_identity_constraint(schema_reader *reader) {
    const read_role role = reader->element.role;
    sxml_schema_identity_constraint constraint = {
        .position = reader->element.position,
        .category = role == SXML_READ_ROLE_KEY      ? SXML_SCHEMA_IDENTITY_CONSTRAINT_KEY
                    : role == SXML_READ_ROLE_KEYREF ? SXML_SCHEMA_IDENTITY_CONSTRAINT_KEYREF
                                                    : SXML_SCHEMA_IDENTITY_CONSTRAINT_UNIQUE};
    const bool has_reference = has_attribute(reader, SXML_READ_ATTRIBUTE_REF);
    const bool has_name = has_attribute(reader, SXML_READ_ATTRIBUTE_NAME);
    const bool has_selector = has_child_role(reader, ROLE_BIT(SXML_READ_ROLE_SELECTOR));
    const bool has_fields = has_child_role(reader, ROLE_BIT(SXML_READ_ROLE_FIELD));
    const bool is_valid = has_reference ? !has_name && !has_selector && !has_fields &&
                                              !has_attribute(reader, SXML_READ_ATTRIBUTE_REFER)
                                        : has_name && has_selector && has_fields &&
                                              (role != SXML_READ_ROLE_KEYREF ||
                                               has_attribute(reader, SXML_READ_ATTRIBUTE_REFER));
    sxml_status status = SXML_STATUS_OK;
    if (!is_valid) {
        status = report(reader, (read_error){.constraint_name = {.bytes = NULL, .byte_count = 0},
                                             .description = "An identity constraint has a name, "
                                                            "a selector and fields (and a "
                                                            "keyref refer), or a ref alone",
                                             .subject = {.bytes = NULL, .byte_count = 0}});
    }
    if (status == SXML_STATUS_OK && has_name) {
        status = global_name(reader, &constraint.constraint_name);
    }
    if (status == SXML_STATUS_OK && has_reference) {
        status = add_reference(
            reader,
            (reference_request){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_IDENTITY_CONSTRAINT,
                                .text = attribute_value(reader, SXML_READ_ATTRIBUTE_REF),
                                .may_name_original = false},
            &constraint.definition_reference);
    }
    if (status == SXML_STATUS_OK && has_attribute(reader, SXML_READ_ATTRIBUTE_REFER)) {
        status = add_reference(
            reader,
            (reference_request){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_IDENTITY_CONSTRAINT,
                                .text = attribute_value(reader, SXML_READ_ATTRIBUTE_REFER),
                                .may_name_original = false},
            &constraint.referenced_key_reference);
    }
    uint32_t index = 0;
    if (status == SXML_STATUS_OK) {
        status = add_component(
            reader,
            (component_entry){.table = SXML_SCHEMA_TABLE_IDENTITY_CONSTRAINT, .entry = &constraint},
            &index);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    reader->element.record->owner =
        (component_key){.table = SXML_SCHEMA_TABLE_IDENTITY_CONSTRAINT, .index = index};
    join_list(reader, SXML_LIST_FIELD_IDENTITY_CONSTRAINTS, index);
    return enter_global(reader,
                        (global_entry){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_IDENTITY_CONSTRAINT,
                                       .name = constraint.constraint_name,
                                       .component = index});
}

static sxml_status read_selector_or_field(schema_reader *reader) {
    reader->element.record->owner = no_component;
    sxml_schema_xpath_record_index record = {.value = 0};
    sxml_status status = add_xpath_record(reader, SXML_READ_ATTRIBUTE_XPATH, &record);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const component_key owner = parent_owner(reader);
    if (reader->element.role == SXML_READ_ROLE_FIELD) {
        join_list(reader, SXML_LIST_FIELD_FIELDS, record.value);
        return SXML_STATUS_OK;
    }
    sxml_schema_identity_constraint *constraint = edit_component(reader, owner);
    if (owner.table == SXML_SCHEMA_TABLE_IDENTITY_CONSTRAINT && constraint != NULL) {
        constraint->selector = record;
    }
    return SXML_STATUS_OK;
}

static sxml_status read_notation(schema_reader *reader) {
    sxml_schema_notation_declaration notation = {
        .position = reader->element.position,
        .has_public_identifier = has_attribute(reader, SXML_READ_ATTRIBUTE_PUBLIC),
        .has_system_identifier = has_attribute(reader, SXML_READ_ATTRIBUTE_SYSTEM)};
    sxml_status status = global_name(reader, &notation.notation_name);
    if (status == SXML_STATUS_OK && !notation.has_public_identifier &&
        !notation.has_system_identifier) {
        /* Part 1, 3.14.1: {public identifier} is required when {system
           identifier} is absent. */
        status = report(reader, (read_error){.constraint_name = {.bytes = NULL, .byte_count = 0},
                                             .description = "A notation has a public or a system "
                                                            "identifier",
                                             .subject = {.bytes = NULL, .byte_count = 0}});
    }
    if (status == SXML_STATUS_OK && notation.has_public_identifier) {
        status = sxml_schema_components_add_text(
            reader->components, attribute_value(reader, SXML_READ_ATTRIBUTE_PUBLIC),
            &notation.public_identifier);
    }
    if (status == SXML_STATUS_OK && notation.has_system_identifier) {
        status = sxml_schema_components_add_text(
            reader->components, attribute_value(reader, SXML_READ_ATTRIBUTE_SYSTEM),
            &notation.system_identifier);
    }
    uint32_t index = 0;
    if (status == SXML_STATUS_OK) {
        status = add_component(
            reader,
            (component_entry){.table = SXML_SCHEMA_TABLE_NOTATION_DECLARATION, .entry = &notation},
            &index);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    reader->element.record->owner = no_component;
    return enter_global(
        reader, (global_entry){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_NOTATION_DECLARATION,
                               .name = notation.notation_name,
                               .component = index});
}

/* ------------------------------------------------------- One element */

static sxml_status read_schema_element(schema_reader *reader) {
    return reader->request->read_kind == SXML_SCHEMA_READ_KIND_DOCUMENT
               ? read_schema_defaults(reader)
               : SXML_STATUS_OK;
}

static sxml_status read_include(schema_reader *reader) {
    return read_document_reference(reader, SXML_SCHEMA_DOCUMENT_RELATION_INCLUDE);
}

static sxml_status read_import(schema_reader *reader) {
    return read_document_reference(reader, SXML_SCHEMA_DOCUMENT_RELATION_IMPORT);
}

/* xs:redefine and xs:override: a reference in the document's read; the
   start of their own read. */
static sxml_status read_redefinition_element(schema_reader *reader) {
    if (reader->element.node.index == reader->first_node &&
        reader->request->read_kind != SXML_SCHEMA_READ_KIND_DOCUMENT) {
        return SXML_STATUS_OK;
    }
    return read_document_reference(reader, reader->element.role == SXML_READ_ROLE_REDEFINE
                                               ? SXML_SCHEMA_DOCUMENT_RELATION_REDEFINE
                                               : SXML_SCHEMA_DOCUMENT_RELATION_OVERRIDE);
}

static sxml_status read_annotation(schema_reader *reader) {
    reader->element.record->owner = no_component;
    return SXML_STATUS_OK;
}

/* How each role is read into components; called from one place, and no
   reader calls another role's (D71). */
typedef sxml_status (*role_reader)(schema_reader *reader);

static const role_reader role_readers[SXML_READ_ROLE_COUNT] = {
    [SXML_READ_ROLE_SCHEMA] = read_schema_element,
    [SXML_READ_ROLE_INCLUDE] = read_include,
    [SXML_READ_ROLE_IMPORT] = read_import,
    [SXML_READ_ROLE_REDEFINE] = read_redefinition_element,
    [SXML_READ_ROLE_OVERRIDE] = read_redefinition_element,
    [SXML_READ_ROLE_ANNOTATION] = read_annotation,
    [SXML_READ_ROLE_APPINFO] = read_annotation,
    [SXML_READ_ROLE_DOCUMENTATION] = read_annotation,
    [SXML_READ_ROLE_DEFAULT_OPEN_CONTENT] = read_default_open_content,
    [SXML_READ_ROLE_GLOBAL_SIMPLE_TYPE] = read_simple_type,
    [SXML_READ_ROLE_LOCAL_SIMPLE_TYPE] = read_simple_type,
    [SXML_READ_ROLE_SIMPLE_RESTRICTION] = read_simple_derivation,
    [SXML_READ_ROLE_LIST] = read_simple_derivation,
    [SXML_READ_ROLE_UNION] = read_simple_derivation,
    [SXML_READ_ROLE_MIN_EXCLUSIVE] = read_facet,
    [SXML_READ_ROLE_MIN_INCLUSIVE] = read_facet,
    [SXML_READ_ROLE_MAX_EXCLUSIVE] = read_facet,
    [SXML_READ_ROLE_MAX_INCLUSIVE] = read_facet,
    [SXML_READ_ROLE_TOTAL_DIGITS] = read_facet,
    [SXML_READ_ROLE_FRACTION_DIGITS] = read_facet,
    [SXML_READ_ROLE_LENGTH] = read_facet,
    [SXML_READ_ROLE_MIN_LENGTH] = read_facet,
    [SXML_READ_ROLE_MAX_LENGTH] = read_facet,
    [SXML_READ_ROLE_ENUMERATION] = read_facet,
    [SXML_READ_ROLE_WHITE_SPACE] = read_facet,
    [SXML_READ_ROLE_PATTERN] = read_facet,
    [SXML_READ_ROLE_EXPLICIT_TIMEZONE] = read_facet,
    [SXML_READ_ROLE_ASSERTION] = read_assertion,
    [SXML_READ_ROLE_GLOBAL_COMPLEX_TYPE] = read_complex_type,
    [SXML_READ_ROLE_LOCAL_COMPLEX_TYPE] = read_complex_type,
    [SXML_READ_ROLE_SIMPLE_CONTENT] = read_content,
    [SXML_READ_ROLE_COMPLEX_CONTENT] = read_content,
    [SXML_READ_ROLE_SIMPLE_CONTENT_RESTRICTION] = read_content_derivation,
    [SXML_READ_ROLE_SIMPLE_CONTENT_EXTENSION] = read_content_derivation,
    [SXML_READ_ROLE_COMPLEX_CONTENT_RESTRICTION] = read_content_derivation,
    [SXML_READ_ROLE_COMPLEX_CONTENT_EXTENSION] = read_content_derivation,
    [SXML_READ_ROLE_OPEN_CONTENT] = read_open_content,
    [SXML_READ_ROLE_GLOBAL_GROUP] = read_group_definition,
    [SXML_READ_ROLE_GROUP_REFERENCE] = read_group_reference,
    [SXML_READ_ROLE_GROUP_ALL] = read_model_group,
    [SXML_READ_ROLE_GROUP_CHOICE] = read_model_group,
    [SXML_READ_ROLE_GROUP_SEQUENCE] = read_model_group,
    [SXML_READ_ROLE_ALL] = read_model_group,
    [SXML_READ_ROLE_CHOICE] = read_model_group,
    [SXML_READ_ROLE_SEQUENCE] = read_model_group,
    [SXML_READ_ROLE_GLOBAL_ELEMENT] = read_element_declaration,
    [SXML_READ_ROLE_LOCAL_ELEMENT] = read_element_declaration,
    [SXML_READ_ROLE_GLOBAL_ATTRIBUTE] = read_global_attribute,
    [SXML_READ_ROLE_LOCAL_ATTRIBUTE] = read_local_attribute,
    [SXML_READ_ROLE_GLOBAL_ATTRIBUTE_GROUP] = read_attribute_group,
    [SXML_READ_ROLE_ATTRIBUTE_GROUP_REFERENCE] = read_attribute_group_reference,
    [SXML_READ_ROLE_ANY] = read_wildcard,
    [SXML_READ_ROLE_ANY_ATTRIBUTE] = read_wildcard,
    [SXML_READ_ROLE_OPEN_CONTENT_ANY] = read_wildcard,
    [SXML_READ_ROLE_ALTERNATIVE] = read_alternative,
    [SXML_READ_ROLE_ASSERT] = read_assertion,
    [SXML_READ_ROLE_UNIQUE] = read_identity_constraint,
    [SXML_READ_ROLE_KEY] = read_identity_constraint,
    [SXML_READ_ROLE_KEYREF] = read_identity_constraint,
    [SXML_READ_ROLE_SELECTOR] = read_selector_or_field,
    [SXML_READ_ROLE_FIELD] = read_selector_or_field,
    [SXML_READ_ROLE_NOTATION] = read_notation,
};

/* The record of the start element's parent, which is not read. */
static const node_record outside_record = {
    .role = SXML_READ_ROLE_NONE,
    .owner = {.table = SXML_SCHEMA_TABLE_TYPE_DEFINITION, .index = 0},
    .scope = {.table = SXML_SCHEMA_TABLE_TYPE_DEFINITION, .index = 0}};

static sxml_status read_element(schema_reader *reader, uint32_t node) {
    element_state *element = &reader->element;
    element->node.index = node;
    element->skips_subtree = false;
    element->record = record_of(reader, node);
    element->role = element->record->role;
    element->rule = &role_rules[element->role];
    element->position = position_of_node(reader, node);
    sxml_status status = describe_node(reader, node, &element->description);
    if (status == SXML_STATUS_OK) {
        status =
            sxml_node_get_in_scope_namespaces(node_reference(reader, node), &element->bindings);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (node == reader->first_node || !element->description.has_parent ||
        !is_in_range(reader, element->description.parent.index)) {
        element->parent_record = &outside_record;
        element->parent_role = SXML_READ_ROLE_NONE;
    } else {
        element->parent_record = record_of(reader, element->description.parent.index);
        element->parent_role = element->parent_record->role;
    }
    /* Local declarations are in the scope of the nearest complex type or
       group; the readers of those set their own. */
    element->record->scope = element->parent_record->scope;
    /* The start element of a redefine's or override's read was checked by
       the document's read. */
    const bool is_checked =
        node == reader->first_node && reader->request->read_kind != SXML_SCHEMA_READ_KIND_DOCUMENT;
    if (!is_checked) {
        status = collect_attributes(reader);
    }
    if (status == SXML_STATUS_OK && is_redefined_child(reader)) {
        status = begin_redefinition(reader);
        if (element->skips_subtree) {
            return status;
        }
    }
    const bool reads_content =
        element->role != SXML_READ_ROLE_REDEFINE && element->role != SXML_READ_ROLE_OVERRIDE
            ? true
            : is_checked;
    if (status == SXML_STATUS_OK && reads_content) {
        status = assign_child_roles(reader);
    }
    if (status == SXML_STATUS_OK) {
        status = role_readers[element->role] == NULL ? SXML_STATUS_OK
                                                     : role_readers[element->role](reader);
    }
    if (element->rule->skips_content) {
        element->skips_subtree = true;
    }
    return status;
}

/* ------------------------------------------------------------ Lists */

/* Writes one gathered list to its field of owner. */
static sxml_status store_list(schema_reader *reader, component_key owner, list_field field) {
    sxml_schema_list list = {.first_entry_index = 0, .entry_count = 0};
    sxml_status status = sxml_schema_components_add_list(
        reader->components,
        (sxml_schema_index_view){.indexes = reader->list_indexes.elements,
                                 .index_count = reader->list_indexes.element_count},
        &list);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *entry = edit_component(reader, owner);
    if (entry == NULL) {
        return SXML_STATUS_OK;
    }
    if (owner.table == SXML_SCHEMA_TABLE_TYPE_DEFINITION) {
        sxml_schema_type_definition *definition = entry;
        if (field == SXML_LIST_FIELD_ATTRIBUTE_USES) {
            definition->attribute_uses = list;
        } else if (field == SXML_LIST_FIELD_ATTRIBUTE_GROUP_REFERENCES) {
            definition->attribute_group_references = list;
        } else if (field == SXML_LIST_FIELD_FACETS) {
            definition->facets = list;
        } else if (field == SXML_LIST_FIELD_ASSERTIONS) {
            definition->assertions = list;
        } else if (field == SXML_LIST_FIELD_FACET_ASSERTIONS) {
            definition->facet_assertions = list;
        } else if (field == SXML_LIST_FIELD_MEMBER_TYPES) {
            definition->anonymous_member_types = list;
        }
    } else if (owner.table == SXML_SCHEMA_TABLE_MODEL_GROUP) {
        sxml_schema_model_group *group = entry;
        group->particles = list;
    } else if (owner.table == SXML_SCHEMA_TABLE_ATTRIBUTE_GROUP) {
        sxml_schema_attribute_group *group = entry;
        if (field == SXML_LIST_FIELD_ATTRIBUTE_USES) {
            group->attribute_uses = list;
        } else if (field == SXML_LIST_FIELD_ATTRIBUTE_GROUP_REFERENCES) {
            group->attribute_group_references = list;
        }
    } else if (owner.table == SXML_SCHEMA_TABLE_ELEMENT_DECLARATION) {
        sxml_schema_element_declaration *declaration = entry;
        if (field == SXML_LIST_FIELD_ALTERNATIVES) {
            declaration->type_alternatives = list;
        } else if (field == SXML_LIST_FIELD_IDENTITY_CONSTRAINTS) {
            declaration->identity_constraints = list;
        }
    } else if (owner.table == SXML_SCHEMA_TABLE_IDENTITY_CONSTRAINT) {
        sxml_schema_identity_constraint *constraint = entry;
        constraint->fields = list;
    }
    return SXML_STATUS_OK;
}

/* Gathers, for each list field, the components the element's children
   gave, in document order, and writes them to the element's owner. */
static sxml_status write_lists_of(schema_reader *reader, uint32_t node) {
    const node_record *record = record_of(reader, node);
    if (record->owner.index == 0) {
        return SXML_STATUS_OK;
    }
    sxml_node_description description;
    sxml_status status = describe_node(reader, node, &description);
    if (status != SXML_STATUS_OK || !description.has_children) {
        return status;
    }
    uint32_t fields = 0;
    bool has_child = true;
    uint32_t child = description.first_child.index;
    while (status == SXML_STATUS_OK && has_child) {
        sxml_node_description child_description;
        status = describe_node(reader, child, &child_description);
        if (status == SXML_STATUS_OK && child_description.node_kind == SXML_NODE_KIND_ELEMENT &&
            is_in_range(reader, child)) {
            fields |= 1U << (unsigned)record_of(reader, child)->joined_list;
        }
        has_child = child_description.has_next_sibling;
        child = child_description.next_sibling.index;
    }
    for (uint32_t field = 1; status == SXML_STATUS_OK && field < (uint32_t)SXML_LIST_FIELD_COUNT;
         field += 1) {
        if ((fields & (1U << field)) == 0) {
            continue;
        }
        reader->list_indexes.element_count = 0;
        has_child = true;
        child = description.first_child.index;
        while (status == SXML_STATUS_OK && has_child) {
            sxml_node_description child_description;
            status = describe_node(reader, child, &child_description);
            if (status == SXML_STATUS_OK && child_description.node_kind == SXML_NODE_KIND_ELEMENT &&
                is_in_range(reader, child) &&
                (uint32_t)record_of(reader, child)->joined_list == field) {
                status = append_list_index(reader, record_of(reader, child)->primary);
            }
            has_child = child_description.has_next_sibling;
            child = child_description.next_sibling.index;
        }
        if (status == SXML_STATUS_OK) {
            status = store_list(reader, record->owner, (list_field)field);
        }
    }
    reader->list_indexes.element_count = 0;
    return status;
}

/* ------------------------------------------------------------ The walk */

/* The namespaces of the document's xs:import elements, which its QName
   values may name (src-resolve.4.2). */
static sxml_status collect_imports(schema_reader *reader) {
    sxml_node_description schema;
    sxml_status status = describe_node(reader, reader->request->schema_element.index, &schema);
    bool has_child = status == SXML_STATUS_OK && schema.has_children;
    uint32_t child = schema.first_child.index;
    while (status == SXML_STATUS_OK && has_child) {
        sxml_node_description description;
        status = describe_node(reader, child, &description);
        if (status != SXML_STATUS_OK) {
            break;
        }
        const sxml_name_parts parts = name_parts_of(reader, description.node_name);
        if (description.node_kind == SXML_NODE_KIND_ELEMENT &&
            is_schema_namespace(parts.namespace_uri) &&
            sxml_byte_view_equals(parts.local_name, SXML_TEXT("import"))) {
            sxml_namespace_uri_identifier namespace_uri = {.index = SXML_NAMESPACE_NONE};
            for (uint32_t attribute_index = 1;
                 status == SXML_STATUS_OK && attribute_index <= description.attribute_count;
                 attribute_index += 1) {
                sxml_node_description attribute;
                status = describe_node(reader, child + attribute_index, &attribute);
                const sxml_name_parts attribute_parts = name_parts_of(reader, attribute.node_name);
                if (status == SXML_STATUS_OK &&
                    attribute_parts.namespace_uri.index == SXML_NAMESPACE_NONE &&
                    sxml_byte_view_equals(attribute_parts.local_name, SXML_TEXT("namespace"))) {
                    sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
                    status = sxml_node_get_content(node_reference(reader, child + attribute_index),
                                                   &value);
                    if (status == SXML_STATUS_OK) {
                        status = intern_uri(reader, value, &namespace_uri);
                    }
                }
            }
            if (status == SXML_STATUS_OK) {
                status = grow_scratch(
                    reader, &reader->imported_namespaces,
                    (sxml_evaluator_growth){
                        .required_count = reader->imported_namespaces.element_count + 1U,
                        .element_byte_count = sizeof(sxml_namespace_uri_identifier),
                        .alignment = alignof(sxml_namespace_uri_identifier)});
            }
            if (status == SXML_STATUS_OK) {
                sxml_namespace_uri_identifier *namespaces = reader->imported_namespaces.elements;
                namespaces[reader->imported_namespaces.element_count] = namespace_uri;
                reader->imported_namespaces.element_count += 1;
            }
        }
        has_child = description.has_next_sibling;
        child = description.next_sibling.index;
    }
    return status;
}

/* ------------------------------------------------------------ IDs */

/* The id attributes of a schema document are of type xs:ID, so no two are
   equal (the schema for schema documents; cvc-id.2). A set of the values
   met, open addressing over a power of two slots. */
typedef struct identifier_set {
    sxml_byte_view *slots;
    size_t slot_count;
    size_t value_count;
} identifier_set;

static uint32_t hash_bytes(sxml_byte_view bytes) {
    uint32_t hash = 2166136261U;
    for (size_t byte_index = 0; byte_index < bytes.byte_count; byte_index += 1) {
        hash = (hash ^ bytes.bytes[byte_index]) * 16777619U;
    }
    return hash;
}

/* The slot of value, or the free slot where it would go. */
static size_t find_identifier_slot(const identifier_set *set, sxml_byte_view value) {
    size_t slot = hash_bytes(value) & (set->slot_count - 1U);
    while (set->slots[slot].bytes != NULL && !sxml_byte_view_equals(set->slots[slot], value)) {
        slot = (slot + 1U) & (set->slot_count - 1U);
    }
    return slot;
}

/* Adds value (not empty) and tells whether it was there. */
typedef struct identifier_insertion {
    bool was_present;
} identifier_insertion;

/* A value to add to a set. */
typedef struct identifier_request {
    identifier_set *set;
    sxml_byte_view value;
} identifier_request;

static sxml_status insert_identifier(schema_reader *reader, identifier_request request,
                                     identifier_insertion *insertion) {
    identifier_set *set = request.set;
    const sxml_byte_view value = request.value;
    if ((set->value_count + 1U) * 2U > set->slot_count) {
        const size_t slot_count = set->slot_count == 0 ? 64U : set->slot_count * 2U;
        void *memory = NULL;
        sxml_status status = sxml_allocator_allocate(
            reader->allocator,
            (sxml_allocation_request){.byte_count = slot_count * sizeof(sxml_byte_view),
                                      .alignment = alignof(sxml_byte_view)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        memset(memory, 0, slot_count * sizeof(sxml_byte_view));
        identifier_set grown = {.slots = memory, .slot_count = slot_count, .value_count = 0};
        for (size_t slot_index = 0; slot_index < set->slot_count; slot_index += 1) {
            if (set->slots[slot_index].bytes != NULL) {
                grown.slots[find_identifier_slot(&grown, set->slots[slot_index])] =
                    set->slots[slot_index];
                grown.value_count += 1;
            }
        }
        sxml_allocator_release(reader->allocator, set->slots);
        *set = grown;
    }
    const size_t slot = find_identifier_slot(set, value);
    insertion->was_present = set->slots[slot].bytes != NULL;
    if (!insertion->was_present) {
        set->slots[slot] = value;
        set->value_count += 1;
    }
    return SXML_STATUS_OK;
}

/* The unqualified id attribute of an element, if it has one. */
static sxml_status find_identifier(const schema_reader *reader, uint32_t element,
                                   sxml_byte_view *value) {
    sxml_node_description element_description;
    const sxml_node_description *description = &element_description;
    sxml_status describe_status = describe_node(reader, element, &element_description);
    if (describe_status != SXML_STATUS_OK) {
        return describe_status;
    }
    *value = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t attribute_index = 1;
         status == SXML_STATUS_OK && attribute_index <= description->attribute_count;
         attribute_index += 1) {
        sxml_node_description attribute;
        status = describe_node(reader, element + attribute_index, &attribute);
        const sxml_name_parts parts = name_parts_of(reader, attribute.node_name);
        if (status == SXML_STATUS_OK && parts.namespace_uri.index == SXML_NAMESPACE_NONE &&
            sxml_byte_view_equals(parts.local_name, SXML_TEXT("id"))) {
            status =
                sxml_node_get_content(node_reference(reader, element + attribute_index), value);
            *value = sxml_xml_text_trim_whitespace(*value);
        }
    }
    return status;
}

/* Reports every id of the document met a second time; the content of
   appinfo and documentation is not the schema document's. */
static sxml_status check_identifiers(schema_reader *reader) {
    identifier_set set = {.slots = NULL, .slot_count = 0, .value_count = 0};
    sxml_status status = SXML_STATUS_OK;
    uint32_t node = reader->first_node;
    while (status == SXML_STATUS_OK && node <= reader->last_node) {
        sxml_node_description description;
        status = describe_node(reader, node, &description);
        if (status != SXML_STATUS_OK || description.node_kind != SXML_NODE_KIND_ELEMENT) {
            node += 1;
            continue;
        }
        const sxml_name_parts parts = name_parts_of(reader, description.node_name);
        if (!is_schema_namespace(parts.namespace_uri) ||
            sxml_byte_view_equals(parts.local_name, SXML_TEXT("appinfo")) ||
            sxml_byte_view_equals(parts.local_name, SXML_TEXT("documentation"))) {
            node = description.last_descendant.index + 1U;
            continue;
        }
        sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
        status = find_identifier(reader, node, &value);
        identifier_insertion insertion = {.was_present = false};
        if (status == SXML_STATUS_OK && value.byte_count != 0) {
            status = insert_identifier(reader, (identifier_request){.set = &set, .value = value},
                                       &insertion);
        }
        if (status == SXML_STATUS_OK && insertion.was_present) {
            reader->element.rule = NULL;
            status = report_at(reader,
                               (read_error){.constraint_name = SXML_TEXT("cvc-id.2"),
                                            .description = "Two elements of a schema document "
                                                           "have the same id",
                                            .subject = value},
                               position_of_node(reader, node));
        }
        node += 1;
    }
    sxml_allocator_release(reader->allocator, set.slots);
    return status;
}

/* Prepares the records of the range and the start element's. */
static sxml_status start_reading(schema_reader *reader) {
    sxml_node_description start;
    sxml_status status = describe_node(reader, reader->request->start_element.index, &start);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    reader->first_node = reader->request->start_element.index;
    reader->last_node = start.last_descendant.index;
    const size_t record_count = (size_t)reader->last_node - reader->first_node + 1U;
    status = grow_scratch(reader, &reader->records,
                          (sxml_evaluator_growth){.required_count = record_count,
                                                  .element_byte_count = sizeof(node_record),
                                                  .alignment = alignof(node_record)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(reader->records.elements, 0, record_count * sizeof(node_record));
    reader->records.element_count = record_count;
    node_record *record = record_of(reader, reader->first_node);
    switch (reader->request->read_kind) {
    case SXML_SCHEMA_READ_KIND_DOCUMENT:
        record->role = SXML_READ_ROLE_SCHEMA;
        break;
    case SXML_SCHEMA_READ_KIND_REDEFINE:
        record->role = SXML_READ_ROLE_REDEFINE;
        break;
    case SXML_SCHEMA_READ_KIND_OVERRIDE:
        record->role = SXML_READ_ROLE_OVERRIDE;
        break;
    }
    /* xml:base on the xs:schema element or above it. */
    uint32_t node = reader->first_node;
    bool has_node = true;
    while (status == SXML_STATUS_OK && has_node && !record->has_xml_base) {
        sxml_node_description description;
        status = describe_node(reader, node, &description);
        for (uint32_t attribute_index = 1;
             status == SXML_STATUS_OK && attribute_index <= description.attribute_count;
             attribute_index += 1) {
            sxml_node_description attribute;
            status = describe_node(reader, node + attribute_index, &attribute);
            const sxml_name_parts parts = name_parts_of(reader, attribute.node_name);
            if (parts.namespace_uri.index == (uint32_t)SXML_NAMESPACE_XML &&
                sxml_byte_view_equals(parts.local_name, SXML_TEXT("base"))) {
                record->has_xml_base = true;
            }
        }
        has_node = description.has_parent;
        node = description.parent.index;
    }
    return status;
}

static sxml_status walk_range(schema_reader *reader) {
    sxml_status status = SXML_STATUS_OK;
    uint32_t node = reader->first_node;
    while (status == SXML_STATUS_OK && node <= reader->last_node) {
        if (reader->redefinition.is_active && node > reader->redefinition.last_node) {
            status = end_redefinition(reader);
            if (status != SXML_STATUS_OK) {
                break;
            }
        }
        sxml_node_description description;
        status = describe_node(reader, node, &description);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (description.node_kind != SXML_NODE_KIND_ELEMENT) {
            node += 1;
            continue;
        }
        if (record_of(reader, node)->role == SXML_READ_ROLE_NONE) {
            node = description.last_descendant.index + 1U;
            continue;
        }
        status = read_element(reader, node);
        if (reader->element.skips_subtree) {
            record_of(reader, node)->owner = no_component;
            node = description.last_descendant.index + 1U;
        } else {
            node += 1;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = end_redefinition(reader);
    }
    for (uint32_t list_node = reader->first_node;
         status == SXML_STATUS_OK && list_node <= reader->last_node; list_node += 1) {
        if (record_of(reader, list_node)->role != SXML_READ_ROLE_NONE) {
            status = write_lists_of(reader, list_node);
        }
    }
    return status;
}

sxml_status sxml_schema_read_document(sxml_schema_assembly *assembly,
                                      const sxml_schema_read_request *request) {
    sxml_schema_compilation *compilation = sxml_schema_assembly_get_compilation(assembly);
    sxml_processor *processor = compilation->configuration->processor;
    schema_reader reader;
    memset(&reader, 0, sizeof(reader));
    reader.assembly = assembly;
    reader.compilation = compilation;
    reader.components = compilation->components;
    reader.name_pool = processor->name_pool;
    reader.allocator = processor->allocator;
    reader.request = request;
    reader.defaults = request->defaults;
    reader.tree = request->tree;
    reader.base_uri = sxml_xml_buffer_empty(processor->allocator);
    reader.knows_versioning_namespace = sxml_name_pool_find_namespace_uri(
        reader.name_pool, SXML_TEXT("http://www.w3.org/2007/XMLSchema-versioning"),
        &reader.versioning_namespace);
    sxml_status status = start_reading(&reader);
    if (status == SXML_STATUS_OK) {
        status = collect_imports(&reader);
    }
    /* Part 1, 4.2.2: an xs:schema element that conditional inclusion
       ignores leaves an empty schema document. */
    inclusion_decision inclusion = {.is_excluded = false};
    if (status == SXML_STATUS_OK && request->read_kind == SXML_SCHEMA_READ_KIND_DOCUMENT) {
        status = decide_inclusion(&reader, request->schema_element.index, &inclusion);
    }
    if (inclusion.is_excluded) {
        reader.defaults->is_read = true;
        reader.last_node = 0;
        reader.first_node = 1;
    }
    if (status == SXML_STATUS_OK && request->read_kind == SXML_SCHEMA_READ_KIND_DOCUMENT) {
        status = check_identifiers(&reader);
    }
    if (status == SXML_STATUS_OK) {
        status = walk_range(&reader);
    }
    sxml_allocator_release(reader.allocator, reader.records.elements);
    sxml_allocator_release(reader.allocator, reader.imported_namespaces.elements);
    sxml_allocator_release(reader.allocator, reader.list_indexes.elements);
    sxml_xml_buffer_release(&reader.base_uri);
    return status;
}
