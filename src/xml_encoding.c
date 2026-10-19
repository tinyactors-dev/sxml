/*
 * Character encodings, characters and source positions for the XML parser
 * (src/xml_internal.h).
 *
 * Golden file: src/memory.c (layout, naming, outputs untouched on failure).
 *
 * The parser reads UTF-8, UTF-16, ISO-8859-1 and US-ASCII (D51). It decides
 * the encoding as XML 1.0 Fifth Edition, Appendix F describes, decodes
 * everything that is not already UTF-8 into a UTF-8 copy, and parses that;
 * positions are mapped back to the input's bytes for diagnostics.
 */
#include "xml_internal.h"

#include <string.h>

/* A byte view over a byte array (not a pointer: sizeof gives the array's
   size). A macro because a function cannot take the size of an array. */
#define SXML_XML_ARRAY_VIEW(array)                                             \
    ((sxml_byte_view){.bytes = (array), .byte_count = sizeof(array)})

/* ------------------------------------------------------ Encoding names */

/* An encoding name and what it names. */
typedef struct sxml_xml_encoding_alias {
    const char *name;
    sxml_xml_encoding_family family;
} sxml_xml_encoding_alias;

/*
 * The IANA names and aliases (IANA "Character Sets" registry) of the
 * encodings sxml reads. XML 1.0, section 4.3.3 asks for the registered
 * names; matching ignores ASCII case.
 */
static const sxml_xml_encoding_alias sxml_xml_encoding_aliases[] = {
    {"UTF-8", SXML_XML_ENCODING_FAMILY_UTF8},
    {"UTF-16", SXML_XML_ENCODING_FAMILY_UTF16},
    {"UTF-16LE", SXML_XML_ENCODING_FAMILY_UTF16_LITTLE_ENDIAN},
    {"UTF-16BE", SXML_XML_ENCODING_FAMILY_UTF16_BIG_ENDIAN},
    {"ISO-10646-UCS-2", SXML_XML_ENCODING_FAMILY_UTF16},
    {"ISO-8859-1", SXML_XML_ENCODING_FAMILY_ISO_8859_1},
    {"ISO_8859-1", SXML_XML_ENCODING_FAMILY_ISO_8859_1},
    {"ISO_8859-1:1987", SXML_XML_ENCODING_FAMILY_ISO_8859_1},
    {"ISO-IR-100", SXML_XML_ENCODING_FAMILY_ISO_8859_1},
    {"LATIN1", SXML_XML_ENCODING_FAMILY_ISO_8859_1},
    {"L1", SXML_XML_ENCODING_FAMILY_ISO_8859_1},
    {"IBM819", SXML_XML_ENCODING_FAMILY_ISO_8859_1},
    {"CP819", SXML_XML_ENCODING_FAMILY_ISO_8859_1},
    {"CSISOLATIN1", SXML_XML_ENCODING_FAMILY_ISO_8859_1},
    {"US-ASCII", SXML_XML_ENCODING_FAMILY_US_ASCII},
    {"ASCII", SXML_XML_ENCODING_FAMILY_US_ASCII},
    {"ANSI_X3.4-1968", SXML_XML_ENCODING_FAMILY_US_ASCII},
    {"ANSI_X3.4-1986", SXML_XML_ENCODING_FAMILY_US_ASCII},
    {"ISO-IR-6", SXML_XML_ENCODING_FAMILY_US_ASCII},
    {"ISO_646.IRV:1991", SXML_XML_ENCODING_FAMILY_US_ASCII},
    {"ISO646-US", SXML_XML_ENCODING_FAMILY_US_ASCII},
    {"US", SXML_XML_ENCODING_FAMILY_US_ASCII},
    {"IBM367", SXML_XML_ENCODING_FAMILY_US_ASCII},
    {"CP367", SXML_XML_ENCODING_FAMILY_US_ASCII},
    {"CSASCII", SXML_XML_ENCODING_FAMILY_US_ASCII},
};

static uint8_t sxml_xml_ascii_upper(uint8_t byte) {
    return byte >= 'a' && byte <= 'z' ? (uint8_t)(byte - ('a' - 'A')) : byte;
}

sxml_xml_encoding_family
sxml_xml_classify_encoding_name(sxml_byte_view encoding_name) {
    for (size_t alias_index = 0;
         alias_index <
         sizeof sxml_xml_encoding_aliases / sizeof sxml_xml_encoding_aliases[0];
         alias_index++) {
        const sxml_xml_encoding_alias *alias =
            &sxml_xml_encoding_aliases[alias_index];
        const size_t alias_byte_count = strlen(alias->name);
        if (alias_byte_count != encoding_name.byte_count) {
            continue;
        }
        bool is_equal = true;
        for (size_t byte_index = 0; byte_index < alias_byte_count;
             byte_index++) {
            if (sxml_xml_ascii_upper(encoding_name.bytes[byte_index]) !=
                (uint8_t)alias->name[byte_index]) {
                is_equal = false;
                break;
            }
        }
        if (is_equal) {
            return alias->family;
        }
    }
    return SXML_XML_ENCODING_FAMILY_UNSUPPORTED;
}

/* ------------------------------------------------------------ Detection */

