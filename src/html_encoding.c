/*
 * The HTML input byte stream (src/html_internal.h; HTML Living Standard,
 * 13.2.3 "The input byte stream", 13.2.3.2 "Determining the character
 * encoding" with its prescan, and 13.2.3.5 "Preprocessing the input
 * stream"; the WHATWG Encoding Standard's decoders, every one of its
 * encodings): octets or a string become the UTF-8 input stream the
 * tokenizer reads, with newlines normalized.
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

sxml_html_encoding_lookup sxml_html_get_encoding(sxml_byte_view label) {
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


/* ------------------------------------------------ Multi-byte decoders */

/*
 * The decoders of the Encoding Standard's legacy multi-byte encodings
 * (10 to 13), each one loop over the bytes. "Restore bytes to ioQueue"
 * gives back the bytes read last, so it moves the offset back; the end of
 * the input is the standard's end-of-queue.
 */

/* The code point of pointer in a pointer-indexed table; 0 for none. */
typedef struct sxml_html_index_view {
    const uint16_t *codepoints;
    size_t codepoint_count;
} sxml_html_index_view;

static uint32_t index_codepoint(sxml_html_index_view index, size_t pointer) {
    return pointer < index.codepoint_count ? index.codepoints[pointer] : 0;
}

static const sxml_html_index_view jis0208_index = {sxml_html_index_jis0208,
                                                   SXML_HTML_INDEX_JIS0208_COUNT};
static const sxml_html_index_view jis0212_index = {sxml_html_index_jis0212,
                                                   SXML_HTML_INDEX_JIS0212_COUNT};
static const sxml_html_index_view euc_kr_index = {sxml_html_index_euc_kr,
                                                  SXML_HTML_INDEX_EUC_KR_COUNT};
static const sxml_html_index_view gb18030_index = {sxml_html_index_gb18030,
                                                   SXML_HTML_INDEX_GB18030_COUNT};

/* Encoding Standard, 5, index Big5: plane 2 code points are listed apart. */
static uint32_t big5_codepoint(size_t pointer) {
    if (pointer < SXML_HTML_INDEX_BIG5_FIRST_POINTER ||
        pointer - SXML_HTML_INDEX_BIG5_FIRST_POINTER >= SXML_HTML_INDEX_BIG5_COUNT) {
        return 0;
    }
    const uint32_t codepoint = sxml_html_index_big5[pointer - SXML_HTML_INDEX_BIG5_FIRST_POINTER];
    if (codepoint == 0) {
        return 0;
    }
    size_t low_index = 0;
    size_t high_index = SXML_HTML_BIG5_PLANE_2_COUNT;
    while (low_index < high_index) {
        const size_t middle_index = low_index + (high_index - low_index) / 2;
        const size_t middle = sxml_html_big5_plane_2_pointers[middle_index];
        if (middle == pointer) {
            return 0x20000U + codepoint;
        }
        if (middle < pointer) {
            low_index = middle_index + 1;
        } else {
            high_index = middle_index;
        }
    }
    return codepoint;
}

/* Encoding Standard, 5, "index gb18030 ranges code point"; UINT32_MAX for
   null. */
static uint32_t gb18030_ranges_codepoint(uint32_t pointer) {
    if ((pointer > 39419 && pointer < 189000) || pointer > 1237575) {
        return UINT32_MAX;
    }
    if (pointer == 7457) {
        return 0xE7C7;
    }
    size_t low_index = 0;
    size_t high_index = SXML_HTML_GB18030_RANGE_COUNT;
    while (high_index - low_index > 1) {
        const size_t middle_index = low_index + (high_index - low_index) / 2;
        if (sxml_html_gb18030_range_pointers[middle_index] <= pointer) {
            low_index = middle_index;
        } else {
            high_index = middle_index;
        }
    }
    return sxml_html_gb18030_range_codepoints[low_index] + pointer -
           sxml_html_gb18030_range_pointers[low_index];
}

/* Writes codepoint, or a decoding error for 0 (none). */
static sxml_status write_found_codepoint(sxml_html_stream_writer *writer, uint32_t codepoint) {
    return codepoint == 0 ? write_decoding_error(writer) : write_codepoint(writer, codepoint);
}

static bool is_in_range(uint8_t byte, sxml_byte_range range) {
    return byte >= range.byte_offset && byte < range.byte_offset + range.byte_count;
}

#define SXML_HTML_BYTES(first, last) ((sxml_byte_range){.byte_offset = (first), .byte_count = (size_t)(last) - (first) + 1})

