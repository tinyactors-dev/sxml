/*
 * Tests for XQuery and XPath Full Text 3.0 through include/sxml/xquery.h
 * alone (docs/DECISIONS.md D214): contains text with every connective,
 * FTWords variant, positional filter and the match options sxml has, the
 * score variables, the static rules, the dynamic errors, the ignore
 * option and the processor without the feature full-text. Every case
 * checks for leaked cells and allocations; one runs under failing
 * allocations.
 *
 * Golden file: tests/xquery_update_test.c.
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

/* How to make the fixture. */
typedef struct fixture_request {
    uint64_t failing_allocation_number;
    /* The processor names sxml_full_text_feature(). */
    bool has_full_text;
    /* What the processor's table resolver serves; NULL for the null
       resolver. */
    const sxml_resource_entry_view *resources;
} fixture_request;

static sxml_allocator fixture_allocator(const fixture *test_fixture) {
    return sxml_counting_allocator_as_allocator(test_fixture->counting_allocator);
}

static sxml_status fixture_create(fixture_request request, fixture *test_fixture) {
    *test_fixture = (fixture){.counting_allocator = NULL, .processor = NULL, .output_buffer = NULL};
    sxml_status status = sxml_counting_allocator_create(
        sxml_default_allocator(),
        &(sxml_counting_allocator_configuration){.backing_allocator = sxml_default_allocator(),
                                                 .failing_allocation_number =
                                                     request.failing_allocation_number},
        &test_fixture->counting_allocator);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_allocator allocator = fixture_allocator(test_fixture);
    const sxml_optional_feature features[1] = {sxml_full_text_feature()};
    status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .resource_resolver = request.resources != NULL
                                     ? sxml_table_resource_resolver(request.resources)
                                     : (sxml_resource_resolver){.state = NULL, .operations = NULL},
            .optional_features = {.optional_features = features,
                                  .optional_feature_count = request.has_full_text ? 1U : 0U}},
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

/* One query to run: its text; out the error record. */
typedef struct query_run {
    sxml_byte_view expression_text;
    sxml_error_record error_record;
} query_run;

