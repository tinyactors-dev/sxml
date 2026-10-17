/*
 * Tests for the XQuery 4.0 front end (src/xquery_parse.c): text to AST
 * through grammars/xquery and the XPath walker's parser part seam (D111),
 * the decoding of XQuery literals and direct constructor text, and the
 * checks the grammar leaves to the tree builder (D29).
 *
 * Golden file: tests/xpath_parse_test.c. Each AST is checked against a
 * compact written form, made by write_node below:
 *
 *     (kind keyword flags "payload" field=(child ...) ...)
 *
 * where keyword is the keyword or decimal format property detail, flags
 * are written by name, and the payload is the decoded text, the name as
 * {uri}prefix:local, or a processing instruction's target and "content".
 */
#include "test_runner.h"
#include "xpath_ast_internal.h"

#include <stdalign.h>
#include <stdio.h>
#include <string.h>

/* Caller memory for objects placed with initialize; 16 suits every type. */
#define TEST_STORAGE_ALIGNMENT 16

/* ------------------------------------------------------ Writing an AST */

/*
 * Kind and field names as the grammar spells them, by designated index so
 * that kinds and fields appended later (D111) need no change here.
 */
static const char *const kind_names[SXML_AST_KIND_COUNT] = {
    [SXML_AST_KIND_XPATH] = "xpath",
    [SXML_AST_KIND_DEFAULT_ELEMENT_NAMESPACE_DECLARATION] = "default_element_namespace_declaration",
    [SXML_AST_KIND_NAMESPACE_DECLARATION] = "namespace_declaration",
    [SXML_AST_KIND_SEQUENCE_EXPRESSION] = "sequence_expression",
    [SXML_AST_KIND_PARENTHESIZED_EXPRESSION] = "parenthesized_expression",
    [SXML_AST_KIND_ENCLOSED_EXPRESSION] = "enclosed_expression",
    [SXML_AST_KIND_CONTEXT_VALUE_REFERENCE] = "context_value_reference",
    [SXML_AST_KIND_VARIABLE_REFERENCE] = "variable_reference",
    [SXML_AST_KIND_INTEGER_LITERAL] = "integer_literal",
    [SXML_AST_KIND_HEX_INTEGER_LITERAL] = "hex_integer_literal",
    [SXML_AST_KIND_BINARY_INTEGER_LITERAL] = "binary_integer_literal",
    [SXML_AST_KIND_DECIMAL_LITERAL] = "decimal_literal",
    [SXML_AST_KIND_DOUBLE_LITERAL] = "double_literal",
    [SXML_AST_KIND_STRING_LITERAL] = "string_literal",
    [SXML_AST_KIND_QNAME_LITERAL] = "qname_literal",
    [SXML_AST_KIND_NEGATIVE_NUMERIC_LITERAL] = "negative_numeric_literal",
    [SXML_AST_KIND_BOOLEAN_CONSTANT] = "boolean_constant",
    [SXML_AST_KIND_STRING_TEMPLATE] = "string_template",
    [SXML_AST_KIND_STRING_TEMPLATE_FIXED_PART] = "string_template_fixed_part",
    [SXML_AST_KIND_FLWOR_EXPRESSION] = "flwor_expression",
    [SXML_AST_KIND_FOR_CLAUSE] = "for_clause",
    [SXML_AST_KIND_FOR_ITEM_BINDING] = "for_item_binding",
    [SXML_AST_KIND_FOR_MEMBER_BINDING] = "for_member_binding",
    [SXML_AST_KIND_FOR_ENTRY_BINDING] = "for_entry_binding",
    [SXML_AST_KIND_FOR_ENTRY_KEY_BINDING] = "for_entry_key_binding",
    [SXML_AST_KIND_FOR_ENTRY_VALUE_BINDING] = "for_entry_value_binding",
    [SXML_AST_KIND_POSITIONAL_VARIABLE] = "positional_variable",
    [SXML_AST_KIND_LET_CLAUSE] = "let_clause",
    [SXML_AST_KIND_LET_VALUE_BINDING] = "let_value_binding",
    [SXML_AST_KIND_LET_SEQUENCE_BINDING] = "let_sequence_binding",
    [SXML_AST_KIND_LET_ARRAY_BINDING] = "let_array_binding",
    [SXML_AST_KIND_LET_MAP_BINDING] = "let_map_binding",
    [SXML_AST_KIND_VARIABLE_NAME_AND_TYPE] = "variable_name_and_type",
    [SXML_AST_KIND_RETURN_CLAUSE] = "return_clause",
    [SXML_AST_KIND_QUANTIFIED_EXPRESSION] = "quantified_expression",
    [SXML_AST_KIND_QUANTIFIER_BINDING] = "quantifier_binding",
    [SXML_AST_KIND_IF_EXPRESSION] = "if_expression",
    [SXML_AST_KIND_OR_EXPRESSION] = "or_expression",
    [SXML_AST_KIND_AND_EXPRESSION] = "and_expression",
    [SXML_AST_KIND_COMPARISON_EXPRESSION] = "comparison_expression",
    [SXML_AST_KIND_OTHERWISE_EXPRESSION] = "otherwise_expression",
    [SXML_AST_KIND_STRING_CONCATENATION_EXPRESSION] = "string_concatenation_expression",
    [SXML_AST_KIND_RANGE_EXPRESSION] = "range_expression",
    [SXML_AST_KIND_ADDITIVE_EXPRESSION] = "additive_expression",
    [SXML_AST_KIND_MULTIPLICATIVE_EXPRESSION] = "multiplicative_expression",
    [SXML_AST_KIND_UNION_EXPRESSION] = "union_expression",
    [SXML_AST_KIND_INTERSECT_EXCEPT_EXPRESSION] = "intersect_except_expression",
    [SXML_AST_KIND_BUT_WITH_EXPRESSION] = "but_with_expression",
    [SXML_AST_KIND_PIPELINE_EXPRESSION] = "pipeline_expression",
    [SXML_AST_KIND_SIMPLE_MAP_EXPRESSION] = "simple_map_expression",
    [SXML_AST_KIND_RELATIVE_PATH_EXPRESSION] = "relative_path_expression",
    [SXML_AST_KIND_INSTANCE_OF_EXPRESSION] = "instance_of_expression",
    [SXML_AST_KIND_TREAT_EXPRESSION] = "treat_expression",
    [SXML_AST_KIND_CASTABLE_EXPRESSION] = "castable_expression",
    [SXML_AST_KIND_CAST_EXPRESSION] = "cast_expression",
    [SXML_AST_KIND_CAST_TARGET] = "cast_target",
    [SXML_AST_KIND_ARROW_EXPRESSION] = "arrow_expression",
    [SXML_AST_KIND_RESTRICTED_DYNAMIC_CALL] = "restricted_dynamic_call",
    [SXML_AST_KIND_UNARY_EXPRESSION] = "unary_expression",
    [SXML_AST_KIND_ABSOLUTE_PATH_EXPRESSION] = "absolute_path_expression",
    [SXML_AST_KIND_AXIS_STEP] = "axis_step",
    [SXML_AST_KIND_UNION_NODE_TEST] = "union_node_test",
    [SXML_AST_KIND_DYNAMIC_NODE_TEST] = "dynamic_node_test",
    [SXML_AST_KIND_WILDCARD] = "wildcard",
    [SXML_AST_KIND_FILTER_EXPRESSION] = "filter_expression",
    [SXML_AST_KIND_PREDICATE] = "predicate",
    [SXML_AST_KIND_DYNAMIC_FUNCTION_CALL] = "dynamic_function_call",
    [SXML_AST_KIND_LOOKUP_EXPRESSION] = "lookup_expression",
    [SXML_AST_KIND_LOOKUP] = "lookup",
    [SXML_AST_KIND_UNARY_LOOKUP] = "unary_lookup",
    [SXML_AST_KIND_LOOKUP_WILDCARD] = "lookup_wildcard",
    [SXML_AST_KIND_METHOD_CALL] = "method_call",
    [SXML_AST_KIND_FUNCTION_CALL] = "function_call",
    [SXML_AST_KIND_ARGUMENT_LIST] = "argument_list",
    [SXML_AST_KIND_POSITIONAL_ARGUMENT_LIST] = "positional_argument_list",
    [SXML_AST_KIND_ARGUMENT_PLACEHOLDER] = "argument_placeholder",
    [SXML_AST_KIND_KEYWORD_ARGUMENT] = "keyword_argument",
    [SXML_AST_KIND_NAMED_FUNCTION_REFERENCE] = "named_function_reference",
    [SXML_AST_KIND_INLINE_FUNCTION_EXPRESSION] = "inline_function_expression",
    [SXML_AST_KIND_MAP_CONSTRUCTOR] = "map_constructor",
    [SXML_AST_KIND_MAP_CONSTRUCTOR_ENTRY] = "map_constructor_entry",
    [SXML_AST_KIND_SQUARE_ARRAY_CONSTRUCTOR] = "square_array_constructor",
    [SXML_AST_KIND_CURLY_ARRAY_CONSTRUCTOR] = "curly_array_constructor",
    [SXML_AST_KIND_COMPUTED_DOCUMENT_CONSTRUCTOR] = "computed_document_constructor",
    [SXML_AST_KIND_COMPUTED_TEXT_CONSTRUCTOR] = "computed_text_constructor",
    [SXML_AST_KIND_COMPUTED_COMMENT_CONSTRUCTOR] = "computed_comment_constructor",
    [SXML_AST_KIND_COMPUTED_ELEMENT_CONSTRUCTOR] = "computed_element_constructor",
    [SXML_AST_KIND_COMPUTED_ATTRIBUTE_CONSTRUCTOR] = "computed_attribute_constructor",
    [SXML_AST_KIND_COMPUTED_NAMESPACE_CONSTRUCTOR] = "computed_namespace_constructor",
    [SXML_AST_KIND_COMPUTED_PROCESSING_INSTRUCTION_CONSTRUCTOR] = "computed_processing_instruction_constructor",
    [SXML_AST_KIND_MARKED_NCNAME] = "marked_ncname",
    [SXML_AST_KIND_TYPE_DECLARATION] = "type_declaration",
    [SXML_AST_KIND_SEQUENCE_TYPE] = "sequence_type",
    [SXML_AST_KIND_ANY_ITEM_TYPE] = "any_item_type",
    [SXML_AST_KIND_GNODE_TYPE] = "gnode_type",
    [SXML_AST_KIND_JNODE_TYPE] = "jnode_type",
    [SXML_AST_KIND_JROOT_SELECTOR] = "jroot_selector",
    [SXML_AST_KIND_DOCUMENT_NODE_TYPE] = "document_node_type",
    [SXML_AST_KIND_ELEMENT_NODE_TYPE] = "element_node_type",
    [SXML_AST_KIND_ATTRIBUTE_NODE_TYPE] = "attribute_node_type",
    [SXML_AST_KIND_SCHEMA_ELEMENT_NODE_TYPE] = "schema_element_node_type",
    [SXML_AST_KIND_SCHEMA_ATTRIBUTE_NODE_TYPE] = "schema_attribute_node_type",
    [SXML_AST_KIND_PROCESSING_INSTRUCTION_NODE_TYPE] = "processing_instruction_node_type",
    [SXML_AST_KIND_COMMENT_NODE_TYPE] = "comment_node_type",
    [SXML_AST_KIND_TEXT_NODE_TYPE] = "text_node_type",
    [SXML_AST_KIND_NAMESPACE_NODE_TYPE] = "namespace_node_type",
    [SXML_AST_KIND_ANY_XNODE_TYPE] = "any_xnode_type",
    [SXML_AST_KIND_NAME_TEST_UNION] = "name_test_union",
    [SXML_AST_KIND_ANY_MAP_TYPE] = "any_map_type",
    [SXML_AST_KIND_ANY_ARRAY_TYPE] = "any_array_type",
    [SXML_AST_KIND_ANY_RECORD_TYPE] = "any_record_type",
    [SXML_AST_KIND_TYPED_MAP_TYPE] = "typed_map_type",
    [SXML_AST_KIND_TYPED_ARRAY_TYPE] = "typed_array_type",
    [SXML_AST_KIND_TYPED_RECORD_TYPE] = "typed_record_type",
    [SXML_AST_KIND_FIELD_DECLARATION] = "field_declaration",
    [SXML_AST_KIND_ENUMERATION_TYPE] = "enumeration_type",
    [SXML_AST_KIND_ANY_FUNCTION_TYPE] = "any_function_type",
    [SXML_AST_KIND_TYPED_FUNCTION_TYPE] = "typed_function_type",
    [SXML_AST_KIND_TYPED_FUNCTION_PARAMETER] = "typed_function_parameter",
    [SXML_AST_KIND_CHOICE_ITEM_TYPE] = "choice_item_type",
    [SXML_AST_KIND_NCNAME] = "ncname",
    [SXML_AST_KIND_PREFIXED_NAME] = "prefixed_name",
    [SXML_AST_KIND_URI_QUALIFIED_NAME] = "uri_qualified_name",
    [SXML_AST_KIND_MODULE] = "module",
    [SXML_AST_KIND_VERSION_DECLARATION] = "version_declaration",
    [SXML_AST_KIND_MAIN_MODULE] = "main_module",
    [SXML_AST_KIND_LIBRARY_MODULE] = "library_module",
    [SXML_AST_KIND_MODULE_DECLARATION] = "module_declaration",
    [SXML_AST_KIND_PROLOG] = "prolog",
    [SXML_AST_KIND_DEFAULT_NAMESPACE_DECLARATION] = "default_namespace_declaration",
    [SXML_AST_KIND_BOUNDARY_SPACE_DECLARATION] = "boundary_space_declaration",
    [SXML_AST_KIND_DEFAULT_COLLATION_DECLARATION] = "default_collation_declaration",
    [SXML_AST_KIND_BASE_URI_DECLARATION] = "base_uri_declaration",
    [SXML_AST_KIND_CONSTRUCTION_DECLARATION] = "construction_declaration",
    [SXML_AST_KIND_ORDERING_MODE_DECLARATION] = "ordering_mode_declaration",
    [SXML_AST_KIND_EMPTY_ORDER_DECLARATION] = "empty_order_declaration",
    [SXML_AST_KIND_COPY_NAMESPACES_DECLARATION] = "copy_namespaces_declaration",
    [SXML_AST_KIND_DECIMAL_FORMAT_DECLARATION] = "decimal_format_declaration",
    [SXML_AST_KIND_DECIMAL_FORMAT_PROPERTY] = "decimal_format_property",
    [SXML_AST_KIND_SCHEMA_IMPORT] = "schema_import",
    [SXML_AST_KIND_SCHEMA_PREFIX] = "schema_prefix",
    [SXML_AST_KIND_MODULE_IMPORT] = "module_import",
    [SXML_AST_KIND_CONTEXT_VALUE_DECLARATION] = "context_value_declaration",
    [SXML_AST_KIND_VARIABLE_DECLARATION] = "variable_declaration",
    [SXML_AST_KIND_FUNCTION_DECLARATION] = "function_declaration",
    [SXML_AST_KIND_PARAMETER_WITH_DEFAULT] = "parameter_with_default",
    [SXML_AST_KIND_ITEM_TYPE_DECLARATION] = "item_type_declaration",
    [SXML_AST_KIND_NOMINATIVE_RECORD_TYPE_DECLARATION] = "nominative_record_type_declaration",
    [SXML_AST_KIND_EXTENDED_FIELD_DECLARATION] = "extended_field_declaration",
    [SXML_AST_KIND_OPTION_DECLARATION] = "option_declaration",
    [SXML_AST_KIND_ANNOTATION] = "annotation",
    [SXML_AST_KIND_TUMBLING_WINDOW_CLAUSE] = "tumbling_window_clause",
    [SXML_AST_KIND_SLIDING_WINDOW_CLAUSE] = "sliding_window_clause",
    [SXML_AST_KIND_WINDOW_START_CONDITION] = "window_start_condition",
    [SXML_AST_KIND_WINDOW_END_CONDITION] = "window_end_condition",
    [SXML_AST_KIND_WHERE_CLAUSE] = "where_clause",
    [SXML_AST_KIND_WHILE_CLAUSE] = "while_clause",
    [SXML_AST_KIND_COUNT_CLAUSE] = "count_clause",
    [SXML_AST_KIND_GROUP_BY_CLAUSE] = "group_by_clause",
    [SXML_AST_KIND_GROUPING_SPECIFICATION] = "grouping_specification",
    [SXML_AST_KIND_ORDER_BY_CLAUSE] = "order_by_clause",
    [SXML_AST_KIND_ORDER_SPECIFICATION] = "order_specification",
    [SXML_AST_KIND_TRACE_CLAUSE] = "trace_clause",
    [SXML_AST_KIND_SWITCH_EXPRESSION] = "switch_expression",
    [SXML_AST_KIND_SWITCH_CASE_CLAUSE] = "switch_case_clause",
    [SXML_AST_KIND_TYPESWITCH_EXPRESSION] = "typeswitch_expression",
    [SXML_AST_KIND_TYPESWITCH_CASE_CLAUSE] = "typeswitch_case_clause",
    [SXML_AST_KIND_TYPESWITCH_DEFAULT_CLAUSE] = "typeswitch_default_clause",
    [SXML_AST_KIND_TRY_CATCH_EXPRESSION] = "try_catch_expression",
    [SXML_AST_KIND_CATCH_CLAUSE] = "catch_clause",
    [SXML_AST_KIND_FINALLY_CLAUSE] = "finally_clause",
    [SXML_AST_KIND_VALIDATE_EXPRESSION] = "validate_expression",
    [SXML_AST_KIND_EXTENSION_EXPRESSION] = "extension_expression",
    [SXML_AST_KIND_PRAGMA] = "pragma",
    [SXML_AST_KIND_PRAGMA_CONTENTS] = "pragma_contents",
    [SXML_AST_KIND_ORDERED_EXPRESSION] = "ordered_expression",
    [SXML_AST_KIND_UNORDERED_EXPRESSION] = "unordered_expression",
    [SXML_AST_KIND_STRING_CONSTRUCTOR] = "string_constructor",
    [SXML_AST_KIND_STRING_CONSTRUCTOR_CHARACTERS] = "string_constructor_characters",
    [SXML_AST_KIND_STRING_INTERPOLATION] = "string_interpolation",
    [SXML_AST_KIND_DIRECT_ELEMENT_CONSTRUCTOR] = "direct_element_constructor",
    [SXML_AST_KIND_DIRECT_ATTRIBUTE] = "direct_attribute",
    [SXML_AST_KIND_DIRECT_ATTRIBUTE_VALUE] = "direct_attribute_value",
    [SXML_AST_KIND_ATTRIBUTE_VALUE_TEXT] = "attribute_value_text",
    [SXML_AST_KIND_DIRECT_ELEMENT_TEXT] = "direct_element_text",
    [SXML_AST_KIND_CDATA_SECTION] = "cdata_section",
    [SXML_AST_KIND_DIRECT_COMMENT_CONSTRUCTOR] = "direct_comment_constructor",
    [SXML_AST_KIND_DIRECT_PROCESSING_INSTRUCTION_CONSTRUCTOR] = "direct_processing_instruction_constructor",
    [SXML_AST_KIND_QNAME] = "qname",
};

