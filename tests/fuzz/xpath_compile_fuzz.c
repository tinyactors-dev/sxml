/*
 * Fuzz target: XPath 4.0 compilation (src/xpath_parse.c, src/xpath_static.c,
 * src/xpath_lower.c, src/ir.c; packet 1.26).
 *
 * The input is one expression text. It is parsed into an AST
 * (sxml_xpath_build_ast), parsed as a SequenceType
 * (sxml_xpath_build_sequence_type_ast), and compiled
 * (sxml_xpath_compile: lowering, the IR passes, the verifier, sealing).
 * The verifier's SXML_STATUS_INVALID_ARGUMENT is an error in sxml, so
 * it stops the fuzzer like a crash. Front-end depth is unbounded on
 * purpose: deep input must cost heap, not C stack (D71); lowering is
 * bounded by SXML_FUZZ_MAXIMUM_EXPRESSION_DEPTH.
 *
 * Golden file: the compilation in tests/evaluator_test.c (evaluate_with).
 */
#include "fuzz_fixture.h"
#include "xpath_ast_internal.h"

/* One of the two front ends. */
typedef struct front_end {
    sxml_status (*build)(const sxml_ast_build_request *request, sxml_ast **ast);
    const char *function_name;
} front_end;

/* Parses text with a front end; nothing is kept. */
static void build_ast(const sxml_fuzz_fixture *fixture, sxml_byte_view text,
                      front_end parser) {
    const sxml_ast_build_request request = {
        .expression_text = text,
        .source_name = SXML_BYTE_VIEW_FROM_LITERAL("fuzz.xpath"),
        .allocator = sxml_fuzz_fixture_allocator(fixture),
        .maximum_expression_depth = 0,
        .error_record = NULL};
    sxml_ast *ast = NULL;
    sxml_fuzz_check_status(parser.build(&request, &ast), parser.function_name);
    sxml_ast_destroy(ast);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) { // NOLINT(readability-identifier-naming)
    sxml_fuzz_fixture fixture;
    if (sxml_fuzz_fixture_create(
            &(sxml_fuzz_fixture_configuration){.has_function_library = true},
            &fixture) != SXML_STATUS_OK) {
        return 0;
    }
    const sxml_byte_view text = sxml_fuzz_input_view(data, size);
    build_ast(&fixture, text,
              (front_end){.build = sxml_xpath_build_ast,
                          .function_name = "sxml_xpath_build_ast"});
    build_ast(&fixture, text,
              (front_end){.build = sxml_xpath_build_sequence_type_ast,
                          .function_name =
                              "sxml_xpath_build_sequence_type_ast"});
    sxml_ir_unit *unit = NULL;
    if (sxml_fuzz_compile_xpath(&fixture, text, &unit) == SXML_STATUS_OK) {
        sxml_ir_unit_release(unit);
    }
    sxml_fuzz_fixture_destroy_checking_leaks(&fixture);
    return 0;
}
