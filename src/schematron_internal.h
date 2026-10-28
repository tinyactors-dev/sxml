#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/schema.h"
#include "sxml/schematron.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xslt.h"
#include "xml_internal.h"
#include "xslt_ast_internal.h"

/*
 * Schematron as a front end to the XSLT AST (ISO/IEC 19757-3:2025;
 * docs/DECISIONS.md, D313, D315, D323). Internal to sxml. Clause numbers
 * are of the standard.
 *
 * One compilation (sxml_schematron_compilation) goes through four files,
 * each a step of D315's path:
 * - src/schematron_read.c reads the schematron documents (the compiled one
 *   and those sch:include and sch:extends href reach, 6.2), checks the
 *   grammar of Annex A and the constraints of Annex B as it walks every
 *   element once, and records the schematron index: the schema's
 *   top-level elements by kind, in schema order, with tables of ids. Its
 *   effective-child cursor (sxml_schematron_cursor) puts included
 *   elements and the children of external declarations and abstract
 *   rules in place of the element that names them, without a copy.
 * - src/schematron_expand.c replaces placeholders in an expression's text
 *   (the instance's parameter table, then the schema's), recording the
 *   expansion segments.
 * - src/schematron_build.c chooses the phase and builds the stylesheet AST
 *   (src/xslt_ast_internal.h) and the schematron table: one mode per
 *   active schematron pattern, one template rule per schematron rule, the
 *   assertions as conditions that add a finding item to the raw result.
 * - src/schematron.c is the public API: it runs the three, compiles the
 *   AST (sxml_xslt_stylesheet_create_from_ast) and turns finding items into
 *   schematron reports; src/schematron_svrl.c writes SVRL from them.
 *
 * Nothing here recurses (D71): every walk keeps an explicit stack in
 * allocator memory.
 *
 * Thread safety: the thread of the processor (D35).
 */

/* The namespace of Schematron's own error codes and of the names the
   builder synthesizes (modes, variables). */
#define SXML_SCHEMATRON_NAMESPACE_URI "urn:sxml:schematron"
#define SXML_SCHEMATRON_SCHEMA_NAMESPACE_URI "http://purl.oclc.org/dsdl/schematron"

/* ------------------------------------------------------------- Elements */

/* The elements of the Schematron namespace (Annex A), FOREIGN for any
   other element. */
typedef enum sxml_schematron_element_kind {
    SXML_SCHEMATRON_ELEMENT_NONE = 0,
    SXML_SCHEMATRON_ELEMENT_ACTIVE,
    SXML_SCHEMATRON_ELEMENT_ASSERT,
    SXML_SCHEMATRON_ELEMENT_DIAGNOSTIC,
    SXML_SCHEMATRON_ELEMENT_DIAGNOSTICS,
    SXML_SCHEMATRON_ELEMENT_DIR,
    SXML_SCHEMATRON_ELEMENT_EMPH,
    SXML_SCHEMATRON_ELEMENT_EXTENDS,
    SXML_SCHEMATRON_ELEMENT_GROUP,
    SXML_SCHEMATRON_ELEMENT_INCLUDE,
    SXML_SCHEMATRON_ELEMENT_LET,
    SXML_SCHEMATRON_ELEMENT_LIBRARY,
    SXML_SCHEMATRON_ELEMENT_NAME,
    SXML_SCHEMATRON_ELEMENT_NS,
    SXML_SCHEMATRON_ELEMENT_P,
    SXML_SCHEMATRON_ELEMENT_PARAM,
    SXML_SCHEMATRON_ELEMENT_PATTERN,
    SXML_SCHEMATRON_ELEMENT_PHASE,
    SXML_SCHEMATRON_ELEMENT_PROPERTIES,
    SXML_SCHEMATRON_ELEMENT_PROPERTY,
    SXML_SCHEMATRON_ELEMENT_REPORT,
    SXML_SCHEMATRON_ELEMENT_RULE,
    SXML_SCHEMATRON_ELEMENT_RULES,
    SXML_SCHEMATRON_ELEMENT_SCHEMA,
    SXML_SCHEMATRON_ELEMENT_SPAN,
    SXML_SCHEMATRON_ELEMENT_TITLE,
    SXML_SCHEMATRON_ELEMENT_VALUE_OF,
    /* An element in the Schematron namespace that Annex A does not
       define. */
    SXML_SCHEMATRON_ELEMENT_UNKNOWN,
    /* An element in another namespace (5.2). */
    SXML_SCHEMATRON_ELEMENT_FOREIGN,
    SXML_SCHEMATRON_ELEMENT_KIND_COUNT,
} sxml_schematron_element_kind;

