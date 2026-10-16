/*
 * Tests for the XPath 4.0 front end (src/xpath_parse.c): text to AST
 * through grammars/xpath, the checks the grammar leaves to the front end
 * (docs/DECISIONS.md, D29) and the decoding of literals and names (D52).
 *
 * Golden file: tests/memory_test.c. Each AST is checked against a compact
 * written form, made by write_ast below:
 *
 *     (kind detail flags "payload" field=(child ...) ...)
 *
 * where detail is an operator token, an axis, an occurrence indicator or a
 * name test kind, flags are written by name, and the payload is the decoded
 * text or the name as {uri}prefix:local with empty parts left out.
 */
#include "test_runner.h"
#include "xpath_ast_internal.h"

#include <stdalign.h>
#include <stdio.h>
#include <string.h>

/* Caller memory for objects placed with initialize; 16 suits every type. */
#define TEST_STORAGE_ALIGNMENT 16

/* ------------------------------------------------------ Writing an AST */

/* Kind names in sxml_ast_kind order, as the grammar spells them. */
static const char *const kind_names[] = {
    "none",
    "xpath",
    "default_element_namespace_declaration",
    "namespace_declaration",
    "sequence_expression",
    "parenthesized_expression",
    "enclosed_expression",
    "context_value_reference",
    "variable_reference",
    "integer_literal",
    "hex_integer_literal",
    "binary_integer_literal",
    "decimal_literal",
    "double_literal",
    "string_literal",
    "qname_literal",
    "negative_numeric_literal",
    "boolean_constant",
    "string_template",
    "string_template_fixed_part",
    "flwor_expression",
    "for_clause",
    "for_item_binding",
    "for_member_binding",
    "for_entry_binding",
    "for_entry_key_binding",
    "for_entry_value_binding",
    "positional_variable",
    "let_clause",
    "let_value_binding",
    "let_sequence_binding",
    "let_array_binding",
    "let_map_binding",
    "variable_name_and_type",
    "return_clause",
    "quantified_expression",
    "quantifier_binding",
    "if_expression",
    "or_expression",
    "and_expression",
    "comparison_expression",
    "otherwise_expression",
    "string_concatenation_expression",
    "range_expression",
    "additive_expression",
    "multiplicative_expression",
    "union_expression",
    "intersect_except_expression",
    "but_with_expression",
    "pipeline_expression",
    "simple_map_expression",
    "relative_path_expression",
    "instance_of_expression",
    "treat_expression",
    "castable_expression",
    "cast_expression",
    "cast_target",
    "arrow_expression",
    "restricted_dynamic_call",
    "unary_expression",
    "absolute_path_expression",
    "axis_step",
    "union_node_test",
    "dynamic_node_test",
    "wildcard",
    "filter_expression",
    "predicate",
    "dynamic_function_call",
    "lookup_expression",
    "lookup",
    "unary_lookup",
    "lookup_wildcard",
    "method_call",
    "function_call",
    "argument_list",
    "positional_argument_list",
    "argument_placeholder",
    "keyword_argument",
    "named_function_reference",
    "inline_function_expression",
    "map_constructor",
    "map_constructor_entry",
    "square_array_constructor",
    "curly_array_constructor",
    "computed_document_constructor",
    "computed_text_constructor",
    "computed_comment_constructor",
    "computed_element_constructor",
    "computed_attribute_constructor",
    "computed_namespace_constructor",
    "computed_processing_instruction_constructor",
    "marked_ncname",
    "type_declaration",
    "sequence_type",
    "any_item_type",
    "gnode_type",
    "jnode_type",
    "jroot_selector",
    "document_node_type",
    "element_node_type",
    "attribute_node_type",
    "schema_element_node_type",
    "schema_attribute_node_type",
    "processing_instruction_node_type",
    "comment_node_type",
    "text_node_type",
    "namespace_node_type",
    "any_xnode_type",
    "name_test_union",
    "any_map_type",
    "any_array_type",
    "any_record_type",
    "typed_map_type",
    "typed_array_type",
    "typed_record_type",
    "field_declaration",
    "enumeration_type",
    "any_function_type",
    "typed_function_type",
    "typed_function_parameter",
    "choice_item_type",
    "ncname",
    "prefixed_name",
    "uri_qualified_name",
};
_Static_assert(sizeof kind_names / sizeof kind_names[0] == SXML_AST_KIND_COUNT,
               "one name per AST kind");

/* Field names in sxml_ast_field order. */
static const char *const field_names[] = {
    "none",        "alternative", "argument",    "arguments",
    "arity",       "binding",     "body",        "clause",
    "condition",   "content",     "declaration", "else",
    "entry",       "expression",  "field",       "function",
    "item",        "item_type",   "key",         "left",
    "lookup",      "member",      "name",        "name_expression",
    "operand",     "parameter",   "part",        "path",
    "position",    "predicate",   "prefix",      "return",
    "return_type", "right",       "selector",    "sequence",
    "target",      "test",        "then",        "type",
    "uri",         "value",       "variable",
};
_Static_assert(sizeof field_names / sizeof field_names[0] ==
                   SXML_AST_FIELD_COUNT,
               "one name per AST field");

/* Operator tokens in sxml_ast_operator order. */
static const char *const operator_names[] = {
    "",
    "or",
    "and",
    "eq",
    "ne",
    "lt",
    "le",
    "gt",
    "ge",
    "=",
    "!=",
    "<",
    "<=",
    ">",
    ">=",
    "is",
    "is-not",
    "<<",
    ">>",
    "precedes-or-is",
    "follows-or-is",
    "otherwise",
    "||",
    "to",
    "+",
    "-",
    "*",
    "div",
    "idiv",
    "mod",
    "union",
    "intersect",
    "except",
    "!",
    "/",
    "//",
    "->",
    "=>",
    "=!>",
    "some",
    "every",
};
_Static_assert(sizeof operator_names / sizeof operator_names[0] ==
                   SXML_AST_OPERATOR_EVERY + 1,
               "one name per operator");

