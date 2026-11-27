/*
 * EXI: the decoder, the encoder and the options reader of
 * include/sxml/exi.h (EXI 1.0 Second Edition; Canonical EXI; D333). This
 * file holds their lifecycles, which follow src/xml.c's parser; the codec
 * itself is in the exi_*.c modules Phase 8 adds (docs/tasks/phase-8.md),
 * and the options reader, which reads the EXI options document of an
 * exi:header element (EXI 1.0, Appendix C; packet 8.13).
 */
#include <stdalign.h>
#include <string.h>

#include "exi_internal.h"
#include "sxml/tree.h"

sxml_exi_limits sxml_exi_limits_resolve(sxml_exi_limits limits) {
    if (limits.maximum_depth == 0) {
        limits.maximum_depth = SXML_EXI_DEFAULT_MAXIMUM_DEPTH;
    }
    if (limits.maximum_content_byte_count == 0) {
        limits.maximum_content_byte_count = sxml_exi_default_maximum_content_byte_count;
    }
    if (limits.maximum_learned_production_count == 0) {
        limits.maximum_learned_production_count = SXML_EXI_DEFAULT_MAXIMUM_LEARNED_PRODUCTION_COUNT;
    }
    return limits;
}

/* Allocates requirements through allocator; the caller initializes. */
static sxml_status sxml_exi_allocate_object(sxml_allocator allocator, sxml_memory_requirements requirements,
                                            void **memory) {
    return sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = requirements.byte_count, .alignment = requirements.alignment},
        memory);
}

/* ---------------------------------------------------------------- Decoder */

static bool sxml_exi_decoder_configuration_is_valid(const sxml_exi_decoder_configuration *configuration) {
    return configuration != NULL && !sxml_allocator_is_null(configuration->allocator) &&
           configuration->name_pool != NULL;
}

sxml_status sxml_exi_decoder_memory_requirements(const sxml_exi_decoder_configuration *configuration,
                                                 sxml_memory_requirements *requirements) {
    if (!sxml_exi_decoder_configuration_is_valid(configuration) || requirements == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){.byte_count = sizeof(struct sxml_exi_decoder),
                                               .alignment = alignof(struct sxml_exi_decoder)};
    return SXML_STATUS_OK;
}

sxml_status sxml_exi_decoder_initialize(sxml_memory_view memory, const sxml_exi_decoder_configuration *configuration,
                                        sxml_exi_decoder **decoder) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_exi_decoder_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK || decoder == NULL) {
        return status == SXML_STATUS_OK ? SXML_STATUS_INVALID_ARGUMENT : status;
    }
    status = sxml_memory_view_check_requirements(memory, requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_exi_decoder *placed = placement;
    *placed = (sxml_exi_decoder){
        .configuration = *configuration,
        .owning_allocator = {.state = NULL, .operations = NULL},
    };
    placed->configuration.limits = sxml_exi_limits_resolve(configuration->limits);
    *decoder = placed;
    return SXML_STATUS_OK;
}

void sxml_exi_decoder_finalize(sxml_exi_decoder *decoder) {
    (void)decoder;
}

sxml_status sxml_exi_decoder_create(sxml_allocator allocator, const sxml_exi_decoder_configuration *configuration,
                                    sxml_exi_decoder **decoder) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_exi_decoder_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK || decoder == NULL) {
        return status == SXML_STATUS_OK ? SXML_STATUS_INVALID_ARGUMENT : status;
    }
    void *memory = NULL;
    status = sxml_exi_allocate_object(allocator, requirements, &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_exi_decoder *created = NULL;
    status = sxml_exi_decoder_initialize((sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count},
                                         configuration, &created);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    created->owning_allocator = allocator;
    *decoder = created;
    return SXML_STATUS_OK;
}

void sxml_exi_decoder_destroy(sxml_exi_decoder *decoder) {
    if (decoder == NULL) {
        return;
    }
    const sxml_allocator allocator = decoder->owning_allocator;
    sxml_exi_decoder_finalize(decoder);
    sxml_allocator_release(allocator, decoder);
}

sxml_status sxml_exi_decoder_decode(sxml_exi_decoder *decoder, const sxml_exi_decode_request *request,
                                    sxml_tree_builder builder) {
    if (decoder == NULL || request == NULL || builder.operations == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_exi_decode_stream(decoder, request, builder);
}

/* ---------------------------------------------------------------- Encoder */

static bool sxml_exi_encoder_configuration_is_valid(const sxml_exi_encoder_configuration *configuration) {
    return configuration != NULL && !sxml_allocator_is_null(configuration->allocator);
}

sxml_status sxml_exi_encoder_memory_requirements(const sxml_exi_encoder_configuration *configuration,
                                                 sxml_memory_requirements *requirements) {
    if (!sxml_exi_encoder_configuration_is_valid(configuration) || requirements == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){.byte_count = sizeof(struct sxml_exi_encoder),
                                               .alignment = alignof(struct sxml_exi_encoder)};
    return SXML_STATUS_OK;
}

sxml_status sxml_exi_encoder_initialize(sxml_memory_view memory, const sxml_exi_encoder_configuration *configuration,
                                        sxml_exi_encoder **encoder) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_exi_encoder_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK || encoder == NULL) {
        return status == SXML_STATUS_OK ? SXML_STATUS_INVALID_ARGUMENT : status;
    }
    status = sxml_memory_view_check_requirements(memory, requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_exi_encoder *placed = placement;
    *placed = (sxml_exi_encoder){
        .configuration = *configuration,
        .owning_allocator = {.state = NULL, .operations = NULL},
    };
    *encoder = placed;
    return SXML_STATUS_OK;
}

void sxml_exi_encoder_finalize(sxml_exi_encoder *encoder) {
    (void)encoder;
}

sxml_status sxml_exi_encoder_create(sxml_allocator allocator, const sxml_exi_encoder_configuration *configuration,
                                    sxml_exi_encoder **encoder) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_exi_encoder_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK || encoder == NULL) {
        return status == SXML_STATUS_OK ? SXML_STATUS_INVALID_ARGUMENT : status;
    }
    void *memory = NULL;
    status = sxml_exi_allocate_object(allocator, requirements, &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_exi_encoder *created = NULL;
    status = sxml_exi_encoder_initialize((sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count},
                                         configuration, &created);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    created->owning_allocator = allocator;
    *encoder = created;
    return SXML_STATUS_OK;
}

void sxml_exi_encoder_destroy(sxml_exi_encoder *encoder) {
    if (encoder == NULL) {
        return;
    }
    const sxml_allocator allocator = encoder->owning_allocator;
    sxml_exi_encoder_finalize(encoder);
    sxml_allocator_release(allocator, encoder);
}

