/*
 * Tests for src/xslt_pattern.c (src/xslt_pattern_internal.h, D118): the
 * pattern front end (AST shape, XTSE0340), match expressions evaluated
 * against every node of a fixed document as a table says (XSLT 4.0, 6.3.2)
 * and against items (6.3.3 to 6.3.9), default priorities (6.4), match
 * functions, and allocation-failure loops. Every case checks for leaked
 * cells and allocations.
 *
 * Golden file: tests/xpath_test.c.
 */
#include "evaluator_internal.h"
#include "ir_pass_internal.h"
#include "processor_internal.h"
#include "xslt_pattern_internal.h"

#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/types.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"
#include "test_runner.h"

#include <stdio.h>
#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* Nodes of the fixed document, the document node included. */
#define FIXTURE_NODE_LIMIT 64U
#define FIXTURE_LABEL_SIZE 16U
#define OUTCOME_SIZE 512U
/* Levels of parentheses in deep_patterns_use_no_c_stack. */
#define TEST_NESTING_DEPTH ((size_t)3000)

/*
 * The fixed document. Every element has an id; labels name nodes: an
 * element by its id, "/" for the document node, "id@name" for an
 * attribute, "id#t", "id#c", "id#pi" for a text node, comment or
 * processing instruction under the element id.
 */
#define FIXTURE_DOCUMENT_TEXT                                                  \
    "<doc id=\"d\">"                                                           \
    "<chapter id=\"c1\" xml:id=\"c1x\">"                                       \
    "<title id=\"t1\">One</title>"                                             \
    "<section id=\"s1\" type=\"a\"><para id=\"p1\">x</para><para id=\"p2\"/>" \
    "</section>"                                                               \
    "<section id=\"s2\"><para id=\"p3\"/><note id=\"n1\"/></section>"          \
    "</chapter>"                                                               \
    "<appendix id=\"a1\"><section id=\"s3\"><para id=\"p4\"/></section>"       \
    "</appendix>"                                                              \
    "<!--c--><?pi data?>"                                                      \
    "</doc>"

static const char fixture_nodes_query[] =
    "let $d := parse-xml('" FIXTURE_DOCUMENT_TEXT "') "
    "return $d/(descendant-or-self::node() | descendant::*/@*)";

static const char fixture_label_query[] =
    "if (. instance of document-node()) then '/' "
    "else if (. instance of element()) then string(@id) "
    "else if (. instance of attribute()) then ../@id || '@' || name() "
    "else if (. instance of text()) then ../@id || '#t' "
    "else if (. instance of comment()) then ../@id || '#c' "
    "else ../@id || '#pi'";

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
    /* The nodes in document order, borrowed from nodes. */
    sxml_sequence nodes;
    sxml_item node_items[FIXTURE_NODE_LIMIT];
    char labels[FIXTURE_NODE_LIMIT][FIXTURE_LABEL_SIZE];
    size_t node_count;
    /* The value of $v: s2 and p4. */
    sxml_sequence variable_value;
    sxml_name_identifier variable_name;
    sxml_namespace_binding namespace_binding;
    /* When not NULL, what patterns are compiled with (allocation-failure
       loops); otherwise the fixture's allocator. */
    sxml_counting_allocator *compiling_allocator;
} fixture;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static sxml_allocator compile_allocator(const fixture *test_fixture) {
    if (test_fixture->compiling_allocator != NULL) {
        return sxml_counting_allocator_as_allocator(test_fixture->compiling_allocator);
    }
    return fixture_allocator(test_fixture);
}

static sxml_item_heap *fixture_heap(const fixture *test_fixture) {
    return sxml_processor_get_item_heap(test_fixture->processor);
}

/* Evaluates public XPath text with an optional context item. */
typedef struct query_request {
    sxml_byte_view expression_text;
    bool has_context_item;
    sxml_item context_item;
} query_request;

