#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/serialize.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "unicode_internal.h"

/*
 * The parts of the serializer (include/sxml/serialize.h; Serialization
 * 4.0; docs/DECISIONS.md D82, D106, D120b). Internal to sxml.
 *
 * - src/serialize.c: the output stream and buffer, the checks of the
 *   parameters every method shares, the character pipeline of section 4
 *   "Phases of Serialization" (character maps, Unicode normalization,
 *   escaping, line endings, encoding) behind sxml_serializer, sequence
 *   normalization (section 2), the adaptive method (section 11) and the
 *   dispatch by method.
 * - src/serialize_xml.c: the XML, XHTML and HTML methods (sections 5, 6
 *   and 7) as one node writer, and the node writer the adaptive method
 *   uses; src/serialize_html.c has what HTML adds (void elements, boolean
 *   attributes, raw text elements, processing instructions).
 * - src/serialize_text.c: the text and CSV methods (sections 8 and 10).
 * - src/serialize_json.c: the JSON method (section 9).
 * - src/serialize_parameters.c: sxml_serialization_parameter_reader.
 * Every method writes through the one character pipeline; the escape
 * style says how text is escaped for the method (D128).
 *
 * No recursion (D71): subtrees are written by walking node indices in
 * document order with a stack of open elements in heap memory; arrays,
 * maps and sequences inside sequences use explicit frames.
 */

/* Bytes collected before a write to the output stream. */
#define SXML_SERIALIZER_STAGING_BYTE_COUNT 512U

/* How characters are escaped (Serialization 4.0, section 4, rule e). */
typedef enum sxml_escape_style {
    /* The XML and XHTML methods (section 5): &amp; &lt; &gt;, &quot; and
       whitespace references in attributes, references for control
       characters, NEL and LINE SEPARATOR. */
    SXML_ESCAPE_STYLE_XML,
    /* Canonical XML 1.1, section 2.3 (parameter canonical): &amp; &lt;
       &gt; &#xD; in text; &amp; &lt; &quot; &#x9; &#xA; &#xD; in
       attributes; nothing else. */
    SXML_ESCAPE_STYLE_CANONICAL,
    /* The HTML method (section 7.2, 7.3): as XML, except that "<" and an
       "&" before "{" stay as they are in attribute values; before HTML5
       control characters are SERE0014. */
    SXML_ESCAPE_STYLE_HTML,
    /* The text method (section 8): nothing is escaped; a character the
       encoding cannot represent is SERE0008. */
    SXML_ESCAPE_STYLE_NONE,
    /* JSON strings (section 9.1): \" \\ \b \f \n \r \t, \uHHHH for
       other controls and characters the encoding cannot represent, \/
       with escape-solidus; RFC 8785's minimal escaping when canonical. */
    SXML_ESCAPE_STYLE_JSON,
    /* CSV fields (section 10.2): the quote character doubled, line
       endings in the field replaced by the line ending; nothing else. */
    SXML_ESCAPE_STYLE_CSV,
} sxml_escape_style;

/* Where characters are written (Serialization 4.0, section 4). */
typedef enum sxml_character_context {
    /* Markup: names, comments, processing instructions, the adaptive
       method's text. Nothing is escaped or mapped; a character the
       encoding cannot represent is SERE0008. */
    SXML_CHARACTER_CONTEXT_MARKUP,
    /* Content of a text node: character maps, normalization, escaping. */
    SXML_CHARACTER_CONTEXT_TEXT,
    /* An attribute value in double quotes. */
    SXML_CHARACTER_CONTEXT_ATTRIBUTE,
    /* An attribute value after URI escaping (section 4, rule a): escaped,
       not mapped, not normalized again. */
    SXML_CHARACTER_CONTEXT_URI_ATTRIBUTE,
    /* The inside of CDATA sections for one text node (section 5.1.8):
       normalized, not mapped; "]]>" and characters the encoding cannot
       represent close the section around a reference. */
    SXML_CHARACTER_CONTEXT_CDATA_SECTION,
    /* Raw text (glossary): text the method writes without escaping
       whatever the escape style, the content of HTML script and style
       elements (section 7.1): mapped and normalized; a character the
       encoding cannot represent is SERE0008. */
    SXML_CHARACTER_CONTEXT_RAW_TEXT,
    /* A text node whose output escaping is disabled (glossary
       disable_output_escaping; XSLT 4.0, section 26.1.8): mapped and
       normalized, not escaped; a character the encoding cannot represent
       is written as a character reference, since the request "is ignored
       in respect of that character" (D188). */
    SXML_CHARACTER_CONTEXT_UNESCAPED_TEXT,
} sxml_character_context;

