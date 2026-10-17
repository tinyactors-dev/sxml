/*
 * Fuzz target: XPath 4.0 evaluation (src/evaluator*.c, src/operator*.c,
 * src/cast*.c and the values of src/xdm*.c; packet 1.26).
 *
 * The input is one expression text, compiled as in xpath_compile_fuzz.c
 * and evaluated with small limits: the item heap's byte limit
 * (SXML_FUZZ_MAXIMUM_HEAP_BYTE_COUNT), an evaluation depth of
 * SXML_FUZZ_MAXIMUM_EVALUATION_DEPTH and a frame slot limit. The context
 * value and $doc are a small document parsed by the XML parser; $x is the
 * document too. The canonical text of every atomic item of the result is
 * written, as serialization would. The internal evaluator is used until
 * the processor and its public XPath API exist (packet 1.23).
 *
 * Golden file: tests/evaluator_test.c (evaluate_with, run_path_case).
 */
#include "cast_internal.h"
#include "evaluator_internal.h"
#include "fuzz_fixture.h"
#include "sxml/tree.h"
#include "sxml/xml.h"

#define SXML_FUZZ_MAXIMUM_EVALUATION_DEPTH 1000U
#define SXML_FUZZ_MAXIMUM_FRAME_SLOT_COUNT 100000U
#define SXML_FUZZ_MAXIMUM_EVALUATION_STEP_COUNT 10000000U
/* Items of the result whose canonical text is written. */
#define SXML_FUZZ_MAXIMUM_WRITTEN_ITEM_COUNT 1000U

static const char context_document_text[] =
    "<?xml version='1.0'?><!DOCTYPE works [<!ATTLIST employee id ID "
    "#IMPLIED>]><works xmlns:p='urn:p'><employee id='e1' name='Jane Doe 1' "
    "p:gender='female'><hours>40</hours><hours>20</hours></employee>"
    "<employee id='e2' name='John Doe 2'><hours>30</hours><!--c--><?pi "
    "data?></employee><p:center xml:lang='en'>text <b>bold</b> tail"
    "</p:center></works>";

/* Parses the context document; *document is its document node, owned
   (the arena tree goes with the last reference to it). */
static sxml_status parse_context_document(const sxml_fuzz_fixture *fixture,
                                          sxml_item *document) {
    const sxml_allocator allocator = sxml_fuzz_fixture_allocator(fixture);
    sxml_xml_parser *parser = NULL;
    sxml_status status = sxml_xml_parser_create(
        allocator,
        &(sxml_xml_parser_configuration){.allocator = allocator,
                                         .name_pool = fixture->name_pool,
                                         .limits = {.maximum_depth = 0}},
        &parser);
    sxml_arena_tree *arena_tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_create(
            allocator,
            &(sxml_arena_tree_configuration){
                .allocator = allocator,
                .initial_memory = {.bytes = NULL, .byte_count = 0},
                .item_heap = fixture->item_heap,
                .keeps_locations = true,
                .duplicate_attribute_policy =
                    SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
                .is_destroyed_when_unreferenced = true,
                .error_record = NULL},
            &arena_tree);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_parser_parse(
            parser,
            &(sxml_xml_parse_request){
                .input = {.bytes = (const uint8_t *)context_document_text,
                          .byte_count = sizeof(context_document_text) - 1},
                .document_uri = SXML_BYTE_VIEW_FROM_LITERAL("urn:works"),
                .diagnostic = NULL},
            sxml_arena_tree_as_tree_builder(arena_tree));
    }
    sxml_xml_parser_destroy(parser);
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *document = sxml_node_item_make((sxml_node_reference){
        .tree = tree, .node_index = {.index = 0}, .namespace_number = 0});
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