/* Encoding Standard, 10.2.1 "gb18030 decoder" (also GBK's, 10.1.1). */
static sxml_status decode_gb18030(sxml_html_stream_writer *writer, sxml_byte_view input) {
    uint8_t first = 0;
    uint8_t second = 0;
    uint8_t third = 0;
    size_t offset = 0;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK) {
        if (offset >= input.byte_count) {
            if (first != 0 || second != 0 || third != 0) {
                status = write_decoding_error(writer);
            }
            break;
        }
        const uint8_t byte = input.bytes[offset];
        offset += 1;
        if (third != 0) {
            if (!is_in_range(byte, SXML_HTML_BYTES(0x30, 0x39))) {
                /* Restore second, third and byte. */
                offset -= 3;
                first = 0;
                second = 0;
                third = 0;
                status = write_decoding_error(writer);
                continue;
            }
            const uint32_t pointer = ((uint32_t)first - 0x81U) * (10U * 126U * 10U) +
                                     ((uint32_t)second - 0x30U) * (10U * 126U) +
                                     ((uint32_t)third - 0x81U) * 10U + byte - 0x30U;
            first = 0;
            second = 0;
            third = 0;
            const uint32_t codepoint = gb18030_ranges_codepoint(pointer);
            status = codepoint == UINT32_MAX ? write_decoding_error(writer)
                                             : write_codepoint(writer, codepoint);
            continue;
        }
        if (second != 0) {
            if (is_in_range(byte, SXML_HTML_BYTES(0x81, 0xFE))) {
                third = byte;
                continue;
            }
            /* Restore second and byte. */
            offset -= 2;
            first = 0;
            second = 0;
            status = write_decoding_error(writer);
            continue;
        }
        if (first != 0) {
            if (is_in_range(byte, SXML_HTML_BYTES(0x30, 0x39))) {
                second = byte;
                continue;
            }
            const uint8_t leading = first;
            first = 0;
            const uint32_t byte_offset = byte < 0x7F ? 0x40U : 0x41U;
            uint32_t codepoint = 0;
            if (is_in_range(byte, SXML_HTML_BYTES(0x40, 0x7E)) ||
                is_in_range(byte, SXML_HTML_BYTES(0x80, 0xFE))) {
                codepoint = index_codepoint(gb18030_index, ((size_t)leading - 0x81U) * 190U +
                                                               (byte - byte_offset));
            }
            if (codepoint == 0 && byte < 0x80) {
                offset -= 1;
            }
            status = write_found_codepoint(writer, codepoint);
            continue;
        }
        if (byte < 0x80) {
            status = write_codepoint(writer, byte);
        } else if (byte == 0x80) {
            status = write_codepoint(writer, 0x20AC);
        } else if (byte != 0xFF) {
            first = byte;
        } else {
            status = write_decoding_error(writer);
        }
    }
    return status;
}

/* Encoding Standard, 11.1.1 "Big5 decoder". */
static sxml_status decode_big5(sxml_html_stream_writer *writer, sxml_byte_view input) {
    uint8_t leading = 0;
    size_t offset = 0;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK) {
        if (offset >= input.byte_count) {
            if (leading != 0) {
                status = write_decoding_error(writer);
            }
            break;
        }
        const uint8_t byte = input.bytes[offset];
        offset += 1;
        if (leading != 0) {
            const uint32_t lead = leading;
            leading = 0;
            const uint32_t byte_offset = byte < 0x7F ? 0x40U : 0x62U;
            size_t pointer = SIZE_MAX;
            if (is_in_range(byte, SXML_HTML_BYTES(0x40, 0x7E)) ||
                is_in_range(byte, SXML_HTML_BYTES(0xA1, 0xFE))) {
                pointer = (lead - 0x81U) * 157U + (byte - byte_offset);
            }
            /* The four pointers that decode to two code points. */
            if (pointer == 1133 || pointer == 1135 || pointer == 1164 || pointer == 1166) {
                status = write_codepoint(writer, pointer < 1164 ? 0x00CAU : 0x00EAU);
                if (status == SXML_STATUS_OK) {
                    status = write_codepoint(writer,
                                             pointer == 1133 || pointer == 1164 ? 0x0304U : 0x030CU);
                }
                continue;
            }
            const uint32_t codepoint = pointer == SIZE_MAX ? 0 : big5_codepoint(pointer);
            if (codepoint == 0 && byte < 0x80) {
                offset -= 1;
            }
            status = write_found_codepoint(writer, codepoint);
            continue;
        }
        if (byte < 0x80) {
            status = write_codepoint(writer, byte);
        } else if (is_in_range(byte, SXML_HTML_BYTES(0x81, 0xFE))) {
            leading = byte;
        } else {
            status = write_decoding_error(writer);
        }
    }
    return status;
}

/* Encoding Standard, 12.1.1 "EUC-JP decoder". */
static sxml_status decode_euc_jp(sxml_html_stream_writer *writer, sxml_byte_view input) {
    uint8_t leading = 0;
    bool uses_jis0212 = false;
    size_t offset = 0;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK) {
        if (offset >= input.byte_count) {
            if (leading != 0) {
                status = write_decoding_error(writer);
            }
            break;
        }
        const uint8_t byte = input.bytes[offset];
        offset += 1;
        if (leading == 0x8E && is_in_range(byte, SXML_HTML_BYTES(0xA1, 0xDF))) {
            leading = 0;
            status = write_codepoint(writer, 0xFF61U - 0xA1U + byte);
            continue;
        }
        if (leading == 0x8F && is_in_range(byte, SXML_HTML_BYTES(0xA1, 0xFE))) {
            uses_jis0212 = true;
            leading = byte;
            continue;
        }
        if (leading != 0) {
            const uint8_t lead = leading;
            leading = 0;
            uint32_t codepoint = 0;
            if (is_in_range(lead, SXML_HTML_BYTES(0xA1, 0xFE)) &&
                is_in_range(byte, SXML_HTML_BYTES(0xA1, 0xFE))) {
                codepoint = index_codepoint(uses_jis0212 ? jis0212_index : jis0208_index,
                                            ((size_t)lead - 0xA1U) * 94U + byte - 0xA1U);
            }
            uses_jis0212 = false;
            if (codepoint == 0 && byte < 0x80) {
                offset -= 1;
            }
            status = write_found_codepoint(writer, codepoint);
            continue;
        }
        if (byte < 0x80) {
            status = write_codepoint(writer, byte);
        } else if (byte == 0x8E || byte == 0x8F || is_in_range(byte, SXML_HTML_BYTES(0xA1, 0xFE))) {
            leading = byte;
        } else {
            status = write_decoding_error(writer);
        }
    }
    return status;
}

