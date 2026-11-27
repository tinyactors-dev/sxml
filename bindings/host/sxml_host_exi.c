/*
 * The EXI entry points of the host shim (include/sxml/exi.h; packet 8.15,
 * D333): encode a document or element node as an EXI stream into the
 * processor's output, and decode a stream into a new document. Every link
 * has them (D272).
 *
 * Golden files: sxml_host_parse and sxml_host_serialize (sxml_host.c) for
 * the shape of a call, and src/serialize_exi.c, which drives the same
 * encoder for the output method exi:exi.
 */
#include "sxml_host_internal.h"

#include "sxml/exi.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/view.h"
#include "sxml/xdm.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The public options of the host's flat ones. */
static sxml_exi_options sxml_host_exi_options_of(const sxml_host_exi_options *options) {
    sxml_exi_options exi_options;
    memset(&exi_options, 0, sizeof(exi_options));
    exi_options.alignment = (sxml_exi_alignment)options->alignment;
    exi_options.fidelity = (sxml_exi_fidelity){
        .preserves_comments = options->preserves_comments != 0,
        .preserves_processing_instructions = options->preserves_processing_instructions != 0,
        .preserves_dtd = options->preserves_dtd != 0,
        .preserves_prefixes = options->preserves_prefixes != 0,
        .preserves_lexical_values = options->preserves_lexical_values != 0};
    exi_options.is_fragment = options->is_fragment != 0;
    exi_options.is_strict = options->is_strict != 0;
    exi_options.allows_self_contained = options->allows_self_contained != 0;
    exi_options.has_block_size = options->has_block_size != 0;
    exi_options.block_size = options->block_size;
    exi_options.has_value_max_length = options->has_value_max_length != 0;
    exi_options.value_max_length = options->value_max_length;
    exi_options.has_value_partition_capacity = options->has_value_partition_capacity != 0;
    exi_options.value_partition_capacity = options->value_partition_capacity;
    exi_options.schema_identifier = (sxml_exi_schema_identifier){
        .schema_identifier_kind = SXML_EXI_SCHEMA_IDENTIFIER_ABSENT, .text = sxml_host_no_text};
    return exi_options;
}

/* The compiled schema the options name, NULL for none; a schema handle
   that has not compiled is refused. */