/* Axis names in sxml_ast_axis order. */
static const char *const axis_names[] = {
    "",
    "ancestor",
    "ancestor-or-self",
    "attribute",
    "child",
    "descendant",
    "descendant-or-self",
    "following",
    "following-or-self",
    "following-sibling",
    "following-sibling-or-self",
    "item",
    "namespace",
    "parent",
    "preceding",
    "preceding-or-self",
    "preceding-sibling",
    "preceding-sibling-or-self",
    "self",
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

static void write_detail(ast_writer writer, const sxml_ast_node *node) {
    written_text *output = writer.output;
    static const char *const occurrences[] = {"", "0", "1", "?",
                                              "", "",  "+", "*"};
    static const char *const name_test_kinds[] = {"any", "expanded",
                                                  "namespace", "local"};
    if (node->kind == SXML_AST_KIND_AXIS_STEP) {
        if (node->detail.axis != SXML_AST_AXIS_NONE) {
            write_text(output, " ");
            write_text(output, axis_names[node->detail.axis]);
        }
    } else if (node->kind == SXML_AST_KIND_SEQUENCE_TYPE) {
        write_text(output, " ");
        write_text(output, occurrences[node->detail.occurrence]);
    } else if (node->kind == SXML_AST_KIND_WILDCARD) {
        write_text(output, " ");
        write_text(output, name_test_kinds[node->detail.name_test_kind]);
    } else if (node->detail.operator!= SXML_AST_OPERATOR_NONE) {
        write_text(output, " ");
        write_text(output, operator_names[node->detail.operator]);
    }
    static const char *const flag_names[] = {"abbreviated", "optional",
                                             "nillable",    "braced_uri",
                                             "true",        "has_signature"};
    for (uint32_t flag_index = 0; flag_index < 6; flag_index += 1) {
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
        kind == SXML_AST_KIND_WILDCARD) {
        write_name(writer, node->payload.name);
    } else if ((kind >= SXML_AST_KIND_INTEGER_LITERAL &&
                kind <= SXML_AST_KIND_STRING_LITERAL) ||
               kind == SXML_AST_KIND_STRING_TEMPLATE_FIXED_PART) {
        write_text(writer.output, " \"");
        write_bytes(writer.output,
                    sxml_ast_get_text(writer.ast, node->payload.text));
        write_text(writer.output, "\"");
    }
}

/* Writes the opening of node: its kind, detail and payload. */
static void write_node_start(ast_writer writer, const sxml_ast_node *node) {
    write_text(writer.output, "(");
    write_text(writer.output, kind_names[node->kind]);
    write_detail(writer, node);
    write_payload(writer, node);
}

/* Writes " field=" for a child about to be written. */
static void write_field_label(ast_writer writer, const sxml_ast_node *child) {
    write_text(writer.output, " ");
    write_text(writer.output, field_names[child->field]);
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
    const char *text;
    /* Parse one SequenceType instead of an expression. */
    bool is_sequence_type;
    uint32_t maximum_expression_depth;
    sxml_allocator allocator;
} parse_request;

static parse_outcome parse_with(parse_request request) {
    parse_outcome outcome = {.status = SXML_STATUS_OK, .ast = NULL};
    memset(&outcome.error_record, 0, sizeof(outcome.error_record));
    const sxml_ast_build_request build_request = {
        .expression_text = sxml_byte_view_from_c_string(request.text),
        .source_name = SXML_BYTE_VIEW_FROM_LITERAL("query.xq"),
        .allocator = request.allocator,
        .maximum_expression_depth = request.maximum_expression_depth,
        .error_record = &outcome.error_record,
    };
    outcome.status =
        request.is_sequence_type
            ? sxml_xpath_build_sequence_type_ast(&build_request, &outcome.ast)
            : sxml_xpath_build_ast(&build_request, &outcome.ast);
    return outcome;
}

static parse_outcome parse_expression(const char *text) {
    return parse_with((parse_request){.text = text,
                                      .is_sequence_type = false,
                                      .maximum_expression_depth = 0,
                                      .allocator = sxml_default_allocator()});
}

static parse_outcome parse_sequence_type(const char *text) {
    return parse_with((parse_request){.text = text,
                                      .is_sequence_type = true,
                                      .maximum_expression_depth = 0,
                                      .allocator = sxml_default_allocator()});
}

/* An expression and the written form of its AST's body. */
typedef struct expected_ast {
    const char *text;
    const char *written;
} expected_ast;

/* Writes the AST of outcome: the body of an XPATH root, or the root. */
static void write_outcome(const parse_outcome *outcome, written_text *output) {
    output->byte_count = 0;
    output->is_truncated = false;
    output->bytes[0] = '\0';
    const sxml_ast *ast = outcome->ast;
    sxml_ast_node_index top = ast->root;
    if (sxml_ast_get_node(ast, top)->kind == SXML_AST_KIND_XPATH &&
        sxml_ast_get_node(ast, top)->first_child.value ==
            sxml_ast_get_node(ast, top)->last_child.value) {
        top = sxml_ast_get_node(ast, top)->first_child;
    }
    write_node((ast_writer){.ast = ast, .output = output}, top);
}

/* Checks each case of expectations; reports the failing text and form. */
typedef struct expectation_list {
    const expected_ast *cases;
    size_t case_count;
    bool is_sequence_type;
} expectation_list;

static void expect_asts(sxml_test_state *state, expectation_list list) {
    for (size_t case_index = 0; case_index < list.case_count; case_index += 1) {
        const expected_ast *expected = &list.cases[case_index];
        parse_outcome outcome = list.is_sequence_type
                                    ? parse_sequence_type(expected->text)
                                    : parse_expression(expected->text);
        SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
        if (outcome.status != SXML_STATUS_OK) {
            printf("  text: %s\n", expected->text);
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

/* ----------------------------------------------------------- Structure */

static const expected_ast operator_cases[] = {
    {"1 + 2 * 3",
     "(additive_expression + left=(integer_literal \"1\") "
     "right=(multiplicative_expression * left=(integer_literal \"2\") "
     "right=(integer_literal \"3\")))"},
    {"1 - 2 - 3",
     "(additive_expression - left=(additive_expression - left=(integer_literal "
     "\"1\") right=(integer_literal \"2\")) right=(integer_literal \"3\"))"},
    {"$a or $b and $c",
     "(or_expression or left=(variable_reference name=(ncname a)) "
     "right=(and_expression and left=(variable_reference name=(ncname b)) "
     "right=(variable_reference name=(ncname c))))"},
    {"1 eq 1, 2 != 3, $a << $b, $a precedes $b, $a is-not $b",
     "(sequence_expression item=(comparison_expression eq "
     "left=(integer_literal "
     "\"1\") right=(integer_literal \"1\")) item=(comparison_expression != "
     "left=(integer_literal \"2\") right=(integer_literal \"3\")) "
     "item=(comparison_expression << left=(variable_reference name=(ncname a)) "
     "right=(variable_reference name=(ncname b))) item=(comparison_expression "
     "<< left=(variable_reference name=(ncname a)) right=(variable_reference "
     "name=(ncname b))) item=(comparison_expression is-not "
     "left=(variable_reference name=(ncname a)) right=(variable_reference "
     "name=(ncname b))))"},
    {"4 \xC3\x97 2 \xC3\xB7 1 idiv 3 mod 2",
     "(multiplicative_expression mod left=(multiplicative_expression idiv "
     "left=(multiplicative_expression div left=(multiplicative_expression * "
     "left=(integer_literal \"4\") right=(integer_literal \"2\")) "
     "right=(integer_literal \"1\")) right=(integer_literal \"3\")) "
     "right=(integer_literal \"2\"))"},
    {"$a | $b union $c intersect $d except $e",
     "(union_expression union left=(union_expression union "
     "left=(variable_reference name=(ncname a)) right=(variable_reference "
     "name=(ncname b))) right=(intersect_except_expression except "
     "left=(intersect_except_expression intersect left=(variable_reference "
     "name=(ncname c)) right=(variable_reference name=(ncname d))) "
     "right=(variable_reference name=(ncname e))))"},
    {"1 to 3 || 'a' otherwise ()",
     "(otherwise_expression otherwise left=(string_concatenation_expression || "
     "left=(range_expression to left=(integer_literal \"1\") "
     "right=(integer_literal \"3\")) right=(string_literal \"a\")) "
     "right=(parenthesized_expression))"},
    {"- + 1",
     "(unary_expression - operand=(unary_expression + operand=(integer_literal "
     "\"1\")))"},
    {"$a -> f() ! .",
     "(pipeline_expression -> left=(variable_reference name=(ncname a)) "
     "right=(simple_map_expression ! left=(function_call name=(ncname f) "
     "arguments=(argument_list)) right=(context_value_reference)))"},
    {"$m but with {1: 2}",
     "(but_with_expression left=(variable_reference name=(ncname m)) "
     "right=(map_constructor entry=(map_constructor_entry key=(integer_literal "
     "\"1\") value=(integer_literal \"2\"))))"},
};

static void test_operators_keep_precedence_and_tokens(sxml_test_state *state) {
    expect_asts(
        state, (expectation_list){.cases = operator_cases,
                                  .case_count = SXML_TEST_COUNT(operator_cases),
                                  .is_sequence_type = false});
}

static const expected_ast path_cases[] = {
    {"/", "(absolute_path_expression /)"},
    {"/a/b[2]",
     "(absolute_path_expression / path=(relative_path_expression / "
     "left=(axis_step test=(ncname a)) right=(axis_step test=(ncname b) "
     "predicate=(predicate expression=(integer_literal \"2\")))))"},
    {"//@id",
     "(absolute_path_expression // path=(axis_step attribute abbreviated "
     "test=(ncname id)))"},
    {"../child::div/descendant-or-self::node()",
     "(relative_path_expression / left=(relative_path_expression / "
     "left=(axis_step parent abbreviated) right=(axis_step child test=(ncname "
     "div))) right=(axis_step descendant-or-self test=(gnode_type)))"},
    {"*, p:*, *:a, Q{u}*, Q{u}a",
     "(sequence_expression item=(axis_step test=(wildcard any )) "
     "item=(axis_step test=(wildcard namespace p:)) item=(axis_step "
     "test=(wildcard local a)) item=(axis_step test=(wildcard namespace "
     "braced_uri {u})) item=(axis_step test=(uri_qualified_name {u}a)))"},
    {"self::(a | b:c | *)",
     "(axis_step self test=(union_node_test test=(ncname a) "
     "test=(prefixed_name b:c) test=(wildcard any )))"},
    {"child::{'a'}",
     "(axis_step child test=(dynamic_node_test "
     "expression=(enclosed_expression expression=(string_literal \"a\"))))"},
    {"$x[1][. > 2]",
     "(filter_expression value=(filter_expression value=(variable_reference "
     "name=(ncname x)) predicate=(predicate expression=(integer_literal "
     "\"1\"))) predicate=(predicate expression=(comparison_expression > "
     "left=(context_value_reference) right=(integer_literal \"2\"))))"},
    {"element(a, xs:string?)",
     "(axis_step test=(element_node_type nillable name=(name_test_union "
     "test=(ncname "
     "a)) type=(prefixed_name xs:string)))"},
    {"jnode((), xs:integer)",
     "(axis_step test=(jnode_type selector=(jroot_selector) "
     "type=(sequence_type 1 item_type=(prefixed_name xs:integer))))"},
};

static void test_paths_and_steps(sxml_test_state *state) {
    expect_asts(state,
                (expectation_list){.cases = path_cases,
                                   .case_count = SXML_TEST_COUNT(path_cases),
                                   .is_sequence_type = false});
}

static const expected_ast function_cases[] = {
    {"function($x as xs:integer) as xs:integer { $x + . }",
     "(inline_function_expression has_signature "
     "parameter=(variable_name_and_type name=(ncname x) "
     "type=(type_declaration type=(sequence_type 1 "
     "item_type=(prefixed_name xs:integer)))) return_type=(type_declaration "
     "type=(sequence_type 1 item_type=(prefixed_name xs:integer))) "
     "body=(enclosed_expression expression=(additive_expression + "
     "left=(variable_reference name=(ncname x)) "
     "right=(context_value_reference))))"},
    {"fn { . + 1 }",
     "(inline_function_expression body=(enclosed_expression "
     "expression=(additive_expression + left=(context_value_reference) "
     "right=(integer_literal \"1\"))))"},
    {"fn() {}",
     "(inline_function_expression has_signature body=(enclosed_expression))"},
    {"Q{http://www.w3.org/2005/xpath-functions}concat#3",
     "(named_function_reference name=(uri_qualified_name "
     "{http://www.w3.org/2005/xpath-functions}concat) "
     "arity=(integer_literal \"3\"))"},
    {"f(?, 2, value := ?)",
     "(function_call name=(ncname f) arguments=(argument_list "
     "argument=(argument_placeholder) argument=(integer_literal \"2\") "
     "argument=(keyword_argument name=(ncname value) "
     "value=(argument_placeholder))))"},
    {"$f(1)(2)",
     "(dynamic_function_call function=(dynamic_function_call "
     "function=(variable_reference name=(ncname f)) "
     "arguments=(positional_argument_list argument=(integer_literal \"1\"))) "
     "arguments=(positional_argument_list argument=(integer_literal \"2\")))"},
    {"$s => upper-case() =!> $g(1)",
     "(arrow_expression =!> value=(arrow_expression => "
     "value=(variable_reference name=(ncname s)) target=(function_call "
     "name=(ncname upper-case) arguments=(argument_list))) "
     "target=(restricted_dynamic_call function=(variable_reference "
     "name=(ncname g)) arguments=(positional_argument_list "
     "argument=(integer_literal \"1\"))))"},
    {"$a =?> size()",
     "(method_call value=(variable_reference name=(ncname a)) name=(ncname "
     "size) arguments=(positional_argument_list))"},
};

static void test_functions_and_calls(sxml_test_state *state) {
    expect_asts(
        state, (expectation_list){.cases = function_cases,
                                  .case_count = SXML_TEST_COUNT(function_cases),
                                  .is_sequence_type = false});
}

static const expected_ast map_array_cases[] = {
    {"map{'a': 1, 'b': (2, 3)}?a",
     "(lookup_expression value=(map_constructor entry=(map_constructor_entry "
     "key=(string_literal \"a\") value=(integer_literal \"1\")) "
     "entry=(map_constructor_entry key=(string_literal \"b\") "
     "value=(parenthesized_expression expression=(sequence_expression "
     "item=(integer_literal \"2\") item=(integer_literal \"3\"))))) "
     "lookup=(lookup key=(ncname a)))"},
    {"{}", "(map_constructor)"},
    {"[1, (), [2]]?*",
     "(lookup_expression value=(square_array_constructor "
     "member=(integer_literal \"1\") member=(parenthesized_expression) "
     "member=(square_array_constructor member=(integer_literal \"2\"))) "
     "lookup=(lookup key=(lookup_wildcard)))"},
    {"array { 1 to 3 }?(2)",
     "(lookup_expression value=(curly_array_constructor "
     "content=(enclosed_expression expression=(range_expression to "
     "left=(integer_literal \"1\") right=(integer_literal \"3\")))) "
     "lookup=(lookup key=(parenthesized_expression "
     "expression=(integer_literal \"2\"))))"},
    {"?key, ?1, ?$k, ?*",
     "(sequence_expression item=(unary_lookup key=(ncname key)) "
     "item=(unary_lookup key=(integer_literal \"1\")) item=(unary_lookup "
     "key=(variable_reference name=(ncname k))) item=(unary_lookup "
     "key=(lookup_wildcard)))"},
};

static void test_maps_arrays_and_lookups(sxml_test_state *state) {
    expect_asts(state, (expectation_list){.cases = map_array_cases,
                                          .case_count =
                                              SXML_TEST_COUNT(map_array_cases),
                                          .is_sequence_type = false});
}

static const expected_ast clause_cases[] = {
    {"for $x at $i in (1, 2), member $m in [3] return $x",
     "(flwor_expression clause=(for_clause binding=(for_item_binding "
     "variable=(variable_name_and_type name=(ncname x)) "
     "position=(positional_variable name=(ncname i)) "
     "sequence=(parenthesized_expression expression=(sequence_expression "
     "item=(integer_literal \"1\") item=(integer_literal \"2\")))) "
     "binding=(for_member_binding variable=(variable_name_and_type "
     "name=(ncname m)) sequence=(square_array_constructor "
     "member=(integer_literal \"3\")))) return=(return_clause "
     "value=(variable_reference name=(ncname x))))"},
    {"let $a := 1, $( $b, $c ) := (2, 3) return $a",
     "(flwor_expression clause=(let_clause binding=(let_value_binding "
     "variable=(variable_name_and_type name=(ncname a)) "
     "value=(integer_literal \"1\")) binding=(let_sequence_binding "
     "variable=(variable_name_and_type name=(ncname b)) "
     "variable=(variable_name_and_type name=(ncname c)) "
     "value=(parenthesized_expression expression=(sequence_expression "
     "item=(integer_literal \"2\") item=(integer_literal \"3\"))))) "
     "return=(return_clause value=(variable_reference name=(ncname a))))"},
    {"some $x in 1 satisfies $x",
     "(quantified_expression some binding=(quantifier_binding "
     "variable=(variable_name_and_type name=(ncname x)) "
     "sequence=(integer_literal \"1\")) condition=(variable_reference "
     "name=(ncname x)))"},
    {"if (1) then 2 else 3",
     "(if_expression condition=(integer_literal \"1\") then=(integer_literal "
     "\"2\") else=(integer_literal \"3\"))"},
    {"if (1) { 2 }",
     "(if_expression condition=(integer_literal \"1\") "
     "then=(enclosed_expression expression=(integer_literal \"2\")))"},
    {"1 instance of xs:integer+",
     "(instance_of_expression value=(integer_literal \"1\") "
     "type=(sequence_type + item_type=(prefixed_name xs:integer)))"},
    {"'1' cast as xs:integer?",
     "(cast_expression value=(string_literal \"1\") type=(cast_target "
     "optional type=(prefixed_name xs:integer)))"},
};

static void test_clauses_and_type_expressions(sxml_test_state *state) {
    expect_asts(state,
                (expectation_list){.cases = clause_cases,
                                   .case_count = SXML_TEST_COUNT(clause_cases),
                                   .is_sequence_type = false});
}

static const expected_ast prolog_cases[] = {
    {"declare default element namespace 'u'; declare namespace p = 'v'; p:a",
     "(xpath declaration=(default_element_namespace_declaration "
     "uri=(string_literal \"u\")) declaration=(namespace_declaration "
     "prefix=(ncname p) uri=(string_literal \"v\")) body=(axis_step "
     "test=(prefixed_name p:a)))"},
};

static void test_namespace_declarations(sxml_test_state *state) {
    expect_asts(state,
                (expectation_list){.cases = prolog_cases,
                                   .case_count = SXML_TEST_COUNT(prolog_cases),
                                   .is_sequence_type = false});
}

/* ------------------------------------------------------------ Decoding */

static const expected_ast literal_cases[] = {
    {"\"a\"\"b\"", "(string_literal \"a\"b\")"},
    {"'it''s'", "(string_literal \"it's\")"},
    {"''", "(string_literal \"\")"},
    {"'say \"hi\"'", "(string_literal \"say \"hi\"\")"},
    {"1_000", "(integer_literal \"1000\")"},
    {"0xFF_ff", "(hex_integer_literal \"FFff\")"},
    {"0b1_01", "(binary_integer_literal \"101\")"},
    {"1_0.5_0", "(decimal_literal \"10.50\")"},
    {".5e-1_0", "(double_literal \".5e-10\")"},
    {"#xs:integer", "(qname_literal name=(prefixed_name xs:integer))"},
    {"`a{{b}}c``d{1}`",
     "(string_template part=(string_template_fixed_part \"a{b}c`d\") "
     "part=(enclosed_expression expression=(integer_literal \"1\")))"},
    {"Q{ http://x }p:local",
     "(axis_step test=(uri_qualified_name { http://x }p:local))"},
    {"div div div", "(multiplicative_expression div left=(axis_step "
                    "test=(ncname div)) right=(axis_step test=(ncname div)))"},
    {"1 (: a (: nested :) comment :) + 2",
     "(additive_expression + left=(integer_literal \"1\") "
     "right=(integer_literal \"2\"))"},
};

static void test_literals_and_names_are_decoded(sxml_test_state *state) {
    expect_asts(state,
                (expectation_list){.cases = literal_cases,
                                   .case_count = SXML_TEST_COUNT(literal_cases),
                                   .is_sequence_type = false});
}

/* -------------------------------------------------------- Sequence types */

static const expected_ast sequence_type_cases[] = {
    {"xs:integer", "(sequence_type 1 item_type=(prefixed_name xs:integer))"},
    {"empty-sequence()", "(sequence_type 0)"},
    {"item()*", "(sequence_type * item_type=(any_item_type))"},
    {"fn(item()) as xs:boolean?",
     "(sequence_type 1 item_type=(typed_function_type "
     "parameter=(typed_function_parameter type=(sequence_type 1 "
     "item_type=(any_item_type))) return_type=(sequence_type ? "
     "item_type=(prefixed_name xs:boolean))))"},
    {"map(xs:string, xs:integer+)",
     "(sequence_type 1 item_type=(typed_map_type key=(prefixed_name "
     "xs:string) value=(sequence_type + item_type=(prefixed_name "
     "xs:integer))))"},
    {"array(empty-sequence())",
     "(sequence_type 1 item_type=(typed_array_type member=(sequence_type "
     "0)))"},
    {"function(*)", "(sequence_type 1 item_type=(any_function_type))"},
    {"record(a, 'b' as xs:int?)",
     "(sequence_type 1 item_type=(typed_record_type "
     "field=(field_declaration name=(ncname a)) field=(field_declaration "
     "name=(string_literal \"b\") type=(sequence_type ? "
     "item_type=(prefixed_name xs:int)))))"},
    {"(xs:int | enum('a'))+",
     "(sequence_type + item_type=(choice_item_type "
     "alternative=(prefixed_name xs:int) alternative=(enumeration_type "
     "value=(string_literal \"a\"))))"},
    {"element(*, xs:anyType?)",
     "(sequence_type 1 item_type=(element_node_type nillable "
     "name=(name_test_union test=(wildcard any )) type=(prefixed_name "
     "xs:anyType)))"},
};

static void test_sequence_types_build_alone(sxml_test_state *state) {
    expect_asts(state, (expectation_list){
                           .cases = sequence_type_cases,
                           .case_count = SXML_TEST_COUNT(sequence_type_cases),
                           .is_sequence_type = true});
}

static void test_sequence_type_spans_count_from_text(sxml_test_state *state) {
    parse_outcome outcome = parse_sequence_type("  xs:int");
    SXML_TEST_REQUIRE(state, outcome.status == SXML_STATUS_OK);
    const sxml_ast_node *root =
        sxml_ast_get_node(outcome.ast, outcome.ast->root);
    SXML_TEST_EXPECT(state, root->kind == SXML_AST_KIND_SEQUENCE_TYPE);
    SXML_TEST_EXPECT(state, root->parent.value == 0);
    SXML_TEST_EXPECT(state, root->field == SXML_AST_FIELD_NONE);
    SXML_TEST_EXPECT(state, root->source_span.byte_offset == 2);
    SXML_TEST_EXPECT(state, root->source_span.byte_count == 6);
    SXML_TEST_EXPECT(state, root->source_span.column_number == 3);
    sxml_ast_destroy(outcome.ast);
}

static void test_sequence_type_rejects_other_text(sxml_test_state *state) {
    static const char *const texts[] = {
        "xs:int, 1",
        "1",
        "xs:int)",
        "",
        "xs:int instance of item()",
        "item() or 1",
        "xs:int) instance of (item()",
    };
    for (size_t text_index = 0; text_index < SXML_TEST_COUNT(texts);
         text_index += 1) {
        parse_outcome outcome = parse_sequence_type(texts[text_index]);
        SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_STATIC_ERROR);
        SXML_TEST_EXPECT(state, outcome.ast == NULL);
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                    outcome.error_record.code.local_name,
                                    SXML_BYTE_VIEW_FROM_LITERAL("XPST0003")));
    }
}

/* -------------------------------------------------------------- Errors */

typedef struct expected_error {
    const char *text;
    uint32_t line_number;
    uint32_t column_number;
} expected_error;

static void
test_syntax_errors_are_xpst0003_with_location(sxml_test_state *state) {
    static const expected_error errors[] = {
        {"1 +", 1, 3},
        {"(1", 1, 3},
        {"", 1, 1},
        {"1 2", 1, 1},
        {"f(\n  1,,2)", 2, 4},
        /* A.3.2: two non-delimiting terminals touch. */
        {"10div 3", 1, 3},
        {"$a eq$b or 1or 2", 1, 13},
        {"\xC3\xA9\xC3\xA9 + 1div 2", 1, 7},
        /* KeySpecifier: an NCName, not a prefixed name. */
        {"$m?a:b", 1, 4},
        {"\n?x:y", 2, 2},
    };
    for (size_t error_index = 0; error_index < SXML_TEST_COUNT(errors);
         error_index += 1) {
        const expected_error *expected = &errors[error_index];
        parse_outcome outcome = parse_expression(expected->text);
        SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_STATIC_ERROR);
        SXML_TEST_EXPECT(state, outcome.ast == NULL);
        const sxml_error_record *record = &outcome.error_record;
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                    record->code.namespace_uri,
                                    SXML_BYTE_VIEW_FROM_LITERAL(
                                        "http://www.w3.org/2005/xqt-errors")));
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                    record->code.local_name,
                                    SXML_BYTE_VIEW_FROM_LITERAL("XPST0003")));
        SXML_TEST_EXPECT(state, record->diagnostic.description.byte_count != 0);
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                    record->diagnostic.location.source_name,
                                    SXML_BYTE_VIEW_FROM_LITERAL("query.xq")));
        const bool location_matches =
            record->diagnostic.location.line_number == expected->line_number &&
            record->diagnostic.location.column_number ==
                expected->column_number;
        SXML_TEST_EXPECT(state, location_matches);
        if (!location_matches) {
            printf("  text: %s\n  location: %u:%u\n", expected->text,
                   (unsigned)record->diagnostic.location.line_number,
                   (unsigned)record->diagnostic.location.column_number);
        }
    }
}

