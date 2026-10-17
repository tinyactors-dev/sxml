/*
 * The XPath 4.0 front end: concrete syntax tree to abstract syntax tree
 * (src/xpath_ast_internal.h; docs/DECISIONS.md D12, D29, D52).
 *
 * Golden file: src/memory.c (layout, naming, outputs untouched on failure).
 *
 * The expression text is parsed with the tree-sitter grammar grammars/xpath.
 * Any ERROR or MISSING node is XPST0003 (D12). Then one walk over the
 * concrete syntax tree, in source order, builds the AST and makes the checks
 * the grammar leaves to the front end (D29): terminal delimitation (XPath
 * 4.0, A.3.2) and a prefixed name as a lookup key (XPath 4.0, 4.7.3
 * KeySpecifier, which allows only an NCName). The walk moves a tree-sitter
 * cursor and keeps the current AST parent in the AST itself (its parent
 * links), so it uses no recursion and no stack beyond the cursor's, however
 * deeply the expression nests. The concrete syntax tree is deleted before
 * returning.
 *
 * The same walker serves the grammars that inherit from grammars/xpath
 * (XQuery 4.0, XSLT 4.0 patterns): a parser part (D111,
 * src/xpath_ast_internal.h) brings the grammar, its extra node types and
 * fields, and hooks that the walk calls for tokens, payloads and complete
 * nodes. With no part, the walk is exactly the XPath one.
 *
 * The tree-sitter runtime allocates with malloc during the call; D12 accepts
 * that for the parse step. Everything that outlives the call is in the AST,
 * which comes from the caller's allocator.
 */
#include "xpath_ast_internal.h"

#include "sxml/string.h"

#include <string.h>
#include <tree_sitter/api.h>

/* Defined by the generated parser of grammars/xpath (D12). */
const TSLanguage *
tree_sitter_xpath(void); // NOLINT(readability-identifier-naming)

/*
 * A sequence type is parsed as the type of an instance-of expression, the
 * one place where the grammar's start rule reaches a lone SequenceType:
 * "() instance of " is read in front of the text, through the input
 * callback, so the text is never copied.
 */
static const char sxml_sequence_type_prefix[] = "() instance of ";

/* ------------------------------------------------------------- Tables */

/* A node type of grammars/xpath and the AST kind it becomes. */
typedef struct sxml_xpath_node_type_entry {
    const char *node_type;
    sxml_ast_kind kind;
} sxml_xpath_node_type_entry;

/*
 * Every named node type of grammars/xpath/src/node-types.json that becomes
 * an AST node, sorted by node_type for binary search (strcmp order). Not
 * here: `comment` (dropped), `axis` and `occurrence_indicator` (the detail
 * of their parent, D52), `ERROR` (rejected before the walk).
 */
static const sxml_xpath_node_type_entry sxml_xpath_node_types[] = {
    {"absolute_path_expression", SXML_AST_KIND_ABSOLUTE_PATH_EXPRESSION},
    {"additive_expression", SXML_AST_KIND_ADDITIVE_EXPRESSION},
    {"and_expression", SXML_AST_KIND_AND_EXPRESSION},
    {"any_array_type", SXML_AST_KIND_ANY_ARRAY_TYPE},
    {"any_function_type", SXML_AST_KIND_ANY_FUNCTION_TYPE},
    {"any_item_type", SXML_AST_KIND_ANY_ITEM_TYPE},
    {"any_map_type", SXML_AST_KIND_ANY_MAP_TYPE},
    {"any_record_type", SXML_AST_KIND_ANY_RECORD_TYPE},
    {"any_xnode_type", SXML_AST_KIND_ANY_XNODE_TYPE},
    {"argument_list", SXML_AST_KIND_ARGUMENT_LIST},
    {"argument_placeholder", SXML_AST_KIND_ARGUMENT_PLACEHOLDER},
    {"arrow_expression", SXML_AST_KIND_ARROW_EXPRESSION},
    {"attribute_node_type", SXML_AST_KIND_ATTRIBUTE_NODE_TYPE},
    {"axis_step", SXML_AST_KIND_AXIS_STEP},
    {"binary_integer_literal", SXML_AST_KIND_BINARY_INTEGER_LITERAL},
    {"boolean_constant", SXML_AST_KIND_BOOLEAN_CONSTANT},
    {"but_with_expression", SXML_AST_KIND_BUT_WITH_EXPRESSION},
    {"cast_expression", SXML_AST_KIND_CAST_EXPRESSION},
    {"cast_target", SXML_AST_KIND_CAST_TARGET},
    {"castable_expression", SXML_AST_KIND_CASTABLE_EXPRESSION},
    {"choice_item_type", SXML_AST_KIND_CHOICE_ITEM_TYPE},
    {"comment_node_type", SXML_AST_KIND_COMMENT_NODE_TYPE},
    {"comparison_expression", SXML_AST_KIND_COMPARISON_EXPRESSION},
    {"computed_attribute_constructor",
     SXML_AST_KIND_COMPUTED_ATTRIBUTE_CONSTRUCTOR},
    {"computed_comment_constructor",
     SXML_AST_KIND_COMPUTED_COMMENT_CONSTRUCTOR},
    {"computed_document_constructor",
     SXML_AST_KIND_COMPUTED_DOCUMENT_CONSTRUCTOR},
    {"computed_element_constructor",
     SXML_AST_KIND_COMPUTED_ELEMENT_CONSTRUCTOR},
    {"computed_namespace_constructor",
     SXML_AST_KIND_COMPUTED_NAMESPACE_CONSTRUCTOR},
    {"computed_processing_instruction_constructor",
     SXML_AST_KIND_COMPUTED_PROCESSING_INSTRUCTION_CONSTRUCTOR},
    {"computed_text_constructor", SXML_AST_KIND_COMPUTED_TEXT_CONSTRUCTOR},
    {"context_value_reference", SXML_AST_KIND_CONTEXT_VALUE_REFERENCE},
    {"curly_array_constructor", SXML_AST_KIND_CURLY_ARRAY_CONSTRUCTOR},
    {"decimal_literal", SXML_AST_KIND_DECIMAL_LITERAL},
    {"default_element_namespace_declaration",
     SXML_AST_KIND_DEFAULT_ELEMENT_NAMESPACE_DECLARATION},
    {"document_node_type", SXML_AST_KIND_DOCUMENT_NODE_TYPE},
    {"double_literal", SXML_AST_KIND_DOUBLE_LITERAL},
    {"dynamic_function_call", SXML_AST_KIND_DYNAMIC_FUNCTION_CALL},
    {"dynamic_node_test", SXML_AST_KIND_DYNAMIC_NODE_TEST},
    {"element_node_type", SXML_AST_KIND_ELEMENT_NODE_TYPE},
    {"enclosed_expression", SXML_AST_KIND_ENCLOSED_EXPRESSION},
    {"enumeration_type", SXML_AST_KIND_ENUMERATION_TYPE},
    {"field_declaration", SXML_AST_KIND_FIELD_DECLARATION},
    {"filter_expression", SXML_AST_KIND_FILTER_EXPRESSION},
    {"flwor_expression", SXML_AST_KIND_FLWOR_EXPRESSION},
    {"for_clause", SXML_AST_KIND_FOR_CLAUSE},
    {"for_entry_binding", SXML_AST_KIND_FOR_ENTRY_BINDING},
    {"for_entry_key_binding", SXML_AST_KIND_FOR_ENTRY_KEY_BINDING},
    {"for_entry_value_binding", SXML_AST_KIND_FOR_ENTRY_VALUE_BINDING},
    {"for_item_binding", SXML_AST_KIND_FOR_ITEM_BINDING},
    {"for_member_binding", SXML_AST_KIND_FOR_MEMBER_BINDING},
    {"function_call", SXML_AST_KIND_FUNCTION_CALL},
    {"gnode_type", SXML_AST_KIND_GNODE_TYPE},
    {"hex_integer_literal", SXML_AST_KIND_HEX_INTEGER_LITERAL},
    {"if_expression", SXML_AST_KIND_IF_EXPRESSION},
    {"inline_function_expression", SXML_AST_KIND_INLINE_FUNCTION_EXPRESSION},
    {"instance_of_expression", SXML_AST_KIND_INSTANCE_OF_EXPRESSION},
    {"integer_literal", SXML_AST_KIND_INTEGER_LITERAL},
    {"intersect_except_expression", SXML_AST_KIND_INTERSECT_EXCEPT_EXPRESSION},
    {"jnode_type", SXML_AST_KIND_JNODE_TYPE},
    {"jroot_selector", SXML_AST_KIND_JROOT_SELECTOR},
    {"keyword_argument", SXML_AST_KIND_KEYWORD_ARGUMENT},
    {"let_array_binding", SXML_AST_KIND_LET_ARRAY_BINDING},
    {"let_clause", SXML_AST_KIND_LET_CLAUSE},
    {"let_map_binding", SXML_AST_KIND_LET_MAP_BINDING},
    {"let_sequence_binding", SXML_AST_KIND_LET_SEQUENCE_BINDING},
    {"let_value_binding", SXML_AST_KIND_LET_VALUE_BINDING},
    {"lookup", SXML_AST_KIND_LOOKUP},
    {"lookup_expression", SXML_AST_KIND_LOOKUP_EXPRESSION},
    {"lookup_wildcard", SXML_AST_KIND_LOOKUP_WILDCARD},
    {"map_constructor", SXML_AST_KIND_MAP_CONSTRUCTOR},
    {"map_constructor_entry", SXML_AST_KIND_MAP_CONSTRUCTOR_ENTRY},
    {"marked_ncname", SXML_AST_KIND_MARKED_NCNAME},
    {"method_call", SXML_AST_KIND_METHOD_CALL},
    {"multiplicative_expression", SXML_AST_KIND_MULTIPLICATIVE_EXPRESSION},
    {"name_test_union", SXML_AST_KIND_NAME_TEST_UNION},
    {"named_function_reference", SXML_AST_KIND_NAMED_FUNCTION_REFERENCE},
    {"namespace_declaration", SXML_AST_KIND_NAMESPACE_DECLARATION},
    {"namespace_node_type", SXML_AST_KIND_NAMESPACE_NODE_TYPE},
    {"ncname", SXML_AST_KIND_NCNAME},
    {"negative_numeric_literal", SXML_AST_KIND_NEGATIVE_NUMERIC_LITERAL},
    {"or_expression", SXML_AST_KIND_OR_EXPRESSION},
    {"otherwise_expression", SXML_AST_KIND_OTHERWISE_EXPRESSION},
    {"parenthesized_expression", SXML_AST_KIND_PARENTHESIZED_EXPRESSION},
    {"pipeline_expression", SXML_AST_KIND_PIPELINE_EXPRESSION},
    {"positional_argument_list", SXML_AST_KIND_POSITIONAL_ARGUMENT_LIST},
    {"positional_variable", SXML_AST_KIND_POSITIONAL_VARIABLE},
    {"predicate", SXML_AST_KIND_PREDICATE},
    {"prefixed_name", SXML_AST_KIND_PREFIXED_NAME},
    {"processing_instruction_node_type",
     SXML_AST_KIND_PROCESSING_INSTRUCTION_NODE_TYPE},
    {"qname_literal", SXML_AST_KIND_QNAME_LITERAL},
    {"quantified_expression", SXML_AST_KIND_QUANTIFIED_EXPRESSION},
    {"quantifier_binding", SXML_AST_KIND_QUANTIFIER_BINDING},
    {"range_expression", SXML_AST_KIND_RANGE_EXPRESSION},
    {"relative_path_expression", SXML_AST_KIND_RELATIVE_PATH_EXPRESSION},
    {"restricted_dynamic_call", SXML_AST_KIND_RESTRICTED_DYNAMIC_CALL},
    {"return_clause", SXML_AST_KIND_RETURN_CLAUSE},
    {"schema_attribute_node_type", SXML_AST_KIND_SCHEMA_ATTRIBUTE_NODE_TYPE},
    {"schema_element_node_type", SXML_AST_KIND_SCHEMA_ELEMENT_NODE_TYPE},
    {"sequence_expression", SXML_AST_KIND_SEQUENCE_EXPRESSION},
    {"sequence_type", SXML_AST_KIND_SEQUENCE_TYPE},
    {"simple_map_expression", SXML_AST_KIND_SIMPLE_MAP_EXPRESSION},
    {"square_array_constructor", SXML_AST_KIND_SQUARE_ARRAY_CONSTRUCTOR},
    {"string_concatenation_expression",
     SXML_AST_KIND_STRING_CONCATENATION_EXPRESSION},
    {"string_literal", SXML_AST_KIND_STRING_LITERAL},
    {"string_template", SXML_AST_KIND_STRING_TEMPLATE},
    {"string_template_fixed_part", SXML_AST_KIND_STRING_TEMPLATE_FIXED_PART},
    {"text_node_type", SXML_AST_KIND_TEXT_NODE_TYPE},
    {"treat_expression", SXML_AST_KIND_TREAT_EXPRESSION},
    {"type_declaration", SXML_AST_KIND_TYPE_DECLARATION},
    {"typed_array_type", SXML_AST_KIND_TYPED_ARRAY_TYPE},
    {"typed_function_parameter", SXML_AST_KIND_TYPED_FUNCTION_PARAMETER},
    {"typed_function_type", SXML_AST_KIND_TYPED_FUNCTION_TYPE},
    {"typed_map_type", SXML_AST_KIND_TYPED_MAP_TYPE},
    {"typed_record_type", SXML_AST_KIND_TYPED_RECORD_TYPE},
    {"unary_expression", SXML_AST_KIND_UNARY_EXPRESSION},
    {"unary_lookup", SXML_AST_KIND_UNARY_LOOKUP},
    {"union_expression", SXML_AST_KIND_UNION_EXPRESSION},
    {"union_node_test", SXML_AST_KIND_UNION_NODE_TEST},
    {"uri_qualified_name", SXML_AST_KIND_URI_QUALIFIED_NAME},
    {"variable_name_and_type", SXML_AST_KIND_VARIABLE_NAME_AND_TYPE},
    {"variable_reference", SXML_AST_KIND_VARIABLE_REFERENCE},
    {"wildcard", SXML_AST_KIND_WILDCARD},
    {"xpath", SXML_AST_KIND_XPATH},
};

