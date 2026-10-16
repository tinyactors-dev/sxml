#pragma once

/*
 * Internals of the XML parser (include/sxml/xml.h; docs/DECISIONS.md D12,
 * D28, D51, D80), shared by its three parts:
 *
 * - src/xml.c: the parser object, the walk over the concrete syntax tree
 *   of grammars/xml, the well-formedness and namespace checks the grammar
 *   leaves to it (grammars/xml/README.md, "What the tree builder checks"),
 *   and the tree builder events.
 * - src/xml_encoding.c: character encodings (detection, transcoding to
 *   UTF-8), the character classes of XML 1.0, and source positions.
 * - src/xml_entity.c: growable buffers and arrays over the parser's
 *   allocator, and the tables of the document type declaration: general
 *   and parameter entities, attribute definitions.
 *
 * Everything here allocates through the allocator stored in the buffer or
 * table, never through the tree-sitter runtime.
 */
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <tree_sitter/api.h>

/* ------------------------------------------------- Buffers and arrays */

/*
 * Bytes that grow through allocator. The bytes move when the buffer grows:
 * keep offsets (sxml_byte_range), not pointers, across appends. A zeroed
 * buffer with an allocator is empty and allocates nothing.
 */
typedef struct sxml_xml_buffer {
    sxml_allocator allocator;
    uint8_t *bytes;
    size_t byte_count;
    size_t byte_capacity;
} sxml_xml_buffer;

/* An empty buffer over allocator. */
sxml_xml_buffer sxml_xml_buffer_empty(sxml_allocator allocator);

/* Releases the bytes; the buffer is empty afterwards. */
void sxml_xml_buffer_release(sxml_xml_buffer *buffer);

/*
 * Makes room for additional_byte_count more bytes. Returns
 * SXML_STATUS_OUT_OF_MEMORY or SXML_STATUS_ARITHMETIC_OVERFLOW, leaving
 * the buffer as it was.
 */
SXML_NODISCARD sxml_status
sxml_xml_buffer_reserve(sxml_xml_buffer *buffer, size_t additional_byte_count);

/* Appends bytes, which must not point into buffer. Fails as reserve. */
SXML_NODISCARD sxml_status sxml_xml_buffer_append(sxml_xml_buffer *buffer,
                                                  sxml_byte_view bytes);

/* Appends one byte. Fails as reserve. */
SXML_NODISCARD sxml_status sxml_xml_buffer_append_byte(sxml_xml_buffer *buffer,
                                                       uint8_t byte);

/* Appends the UTF-8 encoding of codepoint, a Unicode scalar value. */
SXML_NODISCARD sxml_status
sxml_xml_buffer_append_codepoint(sxml_xml_buffer *buffer, uint32_t codepoint);

/* The bytes of range, which must lie within the buffer. */
sxml_byte_view sxml_xml_buffer_view(const sxml_xml_buffer *buffer,
                                    sxml_byte_range range);

/*
 * An array of fixed-size elements that grows through allocator, aligned
 * for any object type. Elements move when it grows: keep indices.
 */
typedef struct sxml_xml_array {
    sxml_allocator allocator;
    size_t element_byte_count;
    uint8_t *elements;
    size_t element_count;
    size_t element_capacity;
} sxml_xml_array;

/* An empty array of elements of element_byte_count bytes. */
typedef struct sxml_xml_array_configuration {
    sxml_allocator allocator;
    size_t element_byte_count;
} sxml_xml_array_configuration;

sxml_xml_array sxml_xml_array_empty(sxml_xml_array_configuration configuration);

/* Releases the elements; the array is empty afterwards. */
void sxml_xml_array_release(sxml_xml_array *array);

/*
 * Appends one zeroed element and writes its address to *element (valid
 * until the array next grows). Fails as sxml_xml_buffer_reserve.
 */
SXML_NODISCARD sxml_status sxml_xml_array_push(sxml_xml_array *array,
                                               void **element);

/* The element at element_index, which must be below element_count. */
void *sxml_xml_array_at(const sxml_xml_array *array, size_t element_index);

/* ---------------------------------------------- Character encodings */

/* The encodings the parser reads (XML 1.0, section 4.3.3; D51). */
typedef enum sxml_xml_encoding {
    SXML_XML_ENCODING_UTF8,
    SXML_XML_ENCODING_UTF16_LITTLE_ENDIAN,
    SXML_XML_ENCODING_UTF16_BIG_ENDIAN,
    SXML_XML_ENCODING_ISO_8859_1,
    SXML_XML_ENCODING_US_ASCII,
} sxml_xml_encoding;

