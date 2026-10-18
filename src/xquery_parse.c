/*
 * The XQuery 4.0 front end: concrete syntax tree of grammars/xquery to the
 * abstract syntax tree of src/xpath_ast_internal.h (docs/DECISIONS.md D29,
 * D52, D59, D111).
 *
 * Golden file: src/xpath_parse.c (tables, decoders, error reporting).
 *
 * grammars/xquery inherits grammars/xpath, so the XPath front end's one
 * walker builds the tree (sxml_ast_build_with_parser_part): it maps the
 * XPath node types and fields itself and this parser part brings the rest,
 * as lowering parts bring their kinds to the lowerer (D103, D111):
 * - the node types and fields XQuery adds, and the field of the module,
 *   which the grammar leaves unnamed;
 * - apply_token: the keyword tokens of the prolog, the FLWOR clauses and
 *   validate become detail and flags;
 * - describe_payload: XQuery's literals and direct constructor text are
 *   decoded here (references, doubled delimiters and braces, attribute
 *   value normalization and end-of-line handling);
 * - finish_node: the checks D29 leaves to the tree builder, made once a
 *   node is complete: end tag names (XQST0118), duplicate attributes as
 *   written (XQST0040), namespace declaration attributes (XQST0022,
 *   XQST0070, XQST0071, XQST0085), boundary whitespace marks, the version
 *   declaration (XQST0031, XQST0087) and NCName fields of a %constructor
 *   record declaration (XPST0003).
 *
 * The hooks keep no state: everything they need is in the AST being built
 * and in the expression text, which they reach through the front end. No
 * hook recurses or calls the walker (D71); every walk below is a loop over
 * sibling or parent links.
 *
 * Section numbers are of XQuery 4.0 (W3C QT4 CG draft, 13 October 2026).
 */
#include "sort_internal.h"
#include "xpath_ast_internal.h"

#include <string.h>

/* Defined by the generated parser of grammars/xquery (D12, D29). */
const struct TSLanguage *
tree_sitter_xquery(void); // NOLINT(readability-identifier-naming)

/* ------------------------------------------------------------- Tables */

/*
 * Every named node type of grammars/xquery/src/node-types.json that
 * grammars/xpath does not have and that makes an AST node, sorted in byte
 * order. Not here: allowing_empty, which is absorbed as a flag of its
 * for_item_binding (D111).
 */
static const sxml_ast_node_type_entry sxml_xquery_node_types[] = {
    {SXML_BYTE_VIEW_INITIALIZER("annotation"), SXML_AST_KIND_ANNOTATION},
    {SXML_BYTE_VIEW_INITIALIZER("attribute_value_text"),
     SXML_AST_KIND_ATTRIBUTE_VALUE_TEXT},
    {SXML_BYTE_VIEW_INITIALIZER("base_uri_declaration"),
     SXML_AST_KIND_BASE_URI_DECLARATION},
    {SXML_BYTE_VIEW_INITIALIZER("boundary_space_declaration"),
     SXML_AST_KIND_BOUNDARY_SPACE_DECLARATION},
    {SXML_BYTE_VIEW_INITIALIZER("catch_clause"), SXML_AST_KIND_CATCH_CLAUSE},
    {SXML_BYTE_VIEW_INITIALIZER("cdata_section"), SXML_AST_KIND_CDATA_SECTION},
    {SXML_BYTE_VIEW_INITIALIZER("construction_declaration"),
     SXML_AST_KIND_CONSTRUCTION_DECLARATION},
    {SXML_BYTE_VIEW_INITIALIZER("context_value_declaration"),
     SXML_AST_KIND_CONTEXT_VALUE_DECLARATION},
    {SXML_BYTE_VIEW_INITIALIZER("copy_namespaces_declaration"),
     SXML_AST_KIND_COPY_NAMESPACES_DECLARATION},
    {SXML_BYTE_VIEW_INITIALIZER("count_clause"), SXML_AST_KIND_COUNT_CLAUSE},
    {SXML_BYTE_VIEW_INITIALIZER("decimal_format_declaration"),
     SXML_AST_KIND_DECIMAL_FORMAT_DECLARATION},
    {SXML_BYTE_VIEW_INITIALIZER("decimal_format_property"),
     SXML_AST_KIND_DECIMAL_FORMAT_PROPERTY},
    {SXML_BYTE_VIEW_INITIALIZER("default_collation_declaration"),
     SXML_AST_KIND_DEFAULT_COLLATION_DECLARATION},
    {SXML_BYTE_VIEW_INITIALIZER("default_namespace_declaration"),
     SXML_AST_KIND_DEFAULT_NAMESPACE_DECLARATION},
    {SXML_BYTE_VIEW_INITIALIZER("direct_attribute"),
     SXML_AST_KIND_DIRECT_ATTRIBUTE},
    {SXML_BYTE_VIEW_INITIALIZER("direct_attribute_value"),
     SXML_AST_KIND_DIRECT_ATTRIBUTE_VALUE},
    {SXML_BYTE_VIEW_INITIALIZER("direct_comment_constructor"),
     SXML_AST_KIND_DIRECT_COMMENT_CONSTRUCTOR},
    {SXML_BYTE_VIEW_INITIALIZER("direct_element_constructor"),
     SXML_AST_KIND_DIRECT_ELEMENT_CONSTRUCTOR},
    {SXML_BYTE_VIEW_INITIALIZER("direct_element_text"),
     SXML_AST_KIND_DIRECT_ELEMENT_TEXT},
    {SXML_BYTE_VIEW_INITIALIZER("direct_processing_instruction_constructor"),
     SXML_AST_KIND_DIRECT_PROCESSING_INSTRUCTION_CONSTRUCTOR},
    {SXML_BYTE_VIEW_INITIALIZER("empty_order_declaration"),
     SXML_AST_KIND_EMPTY_ORDER_DECLARATION},
    {SXML_BYTE_VIEW_INITIALIZER("extended_field_declaration"),
     SXML_AST_KIND_EXTENDED_FIELD_DECLARATION},
    {SXML_BYTE_VIEW_INITIALIZER("extension_expression"),
     SXML_AST_KIND_EXTENSION_EXPRESSION},
    {SXML_BYTE_VIEW_INITIALIZER("finally_clause"),
     SXML_AST_KIND_FINALLY_CLAUSE},
    {SXML_BYTE_VIEW_INITIALIZER("function_declaration"),
     SXML_AST_KIND_FUNCTION_DECLARATION},
    {SXML_BYTE_VIEW_INITIALIZER("group_by_clause"),
     SXML_AST_KIND_GROUP_BY_CLAUSE},
    {SXML_BYTE_VIEW_INITIALIZER("grouping_specification"),
     SXML_AST_KIND_GROUPING_SPECIFICATION},
    {SXML_BYTE_VIEW_INITIALIZER("item_type_declaration"),
     SXML_AST_KIND_ITEM_TYPE_DECLARATION},
    {SXML_BYTE_VIEW_INITIALIZER("library_module"),
     SXML_AST_KIND_LIBRARY_MODULE},
    {SXML_BYTE_VIEW_INITIALIZER("main_module"), SXML_AST_KIND_MAIN_MODULE},
    {SXML_BYTE_VIEW_INITIALIZER("module"), SXML_AST_KIND_MODULE},
    {SXML_BYTE_VIEW_INITIALIZER("module_declaration"),
     SXML_AST_KIND_MODULE_DECLARATION},
    {SXML_BYTE_VIEW_INITIALIZER("module_import"), SXML_AST_KIND_MODULE_IMPORT},
    {SXML_BYTE_VIEW_INITIALIZER("nominative_record_type_declaration"),
     SXML_AST_KIND_NOMINATIVE_RECORD_TYPE_DECLARATION},
    {SXML_BYTE_VIEW_INITIALIZER("option_declaration"),
     SXML_AST_KIND_OPTION_DECLARATION},
    {SXML_BYTE_VIEW_INITIALIZER("order_by_clause"),
     SXML_AST_KIND_ORDER_BY_CLAUSE},
    {SXML_BYTE_VIEW_INITIALIZER("order_specification"),
     SXML_AST_KIND_ORDER_SPECIFICATION},
    {SXML_BYTE_VIEW_INITIALIZER("ordered_expression"),
     SXML_AST_KIND_ORDERED_EXPRESSION},
    {SXML_BYTE_VIEW_INITIALIZER("ordering_mode_declaration"),
     SXML_AST_KIND_ORDERING_MODE_DECLARATION},
    {SXML_BYTE_VIEW_INITIALIZER("parameter_with_default"),
     SXML_AST_KIND_PARAMETER_WITH_DEFAULT},
    {SXML_BYTE_VIEW_INITIALIZER("pragma"), SXML_AST_KIND_PRAGMA},
    {SXML_BYTE_VIEW_INITIALIZER("pragma_contents"),
     SXML_AST_KIND_PRAGMA_CONTENTS},
    {SXML_BYTE_VIEW_INITIALIZER("prolog"), SXML_AST_KIND_PROLOG},
    {SXML_BYTE_VIEW_INITIALIZER("qname"), SXML_AST_KIND_QNAME},
    {SXML_BYTE_VIEW_INITIALIZER("schema_import"), SXML_AST_KIND_SCHEMA_IMPORT},
    {SXML_BYTE_VIEW_INITIALIZER("schema_prefix"), SXML_AST_KIND_SCHEMA_PREFIX},
    {SXML_BYTE_VIEW_INITIALIZER("sliding_window_clause"),
     SXML_AST_KIND_SLIDING_WINDOW_CLAUSE},
    {SXML_BYTE_VIEW_INITIALIZER("string_constructor"),
     SXML_AST_KIND_STRING_CONSTRUCTOR},
    {SXML_BYTE_VIEW_INITIALIZER("string_constructor_characters"),
     SXML_AST_KIND_STRING_CONSTRUCTOR_CHARACTERS},
    {SXML_BYTE_VIEW_INITIALIZER("string_interpolation"),
     SXML_AST_KIND_STRING_INTERPOLATION},
    {SXML_BYTE_VIEW_INITIALIZER("switch_case_clause"),
     SXML_AST_KIND_SWITCH_CASE_CLAUSE},
    {SXML_BYTE_VIEW_INITIALIZER("switch_expression"),
     SXML_AST_KIND_SWITCH_EXPRESSION},
    {SXML_BYTE_VIEW_INITIALIZER("trace_clause"), SXML_AST_KIND_TRACE_CLAUSE},
    {SXML_BYTE_VIEW_INITIALIZER("try_catch_expression"),
     SXML_AST_KIND_TRY_CATCH_EXPRESSION},
    {SXML_BYTE_VIEW_INITIALIZER("tumbling_window_clause"),
     SXML_AST_KIND_TUMBLING_WINDOW_CLAUSE},
    {SXML_BYTE_VIEW_INITIALIZER("typeswitch_case_clause"),
     SXML_AST_KIND_TYPESWITCH_CASE_CLAUSE},
    {SXML_BYTE_VIEW_INITIALIZER("typeswitch_default_clause"),
     SXML_AST_KIND_TYPESWITCH_DEFAULT_CLAUSE},
    {SXML_BYTE_VIEW_INITIALIZER("typeswitch_expression"),
     SXML_AST_KIND_TYPESWITCH_EXPRESSION},
    {SXML_BYTE_VIEW_INITIALIZER("unordered_expression"),
     SXML_AST_KIND_UNORDERED_EXPRESSION},
    {SXML_BYTE_VIEW_INITIALIZER("validate_expression"),
     SXML_AST_KIND_VALIDATE_EXPRESSION},
    {SXML_BYTE_VIEW_INITIALIZER("variable_declaration"),
     SXML_AST_KIND_VARIABLE_DECLARATION},
    {SXML_BYTE_VIEW_INITIALIZER("version_declaration"),
     SXML_AST_KIND_VERSION_DECLARATION},
    {SXML_BYTE_VIEW_INITIALIZER("where_clause"), SXML_AST_KIND_WHERE_CLAUSE},
    {SXML_BYTE_VIEW_INITIALIZER("while_clause"), SXML_AST_KIND_WHILE_CLAUSE},
    {SXML_BYTE_VIEW_INITIALIZER("window_end_condition"),
     SXML_AST_KIND_WINDOW_END_CONDITION},
    {SXML_BYTE_VIEW_INITIALIZER("window_start_condition"),
     SXML_AST_KIND_WINDOW_START_CONDITION},
};