/* A field name of grammars/xpath and the AST field it becomes. */
typedef struct sxml_xpath_field_entry {
    const char *field_name;
    sxml_ast_field field;
} sxml_xpath_field_entry;

/*
 * The fields that hold named nodes, sorted by field_name (strcmp order).
 * The fields that hold tokens or absorbed nodes (operator, quantifier,
 * abbreviation, keyword, empty, optional, nillable, axis, occurrence) are
 * read while describing their parent instead.
 */
static const sxml_xpath_field_entry sxml_xpath_fields[] = {
    {"alternative", SXML_AST_FIELD_ALTERNATIVE},
    {"argument", SXML_AST_FIELD_ARGUMENT},
    {"arguments", SXML_AST_FIELD_ARGUMENTS},
    {"arity", SXML_AST_FIELD_ARITY},
    {"binding", SXML_AST_FIELD_BINDING},
    {"body", SXML_AST_FIELD_BODY},
    {"clause", SXML_AST_FIELD_CLAUSE},
    {"condition", SXML_AST_FIELD_CONDITION},
    {"content", SXML_AST_FIELD_CONTENT},
    {"declaration", SXML_AST_FIELD_DECLARATION},
    {"else", SXML_AST_FIELD_ELSE},
    {"entry", SXML_AST_FIELD_ENTRY},
    {"expression", SXML_AST_FIELD_EXPRESSION},
    {"field", SXML_AST_FIELD_FIELD},
    {"function", SXML_AST_FIELD_FUNCTION},
    {"item", SXML_AST_FIELD_ITEM},
    {"item_type", SXML_AST_FIELD_ITEM_TYPE},
    {"key", SXML_AST_FIELD_KEY},
    {"left", SXML_AST_FIELD_LEFT},
    {"lookup", SXML_AST_FIELD_LOOKUP},
    {"member", SXML_AST_FIELD_MEMBER},
    {"name", SXML_AST_FIELD_NAME},
    {"name_expression", SXML_AST_FIELD_NAME_EXPRESSION},
    {"operand", SXML_AST_FIELD_OPERAND},
    {"parameter", SXML_AST_FIELD_PARAMETER},
    {"part", SXML_AST_FIELD_PART},
    {"path", SXML_AST_FIELD_PATH},
    {"position", SXML_AST_FIELD_POSITION},
    {"predicate", SXML_AST_FIELD_PREDICATE},
    {"prefix", SXML_AST_FIELD_PREFIX},
    {"return", SXML_AST_FIELD_RETURN},
    {"return_type", SXML_AST_FIELD_RETURN_TYPE},
    {"right", SXML_AST_FIELD_RIGHT},
    {"selector", SXML_AST_FIELD_SELECTOR},
    {"sequence", SXML_AST_FIELD_SEQUENCE},
    {"target", SXML_AST_FIELD_TARGET},
    {"test", SXML_AST_FIELD_TEST},
    {"then", SXML_AST_FIELD_THEN},
    {"type", SXML_AST_FIELD_TYPE},
    {"uri", SXML_AST_FIELD_URI},
    {"value", SXML_AST_FIELD_VALUE},
    {"variable", SXML_AST_FIELD_VARIABLE},
};

/* An operator token as written and the operator it selects. */
typedef struct sxml_xpath_operator_entry {
    sxml_byte_view token;
    sxml_ast_operator operator;
} sxml_xpath_operator_entry;