/* Compiles and evaluates run->expression_text and serializes the result
   into the fixture's output buffer (cleared first). */
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
    sxml_sequence result = sxml_sequence_empty();
    status = sxml_xquery_query_evaluate(
        query, &(sxml_xquery_evaluation_request){.error_record = &run->error_record}, &result);
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
    /* 3.2: phrases, any, all, phrase, any word, all words. */
    QUERY_VALUE("words",
                "let $t := 'The quick brown fox' return ($t contains text 'quick brown', "
                "$t contains text 'brown quick', $t contains text {'red', 'fox'} any, "
                "$t contains text {'red', 'fox'} all, $t contains text {'quick', 'fox'} all, "
                "$t contains text {'quick', 'brown'} phrase, $t contains text 'red fox' any word, "
                "$t contains text 'red fox' all words, $t contains text 'fox quick' all words)",
                "true false true false true true true false true"),
    /* 3.1: the connectives; ftnot and not in. */
    QUERY_VALUE("connectives",
                "let $t := 'a b c' return ($t contains text 'a' ftand 'c', "
                "$t contains text 'a' ftand 'd', $t contains text 'd' ftor 'b', "
                "$t contains text ftnot 'd', $t contains text ftnot 'a', "
                "$t contains text 'b' not in 'a b', $t contains text 'c' not in 'a b')",
                "true false true true false false true"),
    /* 3.3: occurs ... times. */
    QUERY_VALUE("times",
                "let $t := 'a b a c a' return ($t contains text 'a' occurs exactly 3 times, "
                "$t contains text 'a' occurs exactly 2 times, "
                "$t contains text 'a' occurs at least 2 times, "
                "$t contains text 'a' occurs at most 2 times, "
                "$t contains text 'a' occurs from 2 to 4 times, "
                "$t contains text 'd' occurs at most 0 times)",
                "true false true false true true"),
    /* 3.5: ordered, window, distance, scope, content. */
    QUERY_VALUE("positional-filters",
                "let $t := 'One two. Three four.' return ($t contains text 'two' ftand 'one' "
                "ordered, $t contains text 'one' ftand 'two' ordered, "
                "$t contains text 'one' ftand 'three' window 3 words, "
                "$t contains text 'one' ftand 'three' window 2 words, "
                "$t contains text 'one' ftand 'four' distance exactly 2 words, "
                "$t contains text 'one' ftand 'three' different sentence, "
                "$t contains text 'one' ftand 'two' same sentence, "
                "$t contains text 'one' at start, $t contains text 'four' at end, "
                "$t contains text 'one two three' entire content, "
                "$t contains text 'one two three four' entire content)",
                "false true true false true true true true true false true"),
    /* Paragraphs end where elements do, not inside a word. */
    QUERY_VALUE("paragraphs",
                "let $d := <d><p>simple <b>p</b>rototypes</p><p>complex</p></d> return "
                "($d contains text 'prototypes', "
                "$d contains text 'simple' ftand 'prototypes' same paragraph, "
                "$d contains text 'simple' ftand 'complex' same paragraph, "
                "$d contains text 'simple' ftand 'complex' different paragraph)",
                "true true false true"),
    /* 3.4: case, diacritics, wildcards, stop words, and options inherited
       from the prolog and outer selections. */
    QUERY_VALUE("match-options",
                "declare ft-option using case sensitive; "
                "let $t := 'Café au lait' return ($t contains text 'café', "
                "$t contains text 'Cafe', $t contains text 'Cafe' using diacritics sensitive, "
                "$t contains text ('cafe' using case insensitive), "
                "$t contains text 'Caf.' using wildcards, "
                "$t contains text 'C.+ au' using wildcards, "
                "$t contains text 'Café x lait' using stop words ('x'), "
                "$t contains text ('CAFÉ' using uppercase) using case insensitive, "
                "$t contains text 'café' using lowercase)",
                "false true false true true true true false false"),
    /* 3.4.4: stemming, by the language's Snowball stemmer. */
    QUERY_VALUE("stemming",
                "('Improving the usability' contains text 'improve' using stemming, "
                "'Improving' contains text 'improve', "
                "'Die Häuser' contains text 'haus' using stemming using language 'de', "
                "'performed tests' contains text 'perform test' using stemming, "
                "'performed' contains text 'perform.*' using stemming using wildcards)",
                "true false true true true"),
    /* 3.7: without content. */
    QUERY_VALUE("ignore",
                "let $d := <d>a <n>b</n> c</d> return ($d contains text 'a c', "
                "$d contains text 'b' without content $d/n, "
                "$d contains text 'a c' without content $d/n, "
                "$d contains text 'a' without content $d)",
                "false false true false"),
    /* 2.3: score variables. */
    /* 2.3: scores from the matches (w / (w + 1 + log2(1 + n)), w the
       weighted matches, n the tokens), 1 where there is no contains
       expression to score, 0 without a match. */
    QUERY_VALUE("scores",
                "(for $p score $s in ('a', 'b') return $s, let score $s := 'a' contains text "
                "'a' return $s, let score $s := 'a' contains text 'b' return $s, "
                "let score $s := 'x' return $s, let score $s := () return $s)",
                "1 1 0.3333333333333333 0 1 0"),
    QUERY_VALUE("for-scores",
                "for $p score $s in (<p>a</p>, <p>a b a</p>, <p>b</p>)[. contains text 'a'] "
                "return round($s, 3)",
                "0.333 0.4"),
    QUERY_VALUE("for-scores-relative",
                "for $p score $s in <r><d><t>x a</t></d></r>/d[t contains text 'a'] return $s > 0",
                "true"),
    QUERY_VALUE("weights",
                "('a b' contains text ('a' weight {0.5}) ftand ('b' weight {2}), "
                "let score $plain := 'a b' contains text 'a' "
                "let score $heavy := 'a b' contains text ('a' weight {2}) "
                "let score $light := 'a b' contains text ('a' weight {0.1}) "
                "return ($heavy > $plain, $light < $plain, $heavy le 1))",
                "true true true true"),
    /* The static rules. */
    QUERY_ERROR("duplicate-option", "'a' contains text 'a' using stemming using no stemming",
                "FTST0019"),
    QUERY_ERROR("stop-words-by-uri", "'a' contains text 'a' using stop words at 'urn:x'",
                "FTST0008"),
    QUERY_ERROR("language", "'a' contains text 'a' using language 'th'", "FTST0009"),
    /* Ideographs are words of one character (UAX #29). */
    QUERY_VALUE("chinese",
                "('网站可用性测试' contains text '可用性' using language 'zh', "
                "'网站可用性测试' contains text '可性', "
                "'网站可用性测试' contains text '可用' ftand '站' ordered)",
                "true false false"),
    QUERY_VALUE("language-with-region", "'a' contains text 'a' using language 'en-GB'", "true"),
    QUERY_ERROR("extension-option-prefix", "'a' contains text 'a' using option p:o 'v'",
                "XPST0081"),
    QUERY_ERROR("ignore-type", "'a' contains text 'a' without content 'b'", "XPTY0004"),
    QUERY_VALUE("escaped-punctuation", "'a specialist.' contains text 'specialist\\.' using "
                "wildcards", "true"),
    QUERY_VALUE("filter-order",
                "'one two three' contains text 'three' ftand 'one' window 3 words ordered",
                "false"),
    QUERY_ERROR("thesaurus-by-uri", "'a' contains text 'a' using thesaurus at 'urn:x'",
                "FTST0018"),
    QUERY_ERROR("extension-without-selection", "'a' contains text (# Q{urn:x}q #) { }", "XQST0079"),
    QUERY_ERROR("score-named-as-variable", "for $t score $t in 1 return $t", "XQST0089"),
    /* The dynamic errors. */
    QUERY_ERROR("weight-range", "'a' contains text 'a' weight {1001}", "FTDY0016"),
    QUERY_ERROR("mild-not-exclude", "'a b' contains text ('a' not in ftnot 'b')", "FTDY0017"),
    QUERY_ERROR("wildcard-syntax", "'a' contains text 'a.{1' using wildcards", "FTDY0020"),
    QUERY_ERROR("words-type", "'a' contains text {1}", "XPTY0004"),
};