/* The states of the ISO-2022-JP decoder (Encoding Standard, 12.2.1). */
typedef enum sxml_html_iso_2022_jp_state {
    SXML_HTML_ISO_2022_JP_STATE_ASCII,
    SXML_HTML_ISO_2022_JP_STATE_ROMAN,
    SXML_HTML_ISO_2022_JP_STATE_KATAKANA,
    SXML_HTML_ISO_2022_JP_STATE_LEADING_BYTE,
    SXML_HTML_ISO_2022_JP_STATE_TRAILING_BYTE,
    SXML_HTML_ISO_2022_JP_STATE_ESCAPE_START,
    SXML_HTML_ISO_2022_JP_STATE_ESCAPE,
} sxml_html_iso_2022_jp_state;

/* The ISO-2022-JP decoder between bytes. */
typedef struct sxml_html_iso_2022_jp_decoder {
    sxml_html_iso_2022_jp_state state;
    sxml_html_iso_2022_jp_state output_state;
    uint8_t leading;
    bool has_output;
} sxml_html_iso_2022_jp_decoder;

/* One byte, not end-of-queue, in the ASCII, Roman, katakana or leading
   byte state; writes what it decodes. */
static sxml_status decode_iso_2022_jp_single(sxml_html_stream_writer *writer,
                                             sxml_html_iso_2022_jp_decoder *decoder,
                                             uint8_t byte) {
    if (byte == 0x1B) {
        decoder->state = SXML_HTML_ISO_2022_JP_STATE_ESCAPE_START;
        return SXML_STATUS_OK;
    }
    decoder->has_output = false;
    switch (decoder->state) {
    case SXML_HTML_ISO_2022_JP_STATE_ASCII:
        return byte < 0x80 && byte != 0x0E && byte != 0x0F ? write_codepoint(writer, byte)
                                                           : write_decoding_error(writer);
    case SXML_HTML_ISO_2022_JP_STATE_ROMAN:
        if (byte == 0x5C) {
            return write_codepoint(writer, 0x00A5);
        }
        if (byte == 0x7E) {
            return write_codepoint(writer, 0x203E);
        }
        return byte < 0x80 && byte != 0x0E && byte != 0x0F ? write_codepoint(writer, byte)
                                                           : write_decoding_error(writer);
    case SXML_HTML_ISO_2022_JP_STATE_KATAKANA:
        return is_in_range(byte, SXML_HTML_BYTES(0x21, 0x5F))
                   ? write_codepoint(writer, 0xFF61U - 0x21U + byte)
                   : write_decoding_error(writer);
    case SXML_HTML_ISO_2022_JP_STATE_LEADING_BYTE:
        if (is_in_range(byte, SXML_HTML_BYTES(0x21, 0x7E))) {
            decoder->leading = byte;
            decoder->state = SXML_HTML_ISO_2022_JP_STATE_TRAILING_BYTE;
            return SXML_STATUS_OK;
        }
        return write_decoding_error(writer);
    case SXML_HTML_ISO_2022_JP_STATE_TRAILING_BYTE:
    case SXML_HTML_ISO_2022_JP_STATE_ESCAPE_START:
    case SXML_HTML_ISO_2022_JP_STATE_ESCAPE:
        break;
    }
    return SXML_STATUS_OK;
}

/* Encoding Standard, 12.2.1 "ISO-2022-JP decoder". */
static sxml_status decode_iso_2022_jp(sxml_html_stream_writer *writer, sxml_byte_view input) {
    sxml_html_iso_2022_jp_decoder decoder = {.state = SXML_HTML_ISO_2022_JP_STATE_ASCII,
                                             .output_state = SXML_HTML_ISO_2022_JP_STATE_ASCII,
                                             .leading = 0,
                                             .has_output = false};
    size_t offset = 0;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK) {
        const bool is_at_end = offset >= input.byte_count;
        const uint8_t byte = is_at_end ? 0 : input.bytes[offset];
        if (!is_at_end) {
            offset += 1;
        }
        switch (decoder.state) {
        case SXML_HTML_ISO_2022_JP_STATE_ASCII:
        case SXML_HTML_ISO_2022_JP_STATE_ROMAN:
        case SXML_HTML_ISO_2022_JP_STATE_KATAKANA:
        case SXML_HTML_ISO_2022_JP_STATE_LEADING_BYTE:
            if (is_at_end) {
                return SXML_STATUS_OK;
            }
            status = decode_iso_2022_jp_single(writer, &decoder, byte);
            break;
        case SXML_HTML_ISO_2022_JP_STATE_TRAILING_BYTE:
            if (byte == 0x1B && !is_at_end) {
                decoder.state = SXML_HTML_ISO_2022_JP_STATE_ESCAPE_START;
                status = write_decoding_error(writer);
                break;
            }
            decoder.state = SXML_HTML_ISO_2022_JP_STATE_LEADING_BYTE;
            if (!is_at_end && is_in_range(byte, SXML_HTML_BYTES(0x21, 0x7E))) {
                status = write_found_codepoint(
                    writer, index_codepoint(jis0208_index,
                                            ((size_t)decoder.leading - 0x21U) * 94U + byte - 0x21U));
            } else {
                status = write_decoding_error(writer);
            }
            break;
        case SXML_HTML_ISO_2022_JP_STATE_ESCAPE_START:
            if (!is_at_end && (byte == 0x24 || byte == 0x28)) {
                decoder.leading = byte;
                decoder.state = SXML_HTML_ISO_2022_JP_STATE_ESCAPE;
                break;
            }
            if (!is_at_end) {
                offset -= 1;
            }
            decoder.has_output = false;
            decoder.state = decoder.output_state;
            status = write_decoding_error(writer);
            break;
        case SXML_HTML_ISO_2022_JP_STATE_ESCAPE: {
            const uint8_t leading = decoder.leading;
            decoder.leading = 0;
            bool is_known = true;
            sxml_html_iso_2022_jp_state state = SXML_HTML_ISO_2022_JP_STATE_ASCII;
            if (leading == 0x28 && byte == 0x4A) {
                state = SXML_HTML_ISO_2022_JP_STATE_ROMAN;
            } else if (leading == 0x28 && byte == 0x49) {
                state = SXML_HTML_ISO_2022_JP_STATE_KATAKANA;
            } else if (leading == 0x24 && (byte == 0x40 || byte == 0x42)) {
                state = SXML_HTML_ISO_2022_JP_STATE_LEADING_BYTE;
            } else {
                is_known = leading == 0x28 && byte == 0x42;
            }
            if (is_known && !is_at_end) {
                decoder.state = state;
                decoder.output_state = state;
                const bool had_output = decoder.has_output;
                decoder.has_output = true;
                if (had_output) {
                    status = write_decoding_error(writer);
                }
                break;
            }
            /* Restore leading, and byte unless at the end. */
            offset -= is_at_end ? 1 : 2;
            decoder.has_output = false;
            decoder.state = decoder.output_state;
            status = write_decoding_error(writer);
            break;
        }
        }
    }
    return status;
}