static sxml_status fixture_query(const fixture *test_fixture, query_request request,
                                 sxml_sequence *result) {
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    sxml_xpath_expression *expression = NULL;
    sxml_status status = sxml_xpath_expression_create(
        fixture_allocator(test_fixture),
        &(sxml_xpath_expression_configuration){.processor = test_fixture->processor,
                                               .expression_text = request.expression_text,
                                               .source_name = TEXT("fixture.xpath"),
                                               .error_record = &error_record},
        &expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_xpath_expression_evaluate(
        expression,
        &(sxml_xpath_evaluation_request){
            .dynamic_context = {.has_context_value = request.has_context_item,
                                .context_value =
                                    request.has_context_item
                                        ? sxml_sequence_from_item(request.context_item)
                                        : sxml_sequence_empty()},
            .error_record = &error_record},
        result);
    sxml_xpath_expression_destroy(expression);
    return status;
}

/* Writes value by the adaptive method into the output buffer. */
static sxml_status fixture_write(const fixture *test_fixture, sxml_sequence value) {
    sxml_output_buffer_clear(test_fixture->output_buffer);
    return sxml_serialize_sequence(
        fixture_heap(test_fixture),
        &(sxml_serialization_request){
            .value = value,
            .serialization_parameters = {.serialization_method =
                                             SXML_SERIALIZATION_METHOD_ADAPTIVE,
                                         .has_item_separator = true,
                                         .item_separator = TEXT(" ")},
            .error_record = NULL},
        sxml_output_buffer_as_output_stream(test_fixture->output_buffer));
}

/* The label of node node_index, from its adaptive output without quotes. */
static sxml_status fixture_label(fixture *test_fixture, size_t node_index) {
    sxml_sequence label = sxml_sequence_empty();
    sxml_status status = fixture_query(
        test_fixture,
        (query_request){.expression_text = {.bytes = (const uint8_t *)fixture_label_query,
                                            .byte_count = sizeof fixture_label_query - 1U},
                        .has_context_item = true,
                        .context_item = test_fixture->node_items[node_index]},
        &label);
    if (status == SXML_STATUS_OK) {
        status = fixture_write(test_fixture, label);
    }
    sxml_sequence_release(label);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_byte_view written = sxml_output_buffer_get_bytes(test_fixture->output_buffer);
    if (written.byte_count < 2U || written.byte_count - 2U >= FIXTURE_LABEL_SIZE) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    memcpy(test_fixture->labels[node_index], written.bytes + 1, written.byte_count - 2U);
    test_fixture->labels[node_index][written.byte_count - 2U] = '\0';
    return SXML_STATUS_OK;
}

static size_t fixture_find_label(const fixture *test_fixture, const char *label) {
    for (size_t node_index = 0; node_index < test_fixture->node_count; node_index += 1) {
        if (strcmp(test_fixture->labels[node_index], label) == 0) {
            return node_index;
        }
    }
    return SIZE_MAX;
}

static sxml_status fixture_create(uint64_t failing_allocation_number, fixture *test_fixture) {
    memset(test_fixture, 0, sizeof(*test_fixture));
    test_fixture->nodes = sxml_sequence_empty();
    test_fixture->variable_value = sxml_sequence_empty();
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){
            .backing_allocator = sxml_default_allocator(),
            .failing_allocation_number = failing_allocation_number},
        &test_fixture->counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator = fixture_allocator(test_fixture);
    status = sxml_processor_create(
        allocator, &(sxml_processor_configuration){.allocator = allocator},
        &test_fixture->processor);
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(
            sxml_default_allocator(),
            &(sxml_output_buffer_configuration){.allocator = sxml_default_allocator(),
                                                .maximum_byte_count = 0},
            &test_fixture->output_buffer);
    }
    if (status == SXML_STATUS_OK) {
        status = fixture_query(
            test_fixture,
            (query_request){.expression_text = {.bytes = (const uint8_t *)fixture_nodes_query,
                                                .byte_count = sizeof fixture_nodes_query - 1U},
                            .has_context_item = false},
            &test_fixture->nodes);
    }
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(test_fixture->nodes);
    while (status == SXML_STATUS_OK) {
        sxml_iteration_step step = {.has_item = false};
        status = sxml_sequence_iterator_next(&iterator, NULL, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        if (test_fixture->node_count == FIXTURE_NODE_LIMIT) {
            status = SXML_STATUS_CAPACITY_EXCEEDED;
            break;
        }
        test_fixture->node_items[test_fixture->node_count] = step.item;
        test_fixture->node_count += 1;
    }
    for (size_t node_index = 0;
         status == SXML_STATUS_OK && node_index < test_fixture->node_count; node_index += 1) {
        status = fixture_label(test_fixture, node_index);
    }
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(test_fixture->processor);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            name_pool, &(sxml_name_request){.namespace_uri = {.index = 0}, .local_name = TEXT("v")},
            &test_fixture->variable_name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_prefix(
            name_pool, TEXT("p"), &test_fixture->namespace_binding.namespace_prefix);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_uri(
            name_pool, TEXT("urn:p"), &test_fixture->namespace_binding.namespace_uri);
    }
    if (status == SXML_STATUS_OK) {
        const size_t second_section = fixture_find_label(test_fixture, "s2");
        const size_t last_para = fixture_find_label(test_fixture, "p4");
        if (second_section == SIZE_MAX || last_para == SIZE_MAX) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        const sxml_item value_items[] = {test_fixture->node_items[second_section],
                                         test_fixture->node_items[last_para]};
        status = sxml_sequence_make_vector(
            fixture_heap(test_fixture), (sxml_item_view){.items = value_items, .item_count = 2},
            &test_fixture->variable_value);
    }
    return status;
}

/* Destroys the fixture; true when nothing leaked. */
static bool fixture_destroy(fixture *test_fixture) {
    bool has_no_leak = true;
    sxml_sequence_release(test_fixture->variable_value);
    sxml_sequence_release(test_fixture->nodes);
    if (test_fixture->processor != NULL) {
        sxml_item_heap *item_heap = fixture_heap(test_fixture);
        sxml_item_heap_clear_raised_error(item_heap);
        has_no_leak = sxml_item_heap_read_counts(item_heap).live_cell_count == 0;
    }
    sxml_processor_destroy(test_fixture->processor);
    sxml_output_buffer_destroy(test_fixture->output_buffer);
    if (test_fixture->counting_allocator != NULL) {
        has_no_leak = has_no_leak && sxml_counting_allocator_read_counts(
                                         test_fixture->counting_allocator)
                                             .live_allocation_count == 0;
        sxml_counting_allocator_destroy(test_fixture->counting_allocator);
    }
    return has_no_leak;
}

/* The static context of the patterns: prefix p, variable $v as node()*. */
typedef struct pattern_context {
    sxml_variable_declaration declaration;
    sxml_xpath_static_context static_context;
} pattern_context;

static void pattern_context_make(const fixture *test_fixture, pattern_context *context) {
    context->declaration = (sxml_variable_declaration){
        .variable_name = test_fixture->variable_name,
        .declared_type = {.item_type = sxml_builtin_type_identifier(SXML_TYPE_GENERIC_NODE),
                          .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}};
    context->static_context = (sxml_xpath_static_context){
        .item_heap = fixture_heap(test_fixture),
        .allocator = compile_allocator(test_fixture),
        .namespace_bindings = {.namespace_bindings = &test_fixture->namespace_binding,
                               .namespace_binding_count = 1},
        .variable_declarations = {.variable_declarations = &context->declaration,
                                  .variable_declaration_count = 1},
        .function_table = sxml_processor_get_function_table(test_fixture->processor),
        .static_base_uri = {.bytes = NULL, .byte_count = 0}};
}

