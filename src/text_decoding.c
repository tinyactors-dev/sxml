/*
 * Text decoding (src/text_decoding_internal.h): the effective encoding of
 * F&O 4.0, section 17.1.5 and EXPath Binary 4.0, bin:infer-encoding, and
 * the decoder of fn:unparsed-text, fn:json-doc and bin:decode-string. Moved
 * here from src/function_library_resource.c (packet 2.12, D131) so the
 * EXPath Binary feature shares it (packet 7.8).
 *
 * Golden file: src/function_library_resource.c, where the code began.
 */
#include "text_decoding_internal.h"

#include "xml_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The byte order mark that starts content, if any. */
typedef enum byte_order_mark {
    SXML_BYTE_ORDER_MARK_NONE,
    SXML_BYTE_ORDER_MARK_UTF8,
    SXML_BYTE_ORDER_MARK_UTF16_BIG_ENDIAN,
    SXML_BYTE_ORDER_MARK_UTF16_LITTLE_ENDIAN,
} byte_order_mark;

static byte_order_mark find_byte_order_mark(sxml_byte_view content) {
    const uint8_t *bytes = content.bytes;
    if (content.byte_count >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        return SXML_BYTE_ORDER_MARK_UTF8;
    }
    if (content.byte_count >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        return SXML_BYTE_ORDER_MARK_UTF16_BIG_ENDIAN;
    }
    if (content.byte_count >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        return SXML_BYTE_ORDER_MARK_UTF16_LITTLE_ENDIAN;
    }
    return SXML_BYTE_ORDER_MARK_NONE;
}

/*
 * Chooses the effective encoding and start position (F&O 4.0, section
 * 17.1.5, after bin:infer-encoding): a byte order mark decides when there
 * is no candidate and is skipped when it agrees with the candidate; a
 * candidate "UTF-16" without one is big-endian; no candidate and no mark
 * is UTF-8. A mark the candidate disagrees with is decoded as characters
 * of the candidate (so it fails, or is U+FEFF's bytes in ISO-8859-1).
 * False for an encoding name sxml does not support (FOUT1190).
 */
bool sxml_text_decoding_choose(sxml_text_decoding_choice choice,
                               sxml_text_decoding *decoding) {
    const sxml_byte_view content = choice.content;
    const sxml_byte_view candidate = choice.candidate;
    const byte_order_mark mark = find_byte_order_mark(content);
    *decoding = (sxml_text_decoding){.encoding = SXML_TEXT_ENCODING_UTF8,
                                .start_offset = 0,
                                .has_candidate = candidate.byte_count > 0};
    if (candidate.byte_count == 0) {
        switch (mark) {
        case SXML_BYTE_ORDER_MARK_UTF8:
            decoding->start_offset = 3;
            break;
        case SXML_BYTE_ORDER_MARK_UTF16_BIG_ENDIAN:
            *decoding = (sxml_text_decoding){.encoding = SXML_TEXT_ENCODING_UTF16_BIG_ENDIAN,
                                        .start_offset = 2,
                                        .has_candidate = false};
            break;
        case SXML_BYTE_ORDER_MARK_UTF16_LITTLE_ENDIAN:
            *decoding = (sxml_text_decoding){.encoding = SXML_TEXT_ENCODING_UTF16_LITTLE_ENDIAN,
                                        .start_offset = 2,
                                        .has_candidate = false};
            break;
        case SXML_BYTE_ORDER_MARK_NONE:
            break;
        }
        return true;
    }
    switch (sxml_xml_classify_encoding_name(candidate)) {
    case SXML_XML_ENCODING_FAMILY_UNSUPPORTED:
        return false;
    case SXML_XML_ENCODING_FAMILY_UTF8:
        decoding->start_offset = mark == SXML_BYTE_ORDER_MARK_UTF8 ? 3 : 0;
        return true;
    case SXML_XML_ENCODING_FAMILY_UTF16:
        decoding->encoding = mark == SXML_BYTE_ORDER_MARK_UTF16_LITTLE_ENDIAN
                                 ? SXML_TEXT_ENCODING_UTF16_LITTLE_ENDIAN
                                 : SXML_TEXT_ENCODING_UTF16_BIG_ENDIAN;
        decoding->start_offset = mark == SXML_BYTE_ORDER_MARK_UTF16_LITTLE_ENDIAN ||
                                         mark == SXML_BYTE_ORDER_MARK_UTF16_BIG_ENDIAN
                                     ? 2
                                     : 0;
        return true;
    case SXML_XML_ENCODING_FAMILY_UTF16_BIG_ENDIAN:
        decoding->encoding = SXML_TEXT_ENCODING_UTF16_BIG_ENDIAN;
        decoding->start_offset = mark == SXML_BYTE_ORDER_MARK_UTF16_BIG_ENDIAN ? 2 : 0;
        return true;
    case SXML_XML_ENCODING_FAMILY_UTF16_LITTLE_ENDIAN:
        decoding->encoding = SXML_TEXT_ENCODING_UTF16_LITTLE_ENDIAN;
        decoding->start_offset = mark == SXML_BYTE_ORDER_MARK_UTF16_LITTLE_ENDIAN ? 2 : 0;
        return true;
    case SXML_XML_ENCODING_FAMILY_ISO_8859_1:
        decoding->encoding = SXML_TEXT_ENCODING_ISO_8859_1;
        return true;
    case SXML_XML_ENCODING_FAMILY_US_ASCII:
        decoding->encoding = SXML_TEXT_ENCODING_US_ASCII;
        return true;
    }
    return false;
}

