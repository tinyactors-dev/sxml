/*
 * The text window of the XML parser (src/xml_internal.h, glossary
 * `text_window`, D233): the part of a document's decoded UTF-8 text the
 * parser holds, decoded from the input a window at a time.
 *
 * Golden file: src/xml_segment.c (a part of the parser over a decoded
 * text, offsets in the whole text), with the decoders of
 * src/xml_encoding.c.
 *
 * A whole input in UTF-8 or US-ASCII is its own text: it is checked at
 * once and never copied (unless XML 1.1 line ends must be translated).
 * Every other input, and every input read through a byte reader, is
 * decoded into a buffer a part at a time: XML 1.0, 4.3.3 (the encodings
 * sxml reads), Appendix F (detection), 2.2 ([2] Char: no U+0000), and
 * XML 1.1, 2.2 and 2.11 (restricted characters, NEL and LINE SEPARATOR).
 * A part ends at a character boundary of the input: the bytes of a
 * character the read has not finished (a UTF-8 sequence, a UTF-16 unit
 * or surrogate pair) wait for the next part, and so does a CR of XML 1.1,
 * whose meaning depends on the character after it.
 */
#include "xml_internal.h"

#include <string.h>

/* The input bytes decoded at least at each extension (and read at least
   at each read). */
enum { SXML_XML_WINDOW_BYTE_COUNT = 32768 };

/* What a byte reader is lent when the raw buffer has no room. */
enum { SXML_XML_READ_BYTE_COUNT = 16384 };

static bool sxml_xml_window_reads(const sxml_xml_text_window *window) {
    return window->configuration.byte_reader.operations != NULL;
}

static sxml_status sxml_xml_window_fail(sxml_xml_text_window *window,
                                        sxml_xml_window_failure failure) {
    window->failure = failure;
    return failure.status;
}

static sxml_status sxml_xml_window_fail_resource(sxml_xml_text_window *window,
                                                 sxml_status status) {
    return sxml_xml_window_fail(
        window, (sxml_xml_window_failure){
                    .failure_kind = SXML_XML_WINDOW_FAILURE_RESOURCE,
                    .status = status,
                    .text_offset = sxml_xml_text_window_end(window),
                    .input_byte_offset = 0,
                    .has_input_byte_offset = false});
}

size_t sxml_xml_text_window_end(const sxml_xml_text_window *window) {
    return window->start_offset + window->text.byte_count;
}

/* The raw bytes waiting to be decoded. */
static sxml_byte_view sxml_xml_window_raw(const sxml_xml_text_window *window) {
    if (sxml_xml_window_reads(window)) {
        return (sxml_byte_view){.bytes = window->raw.bytes,
                                .byte_count = window->raw.byte_count};
    }
    const sxml_byte_view input = window->configuration.input;
    return (sxml_byte_view){.bytes = input.bytes + window->raw_input_offset,
                            .byte_count = input.byte_count - window->raw_input_offset};
}

/* Reads from the byte reader until the raw bytes number at least
   byte_count, or the input ends. */