/* Text to write and where it goes. */
typedef struct sxml_character_data {
    /* Well-formed UTF-8. Borrowed for the call. */
    sxml_byte_view text;
    sxml_character_context character_context;
} sxml_character_data;

/* A frame of the adaptive method's stack (glossary serialization_frame). */
typedef struct sxml_serialization_frame sxml_serialization_frame;

/*
 * The state of one serialization: where the bytes go, the resolved
 * parameters, and the character pipeline. Lives on the C stack of
 * sxml_serialize_sequence; its memory comes from the item heap, so it
 * counts against the heap's limit.
 */
typedef struct sxml_serializer {
    sxml_item_heap *item_heap;
    const sxml_name_pool *name_pool;
    const sxml_type_registry *registry;
    sxml_allocator allocator;
    /* May be NULL. */
    sxml_error_record *error_record;
    /* Borrowed for the serialization. */
    const sxml_serialization_parameters *parameters;
    sxml_output_stream output_stream;
    /* ---- The character pipeline. */
    sxml_character_encoding character_encoding;
    sxml_escape_style escape_style;
    /* XML and XHTML with version 1.0: a C0 control character in text or
       an attribute is SERE0006 (Serialization 4.0, 5.1.2). */
    bool is_xml_version_1_0;
    /* HTML before HTML5: control characters are SERE0014 (7.3). */
    bool is_html_before_5;
    /* JSON: escape-solidus; canonical (RFC 8785 escaping). */
    bool escapes_solidus;
    bool is_canonical_json;
    /* CSV: the quote character. */
    uint32_t csv_quote_codepoint;
    /* What each U+000A becomes (parameter line-ending). */
    sxml_byte_view line_ending_text;
    /* Whether the text of text nodes and attributes is normalized, and
       to which form; fully-normalized is NFC plus the check of
       SERE0012. */
    bool is_normalizing;
    bool is_fully_normalized;
    sxml_unicode_normalization_form normalization_form;
    /* Whether use-character-maps applies to the method. */
    bool applies_character_maps;
    /* The pairs of use-character-maps sorted by codepoint, in heap
       memory; NULL when there are none. */
    sxml_character_mapping *sorted_mappings;
    size_t mapping_count;
    uint8_t staged_bytes[SXML_SERIALIZER_STAGING_BYTE_COUNT];
    size_t staged_byte_count;
    /* ---- The adaptive method. */
    sxml_byte_view item_separator;
    sxml_serialization_frame *frames;
    size_t frame_count;
    size_t frame_capacity;
} sxml_serializer;

/* The answer of a yes-or-no question a fallible function writes (so no
   output parameter is a bare bool). */
typedef struct sxml_answer {
    bool is_true;
} sxml_answer;

/* Makes room for one more element of a stack in heap memory. */
typedef struct sxml_serializer_growth {
    void **elements;
    size_t *element_capacity;
    size_t element_count;
    size_t element_byte_count;
    size_t alignment;
} sxml_serializer_growth;

/* Grows *growth->elements geometrically when it is full; unchanged on
   failure. */
SXML_NODISCARD sxml_status sxml_serializer_grow(sxml_allocator allocator,
                                                const sxml_serializer_growth *growth);

/* A serialization error to raise: the local name of a code in the
   err namespace and a static description. */
typedef struct sxml_serialization_error {
    sxml_byte_view error_local_name;
    sxml_byte_view description;
} sxml_serialization_error;

/* Fills the error record, if any, and returns SXML_STATUS_DYNAMIC_ERROR. */
SXML_NODISCARD sxml_status sxml_serializer_raise_error(sxml_serializer *serializer,
                                                       sxml_serialization_error error);