static void test_separated_terminals_are_accepted(sxml_test_state *state) {
    static const char *const texts[] = {
        "10 div 3", "1(:c:)div 2", "$a eq $b",  "a/b",
        "1+2",      "'a'eq'a'",    "(1)div(2)", "$x[1]is$x[2]",
    };
    for (size_t text_index = 0; text_index < SXML_TEST_COUNT(texts);
         text_index += 1) {
        parse_outcome outcome = parse_expression(texts[text_index]);
        SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
        sxml_ast_destroy(outcome.ast);
    }
}

static void test_error_record_may_be_absent(sxml_test_state *state) {
    sxml_ast *ast = NULL;
    const sxml_ast_build_request request = {
        .expression_text = SXML_BYTE_VIEW_FROM_LITERAL("1 +"),
        .source_name = {.bytes = NULL, .byte_count = 0},
        .allocator = sxml_default_allocator(),
        .maximum_expression_depth = 0,
        .error_record = NULL,
    };
    SXML_TEST_EXPECT(state, sxml_xpath_build_ast(&request, &ast) ==
                                SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, ast == NULL);
}

static void test_invalid_utf8_is_rejected(sxml_test_state *state) {
    parse_outcome outcome = parse_expression("'\xC3('");
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_INVALID_UTF8);
    SXML_TEST_EXPECT(state, outcome.ast == NULL);
}

