#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/export.h"
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"

/*
 * Serialization: a sequence to bytes (Serialization 4.0;
 * docs/DECISIONS.md, D47, D51, D106).
 *
 * Bytes go to an sxml_output_stream, an interface the host implements over
 * its own output (a file, standard output, a JavaScript callback);
 * sxml_output_buffer is the built-in one, collecting the bytes in memory.
 * The bytes are in the encoding the serialization parameters choose,
 * UTF-8 unless they say otherwise.
 *
 * The output methods are those of Serialization 4.0: xml, xhtml, html,
 * text, json, csv and adaptive (sxml_serialization_method). One parameters
 * struct holds every Serialization 4.0 parameter; a host fills it in C, or
 * has an sxml_serialization_parameter_reader fill it from an
 * output:serialization-parameters element or a map (the two forms of the
 * $options argument of fn:serialize). A compiled XQuery query gives the
 * parameters its prolog declares (include/sxml/xquery.h).
 *
 * Thread safety: the thread of the item heap whose values are serialized
 * (D35).
 */

/* ----------------------------------------------------------- Output stream */

/* The operations of an output stream. Tables are static const. */
typedef struct sxml_output_stream_operations {
    /* Writes all of bytes, after everything written before. Returns
       SXML_STATUS_INPUT_OUTPUT_FAILED when the platform refuses the write
       (D50). A failure stops the serialization, which returns the status
       unchanged. */
    sxml_status (*write)(void *state, sxml_byte_view bytes);
} sxml_output_stream_operations;

/* The output stream interface, passed by value. */
typedef struct sxml_output_stream {
    void *state;
    const sxml_output_stream_operations *operations;
} sxml_output_stream;

/*
 * Writes bytes to output_stream. Writing no bytes does nothing. Returns
 * SXML_STATUS_INVALID_ARGUMENT when output_stream has no operations, and
 * the stream's failures.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_output_stream_write(
    sxml_output_stream output_stream, sxml_byte_view bytes);

/* ----------------------------------------------------------- Output buffer */

/*
 * An output stream that collects what is written in memory, growing
 * geometrically through its allocator: for tests, for fn:serialize, and for
 * hosts that want the bytes at once (the WebAssembly package).
 *
 * Thread safety: one thread at a time.
 */
typedef struct sxml_output_buffer sxml_output_buffer;

typedef struct sxml_output_buffer_configuration {
    /* Provides the memory for the bytes. Must not be the null allocator;
       must outlive the buffer. */
    sxml_allocator allocator;
    /* Most bytes the buffer may hold; 0 means no bound. A write that would
       pass it gives SXML_STATUS_LIMIT_EXCEEDED and writes nothing. */
    size_t maximum_byte_count;
} sxml_output_buffer_configuration;

/*
 * Writes the memory requirements of an output buffer, without the bytes
 * it holds, to *requirements. Returns SXML_STATUS_INVALID_ARGUMENT for an
 * invalid configuration.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_output_buffer_memory_requirements(
    const sxml_output_buffer_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places an empty output buffer in memory and writes its address to
 * *output_buffer. memory must stay valid, unmoved, until
 * sxml_output_buffer_finalize. Allocates nothing until the first write.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for an invalid configuration, or
 * SXML_STATUS_INSUFFICIENT_MEMORY or SXML_STATUS_MISALIGNED_MEMORY when
 * memory does not meet the memory requirements.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_output_buffer_initialize(
    sxml_memory_view memory,
    const sxml_output_buffer_configuration *configuration,
    sxml_output_buffer **output_buffer);

/* Releases the bytes. Does nothing when output_buffer is NULL. */
SXML_EXPORT void sxml_output_buffer_finalize(sxml_output_buffer *output_buffer);

/*
 * Allocates an output buffer through allocator and initializes it.
 * allocator is stored and must outlive the buffer. Returns the failures of
 * sxml_output_buffer_initialize and sxml_allocator_allocate.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_output_buffer_create(
    sxml_allocator allocator,
    const sxml_output_buffer_configuration *configuration,
    sxml_output_buffer **output_buffer);

/*
 * Finalizes an output buffer made by sxml_output_buffer_create and releases
 * its memory. Does nothing when output_buffer is NULL.
 */
SXML_EXPORT void sxml_output_buffer_destroy(sxml_output_buffer *output_buffer);