/* A pattern's text and where its errors go (may be NULL). */
typedef struct pattern_source {
    sxml_byte_view text;
    sxml_error_record *error_record;
} pattern_source;

static sxml_status build_pattern(const fixture *test_fixture, pattern_source source,
                                 sxml_ast **ast) {
    const sxml_byte_view text = source.text;
    sxml_error_record *error_record = source.error_record;
    return sxml_xslt_pattern_build_ast(
        &(sxml_ast_build_request){.expression_text = text,
                                  .source_name = TEXT("pattern"),
                                  .allocator = compile_allocator(test_fixture),
                                  .maximum_expression_depth = 0,
                                  .error_record = error_record},
        ast);
}

/* A pattern compiled into a sealed unit whose main function is its match
   expression. */
typedef struct compiled_pattern {
    sxml_ir_unit *unit;
    sxml_error_record error_record;
} compiled_pattern;

static sxml_status compile_pattern(const fixture *test_fixture, sxml_byte_view text,
                                   compiled_pattern *compiled) {
    memset(&compiled->error_record, 0, sizeof(compiled->error_record));
    compiled->unit = NULL;
    pattern_context context;
    pattern_context_make(test_fixture, &context);
    sxml_ast *pattern = NULL;
    sxml_ast *match_expression = NULL;
    sxml_status status = build_pattern(test_fixture, (pattern_source){.text = text, .error_record = &compiled->error_record}, &pattern);
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_pattern_build_match_expression(
            &(sxml_xslt_pattern_match_expression_request){
                .pattern_ast = pattern,
                .static_context = &context.static_context,
                .allocator = compile_allocator(test_fixture),
                .error_record = &compiled->error_record},
            &match_expression);
    }
    sxml_ir_unit *unit = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xpath_lower(
            &(sxml_xpath_lowering_request){.ast = match_expression,
                                           .static_context = &context.static_context,
                                           .maximum_expression_count = 0,
                                           .maximum_expression_depth = 0,
                                           .error_record = &compiled->error_record},
            &unit);
    }
    sxml_ast_destroy(match_expression);
    sxml_ast_destroy(pattern);
    sxml_diagnostic diagnostic = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_run_passes(&(sxml_ir_pass_list_request){
            .pass_request = {.unit = unit,
                             .type_registry = sxml_item_heap_get_type_registry(
                                 fixture_heap(test_fixture)),
                             .error_record = &compiled->error_record,
                             .diagnostic = &diagnostic},
            .passes = sxml_ir_standard_passes,
            .verifies_after_each_pass = true});
        if (status != SXML_STATUS_OK) {
            sxml_ir_unit_release(unit);
            unit = NULL;
        }
    }
    if (status == SXML_STATUS_OK) {
        sxml_ir_unit_seal(unit);
        compiled->unit = unit;
    }
    return status;
}

/* Whether item matches: true, false, or an error (6.5: no match). */
/* A compiled pattern and the item to match. */
typedef struct match_request {
    const compiled_pattern *compiled;
    sxml_item item;
} match_request;

typedef struct match_result {
    bool matches;
} match_result;

static sxml_status evaluate_match(const fixture *test_fixture, match_request request,
                                  match_result *outcome) {
    const compiled_pattern *compiled = request.compiled;
    const sxml_item item = request.item;
    bool *matches = &outcome->matches;
    sxml_dynamic_context *dynamic_context = NULL;
    sxml_status status = sxml_dynamic_context_create(
        fixture_allocator(test_fixture),
        &(sxml_evaluator_configuration){
            .item_heap = fixture_heap(test_fixture),
            .allocator = fixture_allocator(test_fixture),
            .document_loader = {.state = NULL, .operations = NULL},
            .clock = {.state = NULL, .operations = NULL},
            .function_table = sxml_processor_get_function_table(test_fixture->processor),
            .maximum_evaluation_depth = 0,
            .maximum_frame_slot_count = 0,
            .maximum_evaluation_step_count = 0},
        &dynamic_context);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_evaluator_slot global_value = {.value = test_fixture->variable_value,
                                              .slot_state = SXML_EVALUATOR_SLOT_STATE_BOUND};
    sxml_error_record error_record;
    memset(&error_record, 0, sizeof(error_record));
    sxml_sequence result = sxml_sequence_empty();
    status = sxml_evaluator_evaluate(
        dynamic_context,
        &(sxml_evaluation_request){.unit = compiled->unit,
                                   .has_context_value = true,
                                   .context_value = sxml_sequence_from_item(item),
                                   .global_values = {.slots = &global_value, .slot_count = 1},
                                   .static_base_uri = {.bytes = NULL, .byte_count = 0},
                                   .error_record = &error_record},
        &result);
    sxml_dynamic_context_destroy(dynamic_context);
    if (status == SXML_STATUS_DYNAMIC_ERROR || status == SXML_STATUS_TYPE_ERROR) {
        sxml_item_heap_clear_raised_error(fixture_heap(test_fixture));
        *matches = false;
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item value = {.representation = 0};
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof(datum));
    status = sxml_sequence_get_item(result, (sxml_item_request){.item_index = 0}, &value);
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(value, &datum);
    }
    if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_BOOLEAN) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        *matches = datum.datum_details.boolean;
    }
    sxml_sequence_release(result);
    return status;
}