/* --------------------------------------------------------- Source spans */

static void test_spans_count_lines_and_codepoints(sxml_test_state *state) {
    parse_outcome outcome =
        parse_expression("'\xC3\xA9' ||\n  \xC3\xA9t\xC3\xA9");
    SXML_TEST_REQUIRE(state, outcome.status == SXML_STATUS_OK);
    const sxml_ast *ast = outcome.ast;
    const sxml_ast_node_index body = sxml_ast_find_child(
        ast, (sxml_ast_child_query){.parent = ast->root,
                                    .field = SXML_AST_FIELD_BODY});
    const sxml_ast_node *concatenation = sxml_ast_get_node(ast, body);
    SXML_TEST_EXPECT(state, concatenation->kind ==
                                SXML_AST_KIND_STRING_CONCATENATION_EXPRESSION);
    SXML_TEST_EXPECT(state, concatenation->source_span.byte_offset == 0);
    SXML_TEST_EXPECT(state, concatenation->source_span.byte_count == 15);
    const sxml_ast_node_index right = sxml_ast_find_child(
        ast,
        (sxml_ast_child_query){.parent = body, .field = SXML_AST_FIELD_RIGHT});
    const sxml_ast_node *step = sxml_ast_get_node(ast, right);
    SXML_TEST_EXPECT(state, step->source_span.byte_offset == 10);
    SXML_TEST_EXPECT(state, step->source_span.line_number == 2);
    SXML_TEST_EXPECT(state, step->source_span.column_number == 3);
    const sxml_ast_node *name = sxml_ast_get_node(ast, step->first_child);
    SXML_TEST_EXPECT(state, name->kind == SXML_AST_KIND_NCNAME);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(
                         sxml_ast_get_text(ast, name->payload.name.local_name),
                         SXML_BYTE_VIEW_FROM_LITERAL("\xC3\xA9t\xC3\xA9")));
    const sxml_source_location location =
        sxml_ast_get_source_location(ast, right);
    SXML_TEST_EXPECT(
        state, sxml_byte_view_equals(location.source_name,
                                     SXML_BYTE_VIEW_FROM_LITERAL("query.xq")));
    sxml_ast_destroy(outcome.ast);
}