/* Writes the canonical text of the first atomic items of value. */
static void write_value(const sxml_fuzz_fixture *fixture, sxml_sequence value) {
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(value);
    uint8_t output[SXML_CAST_CANONICAL_TEXT_CAPACITY];
    for (uint32_t item_number = 0;
         item_number < SXML_FUZZ_MAXIMUM_WRITTEN_ITEM_COUNT; item_number++) {
        sxml_iteration_step step = {.has_item = false};
        const sxml_status status =
            sxml_sequence_iterator_next(&iterator, NULL, &step);
        sxml_fuzz_check_status(status, "sxml_sequence_iterator_next");
        if (status != SXML_STATUS_OK || !step.has_item) {
            return;
        }
        if (sxml_item_get_kind(step.item) == SXML_ITEM_KIND_ATOMIC) {
            sxml_canonical_text canonical_text = {.text = {.byte_count = 0}};
            /* A QName or binary value may need more than the output. */
            const sxml_status write_status = sxml_cast_write_canonical_text(
                fixture->item_heap,
                &(sxml_canonical_text_request){
                    .item = step.item,
                    .output = {.bytes = output, .byte_count = sizeof(output)}},
                &canonical_text);
            sxml_fuzz_check_status(write_status,
                                   "sxml_cast_write_canonical_text");
        }
    }
}

/* Compiles and evaluates text with document as context value. */
static void evaluate(const sxml_fuzz_fixture *fixture, sxml_byte_view text,
                     sxml_item document) {
    sxml_ir_unit *unit = NULL;
    if (sxml_fuzz_compile_xpath(fixture, text, &unit) != SXML_STATUS_OK) {
        return;
    }
    const sxml_allocator allocator = sxml_fuzz_fixture_allocator(fixture);
    sxml_dynamic_context *dynamic_context = NULL;
    sxml_status status = sxml_dynamic_context_create(
        allocator,
        &(sxml_evaluator_configuration){
            .item_heap = fixture->item_heap,
            .allocator = allocator,
            .document_loader = {.state = NULL, .operations = NULL},
            .function_table = fixture->function_table,
            .maximum_evaluation_depth = SXML_FUZZ_MAXIMUM_EVALUATION_DEPTH,
            .maximum_frame_slot_count = SXML_FUZZ_MAXIMUM_FRAME_SLOT_COUNT,
            /* The step bound of D89 (P9): long evaluations stop instead of
               timing out. */
            .maximum_evaluation_step_count = SXML_FUZZ_MAXIMUM_EVALUATION_STEP_COUNT},
        &dynamic_context);
    sxml_fuzz_check_status(status, "sxml_dynamic_context_create");
    if (status == SXML_STATUS_OK) {
        const sxml_sequence document_value = sxml_sequence_from_item(document);
        sxml_evaluator_slot global_values[SXML_FUZZ_VARIABLE_COUNT];
        for (size_t slot_index = 0; slot_index < SXML_FUZZ_VARIABLE_COUNT;
             slot_index++) {
            global_values[slot_index] = (sxml_evaluator_slot){
                .value = document_value,
                .slot_state = SXML_EVALUATOR_SLOT_STATE_BOUND};
        }
        sxml_error_record error_record = {.value = NULL};
        sxml_sequence value = sxml_sequence_empty();
        status = sxml_evaluator_evaluate(
            dynamic_context,
            &(sxml_evaluation_request){
                .unit = unit,
                .has_context_value = true,
                .context_value = document_value,
                .global_values = {.slots = global_values,
                                  .slot_count = SXML_FUZZ_VARIABLE_COUNT},
                .error_record = &error_record},
            &value);
        sxml_fuzz_check_status(status, "sxml_evaluator_evaluate");
        if (status == SXML_STATUS_OK) {
            write_value(fixture, value);
            sxml_sequence_release(value);
        }
    }
    sxml_dynamic_context_destroy(dynamic_context);
    sxml_ir_unit_release(unit);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) { // NOLINT(readability-identifier-naming)
    sxml_fuzz_fixture fixture;
    if (sxml_fuzz_fixture_create(
            &(sxml_fuzz_fixture_configuration){.has_function_library = true},
            &fixture) != SXML_STATUS_OK) {
        return 0;
    }
    sxml_item document = {.representation = 0};
    const sxml_status status = parse_context_document(&fixture, &document);
    sxml_fuzz_check_status(status, "parsing the context document");
    if (status == SXML_STATUS_OK) {
        evaluate(&fixture, sxml_fuzz_input_view(data, size), document);
        sxml_item_release(document);
    }
    sxml_fuzz_fixture_destroy_checking_leaks(&fixture);
    return 0;
}
