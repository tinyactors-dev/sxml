/*
 * UTF-8 strings: validation, construction, codepoint access, iteration and
 * the Unicode codepoint collation (include/sxml/string.h).
 *
 * Imitates the golden module src/memory.c. Strict validation follows
 * Unicode 16.0, section 3.9, table 3-7 "Well-Formed UTF-8 Byte Sequences".
 * Everything after validation trusts that the bytes are well-formed but
 * still reads only within them, as the header promises.
 */
#include "sxml/string.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------- Decoding */

/* A byte below 0x80 encodes a codepoint on its own. */
#define SXML_UTF8_ASCII_LIMIT 0x80U

/* Continuation bytes are 10xxxxxx: 0x80 to 0xBF. */
#define SXML_UTF8_CONTINUATION_MINIMUM 0x80U
#define SXML_UTF8_CONTINUATION_MAXIMUM 0xBFU
#define SXML_UTF8_CONTINUATION_PAYLOAD_MASK 0x3FU
#define SXML_UTF8_CONTINUATION_PAYLOAD_BITS 6U

/*
 * How a lead byte starts a well-formed encoding (Unicode 16.0, table 3-7):
 * the number of bytes, the payload bits of the lead byte, and the range the
 * second byte must fall in. The narrowed second-byte ranges are what exclude
 * overlong forms (after 0xE0 and 0xF0), surrogates (after 0xED) and values
 * above U+10FFFF (after 0xF4). Every later byte is any continuation byte.
 */
typedef struct sxml_utf8_lead_byte_form {
    /* 0 when the byte cannot start a well-formed encoding. */
    size_t encoded_byte_count;
    uint32_t payload_mask;
    uint8_t second_byte_minimum;
    uint8_t second_byte_maximum;
} sxml_utf8_lead_byte_form;

static sxml_utf8_lead_byte_form sxml_utf8_classify_lead_byte(
    uint8_t lead_byte) {
    sxml_utf8_lead_byte_form form = {
        .encoded_byte_count = 0,
        .payload_mask = 0,
        .second_byte_minimum = SXML_UTF8_CONTINUATION_MINIMUM,
        .second_byte_maximum = SXML_UTF8_CONTINUATION_MAXIMUM,
    };
    if (lead_byte < SXML_UTF8_ASCII_LIMIT) {
        form.encoded_byte_count = 1;
        form.payload_mask = 0x7FU;
    } else if (lead_byte < 0xC2U) {
        /* 0x80 to 0xBF are continuation bytes; 0xC0 and 0xC1 could only
           start overlong two-byte forms. */
    } else if (lead_byte <= 0xDFU) {
        form.encoded_byte_count = 2;
        form.payload_mask = 0x1FU;
    } else if (lead_byte <= 0xEFU) {
        form.encoded_byte_count = 3;
        form.payload_mask = 0x0FU;
        if (lead_byte == 0xE0U) {
            form.second_byte_minimum = 0xA0U;
        } else if (lead_byte == 0xEDU) {
            form.second_byte_maximum = 0x9FU;
        }
    } else if (lead_byte <= 0xF4U) {
        form.encoded_byte_count = 4;
        form.payload_mask = 0x07U;
        if (lead_byte == 0xF0U) {
            form.second_byte_minimum = 0x90U;
        } else if (lead_byte == 0xF4U) {
            form.second_byte_maximum = 0x8FU;
        }
    }
    /* 0xF5 to 0xFF never occur in well-formed UTF-8. */
    return form;
}

static bool sxml_utf8_is_continuation_byte(uint8_t byte) {
    return byte >= SXML_UTF8_CONTINUATION_MINIMUM &&
           byte <= SXML_UTF8_CONTINUATION_MAXIMUM;
}

/*
 * Checks that the encoding starting at byte_offset in utf8 is well-formed.
 * Returns its byte count, or 0 when it is ill-formed or truncated.
 * byte_offset is below utf8.byte_count.
 */
static size_t sxml_utf8_measure_well_formed(sxml_byte_view utf8,
                                            size_t byte_offset) {
    sxml_utf8_lead_byte_form form =
        sxml_utf8_classify_lead_byte(utf8.bytes[byte_offset]);
    if (form.encoded_byte_count == 0) {
        return 0;
    }
    /* Subtracting, not adding, so that no sum can wrap. */
    if (form.encoded_byte_count > utf8.byte_count - byte_offset) {
        return 0;
    }
    if (form.encoded_byte_count == 1) {
        return 1;
    }
    uint8_t second_byte = utf8.bytes[byte_offset + 1];
    if (second_byte < form.second_byte_minimum ||
        second_byte > form.second_byte_maximum) {
        return 0;
    }
    for (size_t byte_index = 2; byte_index < form.encoded_byte_count;
         byte_index++) {
        if (!sxml_utf8_is_continuation_byte(
                utf8.bytes[byte_offset + byte_index])) {
            return 0;
        }
    }
    return form.encoded_byte_count;
}

/*
 * Decodes the codepoint starting at byte_offset in utf8 into *codepoint and
 * returns its byte count, at least 1. byte_offset is below utf8.byte_count.
 *
 * Trusts that the bytes are well-formed, but stays within utf8 whatever they
 * are: an encoding cut short by the end of utf8 is decoded from the bytes
 * that are there, and a byte that cannot start an encoding is decoded as
 * itself. Those results are the "unspecified" ones the header allows.
 */
