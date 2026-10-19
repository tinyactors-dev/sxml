/*
 * Tests for the XQuery Update Facility 3.0 through include/sxml/xquery.h
 * alone (docs/DECISIONS.md D187): copy-modify expressions with every kind
 * of update primitive, the static rules on updating expressions, the
 * dynamic errors of the update expressions and of applying, and an
 * updating query whose changed trees reach the update sink. Every case
 * checks for leaked cells and allocations.
 *
 * Golden file: tests/xquery_test.c.
 */
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/xdm.h"
#include "sxml/xquery.h"
#include "test_runner.h"

#include <string.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* ---------------------------------------------------------------- Fixture */

typedef struct fixture {
    sxml_counting_allocator *counting_allocator;
    sxml_processor *processor;
    sxml_output_buffer *output_buffer;
} fixture;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static sxml_status fixture_create(uint64_t failing_allocation_number, fixture *test_fixture) {
    *test_fixture = (fixture){.counting_allocator = NULL, .processor = NULL, .output_buffer = NULL};
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number =
                                                     failing_allocation_number},
        &test_fixture->counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator = fixture_allocator(test_fixture);
    status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .resource_resolver = {.state = NULL, .operations = NULL}},
        &test_fixture->processor);
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(
            sxml_default_allocator(),
            &(sxml_output_buffer_configuration){.allocator = sxml_default_allocator(),
                                                .maximum_byte_count = 0},
            &test_fixture->output_buffer);
    }
    return status;
}