/* What an encoding name names. */
typedef enum sxml_xml_encoding_family {
    /* Not a name sxml reads. */
    SXML_XML_ENCODING_FAMILY_UNSUPPORTED,
    SXML_XML_ENCODING_FAMILY_UTF8,
    /* "UTF-16": the byte order comes from the byte order mark. */
    SXML_XML_ENCODING_FAMILY_UTF16,
    SXML_XML_ENCODING_FAMILY_UTF16_LITTLE_ENDIAN,
    SXML_XML_ENCODING_FAMILY_UTF16_BIG_ENDIAN,
    SXML_XML_ENCODING_FAMILY_ISO_8859_1,
    SXML_XML_ENCODING_FAMILY_US_ASCII,
} sxml_xml_encoding_family;

/*
 * Classifies an encoding name, without regard to ASCII case, among the
 * IANA names and aliases of the encodings sxml reads.
 */
sxml_xml_encoding_family
sxml_xml_classify_encoding_name(sxml_byte_view encoding_name);

/* What the first bytes of a document say about its encoding. */
typedef struct sxml_xml_encoding_detection {
    /* The byte order mark that starts the input, if any. */
    bool has_utf8_byte_order_mark;
    bool has_utf16_byte_order_mark;
    /* The encoding to decode with. */
    sxml_xml_encoding encoding;
    /* The name in the encoding declaration, read from the bytes before
       decoding; empty when there is none or it cannot be read yet. */
    sxml_byte_view declared_encoding_name;
} sxml_xml_encoding_detection;

/* The input and the encoding named outside it. */
typedef struct sxml_xml_encoding_request {
    sxml_byte_view input;
    /* Empty to detect; otherwise takes precedence (XML 1.0, F.2). */
    sxml_byte_view encoding_name;
} sxml_xml_encoding_request;

/*
 * Detects the encoding as XML 1.0, Appendix F describes: byte order mark,
 * then the encoding declaration of an ASCII-compatible document, else
 * UTF-8. Returns SXML_STATUS_UNSUPPORTED_FEATURE for an encoding sxml does
 * not read (UCS-4, EBCDIC, any other declared or external name), leaving
 * *detection untouched.
 */
SXML_NODISCARD sxml_status
sxml_xml_detect_encoding(const sxml_xml_encoding_request *request,
                         sxml_xml_encoding_detection *detection);

/* Input to decode into a buffer. */
typedef struct sxml_xml_transcoding_request {
    sxml_byte_view input;
    sxml_xml_encoding encoding;
} sxml_xml_transcoding_request;

/* Where transcoding stopped on failure. */
typedef struct sxml_xml_transcoding_failure {
    /* Offset of the bad bytes in the input. */
    size_t input_byte_offset;
} sxml_xml_transcoding_failure;

/*
 * Decodes request->input (UTF-16 or ISO-8859-1; US-ASCII is only checked
 * by sxml_xml_check_ascii) into output as UTF-8, appending; a byte order
 * mark becomes U+FEFF. Returns SXML_STATUS_INVALID_UTF8 for UTF-16 that is
 * not well-formed (a lone surrogate, an odd byte count), filling *failure
 * and leaving what was decoded before in output, and the buffer's
 * failures.
 */
SXML_NODISCARD sxml_status sxml_xml_transcode(
    const sxml_xml_transcoding_request *request, sxml_xml_buffer *output,
    sxml_xml_transcoding_failure *failure);

/*
 * Checks that input is US-ASCII: every byte below 0x80. Returns
 * SXML_STATUS_MALFORMED_INPUT, filling *failure, when one is not
 * (XML 1.0, section 4.3.3: bytes not legal in the encoding are a fatal
 * error).
 */
SXML_NODISCARD sxml_status sxml_xml_check_ascii(
    sxml_byte_view input, sxml_xml_transcoding_failure *failure);

/* ---------------------------------------------------- Characters */

/* [2] Char (XML 1.0 Fifth Edition). */
bool sxml_xml_codepoint_is_char(uint32_t codepoint);

/* [4] NameStartChar. */
bool sxml_xml_codepoint_is_name_start(uint32_t codepoint);

/* [4a] NameChar. */
bool sxml_xml_codepoint_is_name(uint32_t codepoint);