sxml_status sxml_exi_encoder_encode(sxml_exi_encoder *encoder, const sxml_exi_encode_request *request,
                                    sxml_output_stream output_stream) {
    if (encoder == NULL || request == NULL || output_stream.operations == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_exi_encode_stream(encoder, request, output_stream);
}

/* ---------------------------------------------------------- Options reader */

/*
 * The reader's memory: the public object (src/exi_internal.h) first, so a
 * pointer to it is a pointer to this, and the block that holds the views
 * of its last read.
 */
typedef struct sxml_exi_options_reader_memory {
    struct sxml_exi_options_reader options_reader;
    /* The schemaId text and the datatypeRepresentationMap of the last
       read, one block of the configuration's allocator; NULL before. */
    void *view_memory;
} sxml_exi_options_reader_memory;

static sxml_exi_options_reader_memory *sxml_exi_options_reader_memory_of(sxml_exi_options_reader *options_reader) {
    void *memory = options_reader;
    return memory;
}

static bool sxml_exi_options_reader_configuration_is_valid(const sxml_exi_options_reader_configuration *configuration) {
    return configuration != NULL && !sxml_allocator_is_null(configuration->allocator) &&
           configuration->name_pool != NULL;
}

sxml_status sxml_exi_options_reader_memory_requirements(const sxml_exi_options_reader_configuration *configuration,
                                                        sxml_memory_requirements *requirements) {
    if (!sxml_exi_options_reader_configuration_is_valid(configuration) || requirements == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){.byte_count = sizeof(sxml_exi_options_reader_memory),
                                               .alignment = alignof(sxml_exi_options_reader_memory)};
    return SXML_STATUS_OK;
}

sxml_status sxml_exi_options_reader_initialize(sxml_memory_view memory,
                                               const sxml_exi_options_reader_configuration *configuration,
                                               sxml_exi_options_reader **options_reader) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_exi_options_reader_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK || options_reader == NULL) {
        return status == SXML_STATUS_OK ? SXML_STATUS_INVALID_ARGUMENT : status;
    }
    status = sxml_memory_view_check_requirements(memory, requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_exi_options_reader_memory *placed = placement;
    *placed = (sxml_exi_options_reader_memory){
        .options_reader = {.configuration = *configuration, .owning_allocator = {.state = NULL, .operations = NULL}},
        .view_memory = NULL,
    };
    *options_reader = &placed->options_reader;
    return SXML_STATUS_OK;
}

void sxml_exi_options_reader_finalize(sxml_exi_options_reader *options_reader) {
    if (options_reader == NULL) {
        return;
    }
    sxml_exi_options_reader_memory *memory = sxml_exi_options_reader_memory_of(options_reader);
    sxml_allocator_release(options_reader->configuration.allocator, memory->view_memory);
    memory->view_memory = NULL;
}

sxml_status sxml_exi_options_reader_create(sxml_allocator allocator,
                                           const sxml_exi_options_reader_configuration *configuration,
                                           sxml_exi_options_reader **options_reader) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_exi_options_reader_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK || options_reader == NULL) {
        return status == SXML_STATUS_OK ? SXML_STATUS_INVALID_ARGUMENT : status;
    }
    void *memory = NULL;
    status = sxml_exi_allocate_object(allocator, requirements, &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_exi_options_reader *created = NULL;
    status = sxml_exi_options_reader_initialize(
        (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count}, configuration, &created);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    created->owning_allocator = allocator;
    *options_reader = created;
    return SXML_STATUS_OK;
}

void sxml_exi_options_reader_destroy(sxml_exi_options_reader *options_reader) {
    if (options_reader == NULL) {
        return;
    }
    const sxml_allocator allocator = options_reader->owning_allocator;
    sxml_exi_options_reader_finalize(options_reader);
    sxml_allocator_release(allocator, options_reader);
}

/*
 * The elements of the EXI options document (EXI 1.0, Appendix C, "XML
 * Schema for EXI Options Header"), each with its parent, its place in its
 * parent's sequence and its content. The schema is a fixed tree three
 * levels deep, so reading it is a walk over a small explicit stack, not a
 * recursive descent (D71).
 */
typedef enum sxml_exi_option_element {
    SXML_EXI_OPTION_ELEMENT_HEADER,
    SXML_EXI_OPTION_ELEMENT_LESSCOMMON,
    SXML_EXI_OPTION_ELEMENT_UNCOMMON,
    SXML_EXI_OPTION_ELEMENT_ALIGNMENT,
    SXML_EXI_OPTION_ELEMENT_BYTE,
    SXML_EXI_OPTION_ELEMENT_PRE_COMPRESS,
    SXML_EXI_OPTION_ELEMENT_SELF_CONTAINED,
    SXML_EXI_OPTION_ELEMENT_VALUE_MAX_LENGTH,
    SXML_EXI_OPTION_ELEMENT_VALUE_PARTITION_CAPACITY,
    SXML_EXI_OPTION_ELEMENT_DATATYPE_REPRESENTATION_MAP,
    SXML_EXI_OPTION_ELEMENT_PRESERVE,
    SXML_EXI_OPTION_ELEMENT_DTD,
    SXML_EXI_OPTION_ELEMENT_PREFIXES,
    SXML_EXI_OPTION_ELEMENT_LEXICAL_VALUES,
    SXML_EXI_OPTION_ELEMENT_COMMENTS,
    SXML_EXI_OPTION_ELEMENT_PROCESSING_INSTRUCTIONS,
    SXML_EXI_OPTION_ELEMENT_BLOCK_SIZE,
    SXML_EXI_OPTION_ELEMENT_COMMON,
    SXML_EXI_OPTION_ELEMENT_COMPRESSION,
    SXML_EXI_OPTION_ELEMENT_FRAGMENT,
    SXML_EXI_OPTION_ELEMENT_SCHEMA_ID,
    SXML_EXI_OPTION_ELEMENT_STRICT,
    /* An element of another namespace (xsd:any namespace="##other"): the
       user-defined options that start uncommon, skipped. */
    SXML_EXI_OPTION_ELEMENT_OTHER,
    SXML_EXI_OPTION_ELEMENT_COUNT,
} sxml_exi_option_element;

/* What an element of the options document holds. */
typedef enum sxml_exi_option_content {
    /* A sequence of the elements whose parent it is. */
    SXML_EXI_OPTION_CONTENT_ELEMENTS,
    /* Nothing: its presence is the option (an empty complex type). */
    SXML_EXI_OPTION_CONTENT_EMPTY,
    /* An xsd:unsignedInt. */
    SXML_EXI_OPTION_CONTENT_UNSIGNED_INTEGER,
    /* An xsd:string, nillable (schemaId). */
    SXML_EXI_OPTION_CONTENT_STRING,
    /* Two elements of any content, skipped (datatypeRepresentationMap). */
    SXML_EXI_OPTION_CONTENT_PAIR,
    /* Anything, skipped (processContents="skip"). */
    SXML_EXI_OPTION_CONTENT_ANY,
} sxml_exi_option_content;

typedef struct sxml_exi_option_definition {
    const char *local_name;
    sxml_exi_option_element parent;
    /* The place in the parent's sequence; the members of a choice
       (alignment's byte and pre-compress) share one. */
    uint32_t position;
    /* maxOccurs="unbounded". */
    bool is_repeatable;
    sxml_exi_option_content content;
} sxml_exi_option_definition;

#define SXML_EXI_OPTION(name_literal, parent_element, position_value, repeatable, content_kind)                        \
    {                                                                                                                  \
        .local_name = (name_literal), .parent = (parent_element), .position = (position_value),                        \
        .is_repeatable = (repeatable), .content = (content_kind)                                                       \
    }