/* The attributes in no namespace that Annex A names, and the two of the
   XML namespace it allows. */
typedef enum sxml_schematron_attribute_kind {
    SXML_SCHEMATRON_ATTRIBUTE_NONE = 0,
    SXML_SCHEMATRON_ATTRIBUTE_ABSTRACT,
    SXML_SCHEMATRON_ATTRIBUTE_AS,
    SXML_SCHEMATRON_ATTRIBUTE_CLASS,
    SXML_SCHEMATRON_ATTRIBUTE_CONTEXT,
    SXML_SCHEMATRON_ATTRIBUTE_DEFAULT_PHASE,
    SXML_SCHEMATRON_ATTRIBUTE_DIAGNOSTICS,
    SXML_SCHEMATRON_ATTRIBUTE_DOCUMENTS,
    SXML_SCHEMATRON_ATTRIBUTE_FLAG,
    SXML_SCHEMATRON_ATTRIBUTE_FPI,
    SXML_SCHEMATRON_ATTRIBUTE_FROM,
    SXML_SCHEMATRON_ATTRIBUTE_HREF,
    SXML_SCHEMATRON_ATTRIBUTE_ICON,
    SXML_SCHEMATRON_ATTRIBUTE_ID,
    SXML_SCHEMATRON_ATTRIBUTE_IS_A,
    SXML_SCHEMATRON_ATTRIBUTE_NAME,
    SXML_SCHEMATRON_ATTRIBUTE_PATH,
    SXML_SCHEMATRON_ATTRIBUTE_PATTERN,
    SXML_SCHEMATRON_ATTRIBUTE_PREFIX,
    SXML_SCHEMATRON_ATTRIBUTE_PROPERTIES,
    SXML_SCHEMATRON_ATTRIBUTE_QUERY_BINDING,
    SXML_SCHEMATRON_ATTRIBUTE_ROLE,
    SXML_SCHEMATRON_ATTRIBUTE_RULE,
    SXML_SCHEMATRON_ATTRIBUTE_SCHEMA_VERSION,
    SXML_SCHEMATRON_ATTRIBUTE_SCHEMATRON_EDITION,
    SXML_SCHEMATRON_ATTRIBUTE_SCHEME,
    SXML_SCHEMATRON_ATTRIBUTE_SEE,
    SXML_SCHEMATRON_ATTRIBUTE_SELECT,
    SXML_SCHEMATRON_ATTRIBUTE_SEVERITY,
    SXML_SCHEMATRON_ATTRIBUTE_SUBJECT,
    SXML_SCHEMATRON_ATTRIBUTE_TEST,
    SXML_SCHEMATRON_ATTRIBUTE_URI,
    SXML_SCHEMATRON_ATTRIBUTE_VALUE,
    SXML_SCHEMATRON_ATTRIBUTE_VISIT_EACH,
    SXML_SCHEMATRON_ATTRIBUTE_WHEN,
    /* xml:lang and xml:space (the rich attributes); xml:base is read for
       base URI fixup (6.8). */
    SXML_SCHEMATRON_ATTRIBUTE_XML_LANG,
    SXML_SCHEMATRON_ATTRIBUTE_XML_SPACE,
    SXML_SCHEMATRON_ATTRIBUTE_XML_BASE,
    SXML_SCHEMATRON_ATTRIBUTE_KIND_COUNT,
} sxml_schematron_attribute_kind;

/* An element of a schematron document: the document's index in the
   compilation and the element's node in its tree. */
typedef struct sxml_schematron_place {
    uint32_t document;
    sxml_node_index element;
} sxml_schematron_place;

/* An attribute found on an element. */
typedef struct sxml_schematron_attribute {
    bool is_present;
    /* As written, untrimmed; borrowed from the document's tree. */
    sxml_byte_view value;
    /* Where the attribute is in its document (line, column, offset of the
       attribute; byte_count 0). */
    sxml_ast_source_span source_span;
} sxml_schematron_attribute;

/* ------------------------------------------------------------ Documents */

