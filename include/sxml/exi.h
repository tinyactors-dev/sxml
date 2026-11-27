#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/export.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/serialize.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/view.h"

/*
 * EXI: Efficient XML Interchange Format 1.0 (Second Edition), W3C
 * Recommendation 11 February 2014, and Canonical EXI, W3C Recommendation
 * 7 June 2018 (docs/DECISIONS.md, D333).
 *
 * Two codecs at the boundary of the data model, as the XML parser and the
 * serializer are:
 *
 * - sxml_exi_decoder reads an EXI stream and sends tree builder events
 *   (include/sxml/tree.h), as sxml_xml_parser does; the result is an
 *   ordinary tree.
 * - sxml_exi_encoder walks a tree and writes an EXI stream to an
 *   sxml_output_stream, as the serializer does.
 *
 * Both support every EXI option: the four alignments (bit-packed, byte
 * alignment, pre-compression, compression with DEFLATE, RFC 1951), the
 * fidelity options, strict, fragment, selfContained, schemaId,
 * datatypeRepresentationMap, blockSize, valueMaxLength and
 * valuePartitionCapacity, with built-in grammars (no schema) or with
 * schema-informed grammars built from a compiled XML Schema
 * (include/sxml/schema.h). The encoder also writes Canonical EXI.
 *
 * The data model and EXI (D333):
 * - An EXI stream holds an XML Information Set, an XDM tree holds more or
 *   less. The encoder writes a node's name, prefix (with
 *   preserves_prefixes), attributes, text, comments and processing
 *   instructions (with their fidelity options); type annotations are not
 *   written: a schema-informed stream types values by its grammars, not by
 *   the tree's annotations. The decoder builds untyped nodes (element
 *   xs:untyped, attribute xs:untypedAtomic), whatever grammars the stream
 *   used; validating the result is the caller's (sxml_schema_validator).
 * - A tree has no document type declaration and no entity references, so
 *   with preserves_dtd the encoder writes none; the decoder ignores a DT
 *   event's contents and refuses an ER event (SXML_STATUS_UNSUPPORTED_FEATURE),
 *   since it cannot expand the entity.
 * - Namespace declarations: an element's in-scope namespaces that its
 *   parent does not have (or has with another URI) are its declarations
 *   (NS events with preserves_prefixes); without preserves_prefixes the
 *   decoder makes prefixes ns0, ns1, ... as EXI 1.0, section 6.1
 *   ("Fidelity Options") leaves to the decoder.
 *
 * Names in options are identifiers of the name pool of the decoder's or
 * encoder's configuration (the item heap's pool, include/sxml/xdm.h).
 *
 * Thread safety: an encoder or a decoder is used by one thread at a time.
 */

/* A compiled XML Schema (include/sxml/schema.h, which includes this
   header through processor.h). */
typedef struct sxml_schema sxml_schema;

/* ---------------------------------------------------------------- Options */

/*
 * The alignment option (EXI 1.0, section 5.4 "EXI Options"; 9 "EXI
 * Compression"). BIT_PACKED, the default, is the value of a
 * zero-initialized struct.
 */
typedef enum sxml_exi_alignment {
    SXML_EXI_ALIGNMENT_BIT_PACKED = 0,
    /* "byte": every event code and value starts at a byte. */
    SXML_EXI_ALIGNMENT_BYTE_ALIGNMENT,
    /* "pre-compress": values regrouped into channels by name, byte
       aligned, without DEFLATE. */
    SXML_EXI_ALIGNMENT_PRE_COMPRESSION,
    /* "compression": the channels of pre-compression, each compressed
       with DEFLATE (EXI 1.0, section 9). */
    SXML_EXI_ALIGNMENT_COMPRESSION,
} sxml_exi_alignment;

/* The fidelity options (EXI 1.0, section 6.3): what the stream keeps. */
typedef struct sxml_exi_fidelity {
    /* Preserve.comments. */
    bool preserves_comments;
    /* Preserve.pis. */
    bool preserves_processing_instructions;
    /* Preserve.dtd: DOCTYPE and entity reference events (see the top of
       this header for what a tree can hold). */
    bool preserves_dtd;
    /* Preserve.prefixes: prefixes and namespace declarations. */
    bool preserves_prefixes;
    /* Preserve.lexicalValues: values as written, never as typed values. */
    bool preserves_lexical_values;
} sxml_exi_fidelity;

