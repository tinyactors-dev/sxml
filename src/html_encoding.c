/*
 * The HTML input byte stream (src/html_internal.h; HTML Living Standard,
 * 13.2.3 "The input byte stream" and 13.2.3.5 "Preprocessing the input
 * stream"; the WHATWG Encoding Standard's decoders): octets or a string
 * become the UTF-8 input stream the tokenizer reads, with newlines
 * normalized.
 *
 * Golden file: src/xml_encoding.c (detection and transcoding to UTF-8).
 */
#include "html_internal.h"

#include <string.h>

/* ------------------------------------------------------------ Labels */

static uint8_t to_lower_byte(uint8_t byte) {
    return byte >= 'A' && byte <= 'Z' ? (uint8_t)(byte + 0x20U) : byte;
}

static bool is_ascii_whitespace(uint8_t byte) {
    return byte == '\t' || byte == '\n' || byte == '\f' || byte == '\r' || byte == ' ';
}

/* What a label names. */
typedef struct sxml_html_encoding_lookup {
    bool is_found;
    /* SXML_HTML_ENCODING_UNSUPPORTED for a label of an encoding sxml does
       not decode. */
    uint8_t encoding_number;
} sxml_html_encoding_lookup;

/* Encoding Standard, 4.2 "get an encoding": the label without leading
   and trailing ASCII whitespace, compared in ASCII lower case. */
static sxml_html_encoding_lookup find_encoding(sxml_byte_view label) {
    const sxml_html_encoding_lookup not_found = {.is_found = false, .encoding_number = 0};
    size_t start = 0;
    size_t end = label.byte_count;
    while (start < end && is_ascii_whitespace(label.bytes[start])) {
        start += 1;
    }
    while (end > start && is_ascii_whitespace(label.bytes[end - 1])) {
        end -= 1;
    }
    uint8_t lower[64];
    const size_t lower_count = end - start;
    if (lower_count == 0 || lower_count > sizeof(lower)) {
        return not_found;
    }
    for (size_t byte_index = 0; byte_index < lower_count; byte_index += 1) {
        lower[byte_index] = to_lower_byte(label.bytes[start + byte_index]);
    }
    const sxml_byte_view key = {.bytes = lower, .byte_count = lower_count};
    size_t low_index = 0;
    size_t high_index = sxml_html_encoding_label_count;
    while (low_index < high_index) {
        const size_t middle_index = low_index + (high_index - low_index) / 2;
        const sxml_byte_view entry = sxml_html_encoding_labels[middle_index].label;
        const size_t common = entry.byte_count < key.byte_count ? entry.byte_count : key.byte_count;
        int order = memcmp(entry.bytes, key.bytes, common);
        if (order == 0) {
            order = entry.byte_count < key.byte_count   ? -1
                    : entry.byte_count > key.byte_count ? 1
                                                        : 0;
        }
        if (order == 0) {
            return (sxml_html_encoding_lookup){
                .is_found = true,
                .encoding_number = sxml_html_encoding_labels[middle_index].encoding_number};
        }
        if (order < 0) {
            low_index = middle_index + 1;
        } else {
            high_index = middle_index;
        }
    }
    return not_found;
}

/* --------------------------------------------------------- The stream */

/* The input stream being written. */
typedef struct sxml_html_stream_writer {
    sxml_xml_buffer *stream;
    sxml_html_input_outcome *outcome;
    bool follows_carriage_return;
} sxml_html_stream_writer;

static bool is_noncharacter(uint32_t codepoint) {
    return (codepoint >= 0xFDD0 && codepoint <= 0xFDEF) || (codepoint & 0xFFFEU) == 0xFFFEU;
}

/* Appends one decoded codepoint: CR LF and CR become LF (13.2.3.5);
   noncharacters and controls other than ASCII whitespace and U+0000 are
   parse errors. */
static sxml_status write_codepoint(sxml_html_stream_writer *writer, uint32_t codepoint) {
    const bool follows_carriage_return = writer->follows_carriage_return;
    writer->follows_carriage_return = codepoint == '\r';
    if (codepoint == '\r') {
        return sxml_xml_buffer_append_byte(writer->stream, '\n');
    }
    if (codepoint == '\n' && follows_carriage_return) {
        return SXML_STATUS_OK;
    }
    if (is_noncharacter(codepoint) ||
        (codepoint != 0 && codepoint < 0x20 && !is_ascii_whitespace((uint8_t)codepoint)) ||
        (codepoint >= 0x7F && codepoint <= 0x9F)) {
        writer->outcome->has_parse_error = true;
    }
    return sxml_xml_buffer_append_codepoint(writer->stream, codepoint);
}

/* A decoding error: U+FFFD, and an error for fail-on-error (F&O 4.0,
   fn:parse-html: FODC0011 when the octets cannot be decoded). */
static sxml_status write_decoding_error(sxml_html_stream_writer *writer) {
    writer->outcome->has_parse_error = true;
    return write_codepoint(writer, 0xFFFD);
}

