/*
 * The exi output method, Q{urn:sxml:exi}exi (include/sxml/serialize.h,
 * SXML_SERIALIZATION_METHOD_EXI; include/sxml/exi.h; D333): sequence
 * normalization (Serialization 4.0, section 2) into a document, then the
 * EXI encoder over it, its bytes straight to the output stream. An
 * implementation-defined method may skip the phases that make octets of
 * characters (Serialization 4.0, sections 3 and 4), so none of the
 * character pipeline applies.
 *
 * Golden file: src/xslt_output.c (sxml_xslt_build_result_tree: the
 * serializer's normalizer feeding an arena tree, nothing of it copied).
 */
#include "serialize_internal.h"

#include "sxml/exi.h"
#include "sxml/tree.h"
#include "xdm_internal.h"

#include <string.h>

/* An error of the method, in urn:sxml:exi (D333), as the exi functions
   raise them: description is static text. */
typedef struct sxml_exi_method_error {
    sxml_byte_view local_name;
    sxml_byte_view description;
} sxml_exi_method_error;

static sxml_status sxml_exi_method_raise(const sxml_serializer *serializer, sxml_exi_method_error error) {
    if (serializer->error_record != NULL) {
        *serializer->error_record =
            (sxml_error_record){.code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL("urn:sxml:exi"),
                                         .local_name = error.local_name,
                                         .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("exi")},
                                .value = NULL,
                                .diagnostic = {.description = error.description,
                                               .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                                                            .line_number = 0,
                                                            .column_number = 0,
                                                            .byte_offset = 0}}};
    }
    return SXML_STATUS_DYNAMIC_ERROR;
}

/* The document the method encodes: the value itself when it is one
   document node (normalization would copy its children into a new
   document, the same tree), else a new document of the normalized
   sequence. Owned; release it. */
