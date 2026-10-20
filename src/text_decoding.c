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
    *decoding = (sxml_text_decoding){.encoding = SXML_XML_ENCODING_UTF8,
                                .start_offset = 0,
                                .has_candidate = candidate.byte_count > 0};
    if (candidate.byte_count == 0) {
        switch (mark) {
        case SXML_BYTE_ORDER_MARK_UTF8:
            decoding->start_offset = 3;
            break;
        case SXML_BYTE_ORDER_MARK_UTF16_BIG_ENDIAN:
            *decoding = (sxml_text_decoding){.encoding = SXML_XML_ENCODING_UTF16_BIG_ENDIAN,
                                        .start_offset = 2,
                                        .has_candidate = false};
            break;
        case SXML_BYTE_ORDER_MARK_UTF16_LITTLE_ENDIAN:
            *decoding = (sxml_text_decoding){.encoding = SXML_XML_ENCODING_UTF16_LITTLE_ENDIAN,
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
                                 ? SXML_XML_ENCODING_UTF16_LITTLE_ENDIAN
                                 : SXML_XML_ENCODING_UTF16_BIG_ENDIAN;
        decoding->start_offset = mark == SXML_BYTE_ORDER_MARK_UTF16_LITTLE_ENDIAN ||
                                         mark == SXML_BYTE_ORDER_MARK_UTF16_BIG_ENDIAN
                                     ? 2
                                     : 0;
        return true;
    case SXML_XML_ENCODING_FAMILY_UTF16_BIG_ENDIAN:
        decoding->encoding = SXML_XML_ENCODING_UTF16_BIG_ENDIAN;
        decoding->start_offset = mark == SXML_BYTE_ORDER_MARK_UTF16_BIG_ENDIAN ? 2 : 0;
        return true;
    case SXML_XML_ENCODING_FAMILY_UTF16_LITTLE_ENDIAN:
        decoding->encoding = SXML_XML_ENCODING_UTF16_LITTLE_ENDIAN;
        decoding->start_offset = mark == SXML_BYTE_ORDER_MARK_UTF16_LITTLE_ENDIAN ? 2 : 0;
        return true;
    case SXML_XML_ENCODING_FAMILY_ISO_8859_1:
        decoding->encoding = SXML_XML_ENCODING_ISO_8859_1;
        return true;
    case SXML_XML_ENCODING_FAMILY_US_ASCII:
        decoding->encoding = SXML_XML_ENCODING_US_ASCII;
        return true;
    }
    return false;
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
    newline_state state = {.follows_carriage_return = false};
    /* F&O 4.0, 17.1.5: with fallback, what does not decode becomes
       U+FFFD; without, it is an error. */
    sxml_xml_decoder decoder = {.input = request->content,
                                .encoding = request->decoding.encoding,
                                .error_rule = request->has_fallback
                                                  ? SXML_XML_DECODING_ERROR_RULE_REPLACE
                                                  : SXML_XML_DECODING_ERROR_RULE_FAIL,
                                .byte_offset = request->decoding.start_offset};
    while (true) {
        const sxml_xml_decoding_step step = sxml_xml_decoder_next(&decoder);
        if (step.step_kind == SXML_XML_DECODING_STEP_END) {
            return SXML_STATUS_OK;
        }
        if (step.step_kind == SXML_XML_DECODING_STEP_FAILURE) {
            *failure = request->decoding.has_candidate ? SXML_TEXT_DECODING_FAILURE_ENCODING
                                                       : SXML_TEXT_DECODING_FAILURE_UNDECODABLE;
            return SXML_STATUS_OK;
        }
        uint32_t codepoint = step.codepoint;
        if (step.step_kind == SXML_XML_DECODING_STEP_CODEPOINT &&
            !request->permits_any_character && !sxml_xml_codepoint_is_char(codepoint)) {
            if (!request->has_fallback) {
                *failure = SXML_TEXT_DECODING_FAILURE_ENCODING;
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
    }
}

/* ------------------------------------------------------------ Encoding */

static void put_utf16_unit(sxml_text_encoded_codepoint *encoded, uint32_t unit, bool is_big_endian) {
    encoded->bytes[encoded->byte_count] = (uint8_t)(is_big_endian ? unit >> 8U : unit & 0xFFU);
    encoded->bytes[encoded->byte_count + 1] = (uint8_t)(is_big_endian ? unit & 0xFFU : unit >> 8U);
    encoded->byte_count += 2;
}

sxml_text_encoded_codepoint sxml_text_encode_codepoint(sxml_xml_encoding_family family,
                                                       uint32_t codepoint) {
    sxml_text_encoded_codepoint encoded = {.bytes = {0}, .byte_count = 0};
    switch (family) {
    case SXML_XML_ENCODING_FAMILY_UTF8:
        if (codepoint < 0x80) {
            encoded.bytes[0] = (uint8_t)codepoint;
            encoded.byte_count = 1;
        } else if (codepoint < 0x800) {
            encoded.bytes[0] = (uint8_t)(0xC0U | (codepoint >> 6U));
            encoded.bytes[1] = (uint8_t)(0x80U | (codepoint & 0x3FU));
            encoded.byte_count = 2;
        } else if (codepoint < 0x10000) {
            encoded.bytes[0] = (uint8_t)(0xE0U | (codepoint >> 12U));
            encoded.bytes[1] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
            encoded.bytes[2] = (uint8_t)(0x80U | (codepoint & 0x3FU));
            encoded.byte_count = 3;
        } else {
            encoded.bytes[0] = (uint8_t)(0xF0U | (codepoint >> 18U));
            encoded.bytes[1] = (uint8_t)(0x80U | ((codepoint >> 12U) & 0x3FU));
            encoded.bytes[2] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
            encoded.bytes[3] = (uint8_t)(0x80U | (codepoint & 0x3FU));
            encoded.byte_count = 4;
        }
        break;
    case SXML_XML_ENCODING_FAMILY_UTF16:
    case SXML_XML_ENCODING_FAMILY_UTF16_BIG_ENDIAN:
    case SXML_XML_ENCODING_FAMILY_UTF16_LITTLE_ENDIAN: {
        const bool is_big_endian = family != SXML_XML_ENCODING_FAMILY_UTF16_LITTLE_ENDIAN;
        if (codepoint < 0x10000) {
            put_utf16_unit(&encoded, codepoint, is_big_endian);
        } else {
            put_utf16_unit(&encoded, 0xD800U + ((codepoint - 0x10000U) >> 10U), is_big_endian);
            put_utf16_unit(&encoded, 0xDC00U + ((codepoint - 0x10000U) & 0x3FFU), is_big_endian);
        }
        break;
    }
    case SXML_XML_ENCODING_FAMILY_ISO_8859_1:
    case SXML_XML_ENCODING_FAMILY_US_ASCII:
        if (codepoint < (family == SXML_XML_ENCODING_FAMILY_US_ASCII ? 0x80U : 0x100U)) {
            encoded.bytes[0] = (uint8_t)codepoint;
            encoded.byte_count = 1;
        }
        break;
    case SXML_XML_ENCODING_FAMILY_UNSUPPORTED:
        break;
    }
    return encoded;
}