/* Encoding Standard, 12.3.1 "Shift_JIS decoder". */
static sxml_status decode_shift_jis(sxml_html_stream_writer *writer, sxml_byte_view input) {
    uint8_t leading = 0;
    size_t offset = 0;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK) {
        if (offset >= input.byte_count) {
            if (leading != 0) {
                status = write_decoding_error(writer);
            }
            break;
        }
        const uint8_t byte = input.bytes[offset];
        offset += 1;
        if (leading != 0) {
            const uint32_t lead = leading;
            leading = 0;
            const uint32_t byte_offset = byte < 0x7F ? 0x40U : 0x41U;
            const uint32_t leading_offset = lead < 0xA0 ? 0x81U : 0xC1U;
            uint32_t codepoint = 0;
            if (is_in_range(byte, SXML_HTML_BYTES(0x40, 0x7E)) ||
                is_in_range(byte, SXML_HTML_BYTES(0x80, 0xFC))) {
                const uint32_t pointer = (lead - leading_offset) * 188U + byte - byte_offset;
                /* EUDC, interoperable legacy from Windows. */
                codepoint = pointer >= 8836 && pointer <= 10715
                                ? 0xE000U - 8836U + pointer
                                : index_codepoint(jis0208_index, pointer);
            }
            if (codepoint == 0 && byte < 0x80) {
                offset -= 1;
            }
            status = write_found_codepoint(writer, codepoint);
            continue;
        }
        if (byte <= 0x80) {
            status = write_codepoint(writer, byte);
        } else if (is_in_range(byte, SXML_HTML_BYTES(0xA1, 0xDF))) {
            status = write_codepoint(writer, 0xFF61U - 0xA1U + byte);
        } else if (is_in_range(byte, SXML_HTML_BYTES(0x81, 0x9F)) ||
                   is_in_range(byte, SXML_HTML_BYTES(0xE0, 0xFC))) {
            leading = byte;
        } else {
            status = write_decoding_error(writer);
        }
    }
    return status;
}

/* Encoding Standard, 13.1.1 "EUC-KR decoder". */
static sxml_status decode_euc_kr(sxml_html_stream_writer *writer, sxml_byte_view input) {
    uint8_t leading = 0;
    size_t offset = 0;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK) {
        if (offset >= input.byte_count) {
            if (leading != 0) {
                status = write_decoding_error(writer);
            }
            break;
        }
        const uint8_t byte = input.bytes[offset];
        offset += 1;
        if (leading != 0) {
            const uint32_t lead = leading;
            leading = 0;
            uint32_t codepoint = 0;
            if (is_in_range(byte, SXML_HTML_BYTES(0x41, 0xFE))) {
                codepoint = index_codepoint(euc_kr_index, (lead - 0x81U) * 190U + (byte - 0x41U));
            }
            if (codepoint == 0 && byte < 0x80) {
                offset -= 1;
            }
            status = write_found_codepoint(writer, codepoint);
            continue;
        }
        if (byte < 0x80) {
            status = write_codepoint(writer, byte);
        } else if (is_in_range(byte, SXML_HTML_BYTES(0x81, 0xFE))) {
            leading = byte;
        } else {
            status = write_decoding_error(writer);
        }
    }
    return status;
}

/* ------------------------------------------------ Sniffing the encoding */

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

