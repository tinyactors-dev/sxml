/*
 * Unit tests for src/xslt_value_template.c: value templates (XSLT 4.0,
 * section 5.6) to the XPath AST of a string template (D97, D102).
 *
 * Golden file: tests/xpath_ast_test.c (parser parts, AST comparison).
 */
#include "evaluator_internal.h"
#include "ir_pass_internal.h"
#include "processor_internal.h"
#include "sxml/processor.h"
#include "sxml/xdm.h"
#include "test_runner.h"
#include "xpath_static_internal.h"
#include "xslt_value_template_internal.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static sxml_ast_build_request request_of(const char *text,
                                         sxml_error_record *record) {
    return (sxml_ast_build_request){
        .expression_text = sxml_byte_view_from_c_string(text),
        .source_name = SXML_BYTE_VIEW_FROM_LITERAL("test.xsl"),
        .allocator = sxml_default_allocator(),
        .maximum_expression_depth = 0,
        .error_record = record,
    };
}

/* True when both ASTs have the same nodes and text, source spans apart
   (a string template's are one backquote further on). */
static bool asts_match(const sxml_ast *left, const sxml_ast *right) {
    if (left->node_count != right->node_count ||
        left->root.value != right->root.value ||
        left->text_byte_count != right->text_byte_count ||
        (left->text_byte_count != 0 &&
         memcmp(left->text_bytes, right->text_bytes,
                left->text_byte_count) != 0)) {
        return false;
    }
    /* Byte for byte, as tests/xpath_ast_test.c compares ASTs, with the
       span blanked in both. */
    const size_t span_offset = offsetof(sxml_ast_node, source_span);
    for (uint32_t node_number = 1; node_number < left->node_count;
         node_number += 1U) {
        unsigned char left_bytes[sizeof(sxml_ast_node)];
        unsigned char right_bytes[sizeof(sxml_ast_node)];
        memcpy(left_bytes, &left->nodes[node_number], sizeof left_bytes);
        memcpy(right_bytes, &right->nodes[node_number], sizeof right_bytes);
        memset(left_bytes + span_offset, 0, sizeof(sxml_ast_source_span));
        memset(right_bytes + span_offset, 0, sizeof(sxml_ast_source_span));
        if (memcmp(left_bytes, right_bytes, sizeof left_bytes) != 0) {
            return false;
        }
    }
    return true;
}

/* A value template and the XPath string template it must equal. */
typedef struct template_pair {
    const char *value_template;
    const char *string_template;
} template_pair;

static void test_templates_build_string_template_asts(sxml_test_state *state) {
    static const template_pair pairs[] = {
        {"", "``"},
        {"plain text", "`plain text`"},
        {"{ $image-dir }/{ href }", "`{ $image-dir }/{ href }`"},
        {"a{$x}b{{c}}", "`a{$x}b{{c}}`"},
        {"{{{1}}}", "`{{{1}}}`"},
        {"a{}b{ }c{(: } :)}d", "`a{}b{ }c{(: } :)}d`"},
        {"{'}'}{map{1:2}}", "`{'}'}{map{1:2}}`"},
        {"{`x{{{1}}}y`}", "`{`x{{{1}}}y`}`"},
        /* A backquote is text in a value template. */
        {"x`y``z", "`x``y````z`"},
        {"(: text :)", "`(: text :)`"},
        {"\n  { 1 + 2 }\n", "`\n  { 1 + 2 }\n`"},
    };
    for (size_t pair_number = 0; pair_number < SXML_TEST_COUNT(pairs);
         pair_number += 1U) {
        const template_pair pair = pairs[pair_number];
        sxml_ast *expected = NULL;
        sxml_ast *built = NULL;
        SXML_TEST_REQUIRE(state, sxml_xpath_build_ast(
                                     &(sxml_ast_build_request){
                                         .expression_text =
                                             sxml_byte_view_from_c_string(
                                                 pair.string_template),
                                         .source_name = {.bytes = NULL,
                                                         .byte_count = 0},
                                         .allocator = sxml_default_allocator(),
                                         .maximum_expression_depth = 0,
                                         .error_record = NULL},
                                     &expected) == SXML_STATUS_OK);
        const sxml_ast_build_request request =
            request_of(pair.value_template, NULL);
        SXML_TEST_EXPECT(state, sxml_xslt_build_value_template_ast(
                                    &request, &built) == SXML_STATUS_OK);
        const bool is_match = built != NULL && asts_match(expected, built);
        SXML_TEST_EXPECT(state, is_match);
        if (built != NULL) {
            const sxml_ast_node *root = sxml_ast_get_node(built, built->root);
            SXML_TEST_EXPECT(state, root->kind == SXML_AST_KIND_XPATH);
            SXML_TEST_EXPECT(state, sxml_ast_get_node(built, root->first_child)
                                            ->kind ==
                                        SXML_AST_KIND_STRING_TEMPLATE);
            SXML_TEST_EXPECT(state,
                             sxml_byte_view_equals(
                                 built->source_name,
                                 SXML_BYTE_VIEW_FROM_LITERAL("test.xsl")));
        }
        sxml_ast_destroy(built);
        sxml_ast_destroy(expected);
    }
}