/* True when input starts with prefix. */
static bool sxml_xml_starts_with(sxml_byte_view input, sxml_byte_view prefix) {
    return input.byte_count >= prefix.byte_count &&
           memcmp(input.bytes, prefix.bytes, prefix.byte_count) == 0;
}

bool sxml_xml_byte_is_whitespace(uint8_t byte) {
    return byte == 0x20 || byte == 0x09 || byte == 0x0D || byte == 0x0A;
}

/* A cursor over the bytes of an XML declaration read before decoding. */
typedef struct sxml_xml_declaration_reader {
    sxml_byte_view input;
    size_t byte_offset;
} sxml_xml_declaration_reader;

static void
sxml_xml_reader_skip_whitespace(sxml_xml_declaration_reader *reader) {
    while (
        reader->byte_offset < reader->input.byte_count &&
        sxml_xml_byte_is_whitespace(reader->input.bytes[reader->byte_offset])) {
        reader->byte_offset += 1;
    }
}

/* Reads the bytes of a pseudo-attribute name or value up to stop. */
static sxml_byte_view
sxml_xml_reader_take_until(sxml_xml_declaration_reader *reader, uint8_t stop) {
    const size_t start = reader->byte_offset;
    while (reader->byte_offset < reader->input.byte_count &&
           reader->input.bytes[reader->byte_offset] != stop &&
           reader->input.bytes[reader->byte_offset] != '?' &&
           reader->input.bytes[reader->byte_offset] != '>') {
        reader->byte_offset += 1;
    }
    return (sxml_byte_view){.bytes = reader->input.bytes + start,
                            .byte_count = reader->byte_offset - start};
}

/* The pseudo-attributes of an XML declaration read before parsing. */
typedef struct sxml_xml_declaration_scan {
    sxml_byte_view version;
    sxml_byte_view encoding;
    /* The offset just after "?>"; 0 when the declaration does not have
       the expected shape. */
    size_t end_offset;
} sxml_xml_declaration_scan;

/*
 * Reads the pseudo-attributes of the XML declaration at the start of
 * input, which is ASCII-compatible (XML 1.0, [23] XMLDecl, [24]
 * VersionInfo, [80] EncodingDecl). Views stay empty when there is no
 * declaration or it does not have the expected shape; the grammar checks
 * the declaration properly later.
 */
static sxml_xml_declaration_scan
sxml_xml_scan_declaration(sxml_byte_view input) {
    static const uint8_t declaration_start[] = {'<', '?', 'x', 'm', 'l'};
    sxml_xml_declaration_scan scan = {
        .version = {.bytes = NULL, .byte_count = 0},
        .encoding = {.bytes = NULL, .byte_count = 0},
        .end_offset = 0};
    if (!sxml_xml_starts_with(input, SXML_XML_ARRAY_VIEW(declaration_start))) {
        return scan;
    }
    sxml_xml_declaration_reader reader = {
        .input = input, .byte_offset = sizeof declaration_start};
    for (;;) {
        const size_t before_whitespace = reader.byte_offset;
        sxml_xml_reader_skip_whitespace(&reader);
        if (reader.byte_offset + 1 < input.byte_count &&
            input.bytes[reader.byte_offset] == '?' &&
            input.bytes[reader.byte_offset + 1] == '>') {
            scan.end_offset = reader.byte_offset + 2;
            return scan;
        }
        if (reader.byte_offset == before_whitespace ||
            reader.byte_offset >= input.byte_count) {
            return scan;
        }
        const sxml_byte_view name = sxml_xml_reader_take_until(&reader, '=');
        sxml_byte_view trimmed = name;
        while (trimmed.byte_count > 0 &&
               sxml_xml_byte_is_whitespace(
                   trimmed.bytes[trimmed.byte_count - 1])) {
            trimmed.byte_count -= 1;
        }
        if (reader.byte_offset >= input.byte_count ||
            input.bytes[reader.byte_offset] != '=') {
            return scan;
        }
        reader.byte_offset += 1;
        sxml_xml_reader_skip_whitespace(&reader);
        if (reader.byte_offset >= input.byte_count) {
            return scan;
        }
        const uint8_t quote = input.bytes[reader.byte_offset];
        if (quote != '"' && quote != '\'') {
            return scan;
        }
        reader.byte_offset += 1;
        const sxml_byte_view value = sxml_xml_reader_take_until(&reader, quote);
        if (reader.byte_offset >= input.byte_count ||
            input.bytes[reader.byte_offset] != quote) {
            return scan;
        }
        reader.byte_offset += 1;
        if (sxml_byte_view_equals(trimmed,
                                  SXML_BYTE_VIEW_FROM_LITERAL("encoding"))) {
            scan.encoding = value;
        } else if (sxml_byte_view_equals(
                       trimmed, SXML_BYTE_VIEW_FROM_LITERAL("version"))) {
            scan.version = value;
        }
    }
}