/* Every token in an `operator` field of grammars/xpath (XPath 4.0, A.1). */
static const sxml_xpath_operator_entry sxml_xpath_operators[] = {
    {SXML_BYTE_VIEW_INITIALIZER("or"), SXML_AST_OPERATOR_OR},
    {SXML_BYTE_VIEW_INITIALIZER("and"), SXML_AST_OPERATOR_AND},
    {SXML_BYTE_VIEW_INITIALIZER("eq"), SXML_AST_OPERATOR_VALUE_EQUAL},
    {SXML_BYTE_VIEW_INITIALIZER("ne"), SXML_AST_OPERATOR_VALUE_NOT_EQUAL},
    {SXML_BYTE_VIEW_INITIALIZER("lt"), SXML_AST_OPERATOR_VALUE_LESS},
    {SXML_BYTE_VIEW_INITIALIZER("le"), SXML_AST_OPERATOR_VALUE_LESS_OR_EQUAL},
    {SXML_BYTE_VIEW_INITIALIZER("gt"), SXML_AST_OPERATOR_VALUE_GREATER},
    {SXML_BYTE_VIEW_INITIALIZER("ge"),
     SXML_AST_OPERATOR_VALUE_GREATER_OR_EQUAL},
    {SXML_BYTE_VIEW_INITIALIZER("="), SXML_AST_OPERATOR_GENERAL_EQUAL},
    {SXML_BYTE_VIEW_INITIALIZER("!="), SXML_AST_OPERATOR_GENERAL_NOT_EQUAL},
    {SXML_BYTE_VIEW_INITIALIZER("<"), SXML_AST_OPERATOR_GENERAL_LESS},
    {SXML_BYTE_VIEW_INITIALIZER("<="), SXML_AST_OPERATOR_GENERAL_LESS_OR_EQUAL},
    {SXML_BYTE_VIEW_INITIALIZER(">"), SXML_AST_OPERATOR_GENERAL_GREATER},
    {SXML_BYTE_VIEW_INITIALIZER(">="),
     SXML_AST_OPERATOR_GENERAL_GREATER_OR_EQUAL},
    {SXML_BYTE_VIEW_INITIALIZER("is"), SXML_AST_OPERATOR_IS},
    {SXML_BYTE_VIEW_INITIALIZER("is-not"), SXML_AST_OPERATOR_IS_NOT},
    {SXML_BYTE_VIEW_INITIALIZER("<<"), SXML_AST_OPERATOR_PRECEDES},
    {SXML_BYTE_VIEW_INITIALIZER("precedes"), SXML_AST_OPERATOR_PRECEDES},
    {SXML_BYTE_VIEW_INITIALIZER(">>"), SXML_AST_OPERATOR_FOLLOWS},
    {SXML_BYTE_VIEW_INITIALIZER("follows"), SXML_AST_OPERATOR_FOLLOWS},
    {SXML_BYTE_VIEW_INITIALIZER("precedes-or-is"),
     SXML_AST_OPERATOR_PRECEDES_OR_IS},
    {SXML_BYTE_VIEW_INITIALIZER("follows-or-is"),
     SXML_AST_OPERATOR_FOLLOWS_OR_IS},
    {SXML_BYTE_VIEW_INITIALIZER("otherwise"), SXML_AST_OPERATOR_OTHERWISE},
    {SXML_BYTE_VIEW_INITIALIZER("||"), SXML_AST_OPERATOR_CONCATENATE},
    {SXML_BYTE_VIEW_INITIALIZER("to"), SXML_AST_OPERATOR_TO},
    {SXML_BYTE_VIEW_INITIALIZER("+"), SXML_AST_OPERATOR_PLUS},
    {SXML_BYTE_VIEW_INITIALIZER("-"), SXML_AST_OPERATOR_MINUS},
    {SXML_BYTE_VIEW_INITIALIZER("*"), SXML_AST_OPERATOR_MULTIPLY},
    {SXML_BYTE_VIEW_INITIALIZER("\xC3\x97"), SXML_AST_OPERATOR_MULTIPLY},
    {SXML_BYTE_VIEW_INITIALIZER("div"), SXML_AST_OPERATOR_DIVIDE},
    {SXML_BYTE_VIEW_INITIALIZER("\xC3\xB7"), SXML_AST_OPERATOR_DIVIDE},
    {SXML_BYTE_VIEW_INITIALIZER("idiv"), SXML_AST_OPERATOR_INTEGER_DIVIDE},
    {SXML_BYTE_VIEW_INITIALIZER("mod"), SXML_AST_OPERATOR_MODULO},
    {SXML_BYTE_VIEW_INITIALIZER("union"), SXML_AST_OPERATOR_UNION},
    {SXML_BYTE_VIEW_INITIALIZER("|"), SXML_AST_OPERATOR_UNION},
    {SXML_BYTE_VIEW_INITIALIZER("intersect"), SXML_AST_OPERATOR_INTERSECT},
    {SXML_BYTE_VIEW_INITIALIZER("except"), SXML_AST_OPERATOR_EXCEPT},
    {SXML_BYTE_VIEW_INITIALIZER("!"), SXML_AST_OPERATOR_SIMPLE_MAP},
    {SXML_BYTE_VIEW_INITIALIZER("/"), SXML_AST_OPERATOR_SLASH},
    {SXML_BYTE_VIEW_INITIALIZER("//"), SXML_AST_OPERATOR_DOUBLE_SLASH},
    {SXML_BYTE_VIEW_INITIALIZER("->"), SXML_AST_OPERATOR_PIPELINE},
    {SXML_BYTE_VIEW_INITIALIZER("=>"), SXML_AST_OPERATOR_SEQUENCE_ARROW},
    {SXML_BYTE_VIEW_INITIALIZER("=!>"), SXML_AST_OPERATOR_MAPPING_ARROW},
};

/* An axis name as written and the axis (XPath 4.0, 4.6.4.1). */
typedef struct sxml_xpath_axis_entry {
    sxml_byte_view name;
    sxml_ast_axis axis;
} sxml_xpath_axis_entry;

static const sxml_xpath_axis_entry sxml_xpath_axes[] = {
    {SXML_BYTE_VIEW_INITIALIZER("ancestor"), SXML_AST_AXIS_ANCESTOR},
    {SXML_BYTE_VIEW_INITIALIZER("ancestor-or-self"),
     SXML_AST_AXIS_ANCESTOR_OR_SELF},
    {SXML_BYTE_VIEW_INITIALIZER("attribute"), SXML_AST_AXIS_ATTRIBUTE},
    {SXML_BYTE_VIEW_INITIALIZER("child"), SXML_AST_AXIS_CHILD},
    {SXML_BYTE_VIEW_INITIALIZER("descendant"), SXML_AST_AXIS_DESCENDANT},
    {SXML_BYTE_VIEW_INITIALIZER("descendant-or-self"),
     SXML_AST_AXIS_DESCENDANT_OR_SELF},
    {SXML_BYTE_VIEW_INITIALIZER("following"), SXML_AST_AXIS_FOLLOWING},
    {SXML_BYTE_VIEW_INITIALIZER("following-or-self"),
     SXML_AST_AXIS_FOLLOWING_OR_SELF},
    {SXML_BYTE_VIEW_INITIALIZER("following-sibling"),
     SXML_AST_AXIS_FOLLOWING_SIBLING},
    {SXML_BYTE_VIEW_INITIALIZER("following-sibling-or-self"),
     SXML_AST_AXIS_FOLLOWING_SIBLING_OR_SELF},
    {SXML_BYTE_VIEW_INITIALIZER("item"), SXML_AST_AXIS_ITEM},
    {SXML_BYTE_VIEW_INITIALIZER("namespace"), SXML_AST_AXIS_NAMESPACE},
    {SXML_BYTE_VIEW_INITIALIZER("parent"), SXML_AST_AXIS_PARENT},
    {SXML_BYTE_VIEW_INITIALIZER("preceding"), SXML_AST_AXIS_PRECEDING},
    {SXML_BYTE_VIEW_INITIALIZER("preceding-or-self"),
     SXML_AST_AXIS_PRECEDING_OR_SELF},
    {SXML_BYTE_VIEW_INITIALIZER("preceding-sibling"),
     SXML_AST_AXIS_PRECEDING_SIBLING},
    {SXML_BYTE_VIEW_INITIALIZER("preceding-sibling-or-self"),
     SXML_AST_AXIS_PRECEDING_SIBLING_OR_SELF},
    {SXML_BYTE_VIEW_INITIALIZER("self"), SXML_AST_AXIS_SELF},
};

/*
 * XPath 4.0, A.3.2: the named terminals that are non-delimiting; keywords,
 * the anonymous terminals spelled as names, are too.
 */
static const char *const sxml_xpath_non_delimiting_node_types[] = {
    "ncname",          "prefixed_name",       "uri_qualified_name",
    "integer_literal", "hex_integer_literal", "binary_integer_literal",
    "decimal_literal", "double_literal",
};

/* The code of syntax errors (XPath 4.0, 2.4.1) and the static descriptions
   of its error record (D21). */
static const sxml_byte_view sxml_xpath_syntax_error_code =
    SXML_BYTE_VIEW_INITIALIZER("XPST0003");
static const sxml_byte_view sxml_xpath_syntax_error_description =
    SXML_BYTE_VIEW_INITIALIZER("The text is not valid XPath 4.0 syntax "
                               "(XPath 4.0, Appendix A)");
static const sxml_byte_view sxml_xpath_missing_token_description =
    SXML_BYTE_VIEW_INITIALIZER("The expression ends early or misses a "
                               "token (XPath 4.0, Appendix A)");
static const sxml_byte_view sxml_xpath_delimitation_description =
    SXML_BYTE_VIEW_INITIALIZER(
        "A name, keyword or number directly follows another; separate them "
        "with whitespace (XPath 4.0, A.3.2)");
static const sxml_byte_view sxml_xpath_prefixed_key_description =
    SXML_BYTE_VIEW_INITIALIZER(
        "A lookup key must be an NCName, not a prefixed name (XPath 4.0, "
        "KeySpecifier)");
static const sxml_byte_view sxml_xpath_not_sequence_type_description =
    SXML_BYTE_VIEW_INITIALIZER(
        "The text is not one sequence type (XPath 4.0, section 3.1)");

/* ------------------------------------------------------------ Front end */

/* A position in the expression text; numbers count from 1 (glossary
   `source_location`). */
typedef struct sxml_xpath_text_position {
    uint32_t byte_offset;
    uint32_t line_number;
    uint32_t column_number;
} sxml_xpath_text_position;

/* The terminal before the current one, for the A.3.2 check. */
typedef struct sxml_xpath_previous_terminal {
    bool exists;
    bool is_non_delimiting;
    uint32_t end_byte;
} sxml_xpath_previous_terminal;

/* The state of one call of the front end (sxml_ast_front_end in
   src/xpath_ast_internal.h). */
struct sxml_ast_front_end {
    const sxml_ast_build_request *request;
    /* The parser part of a derived grammar (D111); NULL for XPath. */
    const sxml_ast_parser_part *part;
    /* For XPST0003 from an ERROR and from a MISSING node: the part's, or
       the XPath ones. */
    sxml_byte_view syntax_error_description;
    sxml_byte_view missing_token_description;
    /* Bytes read in front of the expression text (the sequence type
       prefix); concrete syntax tree offsets are this much larger. */
    sxml_byte_view prefix;
    sxml_ast *ast;
    /* Reads the children of a node while it is described. */
    TSTreeCursor scan_cursor;
    /* Advances in source order with the walk. */
    sxml_xpath_text_position position;
    sxml_xpath_previous_terminal previous_terminal;
    /* Set when the front end reports a static error: XPST0003, or the code
       a part raises. */
    sxml_byte_view error_code;
    sxml_byte_view error_description;
    uint32_t error_byte_offset;
};

/* A concrete syntax tree node with the AST kind it becomes. */
typedef struct sxml_xpath_syntax_node {
    TSNode node;
    sxml_ast_kind kind;
} sxml_xpath_syntax_node;

/*
 * The input callback of tree-sitter (TSInput.read). Its signature is the
 * runtime's and takes four parameters, an exception to the three-parameter
 * rule (docs/C_GUIDELINES.md, "Function signatures"). Serves the prefix,
 * then the expression text.
 */