static sxml_status sxml_host_exi_schema_of(sxml_host_processor *processor,
                                           const sxml_host_exi_options *options,
                                           const sxml_schema **schema) {
    *schema = NULL;
    if (options->schema == NULL) {
        return SXML_STATUS_OK;
    }
    *schema = sxml_host_schema_compiled(options->schema);
    if (*schema == NULL) {
        processor->error_record.diagnostic.description =
            SXML_BYTE_VIEW_FROM_LITERAL("The schema has not compiled");
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return SXML_STATUS_OK;
}

/* The node of value, when it is one document or element node;
   SXML_STATUS_INVALID_ARGUMENT otherwise, as the encoder takes only
   those (exi.h, sxml_exi_encode_request). */
static sxml_status sxml_host_exi_root_of(sxml_host_processor *processor,
                                         const sxml_host_value *value,
                                         sxml_node_reference *root) {
    uint64_t item_count = 0;
    sxml_status status =
        sxml_sequence_count(value->sequence, &processor->error_record, &item_count);
    sxml_item item = {.representation = 0};
    if (status == SXML_STATUS_OK && item_count == 1) {
        status = sxml_sequence_get_item(
            value->sequence,
            (sxml_item_request){.item_index = 0, .error_record = &processor->error_record},
            &item);
        if (status == SXML_STATUS_OK && sxml_item_get_node(item, root) != SXML_STATUS_OK) {
            item_count = 0;
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_node_description description;
    memset(&description, 0, sizeof(description));
    if (item_count != 1 || sxml_node_describe(*root, &description) != SXML_STATUS_OK ||
        (description.node_kind != SXML_NODE_KIND_DOCUMENT &&
         description.node_kind != SXML_NODE_KIND_ELEMENT)) {
        processor->error_record.diagnostic.description = SXML_BYTE_VIEW_FROM_LITERAL(
            "EXI encodes one document or element node");
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return SXML_STATUS_OK;
}

/* One name of self_contained_names: an NCName in no namespace or an EQName
   Q{uri}local, interned in the processor's pool. INVALID_ARGUMENT when it
   is neither. */
static sxml_status sxml_host_exi_name_of(sxml_host_processor *processor, sxml_byte_view text,
                                         sxml_name_identifier *name) {
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    sxml_byte_view local_name = text;
    if (text.byte_count > 2 && text.bytes[0] == 'Q' && text.bytes[1] == '{') {
        const uint8_t *close = memchr(text.bytes + 2, '}', text.byte_count - 2);
        if (close == NULL) {
            local_name.byte_count = 0;
        } else {
            namespace_uri = (sxml_byte_view){.bytes = text.bytes + 2, .byte_count = (size_t)(close - text.bytes) - 2};
            local_name = (sxml_byte_view){.bytes = close + 1,
                                          .byte_count = text.byte_count - (size_t)(close - text.bytes) - 1};
        }
    }
    if (local_name.byte_count == 0 || memchr(local_name.bytes, ':', local_name.byte_count) != NULL ||
        memchr(local_name.bytes, '{', local_name.byte_count) != NULL) {
        processor->error_record.diagnostic.description =
            SXML_BYTE_VIEW_FROM_LITERAL("a self-contained element is named by an NCName or Q{uri}local");
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_name_pool *name_pool = sxml_processor_get_name_pool(processor->processor);
    sxml_namespace_uri_identifier namespace_identifier = {.index = 0};
    sxml_status status = sxml_name_pool_intern_namespace_uri(name_pool, namespace_uri, &namespace_identifier);
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            name_pool, &(sxml_name_request){.namespace_uri = namespace_identifier, .local_name = local_name}, name);
    }
    return status;
}

/* The self-contained element names of request, in processor memory
   (release names->element_names). */
static sxml_status sxml_host_exi_self_contained_of(sxml_host_processor *processor,
                                                   const sxml_host_exi_encode_request *request,
                                                   sxml_element_name_view *names) {
    *names = (sxml_element_name_view){.element_names = NULL, .element_name_count = 0};
    if (request->self_contained_name_count == 0) {
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        processor->allocator,
        (sxml_allocation_request){.byte_count = request->self_contained_name_count * sizeof(sxml_name_identifier),
                                  .alignment = _Alignof(sxml_name_identifier)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_name_identifier *identifiers = memory;
    for (uint32_t name_index = 0; name_index < request->self_contained_name_count && status == SXML_STATUS_OK;
         name_index++) {
        status = sxml_host_exi_name_of(processor, request->self_contained_names[name_index], &identifiers[name_index]);
    }
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(processor->allocator, memory);
        return status;
    }
    *names = (sxml_element_name_view){.element_names = identifiers,
                                      .element_name_count = request->self_contained_name_count};
    return SXML_STATUS_OK;
}

/* Encodes root with request's options to output_stream, with an encoder
   made for the call. */
static sxml_status sxml_host_exi_write(sxml_host_processor *processor,
                                       const sxml_host_exi_encode_request *request,
                                       sxml_output_stream output_stream) {
    sxml_node_reference root = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0};
    sxml_status status = sxml_host_exi_root_of(processor, request->node, &root);
    const sxml_schema *schema = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_host_exi_schema_of(processor, &request->options, &schema);
    }
    sxml_element_name_view self_contained = {.element_names = NULL, .element_name_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_host_exi_self_contained_of(processor, request, &self_contained);
    }
    sxml_exi_encoder *encoder = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_encoder_create(
            processor->allocator,
            &(sxml_exi_encoder_configuration){.allocator = processor->allocator}, &encoder);
    }
    if (status == SXML_STATUS_OK) {
        const sxml_exi_options options = sxml_host_exi_options_of(&request->options);
        status = sxml_exi_encoder_encode(
            encoder,
            &(sxml_exi_encode_request){
                .root = root,
                .options = &options,
                .schema = schema,
                .header_options = {.includes_cookie = request->includes_cookie != 0,
                                   .includes_options = request->includes_options != 0,
                                   .includes_schema_identifier = request->includes_schema_identifier != 0},
                .canonicalization = {.is_canonical = request->is_canonical != 0,
                                     .omits_options_document = request->omits_options_document != 0,
                                     .uses_utc_time = request->uses_utc_time != 0},
                .self_contained_elements = self_contained,
                .diagnostic = &processor->error_record.diagnostic},
            output_stream);
    }
    sxml_exi_encoder_destroy(encoder);
    sxml_allocator_release(processor->allocator, (void *)self_contained.element_names);
    return status;
}

uint32_t sxml_host_exi_encode(sxml_host_processor *processor,
                              const sxml_host_exi_encode_request *request) {
    sxml_host_begin(processor);
    sxml_output_stream output_stream;
    sxml_status status = sxml_host_output_begin(processor, &output_stream);
    if (status == SXML_STATUS_OK) {
        status = sxml_host_exi_write(processor, request, output_stream);
    }
    status = sxml_host_output_end(processor, status);
    if (status != SXML_STATUS_OK) {
        sxml_host_fail(processor, status);
    }
    return (uint32_t)status;
}

sxml_host_value *sxml_host_exi_parse(sxml_host_processor *processor,
                                     const sxml_host_exi_parse_request *request) {
    sxml_host_begin(processor);
    const sxml_schema *schema = NULL;
    sxml_status status = sxml_host_exi_schema_of(processor, &request->options, &schema);
    const sxml_exi_options options = sxml_host_exi_options_of(&request->options);
    sxml_item document_node = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_processor_parse_exi_document(
            processor->processor,
            &(sxml_exi_decode_request){
                .input = request->input,
                .options = &options,
                .schema = schema,
                .schema_resolver = {.state = NULL, .operations = NULL},
                .document_uri = request->document_uri,
                .base_uri = request->document_uri,
                .diagnostic = &processor->error_record.diagnostic},
            &document_node);
    }
    if (status != SXML_STATUS_OK) {
        sxml_host_fail(processor, status);
        return NULL;
    }
    return sxml_host_value_make(processor, sxml_sequence_from_item(document_node));
}

#if defined(__wasm32__)
/* The JavaScript package (src/index.ts) writes these as 32-bit words. */
_Static_assert(sizeof(sxml_host_exi_options) == 64, "EXI options: 16 words");
_Static_assert(sizeof(sxml_host_exi_encode_request) == 100, "EXI encode request: 25 words");
_Static_assert(sizeof(sxml_host_exi_parse_request) == 80, "EXI parse request: 20 words");
#endif