static void test_fixed_parts_are_expanded(sxml_test_state *state) {
    sxml_ast *built = NULL;
    const sxml_ast_build_request request = request_of("{{a}}`{1}{{", NULL);
    SXML_TEST_REQUIRE(state, sxml_xslt_build_value_template_ast(
                                 &request, &built) == SXML_STATUS_OK);
    const sxml_ast_node *body = sxml_ast_get_node(
        built, sxml_ast_get_node(built, built->root)->first_child);
    const sxml_ast_node *first = sxml_ast_get_node(built, body->first_child);
    const sxml_ast_node *second = sxml_ast_get_node(built, first->next_sibling);
    const sxml_ast_node *third = sxml_ast_get_node(built, second->next_sibling);
    SXML_TEST_EXPECT(state,
                     first->kind == SXML_AST_KIND_STRING_TEMPLATE_FIXED_PART);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                sxml_ast_get_text(built, first->payload.text),
                                SXML_BYTE_VIEW_FROM_LITERAL("{a}`")));
    SXML_TEST_EXPECT(state, first->field == SXML_AST_FIELD_PART);
    SXML_TEST_EXPECT(state, second->kind == SXML_AST_KIND_ENCLOSED_EXPRESSION);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                sxml_ast_get_text(built, third->payload.text),
                                SXML_BYTE_VIEW_FROM_LITERAL("{")));
    SXML_TEST_EXPECT(state, third->next_sibling.value == 0);
    sxml_ast_destroy(built);
}

/* A template that must fail, the code and where. */
typedef struct expected_error {
    const char *value_template;
    const char *error_code;
    uint32_t line_number;
    uint32_t column_number;
} expected_error;

static void test_curly_bracket_errors(sxml_test_state *state) {
    static const expected_error errors[] = {
        /* XSLT 4.0, 5.6 (error-0350a, error-0350b, error-0370a). */
        {"x{3}y{4", "XTSE0350", 1, 6},
        {"{banana", "XTSE0350", 1, 1},
        {"{2+2} and }3", "XTSE0370", 1, 11},
        {"a}}b}", "XTSE0370", 1, 5},
        {"a\n\xc3\xa9}", "XTSE0370", 2, 2},
        /* The "}" is in a string literal or a comment. */
        {"{'}", "XTSE0350", 1, 1},
        {"{(: } :)", "XTSE0350", 1, 1},
        {"{`a{1}", "XTSE0350", 1, 1},
        /* Brackets match; the expression is wrong. */
        {"{1 +}", "XPST0003", 1, 0},
        {"{`}`}", "XPST0003", 1, 0},
        {"{1 2}", "XPST0003", 1, 0},
    };
    for (size_t error_number = 0; error_number < SXML_TEST_COUNT(errors);
         error_number += 1U) {
        const expected_error expected = errors[error_number];
        sxml_error_record record;
        memset(&record, 0, sizeof record);
        sxml_ast *built = NULL;
        const sxml_ast_build_request request =
            request_of(expected.value_template, &record);
        SXML_TEST_EXPECT(state, sxml_xslt_build_value_template_ast(
                                    &request, &built) ==
                                    SXML_STATUS_STATIC_ERROR);
        SXML_TEST_EXPECT(state, built == NULL);
        sxml_ast_destroy(built);
        const bool has_code = sxml_byte_view_equals(
            record.code.local_name,
            sxml_byte_view_from_c_string(expected.error_code));
        SXML_TEST_EXPECT(state, has_code);
        if (!has_code) {
            (void)fprintf(stderr, "for %s: %.*s\n", expected.value_template,
                          (int)record.code.local_name.byte_count,
                          (const char *)record.code.local_name.bytes);
        }
        SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                    record.code.namespace_uri,
                                    SXML_BYTE_VIEW_FROM_LITERAL(
                                        SXML_ERROR_NAMESPACE_URI)));
        SXML_TEST_EXPECT(state, record.diagnostic.description.byte_count != 0);
        SXML_TEST_EXPECT(state, record.diagnostic.location.line_number ==
                                    expected.line_number);
        if (expected.column_number != 0) {
            SXML_TEST_EXPECT(state, record.diagnostic.location.column_number ==
                                        expected.column_number);
        }
    }
}

