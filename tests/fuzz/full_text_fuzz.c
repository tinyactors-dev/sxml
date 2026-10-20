/*
 * Fuzz target: XQuery and XPath Full Text 3.0 selections (src/full_text*.c,
 * src/xquery_lower_full_text.c; packets 7.12a to 7.12d, D214, D235).
 *
 * The input is the text searched, a zero byte and a full-text selection
 * (without a zero byte, the text is searched for itself as a selection).
 * Both must be UTF-8, else the input is skipped. The resource resolver
 * serves the text as urn:fuzz/t.txt and, as a stop word list, as
 * urn:fuzz/s.txt; the selection is written into the query
 *
 *     let $t := unparsed-text('t.txt') let score $s := $t contains text
 *     SELECTION return ($s, $t contains text SELECTION)
 *
 * which is run through include/sxml/xquery.h on a processor that names
 * the Full Text feature, with the limits of xquery_fuzz.c, and its result
 * is serialized. So the selection reaches the grammar, the static rules,
 * the lowering, the tokenizer, the match options (stemming, stop words,
 * wildcards, case and diacritics) and the match program, scores
 * included. Every allocation goes through the fixture's counting
 * allocator, so a leak stops the fuzzer.
 *
 * Golden file: tests/fuzz/ixml_fuzz.c.
 */
#include "fuzz_fixture.h"
#include "sxml/processor.h"
#include "sxml/string.h"
#include "sxml/xquery.h"

#include <string.h>

#define SXML_FUZZ_MAXIMUM_QUERY_EVALUATION_DEPTH 1000U
#define SXML_FUZZ_MAXIMUM_QUERY_STEP_COUNT 10000000U

static const char query_start[] =
    "let $t := unparsed-text('t.txt') let score $s := $t contains text\n";
static const char query_middle[] = "\nreturn ($s, $t contains text\n";
static const char query_end[] = "\n)";

/* The two texts of the input. */
typedef struct fuzz_input {
    sxml_byte_view searched_text;
    sxml_byte_view selection_text;
} fuzz_input;

static fuzz_input split_input(sxml_byte_view input) {
    const uint8_t *separator =
        input.byte_count == 0 ? NULL : memchr(input.bytes, 0, input.byte_count);
    if (separator == NULL) {
        return (fuzz_input){.searched_text = input, .selection_text = input};
    }
    const size_t searched_byte_count = (size_t)(separator - input.bytes);
    return (fuzz_input){
        .searched_text = {.bytes = input.bytes, .byte_count = searched_byte_count},
        .selection_text = {.bytes = separator + 1,
                           .byte_count = input.byte_count - searched_byte_count - 1}};
}

static bool is_utf8(sxml_byte_view text) {
    sxml_utf8_validation validation;
    return sxml_utf8_validate(text, &validation) == SXML_STATUS_OK;
}

/* Appends part to the query text at *byte_count. */
static void append(uint8_t *query, size_t *byte_count, sxml_byte_view part) {
    if (part.byte_count != 0) {
        memcpy(query + *byte_count, part.bytes, part.byte_count);
    }
    *byte_count += part.byte_count;
}

/* Writes the query around selection into memory from the fixture's
   allocator, which the caller releases; *query is empty when allocation
   failed. */
typedef struct fuzz_query {
    uint8_t *bytes;
    size_t byte_count;
} fuzz_query;

static fuzz_query make_query(const sxml_fuzz_fixture *fixture, sxml_byte_view selection) {
    const sxml_byte_view start = SXML_BYTE_VIEW_FROM_LITERAL(query_start);
    const sxml_byte_view middle = SXML_BYTE_VIEW_FROM_LITERAL(query_middle);
    const sxml_byte_view end = SXML_BYTE_VIEW_FROM_LITERAL(query_end);
    const size_t capacity =
        start.byte_count + middle.byte_count + end.byte_count + 2 * selection.byte_count;
    void *memory = NULL;
    if (sxml_allocator_allocate(sxml_fuzz_fixture_allocator(fixture),
                                (sxml_allocation_request){.byte_count = capacity, .alignment = 1},
                                &memory) != SXML_STATUS_OK) {
        return (fuzz_query){.bytes = NULL, .byte_count = 0};
    }
    fuzz_query query = {.bytes = memory, .byte_count = 0};
    append(query.bytes, &query.byte_count, start);
    append(query.bytes, &query.byte_count, selection);
    append(query.bytes, &query.byte_count, middle);
    append(query.bytes, &query.byte_count, selection);
    append(query.bytes, &query.byte_count, end);
    return query;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) { // NOLINT(readability-identifier-naming)
    const fuzz_input input = split_input(sxml_fuzz_input_view(data, size));
    if (!is_utf8(input.searched_text) || !is_utf8(input.selection_text)) {
        return 0;
    }
    const sxml_resource_entry resources[] = {
        {.uri = SXML_BYTE_VIEW_INITIALIZER("urn:fuzz/t.txt"), .content = input.searched_text},
        {.uri = SXML_BYTE_VIEW_INITIALIZER("urn:fuzz/s.txt"), .content = input.searched_text},
    };
    const sxml_resource_entry_view resource_view = {
        .resource_entries = resources,
        .resource_entry_count = sizeof(resources) / sizeof(resources[0])};
    sxml_fuzz_fixture fixture;
    if (sxml_fuzz_fixture_create(&(sxml_fuzz_fixture_configuration){.has_function_library = false},
                                 &fixture) != SXML_STATUS_OK) {
        return 0;
    }
    const sxml_allocator allocator = sxml_fuzz_fixture_allocator(&fixture);
    const fuzz_query query = make_query(&fixture, input.selection_text);
    const sxml_optional_feature feature = sxml_full_text_feature();
    sxml_processor *processor = NULL;
    const sxml_status status =
        query.bytes == NULL
            ? SXML_STATUS_OUT_OF_MEMORY
            : sxml_processor_create(
                  allocator,
                  &(sxml_processor_configuration){
                      .allocator = allocator,
                      .limits = {.maximum_item_heap_byte_count = SXML_FUZZ_MAXIMUM_HEAP_BYTE_COUNT,
                                 .maximum_expression_depth = SXML_FUZZ_MAXIMUM_EXPRESSION_DEPTH,
                                 .maximum_evaluation_depth =
                                     SXML_FUZZ_MAXIMUM_QUERY_EVALUATION_DEPTH,
                                 .maximum_evaluation_step_count =
                                     SXML_FUZZ_MAXIMUM_QUERY_STEP_COUNT},
                      .resource_resolver = sxml_table_resource_resolver(&resource_view),
                      .optional_features = {.optional_features = &feature,
                                            .optional_feature_count = 1}},
                  &processor);
    if (query.bytes != NULL) {
        sxml_fuzz_check_status(status, "sxml_processor_create");
    }
    if (status == SXML_STATUS_OK) {
        sxml_fuzz_run_xquery(
            &fixture, processor,
            (sxml_byte_view){.bytes = query.bytes, .byte_count = query.byte_count});
        sxml_fuzz_processor_destroy_checking_leaks(processor);
    }
    if (query.bytes != NULL) {
        sxml_allocator_release(allocator, query.bytes);
    }
    sxml_fuzz_fixture_destroy_checking_leaks(&fixture);
    return 0;
}
