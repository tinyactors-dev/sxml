/*
 * Where the XML parser cuts a document into document segments
 * (src/xml_internal.h, glossary `document_segment`, D222).
 *
 * Golden file: src/xml_encoding.c (byte scans over a decoded text that
 * return offsets).
 *
 * The parser does not hold the concrete syntax tree of the whole
 * document: it parses the head (the prolog and the start tag of the
 * document element), then one content segment at a time, walking each
 * tree and freeing it before the next. The scans here only find where a
 * segment may end; they check nothing. They follow the delimiters of
 * XML 1.0 (Fifth Edition) so that for a well-formed document every cut
 * lies between two content items: just before a '<' or an '&', or just
 * after the delimiter that ends markup. No token of the grammar reaches
 * across such a cut, so the segments' tokens are the whole text's (no
 * "]]>" in character data and no CR LF is cut apart). For text that is not
 * well-formed a cut may fall inside a construct; the segment before it
 * then ends in an unfinished construct, which the grammar refuses, so a
 * segmented parse accepts exactly what a parse of the whole text would.
 */
#include "xml_internal.h"

#include <string.h>

/* The offset of the first occurrence of delimiter at or after
   byte_offset, or text.byte_count when there is none. */
typedef struct sxml_xml_delimiter_search {
    sxml_byte_view delimiter;
    size_t byte_offset;
} sxml_xml_delimiter_search;

static size_t sxml_xml_find_delimiter(sxml_byte_view text,
                                      sxml_xml_delimiter_search search) {
    const size_t delimiter_byte_count = search.delimiter.byte_count;
    size_t byte_offset = search.byte_offset;
    while (byte_offset + delimiter_byte_count <= text.byte_count) {
        const uint8_t *first = memchr(text.bytes + byte_offset,
                                      search.delimiter.bytes[0],
                                      text.byte_count - byte_offset);
        if (first == NULL) {
            break;
        }
        byte_offset = (size_t)(first - text.bytes);
        if (byte_offset + delimiter_byte_count > text.byte_count) {
            break;
        }
        if (memcmp(first, search.delimiter.bytes, delimiter_byte_count) == 0) {
            return byte_offset;
        }
        byte_offset += 1;
    }
    return text.byte_count;
}

/* The offset just after delimiter, searched from byte_offset; the end of
   the text when it does not occur. */
static size_t sxml_xml_skip_past(sxml_byte_view text,
                                 sxml_xml_delimiter_search search) {
    const size_t found = sxml_xml_find_delimiter(text, search);
    return found == text.byte_count ? found
                                    : found + search.delimiter.byte_count;
}

static bool sxml_xml_text_starts_with(sxml_byte_view text, size_t byte_offset,
                                      sxml_byte_view prefix) {
    return byte_offset + prefix.byte_count <= text.byte_count &&
           memcmp(text.bytes + byte_offset, prefix.bytes, prefix.byte_count) ==
               0;
}

static const sxml_byte_view sxml_xml_comment_start =
    SXML_BYTE_VIEW_INITIALIZER("<!--");
static const sxml_byte_view sxml_xml_comment_end =
    SXML_BYTE_VIEW_INITIALIZER("-->");
static const sxml_byte_view sxml_xml_cdata_start =
    SXML_BYTE_VIEW_INITIALIZER("<![CDATA[");
static const sxml_byte_view sxml_xml_cdata_end =
    SXML_BYTE_VIEW_INITIALIZER("]]>");
static const sxml_byte_view sxml_xml_processing_instruction_start =
    SXML_BYTE_VIEW_INITIALIZER("<?");
static const sxml_byte_view sxml_xml_processing_instruction_end =
    SXML_BYTE_VIEW_INITIALIZER("?>");
static const sxml_byte_view sxml_xml_declaration_start =
    SXML_BYTE_VIEW_INITIALIZER("<!");

/*
 * The end of the tag that starts at byte_offset ([40] STag, [42] ETag,
 * [44] EmptyElemTag): just after its '>', which is the first one outside
 * a quoted attribute value ([10] AttValue). No '<' stands in a tag, not
 * even in an attribute value, so a '<' ends the scan there.
 */
static size_t sxml_xml_skip_tag(sxml_byte_view text, size_t byte_offset) {
    uint8_t quote = 0;
    for (size_t byte_index = byte_offset + 1; byte_index < text.byte_count;
         byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        if (byte == '<') {
            return byte_index;
        }
        if (quote != 0) {
            quote = byte == quote ? 0 : quote;
        } else if (byte == '"' || byte == '\'') {
            quote = byte;
        } else if (byte == '>') {
            return byte_index + 1;
        }
    }
    return text.byte_count;
}

/*
 * The end of the markup declaration at byte_offset ([28] doctypedecl, the
 * only one in a prolog): its '>' outside quoted literals, comments,
 * processing instructions and the brackets of the internal subset, where
 * the declarations of [28b] intSubset have '>' of their own.
 */