static const char *const field_names[SXML_AST_FIELD_COUNT] = {
    [SXML_AST_FIELD_ALTERNATIVE] = "alternative",
    [SXML_AST_FIELD_ARGUMENT] = "argument",
    [SXML_AST_FIELD_ARGUMENTS] = "arguments",
    [SXML_AST_FIELD_ARITY] = "arity",
    [SXML_AST_FIELD_BINDING] = "binding",
    [SXML_AST_FIELD_BODY] = "body",
    [SXML_AST_FIELD_CLAUSE] = "clause",
    [SXML_AST_FIELD_CONDITION] = "condition",
    [SXML_AST_FIELD_CONTENT] = "content",
    [SXML_AST_FIELD_DECLARATION] = "declaration",
    [SXML_AST_FIELD_ELSE] = "else",
    [SXML_AST_FIELD_ENTRY] = "entry",
    [SXML_AST_FIELD_EXPRESSION] = "expression",
    [SXML_AST_FIELD_FIELD] = "field",
    [SXML_AST_FIELD_FUNCTION] = "function",
    [SXML_AST_FIELD_ITEM] = "item",
    [SXML_AST_FIELD_ITEM_TYPE] = "item_type",
    [SXML_AST_FIELD_KEY] = "key",
    [SXML_AST_FIELD_LEFT] = "left",
    [SXML_AST_FIELD_LOOKUP] = "lookup",
    [SXML_AST_FIELD_MEMBER] = "member",
    [SXML_AST_FIELD_NAME] = "name",
    [SXML_AST_FIELD_NAME_EXPRESSION] = "name_expression",
    [SXML_AST_FIELD_OPERAND] = "operand",
    [SXML_AST_FIELD_PARAMETER] = "parameter",
    [SXML_AST_FIELD_PART] = "part",
    [SXML_AST_FIELD_PATH] = "path",
    [SXML_AST_FIELD_POSITION] = "position",
    [SXML_AST_FIELD_PREDICATE] = "predicate",
    [SXML_AST_FIELD_PREFIX] = "prefix",
    [SXML_AST_FIELD_RETURN] = "return",
    [SXML_AST_FIELD_RETURN_TYPE] = "return_type",
    [SXML_AST_FIELD_RIGHT] = "right",
    [SXML_AST_FIELD_SELECTOR] = "selector",
    [SXML_AST_FIELD_SEQUENCE] = "sequence",
    [SXML_AST_FIELD_TARGET] = "target",
    [SXML_AST_FIELD_TEST] = "test",
    [SXML_AST_FIELD_THEN] = "then",
    [SXML_AST_FIELD_TYPE] = "type",
    [SXML_AST_FIELD_URI] = "uri",
    [SXML_AST_FIELD_VALUE] = "value",
    [SXML_AST_FIELD_VARIABLE] = "variable",
    [SXML_AST_FIELD_ANNOTATION] = "annotation",
    [SXML_AST_FIELD_ATTRIBUTE] = "attribute",
    [SXML_AST_FIELD_CASE] = "case",
    [SXML_AST_FIELD_CATCH] = "catch",
    [SXML_AST_FIELD_COLLATION] = "collation",
    [SXML_AST_FIELD_COMPARAND] = "comparand",
    [SXML_AST_FIELD_CONTENTS] = "contents",
    [SXML_AST_FIELD_CURRENT] = "current",
    [SXML_AST_FIELD_DEFAULT] = "default",
    [SXML_AST_FIELD_ENCODING] = "encoding",
    [SXML_AST_FIELD_END] = "end",
    [SXML_AST_FIELD_END_NAME] = "end_name",
    [SXML_AST_FIELD_ERRORS] = "errors",
    [SXML_AST_FIELD_FINALLY] = "finally",
    [SXML_AST_FIELD_LOCATION] = "location",
    [SXML_AST_FIELD_MODULE] = "module",
    [SXML_AST_FIELD_NEXT] = "next",
    [SXML_AST_FIELD_PRAGMA] = "pragma",
    [SXML_AST_FIELD_PREVIOUS] = "previous",
    [SXML_AST_FIELD_PROLOG] = "prolog",
    [SXML_AST_FIELD_PROPERTY] = "property",
    [SXML_AST_FIELD_SPECIFICATION] = "specification",
    [SXML_AST_FIELD_START] = "start",
    [SXML_AST_FIELD_VERSION] = "version",
};