/* What the schemaId option says (EXI 1.0, section 5.4). */
typedef enum sxml_exi_schema_identifier_kind {
    /* No schemaId option: the schema, if any, is known out of band. */
    SXML_EXI_SCHEMA_IDENTIFIER_ABSENT = 0,
    /* schemaId with xsi:nil="true": no schema, the built-in grammars. */
    SXML_EXI_SCHEMA_IDENTIFIER_NONE,
    /* The empty schemaId: the built-in XML Schema types only, no
       declarations (EXI 1.0, section 8.5.1). */
    SXML_EXI_SCHEMA_IDENTIFIER_BUILT_IN_TYPES,
    /* schemaId names a schema, in text. */
    SXML_EXI_SCHEMA_IDENTIFIER_TEXT,
} sxml_exi_schema_identifier_kind;

typedef struct sxml_exi_schema_identifier {
    sxml_exi_schema_identifier_kind schema_identifier_kind;
    /* For SXML_EXI_SCHEMA_IDENTIFIER_TEXT: UTF-8, borrowed (see the
       struct that holds it); empty otherwise. */
    sxml_byte_view text;
} sxml_exi_schema_identifier;

/*
 * One entry of the datatypeRepresentationMap option (EXI 1.0, section 7.4):
 * values of schema_type (and its derived types without an entry of their
 * own) are written with the built-in EXI datatype representation
 * representation (a name in http://www.w3.org/2009/exi, such as
 * exi:string, exi:decimal or exi:integer).
 */
typedef struct sxml_exi_datatype_representation {
    sxml_name_identifier schema_type;
    sxml_name_identifier representation;
} sxml_exi_datatype_representation;

typedef struct sxml_exi_datatype_representation_view {
    const sxml_exi_datatype_representation *datatype_representations;
    size_t datatype_representation_count;
} sxml_exi_datatype_representation_view;

/*
 * The EXI options (EXI 1.0, section 5.4). A zero-initialized value is the
 * default options: bit-packed, nothing preserved, not strict, a document,
 * no schemaId, and unbounded string tables. Views are borrowed for the call
 * that receives the struct, unless the function that wrote it says
 * otherwise.
 */
typedef struct sxml_exi_options {
    sxml_exi_alignment alignment;
    sxml_exi_fidelity fidelity;
    /* strict: schema-informed grammars without their undeclared
       productions; with built-in grammars it means nothing. */
    bool is_strict;
    /* fragment: an EXI body of zero or more elements, comments and
       processing instructions, not a document (section 8.4.2). */
    bool is_fragment;
    /* selfContained: SC productions are allowed (section 8.5.4.4.1);
       which elements the encoder makes self-contained is the encode
       request's. */
    bool allows_self_contained;
    /* blockSize, the values per compression block (default 1,000,000). */
    bool has_block_size;
    /* valueMaxLength (default unbounded). */
    bool has_value_max_length;
    /* valuePartitionCapacity (default unbounded). */
    bool has_value_partition_capacity;
    uint32_t block_size;
    uint32_t value_max_length;
    uint32_t value_partition_capacity;
    sxml_exi_schema_identifier schema_identifier;
    sxml_exi_datatype_representation_view datatype_representation_map;
} sxml_exi_options;

/* The limits that bound the work and memory an untrusted stream can
   cause; 0 in a field means the default given. */
typedef struct sxml_exi_limits {
    /* Element nesting (default 4,096, the XML parser's). */
    uint32_t maximum_depth;
    /* Bytes of decoded content: names, values, text (default
       1,073,741,824). Bounds DEFLATE expansion too. */
    uint64_t maximum_content_byte_count;
    /* Productions the built-in grammars may learn (default 1,000,000). */
    uint32_t maximum_learned_production_count;
} sxml_exi_limits;

/* ---------------------------------------------------------- Options reader */

/*
 * Reads EXI options from their XML form: an exi:header element of the EXI
 * options schema (EXI 1.0, Appendix C; namespace http://www.w3.org/2009/exi),
 * the form an EXI stream's header carries, which the exi output method's
 * parameter takes (D333). It writes the views of the options it reads (the
 * schemaId text, the datatypeRepresentationMap) into its own memory, valid
 * until its next read or its end.
 */
typedef struct sxml_exi_options_reader sxml_exi_options_reader;

typedef struct sxml_exi_options_reader_configuration {
    /* Memory for the views it writes. Must outlive the reader. */
    sxml_allocator allocator;
    /* The pool of the names it writes (and of the element it reads). */
    sxml_name_pool *name_pool;
} sxml_exi_options_reader_configuration;

