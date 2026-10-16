#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/export.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/view.h"

/*
 * The XML parser: XML 1.0 bytes in, tree builder events out
 * (docs/DECISIONS.md, D12, D28, D51).
 *
 * The parser reads a whole document from memory, checks that it is
 * well-formed (XML 1.0 Fifth Edition) and namespace-well-formed (Namespaces
 * in XML 1.0 Third Edition, section 7), and sends its content to any
 * sxml_tree_builder (include/sxml/tree.h): the arena tree to build a
 * document, a host's own builder, or none at all to only check it.
 *
 * - Encodings: UTF-8, UTF-16 (either byte order, with a byte order mark),
 *   ISO-8859-1 and US-ASCII, detected as XML 1.0 Appendix F describes, or
 *   named by the caller from outside the document (XML 1.0, section 4.3.3).
 *   Any other encoding gives SXML_STATUS_UNSUPPORTED_FEATURE. Text sent to
 *   the builder is UTF-8 whatever the input encoding.
 * - Nothing outside the input is read. The external DTD subset and
 *   external entities are not retrieved, as XML 1.0 section 5.1 allows a
 *   processor that does not validate; the internal subset is processed
 *   (entity declarations, attribute defaults and types, unparsed entities).
 * - What the builder receives: a document with every element, attribute
 *   (defaulted ones included, after attribute-value normalization, XML 1.0
 *   section 3.3.3), text node (whitespace included; entity and character
 *   references replaced; CDATA sections as text), comment and processing
 *   instruction, in document order; namespace bindings as declared on each
 *   element; is_id for xml:id and for attributes declared ID in the
 *   internal subset. No type annotations: elements are xs:untyped and
 *   attributes xs:untypedAtomic.
 * - Failures carry an sxml_diagnostic with the line, column (in
 *   codepoints) and byte offset where the document stops being acceptable
 *   (D21). The description names the production or constraint that failed,
 *   for example "WFC: Element Type Match".
 *
 * Thread safety: a parser is used by one thread at a time, the thread of
 * the name pool it interns into (D35).
 */

/* Bounds on the work one document may cause; 0 means no bound. */
typedef struct sxml_xml_limits {
    /* Deepest element nesting, the document element at depth 1. Exceeding
       it gives SXML_STATUS_LIMIT_EXCEEDED. */
    uint32_t maximum_depth;
    /* Most bytes that replacing entity references may produce in one
       document, counted over every replacement, nested ones included: the
       bound on "billion laughs" input (XML 1.0, section 4.4). Exceeding it
       gives SXML_STATUS_LIMIT_EXCEEDED. */
    size_t maximum_entity_expansion_byte_count;
} sxml_xml_limits;

/*
 * A parser. It keeps memory between documents (the entity table, the
 * namespace stack, the attribute list), so one parser can serve many
 * documents.
 */
typedef struct sxml_xml_parser sxml_xml_parser;

typedef struct sxml_xml_parser_configuration {
    /* Provides the parser's working memory. Must not be the null
       allocator; must outlive the parser. The concrete syntax tree of the
       grammar is allocated by the tree-sitter runtime (D12) and freed
       before every parse returns. */
    sxml_allocator allocator;
    /* Element and attribute names and prefixes are interned here; the
       builder the events go to must use the same pool. Kept by reference;
       must outlive the parser. */
    sxml_name_pool *name_pool;
    sxml_xml_limits limits;
} sxml_xml_parser_configuration;

/*
 * One document to parse. Views are borrowed for the call: the builder
 * copies what it keeps.
 */