sxml_xml_declared_version sxml_xml_read_declared_version(sxml_byte_view text) {
    static const uint8_t utf8_mark[] = {0xEF, 0xBB, 0xBF};
    sxml_byte_view after_mark = text;
    size_t mark_byte_count = 0;
    if (sxml_xml_starts_with(text, SXML_XML_ARRAY_VIEW(utf8_mark))) {
        mark_byte_count = sizeof utf8_mark;
        after_mark.bytes += mark_byte_count;
        after_mark.byte_count -= mark_byte_count;
    }
    const sxml_xml_declaration_scan scan =
        sxml_xml_scan_declaration(after_mark);
    sxml_xml_declared_version declared = {.version = SXML_XML_VERSION_1_0,
                                          .declaration_end_offset = 0};
    if (scan.end_offset != 0) {
        declared.declaration_end_offset = mark_byte_count + scan.end_offset;
        /* XML 1.1, [26] VersionNum ::= '1.1'; other 1.x numbers are read
           as 1.0 (XML 1.0 Fifth Edition, 2.8). */
        if (sxml_byte_view_equals(scan.version,
                                  SXML_BYTE_VIEW_FROM_LITERAL("1.1"))) {
            declared.version = SXML_XML_VERSION_1_1;
        }
    }
    return declared;
}

sxml_xml_declared_encoding
sxml_xml_read_declared_encoding(sxml_byte_view text) {
    const sxml_xml_declaration_scan scan = sxml_xml_scan_declaration(text);
    return (sxml_xml_declared_encoding){
        .encoding_name = scan.end_offset == 0
                             ? (sxml_byte_view){.bytes = NULL, .byte_count = 0}
                             : scan.encoding,
        .declaration_end_offset = scan.end_offset};
}

/* The encoding of a family when the byte order is known otherwise. */
static sxml_status
sxml_xml_encoding_of_family(sxml_xml_encoding_family family,
                            sxml_xml_encoding utf16_byte_order,
                            sxml_xml_encoding *encoding) {
    switch (family) {
    case SXML_XML_ENCODING_FAMILY_UTF8:
        *encoding = SXML_XML_ENCODING_UTF8;
        return SXML_STATUS_OK;
    case SXML_XML_ENCODING_FAMILY_UTF16:
        *encoding = utf16_byte_order;
        return SXML_STATUS_OK;
    case SXML_XML_ENCODING_FAMILY_UTF16_LITTLE_ENDIAN:
        *encoding = SXML_XML_ENCODING_UTF16_LITTLE_ENDIAN;
        return SXML_STATUS_OK;
    case SXML_XML_ENCODING_FAMILY_UTF16_BIG_ENDIAN:
        *encoding = SXML_XML_ENCODING_UTF16_BIG_ENDIAN;
        return SXML_STATUS_OK;
    case SXML_XML_ENCODING_FAMILY_ISO_8859_1:
        *encoding = SXML_XML_ENCODING_ISO_8859_1;
        return SXML_STATUS_OK;
    case SXML_XML_ENCODING_FAMILY_US_ASCII:
        *encoding = SXML_XML_ENCODING_US_ASCII;
        return SXML_STATUS_OK;
    case SXML_XML_ENCODING_FAMILY_UNSUPPORTED:
        break;
    }
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}