SXML_NODISCARD SXML_EXPORT sxml_status sxml_exi_options_reader_memory_requirements(
    const sxml_exi_options_reader_configuration *configuration,
    sxml_memory_requirements *requirements);

/* Places a reader in memory; returns SXML_STATUS_INVALID_ARGUMENT for an
   invalid configuration, SXML_STATUS_INSUFFICIENT_MEMORY or
   SXML_STATUS_MISALIGNED_MEMORY for memory that does not meet the
   requirements. */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_exi_options_reader_initialize(
    sxml_memory_view memory, const sxml_exi_options_reader_configuration *configuration,
    sxml_exi_options_reader **options_reader);

/* Releases what the reader allocated. Does nothing when NULL. */
SXML_EXPORT void sxml_exi_options_reader_finalize(sxml_exi_options_reader *options_reader);

SXML_NODISCARD SXML_EXPORT sxml_status sxml_exi_options_reader_create(
    sxml_allocator allocator, const sxml_exi_options_reader_configuration *configuration,
    sxml_exi_options_reader **options_reader);

SXML_EXPORT void sxml_exi_options_reader_destroy(sxml_exi_options_reader *options_reader);

/* The options element to read. */
typedef struct sxml_exi_options_read_request {
    /* An exi:header element, borrowed for the call. */
    sxml_node_reference header_element;
    /* Receives the description of a refusal; may be NULL. */
    sxml_diagnostic *diagnostic;
} sxml_exi_options_read_request;

/*
 * Writes the options header_element states to *options; options it does
 * not state have their defaults.
 *
 * Returns SXML_STATUS_MALFORMED_INPUT when the element is not valid against
 * the EXI options schema, or states an option EXI 1.0 does not allow (a
 * blockSize of 0, a datatypeRepresentationMap entry whose representation
 * is not a built-in EXI datatype), with diagnostic filled, and the
 * failures of the allocator and the name pool. On failure *options is
 * untouched.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_exi_options_reader_read(
    sxml_exi_options_reader *options_reader, const sxml_exi_options_read_request *request,
    sxml_exi_options *options);

/* ---------------------------------------------------------------- Decoder */

/*
 * Finds the schema a stream's schemaId names (EXI 1.0, section 5.4: "the
 * means by which the schemaId is resolved is application-defined"). The
 * processor's resolver (include/sxml/processor.h) is one; the null resolver
 * (operations NULL) finds none.
 */
typedef struct sxml_exi_schema_resolver_operations {
    /* Writes the schema named by identifier (kind
       SXML_EXI_SCHEMA_IDENTIFIER_TEXT, its text borrowed for the call) to
       *schema, which must stay valid until the decode or encode that asked
       returns; NULL when there is none, which the decode reports as
       SXML_STATUS_UNSUPPORTED_FEATURE. Returns another status to stop the
       call with it. */
    sxml_status (*resolve)(void *state, const sxml_exi_schema_identifier *identifier,
                           const sxml_schema **schema);
} sxml_exi_schema_resolver_operations;

typedef struct sxml_exi_schema_resolver {
    void *state;
    const sxml_exi_schema_resolver_operations *operations;
} sxml_exi_schema_resolver;

typedef struct sxml_exi_decoder sxml_exi_decoder;

typedef struct sxml_exi_decoder_configuration {
    /* Working memory: string tables, grammars, channels, DEFLATE windows.
       Must outlive the decoder. */
    sxml_allocator allocator;
    /* The pool the events' names are interned in. Must outlive the
       decoder. */
    sxml_name_pool *name_pool;
    sxml_exi_limits limits;
} sxml_exi_decoder_configuration;

SXML_NODISCARD SXML_EXPORT sxml_status sxml_exi_decoder_memory_requirements(
    const sxml_exi_decoder_configuration *configuration,
    sxml_memory_requirements *requirements);

/* Places a decoder in memory; allocates nothing until the first decode.
   Returns SXML_STATUS_INVALID_ARGUMENT for an invalid configuration (the
   null allocator, no name pool), SXML_STATUS_INSUFFICIENT_MEMORY or
   SXML_STATUS_MISALIGNED_MEMORY for memory that does not meet the
   requirements. */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_exi_decoder_initialize(
    sxml_memory_view memory, const sxml_exi_decoder_configuration *configuration,
    sxml_exi_decoder **decoder);

/* Releases the decoder's working memory. Does nothing when NULL. */
SXML_EXPORT void sxml_exi_decoder_finalize(sxml_exi_decoder *decoder);

