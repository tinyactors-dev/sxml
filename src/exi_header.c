/*
 * The options document of the EXI header (EXI 5.4): an EXI body of the
 * options schema (Appendix C), bit-packed and strict, with the
 * schema-informed grammars 8.5 derives from that schema. The schema is
 * fixed, so its grammars are written out here as data: every element of
 * the schema is a sequence, a choice, empty, or a simple value, and the
 * state of a sequence is the index of the first particle it still
 * accepts. With strict (8.5.4.4.2) a state's productions are its SE
 * (qname) in schema order, then its SE (*), then EE; the event code is
 * an n-bit unsigned integer of their count (6.2).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "exi_internal.h"

/* The elements of the options schema, and its wildcards (##other and
   ##any, both SE (*) in 8.5.4.1.6). */
typedef enum sxml_exi_option_element {
    SXML_EXI_OPTION_HEADER,
    SXML_EXI_OPTION_LESSCOMMON,
    SXML_EXI_OPTION_UNCOMMON,
    SXML_EXI_OPTION_ALIGNMENT,
    SXML_EXI_OPTION_BYTE,
    SXML_EXI_OPTION_PRE_COMPRESS,
    SXML_EXI_OPTION_SELF_CONTAINED,
    SXML_EXI_OPTION_VALUE_MAX_LENGTH,
    SXML_EXI_OPTION_VALUE_PARTITION_CAPACITY,
    SXML_EXI_OPTION_DATATYPE_REPRESENTATION_MAP,
    SXML_EXI_OPTION_PRESERVE,
    SXML_EXI_OPTION_DTD,
    SXML_EXI_OPTION_PREFIXES,
    SXML_EXI_OPTION_LEXICAL_VALUES,
    SXML_EXI_OPTION_COMMENTS,
    SXML_EXI_OPTION_PIS,
    SXML_EXI_OPTION_BLOCK_SIZE,
    SXML_EXI_OPTION_COMMON,
    SXML_EXI_OPTION_COMPRESSION,
    SXML_EXI_OPTION_FRAGMENT,
    SXML_EXI_OPTION_SCHEMA_IDENTIFIER,
    SXML_EXI_OPTION_STRICT,
    SXML_EXI_OPTION_WILDCARD,
    SXML_EXI_OPTION_COUNT,
} sxml_exi_option_element;

typedef enum sxml_exi_option_content {
    /* Particles in order, each optional (minOccurs="0"). */
    SXML_EXI_OPTION_CONTENT_SEQUENCE,
    /* Exactly one of the particles. */
    SXML_EXI_OPTION_CONTENT_CHOICE,
    /* An empty complex type: EE only. */
    SXML_EXI_OPTION_CONTENT_EMPTY,
    /* An xsd:unsignedInt: CH, an Unsigned Integer (7.1.6), EE. */
    SXML_EXI_OPTION_CONTENT_UNSIGNED,
    /* schemaId: a nillable xsd:string. */
    SXML_EXI_OPTION_CONTENT_NILLABLE_STRING,
    /* What the wildcards and datatypeRepresentationMap hold (skipped
       content); not read or written here. */
    SXML_EXI_OPTION_CONTENT_SKIPPED,
} sxml_exi_option_content;

enum { SXML_EXI_OPTION_MAXIMUM_PARTICLES = 6, SXML_EXI_OPTION_MAXIMUM_DEPTH = 8 };

typedef struct sxml_exi_option_particle {
    sxml_exi_option_element element;
    /* maxOccurs="unbounded": the state stays at the particle. */
    bool is_repeated;
} sxml_exi_option_particle;

typedef struct sxml_exi_option_declaration {
    sxml_exi_option_content content;
    uint32_t particle_count;
    sxml_exi_option_particle particles[SXML_EXI_OPTION_MAXIMUM_PARTICLES];
} sxml_exi_option_declaration;

#define SXML_EXI_OPTION_ONE(name)                                                                                 \
    {                                                                                                           \
        .element = (name), .is_repeated = false                                                                 \
    }
#define SXML_EXI_OPTION_MANY(name)                                                                                \
    {                                                                                                           \
        .element = (name), .is_repeated = true                                                                  \
    }

