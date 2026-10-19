#pragma once

/*
 * Internals of the XML parser (include/sxml/xml.h; docs/DECISIONS.md D12,
 * D28, D51, D81), shared by its three parts:
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

/* ------------------------------------------------------ XML version */

/* The version of XML a document follows (XML 1.1 Second Edition, 4.3.4):
   1.1 when its XML declaration says version="1.1", else 1.0, as a
   document without a declaration is (XML 1.1, 2.8). */
typedef enum sxml_xml_version {
    SXML_XML_VERSION_1_0,
    SXML_XML_VERSION_1_1,
} sxml_xml_version;

/* What the XML declaration at the start of a decoded document says. */
typedef struct sxml_xml_declared_version {
    sxml_xml_version version;
    /* The offset just after the declaration's "?>"; 0 without a
       declaration of the expected shape. */
    size_t declaration_end_offset;
} sxml_xml_declared_version;

/*
 * Reads the version pseudo-attribute of the XML declaration at the start
 * of text, which is UTF-8 and may start with a byte order mark ([23]
 * XMLDecl, [24] VersionInfo). A declaration that does not have the
 * expected shape gives version 1.0; the grammar checks it properly later.
 */
sxml_xml_declared_version sxml_xml_read_declared_version(sxml_byte_view text);

/* What the text declaration ([77] TextDecl) or XML declaration at the
   start of an entity's decoded text says. */
typedef struct sxml_xml_declared_encoding {
    /* The encoding name; empty without a declaration or encoding. */
    sxml_byte_view encoding_name;
    /* The offset just after the declaration's "?>"; 0 without a
       declaration of the expected shape. */
    size_t declaration_end_offset;
} sxml_xml_declared_encoding;

/* Reads the declaration at the start of text (UTF-8, no byte order
   mark). The grammar checks a declaration properly where it parses one. */
sxml_xml_declared_encoding sxml_xml_read_declared_encoding(sxml_byte_view text);

/*
 * The byte offset in text (UTF-8, well-formed) of the first [2a]
 * RestrictedChar of XML 1.1, which a document must not contain literally
 * (XML 1.1, [1] document and 2.2), or text.byte_count when there is none.
 */
size_t sxml_xml_find_restricted_character(sxml_byte_view text);

/* True when text (UTF-8) has a NEL (U+0085) or a LINE SEPARATOR (U+2028),
   which XML 1.1 translates to LF (XML 1.1, 2.11). */
bool sxml_xml_has_version_1_1_line_end(sxml_byte_view text);

/*
 * Translates the line ends XML 1.1 adds to those of XML 1.0 in the bytes
 * of text from start_offset on, in place (XML 1.1, 2.11): CR NEL becomes
 * CR, which the parser then reads as one line end, NEL and LINE SEPARATOR
 * become LF, and a CR before a LINE SEPARATOR becomes LF, so that the two
 * stay two line ends. The text shrinks.
 */
void sxml_xml_translate_version_1_1_line_ends(sxml_xml_buffer *text,
                                              size_t start_offset);

/* ---------------------------------------------------- Characters */

/* [2] Char (XML 1.0 Fifth Edition). */
bool sxml_xml_codepoint_is_char(uint32_t codepoint);

/* [2] Char of XML 1.1: every codepoint but U+0000, the surrogates,
   U+FFFE and U+FFFF; as character references, the restricted characters
   too (XML 1.1, 2.2 and 4.1). */
bool sxml_xml_codepoint_is_version_1_1_char(uint32_t codepoint);

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

/* text without its leading and trailing [3] S characters (the
   whitespace-trimmed value of an attribute, XSLT 4.0, 2.4). */
sxml_byte_view sxml_xml_text_trim_whitespace(sxml_byte_view text);

/* True when name matches Namespaces in XML 1.0 [4] NCName: a [5] Name
   without a colon. */
bool sxml_xml_text_is_ncname(sxml_byte_view name);

/* True when every byte of text is a [3] S character; true for empty
   text. */
bool sxml_xml_text_is_whitespace(sxml_byte_view text);

/* Splits *rest at its first token, a run of non-[3] S bytes: *token gets
   it, *rest what follows it. False, and nothing changed, when *rest has
   only whitespace. */