/* One schematron document of the compilation. */
typedef struct sxml_schematron_document {
    /* The document node, retained. */
    sxml_item document_node;
    sxml_tree *tree;
    /* The absolute URI it was read from (or the configuration's
       document_uri for the compiled one); an allocation of its own, never
       moved, released with the compilation. */
    sxml_byte_view uri;
    /* The builder's: the AST module of the document (module 1 for the
       compiled one) and the static contexts of its expressions and of its
       XSLT declarations. */
    sxml_xslt_ast_module_index module;
    sxml_xslt_ast_static_context_index static_context;
    sxml_xslt_ast_static_context_index declaration_context;
} sxml_schematron_document;

/* -------------------------------------------------------------- Index */

/* An element with an id, for the id tables. */
typedef struct sxml_schematron_identified {
    sxml_schematron_place place;
    /* Trimmed; borrowed from the document's tree. */
    sxml_byte_view identifier;
} sxml_schematron_identified;

/* A growable array of the compilation, element type the user's. */
typedef struct sxml_schematron_array {
    void *elements;
    size_t count;
    size_t capacity;
} sxml_schematron_array;

/*
 * The schematron index (glossary `schematron_index`): what reading found
 * at the top of the schema and in its libraries, in schema order (every
 * array of sxml_schematron_place unless noted).
 */
typedef struct sxml_schematron_index {
    sxml_schematron_place schema;
    /* The first sch:title of the schema; document UINT32_MAX for none. */
    sxml_schematron_place title;
    bool has_title;
    sxml_schematron_array namespaces;
    sxml_schematron_array schema_parameters;
    sxml_schematron_array schema_lets;
    /* sxml_schematron_identified, every one with an id. */
    sxml_schematron_array phases;
    /* sch:pattern and sch:group, abstract or not, instances too. */
    sxml_schematron_array patterns;
    /* sxml_schematron_identified: abstract rules (in sch:rules or in a
       pattern), abstract patterns, diagnostics, properties. */
    sxml_schematron_array abstract_rules;
    sxml_schematron_array abstract_patterns;
    sxml_schematron_array diagnostics;
    sxml_schematron_array properties;
    /* sxml_schematron_identified: patterns and groups with an id that are
       not abstract (for sch:active). */
    sxml_schematron_array pattern_identifiers;
    /* XSLT declarations of the schema read as XSLT (Annexes H, J). */
    sxml_schematron_array foreign_declarations;
} sxml_schematron_index;

/* ------------------------------------------------------------ The table */

/* The kinds of what the table records. */
typedef enum sxml_schematron_entry_kind {
    SXML_SCHEMATRON_ENTRY_ASSERT = 0,
    SXML_SCHEMATRON_ENTRY_REPORT,
    SXML_SCHEMATRON_ENTRY_RULE,
} sxml_schematron_entry_kind;

/* Bits of an entry's dynamic attribute values: which of role, flag and
   severity a finding item carries, in that order. */
enum {
    SXML_SCHEMATRON_DYNAMIC_ROLE = 1U << 0U,
    SXML_SCHEMATRON_DYNAMIC_FLAG = 1U << 1U,
    SXML_SCHEMATRON_DYNAMIC_SEVERITY = 1U << 2U,
};

/* A run of the table's text store. */
typedef struct sxml_schematron_text {
    uint32_t byte_offset;
    uint32_t byte_count;
} sxml_schematron_text;

/* A run of the table's reference list (diagnostic or property indices). */
typedef struct sxml_schematron_reference_list {
    uint32_t first_reference;
    uint32_t reference_count;
} sxml_schematron_reference_list;

/*
 * Everything static of one assertion or rule (glossary
 * `schematron_table`): what its finding item does not carry.
 */
typedef struct sxml_schematron_entry {
    sxml_schematron_entry_kind entry_kind;
    /* Bits SXML_SCHEMATRON_DYNAMIC_*. */
    uint32_t dynamic_mask;
    sxml_schematron_text identifier;
    sxml_schematron_text role;
    sxml_schematron_text flag;
    sxml_schematron_text severity;
    sxml_schematron_text see;
    sxml_schematron_text icon;
    sxml_schematron_text formal_public_identifier;
    sxml_schematron_text language;
    /* Assertions: the test as compiled. Rules: the title (SVRL's name). */
    sxml_schematron_text test;
    /* The rule's context as compiled, and its visit-each. */
    sxml_schematron_text context;
    sxml_schematron_text visit_each;
    /* Into the table's patterns. */
    uint32_t pattern;
    /* Assertions: indices into the table's diagnostics and properties. */
    sxml_schematron_reference_list diagnostics;
    sxml_schematron_reference_list properties;
    /* The element in its document: document index, line, column, offset. */
    uint32_t document;
    sxml_ast_source_span source_span;
    /* The subject attribute's place, for XPTY0004 when it selects other
       than one node; line 0 without one. */
    sxml_ast_source_span subject_span;
    uint32_t subject_document;
    /* xml:space="preserve" is in scope of the message: whitespace is not
       collapsed (5.3). */
    bool preserves_space;
} sxml_schematron_entry;