/* Destroys the fixture; true when nothing leaked. */
static bool fixture_destroy(fixture *test_fixture) {
    bool has_no_leak = true;
    if (test_fixture->processor != NULL) {
        sxml_item_heap *item_heap = sxml_processor_get_item_heap(test_fixture->processor);
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

/* --------------------------------------------------------------- Running */

/* The trees an updating query changed, serialized one after another. */
typedef struct tree_collector {
    const fixture *test_fixture;
    size_t tree_count;
} tree_collector;

static sxml_status collect_tree(void *state, const sxml_xquery_updated_tree *updated_tree) {
    tree_collector *collector = state;
    collector->tree_count += 1;
    sxml_serialization_parameters parameters;
    memset(&parameters, 0, sizeof parameters);
    parameters.serialization_method = SXML_SERIALIZATION_METHOD_XML;
    return sxml_serialize_sequence(
        sxml_processor_get_item_heap(collector->test_fixture->processor),
        &(sxml_serialization_request){.value = sxml_sequence_from_item(updated_tree->updated_root),
                                      .serialization_parameters = parameters,
                                      .error_record = NULL},
        sxml_output_buffer_as_output_stream(collector->test_fixture->output_buffer));
}

static const sxml_xquery_update_sink_operations collector_operations = {
    .receive_updated_tree = collect_tree};

/* One query to run: its text; out the error record, whether it is
   updating and how many trees it changed. */
typedef struct query_run {
    sxml_byte_view expression_text;
    sxml_error_record error_record;
    bool is_updating;
    size_t updated_tree_count;
} query_run;

/* Compiles and evaluates run->expression_text and serializes the result,
   then the trees it changed, into the fixture's output buffer (cleared
   first). */
static sxml_status run_query(const fixture *test_fixture, query_run *run) {
    memset(&run->error_record, 0, sizeof(run->error_record));
    sxml_output_buffer_clear(test_fixture->output_buffer);
    sxml_xquery_query *query = NULL;
    sxml_status status = sxml_xquery_query_create(
        fixture_allocator(test_fixture),
        &(sxml_xquery_query_configuration){.processor = test_fixture->processor,
                                           .expression_text = run->expression_text,
                                           .source_name = TEXT("test.xq"),
                                           .error_record = &run->error_record},
        &query);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    run->is_updating = sxml_xquery_query_is_updating(query);
    tree_collector collector = {.test_fixture = test_fixture, .tree_count = 0};
    sxml_sequence result = sxml_sequence_empty();
    status = sxml_xquery_query_evaluate(
        query,
        &(sxml_xquery_evaluation_request){
            .error_record = &run->error_record,
            .update_sink = {.state = &collector, .operations = &collector_operations}},
        &result);
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(
            sxml_processor_get_item_heap(test_fixture->processor),
            &(sxml_serialization_request){
                .value = result,
                .serialization_parameters = sxml_xquery_query_get_serialization_parameters(query),
                .error_record = &run->error_record},
            sxml_output_buffer_as_output_stream(test_fixture->output_buffer));
        sxml_sequence_release(result);
    }
    run->updated_tree_count = collector.tree_count;
    sxml_xquery_query_destroy(query);
    return status;
}

static bool output_is(const fixture *test_fixture, sxml_byte_view expected) {
    return sxml_byte_view_equals(sxml_output_buffer_get_bytes(test_fixture->output_buffer),
                                 expected);
}

/* ---------------------------------------------------------- Query cases */

/* A query and its serialized output, or the error code it raises. */
typedef struct query_case {
    const char *case_name;
    sxml_byte_view expression_text;
    sxml_byte_view expected_output;
    sxml_byte_view expected_error_code;
} query_case;

#define QUERY_VALUE(name, text, output)                                                   \
    {.case_name = (name),                                                                \
     .expression_text = SXML_BYTE_VIEW_INITIALIZER(text),                                \
     .expected_output = SXML_BYTE_VIEW_INITIALIZER(output),                              \
     .expected_error_code = {.bytes = NULL, .byte_count = 0}}

#define QUERY_ERROR(name, text, code)                                                     \
    {.case_name = (name),                                                                \
     .expression_text = SXML_BYTE_VIEW_INITIALIZER(text),                                \
     .expected_output = {.bytes = NULL, .byte_count = 0},                                \
     .expected_error_code = SXML_BYTE_VIEW_INITIALIZER(code)}

static const query_case query_cases[] = {
    /* 5.4.1: every insert position; "into" goes after the children. */
    QUERY_VALUE("insert-positions",
                "copy $c := <a><m/></a> modify (insert node <f/> as first into $c, "
                "insert node <l/> as last into $c, insert node <i/> into $c, "
                "insert node <b/> before $c/m, insert node <x/> after $c/m) return $c",
                "<a><f/><b/><m/><x/><i/><l/></a>"),
    /* Atomic items become text with spaces; attributes go to the element. */
    QUERY_VALUE("insert-content",
                "copy $c := <a/> modify insert nodes (attribute n {1}, 'x', 2, <e/>) into $c "
                "return $c",
                "<a n=\"1\">x 2<e/></a>"),
    QUERY_VALUE("insert-document-children",
                "copy $c := <a/> modify insert node document { <d/>, 't' } into $c return $c",
                "<a><d/>t</a>"),
    /* 5.4.2 and 5.4.3: delete, replace node, replace value of. */
    QUERY_VALUE("delete-and-replace",
                "copy $c := <a x='1' y='2'><b/><c>old</c><d/></a> modify (delete node $c/b, "
                "delete node $c/@y, replace value of node $c/c with ('n', 'ew'), "
                "replace node $c/d with <e/>, replace value of node $c/@x with 9) return $c",
                "<a x=\"9\"><c>n ew</c><e/></a>"),
    QUERY_VALUE("replace-attribute",
                "copy $c := <a x='1'/> modify replace node $c/@x with (attribute y {2}, "
                "attribute z {3}) return $c",
                "<a y=\"2\" z=\"3\"/>"),
    QUERY_VALUE("replace-element-content",
                "copy $c := <a><b/>text</a> modify replace value of node $c with 'all' return $c",
                "<a>all</a>"),
    /* 5.4.4: elements, attributes, processing instructions. */
    QUERY_VALUE("rename",
                "copy $c := <a x='1'><?p v?></a> modify (rename node $c as 'b', rename node "
                "$c/@x as QName('urn:n', 'n:y'), rename node $c/processing-instruction() as 'q') "
                "return $c",
                "<b xmlns:n=\"urn:n\" n:y=\"1\"><?q v?></b>"),
    /* An insert and a delete of one node: the node goes, its siblings stay
       (3.2.2's order). */
    QUERY_VALUE("insert-around-deleted",
                "copy $c := <a><b/></a> modify (delete node $c/b, insert node <x/> before $c/b, "
                "insert node <y/> into $c/b) return $c",
                "<a><x/></a>"),
    QUERY_VALUE("two-copies",
                "copy $a := <a/>, $b := <b/> modify (insert node <x/> into $a, "
                "insert node <y/> into $b) return ($a, $b)",
                "<a><x/></a><b><y/></b>"),
    QUERY_VALUE("updating-function",
                "declare updating function local:f($n) { delete node $n }; "
                "declare %updating function local:g($n) { insert node <g/> into $n }; "
                "copy $c := <a><b/></a> modify (local:f($c/b), local:g($c)) return $c",
                "<a><g/></a>"),
    QUERY_VALUE("vacuous-modify", "copy $c := <a/> modify () return $c", "<a/>"),
    QUERY_VALUE("conditional-update",
                "copy $c := <a><b/><c/></a> modify (for $n in $c/* return if (name($n) = 'b') "
                "then delete node $n else ()) return $c",
                "<a><c/></a>"),
    QUERY_VALUE("try-drops-updates",
                "copy $c := <a/> modify try { insert node <b/> into $c, error() } catch * "
                "{ insert node <c/> into $c } return $c",
                "<a><c/></a>"),
    /* 5.4.6: one copy per item, the modify clause with it as the focus. */
    QUERY_VALUE("transform-with",
                "(<a x='1'><b/></a>, <c x='2'/>) transform with { delete node b, rename node @x "
                "as 'y' }",
                "<a y=\"1\"/><c y=\"2\"/>"),
    QUERY_VALUE("transform-with-empty", "<a/> transform with { }", "<a/>"),
    /* The static rules (2.2, 5.1 to 5.4). */
    QUERY_ERROR("updating-operand", "let $x := <a/> return (insert node <b/> into $x, 1)",
                "XUST0001"),
    QUERY_ERROR("updating-in-let", "let $x := delete node <a/> return 1", "XUST0001"),
    QUERY_ERROR("simple-modify", "copy $c := <a/> modify 1 return $c", "XUST0002"),
    QUERY_ERROR("simple-updating-body", "declare updating function local:f() { 1 }; 1",
                "XUST0002"),
    QUERY_ERROR("updating-simple-body",
                "declare function local:f($n) { delete node $n }; 1", "XUST0001"),
    QUERY_ERROR("updating-result-type",
                "declare updating function local:f() as empty-sequence() { () }; 1",
                "XUST0028"),
    QUERY_ERROR("updating-variable", "declare %updating variable $v := 1; $v", "XUST0032"),
    QUERY_ERROR("two-update-annotations",
                "declare %updating %simple function local:f() { () }; 1", "XUST0033"),
    QUERY_ERROR("revalidation-twice",
                "declare revalidation skip; declare revalidation skip; 1", "XUST0003"),
    QUERY_ERROR("revalidation-strict", "declare revalidation strict; 1", "XUST0026"),
    /* The dynamic errors of the expressions (5.4) and of applying (3.2.2). */
    QUERY_ERROR("empty-target", "copy $c := <a/> modify insert node <b/> into $c/x return $c",
                "XUDY0027"),
    QUERY_ERROR("into-attribute",
                "copy $c := <a x='1'/> modify insert node <b/> into $c/@x return $c", "XUTY0005"),
    QUERY_ERROR("before-root", "copy $c := <a/> modify insert node <b/> before $c return $c",
                "XUDY0029"),
    QUERY_ERROR("late-attribute",
                "copy $c := <a/> modify insert nodes (<b/>, attribute x {1}) into $c return $c",
                "XUTY0004"),
    QUERY_ERROR("replace-root", "copy $c := <a/> modify replace node $c with <b/> return $c",
                "XUDY0009"),
    QUERY_ERROR("rename-twice",
                "copy $c := <a/> modify (rename node $c as 'b', rename node $c as 'c') return $c",
                "XUDY0015"),
    QUERY_ERROR("replace-twice",
                "copy $c := <a><b/></a> modify (replace node $c/b with <x/>, replace node $c/b "
                "with <y/>) return $c",
                "XUDY0016"),
    QUERY_ERROR("replace-value-twice",
                "copy $c := <a/> modify (replace value of node $c with 'x', replace value of "
                "node $c with 'y') return $c",
                "XUDY0017"),
    QUERY_ERROR("not-a-copy",
                "let $d := <d/> return copy $c := <a/> modify insert node <b/> into $d return $c",
                "XUDY0014"),
    QUERY_ERROR("duplicate-attribute",
                "copy $c := <a x='1'/> modify insert node attribute x {2} into $c return $c",
                "XUDY0021"),
    QUERY_ERROR("copy-two-nodes", "copy $c := (<a/>, <b/>) modify () return $c", "XUTY0013"),
    QUERY_ERROR("rename-conflict",
                "copy $c := <p:a xmlns:p='urn:p'/> modify rename node $c as QName('urn:q', "
                "'p:b') return $c",
                "XUDY0023"),
};

static void test_queries_give_values_and_errors(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    for (size_t case_index = 0; case_index < sizeof(query_cases) / sizeof(query_cases[0]);
         case_index += 1) {
        const query_case *current = &query_cases[case_index];
        query_run run = {.expression_text = current->expression_text};
        const sxml_status status = run_query(&test_fixture, &run);
        const bool passed =
            current->expected_error_code.byte_count > 0
                ? status != SXML_STATUS_OK &&
                      sxml_byte_view_equals(run.error_record.code.local_name,
                                            current->expected_error_code)
                : status == SXML_STATUS_OK && !run.is_updating &&
                      output_is(&test_fixture, current->expected_output);
        (void)sxml_test_record_check(state, (sxml_test_check){.passed = passed,
                                                              .expression = current->case_name,
                                                              .file_name = __FILE__,
                                                              .line_number = __LINE__});
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------------ Updating queries */

static void test_updating_query_hands_trees_to_the_sink(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create(0, &test_fixture) == SXML_STATUS_OK);
    /* Two trees change: each reaches the sink once, after the snapshot;
       the query's own value is empty (XQUF 3.0, 2.2.3). */
    query_run run = {.expression_text =
                         TEXT("let $d := document { <a><b/></a> }, $e := <e/> return "
                              "(insert node <c/> into $d/a, delete node $d/a/b, "
                              "rename node $e as 'f')")};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, run.is_updating);
    SXML_TEST_EXPECT(state, run.updated_tree_count == 2);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("<a><c/></a><f/>")));
    /* A simple query is not updating and hands nothing over. */
    run.expression_text = TEXT("copy $c := <a/> modify delete node $c return $c");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, !run.is_updating);
    SXML_TEST_EXPECT(state, run.updated_tree_count == 0);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("<a/>")));
    /* An error while applying hands nothing over. */
    run.expression_text =
        TEXT("let $e := <e/> return (rename node $e as 'f', rename node $e as 'g')");
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_DYNAMIC_ERROR);
    SXML_TEST_EXPECT(state, run.updated_tree_count == 0);
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static const sxml_test_case xquery_update_test_cases[] = {
    {.name = "queries_give_values_and_errors", .function = test_queries_give_values_and_errors},
    {.name = "updating_query_hands_trees_to_the_sink",
     .function = test_updating_query_hands_trees_to_the_sink},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "xquery_update",
            .cases = xquery_update_test_cases,
            .case_count = SXML_TEST_COUNT(xquery_update_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