/* Keyword details in sxml_ast_keyword order. */
static const char *const keyword_names[] = {
    "",        "preserve", "strip",    "ordered", "unordered",
    "greatest", "least",   "element",  "function", "value",
    "item",    "lax",      "strict",
};

/* Decimal format properties in sxml_decimal_format_property_kind order. */
static const char *const property_names[] = {
    "decimal-separator", "grouping-separator", "exponent-separator",
    "percent",           "per-mille",          "zero-digit",
    "digit",             "pattern-separator",  "infinity",
    "NaN",               "minus-sign",
};

/* Flag names in bit order of sxml_ast_flag. */
static const char *const flag_names[] = {
    "abbreviated",
    "optional",
    "nillable",
    "braced_uri",
    "true",
    "has_signature",
    "allowing_empty",
    "fixed",
    "external",
    "stable",
    "descending",
    "empty_greatest",
    "empty_least",
    "only",
    "static",
    "preserve_namespaces",
    "inherit_namespaces",
    "boundary_whitespace",
    "namespace_declaration_attribute",
};

/* Text written so far, up to the capacity of bytes. */
typedef struct written_text {
    char bytes[4096];
    size_t byte_count;
    bool is_truncated;
} written_text;

static void write_bytes(written_text *output, sxml_byte_view bytes) {
    for (size_t byte_index = 0; byte_index < bytes.byte_count;
         byte_index += 1) {
        if (output->byte_count + 1 >= sizeof(output->bytes)) {
            output->is_truncated = true;
            return;
        }
        output->bytes[output->byte_count] = (char)bytes.bytes[byte_index];
        output->byte_count += 1;
    }
    output->bytes[output->byte_count] = '\0';
}

