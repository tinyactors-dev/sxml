/*
 * Tests for the options document of the EXI header (src/exi_header.c; EXI
 * 1.0, 5.4 and Appendix C): the bits it writes, which are EXIficient's, and
 * the options read back from them.
 */
#include "exi_internal.h"
#include "test_runner.h"

#include <string.h>

/* Writes document, padded to a byte. */
static sxml_status write_document(const sxml_exi_options_document *document, sxml_exi_bit_writer *writer) {
    *writer = (sxml_exi_bit_writer){.allocator = sxml_default_allocator()};
    sxml_status status = sxml_exi_write_options_document(writer, document);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_writer_align(writer);
    }
    return status;
}

static bool bytes_are(const sxml_exi_bit_writer *writer, sxml_byte_view expected) {
    const sxml_byte_view bytes = sxml_exi_writer_bytes(writer);
    return bytes.byte_count == expected.byte_count && memcmp(bytes.bytes, expected.bytes, expected.byte_count) == 0;
}

/* Writes options, reads them back into *read_back (its schemaId text in
   *text). */
typedef struct round_trip {
    sxml_exi_options read_back;
    sxml_exi_buffer text;
    sxml_status status;
} round_trip;

static void round_trip_of(const sxml_exi_options_document *document, round_trip *trip) {
    sxml_exi_bit_writer writer;
    trip->text = (sxml_exi_buffer){.allocator = sxml_default_allocator()};
    trip->status = write_document(document, &writer);
    sxml_exi_bit_reader reader = {.input = sxml_exi_writer_bytes(&writer)};
    if (trip->status == SXML_STATUS_OK) {
        trip->status = sxml_exi_read_options_document(&(sxml_exi_options_document_read){
            .reader = &reader, .options = &trip->read_back, .schema_identifier_text = &trip->text});
    }
    sxml_exi_writer_release(&writer);
}

static void test_the_bits_are_exificient_s(sxml_test_state *state) {
    /* tests/exi/golden/header-options: SE (header) 0, lesscommon 00,
       preserve 01, prefixes 001, comments 01, pis 0, EE of lesscommon 1,
       EE of header 10. */
    const sxml_exi_options preserving = {.fidelity = {.preserves_comments = true,
                                                      .preserves_processing_instructions = true,
                                                      .preserves_prefixes = true}};
    sxml_exi_bit_writer writer;
    SXML_TEST_REQUIRE(state, write_document(&(sxml_exi_options_document){.options = &preserving}, &writer) ==
                                 SXML_STATUS_OK);
    static const uint8_t preserve[] = {0x09, 0x58};
    SXML_TEST_EXPECT(state, bytes_are(&writer, (sxml_byte_view){.bytes = preserve, .byte_count = sizeof preserve}));
    sxml_exi_writer_release(&writer);

    /* tests/exi/golden/schema-header: common 01, schemaId 10, CH 0, the
       string "ab" (length + 2, then each character). */
    const sxml_exi_options named = {.schema_identifier = {.schema_identifier_kind = SXML_EXI_SCHEMA_IDENTIFIER_TEXT,
                                                          .text = SXML_BYTE_VIEW_FROM_LITERAL("ab")}};
    SXML_TEST_REQUIRE(state, write_document(&(sxml_exi_options_document){.options = &named}, &writer) ==
                                 SXML_STATUS_OK);
    /* 0 01 10 0, then 00000100 01100001 01100010, then EE of header 1. */
    static const uint8_t schema_identifier[] = {0x30, 0x11, 0x85, 0x8A};
    SXML_TEST_EXPECT(state, bytes_are(&writer, (sxml_byte_view){.bytes = schema_identifier,
                                                                .byte_count = sizeof schema_identifier}));
    sxml_exi_writer_release(&writer);

    /* The defaults: SE (header), then its EE (11). */
    const sxml_exi_options defaults = {0};
    SXML_TEST_REQUIRE(state, write_document(&(sxml_exi_options_document){.options = &defaults}, &writer) ==
                                 SXML_STATUS_OK);
    static const uint8_t empty[] = {0x60};
    SXML_TEST_EXPECT(state, bytes_are(&writer, (sxml_byte_view){.bytes = empty, .byte_count = sizeof empty}));
    sxml_exi_writer_release(&writer);
}