/*
 * Writes character_data through the pipeline: character maps, Unicode
 * normalization, escaping by the serializer's escape style, line endings,
 * encoding (Serialization 4.0, section 4). Raises SERE0008 for a
 * character the encoding cannot represent where no reference is allowed,
 * SERE0012 for a text that starts with a composing character under
 * fully-normalized. Returns the output stream's and the heap's failures.
 */
SXML_NODISCARD sxml_status sxml_serializer_write_character_data(
    sxml_serializer *serializer, const sxml_character_data *character_data);

/* Writes markup (sxml_character_data with SXML_CHARACTER_CONTEXT_MARKUP). */
SXML_NODISCARD sxml_status sxml_serializer_write_markup(sxml_serializer *serializer,
                                                        sxml_byte_view markup);

/* Writes the staged bytes to the output stream. */
SXML_NODISCARD sxml_status sxml_serializer_flush(sxml_serializer *serializer);

/* The name of the serializer's encoding for an XML declaration or a meta
   element ("UTF-8", "UTF-16", "US-ASCII", "ISO-8859-1"). */
sxml_byte_view sxml_serializer_get_encoding_name(const sxml_serializer *serializer);

/* Bytes an atomic item's text may take without heap memory (the cast
   module's canonical text capacity, src/cast_internal.h). */
#define SXML_ATOMIC_TEXT_INLINE_CAPACITY 96U

/* The lexical form of an atomic item, its cast to xs:string (F&O 4.0,
   19.1.2), as sequence normalization, JSON strings and CSV fields write
   it. Used in place: text may point into inline_bytes. */
typedef struct sxml_atomic_text {
    sxml_byte_view text;
    /* Heap memory to release, or NULL. */
    void *memory;
    uint8_t inline_bytes[SXML_ATOMIC_TEXT_INLINE_CAPACITY];
} sxml_atomic_text;

/* Fills *atomic_text for item; release it with
   sxml_serializer_release_atomic_text, also on failure. */
SXML_NODISCARD sxml_status sxml_serializer_get_atomic_text(sxml_serializer *serializer,
                                                           sxml_item item,
                                                           sxml_atomic_text *atomic_text);

void sxml_serializer_release_atomic_text(sxml_serializer *serializer,
                                         sxml_atomic_text *atomic_text);

/* ------------------------------------------------ Sequence normalization */

/* What the sequence normalizer gives next. */
typedef enum sxml_normalized_event_kind {
    /* No more children of the result tree. */
    SXML_NORMALIZED_EVENT_KIND_END,
    /* A text node of the result tree: merged, never empty. */
    SXML_NORMALIZED_EVENT_KIND_TEXT,
    /* An element, comment or processing instruction to copy as a child
       of the result tree; or a text node whose output escaping is
       disabled, kept apart from the merged text (D188). */
    SXML_NORMALIZED_EVENT_KIND_NODE,
} sxml_normalized_event_kind;

/* One child of the result tree. */
typedef struct sxml_normalized_event {
    sxml_normalized_event_kind event_kind;
    /* For TEXT: borrowed from the normalizer until its next call. */
    sxml_byte_view text;
    /* For NODE. */
    sxml_node_reference node;
} sxml_normalized_event;

/* A frame of the normalizer: a sequence or an array whose items are
   flattened. */
typedef struct sxml_normalizer_frame sxml_normalizer_frame;

/*
 * Turns the sequence to serialize into the children of the result tree,
 * one at a time (Serialization 4.0, section 2 "Sequence Normalization"):
 * JNodes become their values, arrays are flattened, atomic items become
 * text (with the item separator, or a space between adjacent atomic
 * items), document nodes are replaced by their children, adjacent text is
 * merged and empty text dropped. Raises SENR0001 for an attribute or
 * namespace node, a map or another function item. A plain value; start
 * one with sxml_sequence_normalizer_start.
 */