static void test_jnode_root_selector_is_one_node(sxml_test_state *state) {
    parse_outcome outcome = parse_expression("jnode( ( ) )");
    SXML_TEST_REQUIRE(state, outcome.status == SXML_STATUS_OK);
    const sxml_ast *ast = outcome.ast;
    size_t selector_count = 0;
    sxml_ast_source_span span = {0, 0, 0, 0};
    for (uint32_t index = 1; index < ast->node_count; index += 1) {
        const sxml_ast_node *node =
            sxml_ast_get_node(ast, (sxml_ast_node_index){.value = index});
        if (node->kind == SXML_AST_KIND_JROOT_SELECTOR) {
            selector_count += 1;
            span = node->source_span;
        }
    }
    SXML_TEST_EXPECT(state, selector_count == 1);
    SXML_TEST_EXPECT(state, span.byte_offset == 7);
    SXML_TEST_EXPECT(state, span.byte_count == 3);
    sxml_ast_destroy(outcome.ast);
}

/* ------------------------------------------------------- Limits, memory */

static void test_depth_limit_is_enforced(sxml_test_state *state) {
    /* xpath > additive > additive > integer: depth 4. */
    parse_outcome outcome =
        parse_with((parse_request){.text = "1 + 2 + 3",
                                   .is_sequence_type = false,
                                   .maximum_expression_depth = 3,
                                   .allocator = sxml_default_allocator()});
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_LIMIT_EXCEEDED);
    SXML_TEST_EXPECT(state, outcome.ast == NULL);
    outcome =
        parse_with((parse_request){.text = "1 + 2 + 3",
                                   .is_sequence_type = false,
                                   .maximum_expression_depth = 4,
                                   .allocator = sxml_default_allocator()});
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
    sxml_ast_destroy(outcome.ast);
}