bool sxml_xml_text_next_token(sxml_byte_view *rest, sxml_byte_view *token);

/* ---------------------------------------------------- Positions */

/*
 * Turns byte offsets in the UTF-8 text of a document into line, column
 * (in codepoints) and the byte offset in the original input. Moves
 * through the text from the last offset asked, so offsets asked in
 * increasing order cost linear time in all, and an earlier offset costs
 * its distance back (or a restart from the beginning, when that is
 * shorter), plus the start of its line when a line end lies between.
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
    /* Declared with an external identifier: read through the request's
       entity resolver when it has one, else not read (XML 1.0, 5.1). */
    SXML_XML_ENTITY_KIND_EXTERNAL_PARSED,
    /* Declared with NDATA. */
    SXML_XML_ENTITY_KIND_UNPARSED,
} sxml_xml_entity_kind;

/* Whether an external entity has been asked of the resolver yet. */
typedef enum sxml_xml_load_state {
    SXML_XML_LOAD_STATE_NOT_LOADED,
    /* Its text is in external_text. */
    SXML_XML_LOAD_STATE_LOADED,
    /* No resolver, or the resolver did not deliver it: not read. */
    SXML_XML_LOAD_STATE_UNAVAILABLE,
} sxml_xml_load_state;

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
    /* Unparsed entities: the notation NDATA names (VC: Notation
       Declared, 4.2.2). */
    sxml_byte_range notation_name;
    /* Declared in the external subset or in a parameter entity: a
       reference to it does not meet WFC: Entity Declared where that
       constraint applies (4.1). */
    bool is_declared_in_external_markup;
    /* The base URI of the entity in which the declaration occurs, against
       which the system identifier is resolved (4.2.2); empty for none. */
    sxml_byte_range base_uri;
    /* External entities: whether the text was read, the text decoded to
       UTF-8 with line ends normalized (2.11), where the content starts
       after a text declaration (4.3.1), and the URI it was found at. The
       text is released by the parser when the parse ends. */
    sxml_xml_load_state load_state;
    sxml_xml_buffer external_text;
    size_t content_offset;
    sxml_byte_range entity_uri;
    /* Set while the entity's replacement text is being expanded: a
       reference to it then is recursion (WFC: No Recursion, 4.1). */
    bool is_being_expanded;
    /* The parsed replacement text of a general entity referenced in
       content, made at the first reference and deleted by the parser when
       the parse ends; NULL before. */
    TSTree *syntax_tree;
} sxml_xml_entity;

/* The declared type of an attribute (XML 1.0, section 3.3.1). Every type
   but CDATA gets the extra normalization of 3.3.3. */
typedef enum sxml_xml_attribute_type {
    SXML_XML_ATTRIBUTE_TYPE_CDATA,
    SXML_XML_ATTRIBUTE_TYPE_ID,
    SXML_XML_ATTRIBUTE_TYPE_IDREF,
    SXML_XML_ATTRIBUTE_TYPE_IDREFS,
    SXML_XML_ATTRIBUTE_TYPE_ENTITY,
    SXML_XML_ATTRIBUTE_TYPE_ENTITIES,
    SXML_XML_ATTRIBUTE_TYPE_NMTOKEN,
    SXML_XML_ATTRIBUTE_TYPE_NMTOKENS,
    /* [58] NotationType: the names are in enumeration. */
    SXML_XML_ATTRIBUTE_TYPE_NOTATION,
    /* [59] Enumeration: the name tokens are in enumeration. */
    SXML_XML_ATTRIBUTE_TYPE_ENUMERATION,
} sxml_xml_attribute_type;

/* [60] DefaultDecl. */
typedef enum sxml_xml_default_kind {
    SXML_XML_DEFAULT_KIND_IMPLIED,
    SXML_XML_DEFAULT_KIND_REQUIRED,
    SXML_XML_DEFAULT_KIND_FIXED,
    SXML_XML_DEFAULT_KIND_VALUE,
} sxml_xml_default_kind;

