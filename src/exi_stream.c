/*
 * EXI bit streams (EXI 1.0, 7.1 "Built-in EXI Datatype Representations";
 * D333): bits most significant first; n-bit unsigned integers in whole
 * bytes, least significant byte first, with byte alignment (7.1.9);
 * Unsigned Integers in 7-bit groups (7.1.6). Golden: src/text_decoding.c's
 * byte-level loops.
 */
#include <string.h>

#include "exi_internal.h"
#include "memory_internal.h"

uint32_t sxml_exi_bit_width(uint64_t value) {
    uint32_t width = 0;
    while (width < 64 && (UINT64_C(1) << width) < value) {
        width++;
    }
    return width;
}

/* ---------------------------------------------------------------- Reading */

static sxml_status sxml_exi_read_bit_group(sxml_exi_bit_reader *reader, uint32_t bit_count, uint32_t *value) {
    uint32_t result = 0;
    uint32_t remaining = bit_count;
    while (remaining > 0) {
        if (reader->byte_position >= reader->input.byte_count) {
            return SXML_STATUS_UNEXPECTED_END_OF_INPUT;
        }
        const uint32_t available = 8U - reader->bit_offset;
        const uint32_t taken = remaining < available ? remaining : available;
        const uint32_t byte = reader->input.bytes[reader->byte_position];
        const uint32_t shift = available - taken;
        const uint32_t bits = (byte >> shift) & ((1U << taken) - 1U);
        result = taken == 32 ? bits : (result << taken) | bits;
        remaining -= taken;
        reader->bit_offset += taken;
        if (reader->bit_offset == 8) {
            reader->bit_offset = 0;
            reader->byte_position++;
        }
    }
    *value = result;
    return SXML_STATUS_OK;
}

sxml_status sxml_exi_read_bits(sxml_exi_bit_reader *reader, sxml_exi_bits_request request, uint32_t *value) {
    if (request.bit_count > 32) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_exi_read_bit_group(reader, request.bit_count, value);
}

sxml_status sxml_exi_read_n_bit(sxml_exi_bit_reader *reader, sxml_exi_bits_request request, uint32_t *value) {
    if (request.bit_count > 32) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (!reader->is_byte_aligned) {
        return sxml_exi_read_bit_group(reader, request.bit_count, value);
    }
    /* 7.1.9: the minimum number of bytes, least significant byte first. */
    const uint32_t byte_count = (request.bit_count + 7U) / 8U;
    uint32_t result = 0;
    for (uint32_t byte_index = 0; byte_index < byte_count; byte_index++) {
        uint32_t byte = 0;
        sxml_status status = sxml_exi_read_bit_group(reader, 8, &byte);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        result |= byte << (8U * byte_index);
    }
    *value = result;
    return SXML_STATUS_OK;
}

sxml_status sxml_exi_read_unsigned(sxml_exi_bit_reader *reader, uint64_t *value) {
    uint64_t result = 0;
    uint32_t shift = 0;
    for (;;) {
        uint32_t octet = 0;
        sxml_status status = sxml_exi_read_bit_group(reader, 8, &octet);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const uint64_t group = octet & 0x7FU;
        if (shift >= 64 || (shift > 57 && (group >> (64U - shift)) != 0)) {
            if (group != 0) {
                return SXML_STATUS_MALFORMED_INPUT;
            }
        } else {
            result |= group << shift;
        }
        if ((octet & 0x80U) == 0) {
            break;
        }
        shift += 7;
        /* A run of zero groups past 64 bits is still the same value, but
           nothing writes one: bound the work it can cause. */
        if (shift > 640) {
            return SXML_STATUS_MALFORMED_INPUT;
        }
    }
    *value = result;
    return SXML_STATUS_OK;
}

sxml_status sxml_exi_read_boolean(sxml_exi_bit_reader *reader, sxml_exi_boolean *boolean) {
    uint32_t bit = 0;
    sxml_status status = sxml_exi_read_n_bit(reader, (sxml_exi_bits_request){.bit_count = 1}, &bit);
    if (status == SXML_STATUS_OK) {
        boolean->value = bit != 0;
    }
    return status;
}

void sxml_exi_reader_align(sxml_exi_bit_reader *reader) {
    if (reader->bit_offset != 0) {
        reader->bit_offset = 0;
        reader->byte_position++;
    }
}

/* ---------------------------------------------------------------- Writing */

static sxml_status sxml_exi_writer_put_byte(sxml_exi_bit_writer *writer, uint8_t byte) {
    if (writer->byte_count == writer->capacity) {
        void *elements = writer->bytes;
        sxml_status status = sxml_allocator_reserve_array(
            writer->allocator, &(sxml_array_reservation_request){.elements = &elements,
                                                                 .capacity = &writer->capacity,
                                                                 .element_count = writer->byte_count,
                                                                 .required_count = writer->byte_count + 1,
                                                                 .element_size = 1,
                                                                 .element_alignment = 1,
                                                                 .minimum_capacity = 256});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        writer->bytes = elements;
    }
    writer->bytes[writer->byte_count++] = byte;
    return SXML_STATUS_OK;
}