/* Encoding Standard, 8.1.1 "UTF-8 decoder": each maximal invalid subpart
   becomes one U+FFFD. */
static sxml_status decode_utf8(sxml_html_stream_writer *writer, sxml_byte_view input) {
    const uint8_t *bytes = input.bytes;
    size_t offset = 0;
    while (offset < input.byte_count) {
        const uint8_t lead = bytes[offset];
        sxml_status status = SXML_STATUS_OK;
        if (lead < 0x80) {
            /* A run of ASCII without CR goes in one append. */
            size_t run_end = offset;
            while (run_end < input.byte_count && bytes[run_end] < 0x80 && bytes[run_end] != '\r' &&
                   !(bytes[run_end] < 0x20 && bytes[run_end] != 0 &&
                     !is_ascii_whitespace(bytes[run_end])) &&
                   bytes[run_end] != 0x7F) {
                run_end += 1;
            }
            if (run_end > offset) {
                if (writer->follows_carriage_return && bytes[offset] == '\n') {
                    offset += 1;
                }
                writer->follows_carriage_return = false;
                status = sxml_xml_buffer_append(
                    writer->stream,
                    (sxml_byte_view){.bytes = bytes + offset, .byte_count = run_end - offset});
                offset = run_end;
            } else {
                status = write_codepoint(writer, lead);
                offset += 1;
            }
            if (status != SXML_STATUS_OK) {
                return status;
            }
            continue;
        }
        size_t needed = 0;
        uint32_t codepoint = 0;
        uint8_t lower_boundary = 0x80;
        uint8_t upper_boundary = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            needed = 1;
            codepoint = lead & 0x1FU;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            lower_boundary = lead == 0xE0 ? 0xA0 : 0x80;
            upper_boundary = lead == 0xED ? 0x9F : 0xBF;
            needed = 2;
            codepoint = lead & 0xFU;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            lower_boundary = lead == 0xF0 ? 0x90 : 0x80;
            upper_boundary = lead == 0xF4 ? 0x8F : 0xBF;
            needed = 3;
            codepoint = lead & 0x7U;
        } else {
            status = write_decoding_error(writer);
            offset += 1;
            if (status != SXML_STATUS_OK) {
                return status;
            }
            continue;
        }
        size_t seen = 0;
        offset += 1;
        bool is_valid = true;
        while (seen < needed) {
            if (offset >= input.byte_count || bytes[offset] < lower_boundary ||
                bytes[offset] > upper_boundary) {
                is_valid = false;
                break;
            }
            codepoint = (codepoint << 6U) | (bytes[offset] & 0x3FU);
            lower_boundary = 0x80;
            upper_boundary = 0xBF;
            seen += 1;
            offset += 1;
        }
        status = is_valid ? write_codepoint(writer, codepoint) : write_decoding_error(writer);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/* Encoding Standard, 14.2 "shared UTF-16 decoder". */
static sxml_status decode_utf16(sxml_html_stream_writer *writer, sxml_byte_view input,
                                uint8_t encoding_number) {
    const bool is_big_endian = encoding_number == SXML_HTML_ENCODING_UTF_16BE;
    uint32_t lead_surrogate = 0;
    size_t offset = 0;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && offset + 1 < input.byte_count) {
        const uint32_t first = input.bytes[offset];
        const uint32_t second = input.bytes[offset + 1];
        const uint32_t unit = is_big_endian ? (first << 8U) | second : (second << 8U) | first;
        offset += 2;
        if (lead_surrogate != 0) {
            const uint32_t lead = lead_surrogate;
            lead_surrogate = 0;
            if (unit >= 0xDC00 && unit <= 0xDFFF) {
                status = write_codepoint(writer, 0x10000U + ((lead - 0xD800U) << 10U) +
                                                     (unit - 0xDC00U));
                continue;
            }
            status = write_decoding_error(writer);
            if (status != SXML_STATUS_OK) {
                break;
            }
        }
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            lead_surrogate = unit;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            status = write_decoding_error(writer);
        } else {
            status = write_codepoint(writer, unit);
        }
    }
    if (status == SXML_STATUS_OK && (lead_surrogate != 0 || offset < input.byte_count)) {
        status = write_decoding_error(writer);
    }
    return status;
}

static sxml_status decode_single_byte(sxml_html_stream_writer *writer, sxml_byte_view input,
                                      uint8_t encoding_number) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t offset = 0; status == SXML_STATUS_OK && offset < input.byte_count; offset += 1) {
        const uint8_t byte = input.bytes[offset];
        if (byte < 0x80) {
            status = write_codepoint(writer, byte);
        } else if (encoding_number == SXML_HTML_ENCODING_X_USER_DEFINED) {
            /* Encoding Standard, 14.5.1. */
            status = write_codepoint(writer, 0xF780U + byte - 0x80U);
        } else {
            const uint16_t codepoint =
                sxml_html_single_byte_codepoints[encoding_number - SXML_HTML_ENCODING_FIRST_SINGLE_BYTE]
                                                [byte - 0x80U];
            status = codepoint == 0 ? write_decoding_error(writer) : write_codepoint(writer, codepoint);
        }
    }
    return status;
}

