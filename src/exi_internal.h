#pragma once

/*
 * EXI internals (include/sxml/exi.h; D333). The objects of the public API;
 * the modules of the codec declare their own parts in the sections below as
 * the packets of Phase 8 add them (docs/tasks/phase-8.md).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/exi.h"
#include "sxml/memory.h"

/* The defaults of sxml_exi_limits fields that are 0. */
enum {
    SXML_EXI_DEFAULT_MAXIMUM_DEPTH = 4096,
    SXML_EXI_DEFAULT_MAXIMUM_LEARNED_PRODUCTION_COUNT = 1000000,
    /* EXI 1.0, section 5.4: blockSize defaults to 1,000,000. */
    SXML_EXI_DEFAULT_BLOCK_SIZE = 1000000,
};

/* 1 GiB of decoded content, the default of maximum_content_byte_count. */
static const uint64_t sxml_exi_default_maximum_content_byte_count = UINT64_C(1073741824);

/* The limits with every 0 replaced by its default. */
sxml_exi_limits sxml_exi_limits_resolve(sxml_exi_limits limits);

struct sxml_exi_decoder {
    sxml_exi_decoder_configuration configuration;
    /* The allocator of sxml_exi_decoder_create; null after initialize. */
    sxml_allocator owning_allocator;
};

struct sxml_exi_encoder {
    sxml_exi_encoder_configuration configuration;
    sxml_allocator owning_allocator;
};

struct sxml_exi_options_reader {
    sxml_exi_options_reader_configuration configuration;
    sxml_allocator owning_allocator;
};