/*
 * The fields of grammars/xquery that hold named nodes and that
 * grammars/xpath does not have, sorted in byte order. The fields that hold
 * tokens (allowing_empty, direction, empty, external, fixed, inherit_mode,
 * kind, mode, only, preserve_mode, stable, static, and name of
 * decimal_format_property) are read by sxml_xquery_apply_token.
 */
static const sxml_ast_field_entry sxml_xquery_fields[] = {
    {SXML_BYTE_VIEW_INITIALIZER("annotation"), SXML_AST_FIELD_ANNOTATION},
    {SXML_BYTE_VIEW_INITIALIZER("attribute"), SXML_AST_FIELD_ATTRIBUTE},
    {SXML_BYTE_VIEW_INITIALIZER("case"), SXML_AST_FIELD_CASE},
    {SXML_BYTE_VIEW_INITIALIZER("catch"), SXML_AST_FIELD_CATCH},
    {SXML_BYTE_VIEW_INITIALIZER("collation"), SXML_AST_FIELD_COLLATION},
    {SXML_BYTE_VIEW_INITIALIZER("comparand"), SXML_AST_FIELD_COMPARAND},
    {SXML_BYTE_VIEW_INITIALIZER("contents"), SXML_AST_FIELD_CONTENTS},
    {SXML_BYTE_VIEW_INITIALIZER("current"), SXML_AST_FIELD_CURRENT},
    {SXML_BYTE_VIEW_INITIALIZER("default"), SXML_AST_FIELD_DEFAULT},
    {SXML_BYTE_VIEW_INITIALIZER("encoding"), SXML_AST_FIELD_ENCODING},
    {SXML_BYTE_VIEW_INITIALIZER("end"), SXML_AST_FIELD_END},
    {SXML_BYTE_VIEW_INITIALIZER("end_name"), SXML_AST_FIELD_END_NAME},
    {SXML_BYTE_VIEW_INITIALIZER("errors"), SXML_AST_FIELD_ERRORS},
    {SXML_BYTE_VIEW_INITIALIZER("finally"), SXML_AST_FIELD_FINALLY},
    {SXML_BYTE_VIEW_INITIALIZER("location"), SXML_AST_FIELD_LOCATION},
    {SXML_BYTE_VIEW_INITIALIZER("next"), SXML_AST_FIELD_NEXT},
    {SXML_BYTE_VIEW_INITIALIZER("pragma"), SXML_AST_FIELD_PRAGMA},
    {SXML_BYTE_VIEW_INITIALIZER("previous"), SXML_AST_FIELD_PREVIOUS},
    {SXML_BYTE_VIEW_INITIALIZER("prolog"), SXML_AST_FIELD_PROLOG},
    {SXML_BYTE_VIEW_INITIALIZER("property"), SXML_AST_FIELD_PROPERTY},
    {SXML_BYTE_VIEW_INITIALIZER("specification"),
     SXML_AST_FIELD_SPECIFICATION},
    {SXML_BYTE_VIEW_INITIALIZER("start"), SXML_AST_FIELD_START},
    {SXML_BYTE_VIEW_INITIALIZER("version"), SXML_AST_FIELD_VERSION},
};

/* The child of module (main_module or library_module) has no grammar
   field; the AST names it MODULE (D111). */
static const sxml_ast_unnamed_child_entry sxml_xquery_unnamed_children[] = {
    {.parent_kind = SXML_AST_KIND_MODULE, .field = SXML_AST_FIELD_MODULE},
};

/* A keyword token as written and the keyword detail it selects. */
typedef struct sxml_xquery_keyword_entry {
    sxml_byte_view token;
    sxml_ast_keyword keyword;
} sxml_xquery_keyword_entry;

/* The tokens of the `mode` and `kind` fields (5.3 to 5.8, 5.14, 5.17,
   4.25). */
static const sxml_xquery_keyword_entry sxml_xquery_keywords[] = {
    {SXML_BYTE_VIEW_INITIALIZER("preserve"), SXML_AST_KEYWORD_PRESERVE},
    {SXML_BYTE_VIEW_INITIALIZER("strip"), SXML_AST_KEYWORD_STRIP},
    {SXML_BYTE_VIEW_INITIALIZER("ordered"), SXML_AST_KEYWORD_ORDERED},
    {SXML_BYTE_VIEW_INITIALIZER("unordered"), SXML_AST_KEYWORD_UNORDERED},
    {SXML_BYTE_VIEW_INITIALIZER("greatest"), SXML_AST_KEYWORD_GREATEST},
    {SXML_BYTE_VIEW_INITIALIZER("least"), SXML_AST_KEYWORD_LEAST},
    {SXML_BYTE_VIEW_INITIALIZER("element"), SXML_AST_KEYWORD_ELEMENT},
    {SXML_BYTE_VIEW_INITIALIZER("function"), SXML_AST_KEYWORD_FUNCTION},
    {SXML_BYTE_VIEW_INITIALIZER("value"), SXML_AST_KEYWORD_VALUE},
    {SXML_BYTE_VIEW_INITIALIZER("item"), SXML_AST_KEYWORD_ITEM},
    {SXML_BYTE_VIEW_INITIALIZER("lax"), SXML_AST_KEYWORD_LAX},
    {SXML_BYTE_VIEW_INITIALIZER("strict"), SXML_AST_KEYWORD_STRICT},
};

/* A decimal format property name (5.10, DFPropertyName). */
typedef struct sxml_xquery_property_entry {
    sxml_byte_view name;
    sxml_decimal_format_property_kind property;
} sxml_xquery_property_entry;

static const sxml_xquery_property_entry sxml_xquery_properties[] = {
    {SXML_BYTE_VIEW_INITIALIZER("decimal-separator"),
     SXML_DECIMAL_FORMAT_PROPERTY_DECIMAL_SEPARATOR},
    {SXML_BYTE_VIEW_INITIALIZER("grouping-separator"),
     SXML_DECIMAL_FORMAT_PROPERTY_GROUPING_SEPARATOR},
    {SXML_BYTE_VIEW_INITIALIZER("exponent-separator"),
     SXML_DECIMAL_FORMAT_PROPERTY_EXPONENT_SEPARATOR},
    {SXML_BYTE_VIEW_INITIALIZER("percent"),
     SXML_DECIMAL_FORMAT_PROPERTY_PERCENT},
    {SXML_BYTE_VIEW_INITIALIZER("per-mille"),
     SXML_DECIMAL_FORMAT_PROPERTY_PER_MILLE},
    {SXML_BYTE_VIEW_INITIALIZER("zero-digit"),
     SXML_DECIMAL_FORMAT_PROPERTY_ZERO_DIGIT},
    {SXML_BYTE_VIEW_INITIALIZER("digit"), SXML_DECIMAL_FORMAT_PROPERTY_DIGIT},
    {SXML_BYTE_VIEW_INITIALIZER("pattern-separator"),
     SXML_DECIMAL_FORMAT_PROPERTY_PATTERN_SEPARATOR},
    {SXML_BYTE_VIEW_INITIALIZER("infinity"),
     SXML_DECIMAL_FORMAT_PROPERTY_INFINITY},
    {SXML_BYTE_VIEW_INITIALIZER("NaN"), SXML_DECIMAL_FORMAT_PROPERTY_NAN},
    {SXML_BYTE_VIEW_INITIALIZER("minus-sign"),
     SXML_DECIMAL_FORMAT_PROPERTY_MINUS_SIGN},
};

/* A field token that sets one flag when written as `token` (or, for an
   empty token, whenever the field is present). */
typedef struct sxml_xquery_flag_entry {
    sxml_byte_view field_name;
    sxml_byte_view token;
    sxml_ast_flag flag;
} sxml_xquery_flag_entry;

static const sxml_xquery_flag_entry sxml_xquery_flag_tokens[] = {
    {SXML_BYTE_VIEW_INITIALIZER("allowing_empty"),
     SXML_BYTE_VIEW_INITIALIZER(""), SXML_AST_FLAG_ALLOWING_EMPTY},
    {SXML_BYTE_VIEW_INITIALIZER("fixed"), SXML_BYTE_VIEW_INITIALIZER(""),
     SXML_AST_FLAG_FIXED},
    {SXML_BYTE_VIEW_INITIALIZER("external"), SXML_BYTE_VIEW_INITIALIZER(""),
     SXML_AST_FLAG_EXTERNAL},
    {SXML_BYTE_VIEW_INITIALIZER("stable"), SXML_BYTE_VIEW_INITIALIZER(""),
     SXML_AST_FLAG_STABLE},
    {SXML_BYTE_VIEW_INITIALIZER("only"), SXML_BYTE_VIEW_INITIALIZER(""),
     SXML_AST_FLAG_ONLY},
    {SXML_BYTE_VIEW_INITIALIZER("static"), SXML_BYTE_VIEW_INITIALIZER(""),
     SXML_AST_FLAG_STATIC},
    {SXML_BYTE_VIEW_INITIALIZER("direction"),
     SXML_BYTE_VIEW_INITIALIZER("descending"), SXML_AST_FLAG_DESCENDING},
    {SXML_BYTE_VIEW_INITIALIZER("preserve_mode"),
     SXML_BYTE_VIEW_INITIALIZER("preserve"), SXML_AST_FLAG_PRESERVE_NAMESPACES},
    {SXML_BYTE_VIEW_INITIALIZER("inherit_mode"),
     SXML_BYTE_VIEW_INITIALIZER("inherit"), SXML_AST_FLAG_INHERIT_NAMESPACES},
};

/* ------------------------------------------------------- Error codes */