SXML_NODISCARD SXML_EXPORT sxml_status sxml_exi_decoder_create(
    sxml_allocator allocator, const sxml_exi_decoder_configuration *configuration,
    sxml_exi_decoder **decoder);

SXML_EXPORT void sxml_exi_decoder_destroy(sxml_exi_decoder *decoder);

/* A stream to decode. */
typedef struct sxml_exi_decode_request {
    /* The whole stream, borrowed for the call. */
    sxml_byte_view input;
    /* The options when the stream's header carries none (EXI 1.0, section
       5.4: then they are known out of band); NULL for the defaults. Ignored
       when the header carries options. */
    const sxml_exi_options *options;
    /* The schema of the stream when its options have no schemaId (then
       known out of band); NULL for the built-in grammars. */
    const sxml_schema *schema;
    /* Finds the schema a schemaId names. */
    sxml_exi_schema_resolver schema_resolver;
    /* dm:document-uri and the base URI of the document node; may be
       empty, as in sxml_xml_parse_request. */
    sxml_byte_view document_uri;
    sxml_byte_view base_uri;
    /* Receives the failure's description and its byte offset; may be
       NULL. */
    sxml_diagnostic *diagnostic;
} sxml_exi_decode_request;

/*
 * Decodes request->input and sends its events to builder: start_document,
 * the content (for a fragment, its elements, comments and processing
 * instructions as children of the document node), end_document. Events may
 * be sent before the whole stream is checked: on failure the builder has
 * received part of them, and the caller discards what it built.
 *
 * Returns, with request->diagnostic filled:
 * - SXML_STATUS_MALFORMED_INPUT for a stream EXI 1.0 does not allow (no
 *   distinguishing bits, an event code no production has, a value out of
 *   range, a string table index past its end, a DEFLATE stream that does
 *   not decode, bytes left after the end of the stream's last block);
 * - SXML_STATUS_UNEXPECTED_END_OF_INPUT for a stream that ends early;
 * - SXML_STATUS_UNSUPPORTED_FEATURE for an EXI version other than 1, an
 *   entity reference event, or a schemaId the resolver cannot find;
 * - SXML_STATUS_LIMIT_EXCEEDED at a limit of the configuration;
 * - SXML_STATUS_OUT_OF_MEMORY when the allocator or the name pool cannot
 *   grow; and a failure of the builder as the builder reported it.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_exi_decoder_decode(
    sxml_exi_decoder *decoder, const sxml_exi_decode_request *request,
    sxml_tree_builder builder);

/* ---------------------------------------------------------------- Encoder */

/* What the encoder writes before the body (EXI 1.0, section 5). */
typedef struct sxml_exi_header_options {
    /* The EXI cookie "$EXI" (section 5.1). */
    bool includes_cookie;
    /* The options, as an options document (section 5.4); without it a
       decoder must know them out of band. */
    bool includes_options;
    /* With includes_options: the schemaId option too, which the options
       state otherwise only when their schema_identifier is not ABSENT. */
    bool includes_schema_identifier;
} sxml_exi_header_options;

/*
 * Canonical EXI (W3C Recommendation 7 June 2018). With is_canonical the
 * encoder writes the stream's canonical form (its sections 3 and 4): no
 * cookie, the options document unless omits_options_document, the most
 * precise production for each event, namespace declarations sorted by
 * prefix, xsi:type and xsi:nil before the other attributes, which are
 * sorted by local name and namespace URI, whitespace-only text in
 * element-only content removed, values in their canonical representation,
 * and compression computed as pre-compression; header_options is then
 * ignored. The Canonical EXI options are the two flags below (its section
 * 2.1).
 */
typedef struct sxml_exi_canonicalization {
    bool is_canonical;
    /* omitOptionsDocument. */
    bool omits_options_document;
    /* utcTime: date-time values with a timezone in UTC. */
    bool uses_utc_time;
} sxml_exi_canonicalization;

typedef struct sxml_exi_encoder sxml_exi_encoder;

typedef struct sxml_exi_encoder_configuration {
    /* Working memory: string tables, grammars, channel buffers, the
       DEFLATE window. Must outlive the encoder. */
    sxml_allocator allocator;
} sxml_exi_encoder_configuration;

SXML_NODISCARD SXML_EXPORT sxml_status sxml_exi_encoder_memory_requirements(
    const sxml_exi_encoder_configuration *configuration,
    sxml_memory_requirements *requirements);