/*
 * The output stream that appends to output_buffer. Its write returns
 * SXML_STATUS_LIMIT_EXCEEDED at maximum_byte_count, the allocator's
 * failures and SXML_STATUS_ARITHMETIC_OVERFLOW, leaving the buffer
 * unchanged. Valid until output_buffer is finalized.
 */
SXML_EXPORT sxml_output_stream sxml_output_buffer_as_output_stream(
    sxml_output_buffer *output_buffer);

/*
 * The bytes written so far, in order. Borrowed: valid until the next write,
 * sxml_output_buffer_clear or finalization.
 */
SXML_EXPORT sxml_byte_view sxml_output_buffer_get_bytes(
    const sxml_output_buffer *output_buffer);

/* Forgets the bytes written, keeping the memory for reuse. */
SXML_EXPORT void sxml_output_buffer_clear(sxml_output_buffer *output_buffer);

/* ---------------------------------------------------- Serialization parameters */

/*
 * The output methods (Serialization 4.0, section 3 "Serialization
 * Parameters", parameter method). The list only grows. A method name with
 * a namespace (an implementation-defined method) has no constant: sxml
 * defines none, and the parameter reader raises SEPM0016 for one.
 */
typedef enum sxml_serialization_method {
    /* Serialization 4.0, "Adaptive Output Method": items one after the
       other with the item separator between them; atomic items in a form
       that shows their type (strings quoted, xs:double with an exponent,
       other types as constructor calls such as xs:date("2026-10-16")),
       nodes as the XML method writes them, maps, arrays and function items
       in XPath-like syntax. The value of a zero-initialized
       sxml_serialization_parameters; the method of the trace sink. */
    SXML_SERIALIZATION_METHOD_ADAPTIVE,
    /* Serialization 4.0, "XML Output Method": the normalized sequence as
       well-formed XML (an external general parsed entity). The default of
       XQuery (5.22.1) and of fn:serialize. */
    SXML_SERIALIZATION_METHOD_XML,
    /* Serialization 4.0, "XHTML Output Method": XML that HTML user agents
       also read (empty elements, xml:lang, the rules of html-version). */
    SXML_SERIALIZATION_METHOD_XHTML,
    /* Serialization 4.0, "HTML Output Method": HTML syntax (void elements,
       raw text elements, boolean attributes), HTML 5 unless html-version
       or version names an earlier version. */
    SXML_SERIALIZATION_METHOD_HTML,
    /* Serialization 4.0, "Text Output Method": the string values of the
       normalized sequence, no markup. */
    SXML_SERIALIZATION_METHOD_TEXT,
    /* Serialization 4.0, "JSON Output Method": maps as objects, arrays as
       arrays, atomic items as JSON values, nodes serialized by
       json-node-output-method into strings; more than one item only with
       json-lines (SERE0023). */
    SXML_SERIALIZATION_METHOD_JSON,
    /* Serialization 4.0, "CSV Output Method": a parsed CSV structure (a
       map with rows and columns, as fn:parse-csv returns) or a sequence
       of arrays of atomic items, as CSV records (SERE0025 for other
       input). */
    SXML_SERIALIZATION_METHOD_CSV,
} sxml_serialization_method;

/*
 * The value of a boolean serialization parameter (allow-duplicate-names,
 * indent, ...). DEFAULT means the parameter was not given: the default
 * the field lists, which is the default of fn:serialize (F&O 4.0,
 * "fn:serialize"), so that a zero-initialized struct needs no further
 * settings. A host language with other defaults (XSLT's xsl:output writes
 * the XML declaration by default) sets YES or NO itself.
 */
typedef enum sxml_serialization_flag {
    SXML_SERIALIZATION_FLAG_DEFAULT = 0,
    SXML_SERIALIZATION_FLAG_NO,
    SXML_SERIALIZATION_FLAG_YES,
} sxml_serialization_flag;

/*
 * The encodings the serializer writes (parameter encoding; Serialization
 * 4.0, 5.1.3). The list only grows. Names are matched without regard to
 * case, with the IANA aliases "UTF8", "UTF16", "ASCII", "US-ASCII",
 * "ISO_8859-1", "latin1"; the parameter reader raises SESU0007 for any
 * other name. A character the encoding cannot represent is written as a
 * character reference where the method allows one, and raises SERE0008
 * where it does not (a comment, a CDATA section, an element name).
 */