/* An active schematron pattern. */
typedef struct sxml_schematron_pattern_entry {
    sxml_schematron_text identifier;
    sxml_schematron_text name;
    sxml_schematron_text role;
    sxml_schematron_text documents;
    bool is_group;
} sxml_schematron_pattern_entry;

/* A diagnostic or property an assertion names. */
typedef struct sxml_schematron_annotation_entry {
    sxml_schematron_text identifier;
    /* Diagnostics: xml:lang in scope. Properties: role. */
    sxml_schematron_text language;
    sxml_schematron_text role;
    sxml_schematron_text scheme;
    bool preserves_space;
} sxml_schematron_annotation_entry;

/* A phase "#ANY" can choose: its id and its active patterns (indices
   into the table's patterns). */
typedef struct sxml_schematron_phase_entry {
    sxml_schematron_text identifier;
    sxml_schematron_reference_list patterns;
} sxml_schematron_phase_entry;

/* The table a compilation makes and the schematron keeps. */
typedef struct sxml_schematron_table {
    sxml_schematron_entry *entries;
    size_t entry_count;
    sxml_schematron_pattern_entry *patterns;
    size_t pattern_count;
    sxml_schematron_annotation_entry *diagnostics;
    size_t diagnostic_count;
    sxml_schematron_annotation_entry *properties;
    size_t property_count;
    uint32_t *references;
    size_t reference_count;
    /* Under #ANY: the phases with a when, in schema order; a phase item n
       names phases[n - 1]. */
    sxml_schematron_phase_entry *phases;
    size_t phase_count;
    /* The sch:ns bindings, prefix then URI (SVRL's
       ns-prefix-in-attribute-values). */
    sxml_schematron_text *namespace_texts;
    size_t namespace_count;
    sxml_schematron_text title;
    sxml_schematron_text schema_version;
    sxml_schematron_text schematron_edition;
    sxml_schematron_text query_binding;
    sxml_schematron_text phase;
    bool has_schematron_edition;
    /* The phase is #ANY: the raw result starts with the phase's number. */
    bool selects_phase;
    uint8_t *text_bytes;
    size_t text_byte_count;
    size_t text_byte_capacity;
} sxml_schematron_table;

/* The bytes of text in the table's store. */
static inline sxml_byte_view sxml_schematron_table_text(const sxml_schematron_table *table,
                                                        sxml_schematron_text text) {
    if (text.byte_count == 0 || (size_t)text.byte_offset + text.byte_count > table->text_byte_count) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return (sxml_byte_view){.bytes = table->text_bytes + text.byte_offset,
                            .byte_count = text.byte_count};
}

/* The bytes a buffer holds, borrowed until it next grows. */
static inline sxml_byte_view sxml_schematron_buffer_text(const sxml_xml_buffer *buffer) {
    return (sxml_byte_view){.bytes = buffer->bytes, .byte_count = buffer->byte_count};
}

/* ---------------------------------------------------------- Compilation */

/* Names the compilation interns once. */
typedef struct sxml_schematron_names {
    sxml_namespace_uri_identifier schematron_namespace;
    sxml_namespace_uri_identifier private_namespace;
    sxml_name_identifier elements[SXML_SCHEMATRON_ELEMENT_KIND_COUNT];
    sxml_name_identifier attributes[SXML_SCHEMATRON_ATTRIBUTE_KIND_COUNT];
} sxml_schematron_names;

/* What the query binding makes of the schema (6.5). */
typedef struct sxml_schematron_binding {
    /* The XSLT version of the AST: 2.0, 3.0 or 4.0. */
    double version;
    /* xslt, xslt2, xslt3, xslt4 or none: XSLT declarations are read, flag,
       role and severity are attribute value templates. */
    bool is_xslt;
    /* xslt2: no xsl:accumulator, no xsl:use-package (Annex H). */
    bool is_xslt_2;
} sxml_schematron_binding;