/* [3] S: space, tab, carriage return, line feed. */
bool sxml_xml_byte_is_whitespace(uint8_t byte);

/*
 * Decodes the codepoint that starts at byte_offset in utf8, which must be
 * well-formed, and returns its encoded byte count (1 to 4).
 */
typedef struct sxml_xml_decoded_codepoint {
    uint32_t codepoint;
    size_t encoded_byte_count;
} sxml_xml_decoded_codepoint;

sxml_xml_decoded_codepoint sxml_xml_decode_codepoint(sxml_byte_view utf8,
                                                     size_t byte_offset);

/* True when name matches [5] Name. */
bool sxml_xml_text_is_name(sxml_byte_view name);

/* ---------------------------------------------------- Positions */

/*
 * Turns byte offsets in the UTF-8 text of a document into line, column
 * (in codepoints) and the byte offset in the original input. Advances
 * forward through the text, so offsets asked in increasing order cost
 * linear time in all; an earlier offset restarts from the beginning.
 * Line ends are LF, CR LF and lone CR (XML 1.0, section 2.11).
 */
typedef struct sxml_xml_position_tracker {
    sxml_byte_view text;
    sxml_xml_encoding input_encoding;
    /* Byte offset of the first character after the byte order mark, in
       the text and in the input. */
    size_t text_start_offset;
    size_t input_start_offset;
    /* Where the tracker is. */
    size_t text_offset;
    size_t input_offset;
    uint32_t line_number;
    uint32_t column_number;
    bool follows_carriage_return;
} sxml_xml_position_tracker;

/* The text, in which encoding its input was, and whether a byte order
   mark starts the input. */
typedef struct sxml_xml_position_tracker_configuration {
    sxml_byte_view text;
    sxml_xml_encoding input_encoding;
    bool has_byte_order_mark;
} sxml_xml_position_tracker_configuration;

sxml_xml_position_tracker sxml_xml_position_tracker_start(
    sxml_xml_position_tracker_configuration configuration);

/* The position of text_offset, which must be at most the text's length. */
typedef struct sxml_xml_position {
    uint32_t line_number;
    uint32_t column_number;
    size_t input_byte_offset;
} sxml_xml_position;

sxml_xml_position
sxml_xml_position_tracker_locate(sxml_xml_position_tracker *tracker,
                                 size_t text_offset);

/* ------------------------------------------------- DTD tables */

/* What kind of entity a declaration made (XML 1.0, section 4). */
typedef enum sxml_xml_entity_kind {
    /* Declared with a literal value: has replacement text. */
    SXML_XML_ENTITY_KIND_INTERNAL,
    /* Declared with an external identifier; not read (XML 1.0, 5.1). */
    SXML_XML_ENTITY_KIND_EXTERNAL_PARSED,
    /* Declared with NDATA. */
    SXML_XML_ENTITY_KIND_UNPARSED,
} sxml_xml_entity_kind;

/* One declared entity. Ranges are in the DTD text of sxml_xml_dtd. */
typedef struct sxml_xml_entity {
    sxml_xml_entity_kind entity_kind;
    bool is_parameter_entity;
    sxml_byte_range entity_name;
    /* Internal entities only. */
    sxml_byte_range replacement_text;
    /* External and unparsed entities, without the quotes. */
    sxml_byte_range system_identifier;
    sxml_byte_range public_identifier;
    /* Set while the entity's replacement text is being expanded: a
       reference to it then is recursion (WFC: No Recursion, 4.1). */
    bool is_being_expanded;
    /* The parsed replacement text of a general entity referenced in
       content, made at the first reference and deleted by the parser when
       the parse ends; NULL before. */
    TSTree *syntax_tree;
} sxml_xml_entity;

/* The declared type of an attribute, as far as normalization and the
   ID properties need it (XML 1.0, section 3.3.1). */
typedef enum sxml_xml_attribute_type {
    SXML_XML_ATTRIBUTE_TYPE_CDATA,
    SXML_XML_ATTRIBUTE_TYPE_ID,
    SXML_XML_ATTRIBUTE_TYPE_IDREF,
    SXML_XML_ATTRIBUTE_TYPE_IDREFS,
    /* ENTITY, ENTITIES, NMTOKEN, NMTOKENS, NOTATION, enumerations. */
    SXML_XML_ATTRIBUTE_TYPE_OTHER_TOKENIZED,
} sxml_xml_attribute_type;