static void write_text(written_text *output, const char *text) {
    write_bytes(output, sxml_byte_view_from_c_string(text));
}

typedef struct ast_writer {
    const sxml_ast *ast;
    written_text *output;
} ast_writer;

static void write_name(ast_writer writer, sxml_ast_name name) {
    sxml_byte_view uri = sxml_ast_get_text(writer.ast, name.namespace_uri);
    sxml_byte_view prefix =
        sxml_ast_get_text(writer.ast, name.namespace_prefix);
    write_text(writer.output, " ");
    if (uri.byte_count != 0) {
        write_text(writer.output, "{");
        write_bytes(writer.output, uri);
        write_text(writer.output, "}");
    }
    if (prefix.byte_count != 0) {
        write_bytes(writer.output, prefix);
        write_text(writer.output, ":");
    }
    write_bytes(writer.output, sxml_ast_get_text(writer.ast, name.local_name));
}

static void write_quoted(ast_writer writer, sxml_ast_text text) {
    write_text(writer.output, " \"");
    write_bytes(writer.output, sxml_ast_get_text(writer.ast, text));
    write_text(writer.output, "\"");
}

static void write_detail(ast_writer writer, const sxml_ast_node *node) {
    written_text *output = writer.output;
    if (node->kind == SXML_AST_KIND_DECIMAL_FORMAT_PROPERTY) {
        write_text(output, " ");
        write_text(output, property_names[node->detail.decimal_format_property]);
    } else if (node->kind >= SXML_AST_KIND_MODULE &&
               node->detail.keyword != SXML_AST_KEYWORD_NONE) {
        write_text(output, " ");
        write_text(output, keyword_names[node->detail.keyword]);
    }
    for (uint32_t flag_index = 0; flag_index < SXML_TEST_COUNT(flag_names);
         flag_index += 1) {
        if ((node->flags & (1U << flag_index)) != 0) {
            write_text(output, " ");
            write_text(output, flag_names[flag_index]);
        }
    }
}

static void write_payload(ast_writer writer, const sxml_ast_node *node) {
    const sxml_ast_kind kind = node->kind;
    if (kind == SXML_AST_KIND_NCNAME || kind == SXML_AST_KIND_PREFIXED_NAME ||
        kind == SXML_AST_KIND_URI_QUALIFIED_NAME ||
        kind == SXML_AST_KIND_WILDCARD || kind == SXML_AST_KIND_QNAME) {
        write_name(writer, node->payload.name);
    } else if (kind ==
               SXML_AST_KIND_DIRECT_PROCESSING_INSTRUCTION_CONSTRUCTOR) {
        write_text(writer.output, " ");
        write_bytes(writer.output,
                    sxml_ast_get_text(
                        writer.ast, node->payload.processing_instruction.target));
        write_quoted(writer, node->payload.processing_instruction.content);
    } else if ((kind >= SXML_AST_KIND_INTEGER_LITERAL &&
                kind <= SXML_AST_KIND_STRING_LITERAL) ||
               kind == SXML_AST_KIND_STRING_TEMPLATE_FIXED_PART ||
               kind == SXML_AST_KIND_PRAGMA_CONTENTS ||
               kind == SXML_AST_KIND_STRING_CONSTRUCTOR_CHARACTERS ||
               kind == SXML_AST_KIND_ATTRIBUTE_VALUE_TEXT ||
               kind == SXML_AST_KIND_DIRECT_ELEMENT_TEXT ||
               kind == SXML_AST_KIND_CDATA_SECTION ||
               kind == SXML_AST_KIND_DIRECT_COMMENT_CONSTRUCTOR) {
        write_quoted(writer, node->payload.text);
    }
}

/* Writes the opening of node: its kind, detail and payload. */
static void write_node_start(ast_writer writer, const sxml_ast_node *node) {
    write_text(writer.output, "(");
    const char *kind_name =
        node->kind < SXML_AST_KIND_COUNT ? kind_names[node->kind] : NULL;
    write_text(writer.output, kind_name == NULL ? "?" : kind_name);
    write_detail(writer, node);
    write_payload(writer, node);
}

/* Writes " field=" for a child about to be written. */
static void write_field_label(ast_writer writer, const sxml_ast_node *child) {
    write_text(writer.output, " ");
    const char *field_name =
        child->field < SXML_AST_FIELD_COUNT ? field_names[child->field] : NULL;
    write_text(writer.output, field_name == NULL ? "?" : field_name);
    write_text(writer.output, "=");
}