/* The labels of the matching nodes, in document order, space separated. */
typedef struct match_outcome {
    sxml_status status;
    char labels[OUTCOME_SIZE];
    sxml_byte_view error_code;
} match_outcome;

static void outcome_append(match_outcome *outcome, const char *label) {
    const size_t used = strlen(outcome->labels);
    const size_t label_length = strlen(label);
    if (used + label_length + 2U >= OUTCOME_SIZE) {
        outcome->status = SXML_STATUS_CAPACITY_EXCEEDED;
        return;
    }
    if (used > 0) {
        outcome->labels[used] = ' ';
        memcpy(outcome->labels + used + 1, label, label_length + 1U);
    } else {
        memcpy(outcome->labels, label, label_length + 1U);
    }
}

static void match_document(const fixture *test_fixture, sxml_byte_view pattern_text,
                           match_outcome *outcome) {
    memset(outcome, 0, sizeof(*outcome));
    compiled_pattern compiled;
    outcome->status = compile_pattern(test_fixture, pattern_text, &compiled);
    if (outcome->status != SXML_STATUS_OK) {
        outcome->error_code = compiled.error_record.code.local_name;
        return;
    }
    for (size_t node_index = 0;
         outcome->status == SXML_STATUS_OK && node_index < test_fixture->node_count;
         node_index += 1) {
        match_result result = {.matches = false};
        outcome->status = evaluate_match(
            test_fixture,
            (match_request){.compiled = &compiled, .item = test_fixture->node_items[node_index]},
            &result);
        if (outcome->status == SXML_STATUS_OK && result.matches) {
            outcome_append(outcome, test_fixture->labels[node_index]);
        }
    }
    sxml_ir_unit_release(compiled.unit);
}

/* ------------------------------------------------------ Document table */

typedef struct document_case {
    const char *pattern_text;
    /* The labels of the matching nodes in document order. */
    const char *expected_labels;
} document_case;

/* XSLT 4.0, 6.3.2 and its examples (6.3.2.1), with the fixed document. */
static const document_case document_cases[] = {
    {"para", "p1 p2 p3 p4"},
    {"*", "d c1 t1 s1 p1 p2 s2 p3 n1 a1 s3 p4"},
    {"section/para", "p1 p2 p3 p4"},
    {"chapter//para", "p1 p2 p3"},
    {"appendix//para", "p4"},
    {"para[1]", "p1 p3 p4"},
    {"para[last()]", "p2 p3 p4"},
    {"section[para][2]", "s2"},
    {"@id", "d@id c1@id t1@id s1@id p1@id p2@id s2@id p3@id n1@id a1@id s3@id p4@id"},
    {"section/@type", "s1@type"},
    {"@xml:id", "c1@xml:id"},
    {"/", "/"},
    {"/doc", "d"},
    {"/*/chapter", "c1"},
    {"//section", "s1 s2 s3"},
    {"text()", "t1#t p1#t"},
    {"comment() | processing-instruction()", "d#c d#pi"},
    {"processing-instruction('pi')", "d#pi"},
    {"document-node()", "/"},
    {"document-node(element(doc))", "/"},
    {"element(para)", "p1 p2 p3 p4"},
    {"attribute(type)", "s1@type"},
    {"chapter|appendix", "c1 a1"},
    {"child::(chapter|appendix)", "c1 a1"},
    {"section[@type = 'a']//para", "p1 p2"},
    {"$v", "s2 p4"},
    {"$v//para", "p3"},
    {"$v/note", "n1"},
    {"$v[2]", "p4"},
    {"root()", "/"},
    {"descendant::para", "p1 p2 p3 p4"},
    {"self::section", "s1 s2 s3"},
    {"* except section", "d c1 t1 p1 p2 p3 n1 a1 p4"},
    /* 6.3.2.2: $S//(para except appendix//para) selects p4 for $S = s3. */
    {"para except appendix//para", "p1 p2 p3 p4"},
    {"section intersect *[para]", "s1 s2 s3"},
    {"(section|title)[1]", "t1 s3"},
    {"node()[2]", "s1 p2 n1 a1"},
    {"section/para[1]", "p1 p3 p4"},
    {"chapter/(section|title)", "t1 s1 s2"},
    {"doc/chapter/section/(para|note)", "p1 p2 p3 n1"},
    {"*:para", "p1 p2 p3 p4"},
    {"section[2]/para", "p3"},
    {"//para[. = 'x']", "p1"},
    {"appendix/descendant::para", "p4"},
    {"chapter/descendant-or-self::section", "s1 s2"},
    {"x/(descendant::para except child::para)", ""},
    {"chapter[@xml:id]/section[last()]/*", "p3 n1"},
    {"p:para", ""},
    {".[. instance of element(para)]", "p1 p2 p3 p4"},
    {"root()/doc", "d"},
    {"root()//para[1]", "p1 p3 p4"},
};

