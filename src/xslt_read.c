/*
 * The stylesheet reader of XSLT 4.0 (src/xslt_read_internal.h; XSLT 4.0,
 * sections 3.3 to 3.11, 5.6; docs/DECISIONS.md D102, D139).
 *
 * Golden files: src/xml.c (a tree walked with an explicit stack of frames
 * in allocator memory, D71) and src/processor.c (a document parsed into
 * an arena tree).
 *
 * The walk keeps one frame per open element: the tree element whose
 * children are being read, the AST node they become children of, the
 * child context (declarations, sequence constructor, listed children) and
 * the element's static context. A child element is read in full (static
 * context, use-when, node, attributes) before its own frame is pushed, so
 * nodes are numbered in document order and an xsl:include or xsl:import
 * pushes the root of the module it reads, which is then read before the
 * declarations after it (D102). Text between elements is collected first,
 * comments, processing instructions and xsl:note elements dropped (3.11.1),
 * and becomes one TEXT_NODE unless whitespace stripping removes it.
 */
#include "xslt_read_internal.h"

#include "function_library_core_internal.h"
#include "processor_internal.h"
#include "tree_internal.h"
#include "xdm_internal.h"
#include "xpath_ast_internal.h"
#include "xslt_static_internal.h"
#include "xslt_value_template_internal.h"

#include "sxml/xml.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------ Tables */

/* How an attribute's value is written (XSLT 4.0, 2.3 "Notation") and so
   which form it is read into (sxml_xslt_ast_value_kind). */
typedef enum value_syntax {
    SXML_VALUE_SYNTAX_EXPRESSION,
    SXML_VALUE_SYNTAX_PATTERN,
    SXML_VALUE_SYNTAX_VALUE_TEMPLATE,
    SXML_VALUE_SYNTAX_SEQUENCE_TYPE,
    SXML_VALUE_SYNTAX_ITEM_TYPE,
    /* A string, kept as written. */
    SXML_VALUE_SYNTAX_STRING,
    /* A URI, ID or NMTOKEN: whitespace-trimmed. */
    SXML_VALUE_SYNTAX_TOKEN,
    /* One character. */
    SXML_VALUE_SYNTAX_CHARACTER,
    /* One character, or a marker character, ":" and a rendition (F&O 4.0,
       "Defining a decimal format"). */
    SXML_VALUE_SYNTAX_CHARACTER_OR_RENDITION,
    SXML_VALUE_SYNTAX_BOOLEAN,
    SXML_VALUE_SYNTAX_DECIMAL,
    /* One word of the rule's keyword mask. */
    SXML_VALUE_SYNTAX_KEYWORD,
    /* A keyword, or an EQName in a namespace (method). */
    SXML_VALUE_SYNTAX_KEYWORD_OR_NAME,
    /* A keyword, or any other NMTOKEN (normalization-form). */
    SXML_VALUE_SYNTAX_KEYWORD_OR_TEXT,
    SXML_VALUE_SYNTAX_EQNAME,
    SXML_VALUE_SYNTAX_NCNAME,
    SXML_VALUE_SYNTAX_EQNAMES,
    /* NameTests: names and wildcards (strip-space, catch errors). */
    SXML_VALUE_SYNTAX_NAME_TESTS,
    /* Mode names of xsl:template: EQNames, #default, #unnamed, #all. */
    SXML_VALUE_SYNTAX_MODES,
    /* The one mode of xsl:apply-templates: EQName, #default, #unnamed,
       #current. */
    SXML_VALUE_SYNTAX_MODE,
    /* EQNames or #all. */
    SXML_VALUE_SYNTAX_ACCUMULATOR_NAMES,
    /* Component tests of xsl:accept and xsl:expose: name tests, with an
       arity for functions. */
    SXML_VALUE_SYNTAX_COMPONENT_TESTS,
    SXML_VALUE_SYNTAX_PREFIX_OR_DEFAULT,
} value_syntax;

/* The bit of a keyword in an attribute's mask; every keyword is below 64. */
#define KEYWORD_BIT(keyword) ((uint64_t)1 << (uint64_t)(keyword))
_Static_assert(SXML_XSLT_AST_KEYWORD_ANY_COMPONENT < 64, "keywords fit a 64-bit mask");

typedef struct attribute_rule {
    sxml_byte_view attribute_name;
    sxml_xslt_ast_attribute_kind attribute_kind;
    value_syntax value_syntax;
    bool is_required;
    uint64_t keyword_mask;
} attribute_rule;

/* What an XSLT element's children may be (its syntax summary). */
typedef enum content_model {
    SXML_CONTENT_MODEL_EMPTY,
    SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
    /* Declarations (and xsl:expose, for xsl:package). */
    SXML_CONTENT_MODEL_DECLARATIONS,
    /* Only the listed children. */
    SXML_CONTENT_MODEL_CHILDREN,
    /* The listed children first, then a sequence constructor. */
    SXML_CONTENT_MODEL_CHILDREN_THEN_SEQUENCE_CONSTRUCTOR,
    /* A sequence constructor, then the listed children (xsl:try). */
    SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR_THEN_CHILDREN,
    /* Text that is an expression, and xsl:fallback (xsl:select). */
    SXML_CONTENT_MODEL_TEXT,
    /* An inline schema (xsl:import-schema), read by src/xslt_schema.c
       from the module's tree. */
    SXML_CONTENT_MODEL_SCHEMA,
} content_model;

typedef enum element_flag {
    SXML_ELEMENT_FLAG_MODULE_ROOT = 1U << 0U,
    SXML_ELEMENT_FLAG_DECLARATION = 1U << 1U,
    SXML_ELEMENT_FLAG_INSTRUCTION = 1U << 2U,
    /* 3.11.1: whitespace text children are removed whatever xml:space. */
    SXML_ELEMENT_FLAG_STRIPS_WHITESPACE = 1U << 3U,
} element_flag;

/* The most child kinds a content model lists (xsl:use-package). */
#define ELEMENT_RULE_MAXIMUM_CHILD_KINDS 6U

typedef struct element_rule {
    sxml_byte_view local_name;
    sxml_xslt_ast_kind kind;
    /* Bits of element_flag. */
    uint32_t element_flags;
    content_model content_model;
    sxml_xslt_ast_kind child_kinds[ELEMENT_RULE_MAXIMUM_CHILD_KINDS];
    uint32_t child_kind_count;
    /* A child that must occur at least once (xsl:when in xsl:choose). */
    sxml_xslt_ast_kind required_child_kind;
    /* The listed children come in the order of child_kinds ("(xsl:when+,
       xsl:otherwise?)"); bit n of single_child_mask: child_kinds[n] occurs
       at most once. */
    bool is_ordered;
    uint32_t single_child_mask;
    uint32_t first_attribute_rule;
    uint32_t attribute_rule_count;
} element_rule;

typedef struct keyword_spelling {
    sxml_byte_view spelling;
    sxml_xslt_ast_keyword keyword;
} keyword_spelling;

/* Generated from the XSLT 4.0 schema (attributes and their types) and
   the element syntax summaries (required attributes, content). */
