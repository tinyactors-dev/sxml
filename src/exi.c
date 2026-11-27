/*
 * EXI: the decoder, the encoder and the options reader of
 * include/sxml/exi.h (EXI 1.0 Second Edition; Canonical EXI; D333). This
 * file holds their lifecycles, which follow src/xml.c's parser; the codec
 * itself is in the exi_*.c modules Phase 8 adds (docs/tasks/phase-8.md).
 */
#include <stdalign.h>

#include "exi_internal.h"

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
    (void)builder;
    if (decoder == NULL || request == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* Packet 8.4 decodes. */
    return SXML_STATUS_UNSUPPORTED_FEATURE;
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
    (void)output_stream;
    if (encoder == NULL || request == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* Packet 8.5 encodes. */
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}

/* ---------------------------------------------------------- Options reader */

static bool sxml_exi_options_reader_configuration_is_valid(
    const sxml_exi_options_reader_configuration *configuration) {
    return configuration != NULL && !sxml_allocator_is_null(configuration->allocator) &&
           configuration->name_pool != NULL;
}

sxml_status sxml_exi_options_reader_memory_requirements(const sxml_exi_options_reader_configuration *configuration,
                                                        sxml_memory_requirements *requirements) {
    if (!sxml_exi_options_reader_configuration_is_valid(configuration) || requirements == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){.byte_count = sizeof(struct sxml_exi_options_reader),
                                               .alignment = alignof(struct sxml_exi_options_reader)};
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
    sxml_exi_options_reader *placed = placement;
    *placed = (sxml_exi_options_reader){
        .configuration = *configuration,
        .owning_allocator = {.state = NULL, .operations = NULL},
    };
    *options_reader = placed;
    return SXML_STATUS_OK;
}

void sxml_exi_options_reader_finalize(sxml_exi_options_reader *options_reader) {
    (void)options_reader;
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

sxml_status sxml_exi_options_reader_read(sxml_exi_options_reader *options_reader,
                                         const sxml_exi_options_read_request *request, sxml_exi_options *options) {
    (void)options;
    if (options_reader == NULL || request == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* Packet 8.13 reads the options document. */
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}