static const sxml_byte_view sxml_xquery_syntax_error_code =
    SXML_BYTE_VIEW_INITIALIZER("XPST0003");
static const sxml_byte_view sxml_xquery_syntax_error_description =
    SXML_BYTE_VIEW_INITIALIZER("The text is not valid XQuery 4.0 syntax "
                               "(XQuery 4.0, Appendix A)");
static const sxml_byte_view sxml_xquery_record_field_description =
    SXML_BYTE_VIEW_INITIALIZER(
        "The fields of a %constructor record declaration must be NCNames, "
        "not string literals (XQuery 4.0, 5.20.2)");
static const sxml_byte_view sxml_xquery_reference_code =
    SXML_BYTE_VIEW_INITIALIZER("XQST0090");
static const sxml_byte_view sxml_xquery_reference_description =
    SXML_BYTE_VIEW_INITIALIZER("A character reference does not identify an "
                               "XML 1.0 character (XQuery 4.0, 4.3.1.2)");
static const sxml_byte_view sxml_xquery_end_tag_code =
    SXML_BYTE_VIEW_INITIALIZER("XQST0118");
static const sxml_byte_view sxml_xquery_end_tag_description =
    SXML_BYTE_VIEW_INITIALIZER("The end tag name differs from the start tag "
                               "name (XQuery 4.0, 4.12.1)");
static const sxml_byte_view sxml_xquery_duplicate_attribute_code =
    SXML_BYTE_VIEW_INITIALIZER("XQST0040");
static const sxml_byte_view sxml_xquery_duplicate_attribute_description =
    SXML_BYTE_VIEW_INITIALIZER("Two attributes of a direct element "
                               "constructor have the same name (XQuery 4.0, "
                               "4.12.1.1)");
static const sxml_byte_view sxml_xquery_duplicate_namespace_code =
    SXML_BYTE_VIEW_INITIALIZER("XQST0071");
static const sxml_byte_view sxml_xquery_duplicate_namespace_description =
    SXML_BYTE_VIEW_INITIALIZER("Two namespace declaration attributes of a "
                               "direct element constructor have the same "
                               "name (XQuery 4.0, 4.12.1.2)");
static const sxml_byte_view sxml_xquery_enclosed_namespace_code =
    SXML_BYTE_VIEW_INITIALIZER("XQST0022");
static const sxml_byte_view sxml_xquery_enclosed_namespace_description =
    SXML_BYTE_VIEW_INITIALIZER("The value of a namespace declaration "
                               "attribute must not contain an enclosed "
                               "expression (XQuery 4.0, 4.12.1.2)");
static const sxml_byte_view sxml_xquery_reserved_namespace_code =
    SXML_BYTE_VIEW_INITIALIZER("XQST0070");
static const sxml_byte_view sxml_xquery_reserved_namespace_description =
    SXML_BYTE_VIEW_INITIALIZER("A namespace declaration attribute binds the "
                               "prefix xml or xmlns, or the namespace of xml "
                               "or xmlns, otherwise than XML Names allows "
                               "(XQuery 4.0, 4.12.1.2)");
static const sxml_byte_view sxml_xquery_undeclared_prefix_code =
    SXML_BYTE_VIEW_INITIALIZER("XQST0085");
static const sxml_byte_view sxml_xquery_undeclared_prefix_description =
    SXML_BYTE_VIEW_INITIALIZER("A namespace declaration attribute binds a "
                               "prefix to the zero-length URI, which XML "
                               "Names 1.0 does not allow (XQuery 4.0, "
                               "4.12.1.2)");
static const sxml_byte_view sxml_xquery_version_code =
    SXML_BYTE_VIEW_INITIALIZER("XQST0031");
static const sxml_byte_view sxml_xquery_version_description =
    SXML_BYTE_VIEW_INITIALIZER("The version declaration names a version "
                               "other than 1.0, 3.0, 3.1 or 4.0 (XQuery 4.0, "
                               "5.1)");
static const sxml_byte_view sxml_xquery_encoding_code =
    SXML_BYTE_VIEW_INITIALIZER("XQST0087");
static const sxml_byte_view sxml_xquery_encoding_description =
    SXML_BYTE_VIEW_INITIALIZER("The encoding declaration is not an XML "
                               "EncName (XQuery 4.0, 5.1)");

/* Namespace names of XML Names 1.0, section 3 (reserved prefixes). */
static const sxml_byte_view sxml_xquery_xml_namespace_uri =
    SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/XML/1998/namespace");
static const sxml_byte_view sxml_xquery_xmlns_namespace_uri =
    SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/2000/xmlns/");
/* The namespace of unprefixed annotations (XQuery 4.0, 5.15). */
static const sxml_byte_view sxml_xquery_annotation_namespace_uri =
    SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/2012/xquery");

/* ---------------------------------------------------------- Helpers */

static sxml_byte_view sxml_xquery_empty_view(void) {
    return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
}

/* The bytes of view from byte_offset, byte_count of them; both in range. */
static sxml_byte_view sxml_xquery_slice(sxml_byte_view view,
                                        sxml_byte_range range) {
    if (range.byte_count == 0) {
        return sxml_xquery_empty_view();
    }
    return (sxml_byte_view){.bytes = view.bytes + range.byte_offset,
                            .byte_count = range.byte_count};
}

/* XML 1.0 [3] S. */
static bool sxml_xquery_is_whitespace(uint8_t byte) {
    return byte == 0x20U || byte == 0x09U || byte == 0x0DU || byte == 0x0AU;
}

/* XML 1.0 [2] Char: what a character reference may produce (XQST0090). */
static bool sxml_xquery_is_xml_character(uint32_t codepoint) {
    return codepoint == 0x09U || codepoint == 0x0AU || codepoint == 0x0DU ||
           (codepoint >= 0x20U && codepoint <= 0xD7FFU) ||
           (codepoint >= 0xE000U && codepoint <= 0xFFFDU) ||
           (codepoint >= 0x10000U && codepoint <= 0x10FFFFU);
}

/* The text of an AST node's source span, borrowed from the expression. */
static sxml_byte_view
sxml_xquery_written_text(const sxml_ast_front_end *front_end,
                         sxml_ast_source_span span) {
    const sxml_byte_view text =
        sxml_ast_front_end_get_expression_text(front_end);
    if (span.byte_offset > text.byte_count ||
        span.byte_count > text.byte_count - span.byte_offset) {
        return sxml_xquery_empty_view();
    }
    return sxml_xquery_slice(text,
                             (sxml_byte_range){.byte_offset = span.byte_offset,
                                               .byte_count = span.byte_count});
}

/* Raises code with description at byte_offset of the expression text. */
typedef struct sxml_xquery_error {
    sxml_byte_view code;
    sxml_byte_view description;
} sxml_xquery_error;

static sxml_status sxml_xquery_raise_at(sxml_ast_front_end *front_end,
                                        sxml_xquery_error error,
                                        uint32_t byte_offset) {
    return sxml_ast_front_end_raise(
        front_end, (sxml_ast_static_error){.error_code = error.code,
                                           .description = error.description,
                                           .byte_offset = byte_offset});
}

/* Raises error at the start of node. */
typedef struct sxml_xquery_node_error {
    sxml_xquery_error error;
    sxml_ast_node_index node;
} sxml_xquery_node_error;

static sxml_status sxml_xquery_raise_at_node(sxml_ast_front_end *front_end,
                                             sxml_xquery_node_error error) {
    const sxml_ast *ast = sxml_ast_front_end_get_ast(front_end);
    return sxml_xquery_raise_at(
        front_end, error.error,
        sxml_ast_get_node(ast, error.node)->source_span.byte_offset);
}

static sxml_ast_node_index sxml_xquery_child(const sxml_ast *ast,
                                             sxml_ast_node_index parent,
                                             sxml_ast_field field) {
    return sxml_ast_find_child(
        ast, (sxml_ast_child_query){.parent = parent, .field = field});
}

/* ------------------------------------------------------------ Tokens */

static bool sxml_xquery_apply_flag_token(sxml_ast_parser_token token,
                                         sxml_ast_node_request *request) {
    for (size_t entry_index = 0;
         entry_index <
         sizeof sxml_xquery_flag_tokens / sizeof sxml_xquery_flag_tokens[0];
         entry_index += 1U) {
        const sxml_xquery_flag_entry *entry =
            &sxml_xquery_flag_tokens[entry_index];
        if (!sxml_byte_view_equals(token.field_name, entry->field_name)) {
            continue;
        }
        if (entry->token.byte_count == 0 ||
            sxml_byte_view_equals(token.written, entry->token)) {
            request->flags |= (uint32_t)entry->flag;
        }
        return true;
    }
    return false;
}

static sxml_ast_keyword sxml_xquery_keyword_of(sxml_byte_view written) {
    for (size_t entry_index = 0;
         entry_index <
         sizeof sxml_xquery_keywords / sizeof sxml_xquery_keywords[0];
         entry_index += 1U) {
        if (sxml_byte_view_equals(written,
                                  sxml_xquery_keywords[entry_index].token)) {
            return sxml_xquery_keywords[entry_index].keyword;
        }
    }
    return SXML_AST_KEYWORD_NONE;
}

static sxml_decimal_format_property_kind
sxml_xquery_property_of(sxml_byte_view written) {
    for (size_t entry_index = 0;
         entry_index <
         sizeof sxml_xquery_properties / sizeof sxml_xquery_properties[0];
         entry_index += 1U) {
        if (sxml_byte_view_equals(written,
                                  sxml_xquery_properties[entry_index].name)) {
            return sxml_xquery_properties[entry_index].property;
        }
    }
    return SXML_DECIMAL_FORMAT_PROPERTY_DECIMAL_SEPARATOR;
}

/*
 * The apply_token hook (D111): the XQuery tokens become detail and flags
 * (src/xpath_ast_internal.h, sxml_ast_kind). Tokens it does not claim
 * follow the XPath rules.
 */
static bool sxml_xquery_apply_token(sxml_ast_parser_token token,
                                    sxml_ast_node_request *request) {
    const sxml_ast_kind kind = request->kind;
    if (sxml_byte_view_equals(token.field_name,
                              SXML_BYTE_VIEW_FROM_LITERAL("empty")) &&
        kind == SXML_AST_KIND_ORDER_SPECIFICATION) {
        /* OrderModifier: "empty" ("greatest" | "least"), 4.13.9. */
        request->flags |=
            sxml_byte_view_equals(token.written,
                                  SXML_BYTE_VIEW_FROM_LITERAL("greatest"))
                ? (uint32_t)SXML_AST_FLAG_EMPTY_GREATEST
                : (uint32_t)SXML_AST_FLAG_EMPTY_LEAST;
        return true;
    }
    if (sxml_byte_view_equals(token.field_name,
                              SXML_BYTE_VIEW_FROM_LITERAL("mode")) ||
        (sxml_byte_view_equals(token.field_name,
                               SXML_BYTE_VIEW_FROM_LITERAL("kind")) &&
         (kind == SXML_AST_KIND_DEFAULT_NAMESPACE_DECLARATION ||
          kind == SXML_AST_KIND_CONTEXT_VALUE_DECLARATION))) {
        request->detail.keyword = sxml_xquery_keyword_of(token.written);
        return true;
    }
    if (sxml_byte_view_equals(token.field_name,
                              SXML_BYTE_VIEW_FROM_LITERAL("name")) &&
        kind == SXML_AST_KIND_DECIMAL_FORMAT_PROPERTY) {
        request->detail.decimal_format_property =
            sxml_xquery_property_of(token.written);
        return true;
    }
    return sxml_xquery_apply_flag_token(token, request);
}