static size_t sxml_xml_skip_declaration(sxml_byte_view text,
                                        size_t byte_offset) {
    size_t bracket_depth = 0;
    size_t byte_index = byte_offset + sxml_xml_declaration_start.byte_count;
    while (byte_index < text.byte_count) {
        const uint8_t byte = text.bytes[byte_index];
        if (byte == '"' || byte == '\'') {
            const uint8_t quote[1] = {byte};
            byte_index = sxml_xml_skip_past(
                text, (sxml_xml_delimiter_search){
                          .delimiter = {.bytes = quote, .byte_count = 1},
                          .byte_offset = byte_index + 1});
        } else if (sxml_xml_text_starts_with(text, byte_index,
                                             sxml_xml_comment_start)) {
            byte_index = sxml_xml_skip_past(
                text, (sxml_xml_delimiter_search){
                          .delimiter = sxml_xml_comment_end,
                          .byte_offset = byte_index + sxml_xml_comment_start.byte_count});
        } else if (sxml_xml_text_starts_with(
                       text, byte_index, sxml_xml_processing_instruction_start)) {
            byte_index = sxml_xml_skip_past(
                text,
                (sxml_xml_delimiter_search){
                    .delimiter = sxml_xml_processing_instruction_end,
                    .byte_offset = byte_index +
                                   sxml_xml_processing_instruction_start.byte_count});
        } else if (byte == '[') {
            bracket_depth += 1;
            byte_index += 1;
        } else if (byte == ']') {
            bracket_depth = bracket_depth == 0 ? 0 : bracket_depth - 1;
            byte_index += 1;
        } else if (byte == '>' && bracket_depth == 0) {
            return byte_index + 1;
        } else {
            byte_index += 1;
        }
    }
    return text.byte_count;
}

/*
 * The end of the content item that starts at byte_offset, a '<' or the
 * start of character data: a comment, a CDATA section, a processing
 * instruction, a tag, or character data up to the next '<' or '&' (a
 * reference counts as the start of character data: references and text
 * may be cut apart, the builder joins adjacent text).
 */
static size_t sxml_xml_skip_content_item(sxml_byte_view text,
                                         size_t byte_offset) {
    if (text.bytes[byte_offset] != '<') {
        /* The first '<', then the first '&' before it (memchr is faster
           than a loop over the bytes). */
        const size_t start = byte_offset + 1;
        const uint8_t *less_than =
            start < text.byte_count
                ? memchr(text.bytes + start, '<', text.byte_count - start)
                : NULL;
        const size_t end =
            less_than == NULL ? text.byte_count : (size_t)(less_than - text.bytes);
        const uint8_t *ampersand =
            start < end ? memchr(text.bytes + start, '&', end - start) : NULL;
        return ampersand == NULL ? end : (size_t)(ampersand - text.bytes);
    }
    if (sxml_xml_text_starts_with(text, byte_offset, sxml_xml_comment_start)) {
        return sxml_xml_skip_past(
            text, (sxml_xml_delimiter_search){
                      .delimiter = sxml_xml_comment_end,
                      .byte_offset = byte_offset + sxml_xml_comment_start.byte_count});
    }
    if (sxml_xml_text_starts_with(text, byte_offset, sxml_xml_cdata_start)) {
        return sxml_xml_skip_past(
            text, (sxml_xml_delimiter_search){
                      .delimiter = sxml_xml_cdata_end,
                      .byte_offset = byte_offset + sxml_xml_cdata_start.byte_count});
    }
    if (sxml_xml_text_starts_with(text, byte_offset,
                                  sxml_xml_processing_instruction_start)) {
        return sxml_xml_skip_past(
            text,
            (sxml_xml_delimiter_search){
                .delimiter = sxml_xml_processing_instruction_end,
                .byte_offset =
                    byte_offset + sxml_xml_processing_instruction_start.byte_count});
    }
    return sxml_xml_skip_tag(text, byte_offset);
}

static bool sxml_xml_is_space_byte(uint8_t byte) {
    return byte == 0x20 || byte == 0x09 || byte == 0x0D || byte == 0x0A;
}

size_t sxml_xml_find_head_end(sxml_byte_view text) {
    static const uint8_t byte_order_mark[] = {0xEF, 0xBB, 0xBF};
    size_t byte_offset = 0;
    if (sxml_xml_text_starts_with(
            text, 0,
            (sxml_byte_view){.bytes = byte_order_mark,
                             .byte_count = sizeof byte_order_mark})) {
        byte_offset = sizeof byte_order_mark;
    }
    /* [22] prolog ::= XMLDecl? Misc* (doctypedecl Misc*)? */
    while (byte_offset < text.byte_count) {
        const uint8_t byte = text.bytes[byte_offset];
        if (sxml_xml_is_space_byte(byte)) {
            byte_offset += 1;
        } else if (byte != '<') {
            /* Not a prolog: the head ends at the next markup, and its
               parse reports the text. */
            return sxml_xml_skip_content_item(text, byte_offset);
        } else if (sxml_xml_text_starts_with(text, byte_offset,
                                             sxml_xml_comment_start) ||
                   sxml_xml_text_starts_with(
                       text, byte_offset, sxml_xml_processing_instruction_start)) {
            byte_offset = sxml_xml_skip_content_item(text, byte_offset);
        } else if (sxml_xml_text_starts_with(text, byte_offset,
                                             sxml_xml_declaration_start)) {
            byte_offset = sxml_xml_skip_declaration(text, byte_offset);
        } else {
            /* [39] element: the start tag of the document element. */
            return sxml_xml_skip_tag(text, byte_offset);
        }
    }
    return text.byte_count;
}

size_t sxml_xml_find_segment_end(const sxml_xml_segment_request *request) {
    const sxml_byte_view text = request->text;
    size_t byte_offset = request->start_offset;
    const size_t target_offset =
        request->target_byte_count > text.byte_count - byte_offset
            ? text.byte_count
            : byte_offset + request->target_byte_count;
    /* The first item always belongs to the segment, however long. */
    if (byte_offset < text.byte_count) {
        byte_offset = sxml_xml_skip_content_item(text, byte_offset);
    }
    while (byte_offset < target_offset) {
        byte_offset = sxml_xml_skip_content_item(text, byte_offset);
    }
    return byte_offset;
}