/* Writes the subtree of index in document order, following parent and
   sibling links instead of recursing (D71). */
static void write_node(ast_writer writer, sxml_ast_node_index index) {
    sxml_ast_node_index current = index;
    write_node_start(writer, sxml_ast_get_node(writer.ast, current));
    for (;;) {
        const sxml_ast_node *node = sxml_ast_get_node(writer.ast, current);
        if (node->first_child.value != 0) {
            current = node->first_child;
            const sxml_ast_node *child = sxml_ast_get_node(writer.ast, current);
            write_field_label(writer, child);
            write_node_start(writer, child);
            continue;
        }
        /* Close finished nodes up to one with a next sibling. */
        for (;;) {
            write_text(writer.output, ")");
            if (current.value == index.value) {
                return;
            }
            node = sxml_ast_get_node(writer.ast, current);
            if (node->next_sibling.value != 0) {
                break;
            }
            current = node->parent;
        }
        current = node->next_sibling;
        const sxml_ast_node *sibling = sxml_ast_get_node(writer.ast, current);
        write_field_label(writer, sibling);
        write_node_start(writer, sibling);
    }
}

/* -------------------------------------------------------------- Helpers */

typedef struct parse_outcome {
    sxml_status status;
    sxml_ast *ast;
    sxml_error_record error_record;
} parse_outcome;

typedef struct parse_request {
    sxml_byte_view text;
    uint32_t maximum_expression_depth;
    sxml_allocator allocator;
} parse_request;

static parse_outcome parse_with(parse_request request) {
    parse_outcome outcome = {.status = SXML_STATUS_OK, .ast = NULL};
    memset(&outcome.error_record, 0, sizeof(outcome.error_record));
    const sxml_ast_build_request build_request = {
        .expression_text = request.text,
        .source_name = SXML_BYTE_VIEW_FROM_LITERAL("query.xq"),
        .allocator = request.allocator,
        .maximum_expression_depth = request.maximum_expression_depth,
        .error_record = &outcome.error_record,
    };
    outcome.status = sxml_xquery_build_ast(&build_request, &outcome.ast);
    return outcome;
}

static parse_outcome parse_query(const char *text) {
    return parse_with(
        (parse_request){.text = sxml_byte_view_from_c_string(text),
                        .maximum_expression_depth = 0,
                        .allocator = sxml_default_allocator()});
}

/* A query and the written form of its AST: the body of a main module
   without a prolog, else the whole module. */
typedef struct expected_ast {
    const char *text;
    const char *written;
} expected_ast;

static void write_outcome(const parse_outcome *outcome, written_text *output) {
    output->byte_count = 0;
    output->is_truncated = false;
    output->bytes[0] = '\0';
    const sxml_ast *ast = outcome->ast;
    sxml_ast_node_index top = ast->root;
    const sxml_ast_node_index module = sxml_ast_find_child(
        ast, (sxml_ast_child_query){.parent = top,
                                    .field = SXML_AST_FIELD_MODULE});
    const sxml_ast_node *module_node = sxml_ast_get_node(ast, module);
    if (sxml_ast_get_node(ast, top)->first_child.value == module.value &&
        module_node->kind == SXML_AST_KIND_MAIN_MODULE &&
        module_node->first_child.value == module_node->last_child.value) {
        top = module_node->first_child;
    }
    write_node((ast_writer){.ast = ast, .output = output}, top);
}

typedef struct expectation_list {
    const expected_ast *cases;
    size_t case_count;
} expectation_list;

static void expect_asts(sxml_test_state *state, expectation_list list) {
    for (size_t case_index = 0; case_index < list.case_count; case_index += 1) {
        const expected_ast *expected = &list.cases[case_index];
        parse_outcome outcome = parse_query(expected->text);
        SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
        if (outcome.status != SXML_STATUS_OK) {
            printf("  text: %s\n  error: %.*s\n", expected->text,
                   (int)outcome.error_record.code.local_name.byte_count,
                   (const char *)outcome.error_record.code.local_name.bytes);
            continue;
        }
        written_text written = {
            .bytes = {0}, .byte_count = 0, .is_truncated = false};
        write_outcome(&outcome, &written);
        const bool matches = strcmp(written.bytes, expected->written) == 0;
        SXML_TEST_EXPECT(state, matches);
        if (!matches) {
            printf("  text:     %s\n  expected: %s\n  written:  %s\n",
                   expected->text, expected->written, written.bytes);
        }
        sxml_ast_destroy(outcome.ast);
    }
}

#define EXPECT_ASTS(state, expected_cases)                                     \
    expect_asts((state),                                                       \
                (expectation_list){.cases = (expected_cases),                  \
                                   .case_count =                               \
                                       SXML_TEST_COUNT(expected_cases)})

/* ------------------------------------------------------------- Modules */

static const expected_ast module_cases[] = {
    {"1",
     "(integer_literal \"1\")"},
    {"xquery version \"3.1\"; 1",
     "(module version=(version_declaration version=(string_literal \"3.1\")) "
     "module=(main_module body=(integer_literal \"1\")))"},
    {"xquery version '4.0' encoding 'UTF-8'; declare variable $x := 1; $x",
     "(module version=(version_declaration version=(string_literal \"4.0\") "
     "encoding=(string_literal \"UTF-8\")) module=(main_module "
     "prolog=(prolog declaration=(variable_declaration "
     "variable=(variable_name_and_type name=(ncname x)) "
     "value=(integer_literal \"1\"))) body=(variable_reference "
     "name=(ncname x))))"},
    {"module namespace m = 'urn:m'; declare function m:f() { 1 };",
     "(module module=(library_module declaration=(module_declaration "
     "prefix=(ncname m) uri=(string_literal \"urn:m\")) "
     "prolog=(prolog declaration=(function_declaration name=(prefixed_name "
     "m:f) body=(enclosed_expression expression=(integer_literal \"1\"))))))"},
};

static void test_modules_and_version_declarations(sxml_test_state *state) {
    EXPECT_ASTS(state, module_cases);
}

/* ------------------------------------------------------------- Prolog */