/*
 * One compilation. Made by src/schematron.c; read by
 * src/schematron_read.c, then src/schematron_build.c fills the AST and the
 * table.
 */
typedef struct sxml_schematron_compilation {
    sxml_processor *processor;
    sxml_allocator allocator;
    sxml_name_pool *name_pool;
    const sxml_schematron_configuration *configuration;
    sxml_xml_parser *parser;
    sxml_schematron_names names;
    sxml_schematron_binding binding;
    /* sxml_schematron_document. */
    sxml_schematron_array documents;
    sxml_schematron_index index;
    /* Errors so far, and whether the sink stopped the compilation. */
    size_t error_count;
    /* sxml_schema_report: the code, document and place of every error
       given to the sink, so that an element read twice (an abstract rule
       or pattern used twice) reports its error once. */
    sxml_schematron_array reported;
    sxml_status stop_status;
    /* The AST being built; NULL before the builder. */
    sxml_xslt_ast *ast;
    sxml_schematron_table table;
    /* The phase compiled: "#ALL", "#ANY" or a phase id; borrowed from a
       document or static. */
    sxml_byte_view phase_id;
    /* Scratch for messages. */
    sxml_xml_buffer description;
} sxml_schematron_compilation;

/* An error of the compilation. */
typedef struct sxml_schematron_error {
    /* "syntax", "reference", ... in the namespace urn:sxml:schematron; or
       with is_xpath an XPath or XSLT code in the err namespace. Static
       text. */
    const char *code;
    bool is_xpath;
    /* Static text. */
    const char *description;
    /* Shown after the description in the sink's text; may be empty. */
    sxml_byte_view subject;
    /* The document and the place in it. */
    uint32_t document;
    sxml_ast_source_span source_span;
} sxml_schematron_error;

/*
 * Reports error: to the configuration's report sink, and, the first, into
 * its error record. Returns SXML_STATUS_OK when compilation goes on (a
 * sink), SXML_STATUS_STATIC_ERROR with the null sink, the sink's status
 * when it stops. Defined in src/schematron_read.c.
 */
SXML_NODISCARD sxml_status sxml_schematron_report_error(sxml_schematron_compilation *compilation,
                                                        const sxml_schematron_error *error);

/* The first error the error record holds, when one came from an XPath or
   XSLT front end into compilation's record: gives it to the sink as is.
   Same returns. */
SXML_NODISCARD sxml_status sxml_schematron_report_record(sxml_schematron_compilation *compilation,
                                                         const sxml_error_record *record);

/* An element to append: its bytes and alignment. */
typedef struct sxml_schematron_array_element {
    const void *element;
    size_t element_size;
    size_t element_alignment;
} sxml_schematron_array_element;

SXML_NODISCARD sxml_status sxml_schematron_array_push(sxml_schematron_compilation *compilation,
                                                      sxml_schematron_array *array,
                                                      sxml_schematron_array_element element);

/* Releases an array's memory. */
void sxml_schematron_array_release(sxml_schematron_compilation *compilation,
                                   sxml_schematron_array *array);

/* The document at index, borrowed until the next document is added. */
sxml_schematron_document *sxml_schematron_document_at(
    const sxml_schematron_compilation *compilation, uint32_t document);

/* The kind of an element (FOREIGN for elements of other namespaces, NONE
   for other node kinds). */
sxml_schematron_element_kind sxml_schematron_element_kind_of(
    const sxml_schematron_compilation *compilation, sxml_schematron_place place);

/* The attribute of kind on the element; is_present false when it has
   none. */
typedef struct sxml_schematron_attribute_query {
    sxml_schematron_place place;
    sxml_schematron_attribute_kind attribute_kind;
} sxml_schematron_attribute_query;

sxml_schematron_attribute sxml_schematron_find_attribute(
    const sxml_schematron_compilation *compilation, sxml_schematron_attribute_query query);

/* The location of an element (its start tag). */
sxml_ast_source_span sxml_schematron_element_span(const sxml_schematron_compilation *compilation,
                                                  sxml_schematron_place place);

/* The value of xml:lang in scope of an element in its own document (6.7),
   and whether xml:space="preserve" is; borrowed from the tree. */