typedef enum sxml_character_encoding {
    /* UTF-8, the default. */
    SXML_CHARACTER_ENCODING_UTF_8 = 0,
    /* UTF-16, big endian (RFC 2781, section 4.3: the order without a byte
       order mark). */
    SXML_CHARACTER_ENCODING_UTF_16,
    /* US-ASCII: U+0000 to U+007F. */
    SXML_CHARACTER_ENCODING_US_ASCII,
    /* ISO-8859-1: U+0000 to U+00FF. */
    SXML_CHARACTER_ENCODING_ISO_8859_1,
} sxml_character_encoding;

/* Parameter standalone (Serialization 4.0, 5.1.9). */
typedef enum sxml_standalone {
    /* The XML declaration has no standalone pseudo-attribute; the
       default. */
    SXML_STANDALONE_OMIT = 0,
    SXML_STANDALONE_YES,
    SXML_STANDALONE_NO,
} sxml_standalone;

/* Parameter normalization-form (Serialization 4.0, 5.1.12). */
typedef enum sxml_normalization_form {
    /* No normalization; the default. */
    SXML_NORMALIZATION_FORM_NONE = 0,
    SXML_NORMALIZATION_FORM_NFC,
    SXML_NORMALIZATION_FORM_NFD,
    SXML_NORMALIZATION_FORM_NFKC,
    SXML_NORMALIZATION_FORM_NFKD,
    /* NFC, and no relevant construct (text node, attribute value, ...)
       starts with a composing character (Serialization 4.0, 5.1.12,
       Character Model for the World Wide Web 1.0). */
    SXML_NORMALIZATION_FORM_FULLY_NORMALIZED,
} sxml_normalization_form;

/*
 * Parameter line-ending (Serialization 4.0, 5.1.7): what every U+000A
 * the serializer writes, in content too, becomes, after character mapping
 * and normalization.
 */
typedef enum sxml_line_ending {
    /* U+000A, unchanged: the default, and the value when the parameter is
       absent. */
    SXML_LINE_ENDING_LINE_FEED = 0,
    /* U+000D U+000A. */
    SXML_LINE_ENDING_CARRIAGE_RETURN_LINE_FEED,
    /* U+000D. */
    SXML_LINE_ENDING_CARRIAGE_RETURN,
} sxml_line_ending;

/*
 * Parameter json-node-output-method (Serialization 4.0, 9.3.12): how the
 * JSON method writes a node, before it becomes a JSON string.
 */
typedef enum sxml_json_node_output_method {
    /* The default. */
    SXML_JSON_NODE_OUTPUT_METHOD_XML = 0,
    SXML_JSON_NODE_OUTPUT_METHOD_XHTML,
    SXML_JSON_NODE_OUTPUT_METHOD_HTML,
    SXML_JSON_NODE_OUTPUT_METHOD_TEXT,
} sxml_json_node_output_method;

/*
 * A list of element names (parameters cdata-section-elements and
 * suppress-indentation): name pool identifiers of the item heap that
 * serializes, none of them 0. Duplicates are allowed and mean nothing.
 */
typedef struct sxml_element_name_view {
    const sxml_name_identifier *element_names;
    size_t element_name_count;
} sxml_element_name_view;

/*
 * One pair of a character map (parameter use-character-maps;
 * Serialization 4.0, section 12 "Character Maps"): where the serializer
 * would write codepoint as character data (text nodes and attribute
 * values; not in CDATA sections, comments and processing instructions;
 * section 12 lists the places for each method), it writes
 * replacement_text as it is, without escaping.
 */
typedef struct sxml_character_mapping {
    /* A character XML allows (XML 1.0, production Char). */
    uint32_t codepoint;
    /* Well-formed UTF-8; may be empty. */
    sxml_byte_view replacement_text;
} sxml_character_mapping;

/* The pairs of a character map; codepoints must be distinct. */
typedef struct sxml_character_map_view {
    const sxml_character_mapping *character_mappings;
    size_t character_mapping_count;
} sxml_character_map_view;