/* One attribute definition (XML 1.0, section 3.3). */
typedef struct sxml_xml_attribute_definition {
    sxml_byte_range attribute_name;
    sxml_xml_attribute_type attribute_type;
    /* True for a default value (#FIXED or a plain value), false for
       #REQUIRED and #IMPLIED. */
    bool has_default_value;
    /* The normalized default value, in the DTD text. */
    sxml_byte_range default_value;
    /* The next definition of the same element, plus one; 0 at the end. */
    uint32_t next_definition;
} sxml_xml_attribute_definition;

/*
 * The declarations of one document: entities and attribute definitions,
 * with their names and values in one text buffer. Kept by the parser and
 * cleared for every document, so the memory is reused.
 */
typedef struct sxml_xml_dtd {
    sxml_xml_buffer text;
    sxml_xml_array entities;
    sxml_xml_array attribute_definitions;
    /* Elements with attribute definitions: sxml_xml_element_entry. */
    sxml_xml_array elements;
    /* Hash indices of names: slots of entry index plus one. */
    sxml_xml_array general_entity_slots;
    sxml_xml_array parameter_entity_slots;
    sxml_xml_array element_slots;
    size_t general_entity_count;
    size_t parameter_entity_count;
} sxml_xml_dtd;

sxml_xml_dtd sxml_xml_dtd_empty(sxml_allocator allocator);

/* Forgets every declaration, keeping the memory. */
void sxml_xml_dtd_clear(sxml_xml_dtd *dtd);

void sxml_xml_dtd_release(sxml_xml_dtd *dtd);

/* Appends text to the DTD text and writes where it went. */
SXML_NODISCARD sxml_status sxml_xml_dtd_store_text(sxml_xml_dtd *dtd,
                                                   sxml_byte_view text,
                                                   sxml_byte_range *range);

/* The bytes of range in the DTD text. */
sxml_byte_view sxml_xml_dtd_text(const sxml_xml_dtd *dtd,
                                 sxml_byte_range range);

/* An entity name and which kind of entity it names. */
typedef struct sxml_xml_entity_key {
    sxml_byte_view entity_name;
    bool is_parameter_entity;
} sxml_xml_entity_key;

/* The outcome of a lookup in a DTD table. */
typedef struct sxml_xml_lookup {
    bool is_found;
    uint32_t entry_index;
} sxml_xml_lookup;

/* Finds an entity. */
sxml_xml_lookup sxml_xml_dtd_find_entity(const sxml_xml_dtd *dtd,
                                         sxml_xml_entity_key key);

/*
 * Adds entity, whose ranges are in the DTD text, unless an entity of that
 * name and kind exists: the first declaration wins (XML 1.0, section
 * 4.2). Writes whether it was added and its index.
 */
SXML_NODISCARD sxml_status sxml_xml_dtd_add_entity(
    sxml_xml_dtd *dtd, const sxml_xml_entity *entity, sxml_xml_lookup *added);

/* The entity at entry_index. */
sxml_xml_entity *sxml_xml_dtd_get_entity(const sxml_xml_dtd *dtd,
                                         uint32_t entry_index);

/* An attribute definition to add, for the element element_name. */
typedef struct sxml_xml_attribute_definition_request {
    sxml_byte_view element_name;
    sxml_xml_attribute_definition definition;
} sxml_xml_attribute_definition_request;

/*
 * Adds an attribute definition unless the element already has one for
 * that attribute name: the first definition wins (XML 1.0, section 3.3).
 * The definition's ranges are in the DTD text. Writes whether it was
 * added.
 */
/* Whether an addition happened. */
typedef struct sxml_xml_addition {
    bool is_added;
} sxml_xml_addition;

SXML_NODISCARD sxml_status sxml_xml_dtd_add_attribute_definition(
    sxml_xml_dtd *dtd, const sxml_xml_attribute_definition_request *request,
    sxml_xml_addition *addition);

/*
 * The first attribute definition of element_name, plus one; 0 when it has
 * none. Follow next_definition for the rest.
 */
uint32_t sxml_xml_dtd_first_definition(const sxml_xml_dtd *dtd,
                                       sxml_byte_view element_name);

/* The attribute definition numbered definition_number (from 1). */
const sxml_xml_attribute_definition *
sxml_xml_dtd_get_definition(const sxml_xml_dtd *dtd,
                            uint32_t definition_number);