/* Stop word lists and thesauri by URI and by default (3.4.3, 3.4.7),
   from the resources below. */
static const query_case resource_cases[] = {
    QUERY_VALUE("stop-words-at",
                "('propagating the errors' contains text 'propagating of errors' using stop "
                "words at 'urn:stop', 'propagating the errors' contains text 'propagating of "
                "errors' using stop words at 'urn:stop' except ('of'))",
                "true false"),
    QUERY_VALUE("stop-words-default",
                "'a b c' contains text 'a x c' using stop words default", "true"),
    QUERY_ERROR("stop-words-missing",
                "'a' contains text 'a' using stop words at 'urn:missing'", "FTST0008"),
    QUERY_VALUE("thesaurus-at",
                "('the software' contains text 'program' using thesaurus at 'urn:thesaurus', "
                "'the software' contains text 'program' using thesaurus at 'urn:thesaurus' "
                "relationship 'NT', "
                "'Web' contains text 'infrastructure' using thesaurus at 'urn:thesaurus' "
                "relationship 'nt' exactly 2 levels, "
                "'networks' contains text 'infrastructure' using thesaurus at "
                "'urn:thesaurus' relationship 'NT' exactly 2 levels, "
                "'the WEB' contains text 'infrastructure' using thesaurus at 'urn:thesaurus', "
                "'program' contains text 'program' using thesaurus at 'urn:thesaurus' "
                "relationship 'BT')",
                "true false true false true true"),
    /* The table's default resource is no thesaurus: the default one is
       then empty. */
    QUERY_VALUE("thesaurus-default",
                "('the software' contains text 'program' using thesaurus default, "
                "'the software' contains text 'program' using thesaurus (default, at "
                "'urn:thesaurus'))",
                "false true"),
    QUERY_ERROR("thesaurus-missing",
                "'a' contains text 'a' using thesaurus at 'urn:missing'", "FTST0018"),
};

static const sxml_resource_entry resource_entries[] = {
    {.uri = SXML_BYTE_VIEW_INITIALIZER("urn:stop"),
     .content = SXML_BYTE_VIEW_INITIALIZER("the of\nand ")},
    {.uri = SXML_BYTE_VIEW_INITIALIZER(""), .content = SXML_BYTE_VIEW_INITIALIZER("x")},
    {.uri = SXML_BYTE_VIEW_INITIALIZER("urn:thesaurus"),
     .content = SXML_BYTE_VIEW_INITIALIZER(
         "<thesaurus xmlns='http://www.w3.org/2007/xqftts/thesaurus'>"
         "<entry><term>program</term><synonym><term>software</term>"
         "<relationship>UF</relationship></synonym></entry>"
         "<entry><term>infrastructure</term><synonym><term>networks</term>"
         "<synonym><term>Web</term><relationship>NT</relationship></synonym>"
         "<relationship>NT</relationship></synonym></entry></thesaurus>")},
};

/* Runs cases in a fixture with the feature (and resources). */
typedef struct case_table {
    const query_case *cases;
    size_t case_count;
    const sxml_resource_entry_view *resources;
} case_table;

static void run_cases(sxml_test_state *state, case_table table) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state, fixture_create((fixture_request){.has_full_text = true,
                                                            .resources = table.resources},
                                           &test_fixture) == SXML_STATUS_OK);
    for (size_t case_index = 0; case_index < table.case_count; case_index += 1) {
        const query_case *current = &table.cases[case_index];
        query_run run = {.expression_text = current->expression_text};
        const sxml_status status = run_query(&test_fixture, &run);
        const bool passed =
            current->expected_error_code.byte_count > 0
                ? status != SXML_STATUS_OK &&
                      sxml_byte_view_equals(run.error_record.code.local_name,
                                            current->expected_error_code)
                : status == SXML_STATUS_OK && output_is(&test_fixture, current->expected_output);
        (void)sxml_test_record_check(state, (sxml_test_check){.passed = passed,
                                                              .expression = current->case_name,
                                                              .file_name = __FILE__,
                                                              .line_number = __LINE__});
    }
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