/*
 * Every serialization parameter of Serialization 4.0 (section 3,
 * "Serialization Parameters"). Each field is the parameter whose name it
 * spells with "_" for "-", except where its comment says otherwise. A
 * zero-initialized value is the adaptive method with every parameter at
 * its default: a field left out of a designated initializer keeps its
 * default. A parameter that does not apply to the method is ignored, as
 * Serialization 4.0 requires; its value is still checked.
 *
 * Parameters that may be absent are a view and a flag: has_<name> is true
 * when <name> is present. Every view is borrowed for the call that
 * receives the struct, unless a function that writes one says otherwise.
 * Text is UTF-8. A value the permitted values of section 3 do not allow
 * raises SEPM0016 when the struct is used (sxml_serialize_sequence); one
 * C cannot express in XDM (ill-formed UTF-8, an unknown enumeration
 * constant, name identifier 0) is an invalid argument.
 *
 * Fields are grouped by size (enumerations and codepoints, flags of
 * presence, views) so the struct has no padding; initialize it by field
 * name.
 */
typedef struct sxml_serialization_parameters {
    /* ---- Enumerations and codepoints. */
    /* method. */
    sxml_serialization_method serialization_method;
    /* allow-duplicate-names: whether the JSON method accepts an object
       with two members of the same name (SERE0022 when it may not); DEFAULT
       is no. */
    sxml_serialization_flag allow_duplicate_names;
    /* byte-order-mark: whether a byte order mark comes first; DEFAULT is
       no. Ignored for US-ASCII and ISO-8859-1, which have none. */
    sxml_serialization_flag byte_order_mark;
    /* canonical: Canonical XML 1.1 for the XML and XHTML methods, RFC 8785
       for JSON; DEFAULT is no. */
    sxml_serialization_flag canonical;
    /* csv-header: whether the CSV method writes a header record from the
       columns of a parsed CSV structure; DEFAULT is no. */
    sxml_serialization_flag csv_header;
    /* csv-quote-character, as a codepoint; 0 means the default, U+0022
       QUOTATION MARK. */
    uint32_t csv_quote_character_codepoint;
    /* csv-separator, as a codepoint; 0 means the default, U+002C COMMA. */
    uint32_t csv_separator_codepoint;
    /* encoding. */
    sxml_character_encoding character_encoding;
    /* escape-solidus: whether the JSON method writes "/" as "\/"; DEFAULT
       is yes. */
    sxml_serialization_flag escape_solidus;
    /* escape-uri-attributes: whether the HTML and XHTML methods
       %-escape URI attribute values; DEFAULT is yes. */
    sxml_serialization_flag escape_uri_attributes;
    /* include-content-type: whether the HTML and XHTML methods add a meta
       element naming the content type; DEFAULT is yes. */
    sxml_serialization_flag include_content_type;
    /* indent: DEFAULT is no. */
    sxml_serialization_flag indent;
    /* indent-attributes: whether indentation may put attributes on lines
       of their own; DEFAULT is no. */
    sxml_serialization_flag indent_attributes;
    /* json-lines: whether the JSON method writes a sequence as one JSON
       text per line (JSON Lines); DEFAULT is no. */
    sxml_serialization_flag json_lines;
    /* json-node-output-method. */
    sxml_json_node_output_method json_node_output_method;
    /* line-ending. */
    sxml_line_ending line_ending;
    /* normalization-form. */
    sxml_normalization_form normalization_form;
    /* omit-xml-declaration: whether the XML and XHTML methods leave out
       the XML declaration; DEFAULT is yes (as fn:serialize; XQuery leaves
       the default to the implementation). SEPM0009 when it is yes while
       standalone is not OMIT, or while version is not 1.0 and
       doctype-system is present (Serialization 4.0, 5.1.9). */
    sxml_serialization_flag omit_xml_declaration;
    /* standalone. */
    sxml_standalone standalone;
    /* undeclare-prefixes: whether the XML method writes xmlns:p=""
       undeclarations (XML Names 1.1; version must be 1.1, else SEPM0010);
       DEFAULT is no. */
    sxml_serialization_flag undeclare_prefixes;

    /* ---- Presence of the parameters that may be absent. */
    bool has_doctype_public;
    bool has_doctype_system;
    bool has_html_version;
    bool has_indent_unit;
    bool has_item_separator;
    bool has_media_type;
    bool has_version;

    /* ---- Text and lists. */
    /* cdata-section-elements: text nodes whose parent has one of these
       names are written as CDATA sections; empty by default. */
    sxml_element_name_view cdata_section_elements;
    /* doctype-public: PubidChar characters only (XML 1.0, production
       PubidChar). */
    sxml_byte_view doctype_public;
    /* doctype-system: must not hold both U+0022 and U+0027. */
    sxml_byte_view doctype_system;
    /* html-version: the lexical form of an xs:decimal, such as "5.0" or
       "4.01". */
    sxml_byte_view html_version;
    /* indent-unit: the whitespace of one level of indentation, spaces only
       or tabs only (the reader turns the escape "\t" into a tab); may be
       empty. When absent, two spaces. */
    sxml_byte_view indent_unit;
    /* item-separator: written between items of the sequence. When absent:
       a newline (U+000A) for the adaptive method; a single space between
       adjacent atomic items, and nothing elsewhere, for the others
       (Serialization 4.0, section 2 "Sequence Normalization"). */
    sxml_byte_view item_separator;
    /* media-type: when absent, the method's (application/xml, text/html,
       ...). Only the media type: no charset parameter. Written where the
       method writes it (the meta element of include-content-type). */
    sxml_byte_view media_type;
    /* suppress-indentation: elements whose content is not indented;
       empty by default. */
    sxml_element_name_view suppress_indentation;
    /* use-character-maps: empty by default. */
    sxml_character_map_view use_character_maps;
    /* version: when absent, "1.0" for the XML and XHTML methods; for the
       HTML method the version when html-version is absent. SESU0013 for
       an XML version other than 1.0 and 1.1. */
    sxml_byte_view version;
} sxml_serialization_parameters;

