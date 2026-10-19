/*
 * Fuzz target: the HTML parser (src/html_internal.h; packet 7.10a). The
 * first byte picks how the rest is given to fn:parse-html: as a string
 * (it must then be UTF-8, else the input is skipped) or as octets, and
 * whether parsing stops at the first parse error. The document is built
 * into an arena tree of a processor that names the HTML feature, and its
 * nodes are read back. HTML has no input that fails to parse, so only a
 * limit, the allocator or fail-on-error may stop it; nesting depth is
 * unbounded on purpose, since deep input must cost heap, not C stack
 * (D71).
 *
 * Golden file: tests/fuzz/xml_parse_fuzz.c.
 */
#include "fuzz_fixture.h"
#include "html_internal.h"
#include "sxml/processor.h"
#include "sxml/string.h"
#include "sxml/tree.h"

/* Reads every node of the document back. */
static void read_document(sxml_item document_node) {
    sxml_node_reference document;
    sxml_fuzz_check_status(sxml_item_get_node(document_node, &document), "sxml_item_get_node");
    sxml_tree_description description = {.node_count = 0};
    sxml_fuzz_check_status(sxml_tree_describe(document.tree, &description), "sxml_tree_describe");
    for (uint32_t node_number = 0; node_number < description.node_count; node_number++) {
        const sxml_node_reference node = {
            .tree = document.tree, .node_index = {.index = node_number}, .namespace_number = 0};
        sxml_node_description node_description = {.node_kind = SXML_NODE_KIND_DOCUMENT};
        sxml_fuzz_check_status(sxml_node_describe(node, &node_description), "sxml_node_describe");
        if (node_description.node_kind != SXML_NODE_KIND_DOCUMENT &&
            node_description.node_kind != SXML_NODE_KIND_ELEMENT) {
            sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
            sxml_fuzz_check_status(sxml_node_get_content(node, &content),
                                   "sxml_node_get_content");
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) {
        return 0;
    }
    const uint8_t mode = data[0];
    const sxml_byte_view input = {.bytes = data + 1, .byte_count = size - 1};
    const bool is_string = (mode & 1U) != 0;
    sxml_utf8_validation validation;
    if (is_string && sxml_utf8_validate(input, &validation) != SXML_STATUS_OK) {
        return 0;
    }
    sxml_fuzz_fixture fixture;
    sxml_fuzz_check_status(
        sxml_fuzz_fixture_create(&(sxml_fuzz_fixture_configuration){.has_function_library = false},
                                 &fixture),
        "sxml_fuzz_fixture_create");
    const sxml_allocator allocator = sxml_fuzz_fixture_allocator(&fixture);
    const sxml_optional_feature feature = sxml_html_parser_feature();
    sxml_processor *processor = NULL;
    sxml_fuzz_check_status(
        sxml_processor_create(
            allocator,
            &(sxml_processor_configuration){
                .allocator = allocator,
                .limits = {.maximum_item_heap_byte_count = SXML_FUZZ_MAXIMUM_HEAP_BYTE_COUNT},
                .optional_features = {.optional_features = &feature, .optional_feature_count = 1}},
            &processor),
        "sxml_processor_create");
    sxml_html_document_outcome outcome = {.failure = SXML_HTML_DOCUMENT_FAILURE_NONE,
                                          .document_node = {.representation = 0}};
    const sxml_status status = sxml_html_parse_document(
        processor,
        &(sxml_html_document_request){
            .input = {.input = input,
                      .is_string = is_string,
                      .encoding_name = (mode & 4U) != 0 ? SXML_BYTE_VIEW_FROM_LITERAL("windows-1251")
                                                        : (sxml_byte_view){.bytes = NULL,
                                                                           .byte_count = 0}},
            .fails_on_parse_error = (mode & 2U) != 0,
            .base_uri = {.bytes = NULL, .byte_count = 0},
            .document_uri = {.bytes = NULL, .byte_count = 0}},
        &outcome);
    if (status != SXML_STATUS_LIMIT_EXCEEDED) {
        sxml_fuzz_check_status(status, "sxml_html_parse_document");
    }
    if (status == SXML_STATUS_OK && outcome.failure == SXML_HTML_DOCUMENT_FAILURE_NONE) {
        read_document(outcome.document_node);
        sxml_item_release(outcome.document_node);
    }
    sxml_processor_destroy(processor);
    sxml_fuzz_fixture_destroy_checking_leaks(&fixture);
    return 0;
}