/* Appendix C, element by element. */
static const sxml_exi_option_declaration sxml_exi_option_declarations[SXML_EXI_OPTION_COUNT] = {
    [SXML_EXI_OPTION_HEADER] = {.content = SXML_EXI_OPTION_CONTENT_SEQUENCE,
                                .particle_count = 3,
                                .particles = {SXML_EXI_OPTION_ONE(SXML_EXI_OPTION_LESSCOMMON),
                                              SXML_EXI_OPTION_ONE(SXML_EXI_OPTION_COMMON),
                                              SXML_EXI_OPTION_ONE(SXML_EXI_OPTION_STRICT)}},
    [SXML_EXI_OPTION_LESSCOMMON] = {.content = SXML_EXI_OPTION_CONTENT_SEQUENCE,
                                    .particle_count = 3,
                                    .particles = {SXML_EXI_OPTION_ONE(SXML_EXI_OPTION_UNCOMMON),
                                                  SXML_EXI_OPTION_ONE(SXML_EXI_OPTION_PRESERVE),
                                                  SXML_EXI_OPTION_ONE(SXML_EXI_OPTION_BLOCK_SIZE)}},
    [SXML_EXI_OPTION_UNCOMMON] = {.content = SXML_EXI_OPTION_CONTENT_SEQUENCE,
                                  .particle_count = 6,
                                  .particles = {SXML_EXI_OPTION_MANY(SXML_EXI_OPTION_WILDCARD),
                                                SXML_EXI_OPTION_ONE(SXML_EXI_OPTION_ALIGNMENT),
                                                SXML_EXI_OPTION_ONE(SXML_EXI_OPTION_SELF_CONTAINED),
                                                SXML_EXI_OPTION_ONE(SXML_EXI_OPTION_VALUE_MAX_LENGTH),
                                                SXML_EXI_OPTION_ONE(SXML_EXI_OPTION_VALUE_PARTITION_CAPACITY),
                                                SXML_EXI_OPTION_MANY(SXML_EXI_OPTION_DATATYPE_REPRESENTATION_MAP)}},
    [SXML_EXI_OPTION_ALIGNMENT] = {.content = SXML_EXI_OPTION_CONTENT_CHOICE,
                                   .particle_count = 2,
                                   .particles = {SXML_EXI_OPTION_ONE(SXML_EXI_OPTION_BYTE),
                                                 SXML_EXI_OPTION_ONE(SXML_EXI_OPTION_PRE_COMPRESS)}},
    [SXML_EXI_OPTION_BYTE] = {.content = SXML_EXI_OPTION_CONTENT_EMPTY},
    [SXML_EXI_OPTION_PRE_COMPRESS] = {.content = SXML_EXI_OPTION_CONTENT_EMPTY},
    [SXML_EXI_OPTION_SELF_CONTAINED] = {.content = SXML_EXI_OPTION_CONTENT_EMPTY},
    [SXML_EXI_OPTION_VALUE_MAX_LENGTH] = {.content = SXML_EXI_OPTION_CONTENT_UNSIGNED},
    [SXML_EXI_OPTION_VALUE_PARTITION_CAPACITY] = {.content = SXML_EXI_OPTION_CONTENT_UNSIGNED},
    [SXML_EXI_OPTION_DATATYPE_REPRESENTATION_MAP] = {.content = SXML_EXI_OPTION_CONTENT_SKIPPED},
    [SXML_EXI_OPTION_PRESERVE] = {.content = SXML_EXI_OPTION_CONTENT_SEQUENCE,
                                  .particle_count = 5,
                                  .particles = {SXML_EXI_OPTION_ONE(SXML_EXI_OPTION_DTD),
                                                SXML_EXI_OPTION_ONE(SXML_EXI_OPTION_PREFIXES),
                                                SXML_EXI_OPTION_ONE(SXML_EXI_OPTION_LEXICAL_VALUES),
                                                SXML_EXI_OPTION_ONE(SXML_EXI_OPTION_COMMENTS),
                                                SXML_EXI_OPTION_ONE(SXML_EXI_OPTION_PIS)}},
    [SXML_EXI_OPTION_DTD] = {.content = SXML_EXI_OPTION_CONTENT_EMPTY},
    [SXML_EXI_OPTION_PREFIXES] = {.content = SXML_EXI_OPTION_CONTENT_EMPTY},
    [SXML_EXI_OPTION_LEXICAL_VALUES] = {.content = SXML_EXI_OPTION_CONTENT_EMPTY},
    [SXML_EXI_OPTION_COMMENTS] = {.content = SXML_EXI_OPTION_CONTENT_EMPTY},
    [SXML_EXI_OPTION_PIS] = {.content = SXML_EXI_OPTION_CONTENT_EMPTY},
    [SXML_EXI_OPTION_BLOCK_SIZE] = {.content = SXML_EXI_OPTION_CONTENT_UNSIGNED},
    [SXML_EXI_OPTION_COMMON] = {.content = SXML_EXI_OPTION_CONTENT_SEQUENCE,
                                .particle_count = 3,
                                .particles = {SXML_EXI_OPTION_ONE(SXML_EXI_OPTION_COMPRESSION),
                                              SXML_EXI_OPTION_ONE(SXML_EXI_OPTION_FRAGMENT),
                                              SXML_EXI_OPTION_ONE(SXML_EXI_OPTION_SCHEMA_IDENTIFIER)}},
    [SXML_EXI_OPTION_COMPRESSION] = {.content = SXML_EXI_OPTION_CONTENT_EMPTY},
    [SXML_EXI_OPTION_FRAGMENT] = {.content = SXML_EXI_OPTION_CONTENT_EMPTY},
    [SXML_EXI_OPTION_SCHEMA_IDENTIFIER] = {.content = SXML_EXI_OPTION_CONTENT_NILLABLE_STRING},
    [SXML_EXI_OPTION_STRICT] = {.content = SXML_EXI_OPTION_CONTENT_EMPTY},
    [SXML_EXI_OPTION_WILDCARD] = {.content = SXML_EXI_OPTION_CONTENT_SKIPPED},
};