/* True when every byte sequence of input is well-formed UTF-8. */
static bool is_well_formed_utf8(sxml_byte_view input) {
    size_t offset = 0;
    while (offset < input.byte_count) {
        const uint8_t lead = input.bytes[offset];
        size_t needed = 0;
        uint8_t lower_boundary = 0x80;
        uint8_t upper_boundary = 0xBF;
        if (lead < 0x80) {
            offset += 1;
            continue;
        }
        if (lead >= 0xC2 && lead <= 0xDF) {
            needed = 1;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            lower_boundary = lead == 0xE0 ? 0xA0 : 0x80;
            upper_boundary = lead == 0xED ? 0x9F : 0xBF;
            needed = 2;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            lower_boundary = lead == 0xF0 ? 0x90 : 0x80;
            upper_boundary = lead == 0xF4 ? 0x8F : 0xBF;
            needed = 3;
        } else {
            return false;
        }
        offset += 1;
        for (size_t continuation_index = 0; continuation_index < needed;
             continuation_index += 1) {
            if (offset >= input.byte_count || input.bytes[offset] < lower_boundary ||
                input.bytes[offset] > upper_boundary) {
                return false;
            }
            lower_boundary = 0x80;
            upper_boundary = 0xBF;
            offset += 1;
        }
    }
    return true;
}

/* 13.2.3.2 "Determining the character encoding", for octets without a
   byte order mark and without an override. */
static uint8_t determine_encoding(sxml_byte_view input) {
    /* Step 9 lets the user agent "attempt to autodetect the character
       encoding": octets that are well-formed UTF-8 are taken as UTF-8.
       Otherwise step 10's default for an unknown locale, windows-1252. */
    if (is_well_formed_utf8(input)) {
        return SXML_HTML_ENCODING_UTF_8;
    }
    return find_encoding(SXML_BYTE_VIEW_FROM_LITERAL("windows-1252")).encoding_number;
}

sxml_status sxml_html_prepare_input(const sxml_html_input_request *request,
                                    sxml_xml_buffer *stream, sxml_html_input_outcome *outcome) {
    *outcome = (sxml_html_input_outcome){.failure = SXML_HTML_INPUT_FAILURE_NONE,
                                         .has_parse_error = false};
    sxml_html_stream_writer writer = {
        .stream = stream, .outcome = outcome, .follows_carriage_return = false};
    sxml_byte_view input = request->input;
    const uint8_t *bytes = input.bytes;
    if (request->is_string) {
        /* F&O 4.0, fn:parse-html: a leading U+FEFF is stripped, and the
           string is UTF-8 with a known definite encoding (13.2.3.1). */
        if (input.byte_count >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
            input.bytes += 3;
            input.byte_count -= 3;
        }
        return decode_utf8(&writer, input);
    }
    /* 13.2.3.2, step 1: BOM sniffing (Encoding Standard, 6). */
    uint8_t encoding_number = SXML_HTML_ENCODING_UTF_8;
    size_t skipped = 0;
    if (input.byte_count >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        skipped = 3;
    } else if (input.byte_count >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        encoding_number = SXML_HTML_ENCODING_UTF_16BE;
        skipped = 2;
    } else if (input.byte_count >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        encoding_number = SXML_HTML_ENCODING_UTF_16LE;
        skipped = 2;
    } else if (request->encoding_name.byte_count > 0) {
        /* Step 2: the encoding option overrides the document's encoding. */
        const sxml_html_encoding_lookup lookup = find_encoding(request->encoding_name);
        encoding_number = lookup.encoding_number;
        if (!lookup.is_found || encoding_number == SXML_HTML_ENCODING_UNSUPPORTED) {
            outcome->failure = SXML_HTML_INPUT_FAILURE_UNKNOWN_ENCODING;
            return SXML_STATUS_OK;
        }
    } else {
        encoding_number = determine_encoding(input);
    }
    if (skipped > 0) {
        input.bytes += skipped;
        input.byte_count -= skipped;
    }
    switch (encoding_number) {
    case SXML_HTML_ENCODING_UTF_8:
        return decode_utf8(&writer, input);
    case SXML_HTML_ENCODING_UTF_16BE:
        return decode_utf16(&writer, input, SXML_HTML_ENCODING_UTF_16BE);
    case SXML_HTML_ENCODING_UTF_16LE:
        return decode_utf16(&writer, input, SXML_HTML_ENCODING_UTF_16LE);
    case SXML_HTML_ENCODING_REPLACEMENT:
        /* Encoding Standard, 14.1.1: one error for any input. */
        return input.byte_count > 0 ? write_decoding_error(&writer) : SXML_STATUS_OK;
    default:
        return decode_single_byte(&writer, input, encoding_number);
    }
}
