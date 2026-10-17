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
 * 4.0; docs/DECISIONS.md D82, D106, D120). Internal to sxml.
 *
 * - src/serialize.c: the output stream and buffer, the checks of the
 *   parameters every method shares, the character pipeline of section 4
 *   "Phases of Serialization" (character maps, Unicode normalization,
 *   escaping, line endings, encoding) behind sxml_serializer, sequence
 *   normalization (section 2), the adaptive method (section 11) and the
 *   dispatch by method.
 * - src/serialize_xml.c: the XML and XHTML methods (sections 5 and 6),
 *   and the node writer the adaptive method uses.
 * - Later parts (HTML, text, JSON, CSV) build on the same pipeline and on
 *   sxml_sequence_normalizer.
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

/* ------------------------------------------------ Sequence normalization */

/* What the sequence normalizer gives next. */
typedef enum sxml_normalized_event_kind {
    /* No more children of the result tree. */
    SXML_NORMALIZED_EVENT_KIND_END,
    /* A text node of the result tree: merged, never empty. */
    SXML_NORMALIZED_EVENT_KIND_TEXT,
    /* An element, comment or processing instruction to copy as a child
       of the result tree. */
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

/* ---------------------------------------------- XML and XHTML methods */

/*
 * Serializes value with the XML or XHTML method, as the serializer's
 * parameters say (Serialization 4.0, sections 5 and 6): sequence
 * normalization, the XML declaration, the document type declaration, the
 * result tree. The parameters were checked by sxml_serialize_sequence;
 * raises SEPM0004, SEPM0009, SEPM0010, SESU0013, SERE0024 and the errors
 * of the pipeline.
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