typedef struct sxml_sequence_normalizer {
    sxml_serializer *serializer;
    /* The input, borrowed. */
    sxml_sequence value;
    sxml_normalizer_frame *frames;
    size_t frame_count;
    size_t frame_capacity;
    /* Number of items of S3 (the flattened sequence) seen so far. */
    uint64_t leaf_count;
    bool was_previous_leaf_atomic;
    /* A document whose children are being given. */
    bool is_in_document;
    sxml_tree *document_tree;
    bool has_next_child;
    sxml_node_index next_child;
    /* A node to give after the pending text. */
    bool has_pending_node;
    sxml_node_reference pending_node;
    /* Text of the result tree being merged, in heap memory. */
    uint8_t *text_bytes;
    size_t text_byte_count;
    size_t text_byte_capacity;
    bool has_given_text;
} sxml_sequence_normalizer;

/* Starts normalizer over value (borrowed until it is finished). */
void sxml_sequence_normalizer_start(sxml_serializer *serializer, sxml_sequence value,
                                    sxml_sequence_normalizer *normalizer);

/* Writes the next child of the result tree to *event. Returns SENR0001
   errors, the sequence's and the heap's failures. */
SXML_NODISCARD sxml_status sxml_sequence_normalizer_next(sxml_sequence_normalizer *normalizer,
                                                         sxml_normalized_event *event);

/* Releases the normalizer's memory. */
void sxml_sequence_normalizer_finish(sxml_sequence_normalizer *normalizer);

/* -------------------------------------------- XML, XHTML and HTML methods */

/*
 * Serializes value with the XML, XHTML or HTML method, as the
 * serializer's parameters say (Serialization 4.0, sections 5, 6 and 7):
 * sequence normalization, the XML declaration, the document type
 * declaration, the result tree. The parameters were checked by
 * sxml_serialize_sequence; raises SEPM0004, SEPM0009, SEPM0010, SESU0013,
 * SERE0003, SERE0015, SERE0024 and the errors of the pipeline.
 */
SXML_NODISCARD sxml_status sxml_serialize_xml_method(sxml_serializer *serializer,
                                                     sxml_sequence value);

/*
 * Writes one node as the adaptive method does (Serialization 4.0, section
 * 11): a document or element subtree with the XML method's rules and
 * defaults (no declaration, no indentation), each element declaring the
 * namespaces its written parent lacks; a text node escaped; an attribute
 * as name="value"; a namespace node as xmlns:prefix="uri"; comments and
 * processing instructions as markup.
 */
SXML_NODISCARD sxml_status sxml_serializer_write_adaptive_node(sxml_serializer *serializer,
                                                               sxml_node_reference node);

/* ------------------------------------------------------ The HTML method */

/* The HTML version a serialization requests (Serialization 4.0, section
   7: html-version, else version, else 5.0): whether it is 5.0 or more. */
bool sxml_serializer_requests_html5(const sxml_serialization_parameters *parameters);

/* Whether an element serialized as an HTML element, named local_name
   (compared without regard to case), has an empty content model: the
   void elements of HTML5, or the empty elements of HTML 4 (section 7.1). */
typedef struct sxml_html_element_query {
    sxml_byte_view local_name;
    bool is_html5;
} sxml_html_element_query;

bool sxml_html_element_is_void(sxml_html_element_query query);

/* Whether the text and attributes below an HTML element named local_name
   are raw text: script and style (section 7.1). */
bool sxml_html_element_has_raw_text(sxml_byte_view local_name);

/* Whether an attribute in no namespace of an HTML element is written in
   minimized form: a boolean attribute of HTML 4 or HTML5 whose value is
   its name, without regard to case (section 7.2). */
typedef struct sxml_html_attribute_query {
    sxml_byte_view attribute_name;
    sxml_byte_view value;
} sxml_html_attribute_query;

bool sxml_html_attribute_is_minimized(sxml_html_attribute_query query);

/* Writes a processing instruction by the HTML method (section 7.4):
   "<?target data>" before HTML5 (SERE0015 for ">" in it), and with HTML5
   the comment "<!--?target data?-->" with "--" split by a space. */
typedef struct sxml_html_processing_instruction {
    sxml_byte_view target;
    sxml_byte_view data;
    bool is_html5;
} sxml_html_processing_instruction;