static void test_options_come_back(sxml_test_state *state) {
    const sxml_exi_options options = {.alignment = SXML_EXI_ALIGNMENT_PRE_COMPRESSION,
                                      .fidelity = {.preserves_lexical_values = true},
                                      .is_strict = true,
                                      .is_fragment = true,
                                      .has_block_size = true,
                                      .block_size = 77,
                                      .has_value_max_length = true,
                                      .value_max_length = 0,
                                      .has_value_partition_capacity = true,
                                      .value_partition_capacity = 4000000000U,
                                      .schema_identifier = {.schema_identifier_kind = SXML_EXI_SCHEMA_IDENTIFIER_TEXT,
                                                            .text = SXML_BYTE_VIEW_FROM_LITERAL("urn:x\xC3\xA9")}};
    round_trip trip = {0};
    round_trip_of(&(sxml_exi_options_document){.options = &options}, &trip);
    SXML_TEST_REQUIRE(state, trip.status == SXML_STATUS_OK);
    const sxml_exi_options *back = &trip.read_back;
    SXML_TEST_EXPECT(state, back->alignment == SXML_EXI_ALIGNMENT_PRE_COMPRESSION);
    SXML_TEST_EXPECT(state, back->fidelity.preserves_lexical_values && !back->fidelity.preserves_comments);
    SXML_TEST_EXPECT(state, back->is_strict && back->is_fragment);
    SXML_TEST_EXPECT(state, back->has_block_size && back->block_size == 77);
    SXML_TEST_EXPECT(state, back->has_value_max_length && back->value_max_length == 0);
    SXML_TEST_EXPECT(state, back->has_value_partition_capacity && back->value_partition_capacity == 4000000000U);
    SXML_TEST_EXPECT(state, back->schema_identifier.schema_identifier_kind == SXML_EXI_SCHEMA_IDENTIFIER_TEXT);
    SXML_TEST_EXPECT(state, trip.text.byte_count == 7 && memcmp(trip.text.bytes, "urn:x\xC3\xA9", 7) == 0);
    sxml_exi_buffer_release(&trip.text);

    /* Compression, selfContained and byte alignment; no schema (nil) and
       the built-in types only (the empty schemaId). */
    const sxml_exi_options compressed = {.alignment = SXML_EXI_ALIGNMENT_COMPRESSION};
    round_trip_of(&(sxml_exi_options_document){.options = &compressed, .includes_schema_identifier = true}, &trip);
    SXML_TEST_REQUIRE(state, trip.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, trip.read_back.alignment == SXML_EXI_ALIGNMENT_COMPRESSION);
    SXML_TEST_EXPECT(state,
                     trip.read_back.schema_identifier.schema_identifier_kind == SXML_EXI_SCHEMA_IDENTIFIER_NONE);
    sxml_exi_buffer_release(&trip.text);
    const sxml_exi_options typed = {.alignment = SXML_EXI_ALIGNMENT_BYTE_ALIGNMENT,
                                    .allows_self_contained = true,
                                    .schema_identifier = {.schema_identifier_kind =
                                                              SXML_EXI_SCHEMA_IDENTIFIER_BUILT_IN_TYPES}};
    round_trip_of(&(sxml_exi_options_document){.options = &typed}, &trip);
    SXML_TEST_REQUIRE(state, trip.status == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, trip.read_back.alignment == SXML_EXI_ALIGNMENT_BYTE_ALIGNMENT);
    SXML_TEST_EXPECT(state, trip.read_back.allows_self_contained);
    SXML_TEST_EXPECT(state, trip.read_back.schema_identifier.schema_identifier_kind ==
                                SXML_EXI_SCHEMA_IDENTIFIER_TEXT &&
                                trip.text.byte_count == 0);
    sxml_exi_buffer_release(&trip.text);
}

static void test_refusals(sxml_test_state *state) {
    /* A schema-informed stream whose schemaId has no text. */
    const sxml_exi_options absent = {0};
    sxml_exi_bit_writer writer;
    SXML_TEST_EXPECT(state, write_document(&(sxml_exi_options_document){.options = &absent,
                                                                         .includes_schema_identifier = true,
                                                                         .is_schema_informed = true},
                                           &writer) == SXML_STATUS_INVALID_ARGUMENT);
    sxml_exi_writer_release(&writer);
    /* strict with comments (5.4): SE (header) 0, lesscommon 00,
       preserve 01, comments 011, EE 1, EE 1, strict 01. */
    static const uint8_t strict_comments[] = {0x0B, 0xD0};
    sxml_exi_options read_back;
    sxml_exi_buffer text = {.allocator = sxml_default_allocator()};
    sxml_exi_bit_reader reader = {.input = {.bytes = strict_comments, .byte_count = sizeof strict_comments}};
    SXML_TEST_EXPECT(state, sxml_exi_read_options_document(&(sxml_exi_options_document_read){
                                .reader = &reader, .options = &read_back, .schema_identifier_text = &text}) ==
                                SXML_STATUS_MALFORMED_INPUT);
    /* A root that is not exi:header: SE (*). */
    static const uint8_t other_root[] = {0x80};
    reader = (sxml_exi_bit_reader){.input = {.bytes = other_root, .byte_count = sizeof other_root}};
    SXML_TEST_EXPECT(state, sxml_exi_read_options_document(&(sxml_exi_options_document_read){
                                .reader = &reader, .options = &read_back, .schema_identifier_text = &text}) ==
                                SXML_STATUS_MALFORMED_INPUT);
    /* A stream that ends inside the document. */
    static const uint8_t cut[] = {0x09};
    reader = (sxml_exi_bit_reader){.input = {.bytes = cut, .byte_count = sizeof cut}};
    SXML_TEST_EXPECT(state, sxml_exi_read_options_document(&(sxml_exi_options_document_read){
                                .reader = &reader, .options = &read_back, .schema_identifier_text = &text}) !=
                                SXML_STATUS_OK);
    sxml_exi_buffer_release(&text);
}

static const sxml_test_case exi_header_test_cases[] = {
    {.name = "the_bits_are_exificient_s", .function = test_the_bits_are_exificient_s},
    {.name = "options_come_back", .function = test_options_come_back},
    {.name = "refusals", .function = test_refusals},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "exi_header",
            .cases = exi_header_test_cases,
            .case_count = SXML_TEST_COUNT(exi_header_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