/* ---------------------------------------------------------- Decoding */

/*
 * What a decoder replaces in literal text, besides line breaks, which it
 * always normalizes (A.3.3.1: CR LF and a lone CR become LF before
 * parsing).
 */
typedef struct sxml_xquery_decode_rules {
    /* A doubled delimiter stands for one ("" in "...", '' in '...',
       `` in a string template); 0 for none. */
    uint8_t delimiter;
    /* "{{" and "}}" stand for "{" and "}" (4.12.1, 4.9.3). */
    bool unescapes_braces;
    /* Predefined entity and character references are replaced
       (4.3.1.2; XQST0090 for a character XML 1.0 does not allow). */
    bool replaces_references;
    /* Each literal tab, line feed and carriage return becomes a space:
       attribute value normalization (XML 1.0, 3.3.3, CDATA), after line
       breaks are normalized. */
    bool normalizes_whitespace;
} sxml_xquery_decode_rules;

/* Text to decode: source, which starts at source_byte_offset of the
   expression text (for error locations). */
typedef struct sxml_xquery_decode_request {
    sxml_byte_view source;
    uint32_t source_byte_offset;
    sxml_xquery_decode_rules rules;
} sxml_xquery_decode_request;

/* The state of one decoding: the text written so far and the start of the
   source bytes not yet copied. */
typedef struct sxml_xquery_decoder {
    sxml_ast *ast;
    sxml_byte_view source;
    size_t piece_start;
    sxml_ast_text text;
} sxml_xquery_decoder;

/* Copies the source bytes from piece_start up to end, then moves
   piece_start to resume. */
typedef struct sxml_xquery_flush {
    size_t end;
    size_t resume;
} sxml_xquery_flush;

static sxml_status sxml_xquery_flush_piece(sxml_xquery_decoder *decoder,
                                           sxml_xquery_flush flush) {
    sxml_status status = SXML_STATUS_OK;
    if (flush.end > decoder->piece_start) {
        status = sxml_ast_append_text(
            decoder->ast,
            sxml_xquery_slice(
                decoder->source,
                (sxml_byte_range){.byte_offset = decoder->piece_start,
                                  .byte_count =
                                      flush.end - decoder->piece_start}),
            &decoder->text);
    }
    decoder->piece_start = flush.resume;
    return status;
}

/* Appends replacement bytes (a decoded character) to the text. */
static sxml_status sxml_xquery_append_bytes(sxml_xquery_decoder *decoder,
                                            sxml_byte_view bytes) {
    return sxml_ast_append_text(decoder->ast, bytes, &decoder->text);
}

/* Writes codepoint (at most U+10FFFF) as UTF-8 into encoded; returns the
   byte count. */
typedef struct sxml_xquery_utf8 {
    uint8_t bytes[4];
} sxml_xquery_utf8;

static size_t sxml_xquery_encode_utf8(uint32_t codepoint,
                                      sxml_xquery_utf8 *encoded) {
    if (codepoint < 0x80U) {
        encoded->bytes[0] = (uint8_t)codepoint;
        return 1;
    }
    if (codepoint < 0x800U) {
        encoded->bytes[0] = (uint8_t)(0xC0U | (codepoint >> 6U));
        encoded->bytes[1] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        return 2;
    }
    if (codepoint < 0x10000U) {
        encoded->bytes[0] = (uint8_t)(0xE0U | (codepoint >> 12U));
        encoded->bytes[1] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
        encoded->bytes[2] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        return 3;
    }
    encoded->bytes[0] = (uint8_t)(0xF0U | (codepoint >> 18U));
    encoded->bytes[1] = (uint8_t)(0x80U | ((codepoint >> 12U) & 0x3FU));
    encoded->bytes[2] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
    encoded->bytes[3] = (uint8_t)(0x80U | (codepoint & 0x3FU));
    return 4;
}

/* A reference read at "&": the character and the bytes it spans; a
   byte_count of 0 when the text there is no well-formed reference. */
typedef struct sxml_xquery_reference {
    uint32_t codepoint;
    size_t byte_count;
} sxml_xquery_reference;

/* The value of a hexadecimal or decimal digit; 16 for any other byte. */
static uint32_t sxml_xquery_digit_value(uint8_t byte) {
    if (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') {
        return (uint32_t)(byte - (uint8_t)'0');
    }
    if (byte >= (uint8_t)'a' && byte <= (uint8_t)'f') {
        return (uint32_t)(byte - (uint8_t)'a') + 10U;
    }
    if (byte >= (uint8_t)'A' && byte <= (uint8_t)'F') {
        return (uint32_t)(byte - (uint8_t)'A') + 10U;
    }
    return 16;
}

/* XML 1.0 [66] CharRef after "&#": digits and ";". A value above U+10FFFF
   is kept as 0x110000, which no check accepts. */
static sxml_xquery_reference
sxml_xquery_read_character_reference(sxml_byte_view rest) {
    const sxml_xquery_reference malformed = {.codepoint = 0, .byte_count = 0};
    size_t byte_index = 2;
    uint32_t base = 10;
    if (byte_index < rest.byte_count && rest.bytes[byte_index] == 'x') {
        base = 16;
        byte_index += 1U;
    }
    const size_t digits_start = byte_index;
    uint32_t codepoint = 0;
    while (byte_index < rest.byte_count) {
        const uint32_t digit = sxml_xquery_digit_value(rest.bytes[byte_index]);
        if (digit >= base) {
            break;
        }
        codepoint = codepoint * base + digit;
        if (codepoint > 0x10FFFFU) {
            codepoint = 0x110000U;
        }
        byte_index += 1U;
    }
    if (byte_index == digits_start || byte_index >= rest.byte_count ||
        rest.bytes[byte_index] != ';') {
        return malformed;
    }
    return (sxml_xquery_reference){.codepoint = codepoint,
                                   .byte_count = byte_index + 1U};
}

/* A predefined entity reference (4.3.1.2) and its character. */
typedef struct sxml_xquery_entity_entry {
    sxml_byte_view reference;
    uint32_t codepoint;
} sxml_xquery_entity_entry;

static const sxml_xquery_entity_entry sxml_xquery_entities[] = {
    {SXML_BYTE_VIEW_INITIALIZER("&lt;"), '<'},
    {SXML_BYTE_VIEW_INITIALIZER("&gt;"), '>'},
    {SXML_BYTE_VIEW_INITIALIZER("&amp;"), '&'},
    {SXML_BYTE_VIEW_INITIALIZER("&quot;"), '"'},
    {SXML_BYTE_VIEW_INITIALIZER("&apos;"), '\''},
};

/* The reference at the start of rest, which starts with "&". */
static sxml_xquery_reference sxml_xquery_read_reference(sxml_byte_view rest) {
    if (rest.bytes == NULL) {
        return (sxml_xquery_reference){.codepoint = 0, .byte_count = 0};
    }
    if (rest.byte_count >= 2U && rest.bytes[1] == '#') {
        return sxml_xquery_read_character_reference(rest);
    }
    for (size_t entry_index = 0;
         entry_index <
         sizeof sxml_xquery_entities / sizeof sxml_xquery_entities[0];
         entry_index += 1U) {
        const sxml_byte_view reference =
            sxml_xquery_entities[entry_index].reference;
        if (rest.byte_count >= reference.byte_count &&
            memcmp(rest.bytes, reference.bytes, reference.byte_count) == 0) {
            return (sxml_xquery_reference){
                .codepoint = sxml_xquery_entities[entry_index].codepoint,
                .byte_count = reference.byte_count};
        }
    }
    return (sxml_xquery_reference){.codepoint = 0, .byte_count = 0};
}

/* Replaces the reference at byte_index of the decoder's source; writes the
   bytes it spans to *consumed. */
typedef struct sxml_xquery_reference_site {
    sxml_ast_front_end *front_end;
    size_t byte_index;
    uint32_t source_byte_offset;
} sxml_xquery_reference_site;

static sxml_status
sxml_xquery_replace_reference(sxml_xquery_decoder *decoder,
                              sxml_xquery_reference_site site,
                              size_t *consumed) {
    const sxml_byte_view source = decoder->source;
    const sxml_xquery_reference reference =
        sxml_xquery_read_reference(sxml_xquery_slice(
            source,
            (sxml_byte_range){.byte_offset = site.byte_index,
                              .byte_count =
                                  source.byte_count - site.byte_index}));
    const uint32_t error_offset =
        site.source_byte_offset + (uint32_t)site.byte_index;
    if (reference.byte_count == 0) {
        /* The grammar and the scanner accept only well-formed
           references; anything else is not XQuery. */
        return sxml_xquery_raise_at(
            site.front_end,
            (sxml_xquery_error){.code = sxml_xquery_syntax_error_code,
                                .description =
                                    sxml_xquery_syntax_error_description},
            error_offset);
    }
    if (!sxml_xquery_is_xml_character(reference.codepoint)) {
        return sxml_xquery_raise_at(
            site.front_end,
            (sxml_xquery_error){.code = sxml_xquery_reference_code,
                                .description =
                                    sxml_xquery_reference_description},
            error_offset);
    }
    sxml_status status = sxml_xquery_flush_piece(
        decoder,
        (sxml_xquery_flush){.end = site.byte_index,
                            .resume = site.byte_index + reference.byte_count});
    sxml_xquery_utf8 encoded = {.bytes = {0, 0, 0, 0}};
    const size_t encoded_byte_count =
        sxml_xquery_encode_utf8(reference.codepoint, &encoded);
    if (status == SXML_STATUS_OK) {
        status = sxml_xquery_append_bytes(
            decoder, (sxml_byte_view){.bytes = encoded.bytes,
                                      .byte_count = encoded_byte_count});
    }
    *consumed = reference.byte_count;
    return status;
}

/*
 * Decodes request->source into a new text of the AST by request->rules.
 * The decoded text is never longer than the source, so the AST's text
 * capacity holds (D52).
 */