/* One step of decoding: a codepoint, or a malformed unit to skip. */
typedef struct decoded_unit {
    bool is_well_formed;
    uint32_t codepoint;
    size_t byte_count;
} decoded_unit;

/* The continuation bytes of a UTF-8 sequence, for a lead byte; 0 for an
   invalid lead byte (RFC 3629, section 4). */
static size_t utf8_continuation_count(uint8_t lead_byte) {
    if (lead_byte >= 0xC2 && lead_byte <= 0xDF) {
        return 1;
    }
    if (lead_byte >= 0xE0 && lead_byte <= 0xEF) {
        return 2;
    }
    if (lead_byte >= 0xF0 && lead_byte <= 0xF4) {
        return 3;
    }
    return 0;
}

/* Decodes the UTF-8 sequence at offset (RFC 3629, table 3-7 of Unicode:
   no overlong forms, surrogates or codepoints past U+10FFFF). */
static decoded_unit decode_utf8(sxml_byte_view content, size_t offset) {
    const uint8_t lead_byte = content.bytes[offset];
    if (lead_byte < 0x80) {
        return (decoded_unit){.is_well_formed = true, .codepoint = lead_byte, .byte_count = 1};
    }
    const decoded_unit malformed = {.is_well_formed = false, .codepoint = 0, .byte_count = 1};
    const size_t continuation_count = utf8_continuation_count(lead_byte);
    if (continuation_count == 0 || content.byte_count - offset <= continuation_count) {
        return malformed;
    }
    uint32_t codepoint = lead_byte & (0x7FU >> (continuation_count + 1));
    for (size_t continuation_index = 1; continuation_index <= continuation_count;
         continuation_index += 1) {
        const uint8_t byte = content.bytes[offset + continuation_index];
        if ((byte & 0xC0U) != 0x80U) {
            return malformed;
        }
        codepoint = (codepoint << 6U) | (byte & 0x3FU);
    }
    static const uint32_t smallest_codepoints[4] = {0, 0x80, 0x800, 0x10000};
    if (codepoint < smallest_codepoints[continuation_count] || codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        return malformed;
    }
    return (decoded_unit){
        .is_well_formed = true, .codepoint = codepoint, .byte_count = continuation_count + 1};
}

/* The UTF-16 code unit at offset, which has two bytes left. */
static uint32_t utf16_code_unit(sxml_byte_view content, size_t offset, sxml_text_encoding encoding) {
    const bool is_big_endian = encoding == SXML_TEXT_ENCODING_UTF16_BIG_ENDIAN;
    const uint32_t first = content.bytes[offset];
    const uint32_t second = content.bytes[offset + 1];
    return is_big_endian ? (first << 8U) | second : (second << 8U) | first;
}