typedef struct sxml_schematron_inherited {
    sxml_byte_view language;
    bool preserves_space;
} sxml_schematron_inherited;

sxml_schematron_inherited sxml_schematron_inherited_of(
    const sxml_schematron_compilation *compilation, sxml_schematron_place place);

/* ------------------------------------------------------ Effective children */

/*
 * A cursor over an element's effective children (glossary
 * `effective_child`): its child elements, with each sch:include replaced
 * by the element it names (5.4.6), each sch:extends href by the children
 * of the external declaration it names (5.4.4) and, when the cursor
 * expands rules, each sch:extends rule by the children of the abstract
 * rule (6.2). Text children are skipped (the grammar checks them, not
 * the cursor). An inclusion that cannot be made is reported and left
 * out; one that would put an element inside itself is
 * schematron:include. Defined in src/schematron_read.c.
 */
typedef struct sxml_schematron_cursor {
    sxml_schematron_compilation *compilation;
    /* Frames: the element whose children are iterated and the next child.
       Owned arrays. */
    sxml_schematron_array frames;
    bool expands_rules;
} sxml_schematron_cursor;

/* Starts a cursor over parent's effective children. */
SXML_NODISCARD sxml_status sxml_schematron_cursor_start(sxml_schematron_cursor *cursor,
                                                        sxml_schematron_place parent);

/* The next effective child, or has_child false at the end. */
typedef struct sxml_schematron_child {
    bool has_child;
    sxml_schematron_place place;
    sxml_schematron_element_kind element_kind;
    /* Put in place of an sch:extends (an external declaration's or an
       abstract rule's child): its position may break the grammar's order
       (5.4.4). */
    bool is_inserted;
    /* Put in place of an sch:include, which is included_by; the grammar
       must allow an include there (Annex A, "inclusion"). */
    bool is_included;
    sxml_schematron_place included_by;
} sxml_schematron_child;

SXML_NODISCARD sxml_status sxml_schematron_cursor_next(sxml_schematron_cursor *cursor,
                                                       sxml_schematron_child *child);

/* Releases the cursor's memory. */
void sxml_schematron_cursor_finish(sxml_schematron_cursor *cursor);

/* Finds an id in an id table (an array of sxml_schematron_identified);
   NULL when none. */
const sxml_schematron_identified *sxml_schematron_find_identified(
    const sxml_schematron_array *table, sxml_byte_view identifier);

/* ------------------------------------------------------------- Reading */

/*
 * Reads the configured document and every document it reaches, checks
 * them and fills compilation->documents and compilation->index (step 3 of
 * D315's path). The query binding is read first; one sxml does not read
 * ends reading. Returns SXML_STATUS_OK also when errors went to the sink
 * (compilation->error_count says), the null sink's or the sink's status
 * when they stop, SXML_STATUS_MALFORMED_INPUT, INVALID_UTF8 or
 * UNSUPPORTED_FEATURE for a compiled document that is not well-formed (the
 * record's diagnostic filled), and the allocator's, the parser's and the
 * resolver's failures.
 */
SXML_NODISCARD sxml_status sxml_schematron_read(sxml_schematron_compilation *compilation);

/* Releases what reading made: documents, index arrays, the parser. */
void sxml_schematron_read_release(sxml_schematron_compilation *compilation);

/* ------------------------------------------------------- Placeholders */

/* A parameter that placeholders name. */
typedef struct sxml_schematron_parameter_value {
    /* An NCName, trimmed. */
    sxml_byte_view name;
    /* The query fragment. */
    sxml_byte_view value;
    /* Where the value is written: the sch:param's value attribute
       (document and span), or, for a configured parameter, none
       (is_configured). */
    uint32_t document;
    sxml_ast_source_span source_span;
    bool is_configured;
} sxml_schematron_parameter_value;

typedef struct sxml_schematron_parameter_table {
    const sxml_schematron_parameter_value *values;
    size_t value_count;
} sxml_schematron_parameter_table;

/* A run of an expanded text that comes from one place (glossary
   `expansion_segment`). */
typedef struct sxml_schematron_segment {
    /* Its first byte in the expanded text. */
    uint32_t expanded_offset;
    /* NULL: the attribute's own text, from source_offset. Otherwise the
       parameter whose value it is, from source_offset in that value. */
    const sxml_schematron_parameter_value *parameter;
    uint32_t source_offset;
} sxml_schematron_segment;