static void test_patterns_match_the_document_as_the_table_says(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, test_fixture.node_count == 31U);
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(document_cases);
         case_index += 1) {
        const document_case *test = &document_cases[case_index];
        match_outcome outcome;
        match_document(&test_fixture,
                       (sxml_byte_view){.bytes = (const uint8_t *)test->pattern_text,
                                        .byte_count = strlen(test->pattern_text)},
                       &outcome);
        const bool passed =
            outcome.status == SXML_STATUS_OK && strcmp(outcome.labels, test->expected_labels) == 0;
        SXML_TEST_EXPECT(state, passed);
        if (!passed) {
            (void)fprintf(stderr, "pattern %s: status %d, got \"%s\"\n", test->pattern_text,
                          (int)outcome.status, outcome.labels);
        }
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* --------------------------------------------------------- Item cases */

typedef struct item_case {
    const char *pattern_text;
    /* An XPath expression giving the item. */
    const char *item_text;
    bool matches;
} item_case;

/* XSLT 4.0, 6.3.3 to 6.3.9. */
static const item_case item_cases[] = {
    {".", "1", true},
    {".", "map{}", true},
    {"atomic(xs:integer)", "1", true},
    {"atomic(xs:integer)", "'1'", false},
    {"atomic(xs:integer)", "1.5", false},
    {"atomic(xs:decimal)", "1", true},
    {"atomic(xs:integer)[. gt 0]", "5", true},
    {"atomic(xs:integer)[. gt 0]", "-1", false},
    {"atomic(xs:string | xs:integer)", "'a'", true},
    {"atomic(xs:string | xs:integer)", "2.0", false},
    {".[. instance of xs:string]", "'a'", true},
    {".[starts-with(., '$')]", "'$x'", true},
    {".[starts-with(., '$')]", "map{}", false},
    {".[1]", "'a'", true},
    {".[2]", "'a'", false},
    {"{}", "map{}", true},
    {"{}", "map{'a': 1}", false},
    {"{*}", "map{'a': 1}", true},
    {"{*}", "[1]", false},
    {"{'a', 'b'}", "map{'a': 1, 'b': 2}", true},
    {"{'a', 'b'}", "map{'a': 1}", false},
    {"{'a', 'b'}", "map{'a': 1, 'b': 2, 'c': 3}", false},
    {"{'a', *}", "map{'a': 1, 'c': 3}", true},
    {"{'a': atomic(xs:integer), *}", "map{'a': 1, 'z': 0}", true},
    {"{'a': atomic(xs:integer), *}", "map{'a': 'x'}", false},
    {"{'a': atomic(xs:integer)+}", "map{'a': (1, 2)}", true},
    {"{'a': atomic(xs:integer)+}", "map{'a': ()}", false},
    {"{'a': atomic(xs:integer)?}", "map{'a': ()}", true},
    {"{'a': empty-sequence()}", "map{'a': ()}", true},
    {"{-1, true()}", "map{-1: 0, true(): 0}", true},
    {"{'x', 'y'}[?x = ?y]", "map{'x': 1, 'y': 1}", true},
    {"{'x', 'y'}[?x = ?y]", "map{'x': 1, 'y': 2}", false},
    {"array(atomic(xs:integer))", "[1, 2]", true},
    {"array(atomic(xs:integer))", "[]", true},
    {"array(atomic(xs:integer))", "[1, 'a']", false},
    {"array(atomic(xs:integer))", "[(1, 2)]", false},
    {"array(.*)", "[(1, 2), ()]", true},
    {"array(empty-sequence())", "[()]", true},
    {"array(empty-sequence())", "[1]", false},
    {"array({*})", "[map{}, map{'a': 1}]", true},
    {"array(.)[array:size(.) = 3]", "[1, 2, 3]", true},
    {"para", "'para'", false},
    {"$v", "1", false},
};

static void test_simple_patterns_match_items(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(item_cases); case_index += 1) {
        const item_case *test = &item_cases[case_index];
        sxml_sequence item_value = sxml_sequence_empty();
        sxml_item item = {.representation = 0};
        compiled_pattern compiled;
        match_result result = {.matches = !test->matches};
        sxml_status status = fixture_query(
            &test_fixture,
            (query_request){.expression_text = {.bytes = (const uint8_t *)test->item_text,
                                                .byte_count = strlen(test->item_text)},
                            .has_context_item = false},
            &item_value);
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_get_item(item_value, (sxml_item_request){.item_index = 0},
                                            &item);
        }
        if (status == SXML_STATUS_OK) {
            status = compile_pattern(&test_fixture,
                                     (sxml_byte_view){.bytes = (const uint8_t *)test->pattern_text,
                                                      .byte_count = strlen(test->pattern_text)},
                                     &compiled);
        }
        if (status == SXML_STATUS_OK) {
            status = evaluate_match(&test_fixture,
                                    (match_request){.compiled = &compiled, .item = item}, &result);
            sxml_ir_unit_release(compiled.unit);
        }
        sxml_sequence_release(item_value);
        const bool passed = status == SXML_STATUS_OK && result.matches == test->matches;
        SXML_TEST_EXPECT(state, passed);
        if (!passed) {
            (void)fprintf(stderr, "pattern %s on %s: status %d\n", test->pattern_text,
                          test->item_text, (int)status);
        }
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------------ Priority table */

typedef struct priority_case {
    const char *pattern_text;
    double priority;
} priority_case;

/* XSLT 4.0, 6.4 and its tables and examples. */
static const priority_case priority_cases[] = {
    {"para", 0.0},
    {"*", -0.5},
    {"@*", -0.5},
    {"@id", 0.0},
    {"p:*", -0.25},
    {"*:para", -0.25},
    {"Q{urn:p}*", -0.25},
    {"/", -0.5},
    {"section/para", 0.5},
    {"para[1]", 0.5},
    {"text()", -0.5},
    {"node()", -0.5},
    {"processing-instruction()", -0.5},
    {"processing-instruction('pi')", 0.0},
    {"element()", -0.5},
    {"element(*)", -0.5},
    {"element(para)", 0.0},
    {"element(p:*)", -0.25},
    {"element(*, xs:integer)", 0.0},
    {"element(p:*, xs:integer)", 0.125},
    {"element(para, xs:anyType)", 0.25},
    {"element(a|b)", 0.0},
    {"element(p:*|*:q, xs:integer)", 0.125},
    {"attribute(*:x)", -0.25},
    {"attribute(x, xs:integer)", 0.25},
    {"schema-element(x)", 0.25},
    {"document-node()", -0.5},
    {"document-node(element(doc))", 0.0},
    {"child::para", 0.0},
    {"child::(a|*)", -0.5},
    {"a | b/c", 0.0},
    {"a | *", -0.5},
    {"(para)", 0.0},
    {"a except b", 0.0},
    {"$v", 0.5},
    {"id('a')", 0.5},
    {"//para", 0.5},
    {".", -1.0},
    {".[1]", 0.5},
    {"atomic(xs:anyAtomicType)", -0.5},
    {"atomic(xs:decimal)", 0.0},
    {"atomic(xs:untypedAtomic)", 0.0},
    {"atomic(xs:integer)", 0.25},
    {"atomic(xs:long)", 0.375},
    {"atomic(xs:int)", 0.4375},
    {"atomic(xs:integer | xs:double)", 0.0},
    {"atomic(xs:numeric)", 0.0},
    {"{}", 0.5},
    {"{*}", -0.25},
    {"{'a'}", 0.0},
    {"{'a', *}", 0.0},
    {"{'a': .}", 0.25},
    {"array(atomic(xs:integer))", 0.25},
    {"array(atomic(xs:integer)*)", 0.25},
    {"array(empty-sequence())", 0.5},
    {"array(.)[1]", 0.5},
    {"jnode(*, array(atomic(xs:integer)))", 0.25},
    {"jnode('dimensions', array(atomic(xs:integer)))", 0.5},
    {"jnode('dimensions', array(atomic(xs:integer)))[1]", 1.0},
    {"jnode(*, empty-sequence())", 0.5},
};

static void test_default_priorities_follow_the_tables(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    pattern_context context;
    pattern_context_make(&test_fixture, &context);
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(priority_cases);
         case_index += 1) {
        const priority_case *test = &priority_cases[case_index];
        sxml_ast *pattern = NULL;
        double priority = 2.0;
        sxml_error_record error_record;
        memset(&error_record, 0, sizeof(error_record));
        sxml_status status =
            build_pattern(&test_fixture, (pattern_source){.text = (sxml_byte_view){.bytes = (const uint8_t *)test->pattern_text,
                                           .byte_count = strlen(test->pattern_text)}, .error_record = &error_record}, &pattern);
        if (status == SXML_STATUS_OK) {
            status = sxml_xslt_pattern_compute_default_priority(
                &(sxml_xslt_pattern_priority_request){.pattern_ast = pattern,
                                                      .static_context = &context.static_context,
                                                      .is_any_namespace = false,
                                                      .error_record = &error_record},
                &priority);
        }
        sxml_ast_destroy(pattern);
        const bool passed = status == SXML_STATUS_OK && priority == test->priority;
        SXML_TEST_EXPECT(state, passed);
        if (!passed) {
            (void)fprintf(stderr, "pattern %s: status %d, priority %g\n", test->pattern_text,
                          (int)status, priority);
        }
    }
    /* 6.4.1: with ##any, para counts as *:para. */
    sxml_ast *pattern = NULL;
    double priority = 2.0;
    SXML_TEST_EXPECT(state, build_pattern(&test_fixture, (pattern_source){.text = TEXT("para"), .error_record = NULL}, &pattern) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, sxml_xslt_pattern_compute_default_priority(
                                &(sxml_xslt_pattern_priority_request){
                                    .pattern_ast = pattern,
                                    .static_context = &context.static_context,
                                    .is_any_namespace = true,
                                    .error_record = NULL},
                                &priority) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, priority == -0.25);
    sxml_ast_destroy(pattern);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ---------------------------------------------------------- Front end */

static const sxml_ast_node *child_of(const sxml_ast *ast, sxml_ast_child_query query,
                                     sxml_ast_node_index *child) {
    *child = sxml_ast_find_child(ast, query);
    return sxml_ast_get_node(ast, *child);
}

/* The AST of src/xslt_pattern_internal.h's example. */
static void test_front_end_builds_the_pattern_ast(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    sxml_ast *ast = NULL;
    SXML_TEST_REQUIRE(state, build_pattern(&test_fixture, (pattern_source){.text = TEXT("chapter/section[1] | @id"), .error_record = NULL}, &ast) == SXML_STATUS_OK);
    sxml_ast_node_index node = ast->root;
    SXML_TEST_EXPECT(state, sxml_ast_get_node(ast, node)->kind == SXML_AST_KIND_PATTERN);
    sxml_ast_node_index union_node = {0};
    const sxml_ast_node *union_pattern =
        child_of(ast, (sxml_ast_child_query){.parent = node, .field = SXML_AST_FIELD_PATTERN}, &union_node);
    SXML_TEST_EXPECT(state, union_pattern->kind == SXML_AST_KIND_UNION_PATTERN);
    SXML_TEST_EXPECT(state, union_pattern->detail.operator== SXML_AST_OPERATOR_UNION);
    sxml_ast_node_index path_node = {0};
    const sxml_ast_node *path = child_of(ast, (sxml_ast_child_query){.parent = union_node, .field = SXML_AST_FIELD_LEFT}, &path_node);
    SXML_TEST_EXPECT(state, path->kind == SXML_AST_KIND_RELATIVE_PATH_PATTERN);
    SXML_TEST_EXPECT(state, path->detail.operator== SXML_AST_OPERATOR_SLASH);
    sxml_ast_node_index step_node = {0};
    const sxml_ast_node *step = child_of(ast, (sxml_ast_child_query){.parent = path_node, .field = SXML_AST_FIELD_RIGHT}, &step_node);
    SXML_TEST_EXPECT(state, step->kind == SXML_AST_KIND_AXIS_STEP_PATTERN);
    SXML_TEST_EXPECT(state, step->detail.axis == SXML_AST_AXIS_NONE);
    sxml_ast_node_index test_node = {0};
    SXML_TEST_EXPECT(state, child_of(ast, (sxml_ast_child_query){.parent = step_node, .field = SXML_AST_FIELD_TEST}, &test_node)->kind ==
                                SXML_AST_KIND_NCNAME);
    sxml_ast_node_index predicate = {0};
    SXML_TEST_EXPECT(state, child_of(ast, (sxml_ast_child_query){.parent = step_node, .field = SXML_AST_FIELD_PREDICATE}, &predicate)->kind ==
                                SXML_AST_KIND_PREDICATE);
    sxml_ast_node_index attribute_node = {0};
    const sxml_ast_node *attribute =
        child_of(ast, (sxml_ast_child_query){.parent = union_node, .field = SXML_AST_FIELD_RIGHT}, &attribute_node);
    SXML_TEST_EXPECT(state, attribute->kind == SXML_AST_KIND_AXIS_STEP_PATTERN);
    SXML_TEST_EXPECT(state, attribute->detail.axis == SXML_AST_AXIS_ATTRIBUTE);
    SXML_TEST_EXPECT(state, (attribute->flags & (uint32_t)SXML_AST_FLAG_ABBREVIATED) != 0);
    sxml_ast_destroy(ast);
    /* A map pattern's "*" is the flag OPEN; an occurrence indicator the
       sequence pattern's detail. */
    SXML_TEST_REQUIRE(state, build_pattern(&test_fixture, (pattern_source){.text = TEXT("{'a': .+, *}"), .error_record = NULL}, &ast) ==
                                 SXML_STATUS_OK);
    sxml_ast_node_index simple_node = {0};
    child_of(ast, (sxml_ast_child_query){.parent = ast->root, .field = SXML_AST_FIELD_PATTERN}, &simple_node);
    sxml_ast_node_index map_node = {0};
    const sxml_ast_node *map = child_of(ast, (sxml_ast_child_query){.parent = simple_node, .field = SXML_AST_FIELD_ITEM}, &map_node);
    SXML_TEST_EXPECT(state, map->kind == SXML_AST_KIND_MAP_PATTERN);
    SXML_TEST_EXPECT(state, (map->flags & (uint32_t)SXML_AST_FLAG_OPEN) != 0);
    sxml_ast_node_index entry_node = {0};
    child_of(ast, (sxml_ast_child_query){.parent = map_node, .field = SXML_AST_FIELD_ENTRY}, &entry_node);
    sxml_ast_node_index sequence_node = {0};
    const sxml_ast_node *sequence = child_of(ast, (sxml_ast_child_query){.parent = entry_node, .field = SXML_AST_FIELD_VALUE}, &sequence_node);
    SXML_TEST_EXPECT(state, sequence->kind == SXML_AST_KIND_SEQUENCE_PATTERN);
    SXML_TEST_EXPECT(state, sequence->detail.occurrence == SXML_OCCURRENCE_ONE_OR_MORE);
    sxml_ast_destroy(ast);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

typedef struct error_case {
    const char *pattern_text;
    const char *error_code;
} error_case;

/* XTSE0340 (6.3.1, 6.3.2). */
static const error_case error_cases[] = {
    {"para[", "XTSE0340"},
    {"1 + 2", "XTSE0340"},
    {"..", "XTSE0340"},
    {"ancestor::para", "XTSE0340"},
    {"para/..", "XTSE0340"},
    {"foo(1)", "XTSE0340"},
    {"id()", "XTSE0340"},
    {"root(.)", "XTSE0340"},
    {"Q{urn:x}doc('a')", "XTSE0340"},
    {"p:doc('a')", "XTSE0340"},
    {"$v + 1", "XTSE0340"},
    {"10div 3", "XTSE0340"},
    {"para[$undeclared]", "XPST0008"},
    {"q:para", "XPST0081"},
};

static void test_invalid_patterns_raise_static_errors(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(error_cases); case_index += 1) {
        const error_case *test = &error_cases[case_index];
        match_outcome outcome;
        match_document(&test_fixture,
                       (sxml_byte_view){.bytes = (const uint8_t *)test->pattern_text,
                                        .byte_count = strlen(test->pattern_text)},
                       &outcome);
        const bool passed =
            outcome.status == SXML_STATUS_STATIC_ERROR &&
            sxml_byte_view_equals(outcome.error_code,
                                  (sxml_byte_view){.bytes = (const uint8_t *)test->error_code,
                                                   .byte_count = strlen(test->error_code)});
        SXML_TEST_EXPECT(state, passed);
        if (!passed) {
            (void)fprintf(stderr, "pattern %s: status %d, code %.*s\n", test->pattern_text,
                          (int)outcome.status, (int)outcome.error_code.byte_count,
                          (const char *)outcome.error_code.bytes);
        }
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* -------------------------------------------------------- Match function */

static void test_match_function_is_added_to_the_unit(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    pattern_context context;
    pattern_context_make(&test_fixture, &context);
    sxml_ir_unit *unit = NULL;
    sxml_ast *main_ast = NULL;
    sxml_ast *pattern = NULL;
    sxml_xpath_lowerer *lowerer = NULL;
    SXML_TEST_REQUIRE(state, sxml_xpath_build_ast(
                                 &(sxml_ast_build_request){.expression_text = TEXT("1"),
                                                           .source_name = TEXT("main"),
                                                           .allocator = fixture_allocator(&test_fixture)},
                                 &main_ast) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_xpath_lower(&(sxml_xpath_lowering_request){
                                                  .ast = main_ast,
                                                  .static_context = &context.static_context},
                                              &unit) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, build_pattern(&test_fixture, (pattern_source){.text = TEXT("b[@x]"), .error_record = NULL}, &pattern) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_xpath_lowerer_create(
                                 fixture_allocator(&test_fixture),
                                 &(sxml_xpath_lowerer_configuration){
                                     .static_context = &context.static_context,
                                     .unit = unit,
                                     .language = SXML_LANGUAGE_XSLT},
                                 &lowerer) == SXML_STATUS_OK);
    sxml_ir_function_index match_function = {0};
    SXML_TEST_EXPECT(state, sxml_xslt_pattern_lower_match_function(
                                lowerer,
                                &(sxml_xslt_pattern_match_function_request){
                                    .pattern_ast = pattern, .static_context = NULL},
                                &match_function) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, match_function.value == 2U);
    const sxml_ir_function *function = sxml_ir_unit_get_function(unit, match_function);
    SXML_TEST_EXPECT(state, function->parameter_count == 1U);
    SXML_TEST_EXPECT(state, function->body.value != 0U);
    sxml_diagnostic diagnostic = {0};
    SXML_TEST_EXPECT(state, sxml_ir_verify(&(sxml_ir_pass_request){
                                .unit = unit,
                                .type_registry = sxml_item_heap_get_type_registry(
                                    fixture_heap(&test_fixture)),
                                .error_record = NULL,
                                .diagnostic = &diagnostic}) == SXML_STATUS_OK);
    sxml_xpath_lowerer_destroy(lowerer);
    sxml_ast_destroy(pattern);
    sxml_ast_destroy(main_ast);
    sxml_ir_unit_release(unit);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* --------------------------------------------------- Allocation failures */

static void test_every_allocation_failure_is_clean(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    const sxml_byte_view pattern_text =
        TEXT("chapter[1]//section/(para|note)[2] | ({'a': array(atomic(xs:int))*}) | "
             "(* except para)[1] | $v/para | (jnode(*, .))");
    bool has_succeeded = false;
    for (uint64_t failing = 1; !has_succeeded && failing < 100000U; failing += 1) {
        SXML_TEST_REQUIRE(state, sxml_counting_allocator_create(
                                     sxml_default_allocator(),
                                     &(sxml_counting_allocator_configuration){
                                         .backing_allocator = sxml_default_allocator(),
                                         .failing_allocation_number = failing},
                                     &test_fixture.compiling_allocator) == SXML_STATUS_OK);
        compiled_pattern compiled;
        const sxml_status status = compile_pattern(&test_fixture, pattern_text, &compiled);
        if (status == SXML_STATUS_OK) {
            has_succeeded = true;
            sxml_ir_unit_release(compiled.unit);
        } else {
            SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY);
            if (status != SXML_STATUS_OUT_OF_MEMORY) {
                (void)fprintf(stderr, "allocation %llu: status %d %.*s\n",
                              (unsigned long long)failing, (int)status,
                              (int)compiled.error_record.code.local_name.byte_count,
                              (const char *)compiled.error_record.code.local_name.bytes);
                failing = 100000U;
            }
        }
        SXML_TEST_EXPECT(state, sxml_counting_allocator_read_counts(
                                    test_fixture.compiling_allocator)
                                        .live_allocation_count == 0);
        sxml_counting_allocator_destroy(test_fixture.compiling_allocator);
        test_fixture.compiling_allocator = NULL;
    }
    SXML_TEST_EXPECT(state, has_succeeded);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* A pattern nested deeply costs no C stack (D71). */
static void test_deep_patterns_use_no_c_stack(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    static char text[TEST_NESTING_DEPTH * 4U + 16U];
    size_t length = 0;
    for (size_t level = 0; level < TEST_NESTING_DEPTH; level += 1) {
        text[length] = '(';
        length += 1;
    }
    const char innermost[] = "para";
    for (size_t character = 0; character + 1U < sizeof innermost; character += 1) {
        text[length] = innermost[character];
        length += 1;
    }
    for (size_t level = 0; level < TEST_NESTING_DEPTH; level += 1) {
        text[length] = '|';
        text[length + 1U] = 'a';
        text[length + 2U] = ')';
        length += 3;
    }
    match_outcome outcome;
    match_document(&test_fixture, (sxml_byte_view){.bytes = (const uint8_t *)text, .byte_count = length},
                   &outcome);
    SXML_TEST_EXPECT(state, outcome.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, strcmp(outcome.labels, "p1 p2 p3 p4") == 0);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static const sxml_test_case xslt_pattern_test_cases[] = {
    {.name = "front_end_builds_the_pattern_ast",
     .function = test_front_end_builds_the_pattern_ast},
    {.name = "invalid_patterns_raise_static_errors",
     .function = test_invalid_patterns_raise_static_errors},
    {.name = "patterns_match_the_document_as_the_table_says",
     .function = test_patterns_match_the_document_as_the_table_says},
    {.name = "simple_patterns_match_items", .function = test_simple_patterns_match_items},
    {.name = "default_priorities_follow_the_tables",
     .function = test_default_priorities_follow_the_tables},
    {.name = "match_function_is_added_to_the_unit",
     .function = test_match_function_is_added_to_the_unit},
    {.name = "every_allocation_failure_is_clean",
     .function = test_every_allocation_failure_is_clean},
    {.name = "deep_patterns_use_no_c_stack", .function = test_deep_patterns_use_no_c_stack},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xslt_pattern",
            .cases = xslt_pattern_test_cases,
            .case_count = SXML_TEST_COUNT(xslt_pattern_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
