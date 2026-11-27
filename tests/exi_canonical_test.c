/*
 * Tests for Canonical EXI in the encoder (src/exi_encoder.c, packet 8.11;
 * Canonical EXI, W3C Recommendation 7 June 2018, sections 3 and 4) through
 * the public API: the header (no cookie, the options document unless
 * omitOptionsDocument), compression written as pre-compression, and
 * attributes and namespace declarations sorted with built-in grammars.
 *
 * Golden file: tests/exi_processor_test.c.
 */
#include "sxml/exi.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/xdm.h"
#include "test_runner.h"

#include <string.h>

typedef struct fixture {
    sxml_processor *processor;
    sxml_exi_encoder *encoder;
    sxml_output_buffer *output_buffer;
    sxml_item document;
    bool has_document;
} fixture;

static sxml_status fixture_create(const char *xml_text, fixture *test_fixture) {
    *test_fixture = (fixture){0};
    const sxml_allocator allocator = sxml_default_allocator();
    sxml_status status = sxml_processor_create(allocator, &(sxml_processor_configuration){.allocator = allocator},
                                               &test_fixture->processor);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_encoder_create(allocator, &(sxml_exi_encoder_configuration){.allocator = allocator},
                                         &test_fixture->encoder);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(
            allocator, &(sxml_output_buffer_configuration){.allocator = allocator, .maximum_byte_count = 0},
            &test_fixture->output_buffer);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_processor_parse_document(
            test_fixture->processor,
            &(sxml_xml_parse_request){.input = {.bytes = (const uint8_t *)xml_text, .byte_count = strlen(xml_text)}},
            &test_fixture->document);
        test_fixture->has_document = status == SXML_STATUS_OK;
    }
    return status;
}

static void fixture_destroy(fixture *test_fixture) {
    if (test_fixture->has_document) {
        sxml_item_release(test_fixture->document);
    }
    sxml_output_buffer_destroy(test_fixture->output_buffer);
    sxml_exi_encoder_destroy(test_fixture->encoder);
    sxml_processor_destroy(test_fixture->processor);
}

/* Encodes the fixture's document; the stream is the buffer's bytes. */
typedef struct encoding_case {
    const sxml_exi_options *options;
    sxml_exi_header_options header_options;
    sxml_exi_canonicalization canonicalization;
} encoding_case;

static sxml_status encode(fixture *test_fixture, const encoding_case *encoding) {
    sxml_node_reference root = {0};
    sxml_status status = sxml_item_get_node(test_fixture->document, &root);
    sxml_output_buffer_clear(test_fixture->output_buffer);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_encoder_encode(
            test_fixture->encoder,
            &(sxml_exi_encode_request){.root = root,
                                       .options = encoding->options,
                                       .header_options = encoding->header_options,
                                       .canonicalization = encoding->canonicalization},
            sxml_output_buffer_as_output_stream(test_fixture->output_buffer));
    }
    return status;
}

static bool stream_is(const fixture *test_fixture, sxml_byte_view expected) {
    const sxml_byte_view bytes = sxml_output_buffer_get_bytes(test_fixture->output_buffer);
    return bytes.byte_count == expected.byte_count && memcmp(bytes.bytes, expected.bytes, expected.byte_count) == 0;
}

#define BYTES(array) ((sxml_byte_view){.bytes = (array), .byte_count = sizeof(array)})

static void test_the_options_document_and_its_omission(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create("<a/>", &test_fixture) == SXML_STATUS_OK);
    /* 3: no cookie, though asked for; the presence bit, then the options
       document of the defaults (SE (header) 0, its EE 11). The body:
       SE (*) with 0 bits, the URI "" (01), "a" (length + 1, then 'a'), EE
       (0.0: 00), ED with 0 bits. */
    SXML_TEST_REQUIRE(state, encode(&test_fixture, &(encoding_case){.header_options = {.includes_cookie = true},
                                                                     .canonicalization = {.is_canonical = true}}) ==
                                 SXML_STATUS_OK);
    static const uint8_t with_options[] = {0xA0, 0x68, 0x13, 0x08};
    SXML_TEST_EXPECT(state, stream_is(&test_fixture, BYTES(with_options)));
    /* omitOptionsDocument: the presence bit is 0. */
    SXML_TEST_REQUIRE(state, encode(&test_fixture, &(encoding_case){.canonicalization = {.is_canonical = true,
                                                                                         .omits_options_document = true}}) ==
                                 SXML_STATUS_OK);
    static const uint8_t without_options[] = {0x80, 0x40, 0x98, 0x40};
    SXML_TEST_EXPECT(state, stream_is(&test_fixture, BYTES(without_options)));
    /* Without Canonical EXI the same body follows the same header bits:
       the canonical form of a document that has no choice to make. */
    SXML_TEST_REQUIRE(state, encode(&test_fixture, &(encoding_case){0}) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, stream_is(&test_fixture, BYTES(without_options)));
    fixture_destroy(&test_fixture);
}