static void test_deep_nesting_needs_no_recursion(sxml_test_state *state) {
    const size_t depth = 20000;
    const sxml_allocator allocator = sxml_default_allocator();
    void *memory = NULL;
    SXML_TEST_REQUIRE(state,
                      sxml_allocator_allocate(
                          allocator,
                          (sxml_allocation_request){.byte_count = 2 * depth + 2,
                                                    .alignment = 1},
                          &memory) == SXML_STATUS_OK);
    char *text = memory;
    size_t byte_count = 0;
    for (size_t level = 0; level < depth; level += 1) {
        text[byte_count] = '(';
        byte_count += 1;
    }
    text[byte_count] = '1';
    byte_count += 1;
    for (size_t level = 0; level < depth; level += 1) {
        text[byte_count] = ')';
        byte_count += 1;
    }
    text[byte_count] = '\0';
    parse_outcome outcome = parse_expression(text);
    sxml_allocator_release(allocator, memory);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
    if (outcome.status == SXML_STATUS_OK) {
        /* xpath, depth parenthesized expressions, the literal. */
        SXML_TEST_EXPECT(state, outcome.ast->node_count == depth + 3);
    }
    sxml_ast_destroy(outcome.ast);
}

/* A counting allocator over the default allocator, placed on the stack. */
typedef struct counting_fixture {
    alignas(TEST_STORAGE_ALIGNMENT) uint8_t storage[128];
    sxml_counting_allocator *counting_allocator;
} counting_fixture;