typedef struct sxml_xml_parse_request {
    /* The whole document, in its encoding. */
    sxml_byte_view input;
    /* The name to report in diagnostics and source locations (a file name
       or URI); may be empty. */
    sxml_byte_view source_name;
    /* dm:document-uri of the document node; empty when absent. fn:doc
       gives the absolute URI it retrieved; fn:parse-xml gives none. */
    sxml_byte_view document_uri;
    /* The base URI of the document node (XDM 4.0, section 7.3), before any
       xml:base attribute applies; empty when absent. */
    sxml_byte_view base_uri;
    /* The encoding named outside the document, such as the charset of an
       HTTP Content-Type, which then takes precedence over the encoding
       declaration (XML 1.0, Appendix F.2); empty to detect it. Matched
       without regard to ASCII case. */
    sxml_byte_view encoding_name;
    /* Receives the failure's description and location; may be NULL. */
    sxml_diagnostic *diagnostic;
} sxml_xml_parse_request;

/*
 * Writes the memory requirements of a parser, without the memory it
 * allocates while parsing, to *requirements. Returns
 * SXML_STATUS_INVALID_ARGUMENT for an invalid configuration (the null
 * allocator, no name pool).
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_xml_parser_memory_requirements(
    const sxml_xml_parser_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places a parser in memory and writes its address to *parser. memory must
 * stay valid, unmoved, until sxml_xml_parser_finalize. Allocates nothing
 * until the first parse.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for an invalid configuration, or
 * SXML_STATUS_INSUFFICIENT_MEMORY or SXML_STATUS_MISALIGNED_MEMORY when
 * memory does not meet the memory requirements.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_xml_parser_initialize(
    sxml_memory_view memory, const sxml_xml_parser_configuration *configuration,
    sxml_xml_parser **parser);

/*
 * Releases the parser's working memory. Does nothing when parser is NULL.
 */
SXML_EXPORT void sxml_xml_parser_finalize(sxml_xml_parser *parser);

/*
 * Allocates a parser through allocator and initializes it. allocator is
 * stored and must outlive the parser; it may also be the configuration's
 * allocator. Returns the failures of sxml_xml_parser_initialize and
 * sxml_allocator_allocate.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_xml_parser_create(
    sxml_allocator allocator, const sxml_xml_parser_configuration *configuration,
    sxml_xml_parser **parser);

/*
 * Finalizes a parser made by sxml_xml_parser_create and releases its
 * memory. Does nothing when parser is NULL.
 */
SXML_EXPORT void sxml_xml_parser_destroy(sxml_xml_parser *parser);

/*
 * Parses request->input as one XML document and sends its events to
 * builder: start_document (with request->document_uri and base_uri), the
 * unparsed entities of the internal subset, the content, end_document.
 * Every event carries its source location. Events may be sent before the
 * whole input is checked: on failure the builder has received part of
 * them, and the caller discards what it built.
 *
 * Returns, with request->diagnostic filled:
 * - SXML_STATUS_MALFORMED_INPUT when the document is not well-formed or
 *   not namespace-well-formed;
 * - SXML_STATUS_INVALID_UTF8 for bytes that are not well-formed in the
 *   detected encoding (UTF-8 or UTF-16), and SXML_STATUS_MALFORMED_INPUT
 *   for a character that is not an XML Char ([2]);
 * - SXML_STATUS_UNSUPPORTED_FEATURE for an encoding other than those
 *   listed at the top of this header;
 * - SXML_STATUS_LIMIT_EXCEEDED at a limit of the configuration;
 * - SXML_STATUS_OUT_OF_MEMORY when the allocator or the name pool cannot
 *   grow.
 * A failure of the builder is returned as the builder reported it (its
 * specification errors go to the builder's own error record).
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_xml_parser_parse(
    sxml_xml_parser *parser, const sxml_xml_parse_request *request,
    sxml_tree_builder builder);

/*
 * Checks request->input as sxml_xml_parser_parse does, without sending
 * events anywhere: the check of sxml check (docs/CLI.md). request's
 * document_uri and base_uri are ignored. Returns the failures of
 * sxml_xml_parser_parse that are not the builder's.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_xml_parser_check(
    sxml_xml_parser *parser, const sxml_xml_parse_request *request);