static size_t sxml_utf8_decode_trusted(sxml_byte_view utf8,
                                       size_t byte_offset,
                                       uint32_t *codepoint) {
    uint8_t lead_byte = utf8.bytes[byte_offset];
    sxml_utf8_lead_byte_form form = sxml_utf8_classify_lead_byte(lead_byte);
    size_t remaining_byte_count = utf8.byte_count - byte_offset;
    if (form.encoded_byte_count <= 1) {
        *codepoint = lead_byte;
        return 1;
    }
    size_t encoded_byte_count = form.encoded_byte_count < remaining_byte_count
                                    ? form.encoded_byte_count
                                    : remaining_byte_count;
    uint32_t decoded = lead_byte & form.payload_mask;
    for (size_t byte_index = 1; byte_index < encoded_byte_count;
         byte_index++) {
        decoded = (decoded << SXML_UTF8_CONTINUATION_PAYLOAD_BITS) |
                  (utf8.bytes[byte_offset + byte_index] &
                   SXML_UTF8_CONTINUATION_PAYLOAD_MASK);
    }
    *codepoint = decoded;
    return encoded_byte_count;
}

/* ----------------------------------------------------------- Validation */

sxml_status sxml_utf8_validate(sxml_byte_view utf8,
                               sxml_utf8_validation *validation) {
    size_t byte_offset = 0;
    size_t codepoint_count = 0;
    bool is_ascii = true;
    sxml_status status = SXML_STATUS_OK;
    while (byte_offset < utf8.byte_count) {
        uint64_t word = 0;
        if (utf8.byte_count - byte_offset >= sizeof word) {
            memcpy(&word, utf8.bytes + byte_offset, sizeof word);
            if ((word & UINT64_C(0x8080808080808080)) == 0) {
                /* Eight ASCII bytes, eight codepoints. */
                byte_offset += sizeof word;
                codepoint_count += sizeof word;
                continue;
            }
        }
        size_t encoded_byte_count =
            sxml_utf8_measure_well_formed(utf8, byte_offset);
        if (encoded_byte_count == 0) {
            status = SXML_STATUS_INVALID_UTF8;
            break;
        }
        if (encoded_byte_count > 1) {
            is_ascii = false;
        }
        byte_offset += encoded_byte_count;
        codepoint_count++;
    }
    /* Filled on failure too: it then describes the well-formed prefix. */
    *validation = (sxml_utf8_validation){
        .valid_byte_count = byte_offset,
        .codepoint_count = codepoint_count,
        .is_ascii = is_ascii,
    };
    return status;
}

sxml_status sxml_string_from_utf8(sxml_byte_view utf8, sxml_string *string) {
    sxml_utf8_validation validation = {
        .valid_byte_count = 0,
        .codepoint_count = 0,
        .is_ascii = false,
    };
    sxml_status status = sxml_utf8_validate(utf8, &validation);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *string = (sxml_string){
        .utf8 = utf8,
        .codepoint_count = validation.codepoint_count,
        .is_ascii = validation.is_ascii,
    };
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------ Codepoint access */

sxml_status sxml_string_codepoint_at(sxml_string string,
                                     size_t codepoint_index,
                                     uint32_t *codepoint) {
    if (codepoint_index >= string.codepoint_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    /* The byte_count check keeps a string with an inconsistent flag inside
       its bytes. */
    if (string.is_ascii && codepoint_index < string.utf8.byte_count) {
        *codepoint = string.utf8.bytes[codepoint_index];
        return SXML_STATUS_OK;
    }
    sxml_codepoint_iterator iterator = {.string = string, .byte_offset = 0};
    uint32_t current_codepoint = 0;
    for (size_t skipped_count = 0; skipped_count < codepoint_index;
         skipped_count++) {
        if (!sxml_codepoint_iterator_next(&iterator, &current_codepoint)) {
            /* Only for bytes that do not match codepoint_count. */
            return SXML_STATUS_INDEX_OUT_OF_RANGE;
        }
    }
    if (!sxml_codepoint_iterator_next(&iterator, &current_codepoint)) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    *codepoint = current_codepoint;
    return SXML_STATUS_OK;
}

bool sxml_codepoint_iterator_next(sxml_codepoint_iterator *iterator,
                                  uint32_t *codepoint) {
    sxml_byte_view utf8 = iterator->string.utf8;
    if (iterator->byte_offset >= utf8.byte_count) {
        return false;
    }
    iterator->byte_offset +=
        sxml_utf8_decode_trusted(utf8, iterator->byte_offset, codepoint);
    return true;
}

/* ----------------------------------------------------------- Comparison */

/*
 * UTF-8 was designed so that byte order of well-formed encodings is
 * codepoint order (Unicode 16.0, section 2.5 "Encoding Forms"), so the
 * codepoint collation (F&O 4.0, section 5.3.2) needs no decoding.
 */
sxml_ordering sxml_string_compare(sxml_string first, sxml_string second) {
    size_t common_byte_count = first.utf8.byte_count < second.utf8.byte_count
                                   ? first.utf8.byte_count
                                   : second.utf8.byte_count;
    /* memcmp with a NULL pointer is undefined even for a count of 0. */
    if (common_byte_count != 0) {
        int difference =
            memcmp(first.utf8.bytes, second.utf8.bytes, common_byte_count);
        if (difference != 0) {
            return difference < 0 ? SXML_ORDERING_LESS : SXML_ORDERING_GREATER;
        }
    }
    if (first.utf8.byte_count == second.utf8.byte_count) {
        return SXML_ORDERING_EQUAL;
    }
    return first.utf8.byte_count < second.utf8.byte_count
               ? SXML_ORDERING_LESS
               : SXML_ORDERING_GREATER;
}

/* Every codepoint has exactly one well-formed encoding, so equal codepoints
   means equal bytes. */
bool sxml_string_equals(sxml_string first, sxml_string second) {
    return sxml_byte_view_equals(first.utf8, second.utf8);
}