/* An element being read or written, and its state. */
typedef struct sxml_exi_option_frame {
    sxml_exi_option_element element;
    /* A sequence: the first particle still accepted; a choice: 0 before
       its particle, 1 after. */
    uint32_t position;
} sxml_exi_option_frame;

/* The productions of a state, in event-code order: SE (qname) of the
   particles from position on, then their SE (*), then EE when the state
   may end. code_of_end is the code of EE, or SXML_EXI_NONE. */
typedef struct sxml_exi_option_state {
    sxml_exi_option_element elements[SXML_EXI_OPTION_MAXIMUM_PARTICLES];
    uint32_t particles[SXML_EXI_OPTION_MAXIMUM_PARTICLES];
    uint32_t element_count;
    uint32_t production_count;
    uint32_t code_of_end;
} sxml_exi_option_state;

static void sxml_exi_option_state_of(const sxml_exi_option_frame *frame, sxml_exi_option_state *state) {
    const sxml_exi_option_declaration *declaration = &sxml_exi_option_declarations[frame->element];
    *state = (sxml_exi_option_state){.code_of_end = SXML_EXI_NONE};
    const bool is_choice = declaration->content == SXML_EXI_OPTION_CONTENT_CHOICE;
    const bool has_particles = is_choice ? frame->position == 0 : declaration->content == SXML_EXI_OPTION_CONTENT_SEQUENCE;
    const uint32_t first = is_choice ? 0 : frame->position;
    for (uint32_t pass_index = 0; has_particles && pass_index < 2; pass_index++) {
        const bool takes_wildcards = pass_index == 1;
        for (uint32_t particle = first; particle < declaration->particle_count; particle++) {
            const sxml_exi_option_element element = declaration->particles[particle].element;
            if ((element == SXML_EXI_OPTION_WILDCARD) == takes_wildcards) {
                state->elements[state->element_count] = element;
                state->particles[state->element_count] = particle;
                state->element_count++;
            }
        }
    }
    state->production_count = state->element_count;
    if (!is_choice || frame->position != 0) {
        state->code_of_end = state->production_count++;
    }
}

/* After particle of frame's element was read or written. */
static void sxml_exi_option_advance(sxml_exi_option_frame *frame, uint32_t particle) {
    const sxml_exi_option_declaration *declaration = &sxml_exi_option_declarations[frame->element];
    if (declaration->content == SXML_EXI_OPTION_CONTENT_CHOICE) {
        frame->position = 1;
    } else {
        frame->position = declaration->particles[particle].is_repeated ? particle : particle + 1;
    }
}

/* ---------------------------------------------------------------- Writing */

typedef struct sxml_exi_option_presence {
    bool is_present[SXML_EXI_OPTION_COUNT];
} sxml_exi_option_presence;

/* Which elements the options document holds, as EXIficient writes them:
   only options that differ from their defaults (5.4, Table 5-1). */