static const sxml_exi_option_definition sxml_exi_option_definitions[SXML_EXI_OPTION_ELEMENT_COUNT] = {
    [SXML_EXI_OPTION_ELEMENT_HEADER] =
        SXML_EXI_OPTION("header", SXML_EXI_OPTION_ELEMENT_COUNT, 0, false, SXML_EXI_OPTION_CONTENT_ELEMENTS),
    [SXML_EXI_OPTION_ELEMENT_LESSCOMMON] =
        SXML_EXI_OPTION("lesscommon", SXML_EXI_OPTION_ELEMENT_HEADER, 0, false, SXML_EXI_OPTION_CONTENT_ELEMENTS),
    [SXML_EXI_OPTION_ELEMENT_UNCOMMON] =
        SXML_EXI_OPTION("uncommon", SXML_EXI_OPTION_ELEMENT_LESSCOMMON, 0, false, SXML_EXI_OPTION_CONTENT_ELEMENTS),
    [SXML_EXI_OPTION_ELEMENT_ALIGNMENT] =
        SXML_EXI_OPTION("alignment", SXML_EXI_OPTION_ELEMENT_UNCOMMON, 1, false, SXML_EXI_OPTION_CONTENT_ELEMENTS),
    [SXML_EXI_OPTION_ELEMENT_BYTE] =
        SXML_EXI_OPTION("byte", SXML_EXI_OPTION_ELEMENT_ALIGNMENT, 0, false, SXML_EXI_OPTION_CONTENT_EMPTY),
    [SXML_EXI_OPTION_ELEMENT_PRE_COMPRESS] =
        SXML_EXI_OPTION("pre-compress", SXML_EXI_OPTION_ELEMENT_ALIGNMENT, 0, false, SXML_EXI_OPTION_CONTENT_EMPTY),
    [SXML_EXI_OPTION_ELEMENT_SELF_CONTAINED] =
        SXML_EXI_OPTION("selfContained", SXML_EXI_OPTION_ELEMENT_UNCOMMON, 2, false, SXML_EXI_OPTION_CONTENT_EMPTY),
    [SXML_EXI_OPTION_ELEMENT_VALUE_MAX_LENGTH] = SXML_EXI_OPTION("valueMaxLength", SXML_EXI_OPTION_ELEMENT_UNCOMMON, 3,
                                                                 false, SXML_EXI_OPTION_CONTENT_UNSIGNED_INTEGER),
    [SXML_EXI_OPTION_ELEMENT_VALUE_PARTITION_CAPACITY] = SXML_EXI_OPTION(
        "valuePartitionCapacity", SXML_EXI_OPTION_ELEMENT_UNCOMMON, 4, false, SXML_EXI_OPTION_CONTENT_UNSIGNED_INTEGER),
    [SXML_EXI_OPTION_ELEMENT_DATATYPE_REPRESENTATION_MAP] = SXML_EXI_OPTION(
        "datatypeRepresentationMap", SXML_EXI_OPTION_ELEMENT_UNCOMMON, 5, true, SXML_EXI_OPTION_CONTENT_PAIR),
    [SXML_EXI_OPTION_ELEMENT_PRESERVE] =
        SXML_EXI_OPTION("preserve", SXML_EXI_OPTION_ELEMENT_LESSCOMMON, 1, false, SXML_EXI_OPTION_CONTENT_ELEMENTS),
    [SXML_EXI_OPTION_ELEMENT_DTD] =
        SXML_EXI_OPTION("dtd", SXML_EXI_OPTION_ELEMENT_PRESERVE, 0, false, SXML_EXI_OPTION_CONTENT_EMPTY),
    [SXML_EXI_OPTION_ELEMENT_PREFIXES] =
        SXML_EXI_OPTION("prefixes", SXML_EXI_OPTION_ELEMENT_PRESERVE, 1, false, SXML_EXI_OPTION_CONTENT_EMPTY),
    [SXML_EXI_OPTION_ELEMENT_LEXICAL_VALUES] =
        SXML_EXI_OPTION("lexicalValues", SXML_EXI_OPTION_ELEMENT_PRESERVE, 2, false, SXML_EXI_OPTION_CONTENT_EMPTY),
    [SXML_EXI_OPTION_ELEMENT_COMMENTS] =
        SXML_EXI_OPTION("comments", SXML_EXI_OPTION_ELEMENT_PRESERVE, 3, false, SXML_EXI_OPTION_CONTENT_EMPTY),
    [SXML_EXI_OPTION_ELEMENT_PROCESSING_INSTRUCTIONS] =
        SXML_EXI_OPTION("pis", SXML_EXI_OPTION_ELEMENT_PRESERVE, 4, false, SXML_EXI_OPTION_CONTENT_EMPTY),
    [SXML_EXI_OPTION_ELEMENT_BLOCK_SIZE] = SXML_EXI_OPTION("blockSize", SXML_EXI_OPTION_ELEMENT_LESSCOMMON, 2, false,
                                                           SXML_EXI_OPTION_CONTENT_UNSIGNED_INTEGER),
    [SXML_EXI_OPTION_ELEMENT_COMMON] =
        SXML_EXI_OPTION("common", SXML_EXI_OPTION_ELEMENT_HEADER, 1, false, SXML_EXI_OPTION_CONTENT_ELEMENTS),
    [SXML_EXI_OPTION_ELEMENT_COMPRESSION] =
        SXML_EXI_OPTION("compression", SXML_EXI_OPTION_ELEMENT_COMMON, 0, false, SXML_EXI_OPTION_CONTENT_EMPTY),
    [SXML_EXI_OPTION_ELEMENT_FRAGMENT] =
        SXML_EXI_OPTION("fragment", SXML_EXI_OPTION_ELEMENT_COMMON, 1, false, SXML_EXI_OPTION_CONTENT_EMPTY),
    [SXML_EXI_OPTION_ELEMENT_SCHEMA_ID] =
        SXML_EXI_OPTION("schemaId", SXML_EXI_OPTION_ELEMENT_COMMON, 2, false, SXML_EXI_OPTION_CONTENT_STRING),
    [SXML_EXI_OPTION_ELEMENT_STRICT] =
        SXML_EXI_OPTION("strict", SXML_EXI_OPTION_ELEMENT_HEADER, 2, false, SXML_EXI_OPTION_CONTENT_EMPTY),
    [SXML_EXI_OPTION_ELEMENT_OTHER] =
        SXML_EXI_OPTION("", SXML_EXI_OPTION_ELEMENT_UNCOMMON, 0, true, SXML_EXI_OPTION_CONTENT_ANY),
};

/* The built-in EXI datatype representations (EXI 1.0, section 7.4, Table
   7-2), the names a datatypeRepresentationMap entry may map to. */
static const char *const sxml_exi_builtin_representations[] = {
    "base64Binary", "hexBinary",  "boolean", "decimal",   "double", "integer", "dateTime", "time",
    "date",         "gYearMonth", "gYear",   "gMonthDay", "gDay",   "gMonth",  "string",
};

/* What one read has found so far. */
typedef struct sxml_exi_options_reading {
    const sxml_name_pool *name_pool;
    sxml_tree *tree;
    sxml_exi_options options;
    /* Which elements the document has. */
    bool is_present[SXML_EXI_OPTION_ELEMENT_COUNT];
    /* The view block being filled: the datatypeRepresentationMap entries,
       then the schemaId text. */
    sxml_exi_datatype_representation *representations;
    size_t representation_capacity;
    uint8_t *text_bytes;
    size_t text_capacity;
    /* Bytes of text_bytes used: each text is copied there at most once,
       so the sum of the document's texts bounds them. */
    size_t text_byte_count;
    sxml_diagnostic *diagnostic;
} sxml_exi_options_reading;

