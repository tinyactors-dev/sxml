/*
 * EXI in the processor (include/sxml/exi.h, include/sxml/processor.h;
 * packet 8.13, D333): the optional feature "exi", its library functions
 * exi:serialize and exi:parse in urn:sxml:exi, sxml_processor_parse_exi_
 * document, and the processor's EXI schema resolver with its compiled
 * schemas kept per schemaId as the feature's state. Not a library part of
 * the registry: its row view is the feature's library_functions, which the
 * processor serves only when its configuration names sxml_exi_feature();
 * without the feature both functions are absent (XPST0017).
 *
 * exi:serialize is the output method exi:exi (src/serialize_exi.c) into
 * an output buffer, so sequence normalization and the encoder are called
 * in one place; exi:parse is sxml_processor_parse_exi_document, the
 * decoder into an arena tree as sxml_processor_parse_document builds one.
 *
 * Golden files: src/function_library_binary.c (the feature, its rows and
 * its errors in the functions' namespace, xs:base64Binary results);
 * src/function_library_source_code.c (a document built in an arena tree
 * of the call's processor); src/processor.c (sxml_processor_parse_document).
 */
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "memory_internal.h"
#include "optional_feature_internal.h"
#include "processor_internal.h"
#include "sxml/exi.h"
#include "sxml/schema.h"
#include "sxml/serialize.h"
#include "xdm_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------- Errors */

/* An error of urn:sxml:exi, where the functions are, as the EXPath
   modules' errors are in their functions' namespaces (D333). */
typedef struct exi_error {
    sxml_byte_view local_name;
    sxml_byte_view description;
} exi_error;

/* Fills call->error_record with the error and returns
   SXML_STATUS_DYNAMIC_ERROR. The description is static text. */