static sxml_status sxml_exi_method_document_of(sxml_serializer *serializer, sxml_sequence value, sxml_item *document) {
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(value, serializer->error_record, &item_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (item_count == 1) {
        sxml_item item = {.representation = 0};
        status = sxml_sequence_get_item(
            value, (sxml_item_request){.item_index = 0, .error_record = serializer->error_record}, &item);
        sxml_node_reference node = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0};
        sxml_node_description description;
        memset(&description, 0, sizeof(description));
        description.node_kind = SXML_NODE_KIND_TEXT;
        if (status == SXML_STATUS_OK && sxml_item_get_kind(item) == SXML_ITEM_KIND_NODE) {
            status = sxml_item_get_node(item, &node);
            if (status == SXML_STATUS_OK) {
                status = sxml_node_describe(node, &description);
            }
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (node.tree != NULL && description.node_kind == SXML_NODE_KIND_DOCUMENT) {
            sxml_item_retain(item);
            *document = item;
            return SXML_STATUS_OK;
        }
    }
    const sxml_allocator allocator = serializer->allocator;
    sxml_arena_tree *arena_tree = NULL;
    status = sxml_arena_tree_create(
        allocator,
        &(sxml_arena_tree_configuration){.allocator = allocator,
                                         .initial_memory = {.bytes = NULL, .byte_count = 0},
                                         .item_heap = serializer->item_heap,
                                         .maximum_node_count = 0,
                                         .maximum_depth = 0,
                                         .maximum_text_byte_count = 0,
                                         .keeps_locations = false,
                                         .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REPLACE,
                                         .is_destroyed_when_unreferenced = true,
                                         .error_record = serializer->error_record,
                                         .base_uri = {.bytes = NULL, .byte_count = 0}},
        &arena_tree);
    const sxml_source_location no_location = {
        .source_name = {.bytes = NULL, .byte_count = 0}, .line_number = 0, .column_number = 0, .byte_offset = 0};
    sxml_tree_builder builder = {.state = NULL, .operations = NULL};
    if (status == SXML_STATUS_OK) {
        builder = sxml_arena_tree_as_tree_builder(arena_tree);
        status = builder.operations->start_document(
            builder.state, &(sxml_document_start_event){.document_uri = {.bytes = NULL, .byte_count = 0},
                                                        .base_uri = {.bytes = NULL, .byte_count = 0},
                                                        .location = no_location});
    }
    sxml_sequence_normalizer normalizer;
    sxml_sequence_normalizer_start(serializer, value, &normalizer);
    while (status == SXML_STATUS_OK) {
        sxml_normalized_event event = {.event_kind = SXML_NORMALIZED_EVENT_KIND_END};
        status = sxml_sequence_normalizer_next(&normalizer, &event);
        if (status != SXML_STATUS_OK || event.event_kind == SXML_NORMALIZED_EVENT_KIND_END) {
            break;
        }
        if (event.event_kind == SXML_NORMALIZED_EVENT_KIND_TEXT) {
            status = builder.operations->add_text(builder.state,
                                                  &(sxml_text_event){.text = event.text, .location = no_location});
        } else {
            status = sxml_tree_builder_copy_node(builder, &(sxml_copy_request){.node = event.node,
                                                                               .preserves_namespaces = true,
                                                                               .inherits_namespaces = true,
                                                                               .preserves_type_annotations = false});
        }
    }
    sxml_sequence_normalizer_finish(&normalizer);
    if (status == SXML_STATUS_OK) {
        status = builder.operations->end_document(builder.state);
    }
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *document =
        sxml_node_item_make((sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0});
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

/* The schema of the encode: the parameters', or the one their resolver
   finds for a schemaId; NULL for none. */
static sxml_status sxml_exi_method_find_schema(const sxml_exi_serialization_parameters *parameters,
                                               const sxml_schema **schema) {
    *schema = parameters->schema;
    const sxml_exi_schema_resolver resolver = parameters->schema_resolver;
    if (*schema != NULL ||
        parameters->options.schema_identifier.schema_identifier_kind != SXML_EXI_SCHEMA_IDENTIFIER_TEXT ||
        resolver.operations == NULL || resolver.operations->resolve == NULL) {
        return SXML_STATUS_OK;
    }
    return resolver.operations->resolve(resolver.state, &parameters->options.schema_identifier, schema);
}

/* What the encoder's refusals are as errors of the method. */
static sxml_status sxml_exi_method_report(const sxml_serializer *serializer, sxml_exi_method_error error,
                                          sxml_status status) {
    if (status == SXML_STATUS_DYNAMIC_ERROR) {
        error.local_name = SXML_BYTE_VIEW_FROM_LITERAL("unrepresentable");
    } else if (status == SXML_STATUS_INVALID_ARGUMENT) {
        error.local_name = SXML_BYTE_VIEW_FROM_LITERAL("invalid-options");
    } else if (status == SXML_STATUS_UNSUPPORTED_FEATURE) {
        error.local_name = SXML_BYTE_VIEW_FROM_LITERAL("unsupported");
    } else {
        return status;
    }
    if (error.description.byte_count == 0) {
        error.description = SXML_BYTE_VIEW_FROM_LITERAL("the EXI encoder refused the document");
    }
    return sxml_exi_method_raise(serializer, error);
}

sxml_status sxml_serialize_exi_method(sxml_serializer *serializer, sxml_sequence value) {
    static const sxml_exi_serialization_parameters default_parameters;
    const sxml_serialization_parameters *parameters = serializer->parameters;
    sxml_exi_serialization_parameters exi =
        parameters->exi_parameters != NULL ? *parameters->exi_parameters : default_parameters;
    /* D333: Serialization 4.0's canonical parameter means Canonical EXI
       for this method. */
    if (parameters->canonical == SXML_SERIALIZATION_FLAG_YES) {
        exi.canonicalization.is_canonical = true;
    }
    const sxml_schema *schema = NULL;
    sxml_status status = sxml_exi_method_find_schema(&exi, &schema);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (schema == NULL && exi.options.schema_identifier.schema_identifier_kind == SXML_EXI_SCHEMA_IDENTIFIER_TEXT) {
        return sxml_exi_method_raise(
            serializer,
            (sxml_exi_method_error){.local_name = SXML_BYTE_VIEW_FROM_LITERAL("unsupported"),
                                    .description = SXML_BYTE_VIEW_FROM_LITERAL(
                                        "no schema is known for the schemaId of the EXI options (EXI 1.0, 5.4)")});
    }
    sxml_item document = {.representation = 0};
    status = sxml_exi_method_document_of(serializer, value, &document);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_node_reference root = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0};
    status = sxml_item_get_node(document, &root);
    sxml_exi_encoder *encoder = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_encoder_create(
            serializer->allocator, &(sxml_exi_encoder_configuration){.allocator = serializer->allocator}, &encoder);
    }
    sxml_diagnostic diagnostic = {
        .description = {.bytes = NULL, .byte_count = 0},
        .location = {
            .source_name = {.bytes = NULL, .byte_count = 0}, .line_number = 0, .column_number = 0, .byte_offset = 0}};
    if (status == SXML_STATUS_OK) {
        status =
            sxml_exi_encoder_encode(encoder,
                                    &(sxml_exi_encode_request){.root = root,
                                                               .options = &exi.options,
                                                               .schema = schema,
                                                               .header_options = exi.header_options,
                                                               .canonicalization = exi.canonicalization,
                                                               .self_contained_elements = exi.self_contained_elements,
                                                               .diagnostic = &diagnostic},
                                    serializer->output_stream);
        status = sxml_exi_method_report(serializer,
                                        (sxml_exi_method_error){.local_name = {.bytes = NULL, .byte_count = 0},
                                                                .description = diagnostic.description},
                                        status);
    }
    sxml_exi_encoder_destroy(encoder);
    sxml_item_release(document);
    return status;
}