static void test_requests_are_checked(sxml_test_state *state) {
    sxml_ast *built = NULL;
    const sxml_ast_build_request request = request_of("{1}", NULL);
    SXML_TEST_EXPECT(state, sxml_xslt_build_value_template_ast(NULL, &built) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state,
                     sxml_xslt_build_value_template_ast(&request, NULL) ==
                                SXML_STATUS_INVALID_ARGUMENT);
    const sxml_ast_build_request invalid_text = request_of("a\xff{1}", NULL);
    SXML_TEST_EXPECT(state, sxml_xslt_build_value_template_ast(
                                &invalid_text, &built) ==
                                SXML_STATUS_INVALID_UTF8);
    sxml_ast_build_request shallow = request_of("{((((1))))}", NULL);
    shallow.maximum_expression_depth = 3;
    SXML_TEST_EXPECT(state, sxml_xslt_build_value_template_ast(
                                &shallow, &built) ==
                                SXML_STATUS_LIMIT_EXCEEDED);
    /* No error record: the status alone. */
    const sxml_ast_build_request unrecorded = request_of("}", NULL);
    SXML_TEST_EXPECT(state, sxml_xslt_build_value_template_ast(
                                &unrecorded, &built) ==
                                SXML_STATUS_STATIC_ERROR);
    SXML_TEST_EXPECT(state, built == NULL);
}

/* ------------------------------------------------------- Effective value */

/* The effective value of a template (5.6), lowered as a string template:
   true when it is expected_text. */
typedef struct effective_value_case {
    const char *value_template;
    const char *expected_text;
} effective_value_case;