static sxml_status report_error(const sxml_function_call *call, exi_error error) {
    if (call->error_record != NULL) {
        *call->error_record =
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

/* A failure of the decoder or of reading the options as the error of
   urn:sxml:exi it is; other statuses as they are. */
typedef struct exi_failure {
    sxml_status status;
    sxml_byte_view description;
} exi_failure;

static sxml_status report_failure(const sxml_function_call *call, exi_failure failure) {
    exi_error error = {.local_name = {.bytes = NULL, .byte_count = 0}, .description = failure.description};
    switch (failure.status) {
    case SXML_STATUS_MALFORMED_INPUT:
    case SXML_STATUS_UNEXPECTED_END_OF_INPUT:
        error.local_name = SXML_BYTE_VIEW_FROM_LITERAL("malformed");
        break;
    case SXML_STATUS_UNSUPPORTED_FEATURE:
        error.local_name = SXML_BYTE_VIEW_FROM_LITERAL("unsupported");
        break;
    case SXML_STATUS_LIMIT_EXCEEDED:
        error.local_name = SXML_BYTE_VIEW_FROM_LITERAL("limit-exceeded");
        break;
    case SXML_STATUS_STATIC_ERROR:
        /* The schema a schemaId names does not compile. */
        error.local_name = SXML_BYTE_VIEW_FROM_LITERAL("invalid-schema");
        break;
    case SXML_STATUS_OK:
    case SXML_STATUS_INVALID_ARGUMENT:
    case SXML_STATUS_OUT_OF_MEMORY:
    case SXML_STATUS_INSUFFICIENT_MEMORY:
    case SXML_STATUS_MISALIGNED_MEMORY:
    case SXML_STATUS_CAPACITY_EXCEEDED:
    case SXML_STATUS_ARITHMETIC_OVERFLOW:
    case SXML_STATUS_INDEX_OUT_OF_RANGE:
    case SXML_STATUS_INVALID_UTF8:
    case SXML_STATUS_INPUT_OUTPUT_FAILED:
    case SXML_STATUS_DYNAMIC_ERROR:
    case SXML_STATUS_TYPE_ERROR:
        return failure.status;
    }
    if (error.description.byte_count == 0) {
        error.description = SXML_BYTE_VIEW_FROM_LITERAL("the EXI stream cannot be decoded");
    }
    return report_error(call, error);
}

/* ------------------------------------------------- Schemas by schemaId */

/* One compiled schema: the schemaId it was asked for (a copy, the
   processor's allocator) and the schema, which the processor keeps
   (sxml_processor_keep_schema). */
typedef struct exi_schema_entry {
    uint8_t *identifier_bytes;
    size_t identifier_byte_count;
    const sxml_schema *schema;
} exi_schema_entry;

/* The feature's state in a processor: the schemas compiled so far. */
typedef struct exi_feature_state {
    exi_schema_entry *entries;
    size_t entry_count;
    size_t entry_capacity;
} exi_feature_state;

static void finalize_feature_state(void *state, sxml_allocator allocator) {
    exi_feature_state *feature_state = state;
    for (size_t entry_index = 0; entry_index < feature_state->entry_count; entry_index += 1) {
        sxml_allocator_release(allocator, feature_state->entries[entry_index].identifier_bytes);
    }
    sxml_allocator_release(allocator, feature_state->entries);
    sxml_allocator_release(allocator, feature_state);
}

/* The feature's state in processor, made at the first use; NULL in
   *state when the configuration did not name the feature. */
static sxml_status find_feature_state(sxml_processor *processor, exi_feature_state **state) {
    *state = NULL;
    if (sxml_processor_find_optional_feature(processor, SXML_OPTIONAL_FEATURE_KIND_EXI) == NULL) {
        return SXML_STATUS_OK;
    }
    *state = sxml_processor_get_optional_feature_state(processor, SXML_OPTIONAL_FEATURE_KIND_EXI);
    if (*state != NULL) {
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        processor->allocator,
        (sxml_allocation_request){.byte_count = sizeof(exi_feature_state), .alignment = alignof(exi_feature_state)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    exi_feature_state *made = memory;
    *made = (exi_feature_state){.entries = NULL, .entry_count = 0, .entry_capacity = 0};
    status = sxml_processor_set_optional_feature_state(processor, SXML_OPTIONAL_FEATURE_KIND_EXI, made);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(processor->allocator, made);
        return status;
    }
    *state = made;
    return SXML_STATUS_OK;
}

/* Records schema under identifier; the processor already keeps it. */
typedef struct exi_schema_record {
    sxml_byte_view identifier;
    const sxml_schema *schema;
} exi_schema_record;

static sxml_status record_schema(sxml_processor *processor, exi_feature_state *state, const exi_schema_record *record) {
    if (state->entry_count == state->entry_capacity) {
        const size_t capacity = state->entry_capacity == 0 ? 4U : state->entry_capacity * 2U;
        size_t byte_count = 0;
        sxml_status status = sxml_size_multiply(capacity, sizeof(exi_schema_entry), &byte_count);
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                processor->allocator,
                (sxml_allocation_request){.byte_count = byte_count, .alignment = alignof(exi_schema_entry)}, &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (state->entry_count > 0) {
            memcpy(memory, state->entries, state->entry_count * sizeof(exi_schema_entry));
        }
        sxml_allocator_release(processor->allocator, state->entries);
        state->entries = memory;
        state->entry_capacity = capacity;
    }
    void *identifier_memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        processor->allocator, (sxml_allocation_request){.byte_count = record->identifier.byte_count, .alignment = 1},
        &identifier_memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memcpy(identifier_memory, record->identifier.bytes, record->identifier.byte_count);
    state->entries[state->entry_count] = (exi_schema_entry){.identifier_bytes = identifier_memory,
                                                            .identifier_byte_count = record->identifier.byte_count,
                                                            .schema = record->schema};
    state->entry_count += 1;
    return SXML_STATUS_OK;
}

/* Compiles the schema document the resolver delivered, as
   SXML_RESOURCE_KIND_SCHEMA_DOCUMENT documents are (include/sxml/
   schema.h), and gives it to the processor to keep. */
typedef struct exi_schema_delivery {
    const sxml_resource *resource;
    sxml_byte_view identifier;
} exi_schema_delivery;

static sxml_status compile_delivered_schema(sxml_processor *processor, const exi_schema_delivery *delivery,
                                            sxml_schema **schema) {
    const sxml_resource *resource = delivery->resource;
    const bool is_node = resource->resource_form == SXML_RESOURCE_FORM_DOCUMENT_NODE;
    const sxml_schema_document document = {
        .document_source = is_node ? SXML_SCHEMA_DOCUMENT_SOURCE_NODE : SXML_SCHEMA_DOCUMENT_SOURCE_TEXT,
        .document_text = is_node ? (sxml_byte_view){.bytes = NULL, .byte_count = 0} : resource->content,
        .document_node = is_node ? resource->document_node : (sxml_item){.representation = 0},
        .document_uri = resource->resource_uri.byte_count > 0 ? resource->resource_uri : delivery->identifier,
        .checks_target_namespace = false,
        .target_namespace_uri = {.bytes = NULL, .byte_count = 0}};
    sxml_status status = sxml_schema_create(
        processor->allocator,
        &(sxml_schema_configuration){.processor = processor,
                                     .schema_documents = {.schema_documents = &document, .schema_document_count = 1},
                                     .base_uri = {.bytes = NULL, .byte_count = 0},
                                     .report_sink = {.state = NULL, .operations = NULL},
                                     .maximum_content_model_state_count = 0,
                                     .error_record = NULL,
                                     .schema_location_hint_node = {.representation = 0}},
        schema);
    if (status == SXML_STATUS_OK) {
        status = sxml_processor_keep_schema(processor, *schema);
        if (status != SXML_STATUS_OK) {
            sxml_schema_destroy(*schema);
            *schema = NULL;
        }
    }
    return status;
}

/* The resolver's resolve: a schema compiled before for the same text, or
   the one the resource resolver delivers (kind EXI_SCHEMA, the text as it
   is for both URIs), compiled and kept. */
static sxml_status resolve_schema(void *state, const sxml_exi_schema_identifier *identifier,
                                  const sxml_schema **schema) {
    sxml_processor *processor = state;
    *schema = NULL;
    if (identifier == NULL || identifier->schema_identifier_kind != SXML_EXI_SCHEMA_IDENTIFIER_TEXT) {
        return SXML_STATUS_OK;
    }
    exi_feature_state *feature_state = NULL;
    sxml_status status = find_feature_state(processor, &feature_state);
    if (status != SXML_STATUS_OK || feature_state == NULL) {
        return status;
    }
    for (size_t entry_index = 0; entry_index < feature_state->entry_count; entry_index += 1) {
        const exi_schema_entry *entry = &feature_state->entries[entry_index];
        if (sxml_byte_view_equals(
                (sxml_byte_view){.bytes = entry->identifier_bytes, .byte_count = entry->identifier_byte_count},
                identifier->text)) {
            *schema = entry->schema;
            return SXML_STATUS_OK;
        }
    }
    const sxml_resource_resolver resolver = processor->resource_resolver;
    if (resolver.operations == NULL || resolver.operations->resolve == NULL) {
        return SXML_STATUS_OK;
    }
    sxml_resource resource;
    memset(&resource, 0, sizeof(resource));
    status = resolver.operations->resolve(resolver.state,
                                          &(sxml_resource_request){.resource_kind = SXML_RESOURCE_KIND_EXI_SCHEMA,
                                                                   .uri_reference = identifier->text,
                                                                   .absolute_uri = identifier->text,
                                                                   .processor = processor},
                                          &resource);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_schema *compiled = NULL;
    if (resource.resource_form == SXML_RESOURCE_FORM_BYTES ||
        resource.resource_form == SXML_RESOURCE_FORM_DOCUMENT_NODE) {
        status = compile_delivered_schema(
            processor, &(exi_schema_delivery){.resource = &resource, .identifier = identifier->text}, &compiled);
    } else if (resource.resource_form != SXML_RESOURCE_FORM_NONE) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (resource.resource_form == SXML_RESOURCE_FORM_DOCUMENT_NODE) {
        sxml_item_release(resource.document_node);
    }
    if (resource.resource_form == SXML_RESOURCE_FORM_SEQUENCE) {
        sxml_sequence_release(resource.items);
    }
    if (resolver.operations->release != NULL) {
        resolver.operations->release(resolver.state, &resource);
    }
    if (status == SXML_STATUS_OK && compiled != NULL) {
        status = record_schema(processor, feature_state,
                               &(exi_schema_record){.identifier = identifier->text, .schema = compiled});
        *schema = compiled;
    }
    return status;
}

static const sxml_exi_schema_resolver_operations processor_schema_resolver_operations = {
    .resolve = resolve_schema,
};

sxml_exi_schema_resolver sxml_processor_exi_schema_resolver(sxml_processor *processor) {
    if (processor == NULL) {
        return (sxml_exi_schema_resolver){.state = NULL, .operations = NULL};
    }
    return (sxml_exi_schema_resolver){.state = processor, .operations = &processor_schema_resolver_operations};
}

/* ------------------------------------------------- Parsing EXI streams */

sxml_status sxml_processor_parse_exi_document(sxml_processor *processor, const sxml_exi_decode_request *request,
                                              sxml_item *document_node) {
    if (processor == NULL || request == NULL || document_node == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (sxml_processor_find_optional_feature(processor, SXML_OPTIONAL_FEATURE_KIND_EXI) == NULL) {
        return SXML_STATUS_UNSUPPORTED_FEATURE;
    }
    sxml_exi_decode_request decode_request = *request;
    if (decode_request.schema_resolver.operations == NULL) {
        decode_request.schema_resolver = sxml_processor_exi_schema_resolver(processor);
    }
    const sxml_allocator allocator = processor->allocator;
    sxml_exi_decoder *decoder = NULL;
    sxml_status status = sxml_exi_decoder_create(
        allocator,
        &(sxml_exi_decoder_configuration){.allocator = allocator,
                                          .name_pool = processor->name_pool,
                                          .limits = {.maximum_depth = processor->limits.xml_limits.maximum_depth,
                                                     .maximum_content_byte_count = 0,
                                                     .maximum_learned_production_count = 0}},
        &decoder);
    sxml_arena_tree *arena_tree = NULL;
    if (status == SXML_STATUS_OK) {
        /* The tree destroys itself with its last reference (D39). A
           decoded tree has no source text, so no locations. */
        status = sxml_arena_tree_create(
            allocator,
            &(sxml_arena_tree_configuration){.allocator = allocator,
                                             .initial_memory = {.bytes = NULL, .byte_count = 0},
                                             .item_heap = processor->item_heap,
                                             .maximum_node_count = processor->limits.maximum_tree_node_count,
                                             .maximum_depth = processor->limits.xml_limits.maximum_depth,
                                             .maximum_text_byte_count = processor->limits.maximum_tree_text_byte_count,
                                             .keeps_locations = false,
                                             .duplicate_attribute_policy = SXML_DUPLICATE_ATTRIBUTE_POLICY_REJECT,
                                             .is_destroyed_when_unreferenced = true,
                                             .error_record = NULL,
                                             .base_uri = {.bytes = NULL, .byte_count = 0}},
            &arena_tree);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_decoder_decode(decoder, &decode_request, sxml_arena_tree_as_tree_builder(arena_tree));
    }
    sxml_exi_decoder_destroy(decoder);
    sxml_tree *tree = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_arena_tree_finish(arena_tree, &tree);
    }
    if (status != SXML_STATUS_OK) {
        sxml_arena_tree_destroy(arena_tree);
        return status;
    }
    *document_node =
        sxml_node_item_make((sxml_node_reference){.tree = tree, .node_index = {.index = 0}, .namespace_number = 0});
    sxml_tree_release(tree);
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Options */

/* The processor of the call's evaluation; NULL outside one. */
static sxml_processor *processor_of(const sxml_function_call *call) {
    const sxml_evaluation_environment *environment = sxml_processor_get_evaluation_environment(call->dynamic_context);
    return environment == NULL ? NULL : environment->processor;
}

/* Reads the 'options' entry, an exi:header element (EXI 1.0, Appendix C),
   with options_reader into *options; exi:invalid-options for one the
   reader refuses. */
typedef struct header_reading {
    sxml_exi_options_reader *options_reader;
    sxml_sequence header;
} header_reading;

static sxml_status read_header(const sxml_function_call *call, const header_reading *reading,
                               sxml_exi_options *options) {
    sxml_item item = {.representation = 0};
    sxml_status status = sxml_sequence_get_item(
        reading->header, (sxml_item_request){.item_index = 0, .error_record = call->error_record}, &item);
    sxml_node_reference header = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_item_get_node(item, &header);
    }
    sxml_diagnostic diagnostic = {
        .description = {.bytes = NULL, .byte_count = 0},
        .location = {
            .source_name = {.bytes = NULL, .byte_count = 0}, .line_number = 0, .column_number = 0, .byte_offset = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_options_reader_read(
            reading->options_reader,
            &(sxml_exi_options_read_request){.header_element = header, .diagnostic = &diagnostic}, options);
    }
    if (status == SXML_STATUS_MALFORMED_INPUT) {
        return report_error(call, (exi_error){.local_name = SXML_BYTE_VIEW_FROM_LITERAL("invalid-options"),
                                              .description = diagnostic.description});
    }
    return status;
}

static sxml_status create_options_reader(const sxml_function_call *call, sxml_exi_options_reader **options_reader) {
    const sxml_allocator allocator = sxml_item_heap_as_allocator(call->item_heap);
    return sxml_exi_options_reader_create(
        allocator,
        &(sxml_exi_options_reader_configuration){.allocator = allocator,
                                                 .name_pool = sxml_item_heap_get_name_pool(call->item_heap)},
        options_reader);
}

/* ------------------------------------------------------ exi:serialize */

/* The options of exi:serialize: the exi method's parameters by local
   name (D333), and canonical. */
enum {
    SXML_EXI_SERIALIZE_OPTION_OPTIONS,
    SXML_EXI_SERIALIZE_OPTION_INCLUDE_COOKIE,
    SXML_EXI_SERIALIZE_OPTION_INCLUDE_OPTIONS,
    SXML_EXI_SERIALIZE_OPTION_INCLUDE_SCHEMA_ID,
    SXML_EXI_SERIALIZE_OPTION_CANONICAL,
    SXML_EXI_SERIALIZE_OPTION_OMIT_OPTIONS_DOCUMENT,
    SXML_EXI_SERIALIZE_OPTION_UTC_TIME,
    SXML_EXI_SERIALIZE_OPTION_SELF_CONTAINED_ELEMENTS,
    SXML_EXI_SERIALIZE_OPTION_COUNT,
};

static const sxml_option_definition serialize_options[SXML_EXI_SERIALIZE_OPTION_COUNT] = {
    SXML_FUNCTION_OPTION("options", SXML_TYPE_ELEMENT_NODE, SXML_OCCURRENCE_ZERO_OR_ONE),
    SXML_FUNCTION_OPTION("include-cookie", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("include-options", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("include-schema-id", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("canonical", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("omit-options-document", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("utc-time", SXML_TYPE_XS_BOOLEAN, SXML_OCCURRENCE_EXACTLY_ONE),
    SXML_FUNCTION_OPTION("self-contained-elements", SXML_TYPE_XS_QNAME, SXML_OCCURRENCE_ZERO_OR_MORE),
};

static bool option_is_true(const sxml_option_values *values, size_t option_index) {
    return sxml_function_library_option_is_true((sxml_option_lookup){.values = values, .option_index = option_index});
}

/* The names of exi:self-contained-elements, in item heap memory
   (*names, released by the caller). */
static sxml_status read_element_names(const sxml_function_call *call, sxml_sequence sequence,
                                      sxml_name_identifier **names) {
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(sequence, call->error_record, &item_count);
    if (status != SXML_STATUS_OK || item_count == 0) {
        return status;
    }
    if (item_count > SIZE_MAX / sizeof(sxml_name_identifier)) {
        return SXML_STATUS_ARITHMETIC_OVERFLOW;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate_array(
        sxml_item_heap_as_allocator(call->item_heap),
        (sxml_array_allocation_request){.element_count = (size_t)item_count,
                                        .element_size = sizeof(sxml_name_identifier),
                                        .element_alignment = alignof(sxml_name_identifier)},
        &memory);
    sxml_name_identifier *identifiers = memory;
    for (uint64_t item_index = 0; status == SXML_STATUS_OK && item_index < item_count; item_index += 1) {
        sxml_item item = {.representation = 0};
        sxml_atomic_datum datum;
        status = sxml_sequence_get_item(
            sequence, (sxml_item_request){.item_index = item_index, .error_record = call->error_record}, &item);
        if (status == SXML_STATUS_OK) {
            status = sxml_atomic_item_get_datum(item, &datum);
        }
        if (status == SXML_STATUS_OK) {
            identifiers[item_index] = datum.datum_details.qname.expanded_name;
        }
    }
    *names = identifiers;
    return status;
}

/* What exi:serialize writes with. */
typedef struct serialize_setup {
    sxml_exi_serialization_parameters parameters;
    sxml_exi_options_reader *options_reader;
    sxml_name_identifier *element_names;
} serialize_setup;

static sxml_status read_serialize_options(const sxml_function_call *call, serialize_setup *setup) {
    sxml_option_values values;
    memset(&values, 0, sizeof(values));
    sxml_status status = sxml_function_library_read_options(
        call,
        &(sxml_option_request){.argument_index = 1,
                               .definitions = {serialize_options, SXML_EXI_SERIALIZE_OPTION_COUNT}},
        &values);
    sxml_exi_serialization_parameters *parameters = &setup->parameters;
    if (status == SXML_STATUS_OK) {
        parameters->header_options = (sxml_exi_header_options){
            .includes_cookie = option_is_true(&values, SXML_EXI_SERIALIZE_OPTION_INCLUDE_COOKIE),
            .includes_options = option_is_true(&values, SXML_EXI_SERIALIZE_OPTION_INCLUDE_OPTIONS),
            .includes_schema_identifier = option_is_true(&values, SXML_EXI_SERIALIZE_OPTION_INCLUDE_SCHEMA_ID)};
        parameters->canonicalization = (sxml_exi_canonicalization){
            .is_canonical = option_is_true(&values, SXML_EXI_SERIALIZE_OPTION_CANONICAL),
            .omits_options_document = option_is_true(&values, SXML_EXI_SERIALIZE_OPTION_OMIT_OPTIONS_DOCUMENT),
            .uses_utc_time = option_is_true(&values, SXML_EXI_SERIALIZE_OPTION_UTC_TIME)};
    }
    if (status == SXML_STATUS_OK && values.is_present[SXML_EXI_SERIALIZE_OPTION_SELF_CONTAINED_ELEMENTS]) {
        status = read_element_names(call, values.values[SXML_EXI_SERIALIZE_OPTION_SELF_CONTAINED_ELEMENTS],
                                    &setup->element_names);
        uint64_t name_count = 0;
        if (status == SXML_STATUS_OK) {
            status = sxml_sequence_count(values.values[SXML_EXI_SERIALIZE_OPTION_SELF_CONTAINED_ELEMENTS],
                                         call->error_record, &name_count);
        }
        parameters->self_contained_elements =
            (sxml_element_name_view){.element_names = setup->element_names, .element_name_count = (size_t)name_count};
    }
    uint64_t header_count = 0;
    if (status == SXML_STATUS_OK && values.is_present[SXML_EXI_SERIALIZE_OPTION_OPTIONS]) {
        status =
            sxml_sequence_count(values.values[SXML_EXI_SERIALIZE_OPTION_OPTIONS], call->error_record, &header_count);
    }
    if (status == SXML_STATUS_OK && header_count == 1) {
        status = create_options_reader(call, &setup->options_reader);
        if (status == SXML_STATUS_OK) {
            status = read_header(call,
                                 &(header_reading){.options_reader = setup->options_reader,
                                                   .header = values.values[SXML_EXI_SERIALIZE_OPTION_OPTIONS]},
                                 &parameters->options);
        }
    }
    sxml_function_library_release_options(&values);
    return status;
}

/* Makes an xs:base64Binary item of octets (copied) as *result. */
static sxml_status make_binary(const sxml_function_call *call, sxml_byte_view octets, sxml_sequence *result) {
    sxml_item item = {.representation = 0};
    const sxml_status status =
        sxml_atomic_item_make(call->item_heap,
                              &(sxml_atomic_item_request){
                                  .type = sxml_builtin_type_identifier(SXML_TYPE_XS_BASE64_BINARY),
                                  .datum = {.datum_kind = SXML_DATUM_KIND_BINARY, .datum_details = {.binary = octets}}},
                              &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

/* exi:serialize($input as item()*, $options as map(*)? := {}) as
   xs:base64Binary (D333): the bytes the output method exi:exi writes for
   $input. */
static sxml_status call_serialize(const sxml_function_call *call, sxml_sequence *result) {
    sxml_processor *processor = processor_of(call);
    if (processor == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    serialize_setup setup;
    memset(&setup, 0, sizeof(setup));
    setup.parameters.schema_resolver = sxml_processor_exi_schema_resolver(processor);
    sxml_status status = read_serialize_options(call, &setup);
    const sxml_allocator allocator = sxml_item_heap_as_allocator(call->item_heap);
    sxml_output_buffer *output = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_output_buffer_create(
            allocator, &(sxml_output_buffer_configuration){.allocator = allocator, .maximum_byte_count = 0}, &output);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_serialize_sequence(
            call->item_heap,
            &(sxml_serialization_request){
                .value = sxml_function_library_get_argument(call, 0),
                .serialization_parameters = {.serialization_method = SXML_SERIALIZATION_METHOD_EXI,
                                             .exi_parameters = &setup.parameters},
                .error_record = call->error_record},
            sxml_output_buffer_as_output_stream(output));
    }
    if (status == SXML_STATUS_OK) {
        status = make_binary(call, sxml_output_buffer_get_bytes(output), result);
    }
    sxml_output_buffer_destroy(output);
    sxml_exi_options_reader_destroy(setup.options_reader);
    sxml_allocator_release(allocator, setup.element_names);
    return status;
}

/* ---------------------------------------------------------- exi:parse */

enum {
    SXML_EXI_PARSE_OPTION_OPTIONS,
    SXML_EXI_PARSE_OPTION_COUNT,
};

static const sxml_option_definition parse_options[SXML_EXI_PARSE_OPTION_COUNT] = {
    SXML_FUNCTION_OPTION("options", SXML_TYPE_ELEMENT_NODE, SXML_OCCURRENCE_ZERO_OR_ONE),
};

/* exi:parse($stream as xs:base64Binary?, $options as map(*)? := {}) as
   document-node()? (D333): the document the stream holds, its options
   the 'options' entry when its header carries none (EXI 1.0, 5.4). */
static sxml_status call_parse(const sxml_function_call *call, sxml_sequence *result) {
    sxml_processor *processor = processor_of(call);
    if (processor == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_optional_item stream = {.has_item = false};
    sxml_status status = sxml_function_library_read_optional_item(call, 0, &stream);
    if (status != SXML_STATUS_OK || !stream.has_item) {
        *result = sxml_sequence_empty();
        return status;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(stream.item, &datum);
    if (status == SXML_STATUS_OK && datum.datum_kind != SXML_DATUM_KIND_BINARY) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_option_values values;
    memset(&values, 0, sizeof(values));
    if (status == SXML_STATUS_OK) {
        status = sxml_function_library_read_options(
            call,
            &(sxml_option_request){.argument_index = 1, .definitions = {parse_options, SXML_EXI_PARSE_OPTION_COUNT}},
            &values);
    }
    uint64_t header_count = 0;
    if (status == SXML_STATUS_OK && values.is_present[SXML_EXI_PARSE_OPTION_OPTIONS]) {
        status = sxml_sequence_count(values.values[SXML_EXI_PARSE_OPTION_OPTIONS], call->error_record, &header_count);
    }
    sxml_exi_options_reader *options_reader = NULL;
    sxml_exi_options options;
    memset(&options, 0, sizeof(options));
    if (status == SXML_STATUS_OK && header_count == 1) {
        status = create_options_reader(call, &options_reader);
        if (status == SXML_STATUS_OK) {
            status = read_header(call,
                                 &(header_reading){.options_reader = options_reader,
                                                   .header = values.values[SXML_EXI_PARSE_OPTION_OPTIONS]},
                                 &options);
        }
    }
    sxml_diagnostic diagnostic = {
        .description = {.bytes = NULL, .byte_count = 0},
        .location = {
            .source_name = {.bytes = NULL, .byte_count = 0}, .line_number = 0, .column_number = 0, .byte_offset = 0}};
    sxml_item document_node = {.representation = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_processor_parse_exi_document(
            processor,
            &(sxml_exi_decode_request){.input = datum.datum_details.binary,
                                       .options = header_count == 1 ? &options : NULL,
                                       .schema = NULL,
                                       .schema_resolver = sxml_processor_exi_schema_resolver(processor),
                                       .document_uri = {.bytes = NULL, .byte_count = 0},
                                       .base_uri = {.bytes = NULL, .byte_count = 0},
                                       .diagnostic = &diagnostic},
            &document_node);
        if (status != SXML_STATUS_OK) {
            status = report_failure(call, (exi_failure){.status = status, .description = diagnostic.description});
        }
    }
    sxml_exi_options_reader_destroy(options_reader);
    sxml_function_library_release_options(&values);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(document_node);
    }
    return status;
}

/* ------------------------------------------------------------ Feature */

static const sxml_function_parameter serialize_parameters[] = {
    SXML_REQUIRED_PARAMETER("input", "item()*"),
    SXML_OPTIONAL_PARAMETER("options", "map(*)?"),
};

static const sxml_function_parameter parse_parameters[] = {
    SXML_REQUIRED_PARAMETER("stream", "xs:base64Binary?"),
    SXML_OPTIONAL_PARAMETER("options", "map(*)?"),
};

/* A row in the urn:sxml:exi namespace. Both read the processor of the
   evaluation (its resolver); exi:parse makes new nodes. */
#define SXML_EXI_ROW(name_text, parameter_array, result_text, row_properties, body)                                    \
    {                                                                                                                  \
        .function_namespace = SXML_NAMESPACE_EXI, .properties = (row_properties),                                      \
        .local_name = SXML_BYTE_VIEW_INITIALIZER(name_text),                                                           \
        .parameters = {(parameter_array), sizeof(parameter_array) / sizeof((parameter_array)[0])},                     \
        .result_type = SXML_BYTE_VIEW_INITIALIZER(result_text), .call = (body), .resumable_body = NULL                 \
    }

static const sxml_native_function exi_functions[] = {
    SXML_EXI_ROW("serialize", serialize_parameters, "xs:base64Binary", SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT,
                 call_serialize),
    SXML_EXI_ROW("parse", parse_parameters, "document-node()?",
                 SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT | SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC, call_parse),
};

/* Not named sxml_function_library_<part>: not a registered part (D191). */
static const sxml_native_function_view exi_library_functions = SXML_LIBRARY_PART(exi_functions);

const sxml_optional_feature_definition sxml_exi_feature_definition = {
    .feature_kind = SXML_OPTIONAL_FEATURE_KIND_EXI,
    .feature_name = SXML_BYTE_VIEW_INITIALIZER("exi"),
    .operations = &exi_library_functions,
    .finalize_state = finalize_feature_state,
    .library_functions = &exi_library_functions,
};

sxml_optional_feature sxml_exi_feature(void) {
    return (sxml_optional_feature){.definition = &sxml_exi_feature_definition};
}