static void test_compression_is_written_as_pre_compression(sxml_test_state *state) {
    fixture test_fixture;
    SXML_TEST_REQUIRE(state, fixture_create("<a>text</a>", &test_fixture) == SXML_STATUS_OK);
    const sxml_exi_options compressed = {.alignment = SXML_EXI_ALIGNMENT_COMPRESSION};
    const sxml_exi_options precompressed = {.alignment = SXML_EXI_ALIGNMENT_PRE_COMPRESSION};
    SXML_TEST_REQUIRE(state, encode(&test_fixture, &(encoding_case){.options = &compressed,
                                                                     .canonicalization = {.is_canonical = true}}) ==
                                 SXML_STATUS_OK);
    sxml_output_buffer *first = NULL;
    SXML_TEST_REQUIRE(state, sxml_output_buffer_create(sxml_default_allocator(),
                                                       &(sxml_output_buffer_configuration){
                                                           .allocator = sxml_default_allocator(), .maximum_byte_count = 0},
                                                       &first) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, sxml_output_stream_write(sxml_output_buffer_as_output_stream(first),
                                                      sxml_output_buffer_get_bytes(test_fixture.output_buffer)) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, encode(&test_fixture, &(encoding_case){.options = &precompressed,
                                                                     .canonicalization = {.is_canonical = true}}) ==
                                 SXML_STATUS_OK);
    const sxml_byte_view compressed_stream = sxml_output_buffer_get_bytes(first);
    SXML_TEST_EXPECT(state, stream_is(&test_fixture, compressed_stream));
    sxml_output_buffer_destroy(first);
    fixture_destroy(&test_fixture);
}

static void test_attributes_and_namespaces_are_sorted(sxml_test_state *state) {
    /* 4.4: attributes by local-name then URI, declarations by prefix,
       with built-in grammars too. */
    fixture unsorted;
    fixture sorted;
    SXML_TEST_REQUIRE(state, fixture_create("<a xmlns:z='urn:z' xmlns:b='urn:b' z:y='1' x='2' b:y='3'/>", &unsorted) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, fixture_create("<a xmlns:b='urn:b' xmlns:z='urn:z' x='2' b:y='3' z:y='1'/>", &sorted) ==
                                 SXML_STATUS_OK);
    const sxml_exi_options prefixes = {.fidelity = {.preserves_prefixes = true}};
    const encoding_case canonical = {.options = &prefixes, .canonicalization = {.is_canonical = true}};
    SXML_TEST_REQUIRE(state, encode(&unsorted, &canonical) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, encode(&sorted, &canonical) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, stream_is(&unsorted, sxml_output_buffer_get_bytes(sorted.output_buffer)));
    /* Without Canonical EXI the document order is kept. */
    SXML_TEST_REQUIRE(state, encode(&unsorted, &(encoding_case){.options = &prefixes}) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, encode(&sorted, &(encoding_case){.options = &prefixes}) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, !stream_is(&unsorted, sxml_output_buffer_get_bytes(sorted.output_buffer)));
    fixture_destroy(&unsorted);
    fixture_destroy(&sorted);
}

static const sxml_test_case exi_canonical_test_cases[] = {
    {.name = "the_options_document_and_its_omission", .function = test_the_options_document_and_its_omission},
    {.name = "compression_is_written_as_pre_compression", .function = test_compression_is_written_as_pre_compression},
    {.name = "attributes_and_namespaces_are_sorted", .function = test_attributes_and_namespaces_are_sorted},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "exi_canonical",
            .cases = exi_canonical_test_cases,
            .case_count = SXML_TEST_COUNT(exi_canonical_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