static sxml_status
sxml_xquery_store_decoded(sxml_ast_front_end *front_end,
                          const sxml_xquery_decode_request *request,
                          sxml_ast_text *text) {
    sxml_xquery_decoder decoder = {
        .ast = sxml_ast_front_end_get_ast(front_end),
        .source = request->source,
        .piece_start = 0,
        .text = {.byte_offset = 0, .byte_count = 0},
    };
    const sxml_xquery_decode_rules rules = request->rules;
    /* A new text starts even when the decoded text is empty. */
    sxml_status status =
        sxml_xquery_append_bytes(&decoder, sxml_xquery_empty_view());
    const sxml_byte_view source = request->source;
    const sxml_byte_view space = SXML_BYTE_VIEW_FROM_LITERAL(" ");
    const sxml_byte_view line_feed = SXML_BYTE_VIEW_FROM_LITERAL("\n");
    size_t byte_index = 0;
    while (status == SXML_STATUS_OK && source.bytes != NULL &&
           byte_index < source.byte_count) {
        const uint8_t byte = source.bytes[byte_index];
        const bool is_doubled =
            byte_index + 1U < source.byte_count &&
            source.bytes[byte_index + 1U] == byte &&
            ((rules.delimiter != 0 && byte == rules.delimiter) ||
             (rules.unescapes_braces && (byte == '{' || byte == '}')));
        if (byte == '\r') {
            const bool is_crlf = byte_index + 1U < source.byte_count &&
                                 source.bytes[byte_index + 1U] == '\n';
            status = sxml_xquery_flush_piece(
                &decoder, (sxml_xquery_flush){.end = byte_index,
                                              .resume = byte_index + 1U});
            /* CR LF: the LF that follows stands for both (A.3.3.1). */
            if (status == SXML_STATUS_OK && !is_crlf) {
                status = sxml_xquery_append_bytes(
                    &decoder, rules.normalizes_whitespace ? space : line_feed);
            }
            byte_index += 1U;
        } else if (rules.normalizes_whitespace &&
                   (byte == '\n' || byte == '\t')) {
            status = sxml_xquery_flush_piece(
                &decoder, (sxml_xquery_flush){.end = byte_index,
                                              .resume = byte_index + 1U});
            if (status == SXML_STATUS_OK) {
                status = sxml_xquery_append_bytes(&decoder, space);
            }
            byte_index += 1U;
        } else if (is_doubled) {
            /* Keep the first of the pair, skip the second. */
            status = sxml_xquery_flush_piece(
                &decoder, (sxml_xquery_flush){.end = byte_index + 1U,
                                              .resume = byte_index + 2U});
            byte_index += 2U;
        } else if (byte == '&' && rules.replaces_references) {
            size_t consumed = 1;
            status = sxml_xquery_replace_reference(
                &decoder,
                (sxml_xquery_reference_site){
                    .front_end = front_end,
                    .byte_index = byte_index,
                    .source_byte_offset = request->source_byte_offset},
                &consumed);
            byte_index += consumed;
        } else {
            byte_index += 1U;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xquery_flush_piece(
            &decoder, (sxml_xquery_flush){.end = source.byte_count,
                                          .resume = source.byte_count});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *text = decoder.text;
    return SXML_STATUS_OK;
}

/* The bytes of written between a prefix and a suffix of the given sizes
   (the delimiters of a token); empty when written is shorter. */
typedef struct sxml_xquery_delimiters {
    size_t opening_byte_count;
    size_t closing_byte_count;
} sxml_xquery_delimiters;

static sxml_byte_view sxml_xquery_inside(sxml_byte_view written,
                                         sxml_xquery_delimiters delimiters) {
    const size_t delimiter_byte_count =
        delimiters.opening_byte_count + delimiters.closing_byte_count;
    if (written.byte_count < delimiter_byte_count) {
        return sxml_xquery_empty_view();
    }
    return sxml_xquery_slice(
        written,
        (sxml_byte_range){.byte_offset = delimiters.opening_byte_count,
                          .byte_count =
                              written.byte_count - delimiter_byte_count});
}

/*
 * The delimiter of the direct attribute value whose content starts at
 * byte_offset: the first byte of its DIRECT_ATTRIBUTE_VALUE node. That node
 * is the walk's current parent, an ancestor of the node added last (or
 * that node), and the first node on the way up whose span contains the
 * content.
 */
static uint8_t
sxml_xquery_attribute_delimiter(sxml_ast_front_end *front_end,
                                uint32_t byte_offset) {
    const sxml_ast *ast = sxml_ast_front_end_get_ast(front_end);
    const sxml_byte_view text =
        sxml_ast_front_end_get_expression_text(front_end);
    sxml_ast_node_index candidate = {.value = ast->node_count - 1U};
    while (candidate.value != 0) {
        const sxml_ast_node *node = sxml_ast_get_node(ast, candidate);
        const sxml_ast_source_span span = node->source_span;
        if (node->kind == SXML_AST_KIND_DIRECT_ATTRIBUTE_VALUE &&
            span.byte_offset <= byte_offset &&
            byte_offset - span.byte_offset < span.byte_count &&
            span.byte_offset < text.byte_count) {
            return text.bytes[span.byte_offset];
        }
        candidate = node->parent;
    }
    return (uint8_t)'"';
}

/* Decodes the inside of written by rules into request's payload text. */
typedef struct sxml_xquery_text_payload {
    sxml_byte_view inside;
    sxml_xquery_decode_rules rules;
} sxml_xquery_text_payload;

static sxml_status
sxml_xquery_describe_text(sxml_ast_parser_node node,
                          sxml_xquery_text_payload payload,
                          sxml_ast_node_request *request) {
    const uint32_t inside_offset =
        request->source_span.byte_offset +
        (uint32_t)(payload.inside.bytes == NULL
                       ? 0
                       : (size_t)(payload.inside.bytes - node.written.bytes));
    return sxml_xquery_store_decoded(
        node.front_end,
        &(sxml_xquery_decode_request){.source = payload.inside,
                                      .source_byte_offset = inside_offset,
                                      .rules = payload.rules},
        &request->payload.text);
}

/* No escapes; only line breaks are normalized. */
static sxml_xquery_decode_rules sxml_xquery_plain_rules(void) {
    return (sxml_xquery_decode_rules){.delimiter = 0,
                                      .unescapes_braces = false,
                                      .replaces_references = false,
                                      .normalizes_whitespace = false};
}

/*
 * A direct comment: the text between "<!--" and "-->" (4.12.2). The
 * scanner already enforces DirCommentContents: a "--" only before ">" and
 * no "-" just before it.
 */
static sxml_status sxml_xquery_describe_comment(sxml_ast_parser_node node,
                                                sxml_ast_node_request *request) {
    return sxml_xquery_describe_text(
        node,
        (sxml_xquery_text_payload){
            .inside = sxml_xquery_inside(
                node.written, (sxml_xquery_delimiters){.opening_byte_count = 4,
                                                       .closing_byte_count = 3}),
            .rules = sxml_xquery_plain_rules()},
        request);
}

/*
 * A direct processing instruction "<?" PITarget (S DirPIContents)? "?>"
 * (4.12.2): the target, and the content after the whitespace that follows
 * the target. The scanner rejects the target "xml" in any case (XPST0003).
 */
static sxml_status
sxml_xquery_describe_processing_instruction(sxml_ast_parser_node node,
                                            sxml_ast_node_request *request) {
    const sxml_byte_view inside = sxml_xquery_inside(
        node.written, (sxml_xquery_delimiters){.opening_byte_count = 2,
                                               .closing_byte_count = 2});
    size_t target_end = 0;
    while (target_end < inside.byte_count &&
           !sxml_xquery_is_whitespace(inside.bytes[target_end])) {
        target_end += 1U;
    }
    size_t content_start = target_end;
    while (content_start < inside.byte_count &&
           sxml_xquery_is_whitespace(inside.bytes[content_start])) {
        content_start += 1U;
    }
    sxml_ast *ast = sxml_ast_front_end_get_ast(node.front_end);
    sxml_ast_processing_instruction stored = {
        .target = {.byte_offset = 0, .byte_count = 0},
        .content = {.byte_offset = 0, .byte_count = 0}};
    sxml_status status = sxml_ast_append_text(
        ast,
        sxml_xquery_slice(inside, (sxml_byte_range){.byte_offset = 0,
                                                    .byte_count = target_end}),
        &stored.target);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ast_node_request content_request = *request;
    status = sxml_xquery_describe_text(
        node,
        (sxml_xquery_text_payload){
            .inside = sxml_xquery_slice(
                inside,
                (sxml_byte_range){.byte_offset = content_start,
                                  .byte_count =
                                      inside.byte_count - content_start}),
            .rules = sxml_xquery_plain_rules()},
        &content_request);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    stored.content = content_request.payload.text;
    request->payload.processing_instruction = stored;
    return SXML_STATUS_OK;
}

/* Pragma contents without the whitespace that separates them from the
   name (4.26: Pragma ::= "(#" S EQName (S PragmaContents)? "#)"). */
static sxml_status
sxml_xquery_describe_pragma_contents(sxml_ast_parser_node node,
                                     sxml_ast_node_request *request) {
    size_t contents_start = 0;
    while (contents_start < node.written.byte_count &&
           sxml_xquery_is_whitespace(node.written.bytes[contents_start])) {
        contents_start += 1U;
    }
    return sxml_xquery_describe_text(
        node,
        (sxml_xquery_text_payload){
            .inside = sxml_xquery_inside(
                node.written,
                (sxml_xquery_delimiters){.opening_byte_count = contents_start,
                                         .closing_byte_count = 0}),
            .rules = sxml_xquery_plain_rules()},
        request);
}

static bool sxml_xquery_contains_byte(sxml_byte_view view, uint8_t byte) {
    return view.byte_count != 0 &&
           memchr(view.bytes, byte, view.byte_count) != NULL;
}

/*
 * A URIQualifiedName or a "Q{uri}*" wildcard whose BracedURILiteral holds a
 * reference (XQuery 4.0, A.3.1 BracedURILiteral allows PredefinedEntityRef
 * and CharRef): the URI decoded, the rest as the XPath decoder stores it.
 */
static sxml_status
sxml_xquery_describe_braced_name(sxml_ast_parser_node node,
                                 sxml_ast_node_request *request) {
    const sxml_byte_view written = node.written;
    size_t uri_end = 2;
    while (uri_end < written.byte_count && written.bytes[uri_end] != '}') {
        uri_end += 1U;
    }
    sxml_ast_name stored = {
        .namespace_prefix = {.byte_offset = 0, .byte_count = 0},
        .local_name = {.byte_offset = 0, .byte_count = 0},
        .namespace_uri = {.byte_offset = 0, .byte_count = 0},
    };
    sxml_status status = sxml_xquery_store_decoded(
        node.front_end,
        &(sxml_xquery_decode_request){
            .source = sxml_xquery_slice(
                written, (sxml_byte_range){.byte_offset = 2,
                                           .byte_count = uri_end - 2U}),
            .source_byte_offset = request->source_span.byte_offset + 2U,
            .rules = {.delimiter = 0,
                      .unescapes_braces = false,
                      .replaces_references = true,
                      .normalizes_whitespace = false}},
        &stored.namespace_uri);
    const size_t rest_start =
        uri_end < written.byte_count ? uri_end + 1U : uri_end;
    const sxml_byte_view rest = sxml_xquery_slice(
        written,
        (sxml_byte_range){.byte_offset = rest_start,
                          .byte_count = written.byte_count - rest_start});
    size_t colon = 0;
    while (colon < rest.byte_count && rest.bytes[colon] != ':') {
        colon += 1U;
    }
    sxml_ast *ast = sxml_ast_front_end_get_ast(node.front_end);
    sxml_byte_view local_name = rest;
    if (status == SXML_STATUS_OK && colon < rest.byte_count) {
        status = sxml_ast_append_text(
            ast,
            sxml_xquery_slice(rest, (sxml_byte_range){.byte_offset = 0,
                                                      .byte_count = colon}),
            &stored.namespace_prefix);
        local_name = sxml_xquery_slice(
            rest, (sxml_byte_range){.byte_offset = colon + 1U,
                                    .byte_count = rest.byte_count - colon - 1U});
    }
    if (request->kind == SXML_AST_KIND_WILDCARD) {
        /* "Q{uri}*" (XPath 4.0, 4.6.4.2): no local name. */
        request->detail.name_test_kind = SXML_NAME_TEST_KIND_NAMESPACE_WILDCARD;
        request->flags |= (uint32_t)SXML_AST_FLAG_BRACED_URI;
        local_name = sxml_xquery_empty_view();
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_ast_append_text(ast, local_name, &stored.local_name);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    request->payload.name = stored;
    return SXML_STATUS_OK;
}

/* The decoding rules and the inside of a literal text kind; false for the
   kinds the XPath decoders or the functions above describe. */
typedef struct sxml_xquery_text_kind {
    bool is_text_kind;
    sxml_xquery_text_payload payload;
} sxml_xquery_text_kind;

static sxml_xquery_text_kind
sxml_xquery_text_kind_of(sxml_ast_parser_node node,
                         const sxml_ast_node_request *request) {
    const sxml_byte_view written = node.written;
    sxml_xquery_text_kind result = {
        .is_text_kind = true,
        .payload = {.inside = written, .rules = sxml_xquery_plain_rules()}};
    const sxml_ast_kind kind = request->kind;
    if (kind == SXML_AST_KIND_STRING_LITERAL) {
        /* 4.3.1.2: the delimiters off, doubled quotes and references. */
        result.payload.inside = sxml_xquery_inside(
            written, (sxml_xquery_delimiters){.opening_byte_count = 1,
                                              .closing_byte_count = 1});
        result.payload.rules.delimiter =
            written.byte_count == 0 ? 0 : written.bytes[0];
        result.payload.rules.replaces_references = true;
    } else if (kind == SXML_AST_KIND_STRING_TEMPLATE_FIXED_PART) {
        /* XPath 4.0, 4.9.3: "{{", "}}" and "``". */
        result.payload.rules.delimiter = (uint8_t)'`';
        result.payload.rules.unescapes_braces = true;
    } else if (kind == SXML_AST_KIND_ATTRIBUTE_VALUE_TEXT) {
        /* 4.12.1.1, rule 1, then attribute value normalization. */
        result.payload.rules.delimiter = sxml_xquery_attribute_delimiter(
            node.front_end, request->source_span.byte_offset);
        result.payload.rules.unescapes_braces = true;
        result.payload.rules.replaces_references = true;
        result.payload.rules.normalizes_whitespace = true;
    } else if (kind == SXML_AST_KIND_DIRECT_ELEMENT_TEXT) {
        /* 4.12.1.3: "{{", "}}" and references in element content. */
        result.payload.rules.unescapes_braces = true;
        result.payload.rules.replaces_references = true;
    } else if (kind == SXML_AST_KIND_CDATA_SECTION) {
        result.payload.inside = sxml_xquery_inside(
            written, (sxml_xquery_delimiters){.opening_byte_count = 9,
                                              .closing_byte_count = 3});
    } else if (kind != SXML_AST_KIND_STRING_CONSTRUCTOR_CHARACTERS) {
        /* String constructor characters (4.9.4) have no escapes; every
           other kind is not literal text. */
        result.is_text_kind = false;
    }
    return result;
}

/*
 * The describe_payload hook (D111): XQuery's decoding of literal text,
 * which XPath does not have (references, direct constructor text, line
 * breaks); the XPath decoders describe every other kind.
 */
static sxml_status
sxml_xquery_describe_payload(sxml_ast_parser_node node,
                             sxml_ast_node_request *request,
                             sxml_ast_payload_outcome *outcome) {
    const sxml_ast_kind kind = request->kind;
    sxml_status status = SXML_STATUS_OK;
    bool is_described = true;
    if (kind == SXML_AST_KIND_DIRECT_COMMENT_CONSTRUCTOR) {
        status = sxml_xquery_describe_comment(node, request);
    } else if (kind == SXML_AST_KIND_DIRECT_PROCESSING_INSTRUCTION_CONSTRUCTOR) {
        status = sxml_xquery_describe_processing_instruction(node, request);
    } else if (kind == SXML_AST_KIND_PRAGMA_CONTENTS) {
        status = sxml_xquery_describe_pragma_contents(node, request);
    } else if ((kind == SXML_AST_KIND_URI_QUALIFIED_NAME ||
                kind == SXML_AST_KIND_WILDCARD) &&
               node.written.byte_count >= 2U && node.written.bytes[0] == 'Q' &&
               node.written.bytes[1] == '{' &&
               sxml_xquery_contains_byte(node.written, (uint8_t)'&')) {
        status = sxml_xquery_describe_braced_name(node, request);
    } else {
        const sxml_xquery_text_kind text_kind =
            sxml_xquery_text_kind_of(node, request);
        is_described = text_kind.is_text_kind;
        if (is_described) {
            status = sxml_xquery_describe_text(node, text_kind.payload, request);
        }
    }
    if (status == SXML_STATUS_OK) {
        outcome->is_described = is_described;
    }
    return status;
}

/* ------------------------------------------------- Direct constructors */

/* A name as written in a tag (QNAME payload): prefix and local name. */
typedef struct sxml_xquery_written_name {
    sxml_byte_view prefix;
    sxml_byte_view local_name;
} sxml_xquery_written_name;

static sxml_xquery_written_name
sxml_xquery_written_name_of(const sxml_ast *ast, sxml_ast_node_index node) {
    const sxml_ast_name name = sxml_ast_get_node(ast, node)->payload.name;
    return (sxml_xquery_written_name){
        .prefix = sxml_ast_get_text(ast, name.namespace_prefix),
        .local_name = sxml_ast_get_text(ast, name.local_name)};
}

static bool sxml_xquery_names_equal(sxml_xquery_written_name first,
                                    sxml_xquery_written_name second) {
    return sxml_byte_view_equals(first.prefix, second.prefix) &&
           sxml_byte_view_equals(first.local_name, second.local_name);
}

/* The written name of an attribute and whether it is a namespace
   declaration attribute; the sort key for duplicate checks. */
typedef struct sxml_xquery_attribute_key {
    bool is_namespace_declaration;
    sxml_xquery_written_name name;
} sxml_xquery_attribute_key;

static sxml_xquery_attribute_key
sxml_xquery_attribute_key_of(const sxml_ast *ast,
                             sxml_ast_node_index attribute) {
    return (sxml_xquery_attribute_key){
        .is_namespace_declaration =
            (sxml_ast_get_node(ast, attribute)->flags &
             (uint32_t)SXML_AST_FLAG_NAMESPACE_DECLARATION_ATTRIBUTE) != 0,
        .name = sxml_xquery_written_name_of(
            ast, sxml_xquery_child(ast, attribute, SXML_AST_FIELD_NAME))};
}

/* Orders two byte strings as memcmp, the shorter first on a common
   prefix. */
static int sxml_xquery_compare_bytes(sxml_byte_view first,
                                     sxml_byte_view second) {
    const size_t common = first.byte_count < second.byte_count
                              ? first.byte_count
                              : second.byte_count;
    const int comparison =
        common == 0 ? 0 : memcmp(first.bytes, second.bytes, common);
    if (comparison != 0 || first.byte_count == second.byte_count) {
        return comparison;
    }
    return first.byte_count < second.byte_count ? -1 : 1;
}

static int sxml_xquery_compare_keys(sxml_xquery_attribute_key first,
                                    sxml_xquery_attribute_key second) {
    if (first.is_namespace_declaration != second.is_namespace_declaration) {
        return first.is_namespace_declaration ? 1 : -1;
    }
    const int comparison =
        sxml_xquery_compare_bytes(first.name.prefix, second.name.prefix);
    if (comparison != 0) {
        return comparison;
    }
    return sxml_xquery_compare_bytes(first.name.local_name,
                                     second.name.local_name);
}

/* Attributes of one element, as node indices in a work array. */
typedef struct sxml_xquery_attribute_list {
    const sxml_ast *ast;
    uint32_t *attributes;
    size_t attribute_count;
} sxml_xquery_attribute_list;

/* Orders attributes (uint32_t node indices) by key, then by source order
   (node index). state is the AST. */
static bool sxml_xquery_attribute_is_before(const void *state, sxml_sort_pair pair) {
    const sxml_ast *ast = state;
    const uint32_t first = *(const uint32_t *)pair.first;
    const uint32_t second = *(const uint32_t *)pair.second;
    const int comparison = sxml_xquery_compare_keys(
        sxml_xquery_attribute_key_of(ast, (sxml_ast_node_index){.value = first}),
        sxml_xquery_attribute_key_of(ast, (sxml_ast_node_index){.value = second}));
    return comparison < 0 || (comparison == 0 && first < second);
}

static void sxml_xquery_sort_attributes(sxml_xquery_attribute_list list) {
    sxml_sort_in_place(&(sxml_sort_request){.elements = list.attributes,
                                            .element_count = list.attribute_count,
                                            .element_size = sizeof(uint32_t),
                                            .is_before = sxml_xquery_attribute_is_before,
                                            .state = list.ast});
}

/* The second attribute of the earliest duplicate name in source order;
   index 0 when every name is distinct. */
typedef struct sxml_xquery_duplicate {
    sxml_ast_node_index attribute;
} sxml_xquery_duplicate;

/* Few attributes are compared pairwise; more are sorted first. */
enum { SXML_XQUERY_PAIRWISE_ATTRIBUTE_COUNT = 32 };

static sxml_xquery_duplicate
sxml_xquery_find_duplicate_pairwise(const sxml_ast *ast,
                                    sxml_ast_node_index element) {
    for (sxml_ast_node_index later =
             sxml_xquery_child(ast, element, SXML_AST_FIELD_ATTRIBUTE);
         later.value != 0;
         later = sxml_ast_find_next_with_same_field(ast, later)) {
        const sxml_xquery_attribute_key later_key =
            sxml_xquery_attribute_key_of(ast, later);
        for (sxml_ast_node_index earlier =
                 sxml_xquery_child(ast, element, SXML_AST_FIELD_ATTRIBUTE);
             earlier.value != later.value;
             earlier = sxml_ast_find_next_with_same_field(ast, earlier)) {
            if (sxml_xquery_compare_keys(
                    sxml_xquery_attribute_key_of(ast, earlier), later_key) ==
                0) {
                return (sxml_xquery_duplicate){.attribute = later};
            }
        }
    }
    return (sxml_xquery_duplicate){.attribute = {.value = 0}};
}

/* Sorts the attributes in a work array from the AST's allocator. */
typedef struct sxml_xquery_attribute_set {
    sxml_ast_node_index element;
    size_t attribute_count;
} sxml_xquery_attribute_set;

static sxml_status
sxml_xquery_find_duplicate_sorted(const sxml_ast *ast,
                                  sxml_xquery_attribute_set set,
                                  sxml_xquery_duplicate *duplicate) {
    void *memory = NULL;
    /* attribute_count is below the AST's node count, a uint32_t, so the
       size fits size_t. */
    sxml_status status = sxml_allocator_allocate(
        ast->allocator,
        (sxml_allocation_request){
            .byte_count = set.attribute_count * sizeof(uint32_t),
            .alignment = _Alignof(uint32_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xquery_attribute_list list = {.ast = ast,
                                       .attributes = memory,
                                       .attribute_count = set.attribute_count};
    size_t entry_index = 0;
    for (sxml_ast_node_index attribute =
             sxml_xquery_child(ast, set.element, SXML_AST_FIELD_ATTRIBUTE);
         attribute.value != 0 && entry_index < set.attribute_count;
         attribute = sxml_ast_find_next_with_same_field(ast, attribute)) {
        list.attributes[entry_index] = attribute.value;
        entry_index += 1U;
    }
    sxml_xquery_sort_attributes(list);
    sxml_xquery_duplicate found = {.attribute = {.value = 0}};
    for (entry_index = 1; entry_index < set.attribute_count;
         entry_index += 1U) {
        const uint32_t previous = list.attributes[entry_index - 1U];
        const uint32_t current = list.attributes[entry_index];
        const bool is_equal =
            sxml_xquery_compare_keys(
                sxml_xquery_attribute_key_of(
                    ast, (sxml_ast_node_index){.value = previous}),
                sxml_xquery_attribute_key_of(
                    ast, (sxml_ast_node_index){.value = current})) == 0;
        /* Equal keys are in source order, so current is a later
           occurrence; keep the earliest such. */
        if (is_equal &&
            (found.attribute.value == 0 || current < found.attribute.value)) {
            found.attribute.value = current;
        }
    }
    sxml_allocator_release(ast->allocator, memory);
    *duplicate = found;
    return SXML_STATUS_OK;
}

/*
 * XQST0040 and XQST0071: the attributes of one direct element constructor
 * have distinct names as written; namespace declaration attributes are
 * compared among themselves (4.12.1.1, 4.12.1.2). Equal expanded names
 * under different prefixes are lowering's (XQST0040).
 */
static sxml_status
sxml_xquery_check_attribute_names(sxml_ast_front_end *front_end,
                                  sxml_ast_node_index element) {
    const sxml_ast *ast = sxml_ast_front_end_get_ast(front_end);
    size_t attribute_count = 0;
    for (sxml_ast_node_index attribute =
             sxml_xquery_child(ast, element, SXML_AST_FIELD_ATTRIBUTE);
         attribute.value != 0;
         attribute = sxml_ast_find_next_with_same_field(ast, attribute)) {
        attribute_count += 1U;
    }
    sxml_xquery_duplicate duplicate = {.attribute = {.value = 0}};
    if (attribute_count <= SXML_XQUERY_PAIRWISE_ATTRIBUTE_COUNT) {
        duplicate = sxml_xquery_find_duplicate_pairwise(ast, element);
    } else {
        const sxml_status status = sxml_xquery_find_duplicate_sorted(
            ast,
            (sxml_xquery_attribute_set){.element = element,
                                        .attribute_count = attribute_count},
            &duplicate);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (duplicate.attribute.value == 0) {
        return SXML_STATUS_OK;
    }
    const bool is_namespace_declaration =
        (sxml_ast_get_node(ast, duplicate.attribute)->flags &
         (uint32_t)SXML_AST_FLAG_NAMESPACE_DECLARATION_ATTRIBUTE) != 0;
    return sxml_xquery_raise_at_node(
        front_end,
        (sxml_xquery_node_error){
            .error = is_namespace_declaration
                         ? (sxml_xquery_error){.code =
                                                   sxml_xquery_duplicate_namespace_code,
                                               .description =
                                                   sxml_xquery_duplicate_namespace_description}
                         : (sxml_xquery_error){.code =
                                                   sxml_xquery_duplicate_attribute_code,
                                               .description =
                                                   sxml_xquery_duplicate_attribute_description},
            .node = duplicate.attribute});
}

/* True when every byte of written is XML whitespace (S). */
static bool sxml_xquery_is_all_whitespace(sxml_byte_view written) {
    for (size_t byte_index = 0; byte_index < written.byte_count;
         byte_index += 1U) {
        if (!sxml_xquery_is_whitespace(written.bytes[byte_index])) {
            return false;
        }
    }
    return true;
}

/* True for content that delimits boundary whitespace (4.12.1.4): the start
   or end of the content (index 0), a direct constructor or an enclosed
   expression. CDATA sections and text do not. */
static bool sxml_xquery_delimits_boundary(const sxml_ast *ast,
                                          sxml_ast_node_index content) {
    const sxml_ast_kind kind = sxml_ast_get_node(ast, content)->kind;
    return content.value == 0 || (kind != SXML_AST_KIND_CDATA_SECTION &&
                                  kind != SXML_AST_KIND_DIRECT_ELEMENT_TEXT);
}

/*
 * Marks the boundary whitespace of an element's content (4.12.1.4):
 * consecutive whitespace written as such, not through character references
 * or CDATA sections, delimited at both ends by the start or end of the
 * content, a direct constructor or an enclosed expression.
 */
static sxml_status
sxml_xquery_mark_boundary_whitespace(sxml_ast_front_end *front_end,
                                     sxml_ast_node_index element) {
    sxml_ast *ast = sxml_ast_front_end_get_ast(front_end);
    sxml_ast_node_index previous = {.value = 0};
    sxml_ast_node_index content =
        sxml_xquery_child(ast, element, SXML_AST_FIELD_CONTENT);
    while (content.value != 0) {
        const sxml_ast_node_index next =
            sxml_ast_find_next_with_same_field(ast, content);
        const sxml_ast_node *node = sxml_ast_get_node(ast, content);
        if (node->kind == SXML_AST_KIND_DIRECT_ELEMENT_TEXT &&
            sxml_xquery_delimits_boundary(ast, previous) &&
            sxml_xquery_delimits_boundary(ast, next) &&
            sxml_xquery_is_all_whitespace(
                sxml_xquery_written_text(front_end, node->source_span))) {
            const sxml_status status = sxml_ast_add_flags(
                ast, (sxml_ast_flags_request){
                         .node = content,
                         .flags = (uint32_t)SXML_AST_FLAG_BOUNDARY_WHITESPACE});
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        previous = content;
        content = next;
    }
    return SXML_STATUS_OK;
}

/* The checks of a complete direct element constructor. */
static sxml_status
sxml_xquery_finish_direct_element(sxml_ast_front_end *front_end,
                                  sxml_ast_node_index element) {
    const sxml_ast *ast = sxml_ast_front_end_get_ast(front_end);
    const sxml_ast_node_index end_name =
        sxml_xquery_child(ast, element, SXML_AST_FIELD_END_NAME);
    /* 4.12.1: the end tag repeats the start tag's name, prefix included. */
    if (end_name.value != 0 &&
        !sxml_xquery_names_equal(
            sxml_xquery_written_name_of(
                ast, sxml_xquery_child(ast, element, SXML_AST_FIELD_NAME)),
            sxml_xquery_written_name_of(ast, end_name))) {
        return sxml_xquery_raise_at_node(
            front_end, (sxml_xquery_node_error){
                           .error = {.code = sxml_xquery_end_tag_code,
                                     .description =
                                         sxml_xquery_end_tag_description},
                           .node = end_name});
    }
    const sxml_status status =
        sxml_xquery_check_attribute_names(front_end, element);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_xquery_mark_boundary_whitespace(front_end, element);
}

/* The decoded value of a namespace declaration attribute without enclosed
   expressions: its text parts in order. */
typedef struct sxml_xquery_attribute_value {
    const sxml_ast *ast;
    sxml_ast_node_index value;
} sxml_xquery_attribute_value;

/* True when the text parts of value, concatenated, equal expected. */
static bool sxml_xquery_value_equals(sxml_xquery_attribute_value value,
                                     sxml_byte_view expected) {
    size_t matched = 0;
    for (sxml_ast_node_index value_part =
             sxml_xquery_child(value.ast, value.value, SXML_AST_FIELD_PART);
         value_part.value != 0;
         value_part = sxml_ast_find_next_with_same_field(value.ast, value_part)) {
        const sxml_byte_view text = sxml_ast_get_text(
            value.ast, sxml_ast_get_node(value.ast, value_part)->payload.text);
        if (text.byte_count > expected.byte_count - matched ||
            (text.byte_count != 0 &&
             memcmp(text.bytes, expected.bytes + matched, text.byte_count) !=
                 0)) {
            return false;
        }
        matched += text.byte_count;
    }
    return matched == expected.byte_count;
}

/* XQST0070 and XQST0085 for the binding of prefix (written_name's local
   name for "xmlns:p", empty for "xmlns") to value. */
typedef struct sxml_xquery_namespace_binding {
    bool has_prefix;
    sxml_byte_view prefix;
    sxml_xquery_attribute_value value;
} sxml_xquery_namespace_binding;

static sxml_xquery_error
sxml_xquery_check_binding(sxml_xquery_namespace_binding binding) {
    const sxml_xquery_error none = {.code = {.bytes = NULL, .byte_count = 0},
                                    .description = {.bytes = NULL,
                                                    .byte_count = 0}};
    const sxml_xquery_error reserved = {
        .code = sxml_xquery_reserved_namespace_code,
        .description = sxml_xquery_reserved_namespace_description};
    const bool binds_xml_prefix =
        binding.has_prefix &&
        sxml_byte_view_equals(binding.prefix,
                              SXML_BYTE_VIEW_FROM_LITERAL("xml"));
    const bool is_xml_uri =
        sxml_xquery_value_equals(binding.value, sxml_xquery_xml_namespace_uri);
    /* 4.12.1.2: xml only to its namespace and nothing else to it; xmlns
       never; nothing to the xmlns namespace. */
    if (binds_xml_prefix != is_xml_uri ||
        (binding.has_prefix &&
         sxml_byte_view_equals(binding.prefix,
                               SXML_BYTE_VIEW_FROM_LITERAL("xmlns"))) ||
        sxml_xquery_value_equals(binding.value,
                                 sxml_xquery_xmlns_namespace_uri)) {
        return reserved;
    }
    /* A prefix bound to "" undeclares it, which needs XML Names 1.1; sxml
       follows XML 1.0 and XML Names 1.0 (xgc: xml-version). */
    if (binding.has_prefix &&
        sxml_xquery_value_equals(binding.value, sxml_xquery_empty_view())) {
        return (sxml_xquery_error){
            .code = sxml_xquery_undeclared_prefix_code,
            .description = sxml_xquery_undeclared_prefix_description};
    }
    return none;
}

/*
 * A complete direct attribute: marks a namespace declaration attribute
 * ("xmlns" or "xmlns:p", 4.12.1.2) and checks its value: no enclosed
 * expression (XQST0022) and no reserved binding (XQST0070, XQST0085).
 */
static sxml_status
sxml_xquery_finish_direct_attribute(sxml_ast_front_end *front_end,
                                    sxml_ast_node_index attribute) {
    sxml_ast *ast = sxml_ast_front_end_get_ast(front_end);
    const sxml_xquery_written_name name = sxml_xquery_written_name_of(
        ast, sxml_xquery_child(ast, attribute, SXML_AST_FIELD_NAME));
    const sxml_byte_view xmlns = SXML_BYTE_VIEW_FROM_LITERAL("xmlns");
    const bool has_prefix = sxml_byte_view_equals(name.prefix, xmlns);
    if (!has_prefix && !(name.prefix.byte_count == 0 &&
                         sxml_byte_view_equals(name.local_name, xmlns))) {
        return SXML_STATUS_OK;
    }
    sxml_status status = sxml_ast_add_flags(
        ast,
        (sxml_ast_flags_request){
            .node = attribute,
            .flags = (uint32_t)SXML_AST_FLAG_NAMESPACE_DECLARATION_ATTRIBUTE});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_ast_node_index value =
        sxml_xquery_child(ast, attribute, SXML_AST_FIELD_VALUE);
    for (sxml_ast_node_index value_part =
             sxml_xquery_child(ast, value, SXML_AST_FIELD_PART);
         value_part.value != 0;
         value_part = sxml_ast_find_next_with_same_field(ast, value_part)) {
        if (sxml_ast_get_node(ast, value_part)->kind ==
            SXML_AST_KIND_ENCLOSED_EXPRESSION) {
            return sxml_xquery_raise_at_node(
                front_end,
                (sxml_xquery_node_error){
                    .error = {.code = sxml_xquery_enclosed_namespace_code,
                              .description =
                                  sxml_xquery_enclosed_namespace_description},
                    .node = value_part});
        }
    }
    const sxml_xquery_error error =
        sxml_xquery_check_binding((sxml_xquery_namespace_binding){
            .has_prefix = has_prefix,
            .prefix = name.local_name,
            .value = {.ast = ast, .value = value}});
    if (error.code.byte_count != 0) {
        status = sxml_xquery_raise_at_node(
            front_end,
            (sxml_xquery_node_error){.error = error, .node = attribute});
    }
    return status;
}

/* ------------------------------------------------------ Declarations */

/* XML 1.0 [81] EncName: [A-Za-z] ([A-Za-z0-9._] | '-')*. */
static bool sxml_xquery_is_encoding_name(sxml_byte_view name) {
    for (size_t byte_index = 0; byte_index < name.byte_count;
         byte_index += 1U) {
        const uint8_t byte = name.bytes[byte_index];
        const bool is_letter = (byte >= (uint8_t)'A' && byte <= (uint8_t)'Z') ||
                               (byte >= (uint8_t)'a' && byte <= (uint8_t)'z');
        const bool is_other = (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') ||
                              byte == '.' || byte == '_' || byte == '-';
        if (!is_letter && (byte_index == 0 || !is_other)) {
            return false;
        }
    }
    return name.byte_count != 0;
}

/* The versions an XQuery 4.0 processor must accept (5.1). */
static const sxml_byte_view sxml_xquery_versions[] = {
    SXML_BYTE_VIEW_INITIALIZER("1.0"),
    SXML_BYTE_VIEW_INITIALIZER("3.0"),
    SXML_BYTE_VIEW_INITIALIZER("3.1"),
    SXML_BYTE_VIEW_INITIALIZER("4.0"),
};

/*
 * A complete version declaration (5.1): sxml processes every module as
 * XQuery 4.0 and rejects other version numbers (XQST0031); an encoding
 * must be an EncName (XQST0087) and is otherwise ignored, as the text is
 * UTF-8.
 */
static sxml_status
sxml_xquery_finish_version_declaration(sxml_ast_front_end *front_end,
                                       sxml_ast_node_index declaration) {
    const sxml_ast *ast = sxml_ast_front_end_get_ast(front_end);
    const sxml_ast_node_index version =
        sxml_xquery_child(ast, declaration, SXML_AST_FIELD_VERSION);
    if (version.value != 0) {
        const sxml_byte_view written =
            sxml_ast_get_text(ast, sxml_ast_get_node(ast, version)->payload.text);
        bool is_known = false;
        for (size_t entry_index = 0;
             entry_index <
             sizeof sxml_xquery_versions / sizeof sxml_xquery_versions[0];
             entry_index += 1U) {
            is_known = is_known || sxml_byte_view_equals(
                                       written, sxml_xquery_versions[entry_index]);
        }
        if (!is_known) {
            return sxml_xquery_raise_at_node(
                front_end, (sxml_xquery_node_error){
                               .error = {.code = sxml_xquery_version_code,
                                         .description =
                                             sxml_xquery_version_description},
                               .node = version});
        }
    }
    const sxml_ast_node_index encoding =
        sxml_xquery_child(ast, declaration, SXML_AST_FIELD_ENCODING);
    if (encoding.value != 0 &&
        !sxml_xquery_is_encoding_name(sxml_ast_get_text(
            ast, sxml_ast_get_node(ast, encoding)->payload.text))) {
        return sxml_xquery_raise_at_node(
            front_end,
            (sxml_xquery_node_error){
                .error = {.code = sxml_xquery_encoding_code,
                          .description = sxml_xquery_encoding_description},
                .node = encoding});
    }
    return SXML_STATUS_OK;
}

/* True for the annotation %constructor, written unprefixed or as
   Q{http://www.w3.org/2012/xquery}constructor (5.15). A prefix bound to
   that namespace is resolved, and checked, by lowering. */
static bool sxml_xquery_is_constructor_annotation(const sxml_ast *ast,
                                                  sxml_ast_node_index annotation) {
    const sxml_ast_node *name = sxml_ast_get_node(
        ast, sxml_xquery_child(ast, annotation, SXML_AST_FIELD_NAME));
    const bool is_constructor = sxml_byte_view_equals(
        sxml_ast_get_text(ast, name->payload.name.local_name),
        SXML_BYTE_VIEW_FROM_LITERAL("constructor"));
    if (name->kind == SXML_AST_KIND_NCNAME) {
        return is_constructor;
    }
    return name->kind == SXML_AST_KIND_URI_QUALIFIED_NAME && is_constructor &&
           sxml_byte_view_equals(
               sxml_ast_get_text(ast, name->payload.name.namespace_uri),
               sxml_xquery_annotation_namespace_uri);
}

/*
 * 5.20.2: when a record declaration has a constructor function, its field
 * names are NCNames, not string literals (XPST0003; the grammar accepts
 * either, D29).
 */
static sxml_status
sxml_xquery_finish_record_declaration(sxml_ast_front_end *front_end,
                                      sxml_ast_node_index declaration) {
    const sxml_ast *ast = sxml_ast_front_end_get_ast(front_end);
    bool has_constructor = false;
    for (sxml_ast_node_index annotation =
             sxml_xquery_child(ast, declaration, SXML_AST_FIELD_ANNOTATION);
         annotation.value != 0;
         annotation = sxml_ast_find_next_with_same_field(ast, annotation)) {
        has_constructor = has_constructor ||
                          sxml_xquery_is_constructor_annotation(ast, annotation);
    }
    if (!has_constructor) {
        return SXML_STATUS_OK;
    }
    for (sxml_ast_node_index field =
             sxml_xquery_child(ast, declaration, SXML_AST_FIELD_FIELD);
         field.value != 0;
         field = sxml_ast_find_next_with_same_field(ast, field)) {
        const sxml_ast_node_index name =
            sxml_xquery_child(ast, field, SXML_AST_FIELD_NAME);
        if (sxml_ast_get_node(ast, name)->kind == SXML_AST_KIND_STRING_LITERAL) {
            return sxml_xquery_raise_at_node(
                front_end,
                (sxml_xquery_node_error){
                    .error = {.code = sxml_xquery_syntax_error_code,
                              .description =
                                  sxml_xquery_record_field_description},
                    .node = name});
        }
    }
    return SXML_STATUS_OK;
}

/*
 * The finish_node hook (D111): the checks the grammar leaves to the tree
 * builder (D29), each made once its node is complete.
 */
static sxml_status sxml_xquery_finish_node(sxml_ast_front_end *front_end,
                                           sxml_ast_node_index node) {
    const sxml_ast_kind kind =
        sxml_ast_get_node(sxml_ast_front_end_get_ast(front_end), node)->kind;
    if (kind == SXML_AST_KIND_DIRECT_ATTRIBUTE) {
        return sxml_xquery_finish_direct_attribute(front_end, node);
    }
    if (kind == SXML_AST_KIND_DIRECT_ELEMENT_CONSTRUCTOR) {
        return sxml_xquery_finish_direct_element(front_end, node);
    }
    if (kind == SXML_AST_KIND_VERSION_DECLARATION) {
        return sxml_xquery_finish_version_declaration(front_end, node);
    }
    if (kind == SXML_AST_KIND_NOMINATIVE_RECORD_TYPE_DECLARATION) {
        return sxml_xquery_finish_record_declaration(front_end, node);
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------- Entry */

static const sxml_ast_parser_part sxml_xquery_parser_part = {
    .language = tree_sitter_xquery,
    .root_node_type = SXML_BYTE_VIEW_INITIALIZER("module"),
    .node_types = {.entries = sxml_xquery_node_types,
                   .entry_count = sizeof sxml_xquery_node_types /
                                  sizeof sxml_xquery_node_types[0]},
    .fields = {.entries = sxml_xquery_fields,
               .entry_count =
                   sizeof sxml_xquery_fields / sizeof sxml_xquery_fields[0]},
    .unnamed_children = {.entries = sxml_xquery_unnamed_children,
                         .entry_count =
                             sizeof sxml_xquery_unnamed_children /
                             sizeof sxml_xquery_unnamed_children[0]},
    .syntax_error_description = SXML_BYTE_VIEW_INITIALIZER(
        "The text is not valid XQuery 4.0 syntax (XQuery 4.0, Appendix A)"),
    .missing_token_description = SXML_BYTE_VIEW_INITIALIZER(
        "The query ends early or misses a token (XQuery 4.0, Appendix A)"),
    .apply_token = sxml_xquery_apply_token,
    .describe_payload = sxml_xquery_describe_payload,
    .finish_node = sxml_xquery_finish_node,
};

sxml_status sxml_xquery_build_ast(const sxml_ast_build_request *request,
                                  sxml_ast **ast) {
    return sxml_ast_build_with_parser_part(request, &sxml_xquery_parser_part,
                                           ast);
}