static bool is_ascii_letter(uint8_t byte) {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

/* True when text starts with prefix, prefix in ASCII lower case, compared
   in ASCII lower case. */
static bool starts_with_lower(sxml_byte_view text, sxml_byte_view prefix) {
    if (text.byte_count < prefix.byte_count) {
        return false;
    }
    for (size_t byte_index = 0; byte_index < prefix.byte_count; byte_index += 1) {
        if (to_lower_byte(text.bytes[byte_index]) != prefix.bytes[byte_index]) {
            return false;
        }
    }
    return true;
}

sxml_html_encoding_lookup sxml_html_extract_meta_encoding(sxml_byte_view content) {
    const sxml_html_encoding_lookup nothing = {.is_found = false, .encoding_number = 0};
    const sxml_byte_view word = SXML_BYTE_VIEW_FROM_LITERAL("charset");
    size_t position = 0;
    for (;;) {
        /* Loop: the first "charset" after position. */
        while (position + word.byte_count <= content.byte_count &&
               !starts_with_lower((sxml_byte_view){.bytes = content.bytes + position,
                                                   .byte_count = content.byte_count - position},
                                  word)) {
            position += 1;
        }
        if (position + word.byte_count > content.byte_count) {
            return nothing;
        }
        position += word.byte_count;
        while (position < content.byte_count && is_ascii_whitespace(content.bytes[position])) {
            position += 1;
        }
        if (position >= content.byte_count || content.bytes[position] != '=') {
            continue;
        }
        position += 1;
        while (position < content.byte_count && is_ascii_whitespace(content.bytes[position])) {
            position += 1;
        }
        if (position >= content.byte_count) {
            return nothing;
        }
        const uint8_t first = content.bytes[position];
        if (first == '"' || first == '\'') {
            size_t closing = position + 1;
            while (closing < content.byte_count && content.bytes[closing] != first) {
                closing += 1;
            }
            if (closing >= content.byte_count) {
                return nothing;
            }
            return sxml_html_get_encoding((sxml_byte_view){.bytes = content.bytes + position + 1,
                                                           .byte_count = closing - position - 1});
        }
        size_t end = position;
        while (end < content.byte_count && !is_ascii_whitespace(content.bytes[end]) &&
               content.bytes[end] != ';') {
            end += 1;
        }
        return sxml_html_get_encoding(
            (sxml_byte_view){.bytes = content.bytes + position, .byte_count = end - position});
    }
}

/* The prescan's position over its bytes (13.2.3.2). Running past the end
   aborts the prescan. */
typedef struct sxml_html_prescan {
    sxml_byte_view bytes;
    size_t position;
} sxml_html_prescan;

/* An attribute that "get an attribute" found: its name and value in ASCII
   lower case. Both fit: the prescan reads at most 1024 bytes. */
enum { SXML_HTML_PRESCAN_BYTE_COUNT = 1024 };

typedef struct sxml_html_prescan_text {
    uint8_t bytes[SXML_HTML_PRESCAN_BYTE_COUNT];
    size_t byte_count;
} sxml_html_prescan_text;

typedef struct sxml_html_prescan_attribute {
    sxml_html_prescan_text name;
    sxml_html_prescan_text value;
} sxml_html_prescan_attribute;

static bool is_prescan_space(uint8_t byte) {
    return byte == 0x09 || byte == 0x0A || byte == 0x0C || byte == 0x0D || byte == 0x20;
}

static void append_lower(sxml_html_prescan_text *text, uint8_t byte) {
    if (text->byte_count < SXML_HTML_PRESCAN_BYTE_COUNT) {
        text->bytes[text->byte_count] = to_lower_byte(byte);
        text->byte_count += 1;
    }
}

/* The steps of "get an attribute". */
typedef enum sxml_html_attribute_step {
    SXML_HTML_ATTRIBUTE_STEP_NAME,
    SXML_HTML_ATTRIBUTE_STEP_SPACES,
    SXML_HTML_ATTRIBUTE_STEP_VALUE,
    SXML_HTML_ATTRIBUTE_STEP_UNQUOTED_VALUE,
    SXML_HTML_ATTRIBUTE_STEP_DONE,
} sxml_html_attribute_step;

/* What "get an attribute" gave. */
typedef enum sxml_html_attribute_result {
    SXML_HTML_ATTRIBUTE_RESULT_FOUND,
    SXML_HTML_ATTRIBUTE_RESULT_NONE,
    /* The bytes ran out: the prescan is aborted. */
    SXML_HTML_ATTRIBUTE_RESULT_OUT_OF_BYTES,
} sxml_html_attribute_result;

/* 13.2.3.2 "get an attribute": one loop over its steps. */
static sxml_html_attribute_result get_attribute(sxml_html_prescan *prescan,
                                                sxml_html_prescan_attribute *attribute) {
    const sxml_byte_view bytes = prescan->bytes;
    attribute->name.byte_count = 0;
    attribute->value.byte_count = 0;
    while (prescan->position < bytes.byte_count &&
           (is_prescan_space(bytes.bytes[prescan->position]) ||
            bytes.bytes[prescan->position] == '/')) {
        prescan->position += 1;
    }
    if (prescan->position >= bytes.byte_count) {
        return SXML_HTML_ATTRIBUTE_RESULT_OUT_OF_BYTES;
    }
    if (bytes.bytes[prescan->position] == '>') {
        return SXML_HTML_ATTRIBUTE_RESULT_NONE;
    }
    sxml_html_attribute_step step = SXML_HTML_ATTRIBUTE_STEP_NAME;
    uint8_t quote = 0;
    while (step != SXML_HTML_ATTRIBUTE_STEP_DONE) {
        if (prescan->position >= bytes.byte_count) {
            return SXML_HTML_ATTRIBUTE_RESULT_OUT_OF_BYTES;
        }
        const uint8_t byte = bytes.bytes[prescan->position];
        switch (step) {
        case SXML_HTML_ATTRIBUTE_STEP_NAME:
            if (byte == '=' && attribute->name.byte_count > 0) {
                prescan->position += 1;
                step = SXML_HTML_ATTRIBUTE_STEP_VALUE;
            } else if (is_prescan_space(byte)) {
                step = SXML_HTML_ATTRIBUTE_STEP_SPACES;
            } else if (byte == '/' || byte == '>') {
                step = SXML_HTML_ATTRIBUTE_STEP_DONE;
            } else {
                append_lower(&attribute->name, byte);
                prescan->position += 1;
            }
            break;
        case SXML_HTML_ATTRIBUTE_STEP_SPACES:
            if (is_prescan_space(byte)) {
                prescan->position += 1;
            } else if (byte != '=') {
                step = SXML_HTML_ATTRIBUTE_STEP_DONE;
            } else {
                prescan->position += 1;
                step = SXML_HTML_ATTRIBUTE_STEP_VALUE;
            }
            break;
        case SXML_HTML_ATTRIBUTE_STEP_VALUE:
            if (quote == 0 && is_prescan_space(byte)) {
                prescan->position += 1;
            } else if (quote != 0) {
                /* The quote loop. */
                prescan->position += 1;
                if (byte == quote) {
                    step = SXML_HTML_ATTRIBUTE_STEP_DONE;
                } else {
                    append_lower(&attribute->value, byte);
                }
            } else if (byte == '"' || byte == '\'') {
                quote = byte;
                prescan->position += 1;
            } else if (byte == '>') {
                step = SXML_HTML_ATTRIBUTE_STEP_DONE;
            } else {
                append_lower(&attribute->value, byte);
                prescan->position += 1;
                step = SXML_HTML_ATTRIBUTE_STEP_UNQUOTED_VALUE;
            }
            break;
        case SXML_HTML_ATTRIBUTE_STEP_UNQUOTED_VALUE:
            if (is_prescan_space(byte) || byte == '>') {
                step = SXML_HTML_ATTRIBUTE_STEP_DONE;
            } else {
                append_lower(&attribute->value, byte);
                prescan->position += 1;
            }
            break;
        case SXML_HTML_ATTRIBUTE_STEP_DONE:
            break;
        }
    }
    return SXML_HTML_ATTRIBUTE_RESULT_FOUND;
}

/* What a meta element of the prescan declares. */
typedef enum sxml_html_meta_result {
    SXML_HTML_META_RESULT_ENCODING,
    SXML_HTML_META_RESULT_NONE,
    SXML_HTML_META_RESULT_OUT_OF_BYTES,
} sxml_html_meta_result;

static bool attribute_name_is(const sxml_html_prescan_attribute *attribute,
                              sxml_byte_view name) {
    return sxml_byte_view_equals(
        (sxml_byte_view){.bytes = attribute->name.bytes, .byte_count = attribute->name.byte_count},
        name);
}

/* The "<meta" branch of the prescan, after "<meta" and its space or
   slash. */
static sxml_html_meta_result prescan_meta(sxml_html_prescan *prescan,
                                          sxml_html_encoding_lookup *charset) {
    sxml_html_prescan_attribute attribute;
    /* The attribute list holds only the names that matter; repeating
       another name changes nothing. */
    bool has_http_equiv = false;
    bool has_content = false;
    bool has_charset = false;
    bool got_pragma = false;
    /* need pragma: null (unknown), true or false. */
    bool knows_need_pragma = false;
    bool needs_pragma = false;
    *charset = (sxml_html_encoding_lookup){.is_found = false, .encoding_number = 0};
    bool has_charset_value = false;
    for (;;) {
        const sxml_html_attribute_result result = get_attribute(prescan, &attribute);
        if (result == SXML_HTML_ATTRIBUTE_RESULT_OUT_OF_BYTES) {
            return SXML_HTML_META_RESULT_OUT_OF_BYTES;
        }
        if (result == SXML_HTML_ATTRIBUTE_RESULT_NONE) {
            break;
        }
        const sxml_byte_view value = {.bytes = attribute.value.bytes,
                                      .byte_count = attribute.value.byte_count};
        if (attribute_name_is(&attribute, SXML_BYTE_VIEW_FROM_LITERAL("http-equiv"))) {
            if (!has_http_equiv) {
                has_http_equiv = true;
                got_pragma = got_pragma || sxml_byte_view_equals(
                                               value, SXML_BYTE_VIEW_FROM_LITERAL("content-type"));
            }
        } else if (attribute_name_is(&attribute, SXML_BYTE_VIEW_FROM_LITERAL("content"))) {
            if (!has_content) {
                has_content = true;
                const sxml_html_encoding_lookup extracted = sxml_html_extract_meta_encoding(value);
                if (extracted.is_found && !has_charset_value) {
                    *charset = extracted;
                    has_charset_value = true;
                    knows_need_pragma = true;
                    needs_pragma = true;
                }
            }
        } else if (attribute_name_is(&attribute, SXML_BYTE_VIEW_FROM_LITERAL("charset"))) {
            if (!has_charset) {
                has_charset = true;
                *charset = sxml_html_get_encoding(value);
                has_charset_value = true;
                knows_need_pragma = true;
                needs_pragma = false;
            }
        }
    }
    /* Processing. */
    if (!knows_need_pragma || (needs_pragma && !got_pragma) || !charset->is_found) {
        return SXML_HTML_META_RESULT_NONE;
    }
    if (charset->encoding_number == SXML_HTML_ENCODING_UTF_16BE ||
        charset->encoding_number == SXML_HTML_ENCODING_UTF_16LE) {
        charset->encoding_number = SXML_HTML_ENCODING_UTF_8;
    } else if (charset->encoding_number == SXML_HTML_ENCODING_X_USER_DEFINED) {
        charset->encoding_number = SXML_HTML_ENCODING_WINDOWS_1252;
    }
    return SXML_HTML_META_RESULT_ENCODING;
}

/* True when the bytes from position start with prefix (lower case),
   compared in ASCII lower case. */
static bool prescan_sees(const sxml_html_prescan *prescan, sxml_byte_view prefix) {
    return starts_with_lower((sxml_byte_view){.bytes = prescan->bytes.bytes + prescan->position,
                                              .byte_count =
                                                  prescan->bytes.byte_count - prescan->position},
                             prefix);
}

/* 13.2.3.2 "get an XML encoding". */
static sxml_html_encoding_lookup get_xml_encoding(sxml_byte_view bytes) {
    const sxml_html_encoding_lookup failure = {.is_found = false, .encoding_number = 0};
    if (bytes.byte_count < 5 || memcmp(bytes.bytes, "<?xml", 5) != 0) {
        return failure;
    }
    size_t declaration_end = 5;
    while (declaration_end < bytes.byte_count && bytes.bytes[declaration_end] != '>') {
        declaration_end += 1;
    }
    if (declaration_end >= bytes.byte_count) {
        return failure;
    }
    size_t position = 5;
    while (position + 8 <= declaration_end && memcmp(bytes.bytes + position, "encoding", 8) != 0) {
        position += 1;
    }
    if (position + 8 > declaration_end) {
        return failure;
    }
    position += 8;
    while (position < bytes.byte_count && bytes.bytes[position] <= 0x20) {
        position += 1;
    }
    if (position >= bytes.byte_count || bytes.bytes[position] != '=') {
        return failure;
    }
    position += 1;
    while (position < bytes.byte_count && bytes.bytes[position] <= 0x20) {
        position += 1;
    }
    if (position >= bytes.byte_count ||
        (bytes.bytes[position] != '"' && bytes.bytes[position] != '\'')) {
        return failure;
    }
    const uint8_t quote = bytes.bytes[position];
    position += 1;
    size_t end = position;
    while (end < bytes.byte_count && bytes.bytes[end] != quote) {
        if (bytes.bytes[end] <= 0x20) {
            return failure;
        }
        end += 1;
    }
    if (end >= bytes.byte_count) {
        return failure;
    }
    sxml_html_encoding_lookup encoding = sxml_html_get_encoding(
        (sxml_byte_view){.bytes = bytes.bytes + position, .byte_count = end - position});
    if (encoding.is_found && (encoding.encoding_number == SXML_HTML_ENCODING_UTF_16BE ||
                              encoding.encoding_number == SXML_HTML_ENCODING_UTF_16LE)) {
        encoding.encoding_number = SXML_HTML_ENCODING_UTF_8;
    }
    return encoding;
}

/* Advances to the next byte equal to target, or past the end. */
static void advance_to(sxml_html_prescan *prescan, uint8_t target) {
    while (prescan->position < prescan->bytes.byte_count &&
           prescan->bytes.bytes[prescan->position] != target) {
        prescan->position += 1;
    }
}

sxml_html_encoding_lookup sxml_html_prescan_encoding(sxml_byte_view input) {
    /* The end condition: the first 1024 bytes, as 13.2.3.2 encourages. */
    const sxml_byte_view bytes = {.bytes = input.bytes,
                                  .byte_count = input.byte_count < SXML_HTML_PRESCAN_BYTE_COUNT
                                                    ? input.byte_count
                                                    : SXML_HTML_PRESCAN_BYTE_COUNT};
    /* Prescan for UTF-16 XML declarations. */
    if (bytes.byte_count >= 6 && memcmp(bytes.bytes, "<\0?\0x\0", 6) == 0) {
        return (sxml_html_encoding_lookup){.is_found = true,
                                           .encoding_number = SXML_HTML_ENCODING_UTF_16LE};
    }
    if (bytes.byte_count >= 6 && memcmp(bytes.bytes, "\0<\0?\0x", 6) == 0) {
        return (sxml_html_encoding_lookup){.is_found = true,
                                           .encoding_number = SXML_HTML_ENCODING_UTF_16BE};
    }
    sxml_html_prescan prescan = {.bytes = bytes, .position = 0};
    while (prescan.position < bytes.byte_count) {
        const uint8_t next = prescan.position + 1 < bytes.byte_count
                                 ? bytes.bytes[prescan.position + 1]
                                 : 0;
        if (prescan_sees(&prescan, SXML_BYTE_VIEW_FROM_LITERAL("<!--"))) {
            /* The first '>' after two '-', which may be those of "<!--". */
            prescan.position += 2;
            while (prescan.position + 2 < bytes.byte_count &&
                   !(bytes.bytes[prescan.position] == '-' &&
                     bytes.bytes[prescan.position + 1] == '-' &&
                     bytes.bytes[prescan.position + 2] == '>')) {
                prescan.position += 1;
            }
            prescan.position += 2;
        } else if (prescan_sees(&prescan, SXML_BYTE_VIEW_FROM_LITERAL("<meta")) &&
                   prescan.position + 5 < bytes.byte_count &&
                   (is_prescan_space(bytes.bytes[prescan.position + 5]) ||
                    bytes.bytes[prescan.position + 5] == '/')) {
            prescan.position += 5;
            sxml_html_encoding_lookup charset;
            const sxml_html_meta_result result = prescan_meta(&prescan, &charset);
            if (result == SXML_HTML_META_RESULT_ENCODING) {
                return charset;
            }
            if (result == SXML_HTML_META_RESULT_OUT_OF_BYTES) {
                break;
            }
        } else if (bytes.bytes[prescan.position] == '<' &&
                   (is_ascii_letter(next) ||
                    (next == '/' && prescan.position + 2 < bytes.byte_count &&
                     is_ascii_letter(bytes.bytes[prescan.position + 2])))) {
            while (prescan.position < bytes.byte_count &&
                   !is_prescan_space(bytes.bytes[prescan.position]) &&
                   bytes.bytes[prescan.position] != '>') {
                prescan.position += 1;
            }
            sxml_html_prescan_attribute attribute;
            sxml_html_attribute_result result = SXML_HTML_ATTRIBUTE_RESULT_FOUND;
            while (result == SXML_HTML_ATTRIBUTE_RESULT_FOUND) {
                result = get_attribute(&prescan, &attribute);
            }
            if (result == SXML_HTML_ATTRIBUTE_RESULT_OUT_OF_BYTES) {
                break;
            }
        } else if (bytes.bytes[prescan.position] == '<' &&
                   (next == '!' || next == '/' || next == '?')) {
            advance_to(&prescan, '>');
        }
        /* Next byte. */
        prescan.position += 1;
    }
    return get_xml_encoding(bytes);
}

/* 13.2.3.2 "Determining the character encoding", for octets without a
   byte order mark and without an override: the prescan, then step 9,
   then step 10. The confidence is tentative. */
static uint8_t determine_encoding(sxml_byte_view input) {
    const sxml_html_encoding_lookup prescanned = sxml_html_prescan_encoding(input);
    if (prescanned.is_found) {
        return prescanned.encoding_number;
    }
    /* Step 9 lets the user agent "attempt to autodetect the character
       encoding": octets that are well-formed UTF-8 are taken as UTF-8.
       Otherwise step 10's default for an unknown locale, windows-1252. */
    if (is_well_formed_utf8(input)) {
        return SXML_HTML_ENCODING_UTF_8;
    }
    return SXML_HTML_ENCODING_WINDOWS_1252;
}

/* Decodes input, which has no byte order mark, in encoding_number. */
static sxml_status decode_in(sxml_html_stream_writer *writer, sxml_byte_view input,
                             uint8_t encoding_number) {
    switch (encoding_number) {
    case SXML_HTML_ENCODING_UTF_8:
        return decode_utf8(writer, input);
    case SXML_HTML_ENCODING_UTF_16BE:
    case SXML_HTML_ENCODING_UTF_16LE:
        return decode_utf16(writer, input, encoding_number);
    case SXML_HTML_ENCODING_REPLACEMENT:
        /* Encoding Standard, 14.1.1: one error for any input. */
        return input.byte_count > 0 ? write_decoding_error(writer) : SXML_STATUS_OK;
    case SXML_HTML_ENCODING_GBK:
    case SXML_HTML_ENCODING_GB18030:
        return decode_gb18030(writer, input);
    case SXML_HTML_ENCODING_BIG5:
        return decode_big5(writer, input);
    case SXML_HTML_ENCODING_EUC_JP:
        return decode_euc_jp(writer, input);
    case SXML_HTML_ENCODING_ISO_2022_JP:
        return decode_iso_2022_jp(writer, input);
    case SXML_HTML_ENCODING_SHIFT_JIS:
        return decode_shift_jis(writer, input);
    case SXML_HTML_ENCODING_EUC_KR:
        return decode_euc_kr(writer, input);
    default:
        return decode_single_byte(writer, input, encoding_number);
    }
}

sxml_status sxml_html_prepare_input(const sxml_html_input_request *request,
                                    sxml_xml_buffer *stream, sxml_html_input_outcome *outcome) {
    *outcome = (sxml_html_input_outcome){.failure = SXML_HTML_INPUT_FAILURE_NONE,
                                         .encoding_number = SXML_HTML_ENCODING_UTF_8,
                                         .is_encoding_tentative = false,
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
    /* 13.2.3.2, step 1: BOM sniffing (Encoding Standard, 6). The decode
       algorithm skips the byte order mark whatever the encoding. */
    uint8_t encoding_number = SXML_HTML_ENCODING_UTF_8;
    size_t skipped = 0;
    bool has_byte_order_mark = true;
    if (input.byte_count >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        skipped = 3;
    } else if (input.byte_count >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        encoding_number = SXML_HTML_ENCODING_UTF_16BE;
        skipped = 2;
    } else if (input.byte_count >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        encoding_number = SXML_HTML_ENCODING_UTF_16LE;
        skipped = 2;
    } else {
        has_byte_order_mark = false;
    }
    if (has_byte_order_mark) {
        /* Confidence certain. */
    } else if (request->has_known_encoding) {
        encoding_number = request->known_encoding_number;
    } else if (request->encoding_name.byte_count > 0) {
        /* Step 2: the encoding option overrides the document's encoding. */
        const sxml_html_encoding_lookup lookup = sxml_html_get_encoding(request->encoding_name);
        if (!lookup.is_found) {
            outcome->failure = SXML_HTML_INPUT_FAILURE_UNKNOWN_ENCODING;
            return SXML_STATUS_OK;
        }
        encoding_number = lookup.encoding_number;
    } else {
        encoding_number = determine_encoding(input);
        outcome->is_encoding_tentative = true;
    }
    outcome->encoding_number = encoding_number;
    input.bytes += skipped;
    input.byte_count -= skipped;
    return decode_in(&writer, input, encoding_number);
}