static bool has_effective_value(sxml_processor *processor,
                                effective_value_case test_case) {
    sxml_item_heap *item_heap = sxml_processor_get_item_heap(processor);
    const sxml_xpath_static_context static_context = {
        .item_heap = item_heap,
        .allocator = sxml_default_allocator(),
        .namespace_bindings = {.namespace_bindings = NULL,
                               .namespace_binding_count = 0},
        .variable_declarations = {.variable_declarations = NULL,
                                  .variable_declaration_count = 0},
        .function_table = sxml_processor_get_function_table(processor),
        .static_base_uri = {.bytes = NULL, .byte_count = 0},
    };
    sxml_error_record record;
    memset(&record, 0, sizeof record);
    sxml_ast *ast = NULL;
    const sxml_ast_build_request request =
        request_of(test_case.value_template, &record);
    sxml_status status = sxml_xslt_build_value_template_ast(&request, &ast);
    sxml_ir_unit *unit = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lower(
            &(sxml_xpath_lowering_request){.ast = ast,
                                           .static_context = &static_context,
                                           .maximum_expression_count = 0,
                                           .maximum_expression_depth = 0,
                                           .error_record = &record},
            &unit);
    }
    sxml_ast_destroy(ast);
    sxml_diagnostic diagnostic;
    memset(&diagnostic, 0, sizeof diagnostic);
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_run_passes(&(sxml_ir_pass_list_request){
            .pass_request = {.unit = unit,
                             .type_registry =
                                 sxml_item_heap_get_type_registry(item_heap),
                             .error_record = &record,
                             .diagnostic = &diagnostic},
            .passes = sxml_ir_standard_passes,
            .verifies_after_each_pass = true});
    }
    sxml_dynamic_context *dynamic_context = NULL;
    if (status == SXML_STATUS_OK) {
        sxml_ir_unit_seal(unit);
        status = sxml_dynamic_context_create(
            sxml_default_allocator(),
            &(sxml_evaluator_configuration){
                .item_heap = item_heap,
                .allocator = sxml_default_allocator(),
                .function_table = static_context.function_table},
            &dynamic_context);
    }
    sxml_sequence value = sxml_sequence_empty();
    if (status == SXML_STATUS_OK) {
        status = sxml_evaluator_evaluate(
            dynamic_context,
            &(sxml_evaluation_request){.unit = unit,
                                       .has_context_value = false,
                                       .error_record = &record},
            &value);
    }
    bool is_expected = false;
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_get_item(
            value, (sxml_item_request){.item_index = 0, .error_record = NULL},
            &item);
    }
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof datum);
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(item, &datum);
    }
    uint64_t item_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_sequence_count(value, NULL, &item_count);
    }
    if (status == SXML_STATUS_OK) {
        is_expected = item_count == 1U &&
                      datum.datum_kind == SXML_DATUM_KIND_STRING &&
                      sxml_byte_view_equals(
                          datum.datum_details.string.utf8,
                          sxml_byte_view_from_c_string(
                              test_case.expected_text));
    }
    if (!is_expected) {
        (void)fprintf(stderr, "for %s: status %d %.*s\n",
                      test_case.value_template, (int)status,
                      (int)record.code.local_name.byte_count,
                      (const char *)record.code.local_name.bytes);
    }
    sxml_sequence_release(value);
    sxml_dynamic_context_destroy(dynamic_context);
    sxml_ir_unit_release(unit);
    return is_expected;
}

static void test_effective_values(sxml_test_state *state) {
    static const effective_value_case cases[] = {
        {"", ""},
        {"plain", "plain"},
        /* The XSLT 4.0 5.6.1 example "Producing a Space-Separated List". */
        {"{10.32, 5.50, 8.31}", "10.32 5.5 8.31"},
        {"x{1 + 2}y{}z{ (: none :) }{{w}}", "x3yz{w}"},
        {"{'}'}{``}`", "}`"},
        {"a{`b{{{1}}}c`}d", "ab{1}cd"},
    };
    sxml_processor *processor = NULL;
    SXML_TEST_REQUIRE(state, sxml_processor_create(
                                 sxml_default_allocator(),
                                 &(sxml_processor_configuration){
                                     .allocator = sxml_default_allocator()},
                                 &processor) == SXML_STATUS_OK);
    for (size_t case_number = 0; case_number < SXML_TEST_COUNT(cases);
         case_number += 1U) {
        SXML_TEST_EXPECT(state,
                         has_effective_value(processor, cases[case_number]));
    }
    SXML_TEST_EXPECT(
        state,
        sxml_item_heap_read_counts(sxml_processor_get_item_heap(processor))
                .live_cell_count == 0);
    sxml_processor_destroy(processor);
}

static const sxml_test_case xslt_value_template_test_cases[] = {
    {.name = "templates_build_string_template_asts",
     .function = test_templates_build_string_template_asts},
    {.name = "fixed_parts_are_expanded",
     .function = test_fixed_parts_are_expanded},
    {.name = "curly_bracket_errors", .function = test_curly_bracket_errors},
    {.name = "requests_are_checked", .function = test_requests_are_checked},
    {.name = "effective_values", .function = test_effective_values},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xslt_value_template",
            .cases = xslt_value_template_test_cases,
            .case_count = SXML_TEST_COUNT(xslt_value_template_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