static sxml_status sxml_xml_window_read(sxml_xml_text_window *window,
                                        size_t byte_count) {
    const sxml_byte_reader reader = window->configuration.byte_reader;
    while (!window->is_raw_end && window->raw.byte_count < byte_count) {
        const size_t wanted = byte_count - window->raw.byte_count;
        sxml_status status = sxml_xml_buffer_reserve(
            &window->raw, wanted > SXML_XML_READ_BYTE_COUNT ? wanted : SXML_XML_READ_BYTE_COUNT);
        if (status != SXML_STATUS_OK) {
            return sxml_xml_window_fail_resource(window, status);
        }
        const sxml_memory_view lent = {
            .bytes = window->raw.bytes + window->raw.byte_count,
            .byte_count = window->raw.byte_capacity - window->raw.byte_count};
        sxml_byte_reading reading = {.byte_count = 0, .is_end = false};
        status = reader.operations->read(reader.state, lent, &reading);
        if (status == SXML_STATUS_OK && reading.byte_count > lent.byte_count) {
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
        if (status != SXML_STATUS_OK) {
            return sxml_xml_window_fail_resource(window, status);
        }
        window->raw.byte_count += reading.byte_count;
        window->is_raw_end = reading.is_end || reading.byte_count == 0;
    }
    return SXML_STATUS_OK;
}

/* Whether the raw bytes end the input: all read, and none decoded after
   them. */
static bool sxml_xml_window_raw_is_last(const sxml_xml_text_window *window) {
    return sxml_xml_window_reads(window) ? window->is_raw_end : true;
}

/* How many of the raw bytes form whole characters of the input's
   encoding; the rest wait for more, unless they are the last. */
static size_t sxml_xml_window_decodable(const sxml_xml_text_window *window,
                                        sxml_byte_view raw, bool is_last) {
    if (is_last) {
        return raw.byte_count;
    }
    switch (window->detection.encoding) {
    case SXML_XML_ENCODING_UTF8:
    case SXML_XML_ENCODING_US_ASCII: {
        /* The last lead byte, if its sequence is not complete. */
        const size_t lowest = raw.byte_count > 4 ? raw.byte_count - 4 : 0;
        for (size_t byte_index = raw.byte_count; byte_index > lowest; byte_index--) {
            const uint8_t byte = raw.bytes[byte_index - 1];
            if ((byte & 0xC0) == 0x80) {
                continue;
            }
            const size_t needed = byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : byte >= 0xC0 ? 2 : 1;
            return byte_index - 1 + needed > raw.byte_count ? byte_index - 1 : raw.byte_count;
        }
        return raw.byte_count;
    }
    case SXML_XML_ENCODING_UTF16_LITTLE_ENDIAN:
    case SXML_XML_ENCODING_UTF16_BIG_ENDIAN: {
        size_t byte_count = raw.byte_count & ~(size_t)1;
        if (byte_count >= 2) {
            const bool is_big_endian =
                window->detection.encoding == SXML_XML_ENCODING_UTF16_BIG_ENDIAN;
            const uint8_t high = is_big_endian ? raw.bytes[byte_count - 2]
                                               : raw.bytes[byte_count - 1];
            /* RFC 2781, 2.2: a high surrogate needs the unit after it. */
            if (high >= 0xD8 && high <= 0xDB) {
                byte_count -= 2;
            }
        }
        return byte_count;
    }
    case SXML_XML_ENCODING_ISO_8859_1:
        break;
    }
    return raw.byte_count;
}

/* Decodes or checks part, the next input bytes, appending to the decoded
   text; describes a failure. */
static sxml_status sxml_xml_window_decode_part(sxml_xml_text_window *window,
                                               sxml_byte_view part) {
    const sxml_xml_encoding encoding = window->detection.encoding;
    const size_t text_end = window->start_offset + window->decoded.byte_count;
    sxml_xml_window_failure failure = {.failure_kind = SXML_XML_WINDOW_FAILURE_NONE,
                                       .status = SXML_STATUS_OK,
                                       .text_offset = 0,
                                       .input_byte_offset = 0,
                                       .has_input_byte_offset = false};
    sxml_status status = SXML_STATUS_OK;
    if (encoding == SXML_XML_ENCODING_UTF8 || encoding == SXML_XML_ENCODING_US_ASCII) {
        size_t valid_byte_count = 0;
        if (encoding == SXML_XML_ENCODING_UTF8) {
            sxml_utf8_validation validation = {
                .valid_byte_count = 0, .codepoint_count = 0, .is_ascii = false};
            status = sxml_utf8_validate(part, &validation);
            valid_byte_count = validation.valid_byte_count;
            failure.failure_kind = SXML_XML_WINDOW_FAILURE_UTF8;
        } else {
            sxml_xml_transcoding_failure ascii_failure = {.input_byte_offset = 0};
            status = sxml_xml_check_ascii(part, &ascii_failure);
            valid_byte_count = ascii_failure.input_byte_offset;
            failure.failure_kind = SXML_XML_WINDOW_FAILURE_ASCII;
        }
        if (status != SXML_STATUS_OK) {
            /* The text before the bad bytes, for the location. */
            part.byte_count = valid_byte_count;
            failure.status = status;
        }
        const sxml_status append_status = sxml_xml_buffer_append(&window->decoded, part);
        if (append_status != SXML_STATUS_OK) {
            return sxml_xml_window_fail_resource(window, append_status);
        }
        if (status != SXML_STATUS_OK) {
            failure.text_offset = text_end + valid_byte_count;
            failure.input_byte_offset = window->raw_input_offset + valid_byte_count;
            failure.has_input_byte_offset = encoding != SXML_XML_ENCODING_UTF8;
            return sxml_xml_window_fail(window, failure);
        }
        return SXML_STATUS_OK;
    }
    sxml_xml_transcoding_failure transcoding_failure = {.input_byte_offset = 0};
    status = sxml_xml_transcode(
        &(sxml_xml_transcoding_request){.input = part, .encoding = encoding},
        &window->decoded, &transcoding_failure);
    if (status == SXML_STATUS_OUT_OF_MEMORY || status == SXML_STATUS_ARITHMETIC_OVERFLOW) {
        return sxml_xml_window_fail_resource(window, SXML_STATUS_OUT_OF_MEMORY);
    }
    if (status != SXML_STATUS_OK) {
        return sxml_xml_window_fail(
            window,
            (sxml_xml_window_failure){
                .failure_kind = SXML_XML_WINDOW_FAILURE_UTF16,
                .status = status,
                .text_offset = window->start_offset + window->decoded.byte_count,
                .input_byte_offset =
                    window->raw_input_offset + transcoding_failure.input_byte_offset,
                .has_input_byte_offset = true});
    }
    return SXML_STATUS_OK;
}

/* Where the checks of a decoded region start (offsets in the buffer):
   the characters from region_offset, the line ends from
   translation_offset (XML 1.1 translates them after the declaration). */
typedef struct sxml_xml_window_region {
    size_t region_offset;
    size_t translation_offset;
} sxml_xml_window_region;

/* The checks of decoded text: no U+0000 ([2] Char), and in XML 1.1 no
   restricted character ([2a]; NEL is none), its line ends translated
   (2.11) and a last CR held back until the character after it is
   known. */
static sxml_status sxml_xml_window_check_region(sxml_xml_text_window *window,
                                                sxml_xml_window_region checked) {
    sxml_xml_buffer *decoded = &window->decoded;
    const bool is_version_1_1 = window->declared.version == SXML_XML_VERSION_1_1;
    if (is_version_1_1) {
        sxml_xml_translate_version_1_1_line_ends(decoded, checked.translation_offset);
    }
    const size_t region_offset = checked.region_offset;
    const sxml_byte_view region = {.bytes = decoded->bytes + region_offset,
                                   .byte_count = decoded->byte_count - region_offset};
    const uint8_t *null_byte =
        region.byte_count == 0 ? NULL : memchr(region.bytes, 0, region.byte_count);
    if (null_byte != NULL) {
        return sxml_xml_window_fail(
            window, (sxml_xml_window_failure){
                        .failure_kind = SXML_XML_WINDOW_FAILURE_NUL,
                        .status = SXML_STATUS_MALFORMED_INPUT,
                        .text_offset = window->start_offset + region_offset +
                                       (size_t)(null_byte - region.bytes),
                        .input_byte_offset = 0,
                        .has_input_byte_offset = false});
    }
    if (is_version_1_1) {
        const size_t restricted_offset = sxml_xml_find_restricted_character(region);
        if (restricted_offset != region.byte_count) {
            return sxml_xml_window_fail(
                window, (sxml_xml_window_failure){
                            .failure_kind = SXML_XML_WINDOW_FAILURE_RESTRICTED,
                            .status = SXML_STATUS_MALFORMED_INPUT,
                            .text_offset = window->start_offset + region_offset + restricted_offset,
                            .input_byte_offset = 0,
                            .has_input_byte_offset = false});
        }
    }
    window->held_byte_count =
        is_version_1_1 && !window->is_complete && decoded->byte_count > region_offset &&
                decoded->bytes[decoded->byte_count - 1] == '\r'
            ? 1
            : 0;
    return SXML_STATUS_OK;
}

/* Shows the decoded text, but what is held back, as the window's text. */
static sxml_status sxml_xml_window_show(sxml_xml_text_window *window) {
    window->text = (sxml_byte_view){
        .bytes = window->decoded.bytes,
        .byte_count = window->decoded.byte_count - window->held_byte_count};
    /* tree-sitter's offsets are 32 bits (D222): documents stay below 4 GB. */
    if (sxml_xml_text_window_end(window) > UINT32_MAX - 64) {
        return sxml_xml_window_fail_resource(window, SXML_STATUS_LIMIT_EXCEEDED);
    }
    return SXML_STATUS_OK;
}

/* Decodes the next part, at least byte_count input bytes (all that is
   left when fewer) and, through a byte reader, all it read, without the
   checks; writes where the part starts in the buffer (at a held CR,
   which is checked again). */
static sxml_status sxml_xml_window_decode_next(sxml_xml_text_window *window,
                                               size_t byte_count, size_t *region_offset) {
    if (sxml_xml_window_reads(window)) {
        const sxml_status status = sxml_xml_window_read(window, byte_count);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_byte_view raw = sxml_xml_window_raw(window);
    bool is_last = sxml_xml_window_raw_is_last(window);
    if (!sxml_xml_window_reads(window) && raw.byte_count > byte_count) {
        raw.byte_count = byte_count;
        is_last = false;
    }
    const size_t part_byte_count = sxml_xml_window_decodable(window, raw, is_last);
    *region_offset = window->decoded.byte_count - window->held_byte_count;
    const sxml_status status = sxml_xml_window_decode_part(
        window, (sxml_byte_view){.bytes = raw.bytes, .byte_count = part_byte_count});
    if (status != SXML_STATUS_OK) {
        window->held_byte_count = 0;
        (void)sxml_xml_window_show(window);
        return status;
    }
    window->raw_input_offset += part_byte_count;
    if (sxml_xml_window_reads(window)) {
        memmove(window->raw.bytes, window->raw.bytes + part_byte_count,
                window->raw.byte_count - part_byte_count);
        window->raw.byte_count -= part_byte_count;
    }
    window->is_complete = is_last;
    return SXML_STATUS_OK;
}

/* Decodes and checks the next part. */
static sxml_status sxml_xml_window_decode(sxml_xml_text_window *window,
                                          size_t byte_count) {
    size_t region_offset = 0;
    sxml_status status = sxml_xml_window_decode_next(window, byte_count, &region_offset);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_xml_window_check_region(
        window, (sxml_xml_window_region){.region_offset = region_offset,
                                         .translation_offset = region_offset});
    const sxml_status show_status = sxml_xml_window_show(window);
    return status != SXML_STATUS_OK ? status : show_status;
}

/* A whole input in UTF-8 or US-ASCII: checked at once and used as it is,
   unless XML 1.1 line ends must be translated, which needs a copy. */
static bool sxml_xml_window_uses_input(sxml_xml_text_window *window) {
    const sxml_xml_encoding encoding = window->detection.encoding;
    if (sxml_xml_window_reads(window) ||
        (encoding != SXML_XML_ENCODING_UTF8 && encoding != SXML_XML_ENCODING_US_ASCII)) {
        return false;
    }
    const sxml_byte_view input = window->configuration.input;
    window->declared = sxml_xml_read_declared_version(input);
    if (window->declared.version != SXML_XML_VERSION_1_1) {
        return true;
    }
    const size_t after_declaration = window->declared.declaration_end_offset;
    return !sxml_xml_has_version_1_1_line_end(
        (sxml_byte_view){.bytes = input.bytes + after_declaration,
                         .byte_count = input.byte_count - after_declaration});
}

/* The whole input as the text, checked as decoding would check it. */
static sxml_status sxml_xml_window_take_input(sxml_xml_text_window *window) {
    const sxml_byte_view input = window->configuration.input;
    window->text = input;
    window->is_complete = true;
    window->raw_input_offset = input.byte_count;
    sxml_status status = SXML_STATUS_OK;
    sxml_xml_window_failure failure = {.failure_kind = SXML_XML_WINDOW_FAILURE_UTF8,
                                       .status = SXML_STATUS_OK,
                                       .text_offset = 0,
                                       .input_byte_offset = 0,
                                       .has_input_byte_offset = false};
    if (window->detection.encoding == SXML_XML_ENCODING_UTF8) {
        sxml_utf8_validation validation = {
            .valid_byte_count = 0, .codepoint_count = 0, .is_ascii = false};
        status = sxml_utf8_validate(input, &validation);
        failure.text_offset = validation.valid_byte_count;
    } else {
        sxml_xml_transcoding_failure ascii_failure = {.input_byte_offset = 0};
        status = sxml_xml_check_ascii(input, &ascii_failure);
        failure.failure_kind = SXML_XML_WINDOW_FAILURE_ASCII;
        failure.text_offset = ascii_failure.input_byte_offset;
        failure.input_byte_offset = ascii_failure.input_byte_offset;
        failure.has_input_byte_offset = true;
    }
    if (status != SXML_STATUS_OK) {
        failure.status = status;
        return sxml_xml_window_fail(window, failure);
    }
    if (input.byte_count > UINT32_MAX - 64) {
        return sxml_xml_window_fail_resource(window, SXML_STATUS_LIMIT_EXCEEDED);
    }
    const uint8_t *null_byte =
        input.byte_count == 0 ? NULL : memchr(input.bytes, 0, input.byte_count);
    if (null_byte != NULL) {
        failure.failure_kind = SXML_XML_WINDOW_FAILURE_NUL;
        failure.status = SXML_STATUS_MALFORMED_INPUT;
        failure.text_offset = (size_t)(null_byte - input.bytes);
        failure.has_input_byte_offset = false;
        return sxml_xml_window_fail(window, failure);
    }
    if (window->declared.version == SXML_XML_VERSION_1_1) {
        const size_t restricted_offset = sxml_xml_find_restricted_character(input);
        if (restricted_offset != input.byte_count) {
            failure.failure_kind = SXML_XML_WINDOW_FAILURE_RESTRICTED;
            failure.status = SXML_STATUS_MALFORMED_INPUT;
            failure.text_offset = restricted_offset;
            failure.has_input_byte_offset = false;
            return sxml_xml_window_fail(window, failure);
        }
    }
    return SXML_STATUS_OK;
}

/* The start of the input, for detection: up to its first '>' (the end of
   an XML declaration, whose bytes are ASCII or UTF-16 code units of
   ASCII), and some bytes after it. */
static sxml_status sxml_xml_window_read_start(sxml_xml_text_window *window) {
    size_t wanted = 64;
    for (;;) {
        const sxml_status status = sxml_xml_window_read(window, wanted);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const uint8_t *greater_than =
            window->raw.byte_count == 0 ? NULL
                                        : memchr(window->raw.bytes, '>', window->raw.byte_count);
        if (window->is_raw_end ||
            (greater_than != NULL &&
             (size_t)(greater_than - window->raw.bytes) + 8 < window->raw.byte_count)) {
            return SXML_STATUS_OK;
        }
        wanted = window->raw.byte_count * 2;
    }
}

sxml_status sxml_xml_text_window_open(const sxml_xml_text_window_configuration *configuration,
                                      sxml_xml_text_window *window) {
    *window = (sxml_xml_text_window){
        .configuration = *configuration,
        .detection = {.has_utf8_byte_order_mark = false,
                      .has_utf16_byte_order_mark = false,
                      .encoding = SXML_XML_ENCODING_UTF8,
                      .declared_encoding_name = {.bytes = NULL, .byte_count = 0}},
        .declared = {.version = SXML_XML_VERSION_1_0, .declaration_end_offset = 0},
        .text = {.bytes = NULL, .byte_count = 0},
        .start_offset = 0,
        .is_complete = false,
        .failure = {.failure_kind = SXML_XML_WINDOW_FAILURE_NONE,
                    .status = SXML_STATUS_OK,
                    .text_offset = 0,
                    .input_byte_offset = 0,
                    .has_input_byte_offset = false},
        .copies = true,
        .decoded = sxml_xml_buffer_empty(configuration->allocator),
        .raw = sxml_xml_buffer_empty(configuration->allocator),
        .raw_input_offset = 0,
        .is_raw_end = false,
        .held_byte_count = 0,
    };
    if (sxml_xml_window_reads(window)) {
        const sxml_status status = sxml_xml_window_read_start(window);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_status status = sxml_xml_detect_encoding(
        &(sxml_xml_encoding_request){.input = sxml_xml_window_raw(window),
                                     .encoding_name = configuration->encoding_name},
        &window->detection);
    if (status != SXML_STATUS_OK) {
        return sxml_xml_window_fail(window,
                                    (sxml_xml_window_failure){
                                        .failure_kind = SXML_XML_WINDOW_FAILURE_ENCODING,
                                        .status = SXML_STATUS_UNSUPPORTED_FEATURE,
                                        .text_offset = 0,
                                        .input_byte_offset = 0,
                                        .has_input_byte_offset = true});
    }
    if (sxml_xml_window_uses_input(window)) {
        window->copies = false;
        return sxml_xml_window_take_input(window);
    }
    /* The first part holds the XML declaration (read_start), whose
       version decides the checks of every part, this one too. */
    size_t region_offset = 0;
    status = sxml_xml_window_decode_next(window, SXML_XML_WINDOW_BYTE_COUNT, &region_offset);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    window->declared = sxml_xml_read_declared_version(
        (sxml_byte_view){.bytes = window->decoded.bytes, .byte_count = window->decoded.byte_count});
    status = sxml_xml_window_check_region(
        window, (sxml_xml_window_region){
                    .region_offset = 0,
                    .translation_offset = window->declared.declaration_end_offset});
    const sxml_status show_status = sxml_xml_window_show(window);
    return status != SXML_STATUS_OK ? status : show_status;
}

sxml_status sxml_xml_text_window_extend(sxml_xml_text_window *window) {
    if (window->is_complete) {
        return SXML_STATUS_OK;
    }
    const size_t byte_count = window->text.byte_count > SXML_XML_WINDOW_BYTE_COUNT
                                  ? window->text.byte_count
                                  : SXML_XML_WINDOW_BYTE_COUNT;
    return sxml_xml_window_decode(window, byte_count);
}

bool sxml_xml_text_window_should_discard(const sxml_xml_text_window *window,
                                         size_t text_offset) {
    if (!window->copies || text_offset <= window->start_offset) {
        return false;
    }
    /* A move of what is kept is worth it when at least as much goes. */
    const size_t given_up = text_offset - window->start_offset;
    return given_up >= window->decoded.byte_count - given_up;
}

void sxml_xml_text_window_discard(sxml_xml_text_window *window, size_t text_offset) {
    const size_t given_up = text_offset - window->start_offset;
    memmove(window->decoded.bytes, window->decoded.bytes + given_up,
            window->decoded.byte_count - given_up);
    window->decoded.byte_count -= given_up;
    window->start_offset = text_offset;
    (void)sxml_xml_window_show(window);
}

void sxml_xml_text_window_close(sxml_xml_text_window *window) {
    if (window->decoded.allocator.operations == NULL &&
        window->raw.allocator.operations == NULL) {
        return;
    }
    sxml_xml_buffer_release(&window->decoded);
    sxml_xml_buffer_release(&window->raw);
}