SXML_NODISCARD sxml_status sxml_serializer_write_html_processing_instruction(
    sxml_serializer *serializer, const sxml_html_processing_instruction *instruction);

/* -------------------------------------------- The text and CSV methods */

/* Serializes value with the text method (Serialization 4.0, section 8):
   the string value of the result tree, unescaped. */
SXML_NODISCARD sxml_status sxml_serialize_text_method(sxml_serializer *serializer,
                                                      sxml_sequence value);

/* Serializes value with the CSV method (section 10): a parsed CSV
   structure or a sequence of arrays of atomic items; SERE0025 for other
   input. */
SXML_NODISCARD sxml_status sxml_serialize_csv_method(sxml_serializer *serializer,
                                                     sxml_sequence value);

/* ---------------------------------------------------- The JSON method */

/* Serializes value with the JSON method (Serialization 4.0, section 9);
   raises SERE0020, SERE0021, SERE0022 and SERE0023. */
SXML_NODISCARD sxml_status sxml_serialize_json_method(sxml_serializer *serializer,
                                                      sxml_sequence value);

/*
 * Serializes node by json-node-output-method into a string and writes it
 * as text of the serializer (a sub-serialization, glossary; section 9.1):
 * with omit-xml-declaration yes and no other parameters, or with all the
 * serializer's parameters when canonical. The sub-serialization never
 * uses the JSON method, so nothing recurses.
 */
SXML_NODISCARD sxml_status sxml_serializer_write_node_as_string(sxml_serializer *serializer,
                                                                sxml_node_reference node);

/* ------------------------------------- Parameters given as text (D146) */

/*
 * One serialization parameter given as text: an XQuery output declaration
 * (XQuery 4.0, section 5.22) or, later, an attribute of xsl:output. The
 * name is the parameter's local name in the serialization namespace; the
 * value has the lexical form of a parameter document (Serialization 4.0,
 * section 3.1 and appendix B: yes/no/true/false/1/0, QName lists,
 * decimals). Views are borrowed for the call.
 */
typedef struct sxml_serialization_parameter_text {
    sxml_byte_view parameter_name;
    sxml_byte_view parameter_value;
} sxml_serialization_parameter_text;

typedef struct sxml_serialization_parameter_text_view {
    const sxml_serialization_parameter_text *parameter_texts;
    size_t parameter_text_count;
} sxml_serialization_parameter_text_view;

/* Parameters to read from text, over an optional parameter document. */
typedef struct sxml_serialization_parameter_text_request {
    /* An output:serialization-parameters element read first, as the
       element form of sxml_serialization_parameter_reader_read reads it
       (SEPM0017 to SEPM0019, SEPM0017 also for another element). */
    bool has_parameter_document;
    sxml_node_reference parameter_document;
    /* Read after the document, each replacing its value; distinct names
       (the caller checks, XQST0110). */
    sxml_serialization_parameter_text_view parameter_texts;
    /* What QNames of the texts resolve with; prefix 0 is the default
       element namespace, which unprefixed element names take. */
    sxml_namespace_binding_view namespace_bindings;
    sxml_error_record *error_record;
    /* When not NULL, receives the index of the text that failed, or
       SIZE_MAX when the document did or nothing failed. */
    size_t *failed_text_index;
} sxml_serialization_parameter_text_request;

/*
 * Reads request's document and texts into *parameters as
 * sxml_serialization_parameter_reader_read reads its options: method xml
 * when none is given, the views owned by the reader until its next read.
 * A text with a value its parameter does not permit, or an unknown name,
 * raises SEPM0016 (an unsupported encoding SESU0007, normalization form
 * SESU0011). Returns SXML_STATUS_DYNAMIC_ERROR with the error record
 * filled, SXML_STATUS_INVALID_ARGUMENT for an invalid request, and the
 * allocator's and name pool's failures; on failure *parameters is
 * untouched.
 */
SXML_NODISCARD sxml_status sxml_serialization_parameter_reader_read_texts(
    sxml_serialization_parameter_reader *parameter_reader,
    const sxml_serialization_parameter_text_request *request,
    sxml_serialization_parameters *parameters);