static void test_queries_give_values_and_errors(sxml_test_state *state) {
    run_cases(state, (case_table){.cases = query_cases,
                                  .case_count = sizeof(query_cases) / sizeof(query_cases[0]),
                                  .resources = NULL});
}

static void test_stop_words_and_thesauri_come_from_the_resolver(sxml_test_state *state) {
    const sxml_resource_entry_view resources = {
        .resource_entries = resource_entries,
        .resource_entry_count = sizeof(resource_entries) / sizeof(resource_entries[0])};
    run_cases(state, (case_table){.cases = resource_cases,
                                  .case_count = sizeof(resource_cases) / sizeof(resource_cases[0]),
                                  .resources = &resources});
}

/* --------------------------------------------- Without the feature */

static void test_without_the_feature_full_text_is_a_syntax_error(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_EXPECT(state,
                     fixture_create((fixture_request){.has_full_text = false}, &test_fixture) ==
                         SXML_STATUS_OK);
    static const sxml_byte_view queries[] = {
        SXML_BYTE_VIEW_INITIALIZER("'a' contains text 'a'"),
        SXML_BYTE_VIEW_INITIALIZER("declare ft-option using stemming; 1"),
        SXML_BYTE_VIEW_INITIALIZER("for $a score $s in 1 return $s"),
    };
    for (size_t query_index = 0; query_index < sizeof(queries) / sizeof(queries[0]);
         query_index += 1) {
        query_run run = {.expression_text = queries[query_index]};
        SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_STATIC_ERROR);
        SXML_TEST_EXPECT(state,
                         sxml_byte_view_equals(run.error_record.code.local_name, TEXT("XPST0003")));
    }
    /* Names that are Full Text keywords stay names. */
    query_run run = {.expression_text = TEXT("let $score := 1 return not(contains('ab', 'b'))")};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("false")));
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
}

/* ------------------------------------------------- Failing allocations */

static const sxml_byte_view failing_query = SXML_BYTE_VIEW_INITIALIZER(
    "<d><p>One two.</p><p>Three four.</p></d> contains text ('one' ftor 'x' using stop "
    "words ('the')) ftand ftnot 'five' window 4 words");

/* The allocations a fixture's setup makes and the total with the query. */
typedef struct allocation_span {
    uint64_t setup_count;
    uint64_t total_count;
} allocation_span;

static allocation_span count_allocations(sxml_test_state *state) {
    allocation_span span = {.setup_count = 0, .total_count = 0};
    fixture test_fixture;
    SXML_TEST_EXPECT(state,
                     fixture_create((fixture_request){.has_full_text = true}, &test_fixture) ==
                         SXML_STATUS_OK);
    span.setup_count =
        sxml_counting_allocator_read_counts(test_fixture.counting_allocator)
            .attempted_allocation_count;
    query_run run = {.expression_text = failing_query};
    SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, output_is(&test_fixture, TEXT("true")));
    span.total_count =
        sxml_counting_allocator_read_counts(test_fixture.counting_allocator)
            .attempted_allocation_count;
    SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    return span;
}

/* Every failure point of the query, sampled to about 1,000 of them: each
   is SXML_STATUS_OUT_OF_MEMORY and leaks nothing. */
static void test_contains_survives_allocation_failures(sxml_test_state *state) {
    const allocation_span span = count_allocations(state);
    SXML_TEST_REQUIRE(state, span.total_count > span.setup_count);
    const uint64_t stride = (span.total_count - span.setup_count) / 1000U + 1U;
    for (uint64_t failing_allocation_number = span.setup_count + 1;
         failing_allocation_number <= span.total_count; failing_allocation_number += stride) {
        fixture test_fixture;
        SXML_TEST_EXPECT(
            state, fixture_create((fixture_request){.failing_allocation_number =
                                                        failing_allocation_number,
                                                    .has_full_text = true},
                                  &test_fixture) == SXML_STATUS_OK);
        query_run run = {.expression_text = failing_query};
        SXML_TEST_EXPECT(state, run_query(&test_fixture, &run) == SXML_STATUS_OUT_OF_MEMORY);
        SXML_TEST_EXPECT(state, fixture_destroy(&test_fixture));
    }
}

static const sxml_test_case full_text_test_cases[] = {
    {.name = "queries_give_values_and_errors", .function = test_queries_give_values_and_errors},
    {.name = "stop_words_and_thesauri_come_from_the_resolver",
     .function = test_stop_words_and_thesauri_come_from_the_resolver},
    {.name = "without_the_feature_full_text_is_a_syntax_error",
     .function = test_without_the_feature_full_text_is_a_syntax_error},
    {.name = "contains_survives_allocation_failures",
     .function = test_contains_survives_allocation_failures},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "full_text",
            .cases = full_text_test_cases,
            .case_count = SXML_TEST_COUNT(full_text_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