/* One attribute definition (XML 1.0, section 3.3). */
typedef struct sxml_xml_attribute_definition {
    sxml_byte_range attribute_name;
    sxml_xml_attribute_type attribute_type;
    /* NOTATION and enumerated types: the names or name tokens, each
       followed by one space, in the DTD text. */
    sxml_byte_range enumeration;
    sxml_xml_default_kind default_kind;
    /* True for a default value (#FIXED or a plain value), false for
       #REQUIRED and #IMPLIED. */
    bool has_default_value;
    /* The normalized default value, in the DTD text. */
    sxml_byte_range default_value;
    /* Declared in the external subset or a parameter entity (VC:
       Standalone Document Declaration, 2.9). */
    bool is_declared_in_external_markup;
    /* The next definition of the same element, plus one; 0 at the end. */
    uint32_t next_definition;
} sxml_xml_attribute_definition;

/* [46] contentspec, by kind. */
typedef enum sxml_xml_content_kind {
    SXML_XML_CONTENT_KIND_EMPTY,
    SXML_XML_CONTENT_KIND_ANY,
    /* [51] Mixed: the particles are the element names allowed. */
    SXML_XML_CONTENT_KIND_MIXED,
    /* [47] children: the particles are the content model. */
    SXML_XML_CONTENT_KIND_CHILDREN,
} sxml_xml_content_kind;

/* One step of a content model written out in document order: groups open
   and end around their particles, as the model nests (3.2.1). */
typedef enum sxml_xml_particle_kind {
    SXML_XML_PARTICLE_KIND_SEQUENCE,
    SXML_XML_PARTICLE_KIND_CHOICE,
    SXML_XML_PARTICLE_KIND_GROUP_END,
    SXML_XML_PARTICLE_KIND_ELEMENT,
} sxml_xml_particle_kind;

typedef struct sxml_xml_particle {
    sxml_xml_particle_kind particle_kind;
    /* '?', '*', '+' or 0 for exactly once; of the group on its start. */
    uint8_t quantifier;
    /* ELEMENT: the element type's name, in the DTD text. */
    sxml_byte_range element_name;
} sxml_xml_particle;

/* An element type with an element declaration or attribute definitions. */
typedef struct sxml_xml_element_entry {
    sxml_byte_range element_name;
    /* Definition numbers (index plus one); 0 for none. */
    uint32_t first_definition;
    uint32_t last_definition;
    /* Named in a content model ([48] cp, [51] Mixed). */
    bool is_referred_to;
    /* Its [45] elementdecl, when it has one: the first wins. */
    bool is_declared;
    bool is_declared_in_external_markup;
    sxml_xml_content_kind content_kind;
    /* Its particles in sxml_xml_dtd.particles. */
    size_t first_particle;
    size_t particle_count;
} sxml_xml_element_entry;

/* One [82] NotationDecl. */
typedef struct sxml_xml_notation {
    sxml_byte_range notation_name;
    sxml_byte_range public_identifier;
    sxml_byte_range system_identifier;
} sxml_xml_notation;

/*
 * The declarations of one document: entities and attribute definitions,
 * with their names and values in one text buffer. Kept by the parser and
 * cleared for every document, so the memory is reused.
 */
typedef struct sxml_xml_dtd {
    sxml_xml_buffer text;
    sxml_xml_array entities;
    sxml_xml_array attribute_definitions;
    /* Elements with declarations or attribute definitions:
       sxml_xml_element_entry. */
    sxml_xml_array elements;
    /* The content models of element declarations: sxml_xml_particle. */
    sxml_xml_array particles;
    /* sxml_xml_notation, in declaration order. */
    sxml_xml_array notations;
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

/* Finds the entry of element_name, adding one without declarations when
   there is none, and writes its index. */
SXML_NODISCARD sxml_status sxml_xml_dtd_get_element(sxml_xml_dtd *dtd,
                                                    sxml_byte_view element_name,
                                                    uint32_t *element_index);

/* Finds the entry of element_name. */
sxml_xml_lookup sxml_xml_dtd_find_element(const sxml_xml_dtd *dtd,
                                          sxml_byte_view element_name);

/* The element entry at element_index. */
sxml_xml_element_entry *sxml_xml_dtd_element_at(const sxml_xml_dtd *dtd,
                                                uint32_t element_index);

/* The attribute definition numbered definition_number (from 1). */
const sxml_xml_attribute_definition *
sxml_xml_dtd_get_definition(const sxml_xml_dtd *dtd,
                            uint32_t definition_number);