static const expected_ast prolog_cases[] = {
    {"declare boundary-space preserve; declare construction strip; "
     "declare ordering unordered; declare default order empty least; "
     "declare copy-namespaces preserve, no-inherit; 1",
     "(module module=(main_module prolog=(prolog "
     "declaration=(boundary_space_declaration preserve) "
     "declaration=(construction_declaration strip) "
     "declaration=(ordering_mode_declaration unordered) "
     "declaration=(empty_order_declaration least) "
     "declaration=(copy_namespaces_declaration preserve_namespaces)) "
     "body=(integer_literal \"1\")))"},
    {"declare copy-namespaces no-preserve, inherit; "
     "declare fixed default element namespace 'urn:e'; "
     "declare default function namespace 'urn:f'; 1",
     "(module module=(main_module prolog=(prolog "
     "declaration=(copy_namespaces_declaration inherit_namespaces) "
     "declaration=(default_namespace_declaration element fixed "
     "uri=(string_literal \"urn:e\")) "
     "declaration=(default_namespace_declaration function "
     "uri=(string_literal \"urn:f\"))) body=(integer_literal \"1\")))"},
    {"declare decimal-format d grouping-separator = '.' NaN = 'x'; "
     "declare default decimal-format minus-sign = '~'; 1",
     "(module module=(main_module prolog=(prolog "
     "declaration=(decimal_format_declaration name=(ncname d) "
     "property=(decimal_format_property grouping-separator "
     "value=(string_literal \".\")) property=(decimal_format_property NaN "
     "value=(string_literal \"x\"))) "
     "declaration=(decimal_format_declaration "
     "property=(decimal_format_property minus-sign "
     "value=(string_literal \"~\")))) body=(integer_literal \"1\")))"},
    {"declare context item as xs:integer external := 2; "
     "declare %private variable $v external; "
     "declare function f($a := 1) external; ."
     ,
     "(module module=(main_module prolog=(prolog "
     "declaration=(context_value_declaration item external "
     "type=(prefixed_name xs:integer) default=(integer_literal \"2\")) "
     "declaration=(variable_declaration external "
     "annotation=(annotation name=(ncname private)) "
     "variable=(variable_name_and_type name=(ncname v))) "
     "declaration=(function_declaration external name=(ncname f) "
     "parameter=(parameter_with_default variable=(variable_name_and_type "
     "name=(ncname a)) default=(integer_literal \"1\")))) "
     "body=(context_value_reference)))"},
    {"declare context value := 1; declare %public record r(%static a "
     "as xs:int := 1, \"b c\"); declare option o 'v'; 1",
     "(module module=(main_module prolog=(prolog "
     "declaration=(context_value_declaration value value=(integer_literal "
     "\"1\")) declaration=(nominative_record_type_declaration "
     "annotation=(annotation name=(ncname public)) name=(ncname r) "
     "field=(extended_field_declaration static name=(ncname a) "
     "type=(sequence_type item_type=(prefixed_name xs:int)) "
     "default=(integer_literal \"1\")) field=(extended_field_declaration "
     "name=(string_literal \"b c\"))) declaration=(option_declaration "
     "name=(ncname o) value=(string_literal \"v\"))) "
     "body=(integer_literal \"1\")))"},
};

static void test_prolog_tokens_become_details_and_flags(
    sxml_test_state *state) {
    EXPECT_ASTS(state, prolog_cases);
}

/* ------------------------------------------------------- Expressions */

static const expected_ast expression_cases[] = {
    {"for $x allowing empty at $i in () return $i",
     "(flwor_expression clause=(for_clause binding=(for_item_binding "
     "allowing_empty variable=(variable_name_and_type name=(ncname x)) "
     "position=(positional_variable name=(ncname i)) "
     "sequence=(parenthesized_expression))) return=(return_clause "
     "value=(variable_reference name=(ncname i))))"},
    {"for sliding window $w in 1 start $s when 1 only end $e when 2 "
     "return $w",
     "(flwor_expression clause=(sliding_window_clause "
     "variable=(variable_name_and_type name=(ncname w)) "
     "sequence=(integer_literal \"1\") start=(window_start_condition "
     "current=(ncname s) condition=(integer_literal \"1\")) "
     "end=(window_end_condition only current=(ncname e) "
     "condition=(integer_literal \"2\"))) return=(return_clause "
     "value=(variable_reference name=(ncname w))))"},
    {"for $x in 1 stable order by $x descending empty greatest collation "
     "'c', $x ascending empty least count $n where 1 while 2 trace 3 "
     "return 4",
     "(flwor_expression clause=(for_clause binding=(for_item_binding "
     "variable=(variable_name_and_type name=(ncname x)) "
     "sequence=(integer_literal \"1\"))) clause=(order_by_clause stable "
     "specification=(order_specification descending empty_greatest "
     "key=(variable_reference name=(ncname x)) collation=(string_literal "
     "\"c\")) specification=(order_specification empty_least "
     "key=(variable_reference name=(ncname x)))) clause=(count_clause "
     "name=(ncname n)) clause=(where_clause condition=(integer_literal "
     "\"1\")) clause=(while_clause condition=(integer_literal \"2\")) "
     "clause=(trace_clause value=(integer_literal \"3\")) "
     "return=(return_clause value=(integer_literal \"4\")))"},
    {"validate lax { 1 }, validate { 2 }, (# p:q  a  b #) (# r #) { 3 }",
     "(sequence_expression item=(validate_expression lax "
     "expression=(integer_literal \"1\")) item=(validate_expression "
     "expression=(integer_literal \"2\")) item=(extension_expression "
     "pragma=(pragma name=(prefixed_name p:q) contents=(pragma_contents "
     "\"a  b \")) pragma=(pragma name=(ncname r) contents=(pragma_contents "
     "\"\")) "
     "expression=(integer_literal \"3\")))"},
    {"``[a `{1}` ]` b]``",
     "(string_constructor part=(string_constructor_characters \"a \") "
     "part=(string_interpolation expression=(integer_literal \"1\")) "
     "part=(string_constructor_characters \" ]` b\"))"},
    {"try { 1 } catch * { 2 } finally { 3 }",
     "(try_catch_expression body=(enclosed_expression "
     "expression=(integer_literal \"1\")) catch=(catch_clause "
     "errors=(name_test_union test=(wildcard )) body=(enclosed_expression "
     "expression=(integer_literal \"2\"))) finally=(finally_clause "
     "body=(enclosed_expression expression=(integer_literal \"3\"))))"},
};

static void test_xquery_expressions(sxml_test_state *state) {
    EXPECT_ASTS(state, expression_cases);
}

/* ---------------------------------------------------------- Decoding */

static const expected_ast decoding_cases[] = {
    /* 4.3.1.2: references and doubled quotes in string literals. */
    {"'a&lt;&#x41;&#66;&amp;''b' || \"&quot;\"\"\"",
     "(string_concatenation_expression left=(string_literal "
     "\"a<AB&'b\") right=(string_literal \"\"\"\"))"},
    {"'&#x10FFFF;&#xE9;'",
     "(string_literal \"\xF4\x8F\xBF\xBF\xC3\xA9\")"},
    /* A.3.3.1: CR LF and a lone CR become LF. */
    {"'a\r\nb\rc'", "(string_literal \"a\nb\nc\")"},
    /* BracedURILiteral allows references in XQuery. */
    {"Q{urn:&amp;}a, Q{urn:&#x61;}*",
     "(sequence_expression item=(axis_step test=(uri_qualified_name "
     "{urn:&}a)) item=(axis_step test=(wildcard braced_uri {urn:a})))"},
    {"`a{{b}}``c{1}`",
     "(string_template part=(string_template_fixed_part \"a{b}`c\") "
     "part=(enclosed_expression expression=(integer_literal \"1\")))"},
};

static void test_literals_are_decoded(sxml_test_state *state) {
    EXPECT_ASTS(state, decoding_cases);
}

/* ------------------------------------------------- Direct constructors */