/* Refuses the document: fills the diagnostic (static text) and returns
   SXML_STATUS_MALFORMED_INPUT. */
static sxml_status sxml_exi_options_refuse(const sxml_exi_options_reading *reading, sxml_byte_view description) {
    if (reading->diagnostic != NULL) {
        *reading->diagnostic = (sxml_diagnostic){.description = description,
                                                 .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                                                              .line_number = 0,
                                                              .column_number = 0,
                                                              .byte_offset = 0}};
    }
    return SXML_STATUS_MALFORMED_INPUT;
}

static sxml_status sxml_exi_options_describe(const sxml_exi_options_reading *reading, sxml_node_index node_index,
                                             sxml_node_description *description) {
    return sxml_node_describe(
        (sxml_node_reference){.tree = reading->tree, .node_index = node_index, .namespace_number = 0}, description);
}

static sxml_status sxml_exi_options_content(const sxml_exi_options_reading *reading, sxml_node_index node_index,
                                            sxml_byte_view *content) {
    return sxml_node_get_content(
        (sxml_node_reference){.tree = reading->tree, .node_index = node_index, .namespace_number = 0}, content);
}

static bool sxml_exi_is_whitespace(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/* XSD 1.1 Part 2, 4.3.6: the whiteSpace facet collapse of the types read
   here leaves the text without leading and trailing whitespace. */
static sxml_byte_view sxml_exi_trim(sxml_byte_view text) {
    if (text.byte_count == 0) {
        return text;
    }
    size_t start = 0;
    size_t end = text.byte_count;
    while (start < end && sxml_exi_is_whitespace(text.bytes[start])) {
        start += 1;
    }
    while (end > start && sxml_exi_is_whitespace(text.bytes[end - 1])) {
        end -= 1;
    }
    return (sxml_byte_view){.bytes = text.bytes + start, .byte_count = end - start};
}

/* The element of the options document's namespace with local name whose
   parent is parent; COUNT for none. */
typedef struct sxml_exi_option_lookup {
    sxml_byte_view local_name;
    sxml_exi_option_element parent;
} sxml_exi_option_lookup;

static sxml_exi_option_element sxml_exi_option_find(sxml_exi_option_lookup lookup) {
    for (size_t element_index = 0; element_index < (size_t)SXML_EXI_OPTION_ELEMENT_OTHER; element_index += 1) {
        const sxml_exi_option_definition *definition = &sxml_exi_option_definitions[element_index];
        if (definition->parent == lookup.parent &&
            sxml_byte_view_equals(lookup.local_name, sxml_byte_view_from_c_string(definition->local_name))) {
            return (sxml_exi_option_element)element_index;
        }
    }
    return SXML_EXI_OPTION_ELEMENT_COUNT;
}

/* The lexical forms of xs:boolean (XSD 1.1 Part 2, 3.3.2). */
typedef struct sxml_exi_boolean_reading {
    bool is_valid;
    bool value;
} sxml_exi_boolean_reading;

static sxml_exi_boolean_reading sxml_exi_options_read_boolean(sxml_byte_view text) {
    const sxml_byte_view trimmed = sxml_exi_trim(text);
    if (sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("true")) ||
        sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("1"))) {
        return (sxml_exi_boolean_reading){.is_valid = true, .value = true};
    }
    return (sxml_exi_boolean_reading){.is_valid =
                                          sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("false")) ||
                                          sxml_byte_view_equals(trimmed, SXML_BYTE_VIEW_FROM_LITERAL("0")),
                                      .value = false};
}

/* The attributes an element of the options document may have: none but
   those of the XML Schema instance namespace a schema-valid document may
   carry anywhere (xsi:schemaLocation, xsi:noNamespaceSchemaLocation), and
   xsi:nil on the nillable schemaId (XSD 1.1 Part 1, 3.3.4.3). Writes
   whether xsi:nil is true. */
typedef struct sxml_exi_attribute_check {
    sxml_node_index element_index;
    const sxml_node_description *description;
    bool is_nillable;
} sxml_exi_attribute_check;

typedef struct sxml_exi_attribute_outcome {
    bool is_nil;
} sxml_exi_attribute_outcome;

static sxml_status sxml_exi_options_check_attributes(const sxml_exi_options_reading *reading,
                                                     const sxml_exi_attribute_check *check,
                                                     sxml_exi_attribute_outcome *outcome) {
    outcome->is_nil = false;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t attribute_number = 1;
         status == SXML_STATUS_OK && attribute_number <= check->description->attribute_count; attribute_number += 1) {
        const sxml_node_index attribute_index = {.index = check->element_index.index + attribute_number};
        sxml_node_description attribute_description;
        sxml_name_parts parts = {.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                                 .local_name = {.bytes = NULL, .byte_count = 0}};
        status = sxml_exi_options_describe(reading, attribute_index, &attribute_description);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_name_parts(reading->name_pool, attribute_description.node_name, &parts);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        const bool is_instance = parts.namespace_uri.index == SXML_NAMESPACE_XML_SCHEMA_INSTANCE;
        if (is_instance &&
            (sxml_byte_view_equals(parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("schemaLocation")) ||
             sxml_byte_view_equals(parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("noNamespaceSchemaLocation")))) {
            continue;
        }
        if (!is_instance || !check->is_nillable ||
            !sxml_byte_view_equals(parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("nil"))) {
            return sxml_exi_options_refuse(
                reading, SXML_BYTE_VIEW_FROM_LITERAL("an element of the EXI options document has an attribute "
                                                     "the EXI options schema does not allow (EXI 1.0, Appendix C)"));
        }
        sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
        status = sxml_exi_options_content(reading, attribute_index, &value);
        if (status != SXML_STATUS_OK) {
            break;
        }
        const sxml_exi_boolean_reading nil = sxml_exi_options_read_boolean(value);
        if (!nil.is_valid) {
            return sxml_exi_options_refuse(reading,
                                           SXML_BYTE_VIEW_FROM_LITERAL("xsi:nil of schemaId is not an xs:boolean"));
        }
        outcome->is_nil = nil.value;
    }
    return status;
}

/* The text of an element of simple content: its text children joined
   (comments and processing instructions between them are not part of the
   value), into the reading's text block when there is more than one;
   MALFORMED_INPUT for an element child. */
static sxml_status sxml_exi_options_simple_text(sxml_exi_options_reading *reading,
                                                const sxml_node_description *description, sxml_byte_view *text) {
    *text = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    size_t text_count = 0;
    size_t total_byte_count = 0;
    sxml_status status = SXML_STATUS_OK;
    for (int pass_number = 0; status == SXML_STATUS_OK && pass_number < 2; pass_number += 1) {
        bool has_child = description->has_children;
        sxml_node_index child = description->first_child;
        size_t filled = 0;
        while (status == SXML_STATUS_OK && has_child) {
            sxml_node_description child_description;
            status = sxml_exi_options_describe(reading, child, &child_description);
            if (status != SXML_STATUS_OK) {
                break;
            }
            if (child_description.node_kind == SXML_NODE_KIND_ELEMENT) {
                return sxml_exi_options_refuse(
                    reading, SXML_BYTE_VIEW_FROM_LITERAL("an option of the EXI options document that has a value "
                                                         "has an element child (EXI 1.0, Appendix C)"));
            }
            if (child_description.node_kind == SXML_NODE_KIND_TEXT) {
                sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
                status = sxml_exi_options_content(reading, child, &content);
                if (status == SXML_STATUS_OK && pass_number == 0) {
                    text_count += 1;
                    total_byte_count += content.byte_count;
                    *text = content;
                } else if (status == SXML_STATUS_OK && content.byte_count > 0) {
                    memcpy(reading->text_bytes + reading->text_byte_count + filled, content.bytes, content.byte_count);
                    filled += content.byte_count;
                }
            }
            has_child = child_description.has_next_sibling;
            child = child_description.next_sibling;
        }
        if (pass_number == 0 && (text_count < 2 || total_byte_count == 0)) {
            break;
        }
        if (pass_number == 0 && total_byte_count > reading->text_capacity - reading->text_byte_count) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        if (pass_number == 1) {
            *text = (sxml_byte_view){.bytes = reading->text_bytes + reading->text_byte_count, .byte_count = filled};
            reading->text_byte_count += filled;
        }
    }
    return status;
}