// ast-grep-ignore: too-many-parameters
static const char *sxml_xpath_read_input( // NOLINT(readability-function-size)
    void *payload, uint32_t byte_index, TSPoint position,
    uint32_t *bytes_read) {
    (void)position;
    const sxml_ast_front_end *front_end = payload;
    const sxml_byte_view prefix = front_end->prefix;
    const sxml_byte_view text = front_end->request->expression_text;
    /* The prefix and text counts were checked to fit uint32_t together. */
    if (byte_index < prefix.byte_count) {
        *bytes_read = (uint32_t)(prefix.byte_count - byte_index);
        return (const char *)prefix.bytes + byte_index;
    }
    const size_t text_offset = byte_index - prefix.byte_count;
    if (text_offset >= text.byte_count) {
        *bytes_read = 0;
        return "";
    }
    *bytes_read = (uint32_t)(text.byte_count - text_offset);
    return (const char *)text.bytes + text_offset;
}

/* Bytes of the expression text from a concrete syntax tree byte offset. */
static uint32_t sxml_xpath_text_offset(const sxml_ast_front_end *front_end,
                                       uint32_t tree_byte_offset) {
    const uint32_t prefix_byte_count = (uint32_t)front_end->prefix.byte_count;
    return tree_byte_offset < prefix_byte_count
               ? 0
               : tree_byte_offset - prefix_byte_count;
}

/* The source bytes of node, borrowed from the expression text. */
static sxml_byte_view
sxml_xpath_node_text(const sxml_ast_front_end *front_end, TSNode node) {
    const uint32_t start =
        sxml_xpath_text_offset(front_end, ts_node_start_byte(node));
    const uint32_t end =
        sxml_xpath_text_offset(front_end, ts_node_end_byte(node));
    if (end <= start) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return (sxml_byte_view){.bytes = front_end->request->expression_text.bytes +
                                     start,
                            .byte_count = end - start};
}

/*
 * Moves *position forward to target_byte_offset of text, counting lines at
 * line feeds and columns in codepoints (UTF-8 lead bytes).
 */
static void sxml_xpath_advance_position(sxml_byte_view text,
                                        uint32_t target_byte_offset,
                                        sxml_xpath_text_position *position) {
    while (position->byte_offset < target_byte_offset &&
           position->byte_offset < text.byte_count) {
        const uint8_t byte = text.bytes[position->byte_offset];
        if (byte == '\n') {
            position->line_number += 1U;
            position->column_number = 1;
        } else if ((byte & 0xC0U) != 0x80U) {
            position->column_number += 1U;
        }
        position->byte_offset += 1U;
    }
}

/* The source span of node; advances the walk's position to its start. */
static sxml_ast_source_span sxml_xpath_span_of(sxml_ast_front_end *front_end,
                                               TSNode node) {
    const uint32_t start =
        sxml_xpath_text_offset(front_end, ts_node_start_byte(node));
    const uint32_t end =
        sxml_xpath_text_offset(front_end, ts_node_end_byte(node));
    sxml_xpath_advance_position(front_end->request->expression_text, start,
                                &front_end->position);
    return (sxml_ast_source_span){
        .byte_offset = start,
        .byte_count = end - start,
        .line_number = front_end->position.line_number,
        .column_number = front_end->position.column_number,
    };
}

/* Records XPST0003 at node; returns SXML_STATUS_STATIC_ERROR. */
static sxml_status
sxml_xpath_report_syntax_error(sxml_ast_front_end *front_end, TSNode node,
                               sxml_byte_view description) {
    front_end->error_code = sxml_xpath_syntax_error_code;
    front_end->error_description = description;
    front_end->error_byte_offset =
        sxml_xpath_text_offset(front_end, ts_node_start_byte(node));
    return SXML_STATUS_STATIC_ERROR;
}

/* The AST kind of node_type; SXML_AST_KIND_NONE when it makes no node. */
// ast-grep-ignore: pointer-parameter-without-length
static sxml_ast_kind sxml_xpath_kind_of_node_type(const char *node_type) {
    size_t low = 0;
    size_t high =
        sizeof sxml_xpath_node_types / sizeof sxml_xpath_node_types[0];
    while (low < high) {
        const size_t middle = low + (high - low) / 2U;
        const int comparison =
            strcmp(node_type, sxml_xpath_node_types[middle].node_type);
        if (comparison == 0) {
            return sxml_xpath_node_types[middle].kind;
        }
        if (comparison < 0) {
            high = middle;
        } else {
            low = middle + 1U;
        }
    }
    return SXML_AST_KIND_NONE;
}

/* Orders two byte strings as strcmp orders C strings. */
static int sxml_xpath_compare_bytes(sxml_byte_view left, sxml_byte_view right) {
    const size_t common = left.byte_count < right.byte_count
                              ? left.byte_count
                              : right.byte_count;
    const int comparison =
        common == 0 ? 0 : memcmp(left.bytes, right.bytes, common);
    if (comparison != 0) {
        return comparison;
    }
    if (left.byte_count == right.byte_count) {
        return 0;
    }
    return left.byte_count < right.byte_count ? -1 : 1;
}

/* The kind a part's sorted table gives node_type; NONE when absent. */
static sxml_ast_kind sxml_xpath_part_kind_of(sxml_ast_node_type_view table,
                                             sxml_byte_view node_type) {
    size_t low = 0;
    size_t high = table.entry_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2U;
        const int comparison = sxml_xpath_compare_bytes(
            node_type, table.entries[middle].node_type);
        if (comparison == 0) {
            return table.entries[middle].kind;
        }
        if (comparison < 0) {
            high = middle;
        } else {
            low = middle + 1U;
        }
    }
    return SXML_AST_KIND_NONE;
}

/* The field a part's sorted table gives field_name; NONE when absent. */
static sxml_ast_field sxml_xpath_part_field_of(sxml_ast_field_view table,
                                               sxml_byte_view field_name) {
    size_t low = 0;
    size_t high = table.entry_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2U;
        const int comparison = sxml_xpath_compare_bytes(
            field_name, table.entries[middle].field_name);
        if (comparison == 0) {
            return table.entries[middle].field;
        }
        if (comparison < 0) {
            high = middle;
        } else {
            low = middle + 1U;
        }
    }
    return SXML_AST_FIELD_NONE;
}

/*
 * The AST kind node becomes; SXML_AST_KIND_NONE for anonymous tokens and
 * for the named nodes that make no AST node (comment, axis,
 * occurrence_indicator). A node of this kind opens an AST node, so the walk
 * calls this both on the way down and on the way up.
 */
static sxml_ast_kind sxml_xpath_kind_of(const sxml_ast_front_end *front_end,
                                        TSNode node) {
    if (!ts_node_is_named(node)) {
        return SXML_AST_KIND_NONE;
    }
    const char *node_type = ts_node_type(node);
    const sxml_ast_kind kind = sxml_xpath_kind_of_node_type(node_type);
    if (kind != SXML_AST_KIND_NONE || front_end->part == NULL) {
        return kind;
    }
    return sxml_xpath_part_kind_of(front_end->part->node_types,
                                   sxml_byte_view_from_c_string(node_type));
}

/* The AST field of field_name; SXML_AST_FIELD_NONE for NULL or unknown. */
// ast-grep-ignore: pointer-parameter-without-length
static sxml_ast_field sxml_xpath_field_of_name(const char *field_name) {
    if (field_name == NULL) {
        return SXML_AST_FIELD_NONE;
    }
    size_t low = 0;
    size_t high = sizeof sxml_xpath_fields / sizeof sxml_xpath_fields[0];
    while (low < high) {
        const size_t middle = low + (high - low) / 2U;
        const int comparison =
            strcmp(field_name, sxml_xpath_fields[middle].field_name);
        if (comparison == 0) {
            return sxml_xpath_fields[middle].field;
        }
        if (comparison < 0) {
            high = middle;
        } else {
            low = middle + 1U;
        }
    }
    return SXML_AST_FIELD_NONE;
}

/* A child's grammar field and the kind of its AST parent. */
typedef struct sxml_xpath_field_query {
    /* NULL when the grammar gives the child no field. */
    const char *field_name;
    sxml_ast_kind parent_kind;
} sxml_xpath_field_query;

/*
 * The AST field of a named child: the XPath table's, else the part's;
 * for a child without a grammar field, the part's unnamed child entry for
 * the parent's kind (D111). SXML_AST_FIELD_NONE when there is none.
 */
static sxml_ast_field sxml_xpath_field_of(const sxml_ast_front_end *front_end,
                                          sxml_xpath_field_query query) {
    const sxml_ast_parser_part *part = front_end->part;
    if (query.field_name == NULL) {
        if (part == NULL) {
            return SXML_AST_FIELD_NONE;
        }
        for (size_t entry_index = 0;
             entry_index < part->unnamed_children.entry_count;
             entry_index += 1U) {
            const sxml_ast_unnamed_child_entry *entry =
                &part->unnamed_children.entries[entry_index];
            if (entry->parent_kind == query.parent_kind) {
                return entry->field;
            }
        }
        return SXML_AST_FIELD_NONE;
    }
    const sxml_ast_field field = sxml_xpath_field_of_name(query.field_name);
    if (field != SXML_AST_FIELD_NONE || part == NULL) {
        return field;
    }
    return sxml_xpath_part_field_of(
        part->fields, sxml_byte_view_from_c_string(query.field_name));
}

static sxml_ast_operator sxml_xpath_operator_of(sxml_byte_view token) {
    for (size_t entry_index = 0;
         entry_index <
         sizeof sxml_xpath_operators / sizeof sxml_xpath_operators[0];
         entry_index += 1U) {
        if (sxml_byte_view_equals(token,
                                  sxml_xpath_operators[entry_index].token)) {
            return sxml_xpath_operators[entry_index].operator;
        }
    }
    return SXML_AST_OPERATOR_NONE;
}