static const expected_ast constructor_cases[] = {
    {"<p:a xmlns:p=\"urn:p\" xmlns='urn:d' b=\"{1}\">x<c/></p:a >",
     "(direct_element_constructor name=(qname p:a) "
     "attribute=(direct_attribute namespace_declaration_attribute "
     "name=(qname xmlns:p) value=(direct_attribute_value "
     "part=(attribute_value_text \"urn:p\"))) attribute=(direct_attribute "
     "namespace_declaration_attribute name=(qname xmlns) "
     "value=(direct_attribute_value part=(attribute_value_text \"urn:d\"))) "
     "attribute=(direct_attribute name=(qname b) "
     "value=(direct_attribute_value part=(enclosed_expression "
     "expression=(integer_literal \"1\")))) content=(direct_element_text "
     "\"x\") content=(direct_element_constructor name=(qname c)) "
     "end_name=(qname p:a))"},
    /* 4.12.1.1: doubled delimiters, braces, references, then literal
       whitespace to spaces (a reference's character stays). */
    {"<a b=\"x\"\"'{{}}&lt;&#9;\t\r\n.\" c='\"''' d=\"\"/>",
     "(direct_element_constructor name=(qname a) "
     "attribute=(direct_attribute name=(qname b) "
     "value=(direct_attribute_value part=(attribute_value_text "
     "\"x\"'{}<\t  .\"))) attribute=(direct_attribute name=(qname c) "
     "value=(direct_attribute_value part=(attribute_value_text "
     "\"\"'\"))) attribute=(direct_attribute name=(qname d) "
     "value=(direct_attribute_value)))"},
    /* 4.12.1.3 and 4.12.1.4: element text and boundary whitespace. */
    {"<a> <b/> {1} x {{&#x20;}} <![CDATA[ ]]> <!-- c --><?t  d ?>\r\n</a>",
     "(direct_element_constructor name=(qname a) "
     "content=(direct_element_text boundary_whitespace \" \") "
     "content=(direct_element_constructor name=(qname b)) "
     "content=(direct_element_text boundary_whitespace \" \") "
     "content=(enclosed_expression expression=(integer_literal \"1\")) "
     "content=(direct_element_text \" x { } \") content=(cdata_section "
     "\" \") content=(direct_element_text \" \") "
     "content=(direct_comment_constructor \" c \") "
     "content=(direct_processing_instruction_constructor t \"d \") "
     "content=(direct_element_text boundary_whitespace \"\n\") "
     "end_name=(qname a))"},
    {"<a>&#x20;</a>, <a>{()} </a>",
     "(sequence_expression item=(direct_element_constructor name=(qname a) "
     "content=(direct_element_text \" \") end_name=(qname a)) "
     "item=(direct_element_constructor name=(qname a) "
     "content=(enclosed_expression expression=(parenthesized_expression)) "
     "content=(direct_element_text "
     "boundary_whitespace \" \") end_name=(qname a)))"},
    {"<!---->, <?pi?>",
     "(sequence_expression item=(direct_comment_constructor \"\") "
     "item=(direct_processing_instruction_constructor pi \"\"))"},
};

static void test_direct_constructors(sxml_test_state *state) {
    EXPECT_ASTS(state, constructor_cases);
}

/* -------------------------------------------------------------- Errors */

typedef struct expected_error {
    const char *text;
    const char *code;
    uint32_t line_number;
    uint32_t column_number;
} expected_error;

static void expect_errors(sxml_test_state *state,
                          const expected_error *errors, size_t error_count) {
    for (size_t error_index = 0; error_index < error_count;
         error_index += 1) {
        const expected_error *expected = &errors[error_index];
        parse_outcome outcome = parse_query(expected->text);
        SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_STATIC_ERROR);
        SXML_TEST_EXPECT(state, outcome.ast == NULL);
        const sxml_error_record *record = &outcome.error_record;
        const bool code_matches =
            sxml_byte_view_equals(record->code.local_name,
                                  sxml_byte_view_from_c_string(expected->code));
        SXML_TEST_EXPECT(state, code_matches);
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                    record->code.namespace_uri,
                                    SXML_BYTE_VIEW_FROM_LITERAL(
                                        "http://www.w3.org/2005/xqt-errors")));
        SXML_TEST_EXPECT(state, record->diagnostic.description.byte_count != 0);
        const bool location_matches =
            record->diagnostic.location.line_number == expected->line_number &&
            record->diagnostic.location.column_number ==
                expected->column_number;
        SXML_TEST_EXPECT(state, location_matches);
        if (!code_matches || !location_matches) {
            printf("  text: %s\n  error: %.*s at %u:%u\n", expected->text,
                   (int)record->code.local_name.byte_count,
                   (const char *)record->code.local_name.bytes,
                   (unsigned)record->diagnostic.location.line_number,
                   (unsigned)record->diagnostic.location.column_number);
        }
        sxml_ast_destroy(outcome.ast);
    }
}

static void test_static_errors_have_codes_and_locations(
    sxml_test_state *state) {
    static const expected_error errors[] = {
        {"1 +", "XPST0003", 1, 3},
        {"<a></b>", "XQST0118", 1, 6},
        {"<p:a></a>", "XQST0118", 1, 8},
        {"<a b='1' c='2'\n b='3'/>", "XQST0040", 2, 2},
        {"<a xmlns:p='u' xmlns:p='v'/>", "XQST0071", 1, 16},
        {"<a xmlns='u' xmlns='u'/>", "XQST0071", 1, 14},
        {"<a xmlns:p='{1}'/>", "XQST0022", 1, 13},
        {"<a xmlns:xml='urn:x'/>", "XQST0070", 1, 4},
        {"<a xmlns:p='http://www.w3.org/XML/1998/namespace'/>", "XQST0070", 1,
         4},
        {"<a xmlns='http://www.w3.org/XML/1998/namespace'/>", "XQST0070", 1,
         4},
        {"<a xmlns:xmlns='urn:x'/>", "XQST0070", 1, 4},
        {"<a xmlns:p='http://www.w3.org/2000/xmlns/'/>", "XQST0070", 1, 4},
        {"<a xmlns:p=''/>", "XQST0085", 1, 4},
        {"'&#0;'", "XQST0090", 1, 2},
        {"'a&#xD800;'", "XQST0090", 1, 3},
        {"'&#99999999999;'", "XQST0090", 1, 2},
        {"<a>\n &#xFFFE;</a>", "XQST0090", 2, 2},
        {"<a b='&#1;'/>", "XQST0090", 1, 7},
        {"xquery version '2.0'; 1", "XQST0031", 1, 16},
        {"xquery version '3.0' encoding '8bit'; 1", "XQST0087", 1, 31},
        {"<!-- a --->", "XPST0003", 1, 1},
        {"declare %constructor record r('a'); 1", "XPST0003", 1, 31},
        {"declare %Q{http://www.w3.org/2012/xquery}constructor record r(a, "
         "'b'); 1",
         "XPST0003", 1, 66},
        {"<a>}</a>", "XPST0003", 1, 1},
    };
    expect_errors(state, errors, SXML_TEST_COUNT(errors));
}

static void test_valid_namespace_declarations_are_accepted(
    sxml_test_state *state) {
    static const char *const texts[] = {
        "<a xmlns:xml='http://www.w3.org/XML/1998/namespace'/>",
        "<a xmlns=''/>",
        "<a xmlns:p='u' p:b='1' b='2' xmlns='v'/>",
        "<a b='1'><a b='1'/></a>",
        "declare record r('a'); 1",
        "xquery encoding 'ISO-8859-1'; 1",
        "xquery version '1.0'; 1",
    };
    for (size_t text_index = 0; text_index < SXML_TEST_COUNT(texts);
         text_index += 1) {
        parse_outcome outcome = parse_query(texts[text_index]);
        SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
        if (outcome.status != SXML_STATUS_OK) {
            printf("  text: %s\n", texts[text_index]);
        }
        sxml_ast_destroy(outcome.ast);
    }
}

/* ------------------------------------------------- Many attributes */

/* Writes "<a" and attribute_count attributes named n<number> into text,
   the last one repeating duplicate_of (none when 0), then "/>". */
typedef struct attribute_text {
    char bytes[8192];
} attribute_text;

typedef struct attribute_request {
    size_t attribute_count;
    size_t duplicate_of;
    bool is_namespace_declaration;
} attribute_request;