/* -------------------------------------------------- Parameter reader */

/*
 * Reads serialization parameters from XDM values and keeps the text and
 * lists they refer to: from an output:serialization-parameters element (a
 * parameter document, Serialization 4.0, section 3.1) or from a map with
 * the option parameter conventions (F&O 4.0, "fn:serialize"). For
 * fn:serialize, for the QT4 driver, and for hosts that take parameters as
 * XML or as a map.
 *
 * Thread safety: the thread of its item heap (D35).
 */
typedef struct sxml_serialization_parameter_reader
    sxml_serialization_parameter_reader;

typedef struct sxml_serialization_parameter_reader_configuration {
    /* Provides the memory for what the read parameters refer to. Must not
       be the null allocator; must outlive the reader. */
    sxml_allocator allocator;
    /* The heap the values come from; its name pool interns the element
       names of cdata-section-elements and suppress-indentation. Kept by
       reference; must outlive the reader. */
    sxml_item_heap *item_heap;
} sxml_serialization_parameter_reader_configuration;

/*
 * Writes the memory requirements of a parameter reader, without what it
 * allocates as it reads, to *requirements. Returns
 * SXML_STATUS_INVALID_ARGUMENT for an invalid configuration (the null
 * allocator, no item heap).
 */
SXML_NODISCARD SXML_EXPORT sxml_status
sxml_serialization_parameter_reader_memory_requirements(
    const sxml_serialization_parameter_reader_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places a parameter reader in memory and writes its address to
 * *parameter_reader. memory must stay valid, unmoved, until
 * sxml_serialization_parameter_reader_finalize. Allocates nothing until
 * the first read.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for an invalid configuration, or
 * SXML_STATUS_INSUFFICIENT_MEMORY or SXML_STATUS_MISALIGNED_MEMORY when
 * memory does not meet the memory requirements.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_serialization_parameter_reader_initialize(
    sxml_memory_view memory,
    const sxml_serialization_parameter_reader_configuration *configuration,
    sxml_serialization_parameter_reader **parameter_reader);

/*
 * Releases what the reader allocated; the parameters it wrote become
 * invalid. Does nothing when parameter_reader is NULL.
 */
SXML_EXPORT void sxml_serialization_parameter_reader_finalize(
    sxml_serialization_parameter_reader *parameter_reader);

/*
 * Allocates a parameter reader through allocator and initializes it.
 * allocator is stored and must outlive the reader. Returns the failures
 * of sxml_serialization_parameter_reader_initialize and
 * sxml_allocator_allocate.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_serialization_parameter_reader_create(
    sxml_allocator allocator,
    const sxml_serialization_parameter_reader_configuration *configuration,
    sxml_serialization_parameter_reader **parameter_reader);

/*
 * Finalizes a parameter reader made by
 * sxml_serialization_parameter_reader_create and releases its memory.
 * Does nothing when parameter_reader is NULL.
 */
SXML_EXPORT void sxml_serialization_parameter_reader_destroy(
    sxml_serialization_parameter_reader *parameter_reader);

/* Parameters to read: the $options of fn:serialize. */
typedef struct sxml_serialization_parameter_read_request {
    /*
     * The empty sequence, one map, or one element node named
     * output:serialization-parameters, in the reader's item heap. Borrowed
     * for the call.
     * - The empty sequence gives the defaults below.
     * - A map: each entry with an xs:string key names a parameter; its
     *   value has the type the F&O 4.0 table gives (XPTY0004 otherwise,
     *   after coercion), the empty sequence meaning the default; an
     *   invalid value raises SEPM0016, an unknown method or encoding
     *   SEPM0016 or SESU0007. Entries with an xs:QName key in a namespace
     *   other than that of serialization are ignored (implementation-
     *   defined parameters: sxml has none); unknown string keys are
     *   ignored as the option parameter conventions say.
     * - An element: read as Serialization 4.0, section 3.1 describes, with
     *   SEPM0017 for a child in the serialization namespace that is not a
     *   parameter or has an invalid value attribute, SEPM0018 for a
     *   character given twice in use-character-maps, SEPM0019 for a
     *   parameter given twice. Children in other namespaces are ignored.
     *   QName values resolve against the in-scope namespaces of their
     *   element, unprefixed names in no namespace.
     */
    sxml_sequence options;
    /* Receives the error raised; may be NULL. Borrowed for the call. */
    sxml_error_record *error_record;
} sxml_serialization_parameter_read_request;

/*
 * Reads request->options and writes the parameters to *parameters: those
 * the options give, method xml when they give none (F&O 4.0,
 * "fn:serialize"), and every other field zero (its default). The views
 * in *parameters belong to the reader: valid until its next read or its
 * finalization.
 *
 * Returns SXML_STATUS_TYPE_ERROR or SXML_STATUS_DYNAMIC_ERROR with the
 * error record filled (the codes above; XPTY0004 also for options that are
 * neither a map nor that element), SXML_STATUS_INVALID_ARGUMENT for
 * options from another heap, and the allocator's and name pool's
 * failures. On failure *parameters is untouched.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_serialization_parameter_reader_read(
    sxml_serialization_parameter_reader *parameter_reader,
    const sxml_serialization_parameter_read_request *request,
    sxml_serialization_parameters *parameters);

/* ----------------------------------------------------------- Serialization */

/* A sequence to serialize. */
typedef struct sxml_serialization_request {
    /* Borrowed for the call. A lazy sequence is read to its end. */
    sxml_sequence value;
    sxml_serialization_parameters serialization_parameters;
    /* Receives a serialization error (SEPM0016, SENR0001, SERE0014, ...)
       or an error raised while reading value; may be NULL. */
    sxml_error_record *error_record;
} sxml_serialization_request;

/*
 * Serializes request->value with request->serialization_parameters and
 * writes the bytes to output_stream, in pieces, as they are produced
 * (Serialization 4.0: sequence normalization, then the method, character
 * maps, normalization, line endings and encoding, section 4 "Phases of
 * Serialization"). The item heap must be the one request->value lives in;
 * it provides the names and types to write, and the names of the
 * parameters are identifiers of its name pool.
 *
 * Returns:
 * - SXML_STATUS_DYNAMIC_ERROR with the error record filled for a
 *   serialization error (SEPM0016 for a parameter value Serialization 4.0
 *   does not permit, SEPM0009 and SEPM0010 for contradicting parameters,
 *   SENR0001 for an item the method cannot write, SERE0008 for a
 *   character the encoding cannot represent where no reference is
 *   allowed, SERE0014, SERE0020, SERE0022, SERE0023, SESU0013, ...) or
 *   for an error raised by a lazy value;
 * - SXML_STATUS_INVALID_ARGUMENT for parameters C cannot express in XDM
 *   (ill-formed UTF-8, an enumeration constant outside its list, name
 *   identifier 0, a character map with a codepoint twice);
 * - SXML_STATUS_UNSUPPORTED_FEATURE for a method this version of sxml does
 *   not implement yet;
 * - the failures of output_stream and of the heap.
 * On failure part of the output may have been written.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_serialize_sequence(
    sxml_item_heap *item_heap, const sxml_serialization_request *request,
    sxml_output_stream output_stream);