static void sxml_exi_option_presence_of(const sxml_exi_options_document *document,
                                        sxml_exi_option_presence *presence) {
    const sxml_exi_options *options = document->options;
    bool *is_present = presence->is_present;
    *presence = (sxml_exi_option_presence){0};
    const bool is_channelized = options->alignment == SXML_EXI_ALIGNMENT_PRE_COMPRESSION ||
                                options->alignment == SXML_EXI_ALIGNMENT_COMPRESSION;
    is_present[SXML_EXI_OPTION_BYTE] = options->alignment == SXML_EXI_ALIGNMENT_BYTE_ALIGNMENT;
    is_present[SXML_EXI_OPTION_PRE_COMPRESS] = options->alignment == SXML_EXI_ALIGNMENT_PRE_COMPRESSION;
    is_present[SXML_EXI_OPTION_ALIGNMENT] = is_present[SXML_EXI_OPTION_BYTE] || is_present[SXML_EXI_OPTION_PRE_COMPRESS];
    is_present[SXML_EXI_OPTION_SELF_CONTAINED] = options->allows_self_contained;
    is_present[SXML_EXI_OPTION_VALUE_MAX_LENGTH] = options->has_value_max_length;
    is_present[SXML_EXI_OPTION_VALUE_PARTITION_CAPACITY] = options->has_value_partition_capacity;
    is_present[SXML_EXI_OPTION_UNCOMMON] =
        is_present[SXML_EXI_OPTION_ALIGNMENT] || is_present[SXML_EXI_OPTION_SELF_CONTAINED] ||
        is_present[SXML_EXI_OPTION_VALUE_MAX_LENGTH] || is_present[SXML_EXI_OPTION_VALUE_PARTITION_CAPACITY];
    is_present[SXML_EXI_OPTION_DTD] = options->fidelity.preserves_dtd;
    is_present[SXML_EXI_OPTION_PREFIXES] = options->fidelity.preserves_prefixes;
    is_present[SXML_EXI_OPTION_LEXICAL_VALUES] = options->fidelity.preserves_lexical_values;
    is_present[SXML_EXI_OPTION_COMMENTS] = options->fidelity.preserves_comments;
    is_present[SXML_EXI_OPTION_PIS] = options->fidelity.preserves_processing_instructions;
    is_present[SXML_EXI_OPTION_PRESERVE] = is_present[SXML_EXI_OPTION_DTD] || is_present[SXML_EXI_OPTION_PREFIXES] ||
                                           is_present[SXML_EXI_OPTION_LEXICAL_VALUES] ||
                                           is_present[SXML_EXI_OPTION_COMMENTS] || is_present[SXML_EXI_OPTION_PIS];
    /* Canonical EXI 3.1: blockSize only with compression or pre-compress. */
    is_present[SXML_EXI_OPTION_BLOCK_SIZE] =
        options->has_block_size && options->block_size != SXML_EXI_DEFAULT_BLOCK_SIZE && is_channelized;
    is_present[SXML_EXI_OPTION_LESSCOMMON] = is_present[SXML_EXI_OPTION_UNCOMMON] ||
                                             is_present[SXML_EXI_OPTION_PRESERVE] ||
                                             is_present[SXML_EXI_OPTION_BLOCK_SIZE];
    is_present[SXML_EXI_OPTION_COMPRESSION] = options->alignment == SXML_EXI_ALIGNMENT_COMPRESSION;
    is_present[SXML_EXI_OPTION_FRAGMENT] = options->is_fragment;
    is_present[SXML_EXI_OPTION_SCHEMA_IDENTIFIER] =
        document->includes_schema_identifier ||
        options->schema_identifier.schema_identifier_kind != SXML_EXI_SCHEMA_IDENTIFIER_ABSENT;
    is_present[SXML_EXI_OPTION_COMMON] = is_present[SXML_EXI_OPTION_COMPRESSION] ||
                                         is_present[SXML_EXI_OPTION_FRAGMENT] ||
                                         is_present[SXML_EXI_OPTION_SCHEMA_IDENTIFIER];
    is_present[SXML_EXI_OPTION_STRICT] = options->is_strict;
}

static sxml_status sxml_exi_option_write_code(sxml_exi_bit_writer *writer, const sxml_exi_option_state *state,
                                              uint32_t code) {
    return sxml_exi_write_n_bit(
        writer, (sxml_exi_bits){.value = code, .bit_count = sxml_exi_bit_width(state->production_count)});
}

/* A string value (7.1.10) that misses the string table, which is empty:
   its length plus two, then its characters (7.3.3). */
static sxml_status sxml_exi_option_write_string(sxml_exi_bit_writer *writer, sxml_byte_view text) {
    sxml_status status = sxml_exi_write_unsigned(writer, (uint64_t)sxml_exi_utf8_codepoint_count(text) + 2U);
    size_t offset = 0;
    while (status == SXML_STATUS_OK && offset < text.byte_count) {
        status = sxml_exi_write_unsigned(writer, sxml_exi_utf8_next(text, &offset));
    }
    return status;
}

/* The content of a value element: schemaId (CH, or AT (xsi:nil) at event
   code 1.0 of its start tag, 8.5.4.4.2) or an unsigned integer. */