static void write_attributes(attribute_request request, attribute_text *text) {
    size_t byte_count = (size_t)snprintf(text->bytes, sizeof(text->bytes), "<a");
    for (size_t attribute_index = 1;
         attribute_index <= request.attribute_count; attribute_index += 1) {
        const size_t number =
            attribute_index == request.attribute_count &&
                    request.duplicate_of != 0
                ? request.duplicate_of
                : attribute_index;
        byte_count += (size_t)snprintf(
            text->bytes + byte_count, sizeof(text->bytes) - byte_count,
            request.is_namespace_declaration ? " xmlns:n%zu='u'" : " n%zu=''",
            number);
    }
    (void)snprintf(text->bytes + byte_count, sizeof(text->bytes) - byte_count,
                   "/>");
}

static void test_many_attributes_are_sorted_for_duplicates(
    sxml_test_state *state) {
    attribute_text text = {.bytes = {0}};
    write_attributes((attribute_request){.attribute_count = 100,
                                         .duplicate_of = 0,
                                         .is_namespace_declaration = false},
                     &text);
    parse_outcome outcome = parse_query(text.bytes);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
    sxml_ast_destroy(outcome.ast);

    write_attributes((attribute_request){.attribute_count = 100,
                                         .duplicate_of = 7,
                                         .is_namespace_declaration = false},
                     &text);
    outcome = parse_query(text.bytes);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                outcome.error_record.code.local_name,
                                SXML_BYTE_VIEW_FROM_LITERAL("XQST0040")));
    /* The repeated attribute, the last one, is reported. */
    const char *last = strrchr(text.bytes, ' ');
    SXML_TEST_EXPECT(state, outcome.error_record.diagnostic.location
                                    .byte_offset ==
                                (size_t)(last - text.bytes) + 1);

    write_attributes((attribute_request){.attribute_count = 50,
                                         .duplicate_of = 49,
                                         .is_namespace_declaration = true},
                     &text);
    outcome = parse_query(text.bytes);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                outcome.error_record.code.local_name,
                                SXML_BYTE_VIEW_FROM_LITERAL("XQST0071")));
}

/* ------------------------------------------------------- Limits, memory */

static void test_deep_constructors_need_no_recursion(sxml_test_state *state) {
    const size_t depth = 10000;
    const sxml_allocator allocator = sxml_default_allocator();
    void *memory = NULL;
    SXML_TEST_REQUIRE(state,
                      sxml_allocator_allocate(
                          allocator,
                          (sxml_allocation_request){.byte_count = 8 * depth,
                                                    .alignment = 1},
                          &memory) == SXML_STATUS_OK);
    uint8_t *text = memory;
    size_t byte_count = 0;
    const sxml_byte_view start_tag = SXML_BYTE_VIEW_FROM_LITERAL("<a>");
    const sxml_byte_view end_tag = SXML_BYTE_VIEW_FROM_LITERAL("</a>");
    for (size_t level = 0; level < 2 * depth; level += 1) {
        const sxml_byte_view tag = level < depth ? start_tag : end_tag;
        for (size_t byte_index = 0; byte_index < tag.byte_count;
             byte_index += 1) {
            text[byte_count] = tag.bytes[byte_index];
            byte_count += 1;
        }
    }
    parse_outcome outcome = parse_with(
        (parse_request){.text = {.bytes = memory, .byte_count = byte_count},
                        .maximum_expression_depth = 0,
                        .allocator = allocator});
    sxml_allocator_release(allocator, memory);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
    sxml_ast_destroy(outcome.ast);
}

static void test_depth_limit_is_enforced(sxml_test_state *state) {
    /* module > main_module > direct_element_constructor > qname: 4. */
    parse_outcome outcome =
        parse_with((parse_request){.text = SXML_BYTE_VIEW_FROM_LITERAL("<a/>"),
                                   .maximum_expression_depth = 3,
                                   .allocator = sxml_default_allocator()});
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, outcome.ast == NULL);
    outcome =
        parse_with((parse_request){.text = SXML_BYTE_VIEW_FROM_LITERAL("<a/>"),
                                   .maximum_expression_depth = 4,
                                   .allocator = sxml_default_allocator()});
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
    sxml_ast_destroy(outcome.ast);
}

/* A counting allocator over the default allocator, placed on the stack. */
typedef struct counting_fixture {
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t storage[128];
    sxml_counting_allocator *counting_allocator;
} counting_fixture;

static void
test_build_survives_every_allocation_failure(sxml_test_state *state) {
    /* 40 attributes: the duplicate check sorts them in a work array. */
    attribute_text text = {.bytes = {0}};
    write_attributes((attribute_request){.attribute_count = 40,
                                         .duplicate_of = 0,
                                         .is_namespace_declaration = false},
                     &text);
    uint64_t failing_allocation_number = 1;
    for (;;) {
        counting_fixture fixture = {.storage = {0}, .counting_allocator = NULL};
        SXML_TEST_REQUIRE(
            state,
            sxml_counting_allocator_initialize(
                (sxml_memory_view){.bytes = fixture.storage,
                                   .byte_count = sizeof(fixture.storage)},
                &(sxml_counting_allocator_configuration){
                    .backing_allocator = sxml_default_allocator(),
                    .failing_allocation_number = failing_allocation_number,
                },
                &fixture.counting_allocator) == SXML_STATUS_OK);
        parse_outcome outcome = parse_with((parse_request){
            .text = sxml_byte_view_from_c_string(text.bytes),
            .maximum_expression_depth = 0,
            .allocator = sxml_counting_allocator_as_allocator(
                fixture.counting_allocator)});
        sxml_ast_destroy(outcome.ast);
        sxml_allocation_counts counts =
            sxml_counting_allocator_read_counts(fixture.counting_allocator);
        sxml_counting_allocator_finalize(fixture.counting_allocator);
        SXML_TEST_EXPECT(state, counts.live_allocation_count == 0);
        if (outcome.status == SXML_STATUS_OK) {
            break;
        }
        SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OUT_OF_MEMORY);
        SXML_TEST_EXPECT(state, outcome.ast == NULL);
        failing_allocation_number += 1;
        SXML_TEST_REQUIRE(state, failing_allocation_number < 10);
    }
    /* The AST and the work array. */
    SXML_TEST_EXPECT(state, failing_allocation_number == 3);
}

static const sxml_test_case xquery_parse_test_cases[] = {
    {.name = "modules_and_version_declarations",
     .function = test_modules_and_version_declarations},
    {.name = "prolog_tokens_become_details_and_flags",
     .function = test_prolog_tokens_become_details_and_flags},
    {.name = "xquery_expressions", .function = test_xquery_expressions},
    {.name = "literals_are_decoded", .function = test_literals_are_decoded},
    {.name = "direct_constructors", .function = test_direct_constructors},
    {.name = "static_errors_have_codes_and_locations",
     .function = test_static_errors_have_codes_and_locations},
    {.name = "valid_namespace_declarations_are_accepted",
     .function = test_valid_namespace_declarations_are_accepted},
    {.name = "many_attributes_are_sorted_for_duplicates",
     .function = test_many_attributes_are_sorted_for_duplicates},
    {.name = "deep_constructors_need_no_recursion",
     .function = test_deep_constructors_need_no_recursion},
    {.name = "depth_limit_is_enforced",
     .function = test_depth_limit_is_enforced},
    {.name = "build_survives_every_allocation_failure",
     .function = test_build_survives_every_allocation_failure},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xquery_parse",
            .cases = xquery_parse_test_cases,
            .case_count = SXML_TEST_COUNT(xquery_parse_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