sxml_status sxml_xml_detect_encoding(const sxml_xml_encoding_request *request,
                                     sxml_xml_encoding_detection *detection) {
    static const uint8_t utf8_mark[] = {0xEF, 0xBB, 0xBF};
    static const uint8_t utf16_big_endian_mark[] = {0xFE, 0xFF};
    static const uint8_t utf16_little_endian_mark[] = {0xFF, 0xFE};
    static const uint8_t ucs4_patterns[][4] = {
        {0x00, 0x00, 0xFE, 0xFF}, {0x00, 0x00, 0xFF, 0xFE},
        {0x00, 0x00, 0x00, 0x3C}, {0x3C, 0x00, 0x00, 0x00},
        {0x00, 0x00, 0x3C, 0x00}, {0x00, 0x3C, 0x00, 0x00},
    };
    static const uint8_t utf16_big_endian_start[] = {0x00, 0x3C, 0x00, 0x3F};
    static const uint8_t utf16_little_endian_start[] = {0x3C, 0x00, 0x3F, 0x00};
    static const uint8_t ebcdic_start[] = {0x4C, 0x6F, 0xA7, 0x94};
    const sxml_byte_view input = request->input;
    sxml_xml_encoding_detection detected = {
        .has_utf8_byte_order_mark =
            sxml_xml_starts_with(input, SXML_XML_ARRAY_VIEW(utf8_mark)),
        .has_utf16_byte_order_mark = false,
        .encoding = SXML_XML_ENCODING_UTF8,
        .declared_encoding_name = {.bytes = NULL, .byte_count = 0},
    };
    /* Appendix F.1: UCS-4 and EBCDIC are recognized only to be refused. */
    for (size_t pattern_index = 0;
         pattern_index < sizeof ucs4_patterns / sizeof ucs4_patterns[0];
         pattern_index++) {
        if (sxml_xml_starts_with(
                input, SXML_XML_ARRAY_VIEW(ucs4_patterns[pattern_index]))) {
            return SXML_STATUS_UNSUPPORTED_FEATURE;
        }
    }
    sxml_xml_encoding utf16_byte_order = SXML_XML_ENCODING_UTF16_BIG_ENDIAN;
    bool is_utf16 = false;
    if (sxml_xml_starts_with(input,
                             SXML_XML_ARRAY_VIEW(utf16_big_endian_mark))) {
        detected.has_utf16_byte_order_mark = true;
        is_utf16 = true;
    } else if (sxml_xml_starts_with(
                   input, SXML_XML_ARRAY_VIEW(utf16_little_endian_mark))) {
        detected.has_utf16_byte_order_mark = true;
        utf16_byte_order = SXML_XML_ENCODING_UTF16_LITTLE_ENDIAN;
        is_utf16 = true;
    } else if (sxml_xml_starts_with(
                   input, SXML_XML_ARRAY_VIEW(utf16_big_endian_start))) {
        is_utf16 = true;
    } else if (sxml_xml_starts_with(
                   input, SXML_XML_ARRAY_VIEW(utf16_little_endian_start))) {
        utf16_byte_order = SXML_XML_ENCODING_UTF16_LITTLE_ENDIAN;
        is_utf16 = true;
    }
    if (request->encoding_name.byte_count != 0) {
        /* F.2: the external name decides; a UTF-16 byte order mark still
           gives the byte order. */
        sxml_status status = sxml_xml_encoding_of_family(
            sxml_xml_classify_encoding_name(request->encoding_name),
            utf16_byte_order, &detected.encoding);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        *detection = detected;
        return SXML_STATUS_OK;
    }
    if (is_utf16) {
        /* The declaration is read after decoding. */
        detected.encoding = utf16_byte_order;
        *detection = detected;
        return SXML_STATUS_OK;
    }
    if (sxml_xml_starts_with(input, SXML_XML_ARRAY_VIEW(ebcdic_start))) {
        return SXML_STATUS_UNSUPPORTED_FEATURE;
    }
    sxml_byte_view after_mark = input;
    if (detected.has_utf8_byte_order_mark) {
        after_mark.bytes += sizeof utf8_mark;
        after_mark.byte_count -= sizeof utf8_mark;
    }
    detected.declared_encoding_name =
        sxml_xml_scan_declaration(after_mark).encoding;
    if (detected.declared_encoding_name.byte_count != 0) {
        sxml_xml_encoding_family family =
            sxml_xml_classify_encoding_name(detected.declared_encoding_name);
        if (family == SXML_XML_ENCODING_FAMILY_UNSUPPORTED) {
            return SXML_STATUS_UNSUPPORTED_FEATURE;
        }
        /* A document in an 8-bit encoding that declares UTF-16 contradicts
           itself; it is decoded as UTF-8 and the parser reports the
           contradiction once the declaration has been parsed. */
        if (family != SXML_XML_ENCODING_FAMILY_UTF16 &&
            family != SXML_XML_ENCODING_FAMILY_UTF16_LITTLE_ENDIAN &&
            family != SXML_XML_ENCODING_FAMILY_UTF16_BIG_ENDIAN &&
            !detected.has_utf8_byte_order_mark) {
            sxml_status status = sxml_xml_encoding_of_family(
                family, utf16_byte_order, &detected.encoding);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
    }
    *detection = detected;
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------- Transcoding */

sxml_status sxml_xml_transcode(const sxml_xml_transcoding_request *request,
                               sxml_xml_buffer *output,
                               sxml_xml_transcoding_failure *failure) {
    const sxml_byte_view input = request->input;
    if (request->encoding == SXML_XML_ENCODING_ISO_8859_1) {
        /* Every byte is the codepoint of the same value (ISO/IEC 8859-1). */
        size_t byte_count = 0;
        sxml_status status =
            sxml_size_multiply(input.byte_count, 2, &byte_count);
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_reserve(output, byte_count);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        for (size_t byte_index = 0; byte_index < input.byte_count;
             byte_index++) {
            const uint8_t byte = input.bytes[byte_index];
            if (byte < 0x80) {
                output->bytes[output->byte_count] = byte;
                output->byte_count += 1;
            } else {
                output->bytes[output->byte_count] =
                    (uint8_t)(0xC0 | (byte >> 6));
                output->bytes[output->byte_count + 1] =
                    (uint8_t)(0x80 | (byte & 0x3F));
                output->byte_count += 2;
            }
        }
        return SXML_STATUS_OK;
    }
    const bool is_big_endian =
        request->encoding == SXML_XML_ENCODING_UTF16_BIG_ENDIAN;
    /* At most 3 UTF-8 bytes for every 2 input bytes. */
    size_t byte_count = 0;
    sxml_status status =
        sxml_size_multiply(input.byte_count / 2 + 1, 3, &byte_count);
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_reserve(output, byte_count);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t byte_index = 0;
    while (byte_index + 1 < input.byte_count) {
        const size_t unit_offset = byte_index;
        uint32_t unit = is_big_endian
                            ? ((uint32_t)input.bytes[byte_index] << 8) |
                                  input.bytes[byte_index + 1]
                            : ((uint32_t)input.bytes[byte_index + 1] << 8) |
                                  input.bytes[byte_index];
        byte_index += 2;
        uint32_t codepoint = unit;
        /* RFC 2781, section 2.2: a high surrogate must be followed by a
           low one; a low one alone is an error. */
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            failure->input_byte_offset = unit_offset;
            return SXML_STATUS_INVALID_UTF8;
        }
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (byte_index + 1 >= input.byte_count) {
                failure->input_byte_offset = unit_offset;
                return SXML_STATUS_INVALID_UTF8;
            }
            uint32_t low = is_big_endian
                               ? ((uint32_t)input.bytes[byte_index] << 8) |
                                     input.bytes[byte_index + 1]
                               : ((uint32_t)input.bytes[byte_index + 1] << 8) |
                                     input.bytes[byte_index];
            if (low < 0xDC00 || low > 0xDFFF) {
                failure->input_byte_offset = unit_offset;
                return SXML_STATUS_INVALID_UTF8;
            }
            byte_index += 2;
            codepoint = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
        }
        status = sxml_xml_buffer_append_codepoint(output, codepoint);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (byte_index != input.byte_count) {
        failure->input_byte_offset = byte_index;
        return SXML_STATUS_INVALID_UTF8;
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_xml_check_ascii(sxml_byte_view input,
                                 sxml_xml_transcoding_failure *failure) {
    for (size_t byte_index = 0; byte_index < input.byte_count; byte_index++) {
        if (input.bytes[byte_index] >= 0x80) {
            failure->input_byte_offset = byte_index;
            return SXML_STATUS_MALFORMED_INPUT;
        }
    }
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------- Characters */

bool sxml_xml_codepoint_is_char(uint32_t codepoint) {
    /* [2] Char ::= #x9 | #xA | #xD | [#x20-#xD7FF] | [#xE000-#xFFFD]
                  | [#x10000-#x10FFFF] */
    return codepoint == 0x9 || codepoint == 0xA || codepoint == 0xD ||
           (codepoint >= 0x20 && codepoint <= 0xD7FF) ||
           (codepoint >= 0xE000 && codepoint <= 0xFFFD) ||
           (codepoint >= 0x10000 && codepoint <= 0x10FFFF);
}

bool sxml_xml_codepoint_is_version_1_1_char(uint32_t codepoint) {
    /* XML 1.1, [2] Char ::= [#x1-#xD7FF] | [#xE000-#xFFFD]
                           | [#x10000-#x10FFFF] */
    return (codepoint >= 0x1 && codepoint <= 0xD7FF) ||
           (codepoint >= 0xE000 && codepoint <= 0xFFFD) ||
           (codepoint >= 0x10000 && codepoint <= 0x10FFFF);
}

/* XML 1.1, [2a] RestrictedChar ::= [#x1-#x8] | [#xB-#xC] | [#xE-#x1F]
   | [#x7F-#x84] | [#x86-#x9F] */
static bool sxml_xml_codepoint_is_restricted(uint32_t codepoint) {
    return (codepoint >= 0x1 && codepoint <= 0x8) || codepoint == 0xB ||
           codepoint == 0xC || (codepoint >= 0xE && codepoint <= 0x1F) ||
           (codepoint >= 0x7F && codepoint <= 0x84) ||
           (codepoint >= 0x86 && codepoint <= 0x9F);
}

size_t sxml_xml_find_restricted_character(sxml_byte_view text) {
    for (size_t byte_index = 0; byte_index < text.byte_count;
         byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        /* Below U+0080 the byte is the codepoint; U+0080 to U+009F are
           0xC2 0x80 to 0xC2 0x9F. */
        if ((byte < 0x80 && sxml_xml_codepoint_is_restricted(byte)) ||
            (byte == 0xC2 && byte_index + 1 < text.byte_count &&
             sxml_xml_codepoint_is_restricted(text.bytes[byte_index + 1]))) {
            return byte_index;
        }
    }
    return text.byte_count;
}

/* The UTF-8 forms of NEL (U+0085) and LINE SEPARATOR (U+2028). */
static bool sxml_xml_is_next_line_at(sxml_byte_view text, size_t byte_index) {
    return byte_index + 1 < text.byte_count && text.bytes[byte_index] == 0xC2 &&
           text.bytes[byte_index + 1] == 0x85;
}

static bool sxml_xml_is_line_separator_at(sxml_byte_view text,
                                          size_t byte_index) {
    return byte_index + 2 < text.byte_count && text.bytes[byte_index] == 0xE2 &&
           text.bytes[byte_index + 1] == 0x80 &&
           text.bytes[byte_index + 2] == 0xA8;
}

bool sxml_xml_has_version_1_1_line_end(sxml_byte_view text) {
    for (size_t byte_index = 0; byte_index < text.byte_count;
         byte_index += 1) {
        if (sxml_xml_is_next_line_at(text, byte_index) ||
            sxml_xml_is_line_separator_at(text, byte_index)) {
            return true;
        }
    }
    return false;
}

void sxml_xml_translate_version_1_1_line_ends(sxml_xml_buffer *text,
                                              size_t start_offset) {
    uint8_t *bytes = text->bytes;
    const sxml_byte_view original = {.bytes = bytes,
                                     .byte_count = text->byte_count};
    size_t write_index = start_offset;
    size_t read_index = start_offset;
    while (read_index < original.byte_count) {
        if (sxml_xml_is_next_line_at(original, read_index)) {
            /* NEL alone is LF; after CR, the CR already ends the line. */
            const bool follows_carriage_return =
                write_index > start_offset && bytes[write_index - 1] == '\r';
            if (!follows_carriage_return) {
                bytes[write_index] = '\n';
                write_index += 1;
            }
            read_index += 2;
        } else if (sxml_xml_is_line_separator_at(original, read_index)) {
            /* CR LINE SEPARATOR is two line ends: the CR must not join the
               LF that follows into one. */
            if (write_index > start_offset && bytes[write_index - 1] == '\r') {
                bytes[write_index - 1] = '\n';
            }
            bytes[write_index] = '\n';
            write_index += 1;
            read_index += 3;
        } else {
            bytes[write_index] = bytes[read_index];
            write_index += 1;
            read_index += 1;
        }
    }
    text->byte_count = write_index;
}

/* A range of codepoints, both ends included. */
typedef struct sxml_xml_codepoint_range {
    uint32_t first;
    uint32_t last;
} sxml_xml_codepoint_range;

/* [4] NameStartChar of XML 1.0 Fifth Edition. */
static const sxml_xml_codepoint_range sxml_xml_name_start_ranges[] = {
    {':', ':'},       {'A', 'Z'},       {'_', '_'},       {'a', 'z'},
    {0xC0, 0xD6},     {0xD8, 0xF6},     {0xF8, 0x2FF},    {0x370, 0x37D},
    {0x37F, 0x1FFF},  {0x200C, 0x200D}, {0x2070, 0x218F}, {0x2C00, 0x2FEF},
    {0x3001, 0xD7FF}, {0xF900, 0xFDCF}, {0xFDF0, 0xFFFD}, {0x10000, 0xEFFFF},
};

/* [4a] NameChar beyond NameStartChar. */
static const sxml_xml_codepoint_range sxml_xml_name_extra_ranges[] = {
    {'-', '-'},   {'.', '.'},     {'0', '9'},
    {0xB7, 0xB7}, {0x300, 0x36F}, {0x203F, 0x2040},
};

static bool sxml_xml_codepoint_in_ranges(uint32_t codepoint,
                                         const sxml_xml_codepoint_range *ranges,
                                         size_t range_count) {
    for (size_t range_index = 0; range_index < range_count; range_index++) {
        if (codepoint >= ranges[range_index].first &&
            codepoint <= ranges[range_index].last) {
            return true;
        }
    }
    return false;
}

bool sxml_xml_codepoint_is_name_start(uint32_t codepoint) {
    return sxml_xml_codepoint_in_ranges(
        codepoint, sxml_xml_name_start_ranges,
        sizeof sxml_xml_name_start_ranges /
            sizeof sxml_xml_name_start_ranges[0]);
}

bool sxml_xml_codepoint_is_name(uint32_t codepoint) {
    return sxml_xml_codepoint_is_name_start(codepoint) ||
           sxml_xml_codepoint_in_ranges(
               codepoint, sxml_xml_name_extra_ranges,
               sizeof sxml_xml_name_extra_ranges /
                   sizeof sxml_xml_name_extra_ranges[0]);
}

/* The encoded byte count of the UTF-8 sequence starting with lead_byte. */
static size_t sxml_xml_utf8_sequence_byte_count(uint8_t lead_byte) {
    if (lead_byte < 0x80) {
        return 1;
    }
    if (lead_byte < 0xE0) {
        return 2;
    }
    if (lead_byte < 0xF0) {
        return 3;
    }
    return 4;
}

sxml_xml_decoded_codepoint sxml_xml_decode_codepoint(sxml_byte_view utf8,
                                                     size_t byte_offset) {
    const uint8_t *bytes = utf8.bytes + byte_offset;
    const size_t encoded_byte_count =
        sxml_xml_utf8_sequence_byte_count(bytes[0]);
    uint32_t codepoint = 0;
    switch (encoded_byte_count) {
    case 1:
        codepoint = bytes[0];
        break;
    case 2:
        codepoint = ((uint32_t)(bytes[0] & 0x1F) << 6) | (bytes[1] & 0x3F);
        break;
    case 3:
        codepoint = ((uint32_t)(bytes[0] & 0x0F) << 12) |
                    ((uint32_t)(bytes[1] & 0x3F) << 6) | (bytes[2] & 0x3F);
        break;
    default:
        codepoint = ((uint32_t)(bytes[0] & 0x07) << 18) |
                    ((uint32_t)(bytes[1] & 0x3F) << 12) |
                    ((uint32_t)(bytes[2] & 0x3F) << 6) | (bytes[3] & 0x3F);
        break;
    }
    return (sxml_xml_decoded_codepoint){
        .codepoint = codepoint, .encoded_byte_count = encoded_byte_count};
}

sxml_byte_view sxml_xml_text_trim_whitespace(sxml_byte_view text) {
    while (text.byte_count > 0 && sxml_xml_byte_is_whitespace(text.bytes[0])) {
        text.bytes += 1;
        text.byte_count -= 1;
    }
    while (text.byte_count > 0 && sxml_xml_byte_is_whitespace(text.bytes[text.byte_count - 1])) {
        text.byte_count -= 1;
    }
    return text;
}

bool sxml_xml_text_is_name(sxml_byte_view name) {
    if (name.byte_count == 0) {
        return false;
    }
    size_t byte_offset = 0;
    bool is_first = true;
    while (byte_offset < name.byte_count) {
        const sxml_xml_decoded_codepoint decoded =
            sxml_xml_decode_codepoint(name, byte_offset);
        if (decoded.encoded_byte_count > name.byte_count - byte_offset) {
            return false;
        }
        const bool is_allowed =
            is_first ? sxml_xml_codepoint_is_name_start(decoded.codepoint)
                     : sxml_xml_codepoint_is_name(decoded.codepoint);
        if (!is_allowed) {
            return false;
        }
        is_first = false;
        byte_offset += decoded.encoded_byte_count;
    }
    return true;
}

bool sxml_xml_text_is_ncname(sxml_byte_view name) {
    return sxml_xml_text_is_name(name) && memchr(name.bytes, ':', name.byte_count) == NULL;
}

bool sxml_xml_text_is_whitespace(sxml_byte_view text) {
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        if (!sxml_xml_byte_is_whitespace(text.bytes[byte_index])) {
            return false;
        }
    }
    return true;
}

bool sxml_xml_text_next_token(sxml_byte_view *rest, sxml_byte_view *token) {
    size_t start = 0;
    while (start < rest->byte_count && sxml_xml_byte_is_whitespace(rest->bytes[start])) {
        start += 1;
    }
    size_t end = start;
    while (end < rest->byte_count && !sxml_xml_byte_is_whitespace(rest->bytes[end])) {
        end += 1;
    }
    if (start == end) {
        return false;
    }
    *token = (sxml_byte_view){.bytes = rest->bytes + start, .byte_count = end - start};
    *rest = (sxml_byte_view){.bytes = rest->bytes + end, .byte_count = rest->byte_count - end};
    return true;
}

/* ------------------------------------------------------------ Positions */

sxml_xml_position_tracker sxml_xml_position_tracker_start(
    sxml_xml_position_tracker_configuration configuration) {
    static const uint8_t utf8_mark[] = {0xEF, 0xBB, 0xBF};
    const bool text_has_mark = sxml_xml_starts_with(
        configuration.text, SXML_XML_ARRAY_VIEW(utf8_mark));
    size_t input_mark_byte_count = 0;
    if (configuration.has_byte_order_mark) {
        input_mark_byte_count =
            configuration.input_encoding == SXML_XML_ENCODING_UTF8 ? 3 : 2;
    }
    const size_t text_start = text_has_mark ? sizeof utf8_mark : 0;
    return (sxml_xml_position_tracker){
        .text = configuration.text,
        .input_encoding = configuration.input_encoding,
        .text_start_offset = text_start,
        .input_start_offset = input_mark_byte_count,
        .text_offset = text_start,
        .input_offset = input_mark_byte_count,
        .line_number = 1,
        .column_number = 1,
        .follows_carriage_return = false,
    };
}

/* How many input bytes the codepoint whose UTF-8 lead byte is given took. */
static size_t sxml_xml_input_byte_count(sxml_xml_encoding encoding,
                                        uint8_t lead_byte) {
    switch (encoding) {
    case SXML_XML_ENCODING_UTF8:
        return sxml_xml_utf8_sequence_byte_count(lead_byte);
    case SXML_XML_ENCODING_UTF16_LITTLE_ENDIAN:
    case SXML_XML_ENCODING_UTF16_BIG_ENDIAN:
        return sxml_xml_utf8_sequence_byte_count(lead_byte) == 4 ? 4 : 2;
    case SXML_XML_ENCODING_ISO_8859_1:
    case SXML_XML_ENCODING_US_ASCII:
        break;
    }
    return 1;
}

/*
 * Moves the tracker back to text_offset, which lies between the start of
 * the text and the tracker, undoing what the forward walk counted: the
 * parser asks for an element's position after its attributes' (P8), and a
 * restart from the beginning there made every parse quadratic. Costs the
 * distance moved, plus the start of the line when a line end is crossed.
 */
static void
sxml_xml_position_tracker_move_back(sxml_xml_position_tracker *tracker,
                                    size_t text_offset) {
    const uint8_t *bytes = tracker->text.bytes;
    bool crosses_line_end = false;
    for (size_t byte_offset = text_offset; byte_offset < tracker->text_offset;
         byte_offset++) {
        const uint8_t byte = bytes[byte_offset];
        if ((byte & 0xC0) == 0x80) {
            continue;
        }
        tracker->input_offset -=
            sxml_xml_input_byte_count(tracker->input_encoding, byte);
        /* XML 1.0, section 2.11: the LF of CR LF ended no line of its own. */
        if (byte == '\r') {
            tracker->line_number -= 1;
            crosses_line_end = true;
        } else if (byte == '\n') {
            const bool follows_carriage_return =
                byte_offset > tracker->text_start_offset &&
                bytes[byte_offset - 1] == '\r';
            if (!follows_carriage_return) {
                tracker->line_number -= 1;
            }
            crosses_line_end = true;
        } else {
            tracker->column_number -= 1;
        }
    }
    if (crosses_line_end) {
        /* The column counts the codepoints since the last line end. */
        size_t line_start = text_offset;
        while (line_start > tracker->text_start_offset &&
               bytes[line_start - 1] != '\r' &&
               bytes[line_start - 1] != '\n') {
            line_start -= 1;
        }
        tracker->column_number = 1;
        for (size_t byte_offset = line_start; byte_offset < text_offset;
             byte_offset++) {
            if ((bytes[byte_offset] & 0xC0) != 0x80) {
                tracker->column_number += 1;
            }
        }
    }
    tracker->follows_carriage_return =
        text_offset > tracker->text_start_offset &&
        bytes[text_offset - 1] == '\r';
    tracker->text_offset = text_offset;
}

/*
 * The forward walk of sxml_xml_position_tracker_locate for a text that is
 * its input (UTF-8 and US-ASCII), a byte at a time without a call (the
 * walk passes every byte of the document once): a codepoint's lead byte
 * counts a column and the input bytes of the codepoint, a continuation
 * byte nothing, as in the general walk.
 */
static void
sxml_xml_position_tracker_advance_in_place(sxml_xml_position_tracker *tracker,
                                           size_t text_offset) {
    const size_t end_offset = text_offset < tracker->text.byte_count
                                  ? text_offset
                                  : tracker->text.byte_count;
    if (tracker->text_offset >= end_offset) {
        return;
    }
    const bool is_utf8 = tracker->input_encoding == SXML_XML_ENCODING_UTF8;
    const uint8_t *bytes = tracker->text.bytes;
    size_t input_offset = tracker->input_offset;
    uint32_t line_number = tracker->line_number;
    uint32_t column_number = tracker->column_number;
    bool follows_carriage_return = tracker->follows_carriage_return;
    for (size_t byte_offset = tracker->text_offset; byte_offset < end_offset;
         byte_offset++) {
        const uint8_t byte = bytes[byte_offset];
        if (byte >= 0x80) {
            if ((byte & 0xC0) != 0x80) {
                input_offset +=
                    is_utf8 ? sxml_xml_utf8_sequence_byte_count(byte) : 1;
                column_number += 1;
                follows_carriage_return = false;
            }
            continue;
        }
        input_offset += 1;
        if (byte == '\r') {
            /* XML 1.0, section 2.11: CR LF, CR and LF each end a line. */
            line_number += 1;
            column_number = 1;
            follows_carriage_return = true;
        } else if (byte == '\n') {
            if (!follows_carriage_return) {
                line_number += 1;
                column_number = 1;
            }
            follows_carriage_return = false;
        } else {
            column_number += 1;
            follows_carriage_return = false;
        }
    }
    tracker->input_offset = input_offset;
    tracker->text_offset = end_offset;
    tracker->line_number = line_number;
    tracker->column_number = column_number;
    tracker->follows_carriage_return = follows_carriage_return;
}

sxml_xml_position
sxml_xml_position_tracker_locate(sxml_xml_position_tracker *tracker,
                                 size_t text_offset) {
    if (text_offset < tracker->text_start_offset) {
        return (sxml_xml_position){
            .line_number = 1, .column_number = 1, .input_byte_offset = 0};
    }
    if (text_offset < tracker->text_offset) {
        /* Going back from where the tracker is costs the distance; going
           forward from the start costs text_offset: take the shorter. */
        if (tracker->text_offset - text_offset <=
            text_offset - tracker->text_start_offset) {
            sxml_xml_position_tracker_move_back(tracker, text_offset);
        } else {
            *tracker = sxml_xml_position_tracker_start(
                (sxml_xml_position_tracker_configuration){
                    .text = tracker->text,
                    .input_encoding = tracker->input_encoding,
                    .has_byte_order_mark = tracker->input_start_offset != 0});
        }
    }
    if (tracker->input_encoding == SXML_XML_ENCODING_UTF8 ||
        tracker->input_encoding == SXML_XML_ENCODING_US_ASCII) {
        sxml_xml_position_tracker_advance_in_place(tracker, text_offset);
    }
    while (tracker->text_offset < text_offset &&
           tracker->text_offset < tracker->text.byte_count) {
        const uint8_t byte = tracker->text.bytes[tracker->text_offset];
        tracker->text_offset += 1;
        if ((byte & 0xC0) == 0x80) {
            continue;
        }
        tracker->input_offset +=
            sxml_xml_input_byte_count(tracker->input_encoding, byte);
        /* XML 1.0, section 2.11: CR LF, CR and LF each end a line. */
        if (byte == '\r') {
            tracker->line_number += 1;
            tracker->column_number = 1;
            tracker->follows_carriage_return = true;
        } else if (byte == '\n') {
            if (!tracker->follows_carriage_return) {
                tracker->line_number += 1;
                tracker->column_number = 1;
            }
            tracker->follows_carriage_return = false;
        } else {
            tracker->column_number += 1;
            tracker->follows_carriage_return = false;
        }
    }
    return (sxml_xml_position){.line_number = tracker->line_number,
                               .column_number = tracker->column_number,
                               .input_byte_offset = tracker->input_offset};
}