/* Decodes the UTF-16 code units at offset: a pair for a supplementary
   codepoint; a lone surrogate or an odd last byte is malformed. */
static decoded_unit decode_utf16(sxml_byte_view content, size_t offset, sxml_text_encoding encoding) {
    if (content.byte_count - offset < 2) {
        return (decoded_unit){.is_well_formed = false, .codepoint = 0, .byte_count = 1};
    }
    const uint32_t unit = utf16_code_unit(content, offset, encoding);
    const decoded_unit lone = {.is_well_formed = false, .codepoint = 0, .byte_count = 2};
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
        return lone;
    }
    if (unit < 0xD800 || unit > 0xDBFF) {
        return (decoded_unit){.is_well_formed = true, .codepoint = unit, .byte_count = 2};
    }
    if (content.byte_count - offset < 4) {
        return lone;
    }
    const uint32_t low_unit = utf16_code_unit(content, offset + 2, encoding);
    if (low_unit < 0xDC00 || low_unit > 0xDFFF) {
        return lone;
    }
    return (decoded_unit){.is_well_formed = true,
                          .codepoint = 0x10000U + ((unit - 0xD800U) << 10U) + (low_unit - 0xDC00U),
                          .byte_count = 4};
}

static decoded_unit decode_unit(sxml_byte_view content, size_t offset, sxml_text_encoding encoding) {
    const uint8_t byte = content.bytes[offset];
    switch (encoding) {
    case SXML_TEXT_ENCODING_UTF8:
        return decode_utf8(content, offset);
    case SXML_TEXT_ENCODING_UTF16_BIG_ENDIAN:
    case SXML_TEXT_ENCODING_UTF16_LITTLE_ENDIAN:
        return decode_utf16(content, offset, encoding);
    case SXML_TEXT_ENCODING_ISO_8859_1:
        return (decoded_unit){.is_well_formed = true, .codepoint = byte, .byte_count = 1};
    case SXML_TEXT_ENCODING_US_ASCII:
        break;
    }
    return (decoded_unit){.is_well_formed = byte < 0x80, .codepoint = byte, .byte_count = 1};
}

/* Appends codepoint, or a line feed for a line end when newlines are
   normalized: CR LF and a lone CR become LF (F&O 4.0, section 17.1.5,
   normalize-newlines). */
typedef struct newline_state {
    bool follows_carriage_return;
} newline_state;

static sxml_status append_character(sxml_library_text *text, newline_state *state,
                                    uint32_t codepoint) {
    const bool was_carriage_return = state->follows_carriage_return;
    state->follows_carriage_return = codepoint == 0x0D;
    if (codepoint == 0x0D) {
        return sxml_function_library_text_append_codepoint(text, 0x0A);
    }
    if (codepoint == 0x0A && was_carriage_return) {
        return SXML_STATUS_OK;
    }
    return sxml_function_library_text_append_codepoint(text, codepoint);
}

sxml_status sxml_text_decode(const sxml_text_decoding_request *request, sxml_library_text *text,
                             sxml_text_decoding_failure *failure) {
    *failure = SXML_TEXT_DECODING_FAILURE_NONE;
    const sxml_byte_view content = request->content;
    newline_state state = {.follows_carriage_return = false};
    size_t offset = request->decoding.start_offset;
    while (offset < content.byte_count) {
        const decoded_unit unit = decode_unit(content, offset, request->decoding.encoding);
        uint32_t codepoint = unit.codepoint;
        if (!unit.is_well_formed ||
            (!request->permits_any_character && !sxml_xml_codepoint_is_char(codepoint))) {
            if (!request->has_fallback) {
                *failure = unit.is_well_formed || request->decoding.has_candidate
                               ? SXML_TEXT_DECODING_FAILURE_ENCODING
                               : SXML_TEXT_DECODING_FAILURE_UNDECODABLE;
                return SXML_STATUS_OK;
            }
            codepoint = 0xFFFD;
        }
        sxml_status status = SXML_STATUS_OK;
        if (request->normalizes_newlines) {
            status = append_character(text, &state, codepoint);
        } else {
            status = sxml_function_library_text_append_codepoint(text, codepoint);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        offset += unit.byte_count;
    }
    return SXML_STATUS_OK;
}