static sxml_status sxml_exi_option_write_value(sxml_exi_bit_writer *writer, const sxml_exi_options_document *document,
                                               sxml_exi_option_element element) {
    const sxml_exi_options *options = document->options;
    switch (element) {
    case SXML_EXI_OPTION_VALUE_MAX_LENGTH:
        return sxml_exi_write_unsigned(writer, options->value_max_length);
    case SXML_EXI_OPTION_VALUE_PARTITION_CAPACITY:
        return sxml_exi_write_unsigned(writer, options->value_partition_capacity);
    case SXML_EXI_OPTION_BLOCK_SIZE:
        return sxml_exi_write_unsigned(writer, options->block_size);
    case SXML_EXI_OPTION_SCHEMA_IDENTIFIER:
        break;
    case SXML_EXI_OPTION_HEADER:
    case SXML_EXI_OPTION_LESSCOMMON:
    case SXML_EXI_OPTION_UNCOMMON:
    case SXML_EXI_OPTION_ALIGNMENT:
    case SXML_EXI_OPTION_BYTE:
    case SXML_EXI_OPTION_PRE_COMPRESS:
    case SXML_EXI_OPTION_SELF_CONTAINED:
    case SXML_EXI_OPTION_DATATYPE_REPRESENTATION_MAP:
    case SXML_EXI_OPTION_PRESERVE:
    case SXML_EXI_OPTION_DTD:
    case SXML_EXI_OPTION_PREFIXES:
    case SXML_EXI_OPTION_LEXICAL_VALUES:
    case SXML_EXI_OPTION_COMMENTS:
    case SXML_EXI_OPTION_PIS:
    case SXML_EXI_OPTION_COMMON:
    case SXML_EXI_OPTION_COMPRESSION:
    case SXML_EXI_OPTION_FRAGMENT:
    case SXML_EXI_OPTION_STRICT:
    case SXML_EXI_OPTION_WILDCARD:
    case SXML_EXI_OPTION_COUNT:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* 5.4: xsi:nil for no schema, the empty schemaId for the built-in
       types only, else the schema's text. */
    sxml_exi_schema_identifier_kind kind = options->schema_identifier.schema_identifier_kind;
    if (kind == SXML_EXI_SCHEMA_IDENTIFIER_ABSENT && document->is_schema_informed) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (kind == SXML_EXI_SCHEMA_IDENTIFIER_ABSENT) {
        kind = SXML_EXI_SCHEMA_IDENTIFIER_NONE;
    }
    const bool is_nil = kind == SXML_EXI_SCHEMA_IDENTIFIER_NONE;
    sxml_status status = sxml_exi_write_n_bit(writer, (sxml_exi_bits){.value = is_nil ? 1U : 0U, .bit_count = 1});
    if (status == SXML_STATUS_OK && is_nil) {
        status = sxml_exi_write_boolean(writer, (sxml_exi_boolean){.value = true});
    } else if (status == SXML_STATUS_OK) {
        const sxml_byte_view text =
            kind == SXML_EXI_SCHEMA_IDENTIFIER_TEXT ? options->schema_identifier.text : (sxml_byte_view){0};
        status = sxml_exi_option_write_string(writer, text);
    }
    return status;
}

sxml_status sxml_exi_write_options_document(sxml_exi_bit_writer *writer, const sxml_exi_options_document *document) {
    if (writer == NULL || document == NULL || document->options == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (document->options->datatype_representation_map.datatype_representation_count > 0) {
        return SXML_STATUS_UNSUPPORTED_FEATURE;
    }
    sxml_exi_option_presence presence;
    sxml_exi_option_presence_of(document, &presence);
    /* 8.5.1 with strict: DocContent is SE (header), SE (*); DocEnd is ED
       alone. */
    sxml_status status = sxml_exi_write_n_bit(writer, (sxml_exi_bits){.value = 0, .bit_count = 1});
    sxml_exi_option_frame frames[SXML_EXI_OPTION_MAXIMUM_DEPTH];
    size_t frame_count = 0;
    frames[frame_count++] = (sxml_exi_option_frame){.element = SXML_EXI_OPTION_HEADER};
    while (status == SXML_STATUS_OK && frame_count > 0) {
        sxml_exi_option_frame *frame = &frames[frame_count - 1];
        const sxml_exi_option_content content = sxml_exi_option_declarations[frame->element].content;
        if (content == SXML_EXI_OPTION_CONTENT_UNSIGNED || content == SXML_EXI_OPTION_CONTENT_NILLABLE_STRING) {
            /* CH is the start tag's only declared production: 0 bits
               for an unsigned integer, 1 for schemaId with its xsi:nil;
               EE follows alone. */
            status = sxml_exi_option_write_value(writer, document, frame->element);
            frame_count--;
            continue;
        }
        sxml_exi_option_state state;
        sxml_exi_option_state_of(frame, &state);
        uint32_t code = state.code_of_end;
        for (uint32_t index = 0; index < state.element_count; index++) {
            if (presence.is_present[state.elements[index]]) {
                code = index;
                break;
            }
        }
        if (code == SXML_EXI_NONE) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        status = sxml_exi_option_write_code(writer, &state, code);
        if (code == state.code_of_end) {
            frame_count--;
        } else if (frame_count == SXML_EXI_OPTION_MAXIMUM_DEPTH) {
            return SXML_STATUS_INVALID_ARGUMENT;
        } else {
            sxml_exi_option_advance(frame, state.particles[code]);
            frames[frame_count++] = (sxml_exi_option_frame){.element = state.elements[code]};
        }
    }
    return status;
}

/* ---------------------------------------------------------------- Reading */

static sxml_status sxml_exi_option_record(const sxml_exi_options_document_read *read, sxml_status status,
                                          sxml_byte_view description) {
    if (read->failure_description != NULL) {
        *read->failure_description = description;
    }
    return status;
}

#define SXML_EXI_OPTION_FAIL(read, status, literal) \
    sxml_exi_option_record((read), (status), SXML_BYTE_VIEW_FROM_LITERAL(literal))

/* Appends codepoint, a Char of XML 1.0 [2], as UTF-8. */
static sxml_status sxml_exi_option_append_codepoint(sxml_exi_buffer *buffer, uint64_t codepoint) {
    const bool is_char = codepoint == 0x9 || codepoint == 0xA || codepoint == 0xD ||
                         (codepoint >= 0x20 && codepoint <= 0xD7FF) || (codepoint >= 0xE000 && codepoint <= 0xFFFD) ||
                         (codepoint >= 0x10000 && codepoint <= 0x10FFFF);
    if (!is_char) {
        return SXML_STATUS_MALFORMED_INPUT;
    }
    uint8_t bytes[4];
    size_t byte_count = 0;
    const uint32_t value = (uint32_t)codepoint;
    if (value < 0x80) {
        bytes[byte_count++] = (uint8_t)value;
    } else if (value < 0x800) {
        bytes[byte_count++] = (uint8_t)(0xC0U | (value >> 6));
        bytes[byte_count++] = (uint8_t)(0x80U | (value & 0x3FU));
    } else if (value < 0x10000) {
        bytes[byte_count++] = (uint8_t)(0xE0U | (value >> 12));
        bytes[byte_count++] = (uint8_t)(0x80U | ((value >> 6) & 0x3FU));
        bytes[byte_count++] = (uint8_t)(0x80U | (value & 0x3FU));
    } else {
        bytes[byte_count++] = (uint8_t)(0xF0U | (value >> 18));
        bytes[byte_count++] = (uint8_t)(0x80U | ((value >> 12) & 0x3FU));
        bytes[byte_count++] = (uint8_t)(0x80U | ((value >> 6) & 0x3FU));
        bytes[byte_count++] = (uint8_t)(0x80U | (value & 0x3FU));
    }
    return sxml_exi_buffer_append(buffer, (sxml_byte_view){.bytes = bytes, .byte_count = byte_count});
}

/* The schemaId's string: a miss of the empty string table (7.3.3). */
static sxml_status sxml_exi_option_read_string(const sxml_exi_options_document_read *read) {
    uint64_t length = 0;
    sxml_status status = sxml_exi_read_unsigned(read->reader, &length);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (length < 2) {
        return SXML_EXI_OPTION_FAIL(read, SXML_STATUS_MALFORMED_INPUT,
                                    "a string table hit in the options document, whose table is empty");
    }
    length -= 2;
    /* Each character takes a byte at least. */
    const sxml_exi_bit_reader *reader = read->reader;
    if (length > reader->input.byte_count - reader->byte_position) {
        return SXML_EXI_OPTION_FAIL(read, SXML_STATUS_MALFORMED_INPUT, "a schemaId longer than the stream");
    }
    for (uint64_t character = 0; status == SXML_STATUS_OK && character < length; character++) {
        uint64_t codepoint = 0;
        status = sxml_exi_read_unsigned(read->reader, &codepoint);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_option_append_codepoint(read->schema_identifier_text, codepoint);
            if (status == SXML_STATUS_MALFORMED_INPUT) {
                return SXML_EXI_OPTION_FAIL(read, status, "a character XML does not allow in the schemaId");
            }
        }
    }
    return status;
}

/* An xsd:unsignedInt (0 to 4294967295) as an Unsigned Integer. */
static sxml_status sxml_exi_option_read_unsigned(const sxml_exi_options_document_read *read, uint32_t *value) {
    uint64_t number = 0;
    const sxml_status status = sxml_exi_read_unsigned(read->reader, &number);
    if (status == SXML_STATUS_OK && number > UINT32_MAX) {
        return SXML_EXI_OPTION_FAIL(read, SXML_STATUS_MALFORMED_INPUT, "an option value past xsd:unsignedInt");
    }
    *value = (uint32_t)number;
    return status;
}

/* The content of a value element, into the options. */
static sxml_status sxml_exi_option_read_value(const sxml_exi_options_document_read *read,
                                              sxml_exi_option_element element) {
    sxml_exi_options *options = read->options;
    switch (element) {
    case SXML_EXI_OPTION_VALUE_MAX_LENGTH:
        options->has_value_max_length = true;
        return sxml_exi_option_read_unsigned(read, &options->value_max_length);
    case SXML_EXI_OPTION_VALUE_PARTITION_CAPACITY:
        options->has_value_partition_capacity = true;
        return sxml_exi_option_read_unsigned(read, &options->value_partition_capacity);
    case SXML_EXI_OPTION_BLOCK_SIZE: {
        options->has_block_size = true;
        const sxml_status status = sxml_exi_option_read_unsigned(read, &options->block_size);
        if (status == SXML_STATUS_OK && options->block_size == 0) {
            return SXML_EXI_OPTION_FAIL(read, SXML_STATUS_MALFORMED_INPUT, "a blockSize of 0");
        }
        return status;
    }
    case SXML_EXI_OPTION_SCHEMA_IDENTIFIER:
        break;
    case SXML_EXI_OPTION_HEADER:
    case SXML_EXI_OPTION_LESSCOMMON:
    case SXML_EXI_OPTION_UNCOMMON:
    case SXML_EXI_OPTION_ALIGNMENT:
    case SXML_EXI_OPTION_BYTE:
    case SXML_EXI_OPTION_PRE_COMPRESS:
    case SXML_EXI_OPTION_SELF_CONTAINED:
    case SXML_EXI_OPTION_DATATYPE_REPRESENTATION_MAP:
    case SXML_EXI_OPTION_PRESERVE:
    case SXML_EXI_OPTION_DTD:
    case SXML_EXI_OPTION_PREFIXES:
    case SXML_EXI_OPTION_LEXICAL_VALUES:
    case SXML_EXI_OPTION_COMMENTS:
    case SXML_EXI_OPTION_PIS:
    case SXML_EXI_OPTION_COMMON:
    case SXML_EXI_OPTION_COMPRESSION:
    case SXML_EXI_OPTION_FRAGMENT:
    case SXML_EXI_OPTION_STRICT:
    case SXML_EXI_OPTION_WILDCARD:
    case SXML_EXI_OPTION_COUNT:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* The start tag: CH (0) or AT (xsi:nil) (1.0, whose second part has
       0 bits); after xsi:nil="true", TypeEmpty: EE alone. */
    uint32_t code = 0;
    sxml_status status = sxml_exi_read_n_bit(read->reader, (sxml_exi_bits_request){.bit_count = 1}, &code);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* xsi:nil="false" leads back to the start tag (8.5.4.4.2); each
       turn reads two bits, so the stream's end ends the loop. */
    while (status == SXML_STATUS_OK && code == 1) {
        sxml_exi_boolean nil = {0};
        status = sxml_exi_read_boolean(read->reader, &nil);
        if (status == SXML_STATUS_OK && nil.value) {
            options->schema_identifier.schema_identifier_kind = SXML_EXI_SCHEMA_IDENTIFIER_NONE;
            return status;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_read_n_bit(read->reader, (sxml_exi_bits_request){.bit_count = 1}, &code);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    options->schema_identifier.schema_identifier_kind = SXML_EXI_SCHEMA_IDENTIFIER_TEXT;
    return sxml_exi_option_read_string(read);
}

/* What an empty element of the options schema says. */
static void sxml_exi_option_set(sxml_exi_options *options, sxml_exi_option_element element) {
    switch (element) {
    case SXML_EXI_OPTION_BYTE:
        options->alignment = SXML_EXI_ALIGNMENT_BYTE_ALIGNMENT;
        break;
    case SXML_EXI_OPTION_PRE_COMPRESS:
        options->alignment = SXML_EXI_ALIGNMENT_PRE_COMPRESSION;
        break;
    case SXML_EXI_OPTION_COMPRESSION:
        options->alignment = SXML_EXI_ALIGNMENT_COMPRESSION;
        break;
    case SXML_EXI_OPTION_SELF_CONTAINED:
        options->allows_self_contained = true;
        break;
    case SXML_EXI_OPTION_DTD:
        options->fidelity.preserves_dtd = true;
        break;
    case SXML_EXI_OPTION_PREFIXES:
        options->fidelity.preserves_prefixes = true;
        break;
    case SXML_EXI_OPTION_LEXICAL_VALUES:
        options->fidelity.preserves_lexical_values = true;
        break;
    case SXML_EXI_OPTION_COMMENTS:
        options->fidelity.preserves_comments = true;
        break;
    case SXML_EXI_OPTION_PIS:
        options->fidelity.preserves_processing_instructions = true;
        break;
    case SXML_EXI_OPTION_FRAGMENT:
        options->is_fragment = true;
        break;
    case SXML_EXI_OPTION_STRICT:
        options->is_strict = true;
        break;
    case SXML_EXI_OPTION_HEADER:
    case SXML_EXI_OPTION_LESSCOMMON:
    case SXML_EXI_OPTION_UNCOMMON:
    case SXML_EXI_OPTION_ALIGNMENT:
    case SXML_EXI_OPTION_VALUE_MAX_LENGTH:
    case SXML_EXI_OPTION_VALUE_PARTITION_CAPACITY:
    case SXML_EXI_OPTION_DATATYPE_REPRESENTATION_MAP:
    case SXML_EXI_OPTION_PRESERVE:
    case SXML_EXI_OPTION_BLOCK_SIZE:
    case SXML_EXI_OPTION_COMMON:
    case SXML_EXI_OPTION_SCHEMA_IDENTIFIER:
    case SXML_EXI_OPTION_WILDCARD:
    case SXML_EXI_OPTION_COUNT:
        break;
    }
}

/* 5.4: the combinations an options document must not have. */
static sxml_status sxml_exi_option_check(const sxml_exi_options_document_read *read,
                                         const sxml_exi_option_presence *seen) {
    const bool *is_seen = seen->is_present;
    const sxml_exi_options *options = read->options;
    if (is_seen[SXML_EXI_OPTION_ALIGNMENT] && is_seen[SXML_EXI_OPTION_COMPRESSION]) {
        return SXML_EXI_OPTION_FAIL(read, SXML_STATUS_MALFORMED_INPUT, "alignment and compression in one options document");
    }
    const bool has_fidelity = options->fidelity.preserves_dtd || options->fidelity.preserves_prefixes ||
                              options->fidelity.preserves_comments ||
                              options->fidelity.preserves_processing_instructions;
    if (options->is_strict && (has_fidelity || options->allows_self_contained)) {
        return SXML_EXI_OPTION_FAIL(read, SXML_STATUS_MALFORMED_INPUT,
                                    "strict with dtd, prefixes, comments, pis or selfContained");
    }
    if (options->allows_self_contained && options->alignment == SXML_EXI_ALIGNMENT_COMPRESSION) {
        return SXML_EXI_OPTION_FAIL(read, SXML_STATUS_MALFORMED_INPUT, "selfContained with compression");
    }
    if (options->allows_self_contained && options->alignment == SXML_EXI_ALIGNMENT_PRE_COMPRESSION) {
        return SXML_EXI_OPTION_FAIL(read, SXML_STATUS_MALFORMED_INPUT, "selfContained with pre-compress");
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_exi_read_options_document(const sxml_exi_options_document_read *read) {
    if (read == NULL || read->reader == NULL || read->options == NULL || read->schema_identifier_text == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *read->options = (sxml_exi_options){0};
    sxml_exi_option_presence seen = {0};
    uint32_t code = 0;
    sxml_status status = sxml_exi_read_n_bit(read->reader, (sxml_exi_bits_request){.bit_count = 1}, &code);
    if (status == SXML_STATUS_OK && code != 0) {
        return SXML_EXI_OPTION_FAIL(read, SXML_STATUS_MALFORMED_INPUT, "an options document whose root is not exi:header");
    }
    sxml_exi_option_frame frames[SXML_EXI_OPTION_MAXIMUM_DEPTH];
    size_t frame_count = 0;
    frames[frame_count++] = (sxml_exi_option_frame){.element = SXML_EXI_OPTION_HEADER};
    while (status == SXML_STATUS_OK && frame_count > 0) {
        sxml_exi_option_frame *frame = &frames[frame_count - 1];
        const sxml_exi_option_content content = sxml_exi_option_declarations[frame->element].content;
        if (content == SXML_EXI_OPTION_CONTENT_UNSIGNED || content == SXML_EXI_OPTION_CONTENT_NILLABLE_STRING) {
            status = sxml_exi_option_read_value(read, frame->element);
            frame_count--;
            continue;
        }
        sxml_exi_option_state state;
        sxml_exi_option_state_of(frame, &state);
        status = sxml_exi_read_n_bit(
            read->reader, (sxml_exi_bits_request){.bit_count = sxml_exi_bit_width(state.production_count)}, &code);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (code >= state.production_count) {
            return SXML_EXI_OPTION_FAIL(read, SXML_STATUS_MALFORMED_INPUT,
                                        "an event code the options document's grammar does not have");
        }
        if (code == state.code_of_end) {
            frame_count--;
            continue;
        }
        const sxml_exi_option_element element = state.elements[code];
        if (sxml_exi_option_declarations[element].content == SXML_EXI_OPTION_CONTENT_SKIPPED) {
            return SXML_EXI_OPTION_FAIL(read, SXML_STATUS_UNSUPPORTED_FEATURE,
                                        "user-defined meta-data or a datatypeRepresentationMap in the options document");
        }
        if (frame_count == SXML_EXI_OPTION_MAXIMUM_DEPTH) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        seen.is_present[element] = true;
        sxml_exi_option_set(read->options, element);
        sxml_exi_option_advance(frame, state.particles[code]);
        frames[frame_count++] = (sxml_exi_option_frame){.element = element};
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_option_check(read, &seen);
    }
    return status;
}