static const attribute_rule attribute_rules[] = {
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("component"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_COMPONENT,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = true,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_ANY_COMPONENT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_ATTRIBUTE_SET) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_FUNCTION) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_ITEM_TYPE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_MODE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_RECORD_TYPE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_TEMPLATE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_VARIABLE)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("names"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAMES,
     .value_syntax = SXML_VALUE_SYNTAX_COMPONENT_TESTS, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("visibility"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_VISIBILITY,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = true,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_ABSTRACT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_FINAL) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_HIDDEN) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PRIVATE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PUBLIC)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("initial-value"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_INITIAL_VALUE,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("as"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_AS,
     .value_syntax = SXML_VALUE_SYNTAX_SEQUENCE_TYPE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("streamable"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_STREAMABLE,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("match"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_MATCH,
     .value_syntax = SXML_VALUE_SYNTAX_PATTERN, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("phase"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_PHASE,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_END) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_START)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("capture"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_CAPTURE,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("regex"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_REGEX,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("flags"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_FLAGS,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("compiled-regex"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_COMPILED_REGEX,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("separator"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SEPARATOR,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("mode"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_MODE,
     .value_syntax = SXML_VALUE_SYNTAX_MODE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("for-each"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_FOR_EACH,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("test"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_TEST,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("error-code"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ERROR_CODE,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("namespace"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAMESPACE,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("separator"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SEPARATOR,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("type"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_TYPE,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("validation"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_VALIDATION,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_LAX) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PRESERVE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRICT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRIP)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("streamable"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_STREAMABLE,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("use-attribute-sets"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_USE_ATTRIBUTE_SETS,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAMES, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("visibility"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_VISIBILITY,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_ABSTRACT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_FINAL) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PRIVATE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PUBLIC)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("errors"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ERRORS,
     .value_syntax = SXML_VALUE_SYNTAX_NAME_TESTS, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("use-character-maps"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_USE_CHARACTER_MAPS,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAMES, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("as"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_AS,
     .value_syntax = SXML_VALUE_SYNTAX_ITEM_TYPE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("use"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_USE,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_ABSENT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_OPTIONAL) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_REQUIRED)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("copy-namespaces"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_COPY_NAMESPACES,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("inherit-namespaces"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_INHERIT_NAMESPACES,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("use-attribute-sets"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_USE_ATTRIBUTE_SETS,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAMES, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("type"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_TYPE,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("validation"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_VALIDATION,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_LAX) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PRESERVE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRICT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRIP)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("copy-accumulators"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_COPY_ACCUMULATORS,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("copy-namespaces"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_COPY_NAMESPACES,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("type"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_TYPE,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("validation"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_VALIDATION,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_LAX) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PRESERVE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRICT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRIP)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("decimal-separator"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_DECIMAL_SEPARATOR,
     .value_syntax = SXML_VALUE_SYNTAX_CHARACTER_OR_RENDITION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("grouping-separator"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_GROUPING_SEPARATOR,
     .value_syntax = SXML_VALUE_SYNTAX_CHARACTER_OR_RENDITION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("infinity"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_INFINITY,
     .value_syntax = SXML_VALUE_SYNTAX_STRING, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("minus-sign"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_MINUS_SIGN,
     .value_syntax = SXML_VALUE_SYNTAX_STRING, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("exponent-separator"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_EXPONENT_SEPARATOR,
     .value_syntax = SXML_VALUE_SYNTAX_CHARACTER_OR_RENDITION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("NaN"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAN,
     .value_syntax = SXML_VALUE_SYNTAX_STRING, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("percent"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_PERCENT,
     .value_syntax = SXML_VALUE_SYNTAX_CHARACTER_OR_RENDITION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("per-mille"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_PER_MILLE,
     .value_syntax = SXML_VALUE_SYNTAX_CHARACTER_OR_RENDITION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("zero-digit"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ZERO_DIGIT,
     .value_syntax = SXML_VALUE_SYNTAX_CHARACTER, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("digit"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_DIGIT,
     .value_syntax = SXML_VALUE_SYNTAX_CHARACTER, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("pattern-separator"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_PATTERN_SEPARATOR,
     .value_syntax = SXML_VALUE_SYNTAX_CHARACTER, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("type"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_TYPE,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("validation"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_VALIDATION,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_LAX) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PRESERVE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRICT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRIP)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("namespace"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAMESPACE,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("inherit-namespaces"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_INHERIT_NAMESPACES,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("use-attribute-sets"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_USE_ATTRIBUTE_SETS,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAMES, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("type"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_TYPE,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("validation"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_VALIDATION,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_LAX) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PRESERVE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRICT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRIP)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("xpath"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_XPATH,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("as"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_AS,
     .value_syntax = SXML_VALUE_SYNTAX_SEQUENCE_TYPE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("base-uri"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_BASE_URI,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("context-item"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_CONTEXT_ITEM,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("namespace-context"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAMESPACE_CONTEXT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("schema-aware"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SCHEMA_AWARE,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("trusted"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_TRUSTED,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("with-params"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_WITH_PARAMS,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("component"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_COMPONENT,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = true,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_ANY_COMPONENT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_ATTRIBUTE_SET) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_FUNCTION) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_ITEM_TYPE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_MODE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_RECORD_TYPE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_TEMPLATE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_VARIABLE)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("names"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAMES,
     .value_syntax = SXML_VALUE_SYNTAX_COMPONENT_TESTS, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("visibility"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_VISIBILITY,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = true,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_ABSTRACT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_FINAL) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PRIVATE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PUBLIC)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_NCNAME, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("as"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_AS,
     .value_syntax = SXML_VALUE_SYNTAX_SEQUENCE_TYPE, .is_required = false,
     .keyword_mask = 0},
    /* xsl:field as the draft has it (5.4.2: static and select; the
       schema still has default and required). */
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("static"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_STATIC,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("separator"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SEPARATOR,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("group-by"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_GROUP_BY,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("group-adjacent"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_GROUP_ADJACENT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("group-starting-with"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_GROUP_STARTING_WITH,
     .value_syntax = SXML_VALUE_SYNTAX_PATTERN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("group-ending-with"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_GROUP_ENDING_WITH,
     .value_syntax = SXML_VALUE_SYNTAX_PATTERN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("merge-when"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_MERGE_WHEN,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("split-when"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SPLIT_WHEN,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("composite"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_COMPOSITE,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("collation"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_COLLATION,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("override"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_OVERRIDE,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("as"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_AS,
     .value_syntax = SXML_VALUE_SYNTAX_SEQUENCE_TYPE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("visibility"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_VISIBILITY,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_ABSTRACT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_FINAL) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PRIVATE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PUBLIC)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("streamability"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_STREAMABILITY,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_ABSORBING) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_ASCENT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_DEEP_DESCENT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_FILTER) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_INSPECTION) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_SHALLOW_DESCENT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_UNCLASSIFIED)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("override-extension-function"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_OVERRIDE_EXTENSION_FUNCTION,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("new-each-time"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NEW_EACH_TIME,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_MAYBE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_NO) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_YES)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("cache"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_CACHE,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("as"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_AS,
     .value_syntax = SXML_VALUE_SYNTAX_ITEM_TYPE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("use"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_USE,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_ABSENT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_OPTIONAL) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_REQUIRED)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("test"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_TEST,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("then"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_THEN,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("else"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ELSE,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("href"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_HREF,
     .value_syntax = SXML_VALUE_SYNTAX_TOKEN, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("namespace"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAMESPACE,
     .value_syntax = SXML_VALUE_SYNTAX_TOKEN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("role"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ROLE,
     .value_syntax = SXML_VALUE_SYNTAX_NCNAME, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("schema-location"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SCHEMA_LOCATION,
     .value_syntax = SXML_VALUE_SYNTAX_TOKEN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("href"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_HREF,
     .value_syntax = SXML_VALUE_SYNTAX_TOKEN, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("as"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_AS,
     .value_syntax = SXML_VALUE_SYNTAX_ITEM_TYPE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("visibility"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_VISIBILITY,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PRIVATE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PUBLIC)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("match"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_MATCH,
     .value_syntax = SXML_VALUE_SYNTAX_PATTERN, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("use"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_USE,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("composite"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_COMPOSITE,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("collation"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_COLLATION,
     .value_syntax = SXML_VALUE_SYNTAX_TOKEN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("duplicates"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_DUPLICATES,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("key"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_KEY,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("lang"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_LANG,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("order"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ORDER,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("collation"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_COLLATION,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("case-order"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_CASE_ORDER,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("data-type"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_DATA_TYPE,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_NCNAME, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("for-each-item"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_FOR_EACH_ITEM,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("for-each-source"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_FOR_EACH_SOURCE,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("streamable"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_STREAMABLE,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("use-accumulators"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_USE_ACCUMULATORS,
     .value_syntax = SXML_VALUE_SYNTAX_ACCUMULATOR_NAMES, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("sort-before-merge"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SORT_BEFORE_MERGE,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("type"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_TYPE,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("validation"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_VALIDATION,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_LAX) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PRESERVE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRICT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRIP)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("terminate"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_TERMINATE,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("error-code"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ERROR_CODE,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("as"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_AS,
     .value_syntax = SXML_VALUE_SYNTAX_SEQUENCE_TYPE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("copy-namespaces"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_COPY_NAMESPACES,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("streamable"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_STREAMABLE,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("use-accumulators"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_USE_ACCUMULATORS,
     .value_syntax = SXML_VALUE_SYNTAX_ACCUMULATOR_NAMES, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("on-no-match"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ON_NO_MATCH,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_DEEP_COPY) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_DEEP_SKIP) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_FAIL) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_SHALLOW_COPY) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_SHALLOW_COPY_ALL) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_SHALLOW_SKIP) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_TEXT_ONLY_COPY)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("on-multiple-match"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ON_MULTIPLE_MATCH,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_FAIL) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_USE_LAST)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("warning-on-no-match"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_WARNING_ON_NO_MATCH,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("warning-on-multiple-match"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_WARNING_ON_MULTIPLE_MATCH,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("typed"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_TYPED,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_LAX) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_NO) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRICT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_UNSPECIFIED) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_YES)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("visibility"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_VISIBILITY,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_FINAL) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PRIVATE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PUBLIC)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("stylesheet-prefix"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_STYLESHEET_PREFIX,
     .value_syntax = SXML_VALUE_SYNTAX_PREFIX_OR_DEFAULT, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("result-prefix"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_RESULT_PREFIX,
     .value_syntax = SXML_VALUE_SYNTAX_PREFIX_OR_DEFAULT, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("value"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_VALUE,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("level"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_LEVEL,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_ANY) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_MULTIPLE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_SINGLE)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("count"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_COUNT,
     .value_syntax = SXML_VALUE_SYNTAX_PATTERN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("from"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_FROM,
     .value_syntax = SXML_VALUE_SYNTAX_PATTERN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("format"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_FORMAT,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("lang"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_LANG,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("letter-value"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_LETTER_VALUE,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("ordinal"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ORDINAL,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("start-at"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_START_AT,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("grouping-separator"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_GROUPING_SEPARATOR,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("grouping-size"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_GROUPING_SIZE,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("method"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_METHOD,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD_OR_NAME, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_ADAPTIVE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_CSV) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_HTML) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_JSON) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_TEXT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_XHTML) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_XML)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("allow-duplicate-names"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ALLOW_DUPLICATE_NAMES,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("build-tree"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_BUILD_TREE,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("byte-order-mark"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_BYTE_ORDER_MARK,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("canonical"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_CANONICAL,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("cdata-section-elements"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_CDATA_SECTION_ELEMENTS,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAMES, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("csv-header"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_CSV_HEADER,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("csv-quote-character"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_CSV_QUOTE_CHARACTER,
     .value_syntax = SXML_VALUE_SYNTAX_STRING, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("csv-separator"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_CSV_SEPARATOR,
     .value_syntax = SXML_VALUE_SYNTAX_STRING, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("doctype-public"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_DOCTYPE_PUBLIC,
     .value_syntax = SXML_VALUE_SYNTAX_STRING, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("doctype-system"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_DOCTYPE_SYSTEM,
     .value_syntax = SXML_VALUE_SYNTAX_STRING, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("encoding"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ENCODING,
     .value_syntax = SXML_VALUE_SYNTAX_STRING, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("escape-solidus"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ESCAPE_SOLIDUS,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("escape-uri-attributes"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ESCAPE_URI_ATTRIBUTES,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("html-version"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_HTML_VERSION,
     .value_syntax = SXML_VALUE_SYNTAX_DECIMAL, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("include-content-type"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_INCLUDE_CONTENT_TYPE,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("indent"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_INDENT,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("indent-attributes"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_INDENT_ATTRIBUTES,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("indent-unit"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_INDENT_UNIT,
     .value_syntax = SXML_VALUE_SYNTAX_STRING, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("item-separator"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ITEM_SEPARATOR,
     .value_syntax = SXML_VALUE_SYNTAX_STRING, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("json-lines"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_JSON_LINES,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("json-node-output-method"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_JSON_NODE_OUTPUT_METHOD,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD_OR_NAME, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_HTML) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_TEXT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_XHTML) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_XML)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("line-ending"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_LINE_ENDING,
     .value_syntax = SXML_VALUE_SYNTAX_STRING, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("media-type"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_MEDIA_TYPE,
     .value_syntax = SXML_VALUE_SYNTAX_STRING, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("normalization-form"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NORMALIZATION_FORM,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD_OR_TEXT, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_FULLY_NORMALIZED) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_NFC) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_NFD) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_NFKC) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_NFKD) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_NO_NORMALIZATION)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("omit-xml-declaration"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_OMIT_XML_DECLARATION,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("parameter-document"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_PARAMETER_DOCUMENT,
     .value_syntax = SXML_VALUE_SYNTAX_TOKEN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("standalone"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_STANDALONE,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_NO) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_OMIT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_YES)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("suppress-indentation"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SUPPRESS_INDENTATION,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAMES, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("undeclare-prefixes"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_UNDECLARE_PREFIXES,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("use-character-maps"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_USE_CHARACTER_MAPS,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAMES, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("version"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_VERSION,
     .value_syntax = SXML_VALUE_SYNTAX_TOKEN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("character"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_CHARACTER,
     .value_syntax = SXML_VALUE_SYNTAX_CHARACTER, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("string"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_STRING,
     .value_syntax = SXML_VALUE_SYNTAX_STRING, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("declared-modes"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_DECLARED_MODES,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("id"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ID,
     .value_syntax = SXML_VALUE_SYNTAX_TOKEN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_TOKEN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("package-version"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_PACKAGE_VERSION,
     .value_syntax = SXML_VALUE_SYNTAX_STRING, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("input-type-annotations"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_INPUT_TYPE_ANNOTATIONS,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PRESERVE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRIP) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_UNSPECIFIED)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("href"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_HREF,
     .value_syntax = SXML_VALUE_SYNTAX_TOKEN, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("path-in-archive"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_PATH_IN_ARCHIVE,
     .value_syntax = SXML_VALUE_SYNTAX_STRING, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("archive-type"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ARCHIVE_TYPE,
     .value_syntax = SXML_VALUE_SYNTAX_STRING, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("is-priority"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_IS_PRIORITY,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("format"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_FORMAT,
     .value_syntax = SXML_VALUE_SYNTAX_STRING, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("as"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_AS,
     .value_syntax = SXML_VALUE_SYNTAX_SEQUENCE_TYPE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("required"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_REQUIRED,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("tunnel"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_TUNNEL,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("static"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_STATIC,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("elements"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ELEMENTS,
     .value_syntax = SXML_VALUE_SYNTAX_NAME_TESTS, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("constructor"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_CONSTRUCTOR,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("visibility"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_VISIBILITY,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PRIVATE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PUBLIC)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("format"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_FORMAT,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("href"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_HREF,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("type"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_TYPE,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("validation"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_VALIDATION,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_LAX) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PRESERVE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRICT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRIP)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("method"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_METHOD,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("allow-duplicate-names"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ALLOW_DUPLICATE_NAMES,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("build-tree"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_BUILD_TREE,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("byte-order-mark"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_BYTE_ORDER_MARK,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("canonical"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_CANONICAL,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("cdata-section-elements"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_CDATA_SECTION_ELEMENTS,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("csv-header"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_CSV_HEADER,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("csv-quote-character"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_CSV_QUOTE_CHARACTER,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("csv-separator"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_CSV_SEPARATOR,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("doctype-public"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_DOCTYPE_PUBLIC,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("doctype-system"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_DOCTYPE_SYSTEM,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("encoding"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ENCODING,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("escape-solidus"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ESCAPE_SOLIDUS,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("escape-uri-attributes"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ESCAPE_URI_ATTRIBUTES,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("html-version"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_HTML_VERSION,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("include-content-type"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_INCLUDE_CONTENT_TYPE,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("indent"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_INDENT,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("indent-attributes"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_INDENT_ATTRIBUTES,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("indent-unit"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_INDENT_UNIT,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("item-separator"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ITEM_SEPARATOR,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("json-lines"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_JSON_LINES,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("json-node-output-method"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_JSON_NODE_OUTPUT_METHOD,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("line-ending"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_LINE_ENDING,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("media-type"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_MEDIA_TYPE,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("normalization-form"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NORMALIZATION_FORM,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("omit-xml-declaration"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_OMIT_XML_DECLARATION,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("parameter-document"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_PARAMETER_DOCUMENT,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("standalone"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_STANDALONE,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("suppress-indentation"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SUPPRESS_INDENTATION,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("undeclare-prefixes"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_UNDECLARE_PREFIXES,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("use-character-maps"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_USE_CHARACTER_MAPS,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAMES, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("output-version"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_OUTPUT_VERSION,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("as"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_AS,
     .value_syntax = SXML_VALUE_SYNTAX_SEQUENCE_TYPE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("as"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_AS,
     .value_syntax = SXML_VALUE_SYNTAX_SEQUENCE_TYPE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("lang"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_LANG,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("data-type"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_DATA_TYPE,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("order"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ORDER,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("case-order"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_CASE_ORDER,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("collation"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_COLLATION,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("stable"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_STABLE,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("href"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_HREF,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("streamable"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_STREAMABLE,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("use-accumulators"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_USE_ACCUMULATORS,
     .value_syntax = SXML_VALUE_SYNTAX_ACCUMULATOR_NAMES, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("type"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_TYPE,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("validation"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_VALIDATION,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_LAX) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PRESERVE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRICT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRIP)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("elements"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ELEMENTS,
     .value_syntax = SXML_VALUE_SYNTAX_NAME_TESTS, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("id"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ID,
     .value_syntax = SXML_VALUE_SYNTAX_TOKEN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("input-type-annotations"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_INPUT_TYPE_ANNOTATIONS,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PRESERVE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRIP) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_UNSPECIFIED)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("main-module"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_MAIN_MODULE,
     .value_syntax = SXML_VALUE_SYNTAX_TOKEN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("match"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_MATCH,
     .value_syntax = SXML_VALUE_SYNTAX_PATTERN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("priority"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_PRIORITY,
     .value_syntax = SXML_VALUE_SYNTAX_DECIMAL, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("mode"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_MODE,
     .value_syntax = SXML_VALUE_SYNTAX_MODES, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("as"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_AS,
     .value_syntax = SXML_VALUE_SYNTAX_SEQUENCE_TYPE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("visibility"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_VISIBILITY,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_ABSTRACT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_FINAL) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PRIVATE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PUBLIC)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("separator"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SEPARATOR,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("cdata"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_CDATA,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("disable-output-escaping"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_DISABLE_OUTPUT_ESCAPING,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("id"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ID,
     .value_syntax = SXML_VALUE_SYNTAX_TOKEN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("input-type-annotations"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_INPUT_TYPE_ANNOTATIONS,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PRESERVE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRIP) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_UNSPECIFIED)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("main-module"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_MAIN_MODULE,
     .value_syntax = SXML_VALUE_SYNTAX_TOKEN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("rollback-output"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_ROLLBACK_OUTPUT,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_TOKEN, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("package-version"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_PACKAGE_VERSION,
     .value_syntax = SXML_VALUE_SYNTAX_STRING, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("separator"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SEPARATOR,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("cdata"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_CDATA,
     .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("disable-output-escaping"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_DISABLE_OUTPUT_ESCAPING,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("as"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_AS,
     .value_syntax = SXML_VALUE_SYNTAX_SEQUENCE_TYPE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("visibility"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_VISIBILITY,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD, .is_required = false,
     .keyword_mask = 
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_ABSTRACT) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_FINAL) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PRIVATE) |
         KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PUBLIC)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("static"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_STATIC,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("test"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_TEST,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("name"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_NAME,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME, .is_required = true,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("as"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_AS,
     .value_syntax = SXML_VALUE_SYNTAX_SEQUENCE_TYPE, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("tunnel"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_TUNNEL,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN, .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("select"), .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
     .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION, .is_required = false,
     .keyword_mask = 0},
};

static const element_rule element_rules[] = {
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("accept"), .kind = SXML_XSLT_AST_KIND_ACCEPT,
     .element_flags = 0,
     .content_model = SXML_CONTENT_MODEL_EMPTY,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 0, .attribute_rule_count = 3},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("accumulator"), .kind = SXML_XSLT_AST_KIND_ACCUMULATOR,
     .element_flags = SXML_ELEMENT_FLAG_DECLARATION | SXML_ELEMENT_FLAG_STRIPS_WHITESPACE,
     .content_model = SXML_CONTENT_MODEL_CHILDREN,
     .child_kinds = {SXML_XSLT_AST_KIND_ACCUMULATOR_RULE},
     .child_kind_count = 1, .required_child_kind = SXML_XSLT_AST_KIND_ACCUMULATOR_RULE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 3, .attribute_rule_count = 4},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("accumulator-rule"), .kind = SXML_XSLT_AST_KIND_ACCUMULATOR_RULE,
     .element_flags = 0,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 7, .attribute_rule_count = 4},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("analyze-string"), .kind = SXML_XSLT_AST_KIND_ANALYZE_STRING,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION | SXML_ELEMENT_FLAG_STRIPS_WHITESPACE,
     .content_model = SXML_CONTENT_MODEL_CHILDREN,
     .child_kinds = {SXML_XSLT_AST_KIND_MATCHING_SUBSTRING, SXML_XSLT_AST_KIND_NON_MATCHING_SUBSTRING, SXML_XSLT_AST_KIND_FALLBACK},
     .child_kind_count = 3, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = true, .single_child_mask = 3U,
     .first_attribute_rule = 11, .attribute_rule_count = 4},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("apply-imports"), .kind = SXML_XSLT_AST_KIND_APPLY_IMPORTS,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION | SXML_ELEMENT_FLAG_STRIPS_WHITESPACE,
     .content_model = SXML_CONTENT_MODEL_CHILDREN,
     .child_kinds = {SXML_XSLT_AST_KIND_WITH_PARAM},
     .child_kind_count = 1, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 15, .attribute_rule_count = 0},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("apply-templates"), .kind = SXML_XSLT_AST_KIND_APPLY_TEMPLATES,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION | SXML_ELEMENT_FLAG_STRIPS_WHITESPACE,
     .content_model = SXML_CONTENT_MODEL_CHILDREN,
     .child_kinds = {SXML_XSLT_AST_KIND_SORT, SXML_XSLT_AST_KIND_WITH_PARAM},
     .child_kind_count = 2, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 15, .attribute_rule_count = 3},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("array"), .kind = SXML_XSLT_AST_KIND_ARRAY,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 18, .attribute_rule_count = 2},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("array-member"), .kind = SXML_XSLT_AST_KIND_ARRAY_MEMBER,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 20, .attribute_rule_count = 1},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("assert"), .kind = SXML_XSLT_AST_KIND_ASSERT,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 21, .attribute_rule_count = 3},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("attribute"), .kind = SXML_XSLT_AST_KIND_ATTRIBUTE,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 24, .attribute_rule_count = 6},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("attribute-set"), .kind = SXML_XSLT_AST_KIND_ATTRIBUTE_SET,
     .element_flags = SXML_ELEMENT_FLAG_DECLARATION | SXML_ELEMENT_FLAG_STRIPS_WHITESPACE,
     .content_model = SXML_CONTENT_MODEL_CHILDREN,
     .child_kinds = {SXML_XSLT_AST_KIND_ATTRIBUTE},
     .child_kind_count = 1, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 30, .attribute_rule_count = 4},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("break"), .kind = SXML_XSLT_AST_KIND_BREAK,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 34, .attribute_rule_count = 1},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("call-template"), .kind = SXML_XSLT_AST_KIND_CALL_TEMPLATE,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION | SXML_ELEMENT_FLAG_STRIPS_WHITESPACE,
     .content_model = SXML_CONTENT_MODEL_CHILDREN,
     .child_kinds = {SXML_XSLT_AST_KIND_WITH_PARAM},
     .child_kind_count = 1, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 35, .attribute_rule_count = 1},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("catch"), .kind = SXML_XSLT_AST_KIND_CATCH,
     .element_flags = 0,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 36, .attribute_rule_count = 2},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("character-map"), .kind = SXML_XSLT_AST_KIND_CHARACTER_MAP,
     .element_flags = SXML_ELEMENT_FLAG_DECLARATION | SXML_ELEMENT_FLAG_STRIPS_WHITESPACE,
     .content_model = SXML_CONTENT_MODEL_CHILDREN,
     .child_kinds = {SXML_XSLT_AST_KIND_OUTPUT_CHARACTER},
     .child_kind_count = 1, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 38, .attribute_rule_count = 2},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("choose"), .kind = SXML_XSLT_AST_KIND_CHOOSE,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION | SXML_ELEMENT_FLAG_STRIPS_WHITESPACE,
     .content_model = SXML_CONTENT_MODEL_CHILDREN,
     .child_kinds = {SXML_XSLT_AST_KIND_WHEN, SXML_XSLT_AST_KIND_OTHERWISE},
     .child_kind_count = 2, .required_child_kind = SXML_XSLT_AST_KIND_WHEN,
     .is_ordered = true, .single_child_mask = 2U,
     .first_attribute_rule = 40, .attribute_rule_count = 0},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("comment"), .kind = SXML_XSLT_AST_KIND_COMMENT,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 40, .attribute_rule_count = 1},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("context-item"), .kind = SXML_XSLT_AST_KIND_CONTEXT_ITEM,
     .element_flags = 0,
     .content_model = SXML_CONTENT_MODEL_EMPTY,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 41, .attribute_rule_count = 2},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("copy"), .kind = SXML_XSLT_AST_KIND_COPY,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 43, .attribute_rule_count = 6},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("copy-of"), .kind = SXML_XSLT_AST_KIND_COPY_OF,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_EMPTY,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 49, .attribute_rule_count = 5},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("decimal-format"), .kind = SXML_XSLT_AST_KIND_DECIMAL_FORMAT,
     .element_flags = SXML_ELEMENT_FLAG_DECLARATION,
     .content_model = SXML_CONTENT_MODEL_EMPTY,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 54, .attribute_rule_count = 12},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("document"), .kind = SXML_XSLT_AST_KIND_DOCUMENT,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 66, .attribute_rule_count = 2},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("element"), .kind = SXML_XSLT_AST_KIND_ELEMENT,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 68, .attribute_rule_count = 6},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("evaluate"), .kind = SXML_XSLT_AST_KIND_EVALUATE,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION | SXML_ELEMENT_FLAG_STRIPS_WHITESPACE,
     .content_model = SXML_CONTENT_MODEL_CHILDREN,
     .child_kinds = {SXML_XSLT_AST_KIND_WITH_PARAM, SXML_XSLT_AST_KIND_FALLBACK},
     .child_kind_count = 2, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 74, .attribute_rule_count = 8},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("expose"), .kind = SXML_XSLT_AST_KIND_EXPOSE,
     .element_flags = 0,
     .content_model = SXML_CONTENT_MODEL_EMPTY,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 82, .attribute_rule_count = 3},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("fallback"), .kind = SXML_XSLT_AST_KIND_FALLBACK,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 85, .attribute_rule_count = 0},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("field"), .kind = SXML_XSLT_AST_KIND_FIELD,
     .element_flags = 0,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 85, .attribute_rule_count = 4},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("for-each"), .kind = SXML_XSLT_AST_KIND_FOR_EACH,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_CHILDREN_THEN_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_SORT},
     .child_kind_count = 1, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = true, .single_child_mask = 0U,
     .first_attribute_rule = 89, .attribute_rule_count = 2},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("for-each-group"), .kind = SXML_XSLT_AST_KIND_FOR_EACH_GROUP,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_CHILDREN_THEN_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_SORT},
     .child_kind_count = 1, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = true, .single_child_mask = 0U,
     .first_attribute_rule = 91, .attribute_rule_count = 9},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("fork"), .kind = SXML_XSLT_AST_KIND_FORK,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION | SXML_ELEMENT_FLAG_STRIPS_WHITESPACE,
     .content_model = SXML_CONTENT_MODEL_CHILDREN,
     .child_kinds = {SXML_XSLT_AST_KIND_FALLBACK, SXML_XSLT_AST_KIND_SEQUENCE, SXML_XSLT_AST_KIND_FOR_EACH_GROUP},
     .child_kind_count = 3, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 100, .attribute_rule_count = 0},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("function"), .kind = SXML_XSLT_AST_KIND_FUNCTION,
     .element_flags = SXML_ELEMENT_FLAG_DECLARATION,
     .content_model = SXML_CONTENT_MODEL_CHILDREN_THEN_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_PARAM},
     .child_kind_count = 1, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = true, .single_child_mask = 0U,
     .first_attribute_rule = 100, .attribute_rule_count = 8},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("global-context-item"), .kind = SXML_XSLT_AST_KIND_GLOBAL_CONTEXT_ITEM,
     .element_flags = SXML_ELEMENT_FLAG_DECLARATION,
     .content_model = SXML_CONTENT_MODEL_EMPTY,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 108, .attribute_rule_count = 2},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("if"), .kind = SXML_XSLT_AST_KIND_IF,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 110, .attribute_rule_count = 3},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("import"), .kind = SXML_XSLT_AST_KIND_IMPORT,
     .element_flags = SXML_ELEMENT_FLAG_DECLARATION,
     .content_model = SXML_CONTENT_MODEL_EMPTY,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 113, .attribute_rule_count = 1},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("import-schema"), .kind = SXML_XSLT_AST_KIND_IMPORT_SCHEMA,
     .element_flags = SXML_ELEMENT_FLAG_DECLARATION,
     .content_model = SXML_CONTENT_MODEL_SCHEMA,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 114, .attribute_rule_count = 3},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("include"), .kind = SXML_XSLT_AST_KIND_INCLUDE,
     .element_flags = SXML_ELEMENT_FLAG_DECLARATION,
     .content_model = SXML_CONTENT_MODEL_EMPTY,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 117, .attribute_rule_count = 1},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("item-type"), .kind = SXML_XSLT_AST_KIND_ITEM_TYPE,
     .element_flags = SXML_ELEMENT_FLAG_DECLARATION,
     .content_model = SXML_CONTENT_MODEL_EMPTY,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 118, .attribute_rule_count = 3},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("iterate"), .kind = SXML_XSLT_AST_KIND_ITERATE,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_CHILDREN_THEN_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_PARAM, SXML_XSLT_AST_KIND_ON_COMPLETION},
     .child_kind_count = 2, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = true, .single_child_mask = 2U,
     .first_attribute_rule = 121, .attribute_rule_count = 1},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("key"), .kind = SXML_XSLT_AST_KIND_KEY,
     .element_flags = SXML_ELEMENT_FLAG_DECLARATION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 122, .attribute_rule_count = 5},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("map"), .kind = SXML_XSLT_AST_KIND_MAP,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 127, .attribute_rule_count = 2},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("map-entry"), .kind = SXML_XSLT_AST_KIND_MAP_ENTRY,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 129, .attribute_rule_count = 2},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("matching-substring"), .kind = SXML_XSLT_AST_KIND_MATCHING_SUBSTRING,
     .element_flags = 0,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 131, .attribute_rule_count = 1},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("merge"), .kind = SXML_XSLT_AST_KIND_MERGE,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION | SXML_ELEMENT_FLAG_STRIPS_WHITESPACE,
     .content_model = SXML_CONTENT_MODEL_CHILDREN,
     .child_kinds = {SXML_XSLT_AST_KIND_MERGE_SOURCE, SXML_XSLT_AST_KIND_MERGE_ACTION, SXML_XSLT_AST_KIND_FALLBACK},
     .child_kind_count = 3, .required_child_kind = SXML_XSLT_AST_KIND_MERGE_SOURCE,
     .is_ordered = true, .single_child_mask = 2U,
     .first_attribute_rule = 132, .attribute_rule_count = 0},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("merge-action"), .kind = SXML_XSLT_AST_KIND_MERGE_ACTION,
     .element_flags = 0,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 132, .attribute_rule_count = 0},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("merge-key"), .kind = SXML_XSLT_AST_KIND_MERGE_KEY,
     .element_flags = 0,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 132, .attribute_rule_count = 6},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("merge-source"), .kind = SXML_XSLT_AST_KIND_MERGE_SOURCE,
     .element_flags = SXML_ELEMENT_FLAG_STRIPS_WHITESPACE,
     .content_model = SXML_CONTENT_MODEL_CHILDREN,
     .child_kinds = {SXML_XSLT_AST_KIND_MERGE_KEY},
     .child_kind_count = 1, .required_child_kind = SXML_XSLT_AST_KIND_MERGE_KEY,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 138, .attribute_rule_count = 9},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("message"), .kind = SXML_XSLT_AST_KIND_MESSAGE,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 147, .attribute_rule_count = 3},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("mode"), .kind = SXML_XSLT_AST_KIND_MODE,
     .element_flags = SXML_ELEMENT_FLAG_DECLARATION | SXML_ELEMENT_FLAG_STRIPS_WHITESPACE,
     .content_model = SXML_CONTENT_MODEL_CHILDREN,
     .child_kinds = {SXML_XSLT_AST_KIND_TEMPLATE},
     .child_kind_count = 1, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 150, .attribute_rule_count = 11},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("namespace"), .kind = SXML_XSLT_AST_KIND_NAMESPACE,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 161, .attribute_rule_count = 2},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("namespace-alias"), .kind = SXML_XSLT_AST_KIND_NAMESPACE_ALIAS,
     .element_flags = SXML_ELEMENT_FLAG_DECLARATION,
     .content_model = SXML_CONTENT_MODEL_EMPTY,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 163, .attribute_rule_count = 2},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("next-iteration"), .kind = SXML_XSLT_AST_KIND_NEXT_ITERATION,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION | SXML_ELEMENT_FLAG_STRIPS_WHITESPACE,
     .content_model = SXML_CONTENT_MODEL_CHILDREN,
     .child_kinds = {SXML_XSLT_AST_KIND_WITH_PARAM},
     .child_kind_count = 1, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 165, .attribute_rule_count = 0},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("next-match"), .kind = SXML_XSLT_AST_KIND_NEXT_MATCH,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION | SXML_ELEMENT_FLAG_STRIPS_WHITESPACE,
     .content_model = SXML_CONTENT_MODEL_CHILDREN,
     .child_kinds = {SXML_XSLT_AST_KIND_WITH_PARAM, SXML_XSLT_AST_KIND_FALLBACK},
     .child_kind_count = 2, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 165, .attribute_rule_count = 0},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("non-matching-substring"), .kind = SXML_XSLT_AST_KIND_NON_MATCHING_SUBSTRING,
     .element_flags = 0,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 165, .attribute_rule_count = 1},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("number"), .kind = SXML_XSLT_AST_KIND_NUMBER,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_EMPTY,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 166, .attribute_rule_count = 12},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("on-completion"), .kind = SXML_XSLT_AST_KIND_ON_COMPLETION,
     .element_flags = 0,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 178, .attribute_rule_count = 1},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("on-empty"), .kind = SXML_XSLT_AST_KIND_ON_EMPTY,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 179, .attribute_rule_count = 1},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("on-non-empty"), .kind = SXML_XSLT_AST_KIND_ON_NON_EMPTY,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 180, .attribute_rule_count = 1},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("otherwise"), .kind = SXML_XSLT_AST_KIND_OTHERWISE,
     .element_flags = 0,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 181, .attribute_rule_count = 1},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("output"), .kind = SXML_XSLT_AST_KIND_OUTPUT,
     .element_flags = SXML_ELEMENT_FLAG_DECLARATION,
     .content_model = SXML_CONTENT_MODEL_EMPTY,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 182, .attribute_rule_count = 33},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("output-character"), .kind = SXML_XSLT_AST_KIND_OUTPUT_CHARACTER,
     .element_flags = 0,
     .content_model = SXML_CONTENT_MODEL_EMPTY,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 215, .attribute_rule_count = 2},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("override"), .kind = SXML_XSLT_AST_KIND_OVERRIDE,
     .element_flags = SXML_ELEMENT_FLAG_STRIPS_WHITESPACE,
     .content_model = SXML_CONTENT_MODEL_CHILDREN,
     .child_kinds = {SXML_XSLT_AST_KIND_TEMPLATE, SXML_XSLT_AST_KIND_FUNCTION, SXML_XSLT_AST_KIND_VARIABLE, SXML_XSLT_AST_KIND_PARAM, SXML_XSLT_AST_KIND_ATTRIBUTE_SET},
     .child_kind_count = 5, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 217, .attribute_rule_count = 0},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("package"), .kind = SXML_XSLT_AST_KIND_PACKAGE,
     .element_flags = SXML_ELEMENT_FLAG_MODULE_ROOT | SXML_ELEMENT_FLAG_STRIPS_WHITESPACE,
     .content_model = SXML_CONTENT_MODEL_DECLARATIONS,
     .child_kinds = {SXML_XSLT_AST_KIND_EXPOSE},
     .child_kind_count = 1, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 217, .attribute_rule_count = 5},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("package-location"), .kind = SXML_XSLT_AST_KIND_PACKAGE_LOCATION,
     .element_flags = 0,
     .content_model = SXML_CONTENT_MODEL_EMPTY,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 222, .attribute_rule_count = 5},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("param"), .kind = SXML_XSLT_AST_KIND_PARAM,
     .element_flags = SXML_ELEMENT_FLAG_DECLARATION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 227, .attribute_rule_count = 6},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("perform-sort"), .kind = SXML_XSLT_AST_KIND_PERFORM_SORT,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_CHILDREN_THEN_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_SORT},
     .child_kind_count = 1, .required_child_kind = SXML_XSLT_AST_KIND_SORT,
     .is_ordered = true, .single_child_mask = 0U,
     .first_attribute_rule = 233, .attribute_rule_count = 1},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("preserve-space"), .kind = SXML_XSLT_AST_KIND_PRESERVE_SPACE,
     .element_flags = SXML_ELEMENT_FLAG_DECLARATION,
     .content_model = SXML_CONTENT_MODEL_EMPTY,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 234, .attribute_rule_count = 1},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("processing-instruction"), .kind = SXML_XSLT_AST_KIND_PROCESSING_INSTRUCTION,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 235, .attribute_rule_count = 2},
    /* xsl:record (4.0; D142): its attributes are fields, read by
       add_node_attributes, so it has no attribute rules. */
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("record"), .kind = SXML_XSLT_AST_KIND_RECORD,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 237, .attribute_rule_count = 0},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("record-type"), .kind = SXML_XSLT_AST_KIND_RECORD_TYPE,
     .element_flags = SXML_ELEMENT_FLAG_DECLARATION,
     .content_model = SXML_CONTENT_MODEL_CHILDREN,
     .child_kinds = {SXML_XSLT_AST_KIND_FIELD},
     .child_kind_count = 1, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 237, .attribute_rule_count = 3},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("result-document"), .kind = SXML_XSLT_AST_KIND_RESULT_DOCUMENT,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 240, .attribute_rule_count = 37},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("select"), .kind = SXML_XSLT_AST_KIND_SELECT,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_TEXT,
     .child_kinds = {SXML_XSLT_AST_KIND_FALLBACK},
     .child_kind_count = 1, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 277, .attribute_rule_count = 1},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("sequence"), .kind = SXML_XSLT_AST_KIND_SEQUENCE,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 278, .attribute_rule_count = 2},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("sort"), .kind = SXML_XSLT_AST_KIND_SORT,
     .element_flags = 0,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 280, .attribute_rule_count = 7},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("source-document"), .kind = SXML_XSLT_AST_KIND_SOURCE_DOCUMENT,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 287, .attribute_rule_count = 5},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("strip-space"), .kind = SXML_XSLT_AST_KIND_STRIP_SPACE,
     .element_flags = SXML_ELEMENT_FLAG_DECLARATION,
     .content_model = SXML_CONTENT_MODEL_EMPTY,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 292, .attribute_rule_count = 1},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("stylesheet"), .kind = SXML_XSLT_AST_KIND_STYLESHEET,
     .element_flags = SXML_ELEMENT_FLAG_MODULE_ROOT | SXML_ELEMENT_FLAG_STRIPS_WHITESPACE,
     .content_model = SXML_CONTENT_MODEL_DECLARATIONS,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 293, .attribute_rule_count = 3},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("switch"), .kind = SXML_XSLT_AST_KIND_SWITCH,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION | SXML_ELEMENT_FLAG_STRIPS_WHITESPACE,
     .content_model = SXML_CONTENT_MODEL_CHILDREN,
     .child_kinds = {SXML_XSLT_AST_KIND_WHEN, SXML_XSLT_AST_KIND_OTHERWISE, SXML_XSLT_AST_KIND_FALLBACK},
     .child_kind_count = 3, .required_child_kind = SXML_XSLT_AST_KIND_WHEN,
     .is_ordered = true, .single_child_mask = 2U,
     .first_attribute_rule = 296, .attribute_rule_count = 1},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("template"), .kind = SXML_XSLT_AST_KIND_TEMPLATE,
     .element_flags = SXML_ELEMENT_FLAG_DECLARATION,
     .content_model = SXML_CONTENT_MODEL_CHILDREN_THEN_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_CONTEXT_ITEM, SXML_XSLT_AST_KIND_PARAM},
     .child_kind_count = 2, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = true, .single_child_mask = 1U,
     .first_attribute_rule = 297, .attribute_rule_count = 6},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("text"), .kind = SXML_XSLT_AST_KIND_TEXT,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 303, .attribute_rule_count = 4},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("transform"), .kind = SXML_XSLT_AST_KIND_STYLESHEET,
     .element_flags = SXML_ELEMENT_FLAG_MODULE_ROOT | SXML_ELEMENT_FLAG_STRIPS_WHITESPACE,
     .content_model = SXML_CONTENT_MODEL_DECLARATIONS,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 307, .attribute_rule_count = 3},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("try"), .kind = SXML_XSLT_AST_KIND_TRY,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR_THEN_CHILDREN,
     .child_kinds = {SXML_XSLT_AST_KIND_CATCH, SXML_XSLT_AST_KIND_FALLBACK},
     .child_kind_count = 2, .required_child_kind = SXML_XSLT_AST_KIND_CATCH,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 310, .attribute_rule_count = 2},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("use-package"), .kind = SXML_XSLT_AST_KIND_USE_PACKAGE,
     .element_flags = SXML_ELEMENT_FLAG_DECLARATION | SXML_ELEMENT_FLAG_STRIPS_WHITESPACE,
     .content_model = SXML_CONTENT_MODEL_CHILDREN,
     .child_kinds = {SXML_XSLT_AST_KIND_PACKAGE_LOCATION, SXML_XSLT_AST_KIND_ACCEPT, SXML_XSLT_AST_KIND_OVERRIDE, SXML_XSLT_AST_KIND_WITH_PARAM},
     .child_kind_count = 4, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 312, .attribute_rule_count = 2},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("value-of"), .kind = SXML_XSLT_AST_KIND_VALUE_OF,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 314, .attribute_rule_count = 4},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("variable"), .kind = SXML_XSLT_AST_KIND_VARIABLE,
     .element_flags = SXML_ELEMENT_FLAG_DECLARATION | SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 318, .attribute_rule_count = 5},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("when"), .kind = SXML_XSLT_AST_KIND_WHEN,
     .element_flags = 0,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 323, .attribute_rule_count = 2},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("where-populated"), .kind = SXML_XSLT_AST_KIND_WHERE_POPULATED,
     .element_flags = SXML_ELEMENT_FLAG_INSTRUCTION,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 325, .attribute_rule_count = 0},
    {.local_name = SXML_BYTE_VIEW_INITIALIZER("with-param"), .kind = SXML_XSLT_AST_KIND_WITH_PARAM,
     .element_flags = 0,
     .content_model = SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR,
     .child_kinds = {SXML_XSLT_AST_KIND_NONE},
     .child_kind_count = 0, .required_child_kind = SXML_XSLT_AST_KIND_NONE,
     .is_ordered = false, .single_child_mask = 0U,
     .first_attribute_rule = 325, .attribute_rule_count = 4},
};

static const keyword_spelling keyword_spellings[] = {
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("*"), .keyword = SXML_XSLT_AST_KEYWORD_ANY_COMPONENT},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("0"), .keyword = SXML_XSLT_AST_KEYWORD_NO},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("1"), .keyword = SXML_XSLT_AST_KEYWORD_YES},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("NFC"), .keyword = SXML_XSLT_AST_KEYWORD_NFC},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("NFD"), .keyword = SXML_XSLT_AST_KEYWORD_NFD},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("NFKC"), .keyword = SXML_XSLT_AST_KEYWORD_NFKC},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("NFKD"), .keyword = SXML_XSLT_AST_KEYWORD_NFKD},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("absent"), .keyword = SXML_XSLT_AST_KEYWORD_ABSENT},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("absorbing"), .keyword = SXML_XSLT_AST_KEYWORD_ABSORBING},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("abstract"), .keyword = SXML_XSLT_AST_KEYWORD_ABSTRACT},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("adaptive"), .keyword = SXML_XSLT_AST_KEYWORD_ADAPTIVE},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("any"), .keyword = SXML_XSLT_AST_KEYWORD_ANY},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("ascent"), .keyword = SXML_XSLT_AST_KEYWORD_ASCENT},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("attribute-set"), .keyword = SXML_XSLT_AST_KEYWORD_ATTRIBUTE_SET},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("csv"), .keyword = SXML_XSLT_AST_KEYWORD_CSV},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("deep-copy"), .keyword = SXML_XSLT_AST_KEYWORD_DEEP_COPY},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("deep-descent"), .keyword = SXML_XSLT_AST_KEYWORD_DEEP_DESCENT},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("deep-skip"), .keyword = SXML_XSLT_AST_KEYWORD_DEEP_SKIP},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("end"), .keyword = SXML_XSLT_AST_KEYWORD_END},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("fail"), .keyword = SXML_XSLT_AST_KEYWORD_FAIL},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("false"), .keyword = SXML_XSLT_AST_KEYWORD_NO},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("filter"), .keyword = SXML_XSLT_AST_KEYWORD_FILTER},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("final"), .keyword = SXML_XSLT_AST_KEYWORD_FINAL},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("fully-normalized"), .keyword = SXML_XSLT_AST_KEYWORD_FULLY_NORMALIZED},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("function"), .keyword = SXML_XSLT_AST_KEYWORD_FUNCTION},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("hidden"), .keyword = SXML_XSLT_AST_KEYWORD_HIDDEN},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("html"), .keyword = SXML_XSLT_AST_KEYWORD_HTML},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("inspection"), .keyword = SXML_XSLT_AST_KEYWORD_INSPECTION},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("item-type"), .keyword = SXML_XSLT_AST_KEYWORD_ITEM_TYPE},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("json"), .keyword = SXML_XSLT_AST_KEYWORD_JSON},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("lax"), .keyword = SXML_XSLT_AST_KEYWORD_LAX},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("maybe"), .keyword = SXML_XSLT_AST_KEYWORD_MAYBE},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("mode"), .keyword = SXML_XSLT_AST_KEYWORD_MODE},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("multiple"), .keyword = SXML_XSLT_AST_KEYWORD_MULTIPLE},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("no"), .keyword = SXML_XSLT_AST_KEYWORD_NO},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("none"), .keyword = SXML_XSLT_AST_KEYWORD_NO_NORMALIZATION},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("omit"), .keyword = SXML_XSLT_AST_KEYWORD_OMIT},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("optional"), .keyword = SXML_XSLT_AST_KEYWORD_OPTIONAL},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("preserve"), .keyword = SXML_XSLT_AST_KEYWORD_PRESERVE},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("private"), .keyword = SXML_XSLT_AST_KEYWORD_PRIVATE},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("public"), .keyword = SXML_XSLT_AST_KEYWORD_PUBLIC},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("record-type"), .keyword = SXML_XSLT_AST_KEYWORD_RECORD_TYPE},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("required"), .keyword = SXML_XSLT_AST_KEYWORD_REQUIRED},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("shallow-copy"), .keyword = SXML_XSLT_AST_KEYWORD_SHALLOW_COPY},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("shallow-copy-all"), .keyword = SXML_XSLT_AST_KEYWORD_SHALLOW_COPY_ALL},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("shallow-descent"), .keyword = SXML_XSLT_AST_KEYWORD_SHALLOW_DESCENT},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("shallow-skip"), .keyword = SXML_XSLT_AST_KEYWORD_SHALLOW_SKIP},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("single"), .keyword = SXML_XSLT_AST_KEYWORD_SINGLE},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("start"), .keyword = SXML_XSLT_AST_KEYWORD_START},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("strict"), .keyword = SXML_XSLT_AST_KEYWORD_STRICT},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("strip"), .keyword = SXML_XSLT_AST_KEYWORD_STRIP},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("template"), .keyword = SXML_XSLT_AST_KEYWORD_TEMPLATE},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("text"), .keyword = SXML_XSLT_AST_KEYWORD_TEXT},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("text-only-copy"), .keyword = SXML_XSLT_AST_KEYWORD_TEXT_ONLY_COPY},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("true"), .keyword = SXML_XSLT_AST_KEYWORD_YES},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("unclassified"), .keyword = SXML_XSLT_AST_KEYWORD_UNCLASSIFIED},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("unspecified"), .keyword = SXML_XSLT_AST_KEYWORD_UNSPECIFIED},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("use-last"), .keyword = SXML_XSLT_AST_KEYWORD_USE_LAST},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("variable"), .keyword = SXML_XSLT_AST_KEYWORD_VARIABLE},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("xhtml"), .keyword = SXML_XSLT_AST_KEYWORD_XHTML},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("xml"), .keyword = SXML_XSLT_AST_KEYWORD_XML},
    {.spelling = SXML_BYTE_VIEW_INITIALIZER("yes"), .keyword = SXML_XSLT_AST_KEYWORD_YES},
};

/* The standard attributes (3.3) and fixed-namespaces (3.6.1, module roots
   only): unprefixed on XSLT elements, in the XSLT namespace elsewhere. */
typedef struct standard_attribute {
    sxml_byte_view local_name;
    sxml_xslt_ast_attribute_kind attribute_kind;
} standard_attribute;

static const standard_attribute standard_attributes[] = {
    {SXML_BYTE_VIEW_INITIALIZER("default-collation"), SXML_XSLT_AST_ATTRIBUTE_DEFAULT_COLLATION},
    {SXML_BYTE_VIEW_INITIALIZER("default-mode"), SXML_XSLT_AST_ATTRIBUTE_DEFAULT_MODE},
    {SXML_BYTE_VIEW_INITIALIZER("default-validation"), SXML_XSLT_AST_ATTRIBUTE_DEFAULT_VALIDATION},
    {SXML_BYTE_VIEW_INITIALIZER("exclude-result-prefixes"),
     SXML_XSLT_AST_ATTRIBUTE_EXCLUDE_RESULT_PREFIXES},
    {SXML_BYTE_VIEW_INITIALIZER("expand-text"), SXML_XSLT_AST_ATTRIBUTE_EXPAND_TEXT},
    {SXML_BYTE_VIEW_INITIALIZER("extension-element-prefixes"),
     SXML_XSLT_AST_ATTRIBUTE_EXTENSION_ELEMENT_PREFIXES},
    {SXML_BYTE_VIEW_INITIALIZER("fixed-namespaces"), SXML_XSLT_AST_ATTRIBUTE_FIXED_NAMESPACES},
    {SXML_BYTE_VIEW_INITIALIZER("schema-role"), SXML_XSLT_AST_ATTRIBUTE_SCHEMA_ROLE},
    {SXML_BYTE_VIEW_INITIALIZER("use-when"), SXML_XSLT_AST_ATTRIBUTE_USE_WHEN},
    {SXML_BYTE_VIEW_INITIALIZER("version"), SXML_XSLT_AST_ATTRIBUTE_VERSION},
    {SXML_BYTE_VIEW_INITIALIZER("xpath-default-namespace"),
     SXML_XSLT_AST_ATTRIBUTE_XPATH_DEFAULT_NAMESPACE},
};

/* The attributes in the XSLT namespace a literal result element may have
   besides the standard ones (11.1.2, 10.2, 25.4). */
static const attribute_rule literal_result_attribute_rules[] = {
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("use-attribute-sets"),
     .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_USE_ATTRIBUTE_SETS,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAMES,
     .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("type"),
     .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_TYPE,
     .value_syntax = SXML_VALUE_SYNTAX_EQNAME,
     .is_required = false,
     .keyword_mask = 0},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("validation"),
     .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_VALIDATION,
     .value_syntax = SXML_VALUE_SYNTAX_KEYWORD,
     .is_required = false,
     .keyword_mask = KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRICT) |
                     KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_LAX) |
                     KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_PRESERVE) |
                     KEYWORD_BIT(SXML_XSLT_AST_KEYWORD_STRIP)},
    {.attribute_name = SXML_BYTE_VIEW_INITIALIZER("inherit-namespaces"),
     .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_INHERIT_NAMESPACES,
     .value_syntax = SXML_VALUE_SYNTAX_BOOLEAN,
     .is_required = false,
     .keyword_mask = 0},
};

/* The rule of the XSLT element local_name, by binary search over the
   sorted table; NULL when there is none. */
static const element_rule *find_element_rule(sxml_byte_view local_name) {
    size_t low = 0;
    size_t high = sizeof(element_rules) / sizeof(element_rules[0]);
    while (low < high) {
        const size_t middle = low + (high - low) / 2U;
        const sxml_byte_view candidate = element_rules[middle].local_name;
        const size_t common = candidate.byte_count < local_name.byte_count
                                  ? candidate.byte_count
                                  : local_name.byte_count;
        int order = common == 0 ? 0 : memcmp(candidate.bytes, local_name.bytes, common);
        if (order == 0) {
            order = candidate.byte_count < local_name.byte_count   ? -1
                    : candidate.byte_count > local_name.byte_count ? 1
                                                                   : 0;
        }
        if (order == 0) {
            return &element_rules[middle];
        }
        if (order < 0) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return NULL;
}

/* EXSLT Common's exsl:document (D206): an extension instruction sxml
   reads as xsl:result-document, whose attributes it shares (XSLT 4.0,
   26.2), when its namespace is an extension namespace (24.2). */
static bool is_exslt_document(sxml_namespace_uri_identifier namespace_uri,
                              sxml_byte_view local_name) {
    return namespace_uri.index == SXML_NAMESPACE_EXSLT_COMMON &&
           sxml_byte_view_equals(local_name, SXML_BYTE_VIEW_FROM_LITERAL("document"));
}

bool sxml_xslt_is_element_available(sxml_xslt_element_query query) {
    if (is_exslt_document(query.namespace_uri, query.local_name)) {
        return true;
    }
    /* xsl:note is no element of the AST but is implemented: it is
       ignored, as 3.11.2 says. */
    return query.namespace_uri.index == SXML_NAMESPACE_XSLT &&
           (find_element_rule(query.local_name) != NULL ||
            sxml_byte_view_equals(query.local_name, SXML_BYTE_VIEW_FROM_LITERAL("note")));
}

static const attribute_rule *find_attribute_rule(const element_rule *rule,
                                                 sxml_byte_view local_name) {
    for (uint32_t offset = 0; offset < rule->attribute_rule_count; offset += 1) {
        const attribute_rule *candidate = &attribute_rules[rule->first_attribute_rule + offset];
        if (sxml_byte_view_equals(candidate->attribute_name, local_name)) {
            return candidate;
        }
    }
    return NULL;
}

static const standard_attribute *find_standard_attribute(sxml_byte_view local_name) {
    for (size_t index = 0; index < sizeof(standard_attributes) / sizeof(standard_attributes[0]);
         index += 1) {
        if (sxml_byte_view_equals(standard_attributes[index].local_name, local_name)) {
            return &standard_attributes[index];
        }
    }
    return NULL;
}

static bool rule_lists_child(const element_rule *rule, sxml_xslt_ast_kind kind) {
    for (uint32_t index = 0; index < rule->child_kind_count; index += 1) {
        if (rule->child_kinds[index] == kind) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------ Text helpers */

/* The number of UTF-8 characters in text (one per non-continuation
   byte). */
static size_t count_characters(sxml_byte_view text) {
    size_t count = 0;
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        if ((text.bytes[byte_index] & 0xC0U) != 0x80U) {
            count += 1;
        }
    }
    return count;
}

/* An xs:decimal (XSD 1.1 Part 2, 3.3.3): optional sign, digits, an
   optional "." with digits; at least one digit. */
static bool read_decimal(sxml_byte_view text, double *number) {
    text = sxml_xml_text_trim_whitespace(text);
    size_t index = 0;
    bool is_negative = false;
    if (index < text.byte_count && (text.bytes[index] == '+' || text.bytes[index] == '-')) {
        is_negative = text.bytes[index] == '-';
        index += 1;
    }
    double value = 0;
    size_t digit_count = 0;
    while (index < text.byte_count && text.bytes[index] >= '0' && text.bytes[index] <= '9') {
        value = value * 10.0 + (double)(text.bytes[index] - '0');
        index += 1;
        digit_count += 1;
    }
    if (index < text.byte_count && text.bytes[index] == '.') {
        index += 1;
        double scale = 0.1;
        while (index < text.byte_count && text.bytes[index] >= '0' && text.bytes[index] <= '9') {
            value += scale * (double)(text.bytes[index] - '0');
            scale /= 10.0;
            index += 1;
            digit_count += 1;
        }
    }
    if (digit_count == 0 || index != text.byte_count) {
        return false;
    }
    *number = is_negative ? -value : value;
    return true;
}

/* ------------------------------------------------------------ Reader */

typedef enum child_context {
    /* A module's document node: its outermost element is the module root
       or, in a simplified module, a literal result element. */
    SXML_CHILD_CONTEXT_DOCUMENT,
    /* Children of a module root: declarations. */
    SXML_CHILD_CONTEXT_DECLARATIONS,
    /* A sequence constructor: instructions, literal result elements, text. */
    SXML_CHILD_CONTEXT_SEQUENCE_CONSTRUCTOR,
    /* The children the element's rule lists, with or without a sequence
       constructor (its content model). */
    SXML_CHILD_CONTEXT_RULE,
    /* An extension or unknown instruction: only xsl:fallback is read. */
    SXML_CHILD_CONTEXT_FALLBACK_ONLY,
    /* Children that are not read: a module root whose use-when is false,
       an inline schema. */
    SXML_CHILD_CONTEXT_SKIPPED,
} child_context;

/* One open element of the walk. */
typedef struct open_element {
    sxml_xslt_ast_module_index module;
    sxml_tree *tree;
    sxml_node_index element;
    bool has_next_child;
    sxml_node_index next_child;
    /* The AST node the children are added to. */
    sxml_xslt_ast_node_index node;
    /* The element's rule; NULL for elements outside the XSLT namespace. */
    const element_rule *rule;
    child_context child_context;
    sxml_xslt_ast_static_context_index static_context;
    /* xml:space="preserve" is in effect (3.11.1). */
    bool preserves_space;
    /* The effective version is above 4.0 (3.9). */
    bool is_forwards_compatible;
    /* A child of the sequence constructor has been read: listed children
       may no longer follow (CHILDREN_THEN_SEQUENCE_CONSTRUCTOR), or have
       begun (SEQUENCE_CONSTRUCTOR_THEN_CHILDREN). */
    bool has_sequence_constructor_child;
    bool has_listed_child_after_sequence_constructor;
    bool has_required_child;
    /* A static variable or parameter: its content must be empty once
       use-when has been applied (9.6). */
    bool requires_no_children;
    /* Listed children seen so far (bits by position in the rule's
       child_kinds) and the position of the last one. */
    uint32_t seen_child_mask;
    uint32_t last_child_position;
    /* The kind of the child element read last, for 3.11.1's rule on
       whitespace after xsl:catch. */
    sxml_xslt_ast_kind previous_child_kind;
} open_element;

/* An attribute of the element being read, after shadow attributes. */
typedef struct element_attribute {
    sxml_name_identifier attribute_name;
    sxml_namespace_uri_identifier namespace_uri;
    sxml_namespace_prefix_identifier namespace_prefix;
    sxml_byte_view local_name;
    sxml_byte_view value;
    sxml_ast_source_span source_span;
    /* The value came from a shadow attribute (3.11.4). */
    bool is_shadow;
    /* A shadow attribute replaced it, or it is a shadow attribute itself. */
    bool is_ignored;
} element_attribute;

/* What one module needs while it is read: its fixed namespace bindings
   (3.6.1), a run of the reader's fixed_bindings. */
typedef struct module_state {
    bool has_fixed_namespaces;
    size_t first_fixed_binding;
    size_t fixed_binding_count;
} module_state;

/* A growable array of the reader; the element type is the user's. */
typedef struct scratch_array {
    void *elements;
    size_t count;
    size_t capacity;
} scratch_array;

typedef struct stylesheet_reader {
    sxml_processor *processor;
    sxml_allocator allocator;
    sxml_name_pool *name_pool;
    sxml_xslt_ast *ast;
    sxml_xslt_static_scope *static_scope;
    sxml_xml_parser *parser;
    sxml_resource_resolver resource_resolver;
    sxml_error_record *error_record;
    sxml_variable_binding_view static_parameters;
    sxml_byte_view default_collation_uri;

    /* open_element: the walk's stack. */
    scratch_array frames;
    /* uint8_t: text being collected between two elements. */
    scratch_array text;
    /* element_attribute: the attributes of the element being read. */
    scratch_array attributes;
    /* sxml_sequence, owned: values of that element's shadow attributes. */
    scratch_array shadow_values;
    /* sxml_namespace_binding, sxml_namespace_uri_identifier: scratch for a
       static context. */
    scratch_array bindings;
    /* sxml_namespace_binding: the namespaces of a static expression. */
    scratch_array evaluation_bindings;
    /* uint8_t: the default collation, then the static base URI. */
    scratch_array context_text;
    scratch_array excluded_uris;
    scratch_array extension_uris;
    /* sxml_xslt_ast_name_token: scratch for a name list. */
    scratch_array name_tokens;
    /* sxml_namespace_binding: the fixed namespace bindings of every
       module; module_state per module index. */
    scratch_array fixed_bindings;
    scratch_array module_states;

    /* The first text node of the text being collected, and where it is. */
    sxml_node_index text_node;
    sxml_ast_source_span text_span;
} stylesheet_reader;

/* The size and alignment of a scratch array's elements, and the count
   it must hold. */
typedef struct scratch_reservation {
    size_t element_size;
    size_t element_alignment;
    size_t needed_count;
} scratch_reservation;

static sxml_status reserve_scratch(const stylesheet_reader *reader, scratch_array *array,
                                   scratch_reservation reservation) {
    if (reservation.needed_count <= array->capacity) {
        return SXML_STATUS_OK;
    }
    size_t new_capacity = array->capacity < 16 ? 16 : array->capacity;
    while (new_capacity < reservation.needed_count) {
        if (new_capacity > SIZE_MAX / 2U) {
            return SXML_STATUS_LIMIT_EXCEEDED;
        }
        new_capacity *= 2U;
    }
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(new_capacity, reservation.element_size, &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            reader->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = reservation.element_alignment},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (array->count > 0) {
        memcpy(memory, array->elements, array->count * reservation.element_size);
    }
    sxml_allocator_release(reader->allocator, array->elements);
    array->elements = memory;
    array->capacity = new_capacity;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Errors */

/* A static error the reader raises, at a place in a module. */
typedef struct reader_error {
    /* Such as "XTSE0010"; static text. */
    const char *error_code;
    /* Static text. */
    const char *description;
    sxml_xslt_ast_module_index module;
    sxml_ast_source_span source_span;
} reader_error;

static sxml_source_location module_location(const stylesheet_reader *reader,
                                            sxml_xslt_ast_module_index module,
                                            sxml_ast_source_span span) {
    return (sxml_source_location){
        .source_name = sxml_xslt_ast_get_module(reader->ast, module)->module_uri,
        .line_number = span.line_number,
        .column_number = span.column_number,
        .byte_offset = span.byte_offset};
}

static sxml_status raise_error(stylesheet_reader *reader, reader_error error) {
    if (reader->error_record != NULL) {
        *reader->error_record = (sxml_error_record){
            .code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(SXML_ERROR_NAMESPACE_URI),
                     .local_name = sxml_byte_view_from_c_string(error.error_code),
                     .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err")},
            .value = NULL,
            .diagnostic = {.description = sxml_byte_view_from_c_string(error.description),
                           .location = module_location(reader, error.module, error.source_span)}};
    }
    return SXML_STATUS_STATIC_ERROR;
}

/* An error a front end or a static expression reported with a location
   inside its text is moved to the attribute or text it came from: the
   line and column of the expression in the module, plus its own offset
   on the first line. */
typedef struct error_relocation {
    sxml_status status;
    sxml_xslt_ast_module_index module;
    sxml_ast_source_span source_span;
} error_relocation;

static sxml_status relocate_error(stylesheet_reader *reader, error_relocation relocation) {
    if (reader->error_record != NULL && sxml_status_has_error_record(relocation.status)) {
        sxml_source_location *location = &reader->error_record->diagnostic.location;
        sxml_ast_source_span span = relocation.source_span;
        if (location->line_number != 0 && location->column_number != 0) {
            span = sxml_ast_moved_source_span(
                (sxml_ast_source_span){.byte_offset = location->byte_offset > UINT32_MAX
                                                          ? UINT32_MAX
                                                          : (uint32_t)location->byte_offset,
                                       .byte_count = 0,
                                       .line_number = location->line_number,
                                       .column_number = location->column_number},
                relocation.source_span);
        }
        *location = module_location(reader, relocation.module, span);
    }
    return relocation.status;
}

/* ------------------------------------------------------------ Tree access */

static sxml_node_reference tree_node(sxml_tree *tree, sxml_node_index node) {
    return (sxml_node_reference){.tree = tree, .node_index = node, .namespace_number = 0};
}

static sxml_ast_source_span location_of(sxml_tree *tree, sxml_node_index node) {
    sxml_source_location location = {.source_name = {.bytes = NULL, .byte_count = 0},
                                     .line_number = 0,
                                     .column_number = 0,
                                     .byte_offset = 0};
    if (tree->operations->get_location == NULL ||
        tree->operations->get_location(tree->state, node, &location) != SXML_STATUS_OK) {
        location.line_number = 0;
    }
    return (sxml_ast_source_span){
        .byte_offset = location.byte_offset > UINT32_MAX ? UINT32_MAX : (uint32_t)location.byte_offset,
        .byte_count = 0,
        .line_number = location.line_number,
        .column_number = location.column_number};
}

static sxml_status describe(sxml_tree *tree, sxml_node_index node,
                            sxml_node_description *description) {
    memset(description, 0, sizeof(*description));
    return sxml_node_describe(tree_node(tree, node), description);
}

/* ------------------------------------------------------------ Frames */

static open_element *frame_at(const stylesheet_reader *reader, size_t frame_index) {
    open_element *frames = reader->frames.elements;
    return &frames[frame_index];
}

static sxml_status push_frame(stylesheet_reader *reader, const open_element *frame) {
    const sxml_status status = reserve_scratch(
        reader, &reader->frames,
        (scratch_reservation){.element_size = sizeof(open_element),
                              .element_alignment = alignof(open_element),
                              .needed_count = reader->frames.count + 1U});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    open_element *frames = reader->frames.elements;
    frames[reader->frames.count] = *frame;
    reader->frames.count += 1;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Modules */

static module_state *state_of_module(const stylesheet_reader *reader, sxml_xslt_ast_module_index module) {
    module_state *states = reader->module_states.elements;
    return &states[module.value];
}

/* The level module of module: itself, or its includer's (3.10.2). */
static uint32_t level_of_module(const stylesheet_reader *reader, uint32_t module) {
    uint32_t current = module;
    while (sxml_xslt_ast_get_module(reader->ast, (sxml_xslt_ast_module_index){.value = current})
               ->relation == SXML_XSLT_AST_MODULE_RELATION_INCLUDED) {
        current = sxml_xslt_ast_get_module(reader->ast,
                                           (sxml_xslt_ast_module_index){.value = current})
                      ->parent_module.value;
    }
    return current;
}

/* Parses a module's bytes into a document node (D81) with source
   locations, the document URI and base URI being uri. */
typedef struct module_text {
    sxml_byte_view content;
    sxml_byte_view encoding_name;
    sxml_byte_view module_uri;
} module_text;

static sxml_status parse_module(stylesheet_reader *reader, module_text text, sxml_item *document) {
    sxml_processor *processor = reader->processor;
    sxml_arena_tree *arena_tree = NULL;
    sxml_status status = sxml_arena_tree_create(
        reader->allocator,
        &(sxml_arena_tree_configuration){
            .allocator = reader->allocator,
            .initial_memory = {.bytes = NULL, .byte_count = 0},
            .item_heap = processor->item_heap,
            .maximum_node_count = processor->limits.maximum_tree_node_count,
            .maximum_depth = processor->limits.xml_limits.maximum_depth,
            .maximum_text_byte_count = processor->limits.maximum_tree_text_byte_count,
            .keeps_locations = true,
            .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
            .is_destroyed_when_unreferenced = true,
            .error_record = NULL,
            .base_uri = {.bytes = NULL, .byte_count = 0}},
        &arena_tree);
    sxml_diagnostic diagnostic;
    memset(&diagnostic, 0, sizeof(diagnostic));
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_parser_parse(
            reader->parser,
            &(sxml_xml_parse_request){.input = text.content,
                                      .source_name = text.module_uri,
                                      .document_uri = text.module_uri,
                                      .base_uri = text.module_uri,
                                      .encoding_name = text.encoding_name,
                                      .diagnostic = &diagnostic,
                                      /* A module's external DTD and entities
                                         come through the resolver (D199). */
                                      .entity_resolver =
                                          sxml_processor_entity_resolver(processor)},
            sxml_arena_tree_as_tree_builder(arena_tree));
    }
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK) {
        if (reader->error_record != NULL && diagnostic.description.byte_count > 0) {
            /* include/sxml/xslt.h: a module that is not well-formed has
               the diagnostic of the XML parser (packet 4.15). */
            memset(reader->error_record, 0, sizeof(*reader->error_record));
            reader->error_record->diagnostic = diagnostic;
        }
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *document = sxml_node_item_make(tree_node(tree, (sxml_node_index){.index = 0}));
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

/* A module to start reading: it is added to the AST and its document
   frame pushed, so the walk reads its outermost element next. */
typedef struct module_start {
    sxml_xslt_ast_module_relation relation;
    sxml_xslt_ast_module_index parent_module;
    sxml_xslt_ast_node_index referencing_node;
    /* Owned by the caller; the AST retains it. */
    sxml_item document;
    sxml_byte_view module_uri;
} module_start;

static sxml_status start_module(stylesheet_reader *reader, const module_start *start) {
    sxml_xslt_ast_module_index module = {.value = 0};
    sxml_status status = sxml_xslt_ast_add_module(
        reader->ast,
        &(sxml_xslt_ast_module_request){.relation = start->relation,
                                        .parent_module = start->parent_module,
                                        .referencing_node = start->referencing_node,
                                        .document = start->document,
                                        .module_uri = start->module_uri},
        &module);
    if (status == SXML_STATUS_OK) {
        status = reserve_scratch(reader, &reader->module_states,
                                 (scratch_reservation){.element_size = sizeof(module_state),
                                                       .element_alignment = alignof(module_state),
                                                       .needed_count = module.value + 1U});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    reader->module_states.count = module.value + 1U;
    *state_of_module(reader, module) =
        (module_state){.has_fixed_namespaces = false, .first_fixed_binding = 0,
                       .fixed_binding_count = 0};
    const sxml_xslt_ast_module *entry = sxml_xslt_ast_get_module(reader->ast, module);
    return push_frame(reader,
                      &(open_element){.module = module,
                                      .tree = entry->tree,
                                      .element = {.index = 0},
                                      .has_next_child = entry->outermost_element.index != 0,
                                      .next_child = entry->outermost_element,
                                      .node = {.value = 0},
                                      .rule = NULL,
                                      .child_context = SXML_CHILD_CONTEXT_DOCUMENT,
                                      .static_context = {.value = 0},
                                      .preserves_space = false,
                                      .is_forwards_compatible = false,
                                      .has_sequence_constructor_child = false,
                                      .has_listed_child_after_sequence_constructor = false,
                                      .has_required_child = false,
                                      .requires_no_children = false,
                                      .seen_child_mask = 0,
                                      .last_child_position = 0,
                                      .previous_child_kind = SXML_XSLT_AST_KIND_NONE});
}

/* ------------------------------------------------------------ Attributes */

static element_attribute *attribute_at(const stylesheet_reader *reader, size_t attribute_index) {
    element_attribute *attributes = reader->attributes.elements;
    return &attributes[attribute_index];
}

static void release_shadow_values(stylesheet_reader *reader) {
    sxml_sequence *values = reader->shadow_values.elements;
    for (size_t value_index = 0; value_index < reader->shadow_values.count; value_index += 1) {
        sxml_sequence_release(values[value_index]);
    }
    reader->shadow_values.count = 0;
}

static sxml_status append_attribute(stylesheet_reader *reader, const element_attribute *attribute) {
    const sxml_status status = reserve_scratch(
        reader, &reader->attributes,
        (scratch_reservation){.element_size = sizeof(element_attribute),
                              .element_alignment = alignof(element_attribute),
                              .needed_count = reader->attributes.count + 1U});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *attribute_at(reader, reader->attributes.count) = *attribute;
    reader->attributes.count += 1;
    return SXML_STATUS_OK;
}

/* The element whose attributes to collect. */
typedef struct element_source {
    sxml_tree *tree;
    sxml_node_index element;
    uint32_t attribute_count;
} element_source;

static sxml_status collect_attributes(stylesheet_reader *reader, element_source source) {
    reader->attributes.count = 0;
    release_shadow_values(reader);
    for (uint32_t offset = 1; offset <= source.attribute_count; offset += 1) {
        const sxml_node_index attribute = {.index = source.element.index + offset};
        sxml_node_description description;
        sxml_status status = describe(source.tree, attribute, &description);
        sxml_name_parts parts = {.namespace_uri = {.index = 0},
                                 .local_name = {.bytes = NULL, .byte_count = 0}};
        sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_name_parts(reader->name_pool, description.node_name, &parts);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_node_get_content(tree_node(source.tree, attribute), &value);
        }
        if (status == SXML_STATUS_OK) {
            status = append_attribute(
                reader, &(element_attribute){.attribute_name = description.node_name,
                                             .namespace_uri = parts.namespace_uri,
                                             .namespace_prefix = description.namespace_prefix,
                                             .local_name = parts.local_name,
                                             .value = value,
                                             .source_span = location_of(source.tree, attribute),
                                             .is_shadow = false,
                                             .is_ignored = false});
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/* The value of the attribute in namespace_uri with local_name among the
   element's attributes (shadows applied); NULL when there is none. */
typedef struct attribute_query {
    sxml_namespace_uri_identifier namespace_uri;
    sxml_byte_view local_name;
} attribute_query;

static const element_attribute *find_element_attribute(const stylesheet_reader *reader,
                                                       attribute_query query) {
    for (size_t attribute_index = 0; attribute_index < reader->attributes.count;
         attribute_index += 1) {
        const element_attribute *attribute = attribute_at(reader, attribute_index);
        if (!attribute->is_ignored && attribute->namespace_uri.index == query.namespace_uri.index &&
            sxml_byte_view_equals(attribute->local_name, query.local_name)) {
            return attribute;
        }
    }
    return NULL;
}

/* A standard attribute: unprefixed on an XSLT element, in the XSLT
   namespace on any other (3.3). */
typedef struct standard_query {
    bool is_xslt_element;
    const char *local_name;
} standard_query;

static const element_attribute *find_standard_value(const stylesheet_reader *reader, standard_query query) {
    return find_element_attribute(
        reader, (attribute_query){.namespace_uri = {.index = query.is_xslt_element
                                                                 ? SXML_NAMESPACE_NONE
                                                                 : SXML_NAMESPACE_XSLT},
                                  .local_name = sxml_byte_view_from_c_string(query.local_name)});
}

/* ------------------------------------------------------- Static contexts */

static sxml_namespace_binding *binding_at(const scratch_array *array, size_t index) {
    sxml_namespace_binding *bindings = array->elements;
    return &bindings[index];
}

/* A binding to add to a run of bindings that starts at first_index; a
   later binding of a prefix in the run replaces the earlier (3.6.1). */
typedef struct binding_addition {
    sxml_namespace_binding binding;
    size_t first_index;
} binding_addition;

static sxml_status append_binding(stylesheet_reader *reader, scratch_array *array,
                                  binding_addition addition) {
    for (size_t index = addition.first_index; index < array->count; index += 1) {
        if (binding_at(array, index)->namespace_prefix.index ==
            addition.binding.namespace_prefix.index) {
            *binding_at(array, index) = addition.binding;
            return SXML_STATUS_OK;
        }
    }
    const sxml_status status = reserve_scratch(
        reader, array,
        (scratch_reservation){.element_size = sizeof(sxml_namespace_binding),
                              .element_alignment = alignof(sxml_namespace_binding),
                              .needed_count = array->count + 1U});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *binding_at(array, array->count) = addition.binding;
    array->count += 1;
    return SXML_STATUS_OK;
}

static sxml_status append_uri(stylesheet_reader *reader, scratch_array *array,
                              sxml_namespace_uri_identifier uri) {
    sxml_namespace_uri_identifier *uris = array->elements;
    for (size_t index = 0; index < array->count; index += 1) {
        if (uris[index].index == uri.index) {
            return SXML_STATUS_OK;
        }
    }
    const sxml_status status = reserve_scratch(
        reader, array,
        (scratch_reservation){.element_size = sizeof(sxml_namespace_uri_identifier),
                              .element_alignment = alignof(sxml_namespace_uri_identifier),
                              .needed_count = array->count + 1U});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uris = array->elements;
    uris[array->count] = uri;
    array->count += 1;
    return SXML_STATUS_OK;
}

static sxml_status append_text_bytes(stylesheet_reader *reader, scratch_array *array, sxml_byte_view bytes) {
    if (bytes.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_status status =
        reserve_scratch(reader, array,
                        (scratch_reservation){.element_size = 1,
                                              .element_alignment = 1,
                                              .needed_count = array->count + bytes.byte_count});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *text = array->elements;
    memcpy(text + array->count, bytes.bytes, bytes.byte_count);
    array->count += bytes.byte_count;
    return SXML_STATUS_OK;
}

/* The native namespace bindings of an element (its in-scope namespaces
   in the tree); the xml prefix is left out. */
static sxml_status native_bindings(element_source source, sxml_namespace_binding_view *bindings) {
    return sxml_node_get_in_scope_namespaces(tree_node(source.tree, source.element), bindings);
}

/* The namespace URI a native prefix is bound to; prefix 0 is the default
   namespace (none when undeclared). False when a prefix is unbound. */
typedef struct native_prefix_query {
    element_source source;
    sxml_namespace_prefix_identifier namespace_prefix;
} native_prefix_query;

static bool find_native_prefix(native_prefix_query query, sxml_namespace_uri_identifier *uri) {
    sxml_namespace_binding_view bindings = {.namespace_bindings = NULL,
                                            .namespace_binding_count = 0};
    if (native_bindings(query.source, &bindings) != SXML_STATUS_OK) {
        return false;
    }
    for (size_t index = 0; index < bindings.namespace_binding_count; index += 1) {
        if (bindings.namespace_bindings[index].namespace_prefix.index ==
            query.namespace_prefix.index) {
            *uri = bindings.namespace_bindings[index].namespace_uri;
            return true;
        }
    }
    if (query.namespace_prefix.index == 0) {
        *uri = (sxml_namespace_uri_identifier){.index = SXML_NAMESPACE_NONE};
        return true;
    }
    return false;
}

/* The prefixes of a prefix list (exclude-result-prefixes,
   extension-element-prefixes, 11.1.3, 24.3): their URIs are added to
   uris; "#all" adds every native namespace when allowed. */
typedef struct prefix_list_request {
    sxml_byte_view text;
    element_source source;
    bool allows_all;
    scratch_array *uris;
    /* For an unbound prefix. */
    const char *error_code;
    sxml_xslt_ast_module_index module;
    sxml_ast_source_span source_span;
} prefix_list_request;

static sxml_status read_prefix_list(stylesheet_reader *reader, const prefix_list_request *request) {
    sxml_byte_view rest = request->text;
    sxml_byte_view token = {.bytes = NULL, .byte_count = 0};
    while (sxml_xml_text_next_token(&rest, &token)) {
        if (request->allows_all && sxml_byte_view_equals(token, SXML_BYTE_VIEW_FROM_LITERAL("#all"))) {
            sxml_namespace_binding_view bindings = {.namespace_bindings = NULL,
                                                    .namespace_binding_count = 0};
            sxml_status status = native_bindings(request->source, &bindings);
            for (size_t index = 0; status == SXML_STATUS_OK && index < bindings.namespace_binding_count;
                 index += 1) {
                if (bindings.namespace_bindings[index].namespace_uri.index != SXML_NAMESPACE_XML) {
                    status = append_uri(reader, request->uris,
                                        bindings.namespace_bindings[index].namespace_uri);
                }
            }
            if (status != SXML_STATUS_OK) {
                return status;
            }
            continue;
        }
        const bool is_default = sxml_byte_view_equals(token, SXML_BYTE_VIEW_FROM_LITERAL("#default"));
        sxml_namespace_prefix_identifier prefix = {.index = 0};
        if (!is_default) {
            if (!sxml_xml_text_is_ncname(token)) {
                return raise_error(reader, (reader_error){.error_code = request->error_code,
                                                          .description = "A prefix list holds a "
                                                                         "token that is no prefix "
                                                                         "(XSLT 4.0, 11.1.3, 24.3)",
                                                          .module = request->module,
                                                          .source_span = request->source_span});
            }
            const sxml_status status =
                sxml_name_pool_intern_namespace_prefix(reader->name_pool, token, &prefix);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        sxml_namespace_uri_identifier uri = {.index = SXML_NAMESPACE_NONE};
        if (is_default && request->allows_all &&
            (!find_native_prefix((native_prefix_query){.source = request->source,
                                                       .namespace_prefix = prefix},
                                 &uri) ||
             uri.index == SXML_NAMESPACE_NONE)) {
            return raise_error(reader, (reader_error){.error_code = "XTSE0809",
                                                      .description = "#default in "
                                                                     "exclude-result-prefixes "
                                                                     "with no default namespace "
                                                                     "(XSLT 4.0, 11.1.3)",
                                                      .module = request->module,
                                                      .source_span = request->source_span});
        }
        if (!find_native_prefix((native_prefix_query){.source = request->source,
                                                      .namespace_prefix = prefix},
                                &uri) ||
            (is_default && uri.index == SXML_NAMESPACE_NONE)) {
            return raise_error(reader, (reader_error){.error_code = request->error_code,
                                                      .description =
                                                          "A prefix in a prefix list has no "
                                                          "namespace declaration (XSLT 4.0, "
                                                          "11.1.3, 24.3)",
                                                      .module = request->module,
                                                      .source_span = request->source_span});
        }
        const sxml_status status = append_uri(reader, request->uris, uri);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/* The well-known prefixes #standard and the single names of 3.6.1. */
typedef struct standard_prefix {
    sxml_byte_view prefix;
    sxml_well_known_namespace namespace_uri;
} standard_prefix;

static const standard_prefix standard_prefixes[] = {
    {SXML_BYTE_VIEW_INITIALIZER("xsl"), SXML_NAMESPACE_XSLT},
    {SXML_BYTE_VIEW_INITIALIZER("xml"), SXML_NAMESPACE_XML},
    {SXML_BYTE_VIEW_INITIALIZER("xs"), SXML_NAMESPACE_XML_SCHEMA},
    {SXML_BYTE_VIEW_INITIALIZER("xsi"), SXML_NAMESPACE_XML_SCHEMA_INSTANCE},
    {SXML_BYTE_VIEW_INITIALIZER("fn"), SXML_NAMESPACE_FUNCTIONS},
    {SXML_BYTE_VIEW_INITIALIZER("math"), SXML_NAMESPACE_MATH_FUNCTIONS},
    {SXML_BYTE_VIEW_INITIALIZER("map"), SXML_NAMESPACE_MAP_FUNCTIONS},
    {SXML_BYTE_VIEW_INITIALIZER("array"), SXML_NAMESPACE_ARRAY_FUNCTIONS},
    {SXML_BYTE_VIEW_INITIALIZER("err"), SXML_NAMESPACE_ERRORS},
};

/* A fixed binding of the module whose run starts at first_index. */
typedef struct fixed_binding {
    sxml_byte_view prefix_text;
    sxml_namespace_uri_identifier namespace_uri;
    size_t first_index;
} fixed_binding;

static sxml_status add_fixed_binding(stylesheet_reader *reader, fixed_binding binding) {
    sxml_namespace_prefix_identifier prefix = {.index = 0};
    const sxml_status status =
        sxml_name_pool_intern_namespace_prefix(reader->name_pool, binding.prefix_text, &prefix);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return append_binding(
        reader, &reader->fixed_bindings,
        (binding_addition){.binding = {.namespace_prefix = prefix,
                                       .namespace_uri = binding.namespace_uri},
                           .first_index = binding.first_index});
}

/* The fixed namespace bindings of a module root (3.6.1), appended to the
   reader's fixed bindings as the module's run. */
typedef struct fixed_namespaces_request {
    const element_attribute *attribute;
    element_source source;
    sxml_xslt_ast_module_index module;
} fixed_namespaces_request;

static sxml_status read_fixed_namespaces(stylesheet_reader *reader, fixed_namespaces_request request) {
    /* Each module's run is at the end of the array; append_binding only
       replaces within it, since earlier runs are left behind. */
    const size_t first = reader->fixed_bindings.count;
    sxml_byte_view rest = request.attribute->value;
    sxml_byte_view token = {.bytes = NULL, .byte_count = 0};
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && sxml_xml_text_next_token(&rest, &token)) {
        if (sxml_byte_view_equals(token, SXML_BYTE_VIEW_FROM_LITERAL("#standard"))) {
            for (size_t index = 0; status == SXML_STATUS_OK &&
                                   index < sizeof(standard_prefixes) / sizeof(standard_prefixes[0]);
                 index += 1) {
                status = add_fixed_binding(
                    reader, (fixed_binding){.prefix_text = standard_prefixes[index].prefix,
                                            .namespace_uri = {.index = standard_prefixes[index]
                                                                           .namespace_uri},
                                            .first_index = first});
            }
            continue;
        }
        size_t equals_index = 0;
        while (equals_index < token.byte_count && token.bytes[equals_index] != '=') {
            equals_index += 1;
        }
        if (equals_index < token.byte_count) {
            const sxml_byte_view prefix = {.bytes = token.bytes, .byte_count = equals_index};
            const sxml_byte_view uri_text = {.bytes = token.bytes + equals_index + 1,
                                             .byte_count = token.byte_count - equals_index - 1};
            sxml_namespace_uri_identifier uri = {.index = 0};
            if (!sxml_xml_text_is_ncname(prefix) || uri_text.byte_count == 0 ||
                sxml_byte_view_equals(prefix, SXML_BYTE_VIEW_FROM_LITERAL("xmlns"))) {
                status = SXML_STATUS_STATIC_ERROR;
                break;
            }
            status = sxml_name_pool_intern_namespace_uri(reader->name_pool, uri_text, &uri);
            if (status == SXML_STATUS_OK) {
                status = add_fixed_binding(reader, (fixed_binding){.prefix_text = prefix,
                                                                   .namespace_uri = uri,
                                                                   .first_index = first});
            }
            continue;
        }
        if (sxml_xml_text_is_ncname(token)) {
            sxml_namespace_prefix_identifier prefix = {.index = 0};
            status = sxml_name_pool_intern_namespace_prefix(reader->name_pool, token, &prefix);
            sxml_namespace_uri_identifier uri = {.index = 0};
            if (status == SXML_STATUS_OK &&
                find_native_prefix((native_prefix_query){.source = request.source,
                                                         .namespace_prefix = prefix},
                                   &uri)) {
                status = add_fixed_binding(reader, (fixed_binding){.prefix_text = token,
                                                                   .namespace_uri = uri,
                                                                   .first_index = first});
                continue;
            }
            bool is_standard = false;
            for (size_t index = 0; status == SXML_STATUS_OK &&
                                   index < sizeof(standard_prefixes) / sizeof(standard_prefixes[0]);
                 index += 1) {
                if (sxml_byte_view_equals(standard_prefixes[index].prefix, token)) {
                    is_standard = true;
                    status = add_fixed_binding(
                        reader, (fixed_binding){.prefix_text = token,
                                                .namespace_uri = {.index = standard_prefixes[index]
                                                                               .namespace_uri},
                                                .first_index = first});
                }
            }
            if (status == SXML_STATUS_OK && !is_standard) {
                status = SXML_STATUS_STATIC_ERROR;
            }
            continue;
        }
        /* A URI of a document whose namespaces to take: not supported. */
        status = SXML_STATUS_STATIC_ERROR;
    }
    if (status == SXML_STATUS_STATIC_ERROR) {
        return raise_error(reader,
                           (reader_error){.error_code = "XTSE0122",
                                          .description = "A token of fixed-namespaces is none of "
                                                         "the permitted forms (XSLT 4.0, 3.6.1)",
                                          .module = request.module,
                                          .source_span = request.attribute->source_span});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *state_of_module(reader, request.module) =
        (module_state){.has_fixed_namespaces = true,
                       .first_fixed_binding = first,
                       .fixed_binding_count = reader->fixed_bindings.count - first};
    return SXML_STATUS_OK;
}


/* ------------------------------------------------------------ Names */

/* An EQName or lexical QName to resolve (XSLT 4.0, 5.1.1). */
typedef struct eqname_request {
    sxml_byte_view text;
    /* The statically known namespaces (the xml prefix is always bound). */
    sxml_namespace_binding_view bindings;
    /* The namespace of an unprefixed name: none, or a default namespace
       where the attribute's rules say so. */
    sxml_namespace_uri_identifier unprefixed_namespace;
} eqname_request;

typedef struct eqname_result {
    sxml_name_identifier name;
    /* The text is no EQName (XTSE0020). */
    bool is_malformed;
    /* Its prefix is not bound (XTSE0280). */
    bool is_unbound;
} eqname_result;

/* The namespace of prefix_text among bindings; false when unbound. */
typedef struct prefix_lookup {
    sxml_byte_view prefix_text;
    sxml_namespace_binding_view bindings;
} prefix_lookup;

static sxml_status lookup_prefix(stylesheet_reader *reader, prefix_lookup lookup,
                                 sxml_namespace_uri_identifier *namespace_uri) {
    if (sxml_byte_view_equals(lookup.prefix_text, SXML_BYTE_VIEW_FROM_LITERAL("xml"))) {
        *namespace_uri = (sxml_namespace_uri_identifier){.index = SXML_NAMESPACE_XML};
        return SXML_STATUS_OK;
    }
    sxml_namespace_prefix_identifier prefix = {.index = 0};
    const sxml_status status =
        sxml_name_pool_intern_namespace_prefix(reader->name_pool, lookup.prefix_text, &prefix);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    for (size_t index = 0; index < lookup.bindings.namespace_binding_count; index += 1) {
        if (lookup.bindings.namespace_bindings[index].namespace_prefix.index == prefix.index) {
            *namespace_uri = lookup.bindings.namespace_bindings[index].namespace_uri;
            return SXML_STATUS_OK;
        }
    }
    return SXML_STATUS_INDEX_OUT_OF_RANGE;
}

static sxml_status resolve_eqname(stylesheet_reader *reader, const eqname_request *request,
                                  eqname_result *result) {
    *result = (eqname_result){.name = {.index = 0}, .is_malformed = false, .is_unbound = false};
    const sxml_byte_view text = sxml_xml_text_trim_whitespace(request->text);
    sxml_namespace_uri_identifier namespace_uri = request->unprefixed_namespace;
    sxml_byte_view local_name = text;
    sxml_status status = SXML_STATUS_OK;
    if (text.byte_count > 2 && text.bytes[0] == 'Q' && text.bytes[1] == '{') {
        size_t close_index = 2;
        while (close_index < text.byte_count && text.bytes[close_index] != '}') {
            close_index += 1;
        }
        if (close_index >= text.byte_count) {
            result->is_malformed = true;
            return SXML_STATUS_OK;
        }
        status = sxml_name_pool_intern_namespace_uri(
            reader->name_pool,
            (sxml_byte_view){.bytes = text.bytes + 2, .byte_count = close_index - 2},
            &namespace_uri);
        local_name = (sxml_byte_view){.bytes = text.bytes + close_index + 1,
                                      .byte_count = text.byte_count - close_index - 1};
    } else {
        size_t colon_index = 0;
        while (colon_index < text.byte_count && text.bytes[colon_index] != ':') {
            colon_index += 1;
        }
        if (colon_index < text.byte_count) {
            const sxml_byte_view prefix = {.bytes = text.bytes, .byte_count = colon_index};
            local_name = (sxml_byte_view){.bytes = text.bytes + colon_index + 1,
                                          .byte_count = text.byte_count - colon_index - 1};
            if (!sxml_xml_text_is_ncname(prefix) || !sxml_xml_text_is_ncname(local_name)) {
                result->is_malformed = true;
                return SXML_STATUS_OK;
            }
            status = lookup_prefix(reader,
                                   (prefix_lookup){.prefix_text = prefix,
                                                   .bindings = request->bindings},
                                   &namespace_uri);
            if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
                result->is_unbound = true;
                return SXML_STATUS_OK;
            }
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!sxml_xml_text_is_ncname(local_name)) {
        result->is_malformed = true;
        return SXML_STATUS_OK;
    }
    return sxml_name_pool_intern_name(
        reader->name_pool,
        &(sxml_name_request){.namespace_uri = namespace_uri, .local_name = local_name},
        &result->name);
}

/* ------------------------------------------------------------ Expressions */

/* Text to parse with the front end of its kind. */
typedef struct expression_text {
    sxml_xslt_ast_value_kind kind;
    sxml_byte_view text;
    sxml_xslt_ast_module_index module;
    sxml_ast_source_span source_span;
} expression_text;

static sxml_status parse_expression(stylesheet_reader *reader, const expression_text *expression,
                                    sxml_ast **ast) {
    const sxml_ast_build_request request = {
        .expression_text = expression->text,
        /* Module URIs never move (D102), so the AST may borrow it. */
        .source_name = sxml_xslt_ast_get_module(reader->ast, expression->module)->module_uri,
        .allocator = reader->allocator,
        .maximum_expression_depth = sxml_processor_get_maximum_expression_depth(reader->processor),
        .error_record = reader->error_record};
    sxml_status status = SXML_STATUS_INVALID_ARGUMENT;
    switch (expression->kind) {
    case SXML_XSLT_AST_VALUE_KIND_EXPRESSION:
        status = sxml_xpath_build_ast(&request, ast);
        break;
    case SXML_XSLT_AST_VALUE_KIND_PATTERN:
        status = sxml_xslt_pattern_build_ast(&request, ast);
        break;
    case SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE:
        status = sxml_xslt_build_value_template_ast(&request, ast);
        break;
    case SXML_XSLT_AST_VALUE_KIND_SEQUENCE_TYPE:
    case SXML_XSLT_AST_VALUE_KIND_ITEM_TYPE:
        status = sxml_xpath_build_sequence_type_ast(&request, ast);
        break;
    case SXML_XSLT_AST_VALUE_KIND_NONE:
    case SXML_XSLT_AST_VALUE_KIND_TEXT:
    case SXML_XSLT_AST_VALUE_KIND_BOOLEAN:
    case SXML_XSLT_AST_VALUE_KIND_NUMBER:
    case SXML_XSLT_AST_VALUE_KIND_KEYWORD:
    case SXML_XSLT_AST_VALUE_KIND_NAME:
    case SXML_XSLT_AST_VALUE_KIND_NAME_LIST:
    case SXML_XSLT_AST_VALUE_KIND_NAMESPACE_BINDING:
        break;
    }
    if (status == SXML_STATUS_OK) {
        /* Its nodes are placed in the module, so a dynamic error points
           where a static one does (D169). */
        sxml_ast_move_source_spans(*ast, expression->source_span);
    }
    return relocate_error(reader, (error_relocation){.status = status,
                                                     .module = expression->module,
                                                     .source_span = expression->source_span});
}

/* An expression AST to hand to the stylesheet AST. */
typedef struct expression_handover {
    sxml_xslt_ast_value_kind kind;
    /* Owned; destroyed when the handover fails. */
    sxml_ast *expression_ast;
    sxml_xslt_ast_static_context_index static_context;
    sxml_xslt_ast_node_index node;
    sxml_ast_source_span source_span;
} expression_handover;

static sxml_status hand_over_expression(stylesheet_reader *reader, const expression_handover *handover,
                                        sxml_xslt_ast_expression_index *expression) {
    const sxml_status status = sxml_xslt_ast_take_expression(
        reader->ast,
        &(sxml_xslt_ast_expression_request){.kind = handover->kind,
                                            .expression_ast = handover->expression_ast,
                                            .static_context = handover->static_context,
                                            .node = handover->node,
                                            .source_span = handover->source_span},
        expression);
    if (status != SXML_STATUS_OK) {
        sxml_ast_destroy(handover->expression_ast);
    }
    return status;
}

/* The index the next expression handed over gets: the AST adds entries
   in order, from 1. Used for node contents, which name their expression
   before the expression names its node. */
static sxml_xslt_ast_expression_index next_expression_index(const stylesheet_reader *reader) {
    return (sxml_xslt_ast_expression_index){
        .value = reader->ast->expression_count == 0 ? 1U : reader->ast->expression_count};
}

/* ------------------------------------------------------------ Values */

/* An attribute value to read into the form of its rule. */
typedef struct value_request {
    const attribute_rule *rule;
    const element_attribute *attribute;
    sxml_xslt_ast_module_index module;
    sxml_xslt_ast_static_context_index static_context;
    sxml_xslt_ast_node_index node;
    element_source source;
} value_request;

static sxml_status raise_value_error(stylesheet_reader *reader, const value_request *request,
                                     reader_error error) {
    return raise_error(reader, (reader_error){.error_code = "XTSE0020",
                                              .description = error.description,
                                              .module = request->module,
                                              .source_span = request->attribute->source_span});
}

static sxml_status raise_name_error(stylesheet_reader *reader, const value_request *request,
                                    const eqname_result *result) {
    if (result->is_unbound) {
        return raise_error(reader,
                           (reader_error){.error_code = "XTSE0280",
                                          .description = "A prefix in a name-valued attribute "
                                                         "has no namespace declaration (XSLT "
                                                         "4.0, 5.1.1)",
                                          .module = request->module,
                                          .source_span = request->attribute->source_span});
    }
    return raise_value_error(reader, request,
(reader_error){.description = "A name-valued attribute holds no EQName (XSLT 4.0, 5.1.1)"});
}

/* The namespace of unprefixed names in an attribute: the default
   namespace of the element for names of result elements
   (cdata-section-elements, suppress-indentation: Serialization 4.0), the
   xpath-default-namespace for element name tests (strip-space,
   preserve-space: 4.2.2), none otherwise. */
static sxml_namespace_uri_identifier unprefixed_namespace_of(const stylesheet_reader *reader,
                                                             const value_request *request) {
    const sxml_xslt_ast_attribute_kind kind = request->rule->attribute_kind;
    if (kind == SXML_XSLT_AST_ATTRIBUTE_CDATA_SECTION_ELEMENTS ||
        kind == SXML_XSLT_AST_ATTRIBUTE_SUPPRESS_INDENTATION) {
        sxml_namespace_uri_identifier uri = {.index = SXML_NAMESPACE_NONE};
        (void)find_native_prefix(
            (native_prefix_query){.source = request->source, .namespace_prefix = {.index = 0}},
            &uri);
        return uri;
    }
    if (kind == SXML_XSLT_AST_ATTRIBUTE_ELEMENTS) {
        return sxml_xslt_ast_get_static_context(reader->ast, request->static_context)
            ->xpath_default_namespace;
    }
    return (sxml_namespace_uri_identifier){.index = SXML_NAMESPACE_NONE};
}

static sxml_status append_name_token(stylesheet_reader *reader, const sxml_xslt_ast_name_token *token) {
    const sxml_status status = reserve_scratch(
        reader, &reader->name_tokens,
        (scratch_reservation){.element_size = sizeof(sxml_xslt_ast_name_token),
                              .element_alignment = alignof(sxml_xslt_ast_name_token),
                              .needed_count = reader->name_tokens.count + 1U});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xslt_ast_name_token *tokens = reader->name_tokens.elements;
    tokens[reader->name_tokens.count] = *token;
    reader->name_tokens.count += 1;
    return SXML_STATUS_OK;
}

/* Reads one token of a name list into *token: a name, a wildcard where
   the syntax has name tests, or a # keyword the syntax allows. */
typedef struct name_token_request {
    const value_request *value;
    sxml_byte_view text;
} name_token_request;

static sxml_status read_name_token(stylesheet_reader *reader, name_token_request request,
                                   sxml_xslt_ast_name_token *token) {
    const value_syntax syntax = request.value->rule->value_syntax;
    const bool has_tests =
        syntax == SXML_VALUE_SYNTAX_NAME_TESTS || syntax == SXML_VALUE_SYNTAX_COMPONENT_TESTS;
    sxml_byte_view text = request.text;
    *token = (sxml_xslt_ast_name_token){.kind = SXML_XSLT_AST_NAME_TOKEN_KIND_NAME,
                                        .name = {.index = 0},
                                        .namespace_uri = {.index = 0},
                                        .has_arity = false,
                                        .arity = 0};
    static const struct {
        sxml_byte_view word;
        sxml_xslt_ast_name_token_kind kind;
    } words[] = {
        {SXML_BYTE_VIEW_INITIALIZER("#default"), SXML_XSLT_AST_NAME_TOKEN_KIND_DEFAULT},
        {SXML_BYTE_VIEW_INITIALIZER("#unnamed"), SXML_XSLT_AST_NAME_TOKEN_KIND_UNNAMED},
        {SXML_BYTE_VIEW_INITIALIZER("#current"), SXML_XSLT_AST_NAME_TOKEN_KIND_CURRENT},
        {SXML_BYTE_VIEW_INITIALIZER("#all"), SXML_XSLT_AST_NAME_TOKEN_KIND_ALL},
    };
    for (size_t index = 0; index < sizeof(words) / sizeof(words[0]); index += 1) {
        if (!sxml_byte_view_equals(text, words[index].word)) {
            continue;
        }
        const sxml_xslt_ast_name_token_kind kind = words[index].kind;
        const bool is_allowed =
            (syntax == SXML_VALUE_SYNTAX_MODES && kind != SXML_XSLT_AST_NAME_TOKEN_KIND_CURRENT) ||
            (syntax == SXML_VALUE_SYNTAX_MODE && kind != SXML_XSLT_AST_NAME_TOKEN_KIND_ALL) ||
            (syntax == SXML_VALUE_SYNTAX_ACCUMULATOR_NAMES && kind == SXML_XSLT_AST_NAME_TOKEN_KIND_ALL);
        if (!is_allowed) {
            return raise_value_error(reader, request.value,
(reader_error){.description = "A name list holds a keyword its attribute does not allow "
                                     "(XSLT 4.0, 2.3)"});
        }
        token->kind = kind;
        return SXML_STATUS_OK;
    }
    if (syntax == SXML_VALUE_SYNTAX_COMPONENT_TESTS) {
        /* name#arity (3.4.5.2). */
        size_t hash_index = text.byte_count;
        while (hash_index > 0 && text.bytes[hash_index - 1] >= '0' && text.bytes[hash_index - 1] <= '9') {
            hash_index -= 1;
        }
        if (hash_index > 1 && hash_index < text.byte_count && text.bytes[hash_index - 1] == '#') {
            uint64_t arity = 0;
            for (size_t index = hash_index; index < text.byte_count && arity < UINT32_MAX; index += 1) {
                arity = arity * 10U + (uint64_t)(text.bytes[index] - '0');
            }
            token->has_arity = true;
            token->arity = arity > UINT32_MAX ? UINT32_MAX : (uint32_t)arity;
            text.byte_count = hash_index - 1;
        }
    }
    const sxml_namespace_binding_view bindings =
        sxml_xslt_ast_get_namespace_bindings(reader->ast, request.value->static_context);
    if (has_tests) {
        if (sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("*"))) {
            token->kind = SXML_XSLT_AST_NAME_TOKEN_KIND_ANY_NAME;
            return SXML_STATUS_OK;
        }
        if (text.byte_count > 2 && text.bytes[0] == '*' && text.bytes[1] == ':') {
            const sxml_byte_view local = {.bytes = text.bytes + 2, .byte_count = text.byte_count - 2};
            if (!sxml_xml_text_is_ncname(local)) {
                return raise_value_error(reader, request.value,
(reader_error){.description = "A name test is malformed (XSLT 4.0, 2.3)"});
            }
            token->kind = SXML_XSLT_AST_NAME_TOKEN_KIND_LOCAL_NAME_WILDCARD;
            return sxml_name_pool_intern_name(
                reader->name_pool,
                &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                                     .local_name = local},
                &token->name);
        }
        if (text.byte_count > 2 && text.bytes[text.byte_count - 1] == '*' &&
            (text.bytes[text.byte_count - 2] == ':' || text.bytes[text.byte_count - 2] == '}')) {
            token->kind = SXML_XSLT_AST_NAME_TOKEN_KIND_NAMESPACE_WILDCARD;
            if (text.bytes[text.byte_count - 2] == '}') {
                if (text.byte_count < 4 || text.bytes[0] != 'Q' || text.bytes[1] != '{') {
                    return raise_value_error(reader, request.value,
(reader_error){.description = "A name test is malformed (XSLT 4.0, 2.3)"});
                }
                return sxml_name_pool_intern_namespace_uri(
                    reader->name_pool,
                    (sxml_byte_view){.bytes = text.bytes + 2, .byte_count = text.byte_count - 4},
                    &token->namespace_uri);
            }
            const sxml_status status = lookup_prefix(
                reader,
                (prefix_lookup){.prefix_text = {.bytes = text.bytes,
                                                .byte_count = text.byte_count - 2},
                                .bindings = bindings},
                &token->namespace_uri);
            if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
                const eqname_result unbound = {.name = {.index = 0},
                                               .is_malformed = false,
                                               .is_unbound = true};
                return raise_name_error(reader, request.value, &unbound);
            }
            return status;
        }
    }
    eqname_result result = {.name = {.index = 0}, .is_malformed = false, .is_unbound = false};
    const sxml_status status = resolve_eqname(
        reader,
        &(eqname_request){.text = text,
                          .bindings = bindings,
                          .unprefixed_namespace = unprefixed_namespace_of(reader, request.value)},
        &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (result.is_malformed || result.is_unbound) {
        return raise_name_error(reader, request.value, &result);
    }
    token->name = result.name;
    return SXML_STATUS_OK;
}

static sxml_status read_name_list(stylesheet_reader *reader, const value_request *request,
                                  sxml_xslt_ast_value *value) {
    reader->name_tokens.count = 0;
    sxml_byte_view rest = request->attribute->value;
    sxml_byte_view token_text = {.bytes = NULL, .byte_count = 0};
    while (sxml_xml_text_next_token(&rest, &token_text)) {
        sxml_xslt_ast_name_token token;
        sxml_status status = read_name_token(
            reader, (name_token_request){.value = request, .text = token_text}, &token);
        if (status == SXML_STATUS_OK) {
            status = append_name_token(reader, &token);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (request->rule->value_syntax == SXML_VALUE_SYNTAX_MODE && reader->name_tokens.count != 1) {
        return raise_value_error(reader, request,
(reader_error){.description = "The mode of xsl:apply-templates is not one token (XSLT 4.0, "
                                 "6.6)"});
    }
    sxml_xslt_ast_name_token_list list = {.first_name_token_index = 0, .name_token_count = 0};
    const sxml_status status = sxml_xslt_ast_add_name_tokens(
        reader->ast,
        (sxml_xslt_ast_name_token_view){.name_tokens = reader->name_tokens.elements,
                                        .name_token_count = reader->name_tokens.count},
        &list);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *value = (sxml_xslt_ast_value){.kind = SXML_XSLT_AST_VALUE_KIND_NAME_LIST,
                                   .payload = {.name_tokens = list}};
    return SXML_STATUS_OK;
}

static sxml_status read_text_value(stylesheet_reader *reader, sxml_byte_view text,
                                   sxml_xslt_ast_value *value) {
    sxml_xslt_ast_text stored = {.byte_offset = 0, .byte_count = 0};
    const sxml_status status = sxml_xslt_ast_append_text(reader->ast, text, &stored);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *value = (sxml_xslt_ast_value){.kind = SXML_XSLT_AST_VALUE_KIND_TEXT,
                                   .payload = {.text = stored}};
    return SXML_STATUS_OK;
}

static bool find_keyword(sxml_byte_view text, sxml_xslt_ast_keyword *keyword) {
    for (size_t index = 0; index < sizeof(keyword_spellings) / sizeof(keyword_spellings[0]);
         index += 1) {
        if (sxml_byte_view_equals(keyword_spellings[index].spelling, text)) {
            *keyword = keyword_spellings[index].keyword;
            return true;
        }
    }
    return false;
}

static sxml_status read_value(stylesheet_reader *reader, const value_request *request,
                              sxml_xslt_ast_value *value) {
    const sxml_byte_view text = request->attribute->value;
    const sxml_byte_view trimmed = sxml_xml_text_trim_whitespace(text);
    sxml_xslt_ast_value_kind expression_kind = SXML_XSLT_AST_VALUE_KIND_NONE;
    switch (request->rule->value_syntax) {
    case SXML_VALUE_SYNTAX_EXPRESSION:
        expression_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION;
        break;
    case SXML_VALUE_SYNTAX_PATTERN:
        expression_kind = SXML_XSLT_AST_VALUE_KIND_PATTERN;
        break;
    case SXML_VALUE_SYNTAX_VALUE_TEMPLATE:
        expression_kind = SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE;
        break;
    case SXML_VALUE_SYNTAX_SEQUENCE_TYPE:
        expression_kind = SXML_XSLT_AST_VALUE_KIND_SEQUENCE_TYPE;
        break;
    case SXML_VALUE_SYNTAX_ITEM_TYPE:
        expression_kind = SXML_XSLT_AST_VALUE_KIND_ITEM_TYPE;
        break;
    case SXML_VALUE_SYNTAX_STRING:
        return read_text_value(reader, text, value);
    case SXML_VALUE_SYNTAX_TOKEN:
        return read_text_value(reader, trimmed, value);
    case SXML_VALUE_SYNTAX_CHARACTER:
        if (count_characters(text) != 1) {
            return raise_value_error(reader, request,
(reader_error){.description = "The attribute must be one character (XSLT 4.0, 2.3)"});
        }
        return read_text_value(reader, text, value);
    case SXML_VALUE_SYNTAX_CHARACTER_OR_RENDITION: {
        /* "c" or "c:rendition" (F&O 4.0, "Defining a decimal format"). */
        size_t first_character_bytes = 1;
        while (first_character_bytes < text.byte_count &&
               (text.bytes[first_character_bytes] & 0xC0U) == 0x80U) {
            first_character_bytes += 1;
        }
        if (text.byte_count == 0 ||
            (first_character_bytes < text.byte_count && text.bytes[first_character_bytes] != ':')) {
            return raise_value_error(reader, request,
(reader_error){.description = "The attribute must be one character, optionally with a "
                                     "rendition (XSLT 4.0, 5.5)"});
        }
        return read_text_value(reader, text, value);
    }
    case SXML_VALUE_SYNTAX_BOOLEAN: {
        sxml_xslt_ast_keyword keyword = SXML_XSLT_AST_KEYWORD_NONE;
        if (!find_keyword(trimmed, &keyword) ||
            (keyword != SXML_XSLT_AST_KEYWORD_YES && keyword != SXML_XSLT_AST_KEYWORD_NO)) {
            return raise_value_error(reader, request,
(reader_error){.description = "The attribute must be yes, no, true, false, 1 or 0 (XSLT "
                                     "4.0, 2.3)"});
        }
        *value = (sxml_xslt_ast_value){
            .kind = SXML_XSLT_AST_VALUE_KIND_BOOLEAN,
            .payload = {.is_true = keyword == SXML_XSLT_AST_KEYWORD_YES}};
        return SXML_STATUS_OK;
    }
    case SXML_VALUE_SYNTAX_DECIMAL: {
        double number = 0;
        if (!read_decimal(trimmed, &number)) {
            if (request->rule->attribute_kind == SXML_XSLT_AST_ATTRIBUTE_PRIORITY) {
                return raise_error(reader, (reader_error){.error_code = "XTSE0530",
                                                          .description = "The priority is not "
                                                                         "an xs:decimal (XSLT "
                                                                         "4.0, 6.5)",
                                                          .module = request->module,
                                                          .source_span =
                                                              request->attribute->source_span});
            }
            return raise_value_error(reader, request,
(reader_error){.description = "The attribute must be an xs:decimal (XSLT 4.0, 2.3)"});
        }
        *value = (sxml_xslt_ast_value){.kind = SXML_XSLT_AST_VALUE_KIND_NUMBER,
                                       .payload = {.number = number}};
        return SXML_STATUS_OK;
    }
    case SXML_VALUE_SYNTAX_KEYWORD:
    case SXML_VALUE_SYNTAX_KEYWORD_OR_NAME:
    case SXML_VALUE_SYNTAX_KEYWORD_OR_TEXT: {
        sxml_xslt_ast_keyword keyword = SXML_XSLT_AST_KEYWORD_NONE;
        if (find_keyword(trimmed, &keyword) &&
            (request->rule->keyword_mask & KEYWORD_BIT(keyword)) != 0) {
            *value = (sxml_xslt_ast_value){.kind = SXML_XSLT_AST_VALUE_KIND_KEYWORD,
                                           .payload = {.keyword = keyword}};
            return SXML_STATUS_OK;
        }
        if (request->rule->value_syntax == SXML_VALUE_SYNTAX_KEYWORD_OR_TEXT && trimmed.byte_count > 0) {
            return read_text_value(reader, trimmed, value);
        }
        if (request->rule->value_syntax == SXML_VALUE_SYNTAX_KEYWORD_OR_NAME) {
            eqname_result result = {.name = {.index = 0}, .is_malformed = false,
                                    .is_unbound = false};
            const sxml_status status = resolve_eqname(
                reader,
                &(eqname_request){.text = trimmed,
                                  .bindings = sxml_xslt_ast_get_namespace_bindings(
                                      reader->ast, request->static_context),
                                  .unprefixed_namespace = {.index = SXML_NAMESPACE_NONE}},
                &result);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            if (!result.is_malformed && !result.is_unbound) {
                sxml_name_parts parts = {.namespace_uri = {.index = 0},
                                         .local_name = {.bytes = NULL, .byte_count = 0}};
                if (sxml_name_pool_get_name_parts(reader->name_pool, result.name, &parts) ==
                        SXML_STATUS_OK &&
                    parts.namespace_uri.index != SXML_NAMESPACE_NONE) {
                    *value = (sxml_xslt_ast_value){.kind = SXML_XSLT_AST_VALUE_KIND_NAME,
                                                   .payload = {.name = result.name}};
                    return SXML_STATUS_OK;
                }
            } else if (result.is_unbound) {
                return raise_name_error(reader, request, &result);
            }
        }
        return raise_value_error(reader, request,
(reader_error){.description = "The attribute has a value its syntax does not allow (XSLT 4.0, "
                                 "2.3)"});
    }
    case SXML_VALUE_SYNTAX_EQNAME:
    case SXML_VALUE_SYNTAX_NCNAME: {
        eqname_result result = {.name = {.index = 0}, .is_malformed = false, .is_unbound = false};
        if (request->rule->value_syntax == SXML_VALUE_SYNTAX_NCNAME && !sxml_xml_text_is_ncname(trimmed)) {
            return raise_value_error(reader, request,
(reader_error){.description = "The attribute must be an NCName (XSLT 4.0, 2.3)"});
        }
        /* 25.1.2: an unprefixed [xsl:]type is in the default namespace for
           elements and types. */
        const sxml_xslt_ast_static_context *context =
            sxml_xslt_ast_get_static_context(reader->ast, request->static_context);
        const sxml_namespace_uri_identifier unprefixed_namespace = {
            .index = request->rule->attribute_kind == SXML_XSLT_AST_ATTRIBUTE_TYPE &&
                             !context->is_any_namespace
                         ? context->xpath_default_namespace.index
                         : (uint32_t)SXML_NAMESPACE_NONE};
        const sxml_status status = resolve_eqname(
            reader,
            &(eqname_request){.text = trimmed,
                              .bindings = sxml_xslt_ast_get_namespace_bindings(
                                  reader->ast, request->static_context),
                              .unprefixed_namespace = unprefixed_namespace},
            &result);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (result.is_malformed || result.is_unbound) {
            return raise_name_error(reader, request, &result);
        }
        *value = (sxml_xslt_ast_value){.kind = SXML_XSLT_AST_VALUE_KIND_NAME,
                                       .payload = {.name = result.name}};
        return SXML_STATUS_OK;
    }
    case SXML_VALUE_SYNTAX_EQNAMES:
    case SXML_VALUE_SYNTAX_NAME_TESTS:
    case SXML_VALUE_SYNTAX_MODES:
    case SXML_VALUE_SYNTAX_MODE:
    case SXML_VALUE_SYNTAX_ACCUMULATOR_NAMES:
    case SXML_VALUE_SYNTAX_COMPONENT_TESTS:
        return read_name_list(reader, request, value);
    case SXML_VALUE_SYNTAX_PREFIX_OR_DEFAULT: {
        const bool is_default = sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("#default"));
        sxml_namespace_prefix_identifier prefix = {.index = 0};
        if (!is_default) {
            if (!sxml_xml_text_is_ncname(trimmed)) {
                return raise_value_error(reader, request,
(reader_error){.description = "The attribute must be a prefix or #default (XSLT 4.0, "
                                         "11.1.4)"});
            }
            const sxml_status status =
                sxml_name_pool_intern_namespace_prefix(reader->name_pool, trimmed, &prefix);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        sxml_namespace_uri_identifier uri = {.index = SXML_NAMESPACE_NONE};
        if (!find_native_prefix(
                (native_prefix_query){.source = request->source, .namespace_prefix = prefix},
                &uri)) {
            return raise_error(reader, (reader_error){.error_code = "XTSE0812",
                                                      .description = "A prefix of "
                                                                     "xsl:namespace-alias has no "
                                                                     "namespace declaration (XSLT "
                                                                     "4.0, 11.1.4)",
                                                      .module = request->module,
                                                      .source_span =
                                                          request->attribute->source_span});
        }
        *value = (sxml_xslt_ast_value){
            .kind = SXML_XSLT_AST_VALUE_KIND_NAMESPACE_BINDING,
            .payload = {.namespace_binding = {.namespace_prefix = prefix, .namespace_uri = uri}}};
        return SXML_STATUS_OK;
    }
    }
    sxml_ast *expression_ast = NULL;
    sxml_status status = parse_expression(
        reader,
        &(expression_text){.kind = expression_kind,
                           .text = text,
                           .module = request->module,
                           .source_span = request->attribute->source_span},
        &expression_ast);
    sxml_xslt_ast_expression_index expression = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = hand_over_expression(
            reader,
            &(expression_handover){.kind = expression_kind,
                                   .expression_ast = expression_ast,
                                   .static_context = request->static_context,
                                   .node = request->node,
                                   .source_span = request->attribute->source_span},
            &expression);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *value = (sxml_xslt_ast_value){.kind = expression_kind, .payload = {.expression = expression}};
    return SXML_STATUS_OK;
}
/* What decides the static context of one element. */
typedef struct context_request {
    element_source source;
    sxml_xslt_ast_module_index module;
    sxml_xslt_ast_static_context_index parent_context;
    bool is_xslt_element;
    bool is_module_root;
    /* The outermost element of a simplified module (3.7). */
    bool is_simplified_root;
    /* A child of xsl:override gets a static context of its own, even with
       nothing changed, so lowering can tell where xsl:original refers to
       (3.4.5.4; src/xslt_package.c). */
    bool is_overriding_declaration;
} context_request;

/* True when the bindings of the scratch array equal those of context. */
static bool bindings_equal(const stylesheet_reader *reader, sxml_xslt_ast_static_context_index context) {
    const sxml_namespace_binding_view parent =
        sxml_xslt_ast_get_namespace_bindings(reader->ast, context);
    if (parent.namespace_binding_count != reader->bindings.count) {
        return false;
    }
    for (size_t index = 0; index < reader->bindings.count; index += 1) {
        const sxml_namespace_binding *binding = binding_at(&reader->bindings, index);
        if (binding->namespace_prefix.index != parent.namespace_bindings[index].namespace_prefix.index ||
            binding->namespace_uri.index != parent.namespace_bindings[index].namespace_uri.index) {
            return false;
        }
    }
    return true;
}

static const char *const codepoint_collation_uri =
    "http://www.w3.org/2005/xpath-functions/collation/codepoint";

/* Fills the reader's scratch with the static context of an element and
   adds it, or gives the parent's when nothing changes (D102). */
static sxml_status compute_static_context(stylesheet_reader *reader, const context_request *request,
                                          sxml_xslt_ast_static_context_index *context) {
    const sxml_xslt_ast_static_context *parent =
        sxml_xslt_ast_get_static_context(reader->ast, request->parent_context);
    const bool has_parent = request->parent_context.value != 0;
    bool is_changed = !has_parent || request->is_overriding_declaration;
    sxml_status status = SXML_STATUS_OK;

    /* Namespaces: the fixed bindings, else the native ones. */
    reader->bindings.count = 0;
    const element_attribute *fixed =
        request->is_module_root ? find_standard_value(reader, (standard_query){
                                                                  .is_xslt_element = true,
                                                                  .local_name = "fixed-namespaces"})
                                : NULL;
    if (fixed != NULL) {
        status = read_fixed_namespaces(
            reader, (fixed_namespaces_request){.attribute = fixed,
                                               .source = request->source,
                                               .module = request->module});
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    const module_state *module = state_of_module(reader, request->module);
    if (module->has_fixed_namespaces) {
        const sxml_namespace_binding *fixed_bindings = reader->fixed_bindings.elements;
        for (size_t index = 0; status == SXML_STATUS_OK && index < module->fixed_binding_count;
             index += 1) {
            status = append_binding(
                reader, &reader->bindings,
                (binding_addition){.binding = fixed_bindings[module->first_fixed_binding + index],
                                   .first_index = 0});
        }
    } else {
        sxml_namespace_binding_view natives = {.namespace_bindings = NULL,
                                               .namespace_binding_count = 0};
        status = native_bindings(request->source, &natives);
        for (size_t index = 0; status == SXML_STATUS_OK && index < natives.namespace_binding_count;
             index += 1) {
            const sxml_namespace_binding binding = natives.namespace_bindings[index];
            if (binding.namespace_prefix.index != 0 &&
                binding.namespace_uri.index != SXML_NAMESPACE_XML &&
                binding.namespace_uri.index != SXML_NAMESPACE_NONE) {
                status = append_binding(reader, &reader->bindings,
                                        (binding_addition){.binding = binding, .first_index = 0});
            }
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (has_parent && !bindings_equal(reader, request->parent_context)) {
        is_changed = true;
    }

    const bool is_xslt = request->is_xslt_element;
    double version = has_parent ? parent->version : 4.0;
    const element_attribute *version_attribute =
        find_standard_value(reader, (standard_query){.is_xslt_element = is_xslt,
                                                     .local_name = "version"});
    if (version_attribute != NULL) {
        is_changed = true;
        if (!read_decimal(version_attribute->value, &version)) {
            return raise_error(reader, (reader_error){.error_code = "XTSE0110",
                                                      .description = "The version attribute is "
                                                                     "not an xs:decimal (XSLT "
                                                                     "4.0, 3.6)",
                                                      .module = request->module,
                                                      .source_span =
                                                          version_attribute->source_span});
        }
    } else if (request->is_module_root) {
        return raise_error(reader, (reader_error){.error_code = "XTSE0010",
                                                  .description = "The outermost element of a "
                                                                 "stylesheet module has no "
                                                                 "version attribute (XSLT 4.0, "
                                                                 "3.6)",
                                                  .module = request->module,
                                                  .source_span = location_of(
                                                      request->source.tree,
                                                      request->source.element)});
    }

    sxml_namespace_uri_identifier xpath_default_namespace =
        has_parent ? parent->xpath_default_namespace
                   : (sxml_namespace_uri_identifier){.index = SXML_NAMESPACE_NONE};
    bool is_any_namespace = has_parent && parent->is_any_namespace;
    const element_attribute *default_namespace_attribute = find_standard_value(
        reader,
        (standard_query){.is_xslt_element = is_xslt, .local_name = "xpath-default-namespace"});
    if (default_namespace_attribute != NULL) {
        is_changed = true;
        const sxml_byte_view uri = sxml_xml_text_trim_whitespace(default_namespace_attribute->value);
        is_any_namespace = sxml_byte_view_equals(uri, SXML_BYTE_VIEW_FROM_LITERAL("##any"));
        xpath_default_namespace = (sxml_namespace_uri_identifier){.index = SXML_NAMESPACE_NONE};
        if (!is_any_namespace) {
            status = sxml_name_pool_intern_namespace_uri(reader->name_pool, uri,
                                                         &xpath_default_namespace);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
    }

    bool expands_text = has_parent ? parent->expands_text
                                   : (request->is_simplified_root && version > 3.0);
    const element_attribute *expand_attribute = find_standard_value(
        reader, (standard_query){.is_xslt_element = is_xslt, .local_name = "expand-text"});
    if (expand_attribute != NULL) {
        is_changed = true;
        const sxml_byte_view word = sxml_xml_text_trim_whitespace(expand_attribute->value);
        if (sxml_byte_view_equals(word, SXML_BYTE_VIEW_FROM_LITERAL("yes")) ||
            sxml_byte_view_equals(word, SXML_BYTE_VIEW_FROM_LITERAL("true")) ||
            sxml_byte_view_equals(word, SXML_BYTE_VIEW_FROM_LITERAL("1"))) {
            expands_text = true;
        } else if (sxml_byte_view_equals(word, SXML_BYTE_VIEW_FROM_LITERAL("no")) ||
                   sxml_byte_view_equals(word, SXML_BYTE_VIEW_FROM_LITERAL("false")) ||
                   sxml_byte_view_equals(word, SXML_BYTE_VIEW_FROM_LITERAL("0"))) {
            expands_text = false;
        } else if (version <= 4.0) {
            return raise_error(reader, (reader_error){.error_code = "XTSE0020",
                                                      .description = "expand-text is not a "
                                                                     "boolean (XSLT 4.0, 2.3)",
                                                      .module = request->module,
                                                      .source_span =
                                                          expand_attribute->source_span});
        }
    }

    sxml_xslt_ast_keyword default_validation =
        has_parent ? parent->default_validation : SXML_XSLT_AST_KEYWORD_STRIP;
    const element_attribute *validation_attribute = find_standard_value(
        reader, (standard_query){.is_xslt_element = is_xslt, .local_name = "default-validation"});
    if (validation_attribute != NULL) {
        is_changed = true;
        const sxml_byte_view word = sxml_xml_text_trim_whitespace(validation_attribute->value);
        if (sxml_byte_view_equals(word, SXML_BYTE_VIEW_FROM_LITERAL("strip"))) {
            default_validation = SXML_XSLT_AST_KEYWORD_STRIP;
        } else if (sxml_byte_view_equals(word, SXML_BYTE_VIEW_FROM_LITERAL("preserve"))) {
            default_validation = SXML_XSLT_AST_KEYWORD_PRESERVE;
        } else if (version <= 4.0) {
            return raise_error(reader, (reader_error){.error_code = "XTSE0020",
                                                      .description = "default-validation is "
                                                                     "neither preserve nor "
                                                                     "strip (XSLT 4.0, 3.6)",
                                                      .module = request->module,
                                                      .source_span =
                                                          validation_attribute->source_span});
        }
    }

    /* Cumulative lists (11.1.3, 24.3): the parent's, the XSLT namespace
       at a module root, then this element's. */
    reader->excluded_uris.count = 0;
    reader->extension_uris.count = 0;
    if (has_parent) {
        const sxml_xslt_ast_namespace_uri_view excluded =
            sxml_xslt_ast_get_namespace_uris(reader->ast, parent->excluded_namespaces);
        for (size_t index = 0; status == SXML_STATUS_OK && index < excluded.namespace_uri_count;
             index += 1) {
            status = append_uri(reader, &reader->excluded_uris, excluded.namespace_uris[index]);
        }
        const sxml_xslt_ast_namespace_uri_view extension =
            sxml_xslt_ast_get_namespace_uris(reader->ast, parent->extension_namespaces);
        for (size_t index = 0; status == SXML_STATUS_OK && index < extension.namespace_uri_count;
             index += 1) {
            status = append_uri(reader, &reader->extension_uris, extension.namespace_uris[index]);
        }
    } else {
        status = append_uri(reader, &reader->excluded_uris,
                            (sxml_namespace_uri_identifier){.index = SXML_NAMESPACE_XSLT});
    }
    const element_attribute *extension_attribute = find_standard_value(
        reader,
        (standard_query){.is_xslt_element = is_xslt, .local_name = "extension-element-prefixes"});
    if (status == SXML_STATUS_OK && extension_attribute != NULL) {
        is_changed = true;
        status = read_prefix_list(reader, &(prefix_list_request){
                                              .text = extension_attribute->value,
                                              .source = request->source,
                                              .allows_all = false,
                                              .uris = &reader->extension_uris,
                                              .error_code = "XTSE1430",
                                              .module = request->module,
                                              .source_span = extension_attribute->source_span});
        const sxml_namespace_uri_identifier *extension = reader->extension_uris.elements;
        for (size_t index = 0; status == SXML_STATUS_OK && index < reader->extension_uris.count;
             index += 1) {
            status = append_uri(reader, &reader->excluded_uris, extension[index]);
        }
    }
    const element_attribute *exclude_attribute = find_standard_value(
        reader,
        (standard_query){.is_xslt_element = is_xslt, .local_name = "exclude-result-prefixes"});
    if (status == SXML_STATUS_OK && exclude_attribute != NULL) {
        is_changed = true;
        status = read_prefix_list(reader, &(prefix_list_request){
                                              .text = exclude_attribute->value,
                                              .source = request->source,
                                              .allows_all = true,
                                              .uris = &reader->excluded_uris,
                                              .error_code = "XTSE0808",
                                              .module = request->module,
                                              .source_span = exclude_attribute->source_span});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }

    /* Default collation and static base URI, copied out of the AST's text
       store, which the add below may move. */
    reader->context_text.count = 0;
    if (has_parent) {
        status = append_text_bytes(reader, &reader->context_text,
                                   sxml_xslt_ast_get_text(reader->ast, parent->default_collation));
    } else if (reader->default_collation_uri.byte_count > 0 &&
               !sxml_byte_view_equals(reader->default_collation_uri,
                                      sxml_byte_view_from_c_string(codepoint_collation_uri))) {
        status = append_text_bytes(reader, &reader->context_text, reader->default_collation_uri);
    }
    const element_attribute *collation_attribute = find_standard_value(
        reader, (standard_query){.is_xslt_element = is_xslt, .local_name = "default-collation"});
    if (status == SXML_STATUS_OK && collation_attribute != NULL) {
        is_changed = true;
        reader->context_text.count = 0;
        sxml_byte_view rest = collation_attribute->value;
        sxml_byte_view token = {.bytes = NULL, .byte_count = 0};
        bool is_found = false;
        while (!is_found && sxml_xml_text_next_token(&rest, &token)) {
            if (sxml_byte_view_equals(token, sxml_byte_view_from_c_string(codepoint_collation_uri))) {
                is_found = true;
            } else if (sxml_collation_is_supported(token)) {
                is_found = true;
                status = append_text_bytes(reader, &reader->context_text, token);
            }
        }
        if (!is_found) {
            return raise_error(reader, (reader_error){.error_code = "XTSE0125",
                                                      .description = "No collation of "
                                                                     "default-collation is "
                                                                     "supported (XSLT 4.0, 3.6.2)",
                                                      .module = request->module,
                                                      .source_span =
                                                          collation_attribute->source_span});
        }
    }
    const size_t collation_byte_count = reader->context_text.count;
    sxml_byte_view base_uri = has_parent
                                  ? sxml_xslt_ast_get_text(reader->ast, parent->static_base_uri)
                                  : sxml_xslt_ast_get_module(reader->ast, request->module)->module_uri;
    const element_attribute *xml_base = find_element_attribute(
        reader, (attribute_query){.namespace_uri = {.index = SXML_NAMESPACE_XML},
                                  .local_name = SXML_BYTE_VIEW_FROM_LITERAL("base")});
    sxml_uri_resolution resolution = {.uri = {.bytes = NULL, .byte_count = 0}, .memory = NULL};
    if (status == SXML_STATUS_OK && xml_base != NULL) {
        is_changed = true;
        status = sxml_uri_resolve(&(sxml_uri_resolution_request){.base_uri = base_uri,
                                                                 .reference = xml_base->value,
                                                                 .allocator = reader->allocator},
                                  &resolution);
        base_uri = resolution.uri;
    }
    if (status == SXML_STATUS_OK) {
        status = append_text_bytes(reader, &reader->context_text, base_uri);
    }
    sxml_allocator_release(reader->allocator, resolution.memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }

    sxml_xslt_ast_name_token default_mode =
        has_parent ? parent->default_mode
                   : (sxml_xslt_ast_name_token){.kind = SXML_XSLT_AST_NAME_TOKEN_KIND_UNNAMED,
                                                .name = {.index = 0},
                                                .namespace_uri = {.index = 0},
                                                .has_arity = false,
                                                .arity = 0};
    const element_attribute *mode_attribute = find_standard_value(
        reader, (standard_query){.is_xslt_element = is_xslt, .local_name = "default-mode"});
    if (mode_attribute != NULL) {
        is_changed = true;
        const sxml_byte_view word = sxml_xml_text_trim_whitespace(mode_attribute->value);
        if (sxml_byte_view_equals(word, SXML_BYTE_VIEW_FROM_LITERAL("#unnamed"))) {
            default_mode.kind = SXML_XSLT_AST_NAME_TOKEN_KIND_UNNAMED;
            default_mode.name = (sxml_name_identifier){.index = 0};
        } else {
            eqname_result result = {.name = {.index = 0}, .is_malformed = false,
                                    .is_unbound = false};
            status = resolve_eqname(
                reader,
                &(eqname_request){
                    .text = word,
                    .bindings = {.namespace_bindings = reader->bindings.elements,
                                 .namespace_binding_count = reader->bindings.count},
                    .unprefixed_namespace = {.index = SXML_NAMESPACE_NONE}},
                &result);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            if (result.is_malformed || result.is_unbound) {
                return raise_error(reader,
                                   (reader_error){.error_code = result.is_unbound ? "XTSE0280"
                                                                                  : "XTSE0020",
                                                  .description = "default-mode is neither "
                                                                 "#unnamed nor an EQName (XSLT "
                                                                 "4.0, 3.6.3)",
                                                  .module = request->module,
                                                  .source_span = mode_attribute->source_span});
            }
            default_mode.kind = SXML_XSLT_AST_NAME_TOKEN_KIND_NAME;
            default_mode.name = result.name;
        }
    }
    if (!is_changed) {
        *context = request->parent_context;
        return SXML_STATUS_OK;
    }
    const uint8_t *text = reader->context_text.elements;
    status = sxml_xslt_ast_add_static_context(
        reader->ast,
        &(sxml_xslt_ast_static_context_request){
            .parent = request->parent_context,
            .namespace_bindings = {.namespace_bindings = reader->bindings.elements,
                                   .namespace_binding_count = reader->bindings.count},
            .xpath_default_namespace = xpath_default_namespace,
            .is_any_namespace = is_any_namespace,
            .default_collation = {.bytes = text, .byte_count = collation_byte_count},
            .default_mode = default_mode,
            .default_validation = default_validation,
            .expands_text = expands_text,
            .version = version,
            .excluded_namespaces = {.namespace_uris = reader->excluded_uris.elements,
                                    .namespace_uri_count = reader->excluded_uris.count},
            .extension_namespaces = {.namespace_uris = reader->extension_uris.elements,
                                     .namespace_uri_count = reader->extension_uris.count},
            .static_base_uri = {.bytes = text == NULL ? NULL : text + collation_byte_count,
                                .byte_count = reader->context_text.count - collation_byte_count}},
        context);
    reader->context_text.count = 0;
    return status;
}

/* ------------------------------------------------------- Static expressions */

/* A static expression of an element (9.7). */
typedef struct static_evaluation {
    sxml_byte_view text;
    bool is_value_template;
    /* The element's statically known namespaces and default element
       namespace, and its base URI. */
    sxml_namespace_binding_view bindings;
    sxml_namespace_uri_identifier default_element_namespace;
    sxml_byte_view static_base_uri;
    sxml_xslt_ast_module_index module;
    sxml_ast_source_span source_span;
} static_evaluation;

static sxml_status evaluate_static(stylesheet_reader *reader, const static_evaluation *evaluation,
                                   sxml_sequence *value) {
    reader->evaluation_bindings.count = 0;
    sxml_status status = SXML_STATUS_OK;
    for (size_t index = 0; status == SXML_STATUS_OK && index < evaluation->bindings.namespace_binding_count;
         index += 1) {
        status = append_binding(
            reader, &reader->evaluation_bindings,
            (binding_addition){.binding = evaluation->bindings.namespace_bindings[index],
                               .first_index = 0});
    }
    if (status == SXML_STATUS_OK &&
        evaluation->default_element_namespace.index != SXML_NAMESPACE_NONE) {
        status = append_binding(
            reader, &reader->evaluation_bindings,
            (binding_addition){.binding = {.namespace_prefix = {.index = 0},
                                           .namespace_uri = evaluation->default_element_namespace},
                               .first_index = 0});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_xslt_static_scope_evaluate(
        reader->static_scope,
        &(sxml_xslt_static_expression){
            .expression_text = evaluation->text,
            .is_value_template = evaluation->is_value_template,
            .namespace_bindings = {.namespace_bindings = reader->evaluation_bindings.elements,
                                   .namespace_binding_count = reader->evaluation_bindings.count},
            .static_base_uri = evaluation->static_base_uri,
            .source_name = sxml_xslt_ast_get_module(reader->ast, evaluation->module)->module_uri,
            .error_record = reader->error_record},
        value);
    return relocate_error(reader, (error_relocation){.status = status,
                                                     .module = evaluation->module,
                                                     .source_span = evaluation->source_span});
}

/* The static evaluation of an expression of an element whose static
   context exists. */
typedef struct context_expression {
    sxml_byte_view text;
    sxml_xslt_ast_module_index module;
    sxml_xslt_ast_static_context_index static_context;
    sxml_ast_source_span source_span;
} context_expression;

static sxml_status evaluate_in_context(stylesheet_reader *reader, const context_expression *expression,
                                       sxml_sequence *value) {
    const sxml_xslt_ast_static_context *context =
        sxml_xslt_ast_get_static_context(reader->ast, expression->static_context);
    return evaluate_static(
        reader,
        &(static_evaluation){
            .text = expression->text,
            .is_value_template = false,
            .bindings = sxml_xslt_ast_get_namespace_bindings(reader->ast, expression->static_context),
            .default_element_namespace = context->is_any_namespace
                                             ? (sxml_namespace_uri_identifier){.index = 0}
                                             : context->xpath_default_namespace,
            .static_base_uri = sxml_xslt_ast_get_text(reader->ast, context->static_base_uri),
            .module = expression->module,
            .source_span = expression->source_span},
        value);
}

/* The string of a one-string sequence (a value template's value). */
static sxml_byte_view string_of(sxml_sequence value) {
    sxml_item item = {.representation = 0};
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof(datum));
    if (sxml_sequence_get_item(value, (sxml_item_request){.item_index = 0, .error_record = NULL},
                               &item) != SXML_STATUS_OK ||
        sxml_atomic_item_get_datum(item, &datum) != SXML_STATUS_OK ||
        datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return datum.datum_details.string.utf8;
}

/* The statically known namespaces of an element before its static
   context exists, for its shadow attributes: the module's fixed bindings,
   else its native ones, into reader->bindings. */
static sxml_status gather_bindings(stylesheet_reader *reader, element_source source,
                                   sxml_xslt_ast_module_index module) {
    reader->bindings.count = 0;
    const module_state *state = state_of_module(reader, module);
    sxml_status status = SXML_STATUS_OK;
    if (state->has_fixed_namespaces) {
        const sxml_namespace_binding *fixed = reader->fixed_bindings.elements;
        for (size_t index = 0; status == SXML_STATUS_OK && index < state->fixed_binding_count;
             index += 1) {
            status = append_binding(
                reader, &reader->bindings,
                (binding_addition){.binding = fixed[state->first_fixed_binding + index],
                                   .first_index = 0});
        }
        return status;
    }
    sxml_namespace_binding_view natives = {.namespace_bindings = NULL,
                                           .namespace_binding_count = 0};
    status = native_bindings(source, &natives);
    for (size_t index = 0; status == SXML_STATUS_OK && index < natives.namespace_binding_count;
         index += 1) {
        const sxml_namespace_binding binding = natives.namespace_bindings[index];
        if (binding.namespace_prefix.index != 0 && binding.namespace_uri.index != SXML_NAMESPACE_XML &&
            binding.namespace_uri.index != SXML_NAMESPACE_NONE) {
            status = append_binding(reader, &reader->bindings,
                                    (binding_addition){.binding = binding, .first_index = 0});
        }
    }
    return status;
}

/* Evaluates the shadow attributes of an XSLT element (3.11.4): each "_n"
   in no namespace becomes the attribute n, replacing one written. */
typedef struct shadow_request {
    element_source source;
    sxml_xslt_ast_module_index module;
    /* The parent's static context: default element namespace and base URI
       (the element's own are not known before its shadows are). */
    sxml_xslt_ast_static_context_index parent_context;
} shadow_request;

static sxml_status apply_shadow_attributes(stylesheet_reader *reader, const shadow_request *request) {
    const size_t original_count = reader->attributes.count;
    bool has_shadow = false;
    for (size_t index = 0; index < original_count; index += 1) {
        const element_attribute *attribute = attribute_at(reader, index);
        if (attribute->namespace_uri.index == SXML_NAMESPACE_NONE &&
            attribute->local_name.byte_count > 1 && attribute->local_name.bytes[0] == '_') {
            has_shadow = true;
        }
    }
    if (!has_shadow) {
        return SXML_STATUS_OK;
    }
    sxml_status status = gather_bindings(reader, request->source, request->module);
    const sxml_xslt_ast_static_context *parent =
        sxml_xslt_ast_get_static_context(reader->ast, request->parent_context);
    const sxml_byte_view base_uri =
        request->parent_context.value != 0
            ? sxml_xslt_ast_get_text(reader->ast, parent->static_base_uri)
            : sxml_xslt_ast_get_module(reader->ast, request->module)->module_uri;
    for (size_t index = 0; status == SXML_STATUS_OK && index < original_count; index += 1) {
        const element_attribute shadow = *attribute_at(reader, index);
        if (shadow.namespace_uri.index != SXML_NAMESPACE_NONE || shadow.local_name.byte_count < 2 ||
            shadow.local_name.bytes[0] != '_') {
            continue;
        }
        attribute_at(reader, index)->is_ignored = true;
        const sxml_byte_view target = {.bytes = shadow.local_name.bytes + 1,
                                       .byte_count = shadow.local_name.byte_count - 1};
        sxml_sequence value = sxml_sequence_empty();
        status = evaluate_static(
            reader,
            &(static_evaluation){
                .text = shadow.value,
                .is_value_template = true,
                .bindings = {.namespace_bindings = reader->bindings.elements,
                             .namespace_binding_count = reader->bindings.count},
                .default_element_namespace = request->parent_context.value != 0 &&
                                                     !parent->is_any_namespace
                                                 ? parent->xpath_default_namespace
                                                 : (sxml_namespace_uri_identifier){.index = 0},
                .static_base_uri = base_uri,
                .module = request->module,
                .source_span = shadow.source_span},
            &value);
        if (status != SXML_STATUS_OK) {
            break;
        }
        status = reserve_scratch(reader, &reader->shadow_values,
                                 (scratch_reservation){.element_size = sizeof(sxml_sequence),
                                                       .element_alignment = alignof(sxml_sequence),
                                                       .needed_count =
                                                           reader->shadow_values.count + 1U});
        if (status != SXML_STATUS_OK) {
            sxml_sequence_release(value);
            break;
        }
        sxml_sequence *values = reader->shadow_values.elements;
        values[reader->shadow_values.count] = value;
        reader->shadow_values.count += 1;
        for (size_t other = 0; other < original_count; other += 1) {
            element_attribute *written = attribute_at(reader, other);
            if (written->namespace_uri.index == SXML_NAMESPACE_NONE &&
                sxml_byte_view_equals(written->local_name, target)) {
                written->is_ignored = true;
            }
        }
        sxml_name_identifier name = {.index = 0};
        status = sxml_name_pool_intern_name(
            reader->name_pool,
            &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                                 .local_name = target},
            &name);
        if (status == SXML_STATUS_OK) {
            sxml_name_parts parts = {.namespace_uri = {.index = 0},
                                     .local_name = {.bytes = NULL, .byte_count = 0}};
            /* The pool's copy of the local name outlives the shadow's
               attribute view. */
            status = sxml_name_pool_get_name_parts(reader->name_pool, name, &parts);
            if (status == SXML_STATUS_OK) {
                status = append_attribute(
                    reader, &(element_attribute){.attribute_name = name,
                                                 .namespace_uri = {.index = SXML_NAMESPACE_NONE},
                                                 .namespace_prefix = {.index = 0},
                                                 .local_name = parts.local_name,
                                                 .value = string_of(value),
                                                 .source_span = shadow.source_span,
                                                 .is_shadow = true,
                                                 .is_ignored = false});
            }
        }
    }
    return status;
}

/* ------------------------------------------------------------ Elements */

/* What an element is in its place (after classification). */
typedef enum element_role {
    /* Not read: forwards-compatible ignoring, or excluded. */
    SXML_ELEMENT_ROLE_IGNORED,
    SXML_ELEMENT_ROLE_MODULE_ROOT,
    /* The literal result element of a simplified module (3.7). */
    SXML_ELEMENT_ROLE_SIMPLIFIED_ROOT,
    SXML_ELEMENT_ROLE_XSLT,
    SXML_ELEMENT_ROLE_UNKNOWN_INSTRUCTION,
    SXML_ELEMENT_ROLE_LITERAL_RESULT,
    SXML_ELEMENT_ROLE_EXTENSION_INSTRUCTION,
    SXML_ELEMENT_ROLE_DATA_ELEMENT,
} element_role;

/* One element met by the walk. */
typedef struct element_input {
    size_t parent_index;
    sxml_node_index element;
    const sxml_node_description *description;
} element_input;

/* The facts of the element read so far. */
typedef struct element_facts {
    sxml_name_identifier element_name;
    sxml_namespace_uri_identifier namespace_uri;
    sxml_byte_view local_name;
    bool is_xslt;
    const element_rule *rule;
    element_source source;
    sxml_ast_source_span source_span;
} element_facts;

/* An element in its parent's frame, for errors raised at it. */
typedef struct element_place {
    const open_element *parent;
    const element_facts *facts;
} element_place;

static sxml_status raise_at_element(stylesheet_reader *reader, element_place place,
                                    reader_error error) {
    error.module = place.parent->module;
    error.source_span = place.facts->source_span;
    return raise_error(reader, error);
}

static bool is_extension_namespace(const stylesheet_reader *reader,
                                   sxml_xslt_ast_static_context_index context,
                                   sxml_namespace_uri_identifier uri) {
    const sxml_xslt_ast_namespace_uri_view uris = sxml_xslt_ast_get_namespace_uris(
        reader->ast, sxml_xslt_ast_get_static_context(reader->ast, context)->extension_namespaces);
    for (size_t index = 0; index < uris.namespace_uri_count; index += 1) {
        if (uris.namespace_uris[index].index == uri.index) {
            return true;
        }
    }
    return false;
}

/* A role decision: the role, or an error to raise. */
typedef struct role_decision {
    element_role role;
    const char *error_code;
    const char *description;
} role_decision;

/* An element whose role to decide. */
typedef struct role_request {
    const open_element *parent;
    const element_facts *facts;
    bool is_forwards_compatible;
} role_request;

static role_decision decide_role(const stylesheet_reader *reader, const role_request *request) {
    const open_element *parent = request->parent;
    const element_facts *facts = request->facts;
    const bool is_forwards_compatible = request->is_forwards_compatible;
    const element_rule *rule = facts->rule;
    const role_decision misplaced = {
        .role = SXML_ELEMENT_ROLE_IGNORED,
        .error_code = "XTSE0010",
        .description = "An XSLT element is not allowed where it occurs (XSLT 4.0, element "
                       "syntax)"};
    const role_decision ignored = {.role = SXML_ELEMENT_ROLE_IGNORED, .error_code = NULL,
                                   .description = NULL};
    if (parent->child_context != SXML_CHILD_CONTEXT_DECLARATIONS && rule != NULL &&
        (rule->kind == SXML_XSLT_AST_KIND_INCLUDE || rule->kind == SXML_XSLT_AST_KIND_IMPORT)) {
        return (role_decision){
            .role = SXML_ELEMENT_ROLE_IGNORED,
            .error_code = rule->kind == SXML_XSLT_AST_KIND_INCLUDE ? "XTSE0170" : "XTSE0190",
            .description = "xsl:include and xsl:import are top-level only (XSLT 4.0, 3.10.2, "
                           "3.10.3)"};
    }
    switch (parent->child_context) {
    case SXML_CHILD_CONTEXT_DOCUMENT:
        if (facts->is_xslt) {
            if (rule != NULL && (rule->element_flags & SXML_ELEMENT_FLAG_MODULE_ROOT) != 0) {
                return (role_decision){.role = SXML_ELEMENT_ROLE_MODULE_ROOT, .error_code = NULL,
                                       .description = NULL};
            }
            /* 3.7 (4.0): an instruction may be the body of a simplified
               module. */
            if (rule != NULL && (rule->element_flags & SXML_ELEMENT_FLAG_INSTRUCTION) != 0) {
                return (role_decision){.role = SXML_ELEMENT_ROLE_XSLT, .error_code = NULL,
                                       .description = NULL};
            }
            return misplaced;
        }
        return (role_decision){.role = SXML_ELEMENT_ROLE_SIMPLIFIED_ROOT, .error_code = NULL,
                               .description = NULL};
    case SXML_CHILD_CONTEXT_DECLARATIONS:
        if (!facts->is_xslt) {
            if (facts->namespace_uri.index == SXML_NAMESPACE_NONE) {
                return (role_decision){.role = SXML_ELEMENT_ROLE_IGNORED,
                                       .error_code = "XTSE0130",
                                       .description = "A top-level element is in no namespace "
                                                      "(XSLT 4.0, 3.6.4)"};
            }
            return (role_decision){.role = SXML_ELEMENT_ROLE_DATA_ELEMENT, .error_code = NULL,
                                   .description = NULL};
        }
        if (rule != NULL && ((rule->element_flags & SXML_ELEMENT_FLAG_DECLARATION) != 0 ||
                             (parent->rule != NULL && rule_lists_child(parent->rule, rule->kind)))) {
            return (role_decision){.role = SXML_ELEMENT_ROLE_XSLT, .error_code = NULL,
                                   .description = NULL};
        }
        /* 3.9: unknown top-level elements are ignored in
           forwards-compatible mode. */
        return is_forwards_compatible ? ignored : misplaced;
    case SXML_CHILD_CONTEXT_SEQUENCE_CONSTRUCTOR:
    case SXML_CHILD_CONTEXT_RULE: {
        /* The rule of a RULE frame (which always has its element's rule);
           NULL for a sequence constructor frame. */
        const element_rule *parent_rule =
            parent->child_context == SXML_CHILD_CONTEXT_RULE ? parent->rule : NULL;
        if (parent->child_context == SXML_CHILD_CONTEXT_RULE && parent_rule == NULL) {
            return misplaced;
        }
        if (parent_rule != NULL && facts->is_xslt && rule != NULL &&
            rule_lists_child(parent_rule, rule->kind)) {
            return (role_decision){.role = SXML_ELEMENT_ROLE_XSLT, .error_code = NULL,
                                   .description = NULL};
        }
        if (parent_rule != NULL &&
            parent_rule->content_model != SXML_CONTENT_MODEL_CHILDREN_THEN_SEQUENCE_CONSTRUCTOR &&
            parent_rule->content_model != SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR_THEN_CHILDREN) {
            if (parent_rule->content_model == SXML_CONTENT_MODEL_EMPTY) {
                return (role_decision){.role = SXML_ELEMENT_ROLE_IGNORED,
                                       .error_code = "XTSE0260",
                                       .description = "An XSLT element that must be empty has "
                                                      "content (XSLT 4.0, 3.11.1)"};
            }
            return is_forwards_compatible ? ignored : misplaced;
        }
        if (!facts->is_xslt) {
            return (role_decision){
                .role = is_extension_namespace(reader, parent->static_context, facts->namespace_uri)
                            ? SXML_ELEMENT_ROLE_EXTENSION_INSTRUCTION
                            : SXML_ELEMENT_ROLE_LITERAL_RESULT,
                .error_code = NULL,
                .description = NULL};
        }
        if (rule != NULL && (rule->element_flags & SXML_ELEMENT_FLAG_INSTRUCTION) != 0) {
            return (role_decision){.role = SXML_ELEMENT_ROLE_XSLT, .error_code = NULL,
                                   .description = NULL};
        }
        if (is_forwards_compatible) {
            return (role_decision){.role = SXML_ELEMENT_ROLE_UNKNOWN_INSTRUCTION, .error_code = NULL,
                                   .description = NULL};
        }
        return misplaced;
    }
    case SXML_CHILD_CONTEXT_FALLBACK_ONLY:
        if (facts->is_xslt && rule != NULL && rule->kind == SXML_XSLT_AST_KIND_FALLBACK) {
            return (role_decision){.role = SXML_ELEMENT_ROLE_XSLT, .error_code = NULL,
                                   .description = NULL};
        }
        return ignored;
    case SXML_CHILD_CONTEXT_SKIPPED:
        return ignored;
    }
    return ignored;
}

/* True when an element has content other than comments, processing
   instructions, xsl:note and whitespace that stripping removes. */
typedef struct content_query {
    element_source source;
    bool preserves_space;
} content_query;

/* Whether an element has content (has_content). */
typedef struct content_presence {
    bool has_content;
} content_presence;

static sxml_status has_content(content_query query, content_presence *presence) {
    bool has = false;
    sxml_node_description description;
    sxml_status status = describe(query.source.tree, query.source.element, &description);
    bool has_child = status == SXML_STATUS_OK && description.has_children;
    sxml_node_index child = description.first_child;
    while (status == SXML_STATUS_OK && has_child) {
        sxml_node_description child_description;
        status = describe(query.source.tree, child, &child_description);
        if (status != SXML_STATUS_OK) {
            break;
        }
        sxml_name_parts parts = {.namespace_uri = {.index = 0},
                                 .local_name = {.bytes = NULL, .byte_count = 0}};
        if (child_description.node_kind == SXML_NODE_KIND_ELEMENT) {
            /* xsl:note is removed before the check (3.11.1, 3.11.2). */
            const bool is_note =
                sxml_name_pool_get_name_parts(query.source.tree->name_pool,
                                              child_description.node_name, &parts) ==
                    SXML_STATUS_OK &&
                parts.namespace_uri.index == SXML_NAMESPACE_XSLT &&
                sxml_byte_view_equals(parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("note"));
            has = has || !is_note;
        } else if (child_description.node_kind == SXML_NODE_KIND_TEXT) {
            sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
            status = sxml_node_get_content(tree_node(query.source.tree, child), &content);
            if (status == SXML_STATUS_OK && (!sxml_xml_text_is_whitespace(content) || query.preserves_space)) {
                has = true;
            }
        }
        has_child = child_description.has_next_sibling;
        child = child_description.next_sibling;
    }
    presence->has_content = has;
    return status;
}

/* xml:space on the element (3.11.1): preserve or default change what
   its children inherit. */
/* What the parent's frame preserves. */
typedef struct space_inheritance {
    bool preserves_space;
} space_inheritance;

static bool preserves_space_of(const stylesheet_reader *reader, space_inheritance inheritance) {
    const bool inherited = inheritance.preserves_space;
    const element_attribute *space = find_element_attribute(
        reader, (attribute_query){.namespace_uri = {.index = SXML_NAMESPACE_XML},
                                  .local_name = SXML_BYTE_VIEW_FROM_LITERAL("space")});
    if (space == NULL) {
        return inherited;
    }
    const sxml_byte_view word = sxml_xml_text_trim_whitespace(space->value);
    if (sxml_byte_view_equals(word, SXML_BYTE_VIEW_FROM_LITERAL("preserve"))) {
        return true;
    }
    if (sxml_byte_view_equals(word, SXML_BYTE_VIEW_FROM_LITERAL("default"))) {
        return false;
    }
    return inherited;
}

/* The attributes of a new node: of an XSLT element by its rule, of a
   literal result element as value templates, of other elements as
   foreign text. */
typedef struct node_attributes_request {
    const open_element *parent;
    const element_facts *facts;
    element_role role;
    sxml_xslt_ast_node_index node;
    sxml_xslt_ast_static_context_index static_context;
    bool is_forwards_compatible;
} node_attributes_request;

/* One attribute of xsl:record (D142). */
typedef struct record_attribute_request {
    const element_attribute *attribute;
    sxml_xslt_ast_module_index module;
    bool is_forwards_compatible;
} record_attribute_request;

/* What reading it gives: the attribute's kind (NONE to drop it) and
   value in *partial, and the rule its value is read by. */
typedef struct record_attribute_reading {
    sxml_xslt_ast_attribute_request *partial;
    const attribute_rule **rule;
} record_attribute_reading;

/* An attribute of xsl:record: in no namespace a field (an expression);
   xsl:duplicates; the XSLT-namespaced standard attributes (TEXT, their
   effect in the static context); in other namespaces foreign text. */
static sxml_status classify_record_attribute(stylesheet_reader *reader,
                                             const record_attribute_request *request,
                                             record_attribute_reading reading) {
    static const attribute_rule field_rule = {
        .attribute_name = SXML_BYTE_VIEW_INITIALIZER(""),
        .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_RECORD_FIELD,
        .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION,
        .is_required = false,
        .keyword_mask = 0};
    static const attribute_rule duplicates_rule = {
        .attribute_name = SXML_BYTE_VIEW_INITIALIZER("duplicates"),
        .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_DUPLICATES,
        .value_syntax = SXML_VALUE_SYNTAX_EXPRESSION,
        .is_required = false,
        .keyword_mask = 0};
    const element_attribute *attribute = request->attribute;
    sxml_xslt_ast_attribute_request *partial = reading.partial;
    if (attribute->namespace_uri.index == SXML_NAMESPACE_NONE) {
        partial->kind = SXML_XSLT_AST_ATTRIBUTE_RECORD_FIELD;
        *reading.rule = &field_rule;
        return SXML_STATUS_OK;
    }
    if (attribute->namespace_uri.index != SXML_NAMESPACE_XSLT) {
        partial->kind = SXML_XSLT_AST_ATTRIBUTE_FOREIGN;
        return read_text_value(reader, attribute->value, &partial->value);
    }
    partial->flags |= SXML_XSLT_AST_ATTRIBUTE_FLAG_XSLT_NAMESPACE;
    if (sxml_byte_view_equals(attribute->local_name, duplicates_rule.attribute_name)) {
        partial->kind = SXML_XSLT_AST_ATTRIBUTE_DUPLICATES;
        *reading.rule = &duplicates_rule;
        return SXML_STATUS_OK;
    }
    const standard_attribute *standard = find_standard_attribute(attribute->local_name);
    if (standard != NULL && standard->attribute_kind != SXML_XSLT_AST_ATTRIBUTE_FIXED_NAMESPACES) {
        partial->kind = standard->attribute_kind;
        return read_text_value(reader, attribute->value, &partial->value);
    }
    if (request->is_forwards_compatible) {
        partial->kind = SXML_XSLT_AST_ATTRIBUTE_NONE;
        return SXML_STATUS_OK;
    }
    return raise_error(reader, (reader_error){.error_code = "XTSE0090",
                                              .description = "An attribute in the XSLT "
                                                             "namespace is not allowed on "
                                                             "xsl:record (XSLT 4.0, 3.3)",
                                              .module = request->module,
                                              .source_span = attribute->source_span});
}

/* XTSE1505 (XSLT 4.0, 25): [xsl:]type and [xsl:]validation are mutually
   exclusive. With src/xslt_schema.c's imports this is what the reader
   checks of schema awareness (D179); lowering resolves the types. */
typedef struct schema_awareness_check {
    sxml_xslt_ast_node_index node;
    sxml_xslt_ast_module_index module;
} schema_awareness_check;

static sxml_status check_type_and_validation(stylesheet_reader *reader,
                                             schema_awareness_check check) {
    const sxml_xslt_ast_attribute_index type = sxml_xslt_ast_find_attribute(
        reader->ast,
        (sxml_xslt_ast_attribute_query){.node = check.node, .kind = SXML_XSLT_AST_ATTRIBUTE_TYPE});
    if (type.value == 0 ||
        sxml_xslt_ast_find_attribute(reader->ast, (sxml_xslt_ast_attribute_query){
                                                      .node = check.node,
                                                      .kind = SXML_XSLT_AST_ATTRIBUTE_VALIDATION})
                .value == 0) {
        return SXML_STATUS_OK;
    }
    return raise_error(reader, (reader_error){.error_code = "XTSE1505",
                                              .description = "Both a type and a validation "
                                                             "attribute (XSLT 4.0, 25)",
                                              .module = check.module,
                                              .source_span = sxml_xslt_ast_get_attribute(
                                                                 reader->ast, type)
                                                                 ->source_span});
}

static sxml_status add_node_attributes(stylesheet_reader *reader, const node_attributes_request *request) {
    const element_facts *facts = request->facts;
    const bool is_rule_element = request->role == SXML_ELEMENT_ROLE_XSLT ||
                                 request->role == SXML_ELEMENT_ROLE_MODULE_ROOT;
    for (size_t index = 0; index < reader->attributes.count; index += 1) {
        const element_attribute attribute = *attribute_at(reader, index);
        if (attribute.is_ignored) {
            continue;
        }
        sxml_xslt_ast_attribute_request partial = {
            .node = request->node,
            .kind = SXML_XSLT_AST_ATTRIBUTE_FOREIGN,
            .flags = attribute.is_shadow ? (uint32_t)SXML_XSLT_AST_ATTRIBUTE_FLAG_SHADOW : 0U,
            .attribute_name = attribute.attribute_name,
            .namespace_prefix = attribute.namespace_prefix,
            .value = {.kind = SXML_XSLT_AST_VALUE_KIND_NONE},
            .source_span = attribute.source_span};
        const attribute_rule *rule = NULL;
        sxml_status status = SXML_STATUS_OK;
        if (is_rule_element && facts->rule->kind == SXML_XSLT_AST_KIND_RECORD) {
            status = classify_record_attribute(
                reader, &(record_attribute_request){.attribute = &attribute,
                                                    .module = request->parent->module,
                                                    .is_forwards_compatible =
                                                        request->is_forwards_compatible},
                (record_attribute_reading){.partial = &partial, .rule = &rule});
            if (status == SXML_STATUS_OK && partial.kind == SXML_XSLT_AST_ATTRIBUTE_NONE) {
                continue;
            }
        } else if (is_rule_element) {
            const bool is_unprefixed = attribute.namespace_uri.index == SXML_NAMESPACE_NONE;
            const standard_attribute *standard =
                is_unprefixed ? find_standard_attribute(attribute.local_name) : NULL;
            rule = is_unprefixed ? find_attribute_rule(facts->rule, attribute.local_name) : NULL;
            if (rule == NULL && standard != NULL &&
                (standard->attribute_kind != SXML_XSLT_AST_ATTRIBUTE_FIXED_NAMESPACES ||
                 request->role == SXML_ELEMENT_ROLE_MODULE_ROOT)) {
                partial.kind = standard->attribute_kind;
                status = read_text_value(reader, attribute.value, &partial.value);
            } else if (rule != NULL) {
                partial.kind = rule->attribute_kind;
            } else if (is_unprefixed || attribute.namespace_uri.index == SXML_NAMESPACE_XSLT) {
                if (request->is_forwards_compatible) {
                    continue;
                }
                return raise_error(reader,
                                   (reader_error){.error_code = "XTSE0090",
                                                  .description = "An attribute is not allowed "
                                                                 "on this XSLT element (XSLT "
                                                                 "4.0, 3.3)",
                                                  .module = request->parent->module,
                                                  .source_span = attribute.source_span});
            } else {
                /* Other namespaces (3.3), and xml:space, xml:base,
                   xml:lang: kept as text. */
                status = read_text_value(reader, attribute.value, &partial.value);
            }
        } else if (request->role == SXML_ELEMENT_ROLE_LITERAL_RESULT ||
                   request->role == SXML_ELEMENT_ROLE_SIMPLIFIED_ROOT) {
            if (attribute.namespace_uri.index == SXML_NAMESPACE_XSLT) {
                const standard_attribute *standard = find_standard_attribute(attribute.local_name);
                partial.flags |= SXML_XSLT_AST_ATTRIBUTE_FLAG_XSLT_NAMESPACE;
                if (standard != NULL &&
                    standard->attribute_kind != SXML_XSLT_AST_ATTRIBUTE_FIXED_NAMESPACES) {
                    partial.kind = standard->attribute_kind;
                    status = read_text_value(reader, attribute.value, &partial.value);
                } else {
                    for (size_t rule_index = 0;
                         rule_index <
                         sizeof(literal_result_attribute_rules) / sizeof(literal_result_attribute_rules[0]);
                         rule_index += 1) {
                        if (sxml_byte_view_equals(literal_result_attribute_rules[rule_index].attribute_name,
                                                  attribute.local_name)) {
                            rule = &literal_result_attribute_rules[rule_index];
                        }
                    }
                    if (rule == NULL) {
                        if (request->is_forwards_compatible) {
                            continue;
                        }
                        return raise_error(
                            reader, (reader_error){.error_code = "XTSE0805",
                                                   .description = "An attribute in the XSLT "
                                                                  "namespace is not allowed on a "
                                                                  "literal result element (XSLT "
                                                                  "4.0, 11.1.2)",
                                                   .module = request->parent->module,
                                                   .source_span = attribute.source_span});
                    }
                    partial.kind = rule->attribute_kind;
                }
            } else {
                /* 11.1.2: an attribute value template. */
                partial.kind = SXML_XSLT_AST_ATTRIBUTE_LITERAL_RESULT;
                static const attribute_rule literal_rule = {
                    .attribute_name = SXML_BYTE_VIEW_INITIALIZER(""),
                    .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_LITERAL_RESULT,
                    .value_syntax = SXML_VALUE_SYNTAX_VALUE_TEMPLATE,
                    .is_required = false,
                    .keyword_mask = 0};
                rule = &literal_rule;
            }
        } else {
            status = read_text_value(reader, attribute.value, &partial.value);
        }
        if (status == SXML_STATUS_OK && rule != NULL) {
            status = read_value(reader,
                                &(value_request){.rule = rule,
                                                 .attribute = &attribute,
                                                 .module = request->parent->module,
                                                 .static_context = request->static_context,
                                                 .node = request->node,
                                                 .source = facts->source},
                                &partial.value);

        }
        if (status == SXML_STATUS_OK) {
            sxml_xslt_ast_attribute_index added = {.value = 0};
            status = sxml_xslt_ast_add_attribute(reader->ast, &partial, &added);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_status checked = check_type_and_validation(
        reader, (schema_awareness_check){.node = request->node, .module = request->parent->module});
    if (checked != SXML_STATUS_OK) {
        return checked;
    }
    /* Required attributes (element syntax summaries; XTSE0010). */
    if (is_rule_element) {
        for (uint32_t offset = 0; offset < facts->rule->attribute_rule_count; offset += 1) {
            const attribute_rule *rule = &attribute_rules[facts->rule->first_attribute_rule + offset];
            if (rule->is_required &&
                find_element_attribute(reader, (attribute_query){
                                                   .namespace_uri = {.index = SXML_NAMESPACE_NONE},
                                                   .local_name = rule->attribute_name}) == NULL) {
                return raise_error(reader,
                                   (reader_error){.error_code = "XTSE0010",
                                                  .description = "A required attribute is "
                                                                 "missing (XSLT 4.0, element "
                                                                 "syntax)",
                                                  .module = request->parent->module,
                                                  .source_span = facts->source_span});
            }
        }
    }
    return SXML_STATUS_OK;
}

/* ---------------------------------------------- Static variables (9.6) */

/* A top-level xsl:variable or xsl:param just added. */
typedef struct declaration_input {
    const open_element *parent;
    const element_facts *facts;
    sxml_xslt_ast_node_index node;
    sxml_xslt_ast_static_context_index static_context;
    /* Set when the declaration is static: its frame then requires no
       children. */
    bool *is_static;
} declaration_input;

static const element_attribute *find_unprefixed(const stylesheet_reader *reader,
                                                sxml_byte_view local_name) {
    return find_element_attribute(
        reader, (attribute_query){.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                                  .local_name = local_name});
}

/* A static value to coerce to the declaration's as attribute, raising
   error_code (static text) when it does not match. */
typedef struct static_value_coercion {
    const declaration_input *input;
    const char *error_code;
} static_value_coercion;

static sxml_status coerce_static_value(stylesheet_reader *reader, static_value_coercion coercion,
                                       sxml_sequence *value) {
    const declaration_input *input = coercion.input;
    const char *error_code = coercion.error_code;
    const element_attribute *as_attribute = find_unprefixed(reader, SXML_BYTE_VIEW_FROM_LITERAL("as"));
    if (as_attribute == NULL) {
        return SXML_STATUS_OK;
    }
    sxml_sequence_type declared_type = {.item_type = {.index = 0},
                                        .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};
    sxml_status status = sxml_xpath_parse_sequence_type(
        &(sxml_sequence_type_parse_request){
            .processor = reader->processor,
            .sequence_type_text = as_attribute->value,
            .source_name = sxml_xslt_ast_get_module(reader->ast, input->parent->module)->module_uri,
            .static_context = {.namespace_bindings = sxml_xslt_ast_get_namespace_bindings(
                                   reader->ast, input->static_context)},
            .error_record = reader->error_record},
        &declared_type);
    sxml_sequence coerced = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_static_scope_coerce(
            reader->static_scope,
            &(sxml_xslt_static_coercion){.value = *value,
                                         .declared_type = declared_type,
                                         .error_code = sxml_byte_view_from_c_string(error_code),
                                         .error_record = reader->error_record},
            &coerced);
    }
    if (status != SXML_STATUS_OK) {
        return relocate_error(reader, (error_relocation){.status = status,
                                                         .module = input->parent->module,
                                                         .source_span = as_attribute->source_span});
    }
    sxml_sequence_release(*value);
    *value = coerced;
    return SXML_STATUS_OK;
}

static sxml_status read_static_declaration(stylesheet_reader *reader, const declaration_input *input) {
    const sxml_xslt_ast *ast = reader->ast;
    const sxml_xslt_ast_attribute *static_attribute = sxml_xslt_ast_get_attribute(
        ast, sxml_xslt_ast_find_attribute(ast, (sxml_xslt_ast_attribute_query){
                                                   .node = input->node,
                                                   .kind = SXML_XSLT_AST_ATTRIBUTE_STATIC}));
    const sxml_xslt_ast_attribute *required_attribute = sxml_xslt_ast_get_attribute(
        ast, sxml_xslt_ast_find_attribute(ast, (sxml_xslt_ast_attribute_query){
                                                   .node = input->node,
                                                   .kind = SXML_XSLT_AST_ATTRIBUTE_REQUIRED}));
    if (required_attribute->value.kind == SXML_XSLT_AST_VALUE_KIND_BOOLEAN &&
        required_attribute->value.payload.is_true) {
        /* 9.2: a required parameter has no default value. */
        content_presence presence = {.has_content = find_unprefixed(
                                         reader, SXML_BYTE_VIEW_FROM_LITERAL("select")) != NULL};
        if (!presence.has_content) {
            const sxml_status content_status = has_content(
                (content_query){.source = input->facts->source, .preserves_space = false},
                &presence);
            if (content_status != SXML_STATUS_OK) {
                return content_status;
            }
        }
        if (presence.has_content) {
            const bool is_function_parameter = input->parent->rule != NULL &&
                                               input->parent->rule->kind == SXML_XSLT_AST_KIND_FUNCTION;
            return raise_error(reader, (reader_error){.error_code = is_function_parameter
                                                                        ? "XTSE0760"
                                                                        : "XTSE0010",
                                                      .description = "A required parameter has "
                                                                     "a default value (XSLT 4.0, "
                                                                     "9.2)",
                                                      .module = input->parent->module,
                                                      .source_span = input->facts->source_span});
        }
    }
    if (static_attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_BOOLEAN ||
        !static_attribute->value.payload.is_true) {
        return SXML_STATUS_OK;
    }
    const reader_error misplaced = {.error_code = "XTSE0010",
                                    .description = "A static variable or parameter is not "
                                                   "top-level, has content or lacks select "
                                                   "(XSLT 4.0, 9.6)",
                                    .module = input->parent->module,
                                    .source_span = input->facts->source_span};
    sxml_status status = SXML_STATUS_OK;
    if (input->parent->child_context != SXML_CHILD_CONTEXT_DECLARATIONS) {
        return raise_error(reader, misplaced);
    }
    *input->is_static = true;
    const sxml_xslt_ast_attribute *name_attribute = sxml_xslt_ast_get_attribute(
        ast, sxml_xslt_ast_find_attribute(ast, (sxml_xslt_ast_attribute_query){
                                                   .node = input->node,
                                                   .kind = SXML_XSLT_AST_ATTRIBUTE_NAME}));
    const sxml_name_identifier name = name_attribute->value.payload.name;
    const bool is_parameter = input->facts->rule->kind == SXML_XSLT_AST_KIND_PARAM;
    const element_attribute *select = find_unprefixed(reader, SXML_BYTE_VIEW_FROM_LITERAL("select"));
    sxml_sequence value = sxml_sequence_empty();
    bool is_supplied = false;
    for (size_t index = 0; is_parameter && index < reader->static_parameters.variable_binding_count;
         index += 1) {
        const sxml_variable_binding *binding = &reader->static_parameters.variable_bindings[index];
        if (binding->variable_name.index == name.index) {
            value = binding->value;
            sxml_sequence_retain(value);
            is_supplied = true;
        }
    }
    if (!is_supplied) {
        const sxml_xslt_ast_attribute *required = sxml_xslt_ast_get_attribute(
            ast, sxml_xslt_ast_find_attribute(ast, (sxml_xslt_ast_attribute_query){
                                                       .node = input->node,
                                                       .kind = SXML_XSLT_AST_ATTRIBUTE_REQUIRED}));
        if (is_parameter && required->value.kind == SXML_XSLT_AST_VALUE_KIND_BOOLEAN &&
            required->value.payload.is_true) {
            (void)raise_error(reader, (reader_error){.error_code = "XTDE0050",
                                               .description = "A required static parameter has "
                                                              "no value (XSLT 4.0, 9.6)",
                                               .module = input->parent->module,
                                               .source_span = input->facts->source_span});
            return SXML_STATUS_DYNAMIC_ERROR;
        }
        if (select != NULL) {
            status = evaluate_in_context(reader,
                                         &(context_expression){.text = select->value,
                                                               .module = input->parent->module,
                                                               .static_context =
                                                                   input->static_context,
                                                               .source_span = select->source_span},
                                         &value);
        } else if (!is_parameter) {
            return raise_error(reader, misplaced);
        } else if (find_unprefixed(reader, SXML_BYTE_VIEW_FROM_LITERAL("as")) == NULL) {
            /* 9.3: no select and no content is the zero-length string. */
            status = sxml_function_library_make_string(
                sxml_processor_get_item_heap(reader->processor),
                (sxml_byte_view){.bytes = NULL, .byte_count = 0}, &value);
        }
    }
    if (status == SXML_STATUS_OK) {
        /* 9.3: a parameter without default whose type does not allow the
           empty sequence is XTDE0700 when no value is supplied. */
        const char *code = is_supplied              ? "XTTE0590"
                           : is_parameter && select == NULL ? "XTDE0700"
                                                            : "XTTE0570";
        status = coerce_static_value(reader, (static_value_coercion){.input = input, .error_code = code}, &value);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_static_scope_add_variable(
            reader->static_scope,
            &(sxml_xslt_static_variable){.variable_name = name, .value = value});
    }
    sxml_sequence_release(value);
    return status;
}

/* ------------------------------------------- Includes and imports (3.10) */

typedef struct module_reference {
    const open_element *parent;
    const element_facts *facts;
    sxml_xslt_ast_node_index node;
    sxml_xslt_ast_static_context_index static_context;
    sxml_xslt_ast_module_relation relation;
} module_reference;

/* What a reference to a module whose URI is already read means. */
typedef enum reference_outcome {
    SXML_REFERENCE_OUTCOME_READ,
    /* An include of a module of the same stylesheet level, or an include
       cycle: ignored (3.10.2). */
    SXML_REFERENCE_OUTCOME_IGNORED,
    /* A module that imports itself, directly or not (XTSE0210). */
    SXML_REFERENCE_OUTCOME_CYCLE,
} reference_outcome;

typedef struct reference_check {
    sxml_xslt_ast_module_index module;
    sxml_xslt_ast_module_relation relation;
    sxml_byte_view uri;
} reference_check;

static reference_outcome check_reference(const stylesheet_reader *reader, reference_check check) {
    const sxml_xslt_ast *ast = reader->ast;
    /* The modules being read: the path from this one up. */
    bool is_include_path = check.relation == SXML_XSLT_AST_MODULE_RELATION_INCLUDED;
    uint32_t current = check.module.value;
    while (current != 0) {
        const sxml_xslt_ast_module *module =
            sxml_xslt_ast_get_module(ast, (sxml_xslt_ast_module_index){.value = current});
        if (sxml_byte_view_equals(module->module_uri, check.uri)) {
            return is_include_path ? SXML_REFERENCE_OUTCOME_IGNORED : SXML_REFERENCE_OUTCOME_CYCLE;
        }
        if (module->relation != SXML_XSLT_AST_MODULE_RELATION_INCLUDED) {
            is_include_path = false;
        }
        current = module->parent_module.value;
    }
    if (check.relation == SXML_XSLT_AST_MODULE_RELATION_INCLUDED) {
        const uint32_t level = level_of_module(reader, check.module.value);
        for (uint32_t other = 1; other < ast->module_count; other += 1) {
            if (level_of_module(reader, other) == level &&
                sxml_byte_view_equals(
                    sxml_xslt_ast_get_module(ast, (sxml_xslt_ast_module_index){.value = other})
                        ->module_uri,
                    check.uri)) {
                return SXML_REFERENCE_OUTCOME_IGNORED;
            }
        }
    }
    return SXML_REFERENCE_OUTCOME_READ;
}

static sxml_status raise_unreadable_module(stylesheet_reader *reader, const module_reference *reference,
                                           const element_attribute *href) {
    return raise_error(reader, (reader_error){.error_code = "XTSE0165",
                                              .description = "The stylesheet module an "
                                                             "xsl:include or xsl:import names "
                                                             "cannot be read (XSLT 4.0, 3.10.1)",
                                              .module = reference->parent->module,
                                              .source_span = href->source_span});
}

static sxml_status read_module_reference(stylesheet_reader *reader, const module_reference *reference) {
    const element_attribute *href = find_unprefixed(reader, SXML_BYTE_VIEW_FROM_LITERAL("href"));
    if (href == NULL) {
        return SXML_STATUS_OK;
    }
    const sxml_byte_view reference_text = sxml_xml_text_trim_whitespace(href->value);
    const sxml_xslt_ast_static_context *context =
        sxml_xslt_ast_get_static_context(reader->ast, reference->static_context);
    /* The base URI is copied: the AST's text store may move below. */
    reader->context_text.count = 0;
    sxml_status status = append_text_bytes(
        reader, &reader->context_text, sxml_xslt_ast_get_text(reader->ast, context->static_base_uri));
    const sxml_byte_view base_uri = {.bytes = reader->context_text.elements,
                                     .byte_count = reader->context_text.count};
    sxml_uri_resolution resolution = {.uri = {.bytes = NULL, .byte_count = 0}, .memory = NULL};
    if (status == SXML_STATUS_OK) {
        status = sxml_uri_resolve(&(sxml_uri_resolution_request){.base_uri = base_uri,
                                                                 .reference = reference_text,
                                                                 .allocator = reader->allocator},
                                  &resolution);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_byte_view absolute_uri = resolution.uri;
    const reference_outcome outcome = check_reference(
        reader, (reference_check){.module = reference->parent->module,
                                  .relation = reference->relation,
                                  .uri = absolute_uri});
    if (outcome != SXML_REFERENCE_OUTCOME_READ) {
        sxml_allocator_release(reader->allocator, resolution.memory);
        if (outcome == SXML_REFERENCE_OUTCOME_CYCLE) {
            return raise_error(reader,
                               (reader_error){.error_code = "XTSE0210",
                                              .description = "A stylesheet module imports "
                                                             "itself, directly or indirectly "
                                                             "(XSLT 4.0, 3.10.3)",
                                              .module = reference->parent->module,
                                              .source_span = href->source_span});
        }
        return SXML_STATUS_OK;
    }
    const sxml_resource_resolver resolver = reader->resource_resolver.operations == NULL
                                                ? reader->processor->resource_resolver
                                                : reader->resource_resolver;
    sxml_resource resource;
    memset(&resource, 0, sizeof(resource));
    bool is_resolved = false;
    if (resolver.operations != NULL && resolver.operations->resolve != NULL) {
        status = resolver.operations->resolve(
            resolver.state,
            &(sxml_resource_request){.resource_kind = SXML_RESOURCE_KIND_XSLT_MODULE,
                                     .uri_reference = reference_text,
                                     .absolute_uri = absolute_uri,
                                     .processor = reader->processor,
                                     .encoding_name = {.bytes = NULL, .byte_count = 0},
                                     .is_default_collection = false,
                                     .target_namespace_uri = {.bytes = NULL, .byte_count = 0},
                                     .package_version = {.bytes = NULL, .byte_count = 0}},
            &resource);
        is_resolved = status == SXML_STATUS_OK;
    }
    sxml_item document = {.representation = 0};
    bool has_document = false;
    if (status == SXML_STATUS_OK && resource.resource_form == SXML_RESOURCE_FORM_BYTES) {
        const sxml_byte_view module_uri =
            resource.resource_uri.byte_count > 0 ? resource.resource_uri : absolute_uri;
        status = parse_module(reader,
                              (module_text){.content = resource.content,
                                            .encoding_name = resource.encoding_name,
                                            .module_uri = module_uri},
                              &document);
        has_document = status == SXML_STATUS_OK;
        if (has_document) {
            status = start_module(reader, &(module_start){.relation = reference->relation,
                                                          .parent_module = reference->parent->module,
                                                          .referencing_node = reference->node,
                                                          .document = document,
                                                          .module_uri = module_uri});
        } else if (status != SXML_STATUS_OUT_OF_MEMORY && status != SXML_STATUS_LIMIT_EXCEEDED) {
            status = raise_unreadable_module(reader, reference, href);
        }
    } else if (status == SXML_STATUS_OK && resource.resource_form != SXML_RESOURCE_FORM_NONE) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    } else if (status == SXML_STATUS_OK) {
        status = raise_unreadable_module(reader, reference, href);
    }
    if (has_document) {
        sxml_item_release(document);
    }
    if (is_resolved && resolver.operations->release != NULL) {
        resolver.operations->release(resolver.state, &resource);
    }
    sxml_allocator_release(reader->allocator, resolution.memory);
    return status;
}

/* --------------------------------------------- Used packages (3.4.2) */

/* The text of attribute kind on node (TEXT values), empty without. */
static sxml_byte_view node_text_of(const stylesheet_reader *reader, sxml_xslt_ast_node_index node,
                                   sxml_xslt_ast_attribute_kind kind) {
    const sxml_xslt_ast_attribute *attribute = sxml_xslt_ast_get_attribute(
        reader->ast,
        sxml_xslt_ast_find_attribute(reader->ast,
                                     (sxml_xslt_ast_attribute_query){.node = node, .kind = kind}));
    if (attribute->value.kind != SXML_XSLT_AST_VALUE_KIND_TEXT) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return sxml_xml_text_trim_whitespace(sxml_xslt_ast_get_text(reader->ast, attribute->value.payload.text));
}

/* The package a use-package asks for: its name and version range. */
typedef struct package_identity {
    sxml_byte_view package_name;
    sxml_byte_view version_range;
} package_identity;

/* A used package already read with the same name and version range: its
   document (3.4.2: a package used twice is read once per compilation);
   {0} when none. */
static sxml_item find_read_package(const stylesheet_reader *reader, package_identity identity) {
    const sxml_xslt_ast *ast = reader->ast;
    for (uint32_t module_index = 1; module_index < ast->module_count; module_index += 1) {
        const sxml_xslt_ast_module *module =
            sxml_xslt_ast_get_module(ast, (sxml_xslt_ast_module_index){.value = module_index});
        if (module->relation != SXML_XSLT_AST_MODULE_RELATION_USED_PACKAGE ||
            module->document.representation == 0) {
            continue;
        }
        if (sxml_byte_view_equals(
                node_text_of(reader, module->referencing_node, SXML_XSLT_AST_ATTRIBUTE_NAME),
                identity.package_name) &&
            sxml_byte_view_equals(node_text_of(reader, module->referencing_node,
                                               SXML_XSLT_AST_ATTRIBUTE_PACKAGE_VERSION),
                                  identity.version_range)) {
            return module->document;
        }
    }
    return (sxml_item){.representation = 0};
}

/* XTSE3005 (3.4.2): a package that depends on itself, directly or not: a
   package of the same name on the path of modules being read. */
static bool is_package_on_path(const stylesheet_reader *reader, sxml_xslt_ast_module_index module,
                               sxml_byte_view package_name) {
    const sxml_xslt_ast *ast = reader->ast;
    uint32_t current = module.value;
    while (current != 0) {
        const sxml_xslt_ast_module *entry =
            sxml_xslt_ast_get_module(ast, (sxml_xslt_ast_module_index){.value = current});
        if (entry->relation != SXML_XSLT_AST_MODULE_RELATION_INCLUDED &&
            sxml_xslt_ast_get_node(ast, entry->root)->kind == SXML_XSLT_AST_KIND_PACKAGE &&
            sxml_byte_view_equals(node_text_of(reader, entry->root, SXML_XSLT_AST_ATTRIBUTE_NAME),
                                  package_name)) {
            return true;
        }
        current = entry->parent_module.value;
    }
    return false;
}

/*
 * An xsl:use-package (3.4.2, 3.4.3): the package is asked from the
 * resolver by name and version range (XSLT_PACKAGE, "*" without
 * package-version) and its principal module read in place as a module of
 * relation USED_PACKAGE, the top of a package of its own; a package read
 * before with the same name and range is not asked again. The used
 * package's name and version are checked by src/xslt_package.c.
 * XTSE3008 for a use-package outside the package's top stylesheet level,
 * XTSE3005 for a package that uses itself, XTSE3000 when the resolver
 * delivers nothing.
 */
static sxml_status read_used_package(stylesheet_reader *reader, const module_reference *reference) {
    const sxml_xslt_ast_module_index module = reference->parent->module;
    const sxml_xslt_ast_module_relation level_relation =
        sxml_xslt_ast_get_module(reader->ast,
                                 (sxml_xslt_ast_module_index){
                                     .value = level_of_module(reader, module.value)})
            ->relation;
    if (level_relation == SXML_XSLT_AST_MODULE_RELATION_IMPORTED) {
        return raise_error(reader, (reader_error){.error_code = "XTSE3008",
                                                  .description = "An xsl:use-package is in a "
                                                                 "module the package imports "
                                                                 "(XSLT 4.0, 3.4.2)",
                                                  .module = module,
                                                  .source_span = reference->facts->source_span});
    }
    const package_identity identity = {
        .package_name = node_text_of(reader, reference->node, SXML_XSLT_AST_ATTRIBUTE_NAME),
        .version_range =
            node_text_of(reader, reference->node, SXML_XSLT_AST_ATTRIBUTE_PACKAGE_VERSION)};
    if (is_package_on_path(reader, module, identity.package_name)) {
        return raise_error(reader, (reader_error){.error_code = "XTSE3005",
                                                  .description = "A package depends on itself "
                                                                 "(XSLT 4.0, 3.4.2)",
                                                  .module = module,
                                                  .source_span = reference->facts->source_span});
    }
    const sxml_item read_before = find_read_package(reader, identity);
    if (read_before.representation != 0) {
        const sxml_xslt_ast_module *other = NULL;
        for (uint32_t module_index = 1; module_index < reader->ast->module_count;
             module_index += 1) {
            const sxml_xslt_ast_module *entry = sxml_xslt_ast_get_module(
                reader->ast, (sxml_xslt_ast_module_index){.value = module_index});
            if (entry->document.representation == read_before.representation) {
                other = entry;
                break;
            }
        }
        /* The URI is copied: the module array may move below. */
        reader->context_text.count = 0;
        sxml_status status = append_text_bytes(reader, &reader->context_text,
                                               other == NULL ? identity.package_name
                                                             : other->module_uri);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return start_module(reader, &(module_start){.relation = reference->relation,
                                                    .parent_module = module,
                                                    .referencing_node = reference->node,
                                                    .document = read_before,
                                                    .module_uri = {.bytes = reader->context_text.elements,
                                                                   .byte_count = reader->context_text.count}});
    }
    const sxml_resource_resolver resolver = reader->resource_resolver.operations == NULL
                                                ? reader->processor->resource_resolver
                                                : reader->resource_resolver;
    sxml_resource resource;
    memset(&resource, 0, sizeof(resource));
    bool is_resolved = false;
    sxml_status status = SXML_STATUS_OK;
    if (resolver.operations != NULL && resolver.operations->resolve != NULL) {
        status = resolver.operations->resolve(
            resolver.state,
            &(sxml_resource_request){
                .resource_kind = SXML_RESOURCE_KIND_XSLT_PACKAGE,
                .uri_reference = identity.package_name,
                .absolute_uri = identity.package_name,
                .processor = reader->processor,
                .encoding_name = {.bytes = NULL, .byte_count = 0},
                .is_default_collection = false,
                .target_namespace_uri = {.bytes = NULL, .byte_count = 0},
                .package_version = identity.version_range.byte_count == 0
                                       ? SXML_BYTE_VIEW_FROM_LITERAL("*")
                                       : identity.version_range},
            &resource);
        is_resolved = status == SXML_STATUS_OK;
    }
    sxml_item document = {.representation = 0};
    bool has_document = false;
    if (status == SXML_STATUS_OK && resource.resource_form == SXML_RESOURCE_FORM_BYTES) {
        const sxml_byte_view module_uri =
            resource.resource_uri.byte_count > 0 ? resource.resource_uri : identity.package_name;
        status = parse_module(reader,
                              (module_text){.content = resource.content,
                                            .encoding_name = resource.encoding_name,
                                            .module_uri = module_uri},
                              &document);
        has_document = status == SXML_STATUS_OK;
        if (has_document) {
            status = start_module(reader, &(module_start){.relation = reference->relation,
                                                          .parent_module = module,
                                                          .referencing_node = reference->node,
                                                          .document = document,
                                                          .module_uri = module_uri});
        }
    } else if (status == SXML_STATUS_OK && resource.resource_form != SXML_RESOURCE_FORM_NONE) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    } else if (status == SXML_STATUS_OK) {
        status = raise_error(reader, (reader_error){.error_code = "XTSE3000",
                                                    .description = "No package of the name and "
                                                                   "version an xsl:use-package "
                                                                   "asks for is found (XSLT 4.0, "
                                                                   "3.4.3)",
                                                    .module = module,
                                                    .source_span = reference->facts->source_span});
    }
    if (has_document) {
        sxml_item_release(document);
    }
    if (is_resolved && resolver.operations->release != NULL) {
        resolver.operations->release(resolver.state, &resource);
    }
    return status;
}

/* ------------------------------------------------------------ Text */

static sxml_status append_text_piece(stylesheet_reader *reader, sxml_tree *tree, sxml_node_index text_node) {
    sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
    sxml_status status = sxml_node_get_content(tree_node(tree, text_node), &content);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (reader->text.count == 0) {
        reader->text_node = text_node;
        reader->text_span = location_of(tree, text_node);
    }
    return append_text_bytes(reader, &reader->text, content);
}

/* The collected text, as a text node of the frame's element unless
   whitespace stripping removes it (3.11.1); following_kind is the kind of
   the element after the text (NONE at the end). */
typedef struct text_end {
    size_t frame_index;
    sxml_xslt_ast_kind following_kind;
} text_end;

static sxml_status flush_text(stylesheet_reader *reader, text_end end) {
    if (reader->text.count == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_byte_view text = {.bytes = reader->text.elements, .byte_count = reader->text.count};
    open_element *frame = frame_at(reader, end.frame_index);
    const bool is_in_text_element = frame->rule != NULL && frame->rule->kind == SXML_XSLT_AST_KIND_TEXT;
    if (sxml_xml_text_is_whitespace(text)) {
        const bool is_stripped =
            (!is_in_text_element && !frame->preserves_space) ||
            (frame->rule != NULL && (frame->rule->element_flags & SXML_ELEMENT_FLAG_STRIPS_WHITESPACE) != 0) ||
            end.following_kind == SXML_XSLT_AST_KIND_PARAM ||
            end.following_kind == SXML_XSLT_AST_KIND_SORT ||
            end.following_kind == SXML_XSLT_AST_KIND_CONTEXT_ITEM ||
            end.following_kind == SXML_XSLT_AST_KIND_ON_COMPLETION ||
            frame->previous_child_kind == SXML_XSLT_AST_KIND_CATCH ||
            frame->child_context == SXML_CHILD_CONTEXT_DECLARATIONS ||
            frame->child_context == SXML_CHILD_CONTEXT_DOCUMENT;
        if (is_stripped) {
            reader->text.count = 0;
            return SXML_STATUS_OK;
        }
    }
    sxml_status status = SXML_STATUS_OK;
    switch (frame->child_context) {
    case SXML_CHILD_CONTEXT_DOCUMENT:
    case SXML_CHILD_CONTEXT_FALLBACK_ONLY:
    case SXML_CHILD_CONTEXT_SKIPPED:
        reader->text.count = 0;
        return SXML_STATUS_OK;
    case SXML_CHILD_CONTEXT_DECLARATIONS:
        status = raise_error(reader, (reader_error){.error_code = "XTSE0120",
                                                    .description = "A module root has a text "
                                                                   "child (XSLT 4.0, 3.6)",
                                                    .module = frame->module,
                                                    .source_span = reader->text_span});
        break;
    case SXML_CHILD_CONTEXT_RULE:
        if (frame->rule != NULL &&
            (frame->rule->content_model == SXML_CONTENT_MODEL_CHILDREN_THEN_SEQUENCE_CONSTRUCTOR ||
             (frame->rule->content_model == SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR_THEN_CHILDREN &&
              !frame->has_listed_child_after_sequence_constructor))) {
            frame->has_sequence_constructor_child = true;
        } else if (!frame->is_forwards_compatible) {
            status = raise_error(reader, (reader_error){.error_code = "XTSE0010",
                                                        .description = "Text is not allowed in "
                                                                       "this XSLT element (XSLT "
                                                                       "4.0, element syntax)",
                                                        .module = frame->module,
                                                        .source_span = reader->text_span});
        } else {
            reader->text.count = 0;
            return SXML_STATUS_OK;
        }
        break;
    case SXML_CHILD_CONTEXT_SEQUENCE_CONSTRUCTOR:
        break;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_xslt_ast_static_context_index context = frame->static_context;
    const sxml_xslt_ast_module_index module = frame->module;
    const sxml_xslt_ast_node_index parent_node = frame->node;
    const bool expands_text = sxml_xslt_ast_get_static_context(reader->ast, context)->expands_text;
    sxml_xslt_ast_value content = {.kind = SXML_XSLT_AST_VALUE_KIND_NONE};
    sxml_ast *expression_ast = NULL;
    if (expands_text) {
        /* 5.6.2: a text value template. */
        status = parse_expression(reader,
                                  &(expression_text){.kind = SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE,
                                                     .text = text,
                                                     .module = module,
                                                     .source_span = reader->text_span},
                                  &expression_ast);
        content = (sxml_xslt_ast_value){.kind = SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE,
                                        .payload = {.expression = next_expression_index(reader)}};
    } else {
        status = read_text_value(reader, text, &content);
    }
    sxml_xslt_ast_node_index node = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_ast_add_node(
            reader->ast,
            &(sxml_xslt_ast_node_request){.kind = SXML_XSLT_AST_KIND_TEXT_NODE,
                                          .flags = 0,
                                          .module = module,
                                          .source_node = reader->text_node,
                                          .static_context = context,
                                          .element_name = {.index = 0},
                                          .namespace_prefix = {.index = 0},
                                          .content = content,
                                          .source_span = reader->text_span},
            &node);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_ast_append_child(
            reader->ast, (sxml_xslt_ast_child_request){.parent = parent_node, .child = node});
    }
    if (status == SXML_STATUS_OK && expression_ast != NULL) {
        sxml_xslt_ast_expression_index expression = {.value = 0};
        status = hand_over_expression(reader,
                                      &(expression_handover){.kind = content.kind,
                                                             .expression_ast = expression_ast,
                                                             .static_context = context,
                                                             .node = node,
                                                             .source_span = reader->text_span},
                                      &expression);
        expression_ast = NULL;
    }
    sxml_ast_destroy(expression_ast);
    reader->text.count = 0;
    return status;
}

/* A frame whose children are all read: its required child (XTSE0010). */
static sxml_status end_frame(stylesheet_reader *reader, size_t frame_index) {
    const open_element *frame = frame_at(reader, frame_index);
    if (frame->requires_no_children &&
        sxml_xslt_ast_get_node(reader->ast, frame->node)->first_child.value != 0) {
        return raise_error(reader, (reader_error){.error_code = "XTSE0010",
                                                  .description = "A static variable or "
                                                                 "parameter has content (XSLT "
                                                                 "4.0, 9.6)",
                                                  .module = frame->module,
                                                  .source_span = location_of(frame->tree,
                                                                             frame->element)});
    }
    if (frame->rule != NULL && frame->child_context == SXML_CHILD_CONTEXT_RULE &&
        frame->rule->required_child_kind != SXML_XSLT_AST_KIND_NONE &&
        !frame->has_required_child && !frame->is_forwards_compatible) {
        return raise_error(reader,
                           (reader_error){.error_code = "XTSE0010",
                                          .description = "An XSLT element lacks a child its "
                                                         "content model requires (XSLT 4.0, "
                                                         "element syntax)",
                                          .module = frame->module,
                                          .source_span = location_of(frame->tree, frame->element)});
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ One element */

/* The node kind of a role, for elements outside a rule. */
static sxml_xslt_ast_kind kind_of_role(element_role role, const element_rule *rule) {
    switch (role) {
    case SXML_ELEMENT_ROLE_MODULE_ROOT:
    case SXML_ELEMENT_ROLE_XSLT:
        return rule->kind;
    case SXML_ELEMENT_ROLE_SIMPLIFIED_ROOT:
    case SXML_ELEMENT_ROLE_LITERAL_RESULT:
        return SXML_XSLT_AST_KIND_LITERAL_RESULT_ELEMENT;
    case SXML_ELEMENT_ROLE_UNKNOWN_INSTRUCTION:
        return SXML_XSLT_AST_KIND_UNKNOWN_INSTRUCTION;
    case SXML_ELEMENT_ROLE_EXTENSION_INSTRUCTION:
        return SXML_XSLT_AST_KIND_EXTENSION_INSTRUCTION;
    case SXML_ELEMENT_ROLE_DATA_ELEMENT:
        return SXML_XSLT_AST_KIND_DATA_ELEMENT;
    case SXML_ELEMENT_ROLE_IGNORED:
        break;
    }
    return SXML_XSLT_AST_KIND_NONE;
}

/* The synthesized root and template of a simplified module (3.7): the
   literal result element becomes the template's body; writes the
   template to *template_node. */
typedef struct simplified_request {
    const open_element *parent;
    const element_facts *facts;
    sxml_xslt_ast_static_context_index static_context;
} simplified_request;

static sxml_status add_simplified_module(stylesheet_reader *reader, const simplified_request *request,
                                         sxml_xslt_ast_node_index *template_node) {
    const sxml_xslt_ast_module_index module = request->parent->module;
    sxml_xslt_ast_node_index root = {.value = 0};
    sxml_status status = sxml_xslt_ast_add_node(
        reader->ast,
        &(sxml_xslt_ast_node_request){.kind = SXML_XSLT_AST_KIND_STYLESHEET,
                                      .flags = SXML_XSLT_AST_NODE_FLAG_SYNTHESIZED,
                                      .module = module,
                                      .source_node = request->facts->source.element,
                                      .static_context = request->static_context,
                                      .element_name = {.index = 0},
                                      .namespace_prefix = {.index = 0},
                                      .content = {.kind = SXML_XSLT_AST_VALUE_KIND_NONE},
                                      .source_span = request->facts->source_span},
        &root);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_ast_add_node(
            reader->ast,
            &(sxml_xslt_ast_node_request){.kind = SXML_XSLT_AST_KIND_TEMPLATE,
                                          .flags = SXML_XSLT_AST_NODE_FLAG_SYNTHESIZED |
                                                   SXML_XSLT_AST_NODE_FLAG_DECLARATION,
                                          .module = module,
                                          .source_node = request->facts->source.element,
                                          .static_context = request->static_context,
                                          .element_name = {.index = 0},
                                          .namespace_prefix = {.index = 0},
                                          .content = {.kind = SXML_XSLT_AST_VALUE_KIND_NONE},
                                          .source_span = request->facts->source_span},
            template_node);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_ast_append_child(
            reader->ast, (sxml_xslt_ast_child_request){.parent = root, .child = *template_node});
    }
    sxml_ast *pattern = NULL;
    if (status == SXML_STATUS_OK) {
        status = parse_expression(reader,
                                  &(expression_text){.kind = SXML_XSLT_AST_VALUE_KIND_PATTERN,
                                                     .text = SXML_BYTE_VIEW_FROM_LITERAL("."),
                                                     .module = module,
                                                     .source_span = {.byte_offset = 0,
                                                                     .byte_count = 0,
                                                                     .line_number = 0,
                                                                     .column_number = 0}},
                                  &pattern);
    }
    sxml_xslt_ast_expression_index expression = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = hand_over_expression(
            reader,
            &(expression_handover){.kind = SXML_XSLT_AST_VALUE_KIND_PATTERN,
                                   .expression_ast = pattern,
                                   .static_context = request->static_context,
                                   .node = *template_node,
                                   .source_span = {.byte_offset = 0,
                                                   .byte_count = 0,
                                                   .line_number = 0,
                                                   .column_number = 0}},
            &expression);
    }
    sxml_name_identifier match_name = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            reader->name_pool,
            &(sxml_name_request){.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                                 .local_name = SXML_BYTE_VIEW_FROM_LITERAL("match")},
            &match_name);
    }
    sxml_xslt_ast_attribute_index attribute = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_ast_add_attribute(
            reader->ast,
            &(sxml_xslt_ast_attribute_request){
                .node = *template_node,
                .kind = SXML_XSLT_AST_ATTRIBUTE_MATCH,
                .flags = SXML_XSLT_AST_ATTRIBUTE_FLAG_SYNTHESIZED,
                .attribute_name = match_name,
                .namespace_prefix = {.index = 0},
                .value = {.kind = SXML_XSLT_AST_VALUE_KIND_PATTERN,
                          .payload = {.expression = expression}},
                .source_span = {.byte_offset = 0, .byte_count = 0, .line_number = 0,
                                .column_number = 0}},
            &attribute);
    }
    return status;
}

/* The text of an xsl:select element: its text children, the expression
   (10.4.2). */
static sxml_status collect_select_text(stylesheet_reader *reader, element_source source) {
    reader->text.count = 0;
    sxml_node_description description;
    sxml_status status = describe(source.tree, source.element, &description);
    bool has_child = status == SXML_STATUS_OK && description.has_children;
    sxml_node_index child = description.first_child;
    while (status == SXML_STATUS_OK && has_child) {
        sxml_node_description child_description;
        status = describe(source.tree, child, &child_description);
        if (status == SXML_STATUS_OK && child_description.node_kind == SXML_NODE_KIND_TEXT) {
            status = append_text_piece(reader, source.tree, child);
        }
        has_child = child_description.has_next_sibling;
        child = child_description.next_sibling;
    }
    return status;
}

/* Checks of single elements that need no other declaration (XTSE0500,
   XTSE4010). Unprefixed function names (private in 4.0) and
   xsl:import-schema are left to lowering. */
/* A node just read from an element. */
typedef struct element_node {
    element_place place;
    sxml_xslt_ast_node_index node;
} element_node;

static sxml_status check_element(stylesheet_reader *reader, element_node element) {
    const open_element *parent = element.place.parent;
    const element_facts *facts = element.place.facts;
    const sxml_xslt_ast_node_index node = element.node;
    const sxml_xslt_ast *ast = reader->ast;
    const sxml_xslt_ast_kind kind = facts->rule->kind;
    if (kind == SXML_XSLT_AST_KIND_TEMPLATE) {
        const bool has_match = sxml_xslt_ast_find_attribute(
                                   ast, (sxml_xslt_ast_attribute_query){
                                            .node = node, .kind = SXML_XSLT_AST_ATTRIBUTE_MATCH})
                                   .value != 0;
        const bool has_name = sxml_xslt_ast_find_attribute(
                                  ast, (sxml_xslt_ast_attribute_query){
                                           .node = node, .kind = SXML_XSLT_AST_ATTRIBUTE_NAME})
                                  .value != 0;
        const bool has_rule_attribute =
            sxml_xslt_ast_find_attribute(ast, (sxml_xslt_ast_attribute_query){
                                                  .node = node,
                                                  .kind = SXML_XSLT_AST_ATTRIBUTE_MODE})
                    .value != 0 ||
            sxml_xslt_ast_find_attribute(ast, (sxml_xslt_ast_attribute_query){
                                                  .node = node,
                                                  .kind = SXML_XSLT_AST_ATTRIBUTE_PRIORITY})
                    .value != 0;
        if (!has_match && parent->rule != NULL && parent->rule->kind == SXML_XSLT_AST_KIND_MODE) {
            return raise_at_element(reader, (element_place){.parent = parent, .facts = facts},
(reader_error){.error_code = "XTSE4010",
                                                   .description = "A template inside xsl:mode "
                                                                  "has no match attribute (XSLT "
                                                                  "4.0, 6.9.6)"});
        }
        if ((!has_match && !has_name) || (!has_match && has_rule_attribute)) {
            return raise_at_element(reader, (element_place){.parent = parent, .facts = facts},
(reader_error){.error_code = "XTSE0500",
                                                   .description = "xsl:template has neither "
                                                                  "match nor name, or mode or "
                                                                  "priority without match (XSLT "
                                                                  "4.0, 6.1)"});
        }
    }
    return SXML_STATUS_OK;
}

/* Marks what a child read means for its parent's content model. */
/* A child element read into the frame at parent_index. */
typedef struct child_note {
    size_t parent_index;
    const element_facts *facts;
    element_role role;
} child_note;

static sxml_status note_child(stylesheet_reader *reader, child_note note) {
    const size_t parent_index = note.parent_index;
    const element_facts *facts = note.facts;
    const element_role role = note.role;
    open_element *parent = frame_at(reader, parent_index);
    const sxml_xslt_ast_kind kind = kind_of_role(role, facts->rule);
    parent->previous_child_kind = kind;
    if (parent->child_context != SXML_CHILD_CONTEXT_RULE) {
        return SXML_STATUS_OK;
    }
    const element_rule *rule = parent->rule;
    if (kind == rule->required_child_kind) {
        parent->has_required_child = true;
    }
    const bool is_listed = facts->is_xslt && facts->rule != NULL &&
                           role == SXML_ELEMENT_ROLE_XSLT && rule_lists_child(rule, kind);
    bool is_out_of_order = false;
    if (is_listed) {
        uint32_t position = 0;
        while (position < rule->child_kind_count && rule->child_kinds[position] != kind) {
            position += 1;
        }
        const uint32_t bit = 1U << position;
        is_out_of_order = (rule->is_ordered && position < parent->last_child_position) ||
                          ((rule->single_child_mask & bit) != 0 && (parent->seen_child_mask & bit) != 0);
        parent->seen_child_mask |= bit;
        parent->last_child_position = position;
    }
    if (rule->content_model == SXML_CONTENT_MODEL_CHILDREN_THEN_SEQUENCE_CONSTRUCTOR) {
        is_out_of_order = is_out_of_order || (is_listed && parent->has_sequence_constructor_child);
        parent->has_sequence_constructor_child =
            parent->has_sequence_constructor_child || !is_listed;
    } else if (rule->content_model == SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR_THEN_CHILDREN) {
        is_out_of_order =
            is_out_of_order || (!is_listed && parent->has_listed_child_after_sequence_constructor);
        parent->has_listed_child_after_sequence_constructor =
            parent->has_listed_child_after_sequence_constructor || is_listed;
    }
    if (is_out_of_order && !parent->is_forwards_compatible) {
        return raise_at_element(reader, (element_place){.parent = parent, .facts = facts},
(reader_error){.error_code = "XTSE0010",
                                               .description = "A child element is out of the "
                                                              "order its parent's content model "
                                                              "gives (XSLT 4.0, element "
                                                              "syntax)"});
    }
    return SXML_STATUS_OK;
}

static sxml_status read_element(stylesheet_reader *reader, element_input input) {
    const open_element parent = *frame_at(reader, input.parent_index);
    const sxml_node_description *description = input.description;
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    sxml_status status =
        sxml_name_pool_get_name_parts(reader->name_pool, description->node_name, &parts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* exsl:document in an extension namespace is read as
       xsl:result-document (D206). */
    const bool is_result_document_alias =
        is_exslt_document(parts.namespace_uri, parts.local_name) &&
        is_extension_namespace(reader, parent.static_context, parts.namespace_uri);
    const bool is_xslt =
        parts.namespace_uri.index == SXML_NAMESPACE_XSLT || is_result_document_alias;
    const element_facts facts = {
        .element_name = description->node_name,
        .namespace_uri = parts.namespace_uri,
        .local_name = parts.local_name,
        .is_xslt = is_xslt,
        .rule = is_xslt ? find_element_rule(is_result_document_alias
                                                ? SXML_BYTE_VIEW_FROM_LITERAL("result-document")
                                                : parts.local_name)
                        : NULL,
        .source = {.tree = parent.tree,
                   .element = input.element,
                   .attribute_count = description->attribute_count},
        .source_span = location_of(parent.tree, input.element)};
    status = collect_attributes(reader, facts.source);
    /* xsl:record's attributes in no namespace are fields, not standard
       or shadow attributes; its standard attributes are written xsl:...,
       as on a literal result element (D142). */
    const bool is_record = facts.rule != NULL && facts.rule->kind == SXML_XSLT_AST_KIND_RECORD;
    const bool has_unprefixed_standard_attributes = is_xslt && !is_record;
    if (status == SXML_STATUS_OK && has_unprefixed_standard_attributes) {
        status = apply_shadow_attributes(
            reader, &(shadow_request){.source = facts.source,
                                      .module = parent.module,
                                      .parent_context = parent.static_context});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool is_document_child = parent.child_context == SXML_CHILD_CONTEXT_DOCUMENT;
    /* 3.4.5.3: the children of xsl:override are declarations of the using
       package. */
    const bool is_overriding_declaration =
        parent.rule != NULL && parent.rule->kind == SXML_XSLT_AST_KIND_OVERRIDE;
    const bool is_module_root = is_document_child && is_xslt && facts.rule != NULL &&
                                (facts.rule->element_flags & SXML_ELEMENT_FLAG_MODULE_ROOT) != 0;
    sxml_xslt_ast_static_context_index context = {.value = 0};
    status = compute_static_context(
        reader,
        &(context_request){.source = facts.source,
                           .module = parent.module,
                           .parent_context = parent.static_context,
                           .is_xslt_element = has_unprefixed_standard_attributes,
                           .is_module_root = is_module_root,
                           .is_simplified_root = is_document_child && !is_module_root,
                           .is_overriding_declaration = is_overriding_declaration},
        &context);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool is_forwards_compatible =
        sxml_xslt_ast_get_static_context(reader->ast, context)->version > 4.0;
    /* The role first: an element forwards-compatible processing ignores
       has no use-when to evaluate (3.9). */
    const role_decision decision = decide_role(reader, &(role_request){.parent = &parent, .facts = &facts, .is_forwards_compatible = is_forwards_compatible});
    if (decision.role == SXML_ELEMENT_ROLE_IGNORED && decision.error_code == NULL) {
        return SXML_STATUS_OK;
    }
    /* [xsl:]use-when (3.11.3). */
    bool is_included = true;
    const element_attribute *use_when = find_standard_value(
        reader, (standard_query){.is_xslt_element = has_unprefixed_standard_attributes,
                                 .local_name = "use-when"});
    if (use_when != NULL) {
        sxml_sequence value = sxml_sequence_empty();
        status = evaluate_in_context(reader,
                                     &(context_expression){.text = use_when->value,
                                                           .module = parent.module,
                                                           .static_context = context,
                                                           .source_span = use_when->source_span},
                                     &value);
        sxml_effective_boolean_value truth = {.is_true = false};
        if (status == SXML_STATUS_OK) {
            status = sxml_evaluator_compute_effective_boolean_value(value, reader->error_record,
                                                                    &truth);
            status = relocate_error(reader, (error_relocation){.status = status,
                                                               .module = parent.module,
                                                               .source_span =
                                                                   use_when->source_span});
        }
        sxml_sequence_release(value);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        is_included = truth.is_true;
    }
    if (!is_included && !is_module_root) {
        return SXML_STATUS_OK;
    }
    if (decision.error_code != NULL) {
        return raise_at_element(reader, (element_place){.parent = &parent, .facts = &facts},
(reader_error){.error_code = decision.error_code,
                                               .description = decision.description});
    }
    const sxml_xslt_ast_kind kind = kind_of_role(decision.role, facts.rule);
    status = note_child(reader, (child_note){.parent_index = input.parent_index, .facts = &facts, .role = decision.role});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xslt_ast_node_index parent_node = parent.node;
    if (is_document_child && !is_module_root) {
        status = add_simplified_module(
            reader,
            &(simplified_request){.parent = &parent, .facts = &facts, .static_context = context},
            &parent_node);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    /* xsl:select's text is its expression, the node's content (10.4.2). */
    sxml_xslt_ast_value content = {.kind = SXML_XSLT_AST_VALUE_KIND_NONE};
    sxml_ast *content_ast = NULL;
    if (kind == SXML_XSLT_AST_KIND_SELECT) {
        status = collect_select_text(reader, facts.source);
        if (status == SXML_STATUS_OK) {
            status = parse_expression(
                reader,
                &(expression_text){.kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                   /* An empty xsl:select is the empty sequence. */
                                   .text = sxml_xml_text_is_whitespace((sxml_byte_view){
                                               .bytes = reader->text.elements,
                                               .byte_count = reader->text.count})
                                               ? SXML_BYTE_VIEW_FROM_LITERAL("()")
                                               : (sxml_byte_view){.bytes = reader->text.elements,
                                                                  .byte_count = reader->text.count},
                                   .module = parent.module,
                                   .source_span = reader->text.count > 0 ? reader->text_span
                                                                         : facts.source_span},
                &content_ast);
        }
        reader->text.count = 0;
        if (status != SXML_STATUS_OK) {
            return status;
        }
        content = (sxml_xslt_ast_value){.kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                        .payload = {.expression = next_expression_index(reader)}};
    }
    const bool is_literal = kind == SXML_XSLT_AST_KIND_LITERAL_RESULT_ELEMENT ||
                            kind == SXML_XSLT_AST_KIND_EXTENSION_INSTRUCTION ||
                            kind == SXML_XSLT_AST_KIND_UNKNOWN_INSTRUCTION ||
                            kind == SXML_XSLT_AST_KIND_DATA_ELEMENT;
    sxml_xslt_ast_node_index node = {.value = 0};
    status = sxml_xslt_ast_add_node(
        reader->ast,
        &(sxml_xslt_ast_node_request){
            .kind = kind,
            .flags = parent.child_context == SXML_CHILD_CONTEXT_DECLARATIONS ||
                             is_overriding_declaration
                         ? (uint32_t)SXML_XSLT_AST_NODE_FLAG_DECLARATION
                         : 0U,
            .module = parent.module,
            .source_node = input.element,
            .static_context = context,
            .element_name = is_literal ? facts.element_name : (sxml_name_identifier){.index = 0},
            .namespace_prefix = is_literal ? description->namespace_prefix
                                           : (sxml_namespace_prefix_identifier){.index = 0},
            .content = content,
            .source_span = facts.source_span},
        &node);
    if (status == SXML_STATUS_OK && parent_node.value != 0) {
        status = sxml_xslt_ast_append_child(
            reader->ast, (sxml_xslt_ast_child_request){.parent = parent_node, .child = node});
    }
    if (status == SXML_STATUS_OK && content_ast != NULL) {
        sxml_xslt_ast_expression_index expression = {.value = 0};
        status = hand_over_expression(reader,
                                      &(expression_handover){.kind = content.kind,
                                                             .expression_ast = content_ast,
                                                             .static_context = context,
                                                             .node = node,
                                                             .source_span = facts.source_span},
                                      &expression);
        content_ast = NULL;
    }
    sxml_ast_destroy(content_ast);
    if (status == SXML_STATUS_OK && decision.role != SXML_ELEMENT_ROLE_DATA_ELEMENT) {
        status = add_node_attributes(reader, &(node_attributes_request){
                                                 .parent = &parent,
                                                 .facts = &facts,
                                                 .role = decision.role,
                                                 .node = node,
                                                 .static_context = context,
                                                 .is_forwards_compatible = is_forwards_compatible});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool preserves_space = preserves_space_of(reader, (space_inheritance){.preserves_space = parent.preserves_space});
    bool is_static = false;
    child_context children = SXML_CHILD_CONTEXT_SEQUENCE_CONSTRUCTOR;
    bool has_frame = description->has_children;
    switch (decision.role) {
    case SXML_ELEMENT_ROLE_MODULE_ROOT:
        children = is_included ? SXML_CHILD_CONTEXT_DECLARATIONS : SXML_CHILD_CONTEXT_SKIPPED;
        break;
    case SXML_ELEMENT_ROLE_SIMPLIFIED_ROOT:
    case SXML_ELEMENT_ROLE_LITERAL_RESULT:
        children = SXML_CHILD_CONTEXT_SEQUENCE_CONSTRUCTOR;
        break;
    case SXML_ELEMENT_ROLE_UNKNOWN_INSTRUCTION:
    case SXML_ELEMENT_ROLE_EXTENSION_INSTRUCTION:
        children = SXML_CHILD_CONTEXT_FALLBACK_ONLY;
        break;
    case SXML_ELEMENT_ROLE_DATA_ELEMENT:
    case SXML_ELEMENT_ROLE_IGNORED:
        has_frame = false;
        break;
    case SXML_ELEMENT_ROLE_XSLT:
        status = check_element(reader, (element_node){.place = {.parent = &parent, .facts = &facts}, .node = node});
        if (status == SXML_STATUS_OK &&
            (kind == SXML_XSLT_AST_KIND_VARIABLE || kind == SXML_XSLT_AST_KIND_PARAM)) {
            status = read_static_declaration(reader, &(declaration_input){
                                                         .parent = &parent,
                                                         .facts = &facts,
                                                         .node = node,
                                                         .static_context = context,
                                                         .is_static = &is_static});
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        switch (facts.rule->content_model) {
        case SXML_CONTENT_MODEL_EMPTY: {
            content_presence presence = {.has_content = false};
            status = has_content(
                (content_query){.source = facts.source, .preserves_space = preserves_space},
                &presence);
            if (status == SXML_STATUS_OK && presence.has_content && !is_forwards_compatible) {
                status = raise_at_element(reader, (element_place){.parent = &parent, .facts = &facts},
(reader_error){.error_code = "XTSE0260",
                                                         .description = "An XSLT element that "
                                                                        "must be empty has "
                                                                        "content (XSLT 4.0, "
                                                                        "3.11.1)"});
            }
            has_frame = false;
            break;
        }
        case SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR:
            children = SXML_CHILD_CONTEXT_SEQUENCE_CONSTRUCTOR;
            break;
        case SXML_CONTENT_MODEL_DECLARATIONS:
            children = SXML_CHILD_CONTEXT_DECLARATIONS;
            break;
        case SXML_CONTENT_MODEL_CHILDREN:
        case SXML_CONTENT_MODEL_CHILDREN_THEN_SEQUENCE_CONSTRUCTOR:
        case SXML_CONTENT_MODEL_SEQUENCE_CONSTRUCTOR_THEN_CHILDREN:
            children = SXML_CHILD_CONTEXT_RULE;
            /* A required child is checked when the frame ends, also for
               an element without children. */
            has_frame = true;
            break;
        case SXML_CONTENT_MODEL_TEXT:
            children = SXML_CHILD_CONTEXT_FALLBACK_ONLY;
            break;
        case SXML_CONTENT_MODEL_SCHEMA:
            has_frame = false;
            break;
        }
        if (status == SXML_STATUS_OK &&
            (kind == SXML_XSLT_AST_KIND_INCLUDE || kind == SXML_XSLT_AST_KIND_IMPORT)) {
            status = read_module_reference(
                reader, &(module_reference){.parent = &parent,
                                            .facts = &facts,
                                            .node = node,
                                            .static_context = context,
                                            .relation = kind == SXML_XSLT_AST_KIND_INCLUDE
                                                            ? SXML_XSLT_AST_MODULE_RELATION_INCLUDED
                                                            : SXML_XSLT_AST_MODULE_RELATION_IMPORTED});
        }
        if (status == SXML_STATUS_OK && kind == SXML_XSLT_AST_KIND_USE_PACKAGE) {
            status = read_used_package(
                reader, &(module_reference){.parent = &parent,
                                            .facts = &facts,
                                            .node = node,
                                            .static_context = context,
                                            .relation = SXML_XSLT_AST_MODULE_RELATION_USED_PACKAGE});
        }
        break;
    }
    if (status != SXML_STATUS_OK || !has_frame) {
        return status;
    }
    return push_frame(reader,
                      &(open_element){.module = parent.module,
                                      .tree = parent.tree,
                                      .element = input.element,
                                      .has_next_child = description->has_children,
                                      .next_child = description->first_child,
                                      .node = node,
                                      .rule = facts.rule,
                                      .child_context = children,
                                      .static_context = context,
                                      .preserves_space = preserves_space,
                                      .is_forwards_compatible = is_forwards_compatible,
                                      .has_sequence_constructor_child = false,
                                      .has_listed_child_after_sequence_constructor = false,
                                      .has_required_child = false,
                                      .requires_no_children = is_static,
                                      .seen_child_mask = 0,
                                      .last_child_position = 0,
                                      .previous_child_kind = SXML_XSLT_AST_KIND_NONE});
}

/* ------------------------------------------------------------ The walk */

static bool is_note_element(const stylesheet_reader *reader, const sxml_node_description *description) {
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    return sxml_name_pool_get_name_parts(reader->name_pool, description->node_name, &parts) ==
               SXML_STATUS_OK &&
           parts.namespace_uri.index == SXML_NAMESPACE_XSLT &&
           sxml_byte_view_equals(parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("note"));
}

/* The XSLT kind an element will have, for 3.11.1's rule on whitespace
   before xsl:param and friends; NONE for others. */
static sxml_xslt_ast_kind kind_ahead(const stylesheet_reader *reader, const sxml_node_description *description) {
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    if (sxml_name_pool_get_name_parts(reader->name_pool, description->node_name, &parts) !=
            SXML_STATUS_OK ||
        parts.namespace_uri.index != SXML_NAMESPACE_XSLT) {
        return SXML_XSLT_AST_KIND_NONE;
    }
    const element_rule *rule = find_element_rule(parts.local_name);
    return rule == NULL ? SXML_XSLT_AST_KIND_NONE : rule->kind;
}

static sxml_status walk(stylesheet_reader *reader) {
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && reader->frames.count > 0) {
        const size_t top_index = reader->frames.count - 1U;
        open_element *top = frame_at(reader, top_index);
        if (!top->has_next_child) {
            status = flush_text(reader, (text_end){.frame_index = top_index,
                                                   .following_kind = SXML_XSLT_AST_KIND_NONE});
            if (status == SXML_STATUS_OK) {
                status = end_frame(reader, top_index);
            }
            reader->frames.count -= 1;
            continue;
        }
        const sxml_node_index child = top->next_child;
        sxml_node_description description;
        status = describe(top->tree, child, &description);
        if (status != SXML_STATUS_OK) {
            break;
        }
        top->has_next_child =
            top->child_context != SXML_CHILD_CONTEXT_DOCUMENT && description.has_next_sibling;
        top->next_child = description.next_sibling;
        switch (description.node_kind) {
        case SXML_NODE_KIND_TEXT:
            status = append_text_piece(reader, top->tree, child);
            break;
        case SXML_NODE_KIND_ELEMENT:
            /* 3.11.2: xsl:note is removed with its content, so text either
               side of it merges. */
            if (is_note_element(reader, &description)) {
                break;
            }
            status = flush_text(reader, (text_end){.frame_index = top_index,
                                                   .following_kind = kind_ahead(reader, &description)});
            if (status == SXML_STATUS_OK) {
                status = read_element(reader, (element_input){.parent_index = top_index,
                                                              .element = child,
                                                              .description = &description});
            }
            break;
        case SXML_NODE_KIND_DOCUMENT:
        case SXML_NODE_KIND_ATTRIBUTE:
        case SXML_NODE_KIND_NAMESPACE:
        case SXML_NODE_KIND_PROCESSING_INSTRUCTION:
        case SXML_NODE_KIND_COMMENT:
        case SXML_NODE_KIND_ANY:
            break;
        }
    }
    return status;
}

/* ------------------------------------------------------------ Entry */

static void release_reader(stylesheet_reader *reader) {
    release_shadow_values(reader);
    scratch_array *arrays[] = {&reader->frames,         &reader->text,
                               &reader->attributes,     &reader->shadow_values,
                               &reader->bindings,       &reader->evaluation_bindings,
                               &reader->context_text,   &reader->excluded_uris,
                               &reader->extension_uris, &reader->name_tokens,
                               &reader->fixed_bindings, &reader->module_states};
    for (size_t index = 0; index < sizeof(arrays) / sizeof(arrays[0]); index += 1) {
        sxml_allocator_release(reader->allocator, arrays[index]->elements);
        *arrays[index] = (scratch_array){.elements = NULL, .count = 0, .capacity = 0};
    }
    sxml_xslt_static_scope_destroy(reader->static_scope);
    sxml_xml_parser_destroy(reader->parser);
    reader->static_scope = NULL;
    reader->parser = NULL;
}

sxml_status sxml_xslt_read_stylesheet(const sxml_xslt_read_request *request, sxml_xslt_ast **ast) {
    if (request == NULL || request->processor == NULL || ast == NULL ||
        (request->principal_module_text.byte_count > 0 &&
         request->principal_module_text.bytes == NULL)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_processor *processor = request->processor;
    stylesheet_reader reader;
    memset(&reader, 0, sizeof(reader));
    reader.processor = processor;
    reader.allocator = processor->allocator;
    reader.name_pool = processor->name_pool;
    reader.resource_resolver = request->resource_resolver;
    reader.error_record = request->error_record;
    reader.static_parameters = request->static_parameters;
    reader.default_collation_uri = request->default_collation_uri;
    const uint64_t node_limit = processor->limits.maximum_tree_node_count;
    sxml_status status = sxml_xslt_ast_create(
        reader.allocator,
        &(sxml_xslt_ast_configuration){
            .allocator = reader.allocator,
            .item_heap = processor->item_heap,
            .maximum_module_count = 0,
            .maximum_node_count = node_limit > UINT32_MAX ? UINT32_MAX : (uint32_t)node_limit},
        &reader.ast);
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_parser_create(
            reader.allocator,
            &(sxml_xml_parser_configuration){.allocator = reader.allocator,
                                             .name_pool = processor->name_pool,
                                             .limits = processor->limits.xml_limits},
            &reader.parser);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_static_scope_create(
            reader.allocator,
            &(sxml_xslt_static_scope_configuration){.processor = processor,
                                                    .resource_resolver = request->resource_resolver},
            &reader.static_scope);
    }
    sxml_item document = {.representation = 0};
    bool has_document = false;
    if (status == SXML_STATUS_OK) {
        status = parse_module(&reader,
                              (module_text){.content = request->principal_module_text,
                                            .encoding_name = {.bytes = NULL, .byte_count = 0},
                                            .module_uri = request->principal_module_uri},
                              &document);
        has_document = status == SXML_STATUS_OK;
    }
    if (status == SXML_STATUS_OK) {
        status = start_module(&reader,
                              &(module_start){.relation = SXML_XSLT_AST_MODULE_RELATION_PRINCIPAL,
                                              .parent_module = {.value = 0},
                                              .referencing_node = {.value = 0},
                                              .document = document,
                                              .module_uri = request->principal_module_uri});
    }
    if (has_document) {
        sxml_item_release(document);
    }
    if (status == SXML_STATUS_OK) {
        status = walk(&reader);
    }
    release_reader(&reader);
    if (status != SXML_STATUS_OK) {
        sxml_xslt_ast_destroy(reader.ast);
        return status;
    }
    sxml_xslt_ast_seal(reader.ast);
    *ast = reader.ast;
    return SXML_STATUS_OK;
}