static sxml_status sxml_exi_write_bit_group(sxml_exi_bit_writer *writer, sxml_exi_bits bits) {
    uint32_t remaining = bits.bit_count;
    while (remaining > 0) {
        const uint32_t room = 8U - writer->pending_bit_count;
        const uint32_t taken = remaining < room ? remaining : room;
        const uint32_t shift = remaining - taken;
        const uint32_t chunk = (bits.value >> shift) & ((1U << taken) - 1U);
        writer->pending_bits = (writer->pending_bits << taken) | chunk;
        writer->pending_bit_count += taken;
        remaining -= taken;
        if (writer->pending_bit_count == 8) {
            sxml_status status = sxml_exi_writer_put_byte(writer, (uint8_t)writer->pending_bits);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            writer->pending_bits = 0;
            writer->pending_bit_count = 0;
        }
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_exi_write_bits(sxml_exi_bit_writer *writer, sxml_exi_bits bits) {
    if (bits.bit_count > 32) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return sxml_exi_write_bit_group(writer, bits);
}

sxml_status sxml_exi_write_n_bit(sxml_exi_bit_writer *writer, sxml_exi_bits bits) {
    if (bits.bit_count > 32) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (!writer->is_byte_aligned) {
        return sxml_exi_write_bit_group(writer, bits);
    }
    const uint32_t byte_count = (bits.bit_count + 7U) / 8U;
    for (uint32_t byte_index = 0; byte_index < byte_count; byte_index++) {
        sxml_status status = sxml_exi_write_bit_group(
            writer, (sxml_exi_bits){.value = (bits.value >> (8U * byte_index)) & 0xFFU, .bit_count = 8});
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_exi_write_unsigned(sxml_exi_bit_writer *writer, uint64_t value) {
    uint64_t remaining = value;
    for (;;) {
        const uint32_t group = (uint32_t)(remaining & 0x7FU);
        remaining >>= 7;
        sxml_status status =
            sxml_exi_write_bit_group(writer, (sxml_exi_bits){.value = group | (remaining != 0 ? 0x80U : 0U), .bit_count = 8});
        if (status != SXML_STATUS_OK || remaining == 0) {
            return status;
        }
    }
}

sxml_status sxml_exi_write_boolean(sxml_exi_bit_writer *writer, sxml_exi_boolean boolean) {
    return sxml_exi_write_n_bit(writer, (sxml_exi_bits){.value = boolean.value ? 1U : 0U, .bit_count = 1});
}

sxml_status sxml_exi_writer_align(sxml_exi_bit_writer *writer) {
    if (writer->pending_bit_count == 0) {
        return SXML_STATUS_OK;
    }
    return sxml_exi_write_bit_group(writer, (sxml_exi_bits){.value = 0, .bit_count = 8U - writer->pending_bit_count});
}

sxml_byte_view sxml_exi_writer_bytes(const sxml_exi_bit_writer *writer) {
    return (sxml_byte_view){.bytes = writer->bytes, .byte_count = writer->byte_count};
}

void sxml_exi_writer_release(sxml_exi_bit_writer *writer) {
    sxml_allocator_release(writer->allocator, writer->bytes);
    writer->bytes = NULL;
    writer->byte_count = 0;
    writer->capacity = 0;
}

/* ------------------------------------------------------------- Characters */

uint32_t sxml_exi_utf8_next(sxml_byte_view text, size_t *offset) {
    const uint8_t *bytes = text.bytes + *offset;
    const size_t left = text.byte_count - *offset;
    uint32_t codepoint = bytes[0];
    size_t length = 1;
    if (codepoint >= 0xF0 && left >= 4) {
        codepoint = ((codepoint & 0x07U) << 18) | ((bytes[1] & 0x3FU) << 12) | ((bytes[2] & 0x3FU) << 6) |
                    (bytes[3] & 0x3FU);
        length = 4;
    } else if (codepoint >= 0xE0 && left >= 3) {
        codepoint = ((codepoint & 0x0FU) << 12) | ((bytes[1] & 0x3FU) << 6) | (bytes[2] & 0x3FU);
        length = 3;
    } else if (codepoint >= 0xC0 && left >= 2) {
        codepoint = ((codepoint & 0x1FU) << 6) | (bytes[1] & 0x3FU);
        length = 2;
    }
    *offset += length;
    return codepoint;
}

size_t sxml_exi_utf8_codepoint_count(sxml_byte_view text) {
    size_t codepoint_count = 0;
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index++) {
        if ((text.bytes[byte_index] & 0xC0U) != 0x80U) {
            codepoint_count++;
        }
    }
    return codepoint_count;
}
