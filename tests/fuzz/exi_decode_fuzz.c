/*
 * Fuzz target: the EXI decoder (include/sxml/exi.h; packet 8.16, D333).
 *
 * The first byte of the input picks the out-of-band options: bits 0 and 1
 * the alignment (bit-packed, byte alignment, pre-compression, compression),
 * bits 2 to 5 the fidelity options comments, processing instructions,
 * prefixes and lexical values, bit 6 fragment, bit 7 a block size of 4;
 * the rest is the stream. It is decoded into an arena tree with tight
 * limits. When it decodes, the tree is encoded again with the same options
 * and that stream must decode too, to a tree of the same node count: the
 * encoder must write what the decoder reads. Every allocation goes through
 * the fixture's counting allocator, so a leak stops the fuzzer.
 *
 * Golden file: tests/fuzz/xml_parse_fuzz.c.
 */
#include "fuzz_fixture.h"
#include "sxml/exi.h"
#include "sxml/serialize.h"
#include "sxml/tree.h"

#include <stdio.h>
#include <stdlib.h>

/* The options the first byte picks. */
static sxml_exi_options exi_fuzz_options(uint8_t selector) {
    static const sxml_exi_alignment alignments[4] = {
        SXML_EXI_ALIGNMENT_BIT_PACKED, SXML_EXI_ALIGNMENT_BYTE_ALIGNMENT, SXML_EXI_ALIGNMENT_PRE_COMPRESSION,
        SXML_EXI_ALIGNMENT_COMPRESSION};
    return (sxml_exi_options){
        .alignment = alignments[selector & 3U],
        .fidelity = {.preserves_comments = (selector & 4U) != 0,
                     .preserves_processing_instructions = (selector & 8U) != 0,
                     .preserves_prefixes = (selector & 16U) != 0,
                     .preserves_lexical_values = (selector & 32U) != 0},
        .is_fragment = (selector & 64U) != 0,
        .has_block_size = (selector & 128U) != 0,
        .block_size = 4,
    };
}

/* A stream to decode with options, into a tree of fixture's heap. */
typedef struct exi_fuzz_decoding {
    sxml_exi_decoder *decoder;
    const sxml_exi_options *options;
    sxml_byte_view stream;
} exi_fuzz_decoding;

/* Decodes; *tree is NULL when the stream is refused. */
static void exi_fuzz_decode(const sxml_fuzz_fixture *fixture, const exi_fuzz_decoding *decoding, sxml_tree **tree) {
    *tree = NULL;
    const sxml_allocator allocator = sxml_fuzz_fixture_allocator(fixture);
    sxml_arena_tree *arena_tree = NULL;
    sxml_status status = sxml_arena_tree_create(
        allocator,
        &(sxml_arena_tree_configuration){.allocator = allocator,
                                         .item_heap = fixture->item_heap,
                                         .maximum_node_count = 100000,
                                         .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
                                         .is_destroyed_when_unreferenced = true},
        &arena_tree);
    sxml_fuzz_check_status(status, "sxml_arena_tree_create");
    if (status != SXML_STATUS_OK) {
        return;
    }
    sxml_diagnostic diagnostic = {.description = {.byte_count = 0}};
    status = sxml_exi_decoder_decode(decoding->decoder,
                                     &(sxml_exi_decode_request){.input = decoding->stream,
                                                                .options = decoding->options,
                                                                .diagnostic = &diagnostic},
                                     sxml_arena_tree_as_tree_builder(arena_tree));
    sxml_fuzz_check_status(status, "sxml_exi_decoder_decode");
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, tree);
        sxml_fuzz_check_status(status, "sxml_arena_tree_finish");
    }
    if (status != SXML_STATUS_OK) {
        *tree = NULL;
        sxml_arena_tree_destroy(arena_tree);
    }
    /* Otherwise the caller holds the tree's one reference; releasing it
       destroys the arena tree. */
}

static uint32_t exi_fuzz_node_count(sxml_tree *tree) {
    sxml_tree_description description = {.node_count = 0};
    sxml_fuzz_check_status(sxml_tree_describe(tree, &description), "sxml_tree_describe");
    return description.node_count;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) { // NOLINT(readability-identifier-naming)
    if (size < 1) {
        return 0;
    }
    sxml_fuzz_fixture fixture;
    if (sxml_fuzz_fixture_create(&(sxml_fuzz_fixture_configuration){.has_function_library = false}, &fixture) !=
        SXML_STATUS_OK) {
        return 0;
    }
    const sxml_allocator allocator = sxml_fuzz_fixture_allocator(&fixture);
    const sxml_exi_options options = exi_fuzz_options(data[0]);
    sxml_exi_decoder *decoder = NULL;
    sxml_exi_encoder *encoder = NULL;
    sxml_status status = sxml_exi_decoder_create(
        allocator,
        &(sxml_exi_decoder_configuration){.allocator = allocator,
                                          .name_pool = fixture.name_pool,
                                          .limits = {.maximum_depth = 256,
                                                     .maximum_content_byte_count = 1U << 20U,
                                                     .maximum_learned_production_count = 100000}},
        &decoder);
    sxml_fuzz_check_status(status, "sxml_exi_decoder_create");
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_encoder_create(allocator, &(sxml_exi_encoder_configuration){.allocator = allocator}, &encoder);
        sxml_fuzz_check_status(status, "sxml_exi_encoder_create");
    }
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        exi_fuzz_decode(&fixture,
                        &(exi_fuzz_decoding){.decoder = decoder,
                                             .options = &options,
                                             .stream = {.bytes = data + 1, .byte_count = size - 1}},
                        &tree);
    }
    if (tree != NULL) {
        sxml_output_buffer *output_buffer = NULL;
        status = sxml_output_buffer_create(allocator, &(sxml_output_buffer_configuration){.allocator = allocator},
                                           &output_buffer);
        sxml_fuzz_check_status(status, "sxml_output_buffer_create");
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_encoder_encode(
                encoder, &(sxml_exi_encode_request){.root = {.tree = tree}, .options = &options},
                sxml_output_buffer_as_output_stream(output_buffer));
            /* A tree the decoder made is one the encoder must write. */
            if (status != SXML_STATUS_OK && status != SXML_STATUS_OUT_OF_MEMORY) {
                (void)fprintf(stderr, "a decoded tree does not encode: %d\n", (int)status);
                abort();
            }
        }
        sxml_tree *again = NULL;
        if (status == SXML_STATUS_OK) {
            exi_fuzz_decode(&fixture,
                            &(exi_fuzz_decoding){.decoder = decoder,
                                                 .options = &options,
                                                 .stream = sxml_output_buffer_get_bytes(output_buffer)},
                            &again);
            if (again == NULL || exi_fuzz_node_count(again) != exi_fuzz_node_count(tree)) {
                (void)fprintf(stderr, "an encoded tree does not decode to its node count\n");
                abort();
            }
            sxml_tree_release(again);
        }
        sxml_output_buffer_destroy(output_buffer);
        sxml_tree_release(tree);
    }
    sxml_exi_encoder_destroy(encoder);
    sxml_exi_decoder_destroy(decoder);
    sxml_fuzz_fixture_destroy_checking_leaks(&fixture);
    return 0;
}