/* A text to expand: the instance's table first, then the schema's. */
typedef struct sxml_schematron_expansion_request {
    sxml_byte_view text;
    sxml_schematron_parameter_table instance_parameters;
    sxml_schematron_parameter_table schema_parameters;
    /* A pattern's renamed variables (D323), replaced as schema
       parameters are, after them. */
    sxml_schematron_parameter_table renamed_variables;
} sxml_schematron_expansion_request;

/* An expanded text and its segments; owned, release with
   sxml_schematron_expansion_release. */
typedef struct sxml_schematron_expansion {
    sxml_xml_buffer text;
    sxml_schematron_segment *segments;
    size_t segment_count;
    size_t segment_capacity;
    /* A placeholder was replaced. */
    bool has_placeholder;
    sxml_allocator allocator;
} sxml_schematron_expansion;

/*
 * Copies request->text into expansion, replacing each placeholder: at each
 * "$", the run of NCName characters after it (the parameter name token)
 * is looked up whole in the instance's table, then the schema's, and its
 * value copied instead (Annexes C, H, J); a token that names no parameter
 * stays as it is. A value is not scanned again, except that an instance
 * parameter's value is scanned once for schema parameters. Defined in
 * src/schematron_expand.c. Returns the allocator's failures.
 */
SXML_NODISCARD sxml_status sxml_schematron_expand(sxml_allocator allocator,
                                                  const sxml_schematron_expansion_request *request,
                                                  sxml_schematron_expansion *expansion);

void sxml_schematron_expansion_release(sxml_schematron_expansion *expansion);

/* Where an offset of the expanded text was written: its segment's
   parameter (NULL for the attribute) and the offset there. */
typedef struct sxml_schematron_expansion_origin {
    const sxml_schematron_parameter_value *parameter;
    uint32_t source_offset;
} sxml_schematron_expansion_origin;

sxml_schematron_expansion_origin sxml_schematron_expansion_origin_of(
    const sxml_schematron_expansion *expansion, uint32_t expanded_offset);

/* --------------------------------------------------------------- Building */

/*
 * Chooses the phase, builds the stylesheet AST into compilation->ast and
 * the table into compilation->table (steps 2 and 3 of D315's path), every
 * error to the sink. Returns as sxml_schematron_read. Defined in
 * src/schematron_build.c.
 */
SXML_NODISCARD sxml_status sxml_schematron_build(sxml_schematron_compilation *compilation);

/* Releases the table's arrays. */
void sxml_schematron_table_release(sxml_allocator allocator, sxml_schematron_table *table);

/* ------------------------------------------------------------------ SVRL */

/* What the SVRL writer needs of a compiled schematron. */
typedef struct sxml_schematron_svrl_writer {
    sxml_output_stream output_stream;
    const sxml_schematron_table *table;
    /* The phase in force. */
    sxml_byte_view phase_id;
    /* Under "#ANY", the patterns of the phase chosen (indices into the
       table's references); otherwise every pattern of the table. */
    sxml_schematron_reference_list phase_patterns;
    bool has_phase_patterns;
    /* How many of the active patterns have their active-pattern element
       written. */
    size_t open_pattern;
    sxml_xml_buffer buffer;
    sxml_status status;
} sxml_schematron_svrl_writer;

/* Starts SVRL: the schematron-output start tag and the namespaces.
   Defined in src/schematron_svrl.c. */
SXML_NODISCARD sxml_status sxml_schematron_svrl_start(sxml_schematron_svrl_writer *writer);

/* Writes one report (a fired rule, a failed assert or a successful
   report), with the active-pattern element first when it starts a
   pattern. entry is the report's entry in the table. */
typedef struct sxml_schematron_svrl_report {
    const sxml_schematron_report *report;
    const sxml_schematron_entry *entry;
    /* A fired rule's subordinate document: its URI, empty otherwise. */
    sxml_byte_view document_uri;
} sxml_schematron_svrl_report;

SXML_NODISCARD sxml_status sxml_schematron_svrl_write(sxml_schematron_svrl_writer *writer,
                                                      const sxml_schematron_svrl_report *report);

/* Ends SVRL: the active patterns no report named, then the end tag. */
SXML_NODISCARD sxml_status sxml_schematron_svrl_finish(sxml_schematron_svrl_writer *writer);

/* Releases the writer's buffer. */
void sxml_schematron_svrl_release(sxml_schematron_svrl_writer *writer);