/* Reads an xsd:unsignedInt (XSD 1.1 Part 2, 3.4.24: 0 to 4294967295, the
   lexical form of xs:integer, a sign allowed). */
static sxml_status sxml_exi_options_read_unsigned_integer(sxml_exi_options_reading *reading,
                                                          const sxml_node_description *description, uint32_t *value) {
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    const sxml_status status = sxml_exi_options_simple_text(reading, description, &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    text = sxml_exi_trim(text);
    size_t offset = 0;
    bool is_negative = false;
    if (offset < text.byte_count && (text.bytes[0] == '+' || text.bytes[0] == '-')) {
        is_negative = text.bytes[0] == '-';
        offset = 1;
    }
    if (offset == text.byte_count) {
        return sxml_exi_options_refuse(
            reading, SXML_BYTE_VIEW_FROM_LITERAL("an option of the EXI options document is not an xsd:unsignedInt"));
    }
    uint64_t number = 0;
    for (; offset < text.byte_count; offset += 1) {
        const uint8_t digit = text.bytes[offset];
        if (digit < '0' || digit > '9') {
            return sxml_exi_options_refuse(
                reading,
                SXML_BYTE_VIEW_FROM_LITERAL("an option of the EXI options document is not an xsd:unsignedInt"));
        }
        number = number * 10U + (uint64_t)(digit - '0');
        if (number > UINT32_MAX) {
            return sxml_exi_options_refuse(
                reading, SXML_BYTE_VIEW_FROM_LITERAL("an option of the EXI options document is larger than an "
                                                     "xsd:unsignedInt (4294967295)"));
        }
    }
    if (is_negative && number != 0) {
        return sxml_exi_options_refuse(
            reading, SXML_BYTE_VIEW_FROM_LITERAL("an option of the EXI options document is negative"));
    }
    *value = (uint32_t)number;
    return SXML_STATUS_OK;
}

/* An option that is not a sequence of options: its element, and whether
   xsi:nil is true (schemaId only). */
typedef struct sxml_exi_option_leaf {
    sxml_exi_option_element element;
    const sxml_node_description *description;
    bool is_nil;
} sxml_exi_option_leaf;

/* Reads schemaId (EXI 1.0, section 5.4): xsi:nil="true" for no schema,
   the empty string for the built-in types, else a schema's identifier. */
static sxml_status sxml_exi_options_read_schema_identifier(sxml_exi_options_reading *reading,
                                                           const sxml_exi_option_leaf *leaf) {
    sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
    const sxml_status status = sxml_exi_options_simple_text(reading, leaf->description, &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_exi_schema_identifier *identifier = &reading->options.schema_identifier;
    const sxml_byte_view no_text = {.bytes = NULL, .byte_count = 0};
    if (leaf->is_nil) {
        /* XSD 1.1 Part 1, 3.3.4.3: a nil element has no character or
           element children. */
        if (text.byte_count > 0) {
            return sxml_exi_options_refuse(reading,
                                           SXML_BYTE_VIEW_FROM_LITERAL("schemaId with xsi:nil=\"true\" has content"));
        }
        *identifier =
            (sxml_exi_schema_identifier){.schema_identifier_kind = SXML_EXI_SCHEMA_IDENTIFIER_NONE, .text = no_text};
        return SXML_STATUS_OK;
    }
    if (text.byte_count == 0) {
        *identifier = (sxml_exi_schema_identifier){.schema_identifier_kind = SXML_EXI_SCHEMA_IDENTIFIER_BUILT_IN_TYPES,
                                                   .text = no_text};
        return SXML_STATUS_OK;
    }
    /* The text goes into the view block: the content of a single text
       node belongs to the tree, which the options may outlive. */
    const bool is_in_block = reading->text_bytes != NULL && text.bytes >= reading->text_bytes &&
                             text.bytes < reading->text_bytes + reading->text_capacity;
    if (!is_in_block) {
        if (reading->text_bytes == NULL || text.byte_count > reading->text_capacity - reading->text_byte_count) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        uint8_t *copy = reading->text_bytes + reading->text_byte_count;
        memcpy(copy, text.bytes, text.byte_count);
        reading->text_byte_count += text.byte_count;
        text.bytes = copy;
    }
    *identifier = (sxml_exi_schema_identifier){.schema_identifier_kind = SXML_EXI_SCHEMA_IDENTIFIER_TEXT, .text = text};
    return SXML_STATUS_OK;
}

/* The two element children of a datatypeRepresentationMap. */
typedef struct sxml_exi_representation_pair {
    sxml_name_identifier names[2];
    size_t name_count;
} sxml_exi_representation_pair;

static sxml_status sxml_exi_options_read_pair(const sxml_exi_options_reading *reading,
                                              const sxml_node_description *description,
                                              sxml_exi_representation_pair *pair) {
    bool has_child = description->has_children;
    sxml_node_index child = description->first_child;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && has_child) {
        sxml_node_description child_description;
        status = sxml_exi_options_describe(reading, child, &child_description);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (child_description.node_kind == SXML_NODE_KIND_ELEMENT) {
            if (pair->name_count == 2) {
                return sxml_exi_options_refuse(
                    reading, SXML_BYTE_VIEW_FROM_LITERAL("a datatypeRepresentationMap has more than two elements"));
            }
            pair->names[pair->name_count] = child_description.node_name;
            pair->name_count += 1;
        } else if (child_description.node_kind == SXML_NODE_KIND_TEXT) {
            sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
            status = sxml_exi_options_content(reading, child, &content);
            if (status == SXML_STATUS_OK && sxml_exi_trim(content).byte_count > 0) {
                return sxml_exi_options_refuse(reading,
                                               SXML_BYTE_VIEW_FROM_LITERAL("a datatypeRepresentationMap has text"));
            }
        }
        has_child = child_description.has_next_sibling;
        child = child_description.next_sibling;
    }
    if (status == SXML_STATUS_OK && pair->name_count != 2) {
        return sxml_exi_options_refuse(
            reading, SXML_BYTE_VIEW_FROM_LITERAL("a datatypeRepresentationMap does not have two elements: a schema "
                                                 "type and its representation"));
    }
    return status;
}

/* Reads a datatypeRepresentationMap entry: the schema type (an element of
   another namespace, named by the type's QName) and the representation (a
   built-in EXI datatype representation, EXI 1.0, section 7.4). */
static sxml_status sxml_exi_options_read_representation(sxml_exi_options_reading *reading,
                                                        const sxml_node_description *description) {
    sxml_exi_representation_pair pair = {.names = {{.index = 0}, {.index = 0}}, .name_count = 0};
    sxml_status status = sxml_exi_options_read_pair(reading, description, &pair);
    sxml_name_parts type_parts = {.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                                  .local_name = {.bytes = NULL, .byte_count = 0}};
    sxml_name_parts representation_parts = type_parts;
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_name_parts(reading->name_pool, pair.names[0], &type_parts);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_name_parts(reading->name_pool, pair.names[1], &representation_parts);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* xsd:any namespace="##other": neither the options namespace nor no
       namespace (XSD 1.1 Part 1, 3.10.1). */
    if (type_parts.namespace_uri.index == SXML_NAMESPACE_EXI_FORMAT ||
        type_parts.namespace_uri.index == SXML_NAMESPACE_NONE) {
        return sxml_exi_options_refuse(
            reading, SXML_BYTE_VIEW_FROM_LITERAL("the schema type of a datatypeRepresentationMap is in the EXI "
                                                 "namespace or in no namespace"));
    }
    bool is_builtin = false;
    const size_t builtin_count = sizeof(sxml_exi_builtin_representations) / sizeof(sxml_exi_builtin_representations[0]);
    for (size_t representation_index = 0;
         representation_parts.namespace_uri.index == SXML_NAMESPACE_EXI_FORMAT && representation_index < builtin_count;
         representation_index += 1) {
        is_builtin =
            is_builtin ||
            sxml_byte_view_equals(representation_parts.local_name,
                                  sxml_byte_view_from_c_string(sxml_exi_builtin_representations[representation_index]));
    }
    if (!is_builtin) {
        return sxml_exi_options_refuse(
            reading, SXML_BYTE_VIEW_FROM_LITERAL("the representation of a datatypeRepresentationMap is not a "
                                                 "built-in EXI datatype representation (EXI 1.0, section 7.4)"));
    }
    const size_t entry_index = reading->options.datatype_representation_map.datatype_representation_count;
    if (entry_index >= reading->representation_capacity) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    reading->representations[entry_index] =
        (sxml_exi_datatype_representation){.schema_type = pair.names[0], .representation = pair.names[1]};
    reading->options.datatype_representation_map = (sxml_exi_datatype_representation_view){
        .datatype_representations = reading->representations, .datatype_representation_count = entry_index + 1};
    return SXML_STATUS_OK;
}

/* Checks an empty option (XSD 1.1 Part 1, 3.4.4.2, clause 1.1: an empty
   content type has no character or element children). */
static sxml_status sxml_exi_options_check_empty(const sxml_exi_options_reading *reading,
                                                const sxml_node_description *description) {
    bool has_child = description->has_children;
    sxml_node_index child = description->first_child;
    while (has_child) {
        sxml_node_description child_description;
        const sxml_status status = sxml_exi_options_describe(reading, child, &child_description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (child_description.node_kind == SXML_NODE_KIND_ELEMENT ||
            child_description.node_kind == SXML_NODE_KIND_TEXT) {
            return sxml_exi_options_refuse(
                reading, SXML_BYTE_VIEW_FROM_LITERAL("an option of the EXI options document that is an empty "
                                                     "element has content"));
        }
        has_child = child_description.has_next_sibling;
        child = child_description.next_sibling;
    }
    return SXML_STATUS_OK;
}

/* Reads one option whose content is a number: valueMaxLength,
   valuePartitionCapacity, blockSize. */

static sxml_status sxml_exi_options_read_number(sxml_exi_options_reading *reading, const sxml_exi_option_leaf *leaf) {
    sxml_exi_options *options = &reading->options;
    uint32_t value = 0;
    const sxml_status status = sxml_exi_options_read_unsigned_integer(reading, leaf->description, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (leaf->element == SXML_EXI_OPTION_ELEMENT_VALUE_MAX_LENGTH) {
        options->has_value_max_length = true;
        options->value_max_length = value;
        return SXML_STATUS_OK;
    }
    if (leaf->element == SXML_EXI_OPTION_ELEMENT_VALUE_PARTITION_CAPACITY) {
        options->has_value_partition_capacity = true;
        options->value_partition_capacity = value;
        return SXML_STATUS_OK;
    }
    /* Appendix C: blockSize has minInclusive 1. */
    if (value == 0) {
        return sxml_exi_options_refuse(
            reading, SXML_BYTE_VIEW_FROM_LITERAL("blockSize is 0; it is at least 1 (EXI 1.0, Appendix C)"));
    }
    options->has_block_size = true;
    options->block_size = value;
    return SXML_STATUS_OK;
}

static sxml_status sxml_exi_options_read_leaf(sxml_exi_options_reading *reading, const sxml_exi_option_leaf *leaf) {
    switch (sxml_exi_option_definitions[leaf->element].content) {
    case SXML_EXI_OPTION_CONTENT_EMPTY:
        return sxml_exi_options_check_empty(reading, leaf->description);
    case SXML_EXI_OPTION_CONTENT_UNSIGNED_INTEGER:
        return sxml_exi_options_read_number(reading, leaf);
    case SXML_EXI_OPTION_CONTENT_STRING:
        return sxml_exi_options_read_schema_identifier(reading, leaf);
    case SXML_EXI_OPTION_CONTENT_PAIR:
        return sxml_exi_options_read_representation(reading, leaf->description);
    case SXML_EXI_OPTION_CONTENT_ANY:
    case SXML_EXI_OPTION_CONTENT_ELEMENTS:
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* The elements whose children are yet to be read: at most one per level
   of the schema. */
enum { SXML_EXI_OPTIONS_STACK_CAPACITY = 8 };

typedef struct sxml_exi_option_container {
    sxml_exi_option_element element;
    sxml_node_index node_index;
} sxml_exi_option_container;

typedef struct sxml_exi_option_stack {
    sxml_exi_option_container containers[SXML_EXI_OPTIONS_STACK_CAPACITY];
    size_t container_count;
} sxml_exi_option_stack;

/* Where in a container's sequence the reading is. */
typedef struct sxml_exi_sequence_place {
    uint32_t next_position;
    bool has_last;
    uint32_t last_position;
} sxml_exi_sequence_place;

/* Finds which option a child element of container is (*element), and
   checks it comes in its place of the sequence (Appendix C), which it
   moves past; refused otherwise. */
typedef struct sxml_exi_child_element {
    sxml_exi_option_element container;
    sxml_name_identifier name;
    sxml_exi_sequence_place *place;
} sxml_exi_child_element;

static sxml_status sxml_exi_options_place_child(const sxml_exi_options_reading *reading,
                                                const sxml_exi_child_element *child, sxml_exi_option_element *element) {
    sxml_exi_sequence_place *place = child->place;
    sxml_name_parts parts = {.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    const sxml_status status = sxml_name_pool_get_name_parts(reading->name_pool, child->name, &parts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *element = SXML_EXI_OPTION_ELEMENT_COUNT;
    if (parts.namespace_uri.index == SXML_NAMESPACE_EXI_FORMAT) {
        *element =
            sxml_exi_option_find((sxml_exi_option_lookup){.local_name = parts.local_name, .parent = child->container});
    } else if (parts.namespace_uri.index != SXML_NAMESPACE_NONE &&
               child->container == SXML_EXI_OPTION_ELEMENT_UNCOMMON) {
        *element = SXML_EXI_OPTION_ELEMENT_OTHER;
    }
    if (*element == SXML_EXI_OPTION_ELEMENT_COUNT) {
        return sxml_exi_options_refuse(
            reading, SXML_BYTE_VIEW_FROM_LITERAL("an element of the EXI options document is not one the EXI "
                                                 "options schema allows there (EXI 1.0, Appendix C)"));
    }
    const sxml_exi_option_definition *definition = &sxml_exi_option_definitions[*element];
    const bool repeats = place->has_last && definition->position == place->last_position && definition->is_repeatable;
    if (definition->position < place->next_position && !repeats) {
        return sxml_exi_options_refuse(
            reading, SXML_BYTE_VIEW_FROM_LITERAL("the options of the EXI options document are out of order or "
                                                 "repeated (EXI 1.0, Appendix C)"));
    }
    *place = (sxml_exi_sequence_place){
        .next_position = definition->position + 1U, .has_last = true, .last_position = definition->position};
    return SXML_STATUS_OK;
}

/* Reads one child element of a container: a container is pushed for
   later, a leaf read now. */
typedef struct sxml_exi_option_child {
    sxml_exi_option_element element;
    sxml_node_index node_index;
    const sxml_node_description *description;
} sxml_exi_option_child;

static sxml_status sxml_exi_options_read_child(sxml_exi_options_reading *reading, const sxml_exi_option_child *child,
                                               sxml_exi_option_stack *stack) {
    reading->is_present[child->element] = true;
    if (child->element == SXML_EXI_OPTION_ELEMENT_OTHER) {
        return SXML_STATUS_OK;
    }
    sxml_exi_attribute_outcome outcome = {.is_nil = false};
    const sxml_status status = sxml_exi_options_check_attributes(
        reading,
        &(sxml_exi_attribute_check){.element_index = child->node_index,
                                    .description = child->description,
                                    .is_nillable = child->element == SXML_EXI_OPTION_ELEMENT_SCHEMA_ID},
        &outcome);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (sxml_exi_option_definitions[child->element].content == SXML_EXI_OPTION_CONTENT_ELEMENTS) {
        if (stack->container_count == SXML_EXI_OPTIONS_STACK_CAPACITY) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        stack->containers[stack->container_count] =
            (sxml_exi_option_container){.element = child->element, .node_index = child->node_index};
        stack->container_count += 1;
        return SXML_STATUS_OK;
    }
    return sxml_exi_options_read_leaf(reading, &(sxml_exi_option_leaf){.element = child->element,
                                                                       .description = child->description,
                                                                       .is_nil = outcome.is_nil});
}

/* Reads the children of one container: each element of the options
   namespace in its place; whitespace text only. */
static sxml_status sxml_exi_options_read_container(sxml_exi_options_reading *reading,
                                                   sxml_exi_option_container container, sxml_exi_option_stack *stack) {
    sxml_node_description description;
    sxml_status status = sxml_exi_options_describe(reading, container.node_index, &description);
    bool has_child = status == SXML_STATUS_OK && description.has_children;
    sxml_node_index child = description.first_child;
    sxml_exi_sequence_place place = {.next_position = 0, .has_last = false, .last_position = 0};
    while (status == SXML_STATUS_OK && has_child) {
        sxml_node_description child_description;
        status = sxml_exi_options_describe(reading, child, &child_description);
        if (status != SXML_STATUS_OK) {
            break;
        }
        const sxml_node_index child_index = child;
        has_child = child_description.has_next_sibling;
        child = child_description.next_sibling;
        if (child_description.node_kind == SXML_NODE_KIND_TEXT) {
            sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
            status = sxml_exi_options_content(reading, child_index, &content);
            if (status == SXML_STATUS_OK && sxml_exi_trim(content).byte_count > 0) {
                return sxml_exi_options_refuse(
                    reading, SXML_BYTE_VIEW_FROM_LITERAL("an element of the EXI options document that holds "
                                                         "options has text"));
            }
            continue;
        }
        if (child_description.node_kind != SXML_NODE_KIND_ELEMENT) {
            continue;
        }
        sxml_exi_option_element element = SXML_EXI_OPTION_ELEMENT_COUNT;
        status = sxml_exi_options_place_child(reading,
                                              &(sxml_exi_child_element){.container = container.element,
                                                                        .name = child_description.node_name,
                                                                        .place = &place},
                                              &element);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_options_read_child(reading,
                                                 &(sxml_exi_option_child){.element = element,
                                                                          .node_index = child_index,
                                                                          .description = &child_description},
                                                 stack);
        }
    }
    return status;
}

/* Turns the elements present into options and checks the constraints of
   EXI 1.0, section 5.4, between them. */
static sxml_status sxml_exi_options_finish(sxml_exi_options_reading *reading) {
    const bool *present = reading->is_present;
    sxml_exi_options *options = &reading->options;
    if (present[SXML_EXI_OPTION_ELEMENT_ALIGNMENT] && !present[SXML_EXI_OPTION_ELEMENT_BYTE] &&
        !present[SXML_EXI_OPTION_ELEMENT_PRE_COMPRESS]) {
        return sxml_exi_options_refuse(
            reading, SXML_BYTE_VIEW_FROM_LITERAL("alignment holds neither byte nor pre-compress (EXI 1.0, "
                                                 "Appendix C)"));
    }
    const bool is_compressed = present[SXML_EXI_OPTION_ELEMENT_COMPRESSION];
    /* 5.4: alignment "MUST NOT" be given with compression. */
    if (is_compressed && present[SXML_EXI_OPTION_ELEMENT_ALIGNMENT]) {
        return sxml_exi_options_refuse(
            reading, SXML_BYTE_VIEW_FROM_LITERAL("alignment is given with compression, which EXI 1.0, section "
                                                 "5.4, does not allow"));
    }
    options->alignment = is_compressed                                   ? SXML_EXI_ALIGNMENT_COMPRESSION
                         : present[SXML_EXI_OPTION_ELEMENT_BYTE]         ? SXML_EXI_ALIGNMENT_BYTE_ALIGNMENT
                         : present[SXML_EXI_OPTION_ELEMENT_PRE_COMPRESS] ? SXML_EXI_ALIGNMENT_PRE_COMPRESSION
                                                                         : SXML_EXI_ALIGNMENT_BIT_PACKED;
    options->fidelity = (sxml_exi_fidelity){
        .preserves_comments = present[SXML_EXI_OPTION_ELEMENT_COMMENTS],
        .preserves_processing_instructions = present[SXML_EXI_OPTION_ELEMENT_PROCESSING_INSTRUCTIONS],
        .preserves_dtd = present[SXML_EXI_OPTION_ELEMENT_DTD],
        .preserves_prefixes = present[SXML_EXI_OPTION_ELEMENT_PREFIXES],
        .preserves_lexical_values = present[SXML_EXI_OPTION_ELEMENT_LEXICAL_VALUES]};
    options->is_strict = present[SXML_EXI_OPTION_ELEMENT_STRICT];
    options->is_fragment = present[SXML_EXI_OPTION_ELEMENT_FRAGMENT];
    options->allows_self_contained = present[SXML_EXI_OPTION_ELEMENT_SELF_CONTAINED];
    /* 5.4: strict cannot be used with dtd, prefixes, comments, pis or
       selfContained. */
    if (options->is_strict && (options->fidelity.preserves_dtd || options->fidelity.preserves_prefixes ||
                               options->fidelity.preserves_comments ||
                               options->fidelity.preserves_processing_instructions || options->allows_self_contained)) {
        return sxml_exi_options_refuse(
            reading, SXML_BYTE_VIEW_FROM_LITERAL("strict is given with dtd, prefixes, comments, pis or "
                                                 "selfContained, which EXI 1.0, section 5.4, does not allow"));
    }
    /* 5.4: selfContained cannot be used with compression or pre-compress
       (or strict, above). */
    if (options->allows_self_contained && (options->alignment == SXML_EXI_ALIGNMENT_COMPRESSION ||
                                           options->alignment == SXML_EXI_ALIGNMENT_PRE_COMPRESSION)) {
        return sxml_exi_options_refuse(
            reading, SXML_BYTE_VIEW_FROM_LITERAL("selfContained is given with compression or pre-compress, which "
                                                 "EXI 1.0, section 5.4, does not allow"));
    }
    return SXML_STATUS_OK;
}

/* Sizes the view block: an upper bound of the datatypeRepresentationMap
   entries (every element of that name) and of the schemaId text (every
   text of the document). */
typedef struct sxml_exi_view_bounds {
    size_t representation_count;
    size_t text_byte_count;
} sxml_exi_view_bounds;

static sxml_status sxml_exi_options_bound_views(const sxml_exi_options_reading *reading,
                                                const sxml_node_description *header, sxml_exi_view_bounds *bounds) {
    *bounds = (sxml_exi_view_bounds){.representation_count = 0, .text_byte_count = 0};
    sxml_status status = SXML_STATUS_OK;
    const uint32_t first_index = header->has_children ? header->first_child.index : header->last_descendant.index + 1U;
    for (uint32_t node_index = first_index; status == SXML_STATUS_OK && node_index <= header->last_descendant.index;
         node_index += 1) {
        sxml_node_description description;
        status = sxml_exi_options_describe(reading, (sxml_node_index){.index = node_index}, &description);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (description.node_kind == SXML_NODE_KIND_TEXT) {
            sxml_byte_view content = {.bytes = NULL, .byte_count = 0};
            status = sxml_exi_options_content(reading, (sxml_node_index){.index = node_index}, &content);
            bounds->text_byte_count += content.byte_count;
            continue;
        }
        if (description.node_kind != SXML_NODE_KIND_ELEMENT) {
            continue;
        }
        sxml_name_parts parts = {.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                                 .local_name = {.bytes = NULL, .byte_count = 0}};
        status = sxml_name_pool_get_name_parts(reading->name_pool, description.node_name, &parts);
        if (status == SXML_STATUS_OK && parts.namespace_uri.index == SXML_NAMESPACE_EXI_FORMAT &&
            sxml_byte_view_equals(parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("datatypeRepresentationMap"))) {
            bounds->representation_count += 1;
        }
    }
    return status;
}

/* Allocates the view block of bounds into reading; *memory receives it
   (NULL when nothing needs a view). */
typedef struct sxml_exi_view_allocation {
    sxml_allocator allocator;
    sxml_exi_view_bounds bounds;
} sxml_exi_view_allocation;

static sxml_status sxml_exi_options_allocate_views(sxml_exi_options_reading *reading,
                                                   const sxml_exi_view_allocation *allocation, void **memory) {
    *memory = NULL;
    size_t representation_byte_count = 0;
    sxml_status status = sxml_size_multiply(allocation->bounds.representation_count,
                                            sizeof(sxml_exi_datatype_representation), &representation_byte_count);
    size_t byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_add(representation_byte_count, allocation->bounds.text_byte_count, &byte_count);
    }
    if (status != SXML_STATUS_OK || byte_count == 0) {
        return status;
    }
    status = sxml_allocator_allocate(
        allocation->allocator,
        (sxml_allocation_request){.byte_count = byte_count, .alignment = alignof(sxml_exi_datatype_representation)},
        memory);
    if (status == SXML_STATUS_OK) {
        uint8_t *bytes = *memory;
        void *representations = bytes;
        reading->representations = representations;
        reading->representation_capacity = allocation->bounds.representation_count;
        reading->text_bytes = bytes + representation_byte_count;
        reading->text_capacity = allocation->bounds.text_byte_count;
    }
    return status;
}

/* Checks that the element to read is exi:header (Appendix C). */
static sxml_status sxml_exi_options_check_header(const sxml_exi_options_reading *reading,
                                                 const sxml_node_description *header) {
    sxml_name_parts parts = {.namespace_uri = {.index = SXML_NAMESPACE_NONE},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    if (header->node_kind == SXML_NODE_KIND_ELEMENT) {
        const sxml_status status = sxml_name_pool_get_name_parts(reading->name_pool, header->node_name, &parts);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (header->node_kind != SXML_NODE_KIND_ELEMENT || parts.namespace_uri.index != SXML_NAMESPACE_EXI_FORMAT ||
        !sxml_byte_view_equals(parts.local_name, SXML_BYTE_VIEW_FROM_LITERAL("header"))) {
        return sxml_exi_options_refuse(
            reading, SXML_BYTE_VIEW_FROM_LITERAL("the EXI options document is not a header element in "
                                                 "http://www.w3.org/2009/exi (EXI 1.0, Appendix C)"));
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_exi_options_reader_read(sxml_exi_options_reader *options_reader,
                                         const sxml_exi_options_read_request *request, sxml_exi_options *options) {
    if (options_reader == NULL || request == NULL || options == NULL || request->header_element.tree == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_exi_options_reading reading;
    memset(&reading, 0, sizeof(reading));
    reading.name_pool = options_reader->configuration.name_pool;
    reading.tree = request->header_element.tree;
    reading.diagnostic = request->diagnostic;
    sxml_node_description header;
    sxml_status status = sxml_node_describe(request->header_element, &header);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_options_check_header(&reading, &header);
    }
    sxml_exi_attribute_outcome outcome = {.is_nil = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_options_check_attributes(
            &reading,
            &(sxml_exi_attribute_check){
                .element_index = request->header_element.node_index, .description = &header, .is_nillable = false},
            &outcome);
    }
    sxml_exi_view_bounds bounds = {.representation_count = 0, .text_byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_options_bound_views(&reading, &header, &bounds);
    }
    void *view_memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_options_allocate_views(
            &reading,
            &(sxml_exi_view_allocation){.allocator = options_reader->configuration.allocator, .bounds = bounds},
            &view_memory);
    }
    sxml_exi_option_stack stack = {
        .containers = {{.element = SXML_EXI_OPTION_ELEMENT_HEADER, .node_index = request->header_element.node_index}},
        .container_count = 1};
    while (status == SXML_STATUS_OK && stack.container_count > 0) {
        stack.container_count -= 1;
        status = sxml_exi_options_read_container(&reading, stack.containers[stack.container_count], &stack);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_options_finish(&reading);
    }
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(options_reader->configuration.allocator, view_memory);
        return status;
    }
    sxml_exi_options_reader_memory *memory = sxml_exi_options_reader_memory_of(options_reader);
    sxml_allocator_release(options_reader->configuration.allocator, memory->view_memory);
    memory->view_memory = view_memory;
    *options = reading.options;
    return SXML_STATUS_OK;
}
