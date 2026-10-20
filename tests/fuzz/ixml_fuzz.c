/*
 * Fuzz target: Invisible XML (src/ixml.c, src/ixml_earley.c,
 * src/function_library_ixml.c; packet 7.18, D215).
 *
 * The input is a mode byte, the text of an iXML grammar, a zero byte and
 * the text to parse (without a zero byte, the grammar parses itself).
 * Both texts must be UTF-8, else the input is skipped. The resource
 * resolver serves them as urn:fuzz/g.ixml and urn:fuzz/i.txt, and the
 * query invisible-xml($grammar, {'fail-on-error': B})($input) is run
 * through include/sxml/xquery.h on a processor that names the Invisible
 * XML feature, with the limits of xquery_fuzz.c, and its result is
 * serialized. So the grammar of grammars reads the grammar text, the
 * grammar compiles, the Earley parser reads the input and the parse tree
 * becomes XML, each bounded by the processor's step and heap limits.
 * Every allocation goes through the fixture's counting allocator, so a
 * leak stops the fuzzer.
 *
 * Golden file: tests/fuzz/xquery_fuzz.c and tests/fuzz/dtd_fuzz.c (the
 * split input).
 */
#include "fuzz_fixture.h"
#include "sxml/processor.h"
#include "sxml/string.h"

#include <string.h>

#define SXML_FUZZ_MAXIMUM_QUERY_EVALUATION_DEPTH 1000U
#define SXML_FUZZ_MAXIMUM_QUERY_STEP_COUNT 10000000U

static const char failing_query_text[] =
    "invisible-xml(unparsed-text('g.ixml'), {'fail-on-error': true()})"
    "(unparsed-text('i.txt'))";
static const char query_text[] =
    "invisible-xml(unparsed-text('g.ixml'))(unparsed-text('i.txt'))";

/* The two texts of the input. */
typedef struct fuzz_input {
    sxml_byte_view grammar_text;
    sxml_byte_view parsed_text;
} fuzz_input;

static fuzz_input split_input(sxml_byte_view input) {
    const uint8_t *separator =
        input.byte_count == 0 ? NULL : memchr(input.bytes, 0, input.byte_count);
    if (separator == NULL) {
        return (fuzz_input){.grammar_text = input, .parsed_text = input};
    }
    const size_t grammar_byte_count = (size_t)(separator - input.bytes);
    return (fuzz_input){
        .grammar_text = {.bytes = input.bytes, .byte_count = grammar_byte_count},
        .parsed_text = {.bytes = separator + 1,
                        .byte_count = input.byte_count - grammar_byte_count - 1}};
}

static bool is_utf8(sxml_byte_view text) {
    sxml_utf8_validation validation;
    return sxml_utf8_validate(text, &validation) == SXML_STATUS_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) { // NOLINT(readability-identifier-naming)
    if (size == 0) {
        return 0;
    }
    const bool fails_on_error = (data[0] & 1U) != 0;
    const fuzz_input input = split_input(sxml_fuzz_input_view(data + 1, size - 1));
    if (!is_utf8(input.grammar_text) || !is_utf8(input.parsed_text)) {
        return 0;
    }
    const sxml_resource_entry resources[] = {
        {.uri = SXML_BYTE_VIEW_INITIALIZER("urn:fuzz/g.ixml"), .content = input.grammar_text},
        {.uri = SXML_BYTE_VIEW_INITIALIZER("urn:fuzz/i.txt"), .content = input.parsed_text},
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
    const sxml_optional_feature feature = sxml_invisible_xml_feature();
    sxml_processor *processor = NULL;
    const sxml_status status = sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .limits = {.maximum_item_heap_byte_count = SXML_FUZZ_MAXIMUM_HEAP_BYTE_COUNT,
                       .maximum_expression_depth = SXML_FUZZ_MAXIMUM_EXPRESSION_DEPTH,
                       .maximum_evaluation_depth = SXML_FUZZ_MAXIMUM_QUERY_EVALUATION_DEPTH,
                       .maximum_evaluation_step_count = SXML_FUZZ_MAXIMUM_QUERY_STEP_COUNT},
            .resource_resolver = sxml_table_resource_resolver(&resource_view),
            .optional_features = {.optional_features = &feature, .optional_feature_count = 1}},
        &processor);
    sxml_fuzz_check_status(status, "sxml_processor_create");
    if (status == SXML_STATUS_OK) {
        sxml_fuzz_run_xquery(&fixture, processor,
                             fails_on_error ? SXML_BYTE_VIEW_FROM_LITERAL(failing_query_text)
                                            : SXML_BYTE_VIEW_FROM_LITERAL(query_text));
        sxml_fuzz_processor_destroy_checking_leaks(processor);
    }
    sxml_fuzz_fixture_destroy_checking_leaks(&fixture);
    return 0;
}