static sxml_ast_axis sxml_xpath_axis_of(sxml_byte_view name) {
    for (size_t entry_index = 0;
         entry_index < sizeof sxml_xpath_axes / sizeof sxml_xpath_axes[0];
         entry_index += 1U) {
        if (sxml_byte_view_equals(name, sxml_xpath_axes[entry_index].name)) {
            return sxml_xpath_axes[entry_index].axis;
        }
    }
    return SXML_AST_AXIS_NONE;
}

static bool sxml_xpath_is_non_delimiting(TSNode terminal) {
    const char *node_type = ts_node_type(terminal);
    if (!ts_node_is_named(terminal)) {
        /* Keywords are the anonymous terminals that start with a letter. */
        return (node_type[0] >= 'a' && node_type[0] <= 'z') ||
               (node_type[0] >= 'A' && node_type[0] <= 'Z');
    }
    for (size_t entry_index = 0;
         entry_index < sizeof sxml_xpath_non_delimiting_node_types /
                           sizeof sxml_xpath_non_delimiting_node_types[0];
         entry_index += 1U) {
        if (strcmp(node_type,
                   sxml_xpath_non_delimiting_node_types[entry_index]) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * XPath 4.0, A.3.2: two non-delimiting terminals must be separated by
 * whitespace or a comment ("10div 3" is an error). Called for every
 * terminal in source order.
 */
static sxml_status sxml_xpath_check_terminal(sxml_ast_front_end *front_end,
                                             TSNode terminal) {
    const bool is_non_delimiting = sxml_xpath_is_non_delimiting(terminal);
    const uint32_t start_byte = ts_node_start_byte(terminal);
    const sxml_xpath_previous_terminal previous = front_end->previous_terminal;
    if (previous.exists && previous.is_non_delimiting && is_non_delimiting &&
        previous.end_byte == start_byte) {
        return sxml_xpath_report_syntax_error(
            front_end, terminal, sxml_xpath_delimitation_description);
    }
    front_end->previous_terminal = (sxml_xpath_previous_terminal){
        .exists = true,
        .is_non_delimiting = is_non_delimiting,
        .end_byte = ts_node_end_byte(terminal),
    };
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------- Decoding payloads */

/*
 * The bytes of source that piece selects. piece is always inside source:
 * the decoders below only form ranges between positions they have read.
 */
static sxml_byte_view sxml_xpath_slice(sxml_byte_view source,
                                       sxml_byte_range piece) {
    if (piece.byte_count == 0) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return (sxml_byte_view){.bytes = source.bytes + piece.byte_offset,
                            .byte_count = piece.byte_count};
}

/* Bytes of a source text: the range piece of source. */
typedef struct sxml_xpath_source_piece {
    sxml_byte_view source;
    sxml_byte_range piece;
} sxml_xpath_source_piece;

/* Appends the bytes of source_piece to *text (in/out). */
static sxml_status sxml_xpath_append_piece(sxml_ast *ast,
                                           sxml_xpath_source_piece source_piece,
                                           sxml_ast_text *text) {
    if (source_piece.piece.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    /* When *text is still empty this starts a new text, at the same offset
       as the empty one: both are at the end of the store. */
    return sxml_ast_append_text(
        ast, sxml_xpath_slice(source_piece.source, source_piece.piece), text);
}

/* Copies source to a new text of the store. */
static sxml_status sxml_xpath_store_text(sxml_ast *ast, sxml_byte_view source,
                                         sxml_ast_text *text) {
    sxml_ast_text stored = {.byte_offset = 0, .byte_count = 0};
    sxml_status status = sxml_ast_append_text(ast, source, &stored);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *text = stored;
    return SXML_STATUS_OK;
}

/*
 * Copies source to a new text with each doubled escape character written
 * once: a doubled quote in a string literal (XPath 4.0, A.3.1
 * StringLiteral, EscapeQuot and EscapeApos) or "{{", "}}" and "``" in a
 * string template fixed part (XPath 4.0, 4.9.3). escapes lists the
 * characters that are doubled.
 */
typedef struct sxml_xpath_unescape_request {
    sxml_byte_view source;
    sxml_byte_view escapes;
} sxml_xpath_unescape_request;

static bool sxml_xpath_byte_is_in(uint8_t byte, sxml_byte_view set) {
    for (size_t set_index = 0; set_index < set.byte_count; set_index += 1U) {
        if (set.bytes[set_index] == byte) {
            return true;
        }
    }
    return false;
}

static sxml_status
sxml_xpath_store_unescaped(sxml_ast *ast, sxml_xpath_unescape_request request,
                           sxml_ast_text *text) {
    const sxml_byte_view source = request.source;
    sxml_ast_text stored = {.byte_offset = 0, .byte_count = 0};
    /* A new text starts even when the decoded text is empty. */
    sxml_status status = sxml_ast_append_text(
        ast, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, &stored);
    size_t piece_start = 0;
    size_t byte_index = 0;
    while (status == SXML_STATUS_OK && byte_index < source.byte_count) {
        const uint8_t byte = source.bytes[byte_index];
        const bool is_doubled_escape =
            sxml_xpath_byte_is_in(byte, request.escapes) &&
            byte_index + 1U < source.byte_count &&
            source.bytes[byte_index + 1U] == byte;
        if (is_doubled_escape) {
            /* Keep the first of the pair, skip the second. */
            status = sxml_xpath_append_piece(
                ast,
                (sxml_xpath_source_piece){
                    .source = source,
                    .piece = {.byte_offset = piece_start,
                              .byte_count = byte_index + 1U - piece_start}},
                &stored);
            byte_index += 2U;
            piece_start = byte_index;
        } else {
            byte_index += 1U;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_append_piece(
            ast,
            (sxml_xpath_source_piece){
                .source = source,
                .piece = {.byte_offset = piece_start,
                          .byte_count = source.byte_count - piece_start}},
            &stored);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *text = stored;
    return SXML_STATUS_OK;
}

/*
 * Copies the digits of a numeric literal, which digits selects (without
 * the "0x" or "0b" prefix), leaving out "_" separators (XPath 4.0, A.3.1
 * Digits, HexDigits, BinaryDigits).
 */
static sxml_status sxml_xpath_store_digits(sxml_ast *ast,
                                           sxml_xpath_source_piece digits,
                                           sxml_ast_text *text) {
    sxml_ast_text stored = {.byte_offset = 0, .byte_count = 0};
    sxml_status status = sxml_ast_append_text(
        ast, (sxml_byte_view){.bytes = NULL, .byte_count = 0}, &stored);
    const sxml_byte_view source = digits.source;
    const size_t end = digits.piece.byte_offset + digits.piece.byte_count;
    size_t piece_start = digits.piece.byte_offset;
    for (size_t byte_index = piece_start;
         status == SXML_STATUS_OK && byte_index <= end; byte_index += 1U) {
        if (byte_index == end || source.bytes[byte_index] == '_') {
            status = sxml_xpath_append_piece(
                ast,
                (sxml_xpath_source_piece){
                    .source = source,
                    .piece = {.byte_offset = piece_start,
                              .byte_count = byte_index - piece_start}},
                &stored);
            piece_start = byte_index + 1U;
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *text = stored;
    return SXML_STATUS_OK;
}

/* The offset of the first byte equal to target in source, or byte_count. */
static size_t sxml_xpath_find_byte(sxml_byte_view source, uint8_t target) {
    size_t byte_index = 0;
    while (byte_index < source.byte_count &&
           source.bytes[byte_index] != target) {
        byte_index += 1U;
    }
    return byte_index;
}

/*
 * Stores the parts of a name as written (XPath 4.0, A.3.1 EQName, Wildcard):
 * "local", "prefix:local", "Q{uri}local", "Q{uri}prefix:local", and for a
 * wildcard "*", "prefix:*", "*:local" and "Q{uri}*". An empty local name
 * stands for a "*".
 */
static sxml_status sxml_xpath_store_name(sxml_ast *ast, sxml_byte_view written,
                                         sxml_ast_name *name) {
    sxml_ast_name stored = {
        .namespace_prefix = {.byte_offset = 0, .byte_count = 0},
        .local_name = {.byte_offset = 0, .byte_count = 0},
        .namespace_uri = {.byte_offset = 0, .byte_count = 0},
    };
    sxml_status status = SXML_STATUS_OK;
    sxml_byte_view rest = written;
    if (rest.byte_count >= 2U && rest.bytes[0] == 'Q' && rest.bytes[1] == '{') {
        const sxml_byte_view after_brace = {.bytes = rest.bytes + 2,
                                            .byte_count = rest.byte_count - 2U};
        /* BracedURILiteral has no "}" inside (XPath 4.0, A.3.1). */
        const size_t uri_byte_count = sxml_xpath_find_byte(after_brace, '}');
        status = sxml_xpath_store_text(
            ast,
            sxml_xpath_slice(after_brace,
                             (sxml_byte_range){.byte_offset = 0,
                                               .byte_count = uri_byte_count}),
            &stored.namespace_uri);
        const size_t consumed = uri_byte_count < after_brace.byte_count
                                    ? uri_byte_count + 1U
                                    : uri_byte_count;
        rest =
            (sxml_byte_view){.bytes = after_brace.bytes + consumed,
                             .byte_count = after_brace.byte_count - consumed};
    }
    const size_t colon_offset = sxml_xpath_find_byte(rest, ':');
    sxml_byte_view local_name = rest;
    if (status == SXML_STATUS_OK && colon_offset < rest.byte_count) {
        const sxml_byte_view prefix = sxml_xpath_slice(
            rest,
            (sxml_byte_range){.byte_offset = 0, .byte_count = colon_offset});
        /* In "*:local" the "*" stands for any namespace, not a prefix. */
        const bool is_any_prefix =
            prefix.byte_count == 1U && prefix.bytes[0] == '*';
        if (!is_any_prefix) {
            status =
                sxml_xpath_store_text(ast, prefix, &stored.namespace_prefix);
        }
        local_name = sxml_xpath_slice(
            rest, (sxml_byte_range){.byte_offset = colon_offset + 1U,
                                    .byte_count =
                                        rest.byte_count - colon_offset - 1U});
    }
    /* "*" as the local name part is the wildcard, not a name. */
    if (local_name.byte_count == 1U && local_name.bytes[0] == '*') {
        local_name = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_store_text(ast, local_name, &stored.local_name);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *name = stored;
    return SXML_STATUS_OK;
}

/* The name test kind of a wildcard as written (XPath 4.0, 4.6.4.2). */
static sxml_name_test_kind sxml_xpath_wildcard_kind(sxml_byte_view written) {
    if (written.byte_count <= 1U) {
        return SXML_NAME_TEST_KIND_ANY_NAME;
    }
    if (written.bytes[0] == '*') {
        return SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD;
    }
    return SXML_NAME_TEST_KIND_NAMESPACE_WILDCARD;
}

/* Fills the payload of a leaf of kind syntax_node.kind from its text. */
static sxml_status
sxml_xpath_describe_payload(sxml_ast_front_end *front_end,
                            sxml_xpath_syntax_node syntax_node,
                            sxml_ast_node_request *request) {
    const sxml_byte_view written =
        sxml_xpath_node_text(front_end, syntax_node.node);
    sxml_ast *ast = front_end->ast;
    const sxml_xpath_source_piece all_digits = {
        .source = written,
        .piece = {.byte_offset = 0, .byte_count = written.byte_count}};
    /* "0x" and "0b" (XPath 4.0, A.3.1 HexIntegerLiteral and
       BinaryIntegerLiteral); the grammar guarantees they are there. */
    const sxml_xpath_source_piece prefixed_digits = {
        .source = written,
        .piece = {.byte_offset = 2,
                  .byte_count =
                      written.byte_count < 2U ? 0 : written.byte_count - 2U}};
    const sxml_ast_kind kind = syntax_node.kind;
    if (kind == SXML_AST_KIND_INTEGER_LITERAL ||
        kind == SXML_AST_KIND_DECIMAL_LITERAL ||
        kind == SXML_AST_KIND_DOUBLE_LITERAL) {
        return sxml_xpath_store_digits(ast, all_digits, &request->payload.text);
    }
    if (kind == SXML_AST_KIND_HEX_INTEGER_LITERAL ||
        kind == SXML_AST_KIND_BINARY_INTEGER_LITERAL) {
        return sxml_xpath_store_digits(ast, prefixed_digits,
                                       &request->payload.text);
    }
    if (kind == SXML_AST_KIND_STRING_LITERAL) {
        /* The token is a quote, the content and the same quote again. */
        const size_t content_byte_count =
            written.byte_count < 2U ? 0 : written.byte_count - 2U;
        const sxml_byte_view content = sxml_xpath_slice(
            written, (sxml_byte_range){.byte_offset = 1,
                                       .byte_count = content_byte_count});
        const sxml_byte_view quote = sxml_xpath_slice(
            written,
            (sxml_byte_range){.byte_offset = 0,
                              .byte_count = written.byte_count == 0 ? 0 : 1});
        return sxml_xpath_store_unescaped(
            ast,
            (sxml_xpath_unescape_request){.source = content, .escapes = quote},
            &request->payload.text);
    }
    if (kind == SXML_AST_KIND_STRING_TEMPLATE_FIXED_PART) {
        return sxml_xpath_store_unescaped(
            ast,
            (sxml_xpath_unescape_request){
                .source = written,
                .escapes = SXML_BYTE_VIEW_FROM_LITERAL("{}`")},
            &request->payload.text);
    }
    if (kind == SXML_AST_KIND_WILDCARD) {
        request->detail.name_test_kind = sxml_xpath_wildcard_kind(written);
        if (written.byte_count >= 2U && written.bytes[0] == 'Q') {
            request->flags |= (uint32_t)SXML_AST_FLAG_BRACED_URI;
        }
        return sxml_xpath_store_name(ast, written, &request->payload.name);
    }
    if (kind == SXML_AST_KIND_NCNAME || kind == SXML_AST_KIND_PREFIXED_NAME ||
        kind == SXML_AST_KIND_URI_QUALIFIED_NAME ||
        kind == SXML_AST_KIND_QNAME) {
        return sxml_xpath_store_name(ast, written, &request->payload.name);
    }
    return SXML_STATUS_OK;
}

/* What an anonymous child or an absorbed named child adds to its parent. */
typedef struct sxml_xpath_token {
    TSNode node;
    /* The field the token has in its parent; NULL when it has none. */
    const char *field_name;
} sxml_xpath_token;

static bool sxml_xpath_names_field(sxml_xpath_token token,
                                   sxml_byte_view field_name) {
    return token.field_name != NULL &&
           sxml_byte_view_equals(sxml_byte_view_from_c_string(token.field_name),
                                 field_name);
}

/* Adds what token says to request (detail and flags, D52). */
static void sxml_xpath_apply_token(const sxml_ast_front_end *front_end,
                                   sxml_xpath_token token,
                                   sxml_ast_node_request *request) {
    const sxml_byte_view written = sxml_xpath_node_text(front_end, token.node);
    if (sxml_xpath_names_field(token,
                               SXML_BYTE_VIEW_FROM_LITERAL("operator"))) {
        request->detail.operator= sxml_xpath_operator_of(written);
    } else if (sxml_xpath_names_field(
                   token, SXML_BYTE_VIEW_FROM_LITERAL("quantifier"))) {
        request->detail.operator= sxml_byte_view_equals(
            written, SXML_BYTE_VIEW_FROM_LITERAL("some"))
            ? SXML_AST_OPERATOR_SOME
            : SXML_AST_OPERATOR_EVERY;
    } else if (sxml_xpath_names_field(
                   token, SXML_BYTE_VIEW_FROM_LITERAL("abbreviation"))) {
        /* XPath 4.0, 4.6.6: "@" is attribute::, ".." is parent::node(). */
        request->detail.axis =
            sxml_byte_view_equals(written, SXML_BYTE_VIEW_FROM_LITERAL("@"))
                ? SXML_AST_AXIS_ATTRIBUTE
                : SXML_AST_AXIS_PARENT;
        request->flags |= (uint32_t)SXML_AST_FLAG_ABBREVIATED;
    } else if (sxml_xpath_names_field(token,
                                      SXML_BYTE_VIEW_FROM_LITERAL("axis"))) {
        const TSNode axis_name =
            ts_node_child_by_field_name(token.node, "name", 4);
        request->detail.axis =
            sxml_xpath_axis_of(sxml_xpath_node_text(front_end, axis_name));
    } else if (sxml_xpath_names_field(
                   token, SXML_BYTE_VIEW_FROM_LITERAL("occurrence"))) {
        if (sxml_byte_view_equals(written, SXML_BYTE_VIEW_FROM_LITERAL("?"))) {
            request->detail.occurrence = SXML_OCCURRENCE_ZERO_OR_ONE;
        } else if (sxml_byte_view_equals(written,
                                         SXML_BYTE_VIEW_FROM_LITERAL("*"))) {
            request->detail.occurrence = SXML_OCCURRENCE_ZERO_OR_MORE;
        } else {
            request->detail.occurrence = SXML_OCCURRENCE_ONE_OR_MORE;
        }
    } else if (sxml_xpath_names_field(token,
                                      SXML_BYTE_VIEW_FROM_LITERAL("empty"))) {
        request->detail.occurrence = SXML_OCCURRENCE_ZERO;
    } else if (sxml_xpath_names_field(
                   token, SXML_BYTE_VIEW_FROM_LITERAL("optional"))) {
        request->flags |= (uint32_t)SXML_AST_FLAG_OPTIONAL;
    } else if (sxml_xpath_names_field(
                   token, SXML_BYTE_VIEW_FROM_LITERAL("nillable"))) {
        request->flags |= (uint32_t)SXML_AST_FLAG_NILLABLE;
    } else if (sxml_xpath_names_field(token,
                                      SXML_BYTE_VIEW_FROM_LITERAL("value"))) {
        /* The only token in a `value` field is that of boolean_constant. */
        if (sxml_byte_view_equals(written,
                                  SXML_BYTE_VIEW_FROM_LITERAL("true"))) {
            request->flags |= (uint32_t)SXML_AST_FLAG_TRUE;
        }
    } else if (request->kind == SXML_AST_KIND_INLINE_FUNCTION_EXPRESSION &&
               sxml_byte_view_equals(written,
                                     SXML_BYTE_VIEW_FROM_LITERAL("("))) {
        /* XPath 4.0, 4.5.6: a parameter list makes it not a focus function. */
        request->flags |= (uint32_t)SXML_AST_FLAG_HAS_SIGNATURE;
    }
}

/*
 * The end byte of the ")" that closes the "(" of a jnode() root selector.
 * The grammar aliases each of the two tokens to jroot_selector; the AST has
 * one node for both (D52), which spans them.
 */
static uint32_t sxml_xpath_jroot_selector_end(TSNode opening) {
    TSNode sibling = ts_node_next_sibling(opening);
    while (!ts_node_is_null(sibling)) {
        if (strcmp(ts_node_type(sibling), "jroot_selector") == 0) {
            return ts_node_end_byte(sibling);
        }
        sibling = ts_node_next_sibling(sibling);
    }
    return ts_node_end_byte(opening);
}

/*
 * Lets the part claim token (D111); true when it did, so the XPath rules
 * are skipped.
 */
static bool sxml_xpath_apply_part_token(const sxml_ast_front_end *front_end,
                                        sxml_xpath_token token,
                                        sxml_ast_node_request *request) {
    const sxml_ast_parser_part *part = front_end->part;
    if (part == NULL || part->apply_token == NULL) {
        return false;
    }
    const sxml_byte_view field_name =
        token.field_name == NULL
            ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
            : sxml_byte_view_from_c_string(token.field_name);
    return part->apply_token(
        (sxml_ast_parser_token){
            .field_name = field_name,
            .written = sxml_xpath_node_text(front_end, token.node)},
        request);
}

/*
 * Fills the payload of request: through the part's describe_payload when
 * there is one and it describes the node (D111), else with the XPath
 * decoders.
 */
static sxml_status
sxml_xpath_describe_part_payload(sxml_ast_front_end *front_end,
                                 sxml_xpath_syntax_node syntax_node,
                                 sxml_ast_node_request *request) {
    const sxml_ast_parser_part *part = front_end->part;
    if (part != NULL && part->describe_payload != NULL) {
        sxml_ast_payload_outcome outcome = {.is_described = false};
        const sxml_status status = part->describe_payload(
            (sxml_ast_parser_node){
                .front_end = front_end,
                .written = sxml_xpath_node_text(front_end, syntax_node.node)},
            request, &outcome);
        if (status != SXML_STATUS_OK || outcome.is_described) {
            return status;
        }
    }
    return sxml_xpath_describe_payload(front_end, syntax_node, request);
}

/* Fills *request for syntax_node: kind, span, payload, detail and flags. */
static sxml_status sxml_xpath_describe_node(sxml_ast_front_end *front_end,
                                            sxml_xpath_syntax_node syntax_node,
                                            sxml_ast_node_request *request) {
    sxml_ast_node_request described = {
        .kind = syntax_node.kind,
        .detail =
            {
                .operator= SXML_AST_OPERATOR_NONE },
                .flags = 0,
                .source_span = sxml_xpath_span_of(front_end, syntax_node.node),
                .payload = {.text = {.byte_offset = 0, .byte_count = 0}},
        };
    if (syntax_node.kind == SXML_AST_KIND_SEQUENCE_TYPE) {
        /* No indicator means exactly one (XPath 4.0, 3.1.1). */
        described.detail.occurrence = SXML_OCCURRENCE_EXACTLY_ONE;
    }
    if (syntax_node.kind == SXML_AST_KIND_JROOT_SELECTOR) {
        const uint32_t end = sxml_xpath_text_offset(
            front_end, sxml_xpath_jroot_selector_end(syntax_node.node));
        described.source_span.byte_count =
            end - described.source_span.byte_offset;
    }
    sxml_status status =
        sxml_xpath_describe_part_payload(front_end, syntax_node, &described);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    TSTreeCursor *scan_cursor = &front_end->scan_cursor;
    ts_tree_cursor_reset(scan_cursor, syntax_node.node);
    bool has_child = ts_tree_cursor_goto_first_child(scan_cursor);
    while (has_child) {
        const TSNode child = ts_tree_cursor_current_node(scan_cursor);
        if (sxml_xpath_kind_of(front_end, child) == SXML_AST_KIND_NONE) {
            const sxml_xpath_token token = {
                .node = child,
                .field_name = ts_tree_cursor_current_field_name(scan_cursor)};
            if (!sxml_xpath_apply_part_token(front_end, token, &described)) {
                sxml_xpath_apply_token(front_end, token, &described);
            }
        }
        has_child = ts_tree_cursor_goto_next_sibling(scan_cursor);
    }
    *request = described;
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------------- Walk */

/* The walk's place in the AST. */
typedef struct sxml_xpath_walk {
    /* The AST node that the cursor's current children belong to; 0 above
       the top node. */
    sxml_ast_node_index parent;
    /* How many AST nodes are open above the cursor (parent's depth). */
    uint32_t depth;
    sxml_ast_node_index top;
} sxml_xpath_walk;

/* True for a jroot_selector that is the ")" of "()" (see above). */
static bool
sxml_xpath_is_closing_jroot_selector(const sxml_ast_front_end *front_end,
                                     sxml_xpath_syntax_node syntax_node) {
    if (syntax_node.kind != SXML_AST_KIND_JROOT_SELECTOR) {
        return false;
    }
    const sxml_byte_view written =
        sxml_xpath_node_text(front_end, syntax_node.node);
    return written.byte_count == 1U && written.bytes[0] == ')';
}

/* Calls the part's finish_node for a complete node (D111). */
static sxml_status sxml_xpath_finish_node(sxml_ast_front_end *front_end,
                                          sxml_ast_node_index node) {
    const sxml_ast_parser_part *part = front_end->part;
    if (part == NULL || part->finish_node == NULL) {
        return SXML_STATUS_OK;
    }
    return part->finish_node(front_end, node);
}

/*
 * Makes the AST node for the cursor's current node, when it makes one, and
 * appends it to walk->parent in the field the cursor reports; walk is
 * in/out: a new node with children becomes the parent of what follows.
 */
static sxml_status sxml_xpath_visit(sxml_ast_front_end *front_end,
                                    const TSTreeCursor *cursor,
                                    sxml_xpath_walk *walk) {
    const TSNode node = ts_tree_cursor_current_node(cursor);
    const sxml_xpath_syntax_node syntax_node = {
        .node = node, .kind = sxml_xpath_kind_of(front_end, node)};
    if (syntax_node.kind == SXML_AST_KIND_NONE ||
        sxml_xpath_is_closing_jroot_selector(front_end, syntax_node)) {
        return SXML_STATUS_OK;
    }
    const uint32_t maximum_depth = front_end->request->maximum_expression_depth;
    if (maximum_depth != 0 && walk->depth >= maximum_depth) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    sxml_ast_field field = SXML_AST_FIELD_NONE;
    if (walk->parent.value != 0) {
        const sxml_ast_kind parent_kind =
            sxml_ast_get_node(front_end->ast, walk->parent)->kind;
        field = sxml_xpath_field_of(
            front_end, (sxml_xpath_field_query){
                           .field_name =
                               ts_tree_cursor_current_field_name(cursor),
                           .parent_kind = parent_kind});
        if (field == SXML_AST_FIELD_NONE) {
            /* Every named child of grammars/xpath has a field; a node
               without one is not XPath this front end knows. */
            return sxml_xpath_report_syntax_error(
                front_end, node, front_end->syntax_error_description);
        }
        /* XPath 4.0, 4.7.3: KeySpecifier allows an NCName, not a QName; the
           grammar accepts "a:b" there to keep it one token (D29). */
        if (syntax_node.kind == SXML_AST_KIND_PREFIXED_NAME &&
            field == SXML_AST_FIELD_KEY &&
            (parent_kind == SXML_AST_KIND_LOOKUP ||
             parent_kind == SXML_AST_KIND_UNARY_LOOKUP)) {
            return sxml_xpath_report_syntax_error(
                front_end, node, sxml_xpath_prefixed_key_description);
        }
    }
    sxml_ast_node_request request = {.kind = SXML_AST_KIND_NONE};
    sxml_status status =
        sxml_xpath_describe_node(front_end, syntax_node, &request);
    sxml_ast_node_index added = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ast_add_node(front_end->ast, &request, &added);
    }
    if (status == SXML_STATUS_OK && walk->parent.value != 0) {
        status = sxml_ast_append_child(
            front_end->ast, (sxml_ast_child_request){.parent = walk->parent,
                                                     .field = field,
                                                     .child = added});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (walk->parent.value == 0) {
        walk->top = added;
    }
    /* The AST node opens for the children the cursor visits next; a leaf
       has none, so the walk never descends or returns through it, and it
       is complete now. */
    if (ts_node_child_count(node) != 0) {
        walk->parent = added;
        walk->depth += 1U;
        return SXML_STATUS_OK;
    }
    return sxml_xpath_finish_node(front_end, added);
}

/*
 * Closes walk->parent, whose concrete syntax tree node the walk leaves:
 * the part's finish_node, then its parent becomes the walk's parent.
 */
static sxml_status sxml_xpath_close_node(sxml_ast_front_end *front_end,
                                         sxml_xpath_walk *walk) {
    const sxml_status status = sxml_xpath_finish_node(front_end, walk->parent);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    walk->parent = sxml_ast_get_node(front_end->ast, walk->parent)->parent;
    walk->depth -= 1U;
    return SXML_STATUS_OK;
}

/*
 * Walks the concrete syntax tree below top in source order and builds its
 * AST; writes the AST node of top to *top_node.
 */
static sxml_status sxml_xpath_build_tree(sxml_ast_front_end *front_end,
                                         TSNode top,
                                         sxml_ast_node_index *top_node) {
    TSTreeCursor cursor = ts_tree_cursor_new(top);
    sxml_xpath_walk walk = {
        .parent = {.value = 0}, .depth = 0, .top = {.value = 0}};
    sxml_status status = SXML_STATUS_OK;
    bool is_walking = true;
    while (is_walking) {
        const TSNode node = ts_tree_cursor_current_node(&cursor);
        if (ts_node_child_count(node) == 0) {
            status = sxml_xpath_check_terminal(front_end, node);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_xpath_visit(front_end, &cursor, &walk);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (ts_tree_cursor_goto_first_child(&cursor)) {
            continue;
        }
        /* Up until a next sibling; each node left on the way up that made
           an AST node with children closes it. */
        while (!ts_tree_cursor_goto_next_sibling(&cursor)) {
            if (!ts_tree_cursor_goto_parent(&cursor)) {
                is_walking = false;
                break;
            }
            if (sxml_xpath_kind_of(front_end,
                                   ts_tree_cursor_current_node(&cursor)) !=
                SXML_AST_KIND_NONE) {
                status = sxml_xpath_close_node(front_end, &walk);
                if (status != SXML_STATUS_OK) {
                    is_walking = false;
                    break;
                }
            }
        }
    }
    ts_tree_cursor_delete(&cursor);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *top_node = walk.top;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Entry */

/*
 * The first ERROR or MISSING node in source order, or a null node when the
 * tree has none. Iterative, so a deep tree cannot exhaust the stack.
 */
static TSNode sxml_xpath_find_error(TSNode root) {
    TSNode found = {.context = {0, 0, 0, 0}, .id = NULL, .tree = NULL};
    if (!ts_node_has_error(root)) {
        return found;
    }
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    bool is_searching = true;
    while (is_searching) {
        const TSNode node = ts_tree_cursor_current_node(&cursor);
        if (ts_node_is_error(node) || ts_node_is_missing(node)) {
            found = node;
            break;
        }
        if (ts_node_has_error(node) &&
            ts_tree_cursor_goto_first_child(&cursor)) {
            continue;
        }
        while (!ts_tree_cursor_goto_next_sibling(&cursor)) {
            if (!ts_tree_cursor_goto_parent(&cursor)) {
                is_searching = false;
                break;
            }
        }
    }
    ts_tree_cursor_delete(&cursor);
    if (ts_node_is_null(found)) {
        /* has_error without an ERROR or MISSING descendant: report the
           root rather than accept the text. */
        return root;
    }
    return found;
}

/*
 * The node the AST is built from: the root for an expression; for a
 * sequence type, the type of the instance-of expression the prefix starts,
 * which must be the whole text. Writes a null node when the text is not
 * that.
 */
static TSNode sxml_xpath_select_top(const sxml_ast_front_end *front_end,
                                    TSNode root) {
    const TSNode null_node = {
        .context = {0, 0, 0, 0}, .id = NULL, .tree = NULL};
    if (front_end->part != NULL) {
        /* A part's grammar: its start rule's node, whole (D111). */
        return sxml_byte_view_equals(
                   sxml_byte_view_from_c_string(ts_node_type(root)),
                   front_end->part->root_node_type)
                   ? root
                   : null_node;
    }
    if (strcmp(ts_node_type(root), "xpath") != 0) {
        return null_node;
    }
    if (front_end->prefix.byte_count == 0) {
        return root;
    }
    const TSNode body = ts_node_child_by_field_name(root, "body", 4);
    if (ts_node_is_null(body) ||
        strcmp(ts_node_type(body), "instance_of_expression") != 0) {
        return null_node;
    }
    const TSNode value = ts_node_child_by_field_name(body, "value", 5);
    const TSNode type = ts_node_child_by_field_name(body, "type", 4);
    /* The prefix's "()" must be the value, and the type must start in the
       text, so the type is all of the text. */
    if (ts_node_is_null(value) || ts_node_is_null(type) ||
        ts_node_start_byte(value) != 0 || ts_node_end_byte(value) != 2U ||
        ts_node_start_byte(type) < front_end->prefix.byte_count) {
        return null_node;
    }
    return type;
}

/* Fills the caller's error record for XPST0003 (XPath 4.0, 2.4.1) or the
   code a part raised. */
static void
sxml_xpath_fill_error_record(const sxml_ast_front_end *front_end) {
    sxml_error_record *error_record = front_end->request->error_record;
    if (error_record == NULL) {
        return;
    }
    sxml_xpath_text_position position = {
        .byte_offset = 0, .line_number = 1, .column_number = 1};
    sxml_xpath_advance_position(front_end->request->expression_text,
                                front_end->error_byte_offset, &position);
    *error_record = (sxml_error_record){
        .code =
            {
                .namespace_uri =
                    SXML_BYTE_VIEW_FROM_LITERAL(SXML_ERROR_NAMESPACE_URI),
                .local_name = front_end->error_code,
                .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err"),
            },
        .value = NULL,
        .diagnostic =
            {
                .description = front_end->error_description,
                .location =
                    {
                        .source_name = front_end->request->source_name,
                        .line_number = position.line_number,
                        .column_number = position.column_number,
                        .byte_offset = position.byte_offset,
                    },
            },
    };
}

/* Builds the AST of the top node of tree into a new AST. */
static sxml_status sxml_xpath_build_from_tree(sxml_ast_front_end *front_end,
                                              TSNode root, sxml_ast **ast) {
    const TSNode error_node = sxml_xpath_find_error(root);
    if (!ts_node_is_null(error_node)) {
        return sxml_xpath_report_syntax_error(
            front_end, error_node,
            ts_node_is_missing(error_node)
                ? front_end->missing_token_description
                : front_end->syntax_error_description);
    }
    const TSNode top = sxml_xpath_select_top(front_end, root);
    if (ts_node_is_null(top)) {
        return sxml_xpath_report_syntax_error(
            front_end, root,
            front_end->part == NULL ? sxml_xpath_not_sequence_type_description
                                    : front_end->syntax_error_description);
    }
    /* D52: every AST node comes from a distinct concrete syntax tree node,
       and decoded text is never longer than its source. */
    sxml_ast *new_ast = NULL;
    sxml_status status =
        sxml_ast_create(front_end->request->allocator,
                        &(sxml_ast_configuration){
                            .node_capacity = ts_node_descendant_count(top),
                            .text_byte_capacity =
                                front_end->request->expression_text.byte_count,
                            .source_name = front_end->request->source_name,
                        },
                        &new_ast);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    front_end->ast = new_ast;
    sxml_ast_node_index top_node = {.value = 0};
    status = sxml_xpath_build_tree(front_end, top, &top_node);
    front_end->ast = NULL;
    if (status != SXML_STATUS_OK) {
        sxml_ast_destroy(new_ast);
        return status;
    }
    new_ast->root = top_node;
    *ast = new_ast;
    return SXML_STATUS_OK;
}

/* What to parse the request's text with: the XPath grammar after a
   prefix, or a part's grammar (D111). */
typedef struct sxml_xpath_grammar {
    sxml_byte_view prefix;
    const sxml_ast_parser_part *part;
} sxml_xpath_grammar;

/* The description, or fallback when it is empty. */
static sxml_byte_view sxml_xpath_description_or(sxml_byte_view description,
                                                sxml_byte_view fallback) {
    return description.byte_count == 0 ? fallback : description;
}

/* Parses the request's text as grammar says and builds the AST. */
static sxml_status sxml_xpath_parse(const sxml_ast_build_request *request,
                                    sxml_xpath_grammar grammar,
                                    sxml_ast **ast) {
    const sxml_byte_view prefix = grammar.prefix;
    const sxml_ast_parser_part *part = grammar.part;
    const sxml_byte_view text = request->expression_text;
    if (text.bytes == NULL && text.byte_count != 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (text.byte_count > (size_t)UINT32_MAX - prefix.byte_count) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_utf8_validation validation = {
        .valid_byte_count = 0, .codepoint_count = 0, .is_ascii = false};
    sxml_status status = sxml_utf8_validate(text, &validation);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const TSLanguage *language =
        part == NULL ? tree_sitter_xpath()
        : part->language == NULL ? NULL
                                 : part->language();
    TSParser *parser = ts_parser_new();
    if (language == NULL || !ts_parser_set_language(parser, language)) {
        ts_parser_delete(parser);
        return SXML_STATUS_UNSUPPORTED_FEATURE;
    }
    const sxml_byte_view no_description = {.bytes = NULL, .byte_count = 0};
    sxml_ast_front_end front_end = {
        .request = request,
        .part = part,
        .syntax_error_description = sxml_xpath_description_or(
            part == NULL ? no_description : part->syntax_error_description,
            sxml_xpath_syntax_error_description),
        .missing_token_description = sxml_xpath_description_or(
            part == NULL ? no_description : part->missing_token_description,
            sxml_xpath_missing_token_description),
        .prefix = prefix,
        .ast = NULL,
        .position = {.byte_offset = 0, .line_number = 1, .column_number = 1},
        .previous_terminal = {.exists = false,
                              .is_non_delimiting = false,
                              .end_byte = 0},
        .error_code = sxml_xpath_syntax_error_code,
        .error_description = {.bytes = NULL, .byte_count = 0},
        .error_byte_offset = 0,
    };
    TSTree *tree = ts_parser_parse(parser, NULL,
                                   (TSInput){
                                       .payload = &front_end,
                                       .read = sxml_xpath_read_input,
                                       .encoding = TSInputEncodingUTF8,
                                       .decode = NULL,
                                   });
    if (tree == NULL) {
        /* Only a cancelled parse returns no tree; sxml never cancels. */
        ts_parser_delete(parser);
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const TSNode root = ts_tree_root_node(tree);
    front_end.scan_cursor = ts_tree_cursor_new(root);
    status = sxml_xpath_build_from_tree(&front_end, root, ast);
    ts_tree_cursor_delete(&front_end.scan_cursor);
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    if (status == SXML_STATUS_STATIC_ERROR) {
        sxml_xpath_fill_error_record(&front_end);
    }
    return status;
}

sxml_status sxml_xpath_build_ast(const sxml_ast_build_request *request,
                                 sxml_ast **ast) {
    return sxml_xpath_parse(
        request,
        (sxml_xpath_grammar){.prefix = {.bytes = NULL, .byte_count = 0},
                             .part = NULL},
        ast);
}

sxml_status
sxml_xpath_build_sequence_type_ast(const sxml_ast_build_request *request,
                                   sxml_ast **ast) {
    return sxml_xpath_parse(
        request,
        (sxml_xpath_grammar){.prefix = SXML_BYTE_VIEW_FROM_LITERAL(
                                 sxml_sequence_type_prefix),
                             .part = NULL},
        ast);
}

/* ------------------------------------------------------- Parser parts */

sxml_status
sxml_ast_build_with_parser_part(const sxml_ast_build_request *request,
                                const sxml_ast_parser_part *part,
                                sxml_ast **ast) {
    if (part == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_xpath_parse(
        request,
        (sxml_xpath_grammar){.prefix = {.bytes = NULL, .byte_count = 0},
                             .part = part},
        ast);
}

sxml_ast *sxml_ast_front_end_get_ast(sxml_ast_front_end *front_end) {
    return front_end->ast;
}

sxml_byte_view
sxml_ast_front_end_get_expression_text(const sxml_ast_front_end *front_end) {
    return front_end->request->expression_text;
}

sxml_status sxml_ast_front_end_raise(sxml_ast_front_end *front_end,
                                     sxml_ast_static_error error) {
    front_end->error_code = error.error_code;
    front_end->error_description = error.description;
    front_end->error_byte_offset = error.byte_offset;
    return SXML_STATUS_STATIC_ERROR;
}