/* Places an encoder in memory; allocates nothing until the first encode.
   Returns SXML_STATUS_INVALID_ARGUMENT for an invalid configuration (the
   null allocator), SXML_STATUS_INSUFFICIENT_MEMORY or
   SXML_STATUS_MISALIGNED_MEMORY for memory that does not meet the
   requirements. */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_exi_encoder_initialize(
    sxml_memory_view memory, const sxml_exi_encoder_configuration *configuration,
    sxml_exi_encoder **encoder);

/* Releases the encoder's working memory. Does nothing when NULL. */
SXML_EXPORT void sxml_exi_encoder_finalize(sxml_exi_encoder *encoder);

SXML_NODISCARD SXML_EXPORT sxml_status sxml_exi_encoder_create(
    sxml_allocator allocator, const sxml_exi_encoder_configuration *configuration,
    sxml_exi_encoder **encoder);

SXML_EXPORT void sxml_exi_encoder_destroy(sxml_exi_encoder *encoder);

/* A tree to encode. */
typedef struct sxml_exi_encode_request {
    /* A document node, or an element written as a document's only
       element; with options->is_fragment a document node's children (or
       an element) are the fragment. Borrowed for the call. */
    sxml_node_reference root;
    /* The options; NULL for the defaults. */
    const sxml_exi_options *options;
    /* The schema of the schema-informed grammars when options name none
       the encoder must find (schema_identifier ABSENT or TEXT); NULL with
       schema_identifier ABSENT or NONE means the built-in grammars. */
    const sxml_schema *schema;
    sxml_exi_header_options header_options;
    sxml_exi_canonicalization canonicalization;
    /* The elements written self-contained (SC events), with
       options->allows_self_contained; ignored otherwise. Names of the
       tree's pool. */
    sxml_element_name_view self_contained_elements;
    /* Receives the description of a refusal; may be NULL. */
    sxml_diagnostic *diagnostic;
} sxml_exi_encode_request;

/*
 * Encodes request->root as an EXI stream with request->options and writes
 * it to output_stream (EXI 1.0, sections 5 to 9), with the whole of a
 * compression block buffered before it is written.
 *
 * Returns, with request->diagnostic filled:
 * - SXML_STATUS_DYNAMIC_ERROR for content the options cannot represent:
 *   in strict mode, content the schema-informed grammars have no
 *   production for (an undeclared element or attribute, a value that is
 *   not of its type, a comment or processing instruction); a fragment
 *   option with a root that is neither a document node nor an element;
 * - SXML_STATUS_INVALID_ARGUMENT for options EXI 1.0 does not allow (a
 *   block size of 0, a representation that is not a built-in EXI
 *   datatype) or a schema identifier of kind TEXT without a schema;
 * - the failures of output_stream, the allocator and the tree.
 * On failure part of the stream may have been written.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_exi_encoder_encode(
    sxml_exi_encoder *encoder, const sxml_exi_encode_request *request,
    sxml_output_stream output_stream);

/* ---------------------------------------------------- The exi output method */

/*
 * The parameters of the output method Q{urn:sxml:exi}exi
 * (SXML_SERIALIZATION_METHOD_EXI, include/sxml/serialize.h; D333), which
 * sxml_serialization_parameters.exi_parameters points to. The serialization
 * parameter reader fills one from these parameters, in namespace
 * urn:sxml:exi (prefix exi), of an output:serialization-parameters element
 * or a map:
 *
 * - exi:options: an exi:header element of the EXI options schema
 *   (http://www.w3.org/2009/exi), read as sxml_exi_options_reader does;
 * - exi:include-cookie, exi:include-options, exi:include-schema-id: yes or
 *   no, header_options;
 * - exi:omit-options-document, exi:utc-time: yes or no, the Canonical EXI
 *   options; Serialization 4.0's canonical parameter sets
 *   canonicalization.is_canonical;
 * - exi:self-contained-elements: a list of element names, as
 *   cdata-section-elements is.
 *
 * Views and names are borrowed as the struct that holds them says.
 */
typedef struct sxml_exi_serialization_parameters {
    sxml_exi_options options;
    sxml_exi_header_options header_options;
    sxml_exi_canonicalization canonicalization;
    sxml_element_name_view self_contained_elements;
    /* The schema for options.schema_identifier; NULL asks schema_resolver
       for one of kind TEXT. */
    const sxml_schema *schema;
    sxml_exi_schema_resolver schema_resolver;
} sxml_exi_serialization_parameters;