static sxml_status
counting_fixture_initialize(counting_fixture *fixture,
                            uint64_t failing_allocation_number) {
    return sxml_counting_allocator_initialize(
        (sxml_memory_view){.bytes = fixture->storage,
                           .byte_count = sizeof(fixture->storage)},
        &(sxml_counting_allocator_configuration){
            .backing_allocator = sxml_default_allocator(),
            .failing_allocation_number = failing_allocation_number,
        },
        &fixture->counting_allocator);
}

static void
test_build_survives_every_allocation_failure(sxml_test_state *state) {
    uint64_t failing_allocation_number = 1;
    for (;;) {
        counting_fixture fixture = {.storage = {0}, .counting_allocator = NULL};
        SXML_TEST_REQUIRE(state, counting_fixture_initialize(
                                     &fixture, failing_allocation_number) ==
                                     SXML_STATUS_OK);
        parse_outcome outcome = parse_with((parse_request){
            .text = "let $f := function($x){$x + .} return $f(4)",
            .is_sequence_type = false,
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
    /* The AST is one allocation; tree-sitter's memory is its own (D12). */
    SXML_TEST_EXPECT(state, failing_allocation_number == 2);
}

/* ---------------------------------------------------- Slice queries */

/*
 * Queries of the torture slice (tests/qt4/torture-slice.txt), one or more
 * per area, so a front end regression shows here before the QT4 driver
 * runs.
 */
static void test_slice_queries_build(sxml_test_state *state) {
    static const char *const queries[] = {
        "1 + 1 eq 2",
        "xs:int(\"2147483647\") + xs:int(\"2147483647\")",
        "(xs:float(\"1.5\") div xs:float(\"2\")) instance of xs:float",
        "let $f := function($x){$x + .} return 3!$f(4)",
        "map:merge((map{1:'a'}, map{2:'b'}), map{'duplicates':'use-last'})",
        "array:filter([1, 2, 3], fn($m) { $m gt 1 })",
        "fn:filter(1 to 10, function($a) {$a mod 2 = 0})",
        "for-each(1 to 3, string#1)",
        "(1 to 5)[. mod 2 = 1] => sum()",
        "$in//b[@id = 'x']/following-sibling::*[1]",
        "parse-json('{\"a\":[1,2]}')?a?*",
        "some $x in (1, 2, 3) satisfies $x eq 2",
        "QName('http://example.com/', 'p:local') instance of xs:QName",
        "fn:error(xs:QName('err:FOER0000'))",
        "concat(?, 'b')('a')",
        "let $m := {'a': 1} return $m?a",
        "array { 1, 2 }?2",
        "`{1}+{2}`",
        "xs:NMTOKENS('a b c') ! string()",
        "(1, 2, 3)[last()] instance of xs:integer",
        "if (true()) then 'a' else 'b'",
        "every $f in (true#0, false#0) satisfies $f() = true()",
        "map{'a':1} => map:put('b', 2) => map:size()",
        "for $x in 1 to 3, $y in ('a', 'b') return $x || $y",
        "/root/(a | b)[2]/text()",
        "$m?('a', 'b')",
        "-xs:double('INF') lt 0e0",
        "matches('abc', '^a.c$')",
        "tokenize('a,b,,c', ',')",
        "1 to 3 ! (. * 2)",
    };
    for (size_t query_index = 0; query_index < SXML_TEST_COUNT(queries);
         query_index += 1) {
        parse_outcome outcome = parse_expression(queries[query_index]);
        SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
        if (outcome.status != SXML_STATUS_OK) {
            printf("  query: %s\n", queries[query_index]);
        }
        sxml_ast_destroy(outcome.ast);
    }
}

static const sxml_test_case xpath_parse_test_cases[] = {
    {.name = "operators_keep_precedence_and_tokens",
     .function = test_operators_keep_precedence_and_tokens},
    {.name = "paths_and_steps", .function = test_paths_and_steps},
    {.name = "functions_and_calls", .function = test_functions_and_calls},
    {.name = "maps_arrays_and_lookups",
     .function = test_maps_arrays_and_lookups},
    {.name = "clauses_and_type_expressions",
     .function = test_clauses_and_type_expressions},
    {.name = "namespace_declarations", .function = test_namespace_declarations},
    {.name = "literals_and_names_are_decoded",
     .function = test_literals_and_names_are_decoded},
    {.name = "sequence_types_build_alone",
     .function = test_sequence_types_build_alone},
    {.name = "sequence_type_spans_count_from_text",
     .function = test_sequence_type_spans_count_from_text},
    {.name = "sequence_type_rejects_other_text",
     .function = test_sequence_type_rejects_other_text},
    {.name = "syntax_errors_are_xpst0003_with_location",
     .function = test_syntax_errors_are_xpst0003_with_location},
    {.name = "separated_terminals_are_accepted",
     .function = test_separated_terminals_are_accepted},
    {.name = "error_record_may_be_absent",
     .function = test_error_record_may_be_absent},
    {.name = "invalid_utf8_is_rejected",
     .function = test_invalid_utf8_is_rejected},
    {.name = "spans_count_lines_and_codepoints",
     .function = test_spans_count_lines_and_codepoints},
    {.name = "jnode_root_selector_is_one_node",
     .function = test_jnode_root_selector_is_one_node},
    {.name = "depth_limit_is_enforced",
     .function = test_depth_limit_is_enforced},
    {.name = "deep_nesting_needs_no_recursion",
     .function = test_deep_nesting_needs_no_recursion},
    {.name = "build_survives_every_allocation_failure",
     .function = test_build_survives_every_allocation_failure},
    {.name = "slice_queries_build", .function = test_slice_queries_build},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xpath_parse",
            .cases = xpath_parse_test_cases,
            .case_count = SXML_TEST_COUNT(xpath_parse_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
