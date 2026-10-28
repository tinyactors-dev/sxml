#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sxml/export.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"

/*
 * Schematron: compiling a Schematron schema and validating documents with
 * it (ISO/IEC 19757-3:2025, the fourth edition; docs/DECISIONS.md, D313,
 * D315).
 *
 * A Schematron schema is compiled once, for one phase, from its
 * schematron document and every document its sch:include and sch:extends
 * href elements reach, into an sxml_schematron. Compiling reads the
 * documents into the stylesheet AST of XSLT (one mode per active
 * schematron pattern, one template rule per schematron rule, the
 * assertions as conditions in its body) and compiles that as a stylesheet
 * is compiled: no XSLT text is generated, and errors point into the
 * schematron documents. Abstract patterns, abstract rules and sch:include
 * are expanded while reading, without an expanded copy of the documents.
 *
 * The schema is then run over one node at a time (a document node
 * usually), any number of times: every finding (a failed assert or a
 * fired report) goes to a report sink as an sxml_schematron_report with
 * the assertion's attributes, its message, its diagnostics and
 * properties, the location of the assertion in its schematron document
 * and the subject node with its location in the instance. SVRL is one
 * output written from those reports:
 *
 *     sxml_schematron *schematron = NULL;
 *     status = sxml_schematron_create(
 *         allocator,
 *         &(sxml_schematron_configuration){
 *             .processor = processor,
 *             .document_text = schema_bytes,
 *             .document_uri = schema_uri,
 *             .error_record = &error_record,
 *         },
 *         &schematron);
 *     ...
 *     sxml_schematron_outcome outcome;
 *     status = sxml_schematron_validate_node(
 *         schematron,
 *         &(sxml_schematron_validation_request){
 *             .node = document,
 *             .report_sink = report_sink,
 *             .error_record = &error_record,
 *         },
 *         &outcome);
 *
 * Query bindings (the queryBinding attribute, D313): xslt2, xslt3,
 * xpath2, xpath3 and xpath31 are read, and a schema without one as
 * xslt3. Expressions are XPath 4.0 and rule contexts XSLT 4.0 patterns
 * for every binding. Under xslt2 and xslt3 the xsl:function, xsl:key,
 * xsl:include, xsl:import and xsl:import-schema children of sch:schema,
 * and under xslt3 also xsl:accumulator and xsl:use-package, are read as
 * XSLT 4.0 declarations of the compiled stylesheet, and xsl:copy-of in a
 * sch:property as an instruction; other XSLT elements, and all of them
 * under the xpath bindings, are foreign elements, which Schematron
 * ignores. Any
 * other binding (xslt, xpath, xquery, xquery3, xquery31, ...) fails the
 * compilation with the error schematron:query-binding at the attribute.
 *
 * Expressions see the namespaces the sch:ns elements declare, besides
 * XPath's predeclared ones, and not the XML namespace declarations of the
 * schematron documents (an XSLT declaration sees both).
 *
 * Errors: errors of Schematron itself carry codes in the namespace
 * urn:sxml:schematron, shown with the prefix "schematron" (the standard
 * names none):
 * - schematron:query-binding: a query binding sxml does not read;
 * - schematron:not-schematron: a document element that is not
 *   sch:schema;
 * - schematron:include: a document that sch:include or sch:extends href
 *   names cannot be read, is not well-formed, has no element of the
 *   fragment identifier, or includes itself;
 * - schematron:reference: an id that names nothing of its kind (an
 *   active pattern, is-a, extends rule, a diagnostic, a property);
 * - schematron:phase: the configuration names a phase the schema does
 *   not have;
 * - schematron:variable: a variable defined twice in one scope;
 * - schematron:syntax: an element of the Schematron namespace where the
 *   schema does not allow it, or without an attribute it needs;
 * - schematron:unsupported: a construct of the fourth edition sxml does
 *   not have yet (see D315).
 * Errors in expressions keep the codes of XPath and XSLT (XPST0003,
 * XPST0008, XTSE0010, ...). Every error's location names the schematron
 * document's absolute URI with the line and column of the element or
 * attribute, or of the position inside the expression; a syntax error in
 * text a placeholder put into an expression points into the sch:param's
 * value. Dynamic errors of a validation (FORG0001, XPTY0004, ...) stop it,
 * located at the expression in its schematron document.
 *
 * Thread safety: the thread of the processor (D35).
 */

/* ------------------------------------------------------------- Reports */

/* What a report reports. The list only grows. */
typedef enum sxml_schematron_report_kind {
    /* An assert whose test was false for a context node: a finding. */
    SXML_SCHEMATRON_REPORT_KIND_FAILED_ASSERT = 0,
    /* A report whose test was true for a context node: a finding. */
    SXML_SCHEMATRON_REPORT_KIND_SUCCESSFUL_REPORT,
    /* A rule applied to a context node; only when the validation request
       asks (reports_fired_rules). */
    SXML_SCHEMATRON_REPORT_KIND_FIRED_RULE,
} sxml_schematron_report_kind;

/* A diagnostic the assertion names, evaluated for its context node. */
typedef struct sxml_schematron_diagnostic {
    /* The sch:diagnostic's id. */
    sxml_byte_view diagnostic_id;
    /* xml:lang in scope of the sch:diagnostic; empty when none. */
    sxml_byte_view language;
    /* Its content: text, with sch:value-of and sch:name replaced by their
       values and the markup of sch:emph, sch:dir, sch:span and foreign
       elements reduced to its text. */
    sxml_byte_view text;
} sxml_schematron_diagnostic;

typedef struct sxml_schematron_diagnostic_view {
    const sxml_schematron_diagnostic *diagnostics;
    size_t diagnostic_count;
} sxml_schematron_diagnostic_view;

/* A property the assertion names, evaluated for its context node. */
typedef struct sxml_schematron_property {
    /* The sch:property's id, role and scheme attributes; empty when
       absent. */
    sxml_byte_view property_id;
    sxml_byte_view role;
    sxml_byte_view scheme;
    /* Its content, as sxml_schematron_diagnostic.text; xsl:copy-of
       contributes the string value of what it selects. */
    sxml_byte_view text;
} sxml_schematron_property;

typedef struct sxml_schematron_property_view {
    const sxml_schematron_property *properties;
    size_t property_count;
} sxml_schematron_property_view;

/*
 * One finding, or one fired rule, as a report sink receives it.
 * Everything it refers to is borrowed for the call: views into the
 * compiled schema stay valid while it lives, the evaluated texts and the
 * node only for the call (retain the node to keep it).
 */
typedef struct sxml_schematron_report {
    sxml_schematron_report_kind report_kind;
    /* The attributes of the assertion (FIRED_RULE: of the rule) as
       written; each empty when absent. Not inherited from the rule or
       the pattern. */
    sxml_byte_view id;
    sxml_byte_view role;
    sxml_byte_view flag;
    sxml_byte_view see;
    sxml_byte_view icon;
    /* The fpi attribute. */
    sxml_byte_view formal_public_identifier;
    /* xml:lang in scope of the assertion (the rule); empty when none. */
    sxml_byte_view language;
    /* The assertion's test as compiled, placeholders replaced (SVRL's
       test attribute); empty for FIRED_RULE. */
    sxml_byte_view test_text;
    /* The rule's context as compiled, placeholders replaced (SVRL's
       context attribute of fired-rule). */
    sxml_byte_view context_text;
    /* The id of the active pattern (for a pattern instance, the
       instance's) and its position among the active patterns, from 0, in
       the order of the schema. */
    sxml_byte_view pattern_id;
    size_t pattern_index;
    /* The message: the assertion's content, evaluated as
       sxml_schematron_diagnostic.text is. Empty for FIRED_RULE. */
    sxml_byte_view message;
    /* The diagnostics and properties the assertion names, in the order
       its diagnostics and properties attributes give them; empty for
       FIRED_RULE. */
    sxml_schematron_diagnostic_view diagnostics;
    sxml_schematron_property_view properties;
    /* The assertion's (FIRED_RULE: the rule's) element in its schematron
       document: the document's absolute URI as source_name, line and
       column of its start tag. Inside an abstract pattern or an abstract
       rule, the element there; pattern_id names the instance. */
    sxml_source_location schema_location;
    /* The subject node (FIRED_RULE: the context node), in the item heap
       of the schema's processor. */
    sxml_item node;
    /* The node's location as its tree keeps it: the document URI as
       source_name, line and column; line_number 0 when the tree keeps no
       locations. */
    sxml_source_location node_location;
    /* The node's path, as fn:path gives it (F&O 4.0, "fn:path"): SVRL's
       location attribute. */
    sxml_byte_view node_path;
    /* An error finding (D313): a FAILED_ASSERT or SUCCESSFUL_REPORT whose
       role or flag is "fatal" or "error", or that has neither. false for
       FIRED_RULE. */
    bool is_error;
} sxml_schematron_report;

/* The operations of a report sink. Tables are static const. */
typedef struct sxml_schematron_report_sink_operations {
    /* Receives one report, in the order the validation makes them (see
       sxml_schematron_validate_node). Returning SXML_STATUS_OK lets the
       validation go on; any other status stops it with that status. */
    sxml_status (*receive)(void *state, const sxml_schematron_report *report);
} sxml_schematron_report_sink_operations;

/*
 * The report sink interface, passed by value. A sink whose operations is
 * NULL is the null sink: reports are counted in the outcome and
 * discarded.
 */
typedef struct sxml_schematron_report_sink {
    void *state;
    const sxml_schematron_report_sink_operations *operations;
} sxml_schematron_report_sink;

/* ----------------------------------------------------------- Schematron */

/*
 * A Schematron schema compiled for one processor and one phase. Its fixed
 * part lives in the memory given to initialize (or allocated by create);
 * its compiled form is allocated from the processor's allocator and lives
 * until it is finalized.
 */
typedef struct sxml_schematron sxml_schematron;

/* Where the schematron document to compile comes from. The list only
   grows. */
typedef enum sxml_schematron_document_source {
    /* document_text holds it. */
    SXML_SCHEMATRON_DOCUMENT_SOURCE_TEXT = 0,
    /* The resource resolver delivers it: one request with kind
       SXML_RESOURCE_KIND_XML_DOCUMENT, document_uri as uri_reference,
       resolved against base_uri as absolute_uri. A document the resolver
       does not deliver is schematron:include. */
    SXML_SCHEMATRON_DOCUMENT_SOURCE_RESOLVED,
    /* document_node holds it, already parsed: a document node whose
       document element is sch:schema, or an sch:schema element, read
       where it is. Errors have lines and columns when its tree keeps
       locations. */
    SXML_SCHEMATRON_DOCUMENT_SOURCE_NODE,
} sxml_schematron_document_source;

/* A Schematron schema to compile. */
typedef struct sxml_schematron_configuration {
    /* Kept by reference; must outlive the schematron. Its allocator gives
       the compiled form, its resolver the documents that sch:include and
       sch:extends href name (kind SXML_RESOURCE_KIND_XML_DOCUMENT, the
       href without its fragment identifier as uri_reference, resolved
       against the base URI of the element as absolute_uri; a document is
       parsed once however often it is named), its limits bound the
       documents' trees and the expressions. */
    sxml_processor *processor;
    sxml_schematron_document_source document_source;
    /* TEXT: the document, an XML document in any encoding the XML parser
       reads. Borrowed for the call. */
    sxml_byte_view document_text;
    /* NODE: the node, in the item heap of the processor. Borrowed for the
       call; compilation keeps what it needs. */
    sxml_item document_node;
    /* TEXT and NODE: the document's absolute URI, the source name of its
       errors and the base URI of its relative hrefs; may be empty.
       RESOLVED: the URI reference to ask for. Borrowed for the call. */
    sxml_byte_view document_uri;
    /* The base URI that a RESOLVED document_uri is resolved against;
       empty when absent. Borrowed for the call. */
    sxml_byte_view base_uri;
    /* The phase to compile: the id of a sch:phase, "#ALL" (every
       pattern), or "#DEFAULT" or empty (the schema's defaultPhase
       attribute, else "#ALL"). Only the active patterns of the phase,
       and the variables of the schema, of the phase and of those
       patterns, are compiled. Borrowed for the call. */
    sxml_byte_view phase_id;
    /* Receives the first error, or the location of a limit reached; may
       be NULL. Borrowed for the call. */
    sxml_error_record *error_record;
} sxml_schematron_configuration;

/*
 * Writes the memory requirements of a schematron's fixed part, which do
 * not depend on the schema, to *requirements. Returns
 * SXML_STATUS_INVALID_ARGUMENT for an invalid configuration (no
 * processor).
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_schematron_memory_requirements(
    const sxml_schematron_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Reads the schematron document and every document it reaches, compiles
 * the active patterns of the configured phase into a schematron placed in
 * memory, and writes its address to *schematron. memory must stay valid,
 * unmoved, until sxml_schematron_finalize. Compilation keeps nothing of
 * the documents' texts.
 *
 * Returns:
 * - SXML_STATUS_STATIC_ERROR or SXML_STATUS_TYPE_ERROR, with the error
 *   record filled (code, description, location in a schematron
 *   document), for the first error of Schematron (the codes above), of
 *   an expression, a pattern or an XSLT declaration;
 * - SXML_STATUS_DYNAMIC_ERROR, with the error record filled, for a
 *   dynamic error of a static expression of an XSLT declaration;
 * - SXML_STATUS_MALFORMED_INPUT, SXML_STATUS_INVALID_UTF8 or
 *   SXML_STATUS_UNSUPPORTED_FEATURE for a document that is not
 *   namespace-well-formed XML, with the record's diagnostic filled as
 *   sxml_xml_parser_parse fills it;
 * - SXML_STATUS_LIMIT_EXCEEDED at a limit of the processor, with only the
 *   record's diagnostic filled;
 * - SXML_STATUS_INVALID_ARGUMENT for an invalid configuration, and for a
 *   document the resolver delivered in a form its kind does not allow;
 * - SXML_STATUS_INSUFFICIENT_MEMORY or SXML_STATUS_MISALIGNED_MEMORY when
 *   memory does not meet the memory requirements, and the allocator's and
 *   the resolver's failures.
 * On failure *schematron is untouched and nothing is left allocated,
 * except item types the type registry interned, which stay valid.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_schematron_initialize(
    sxml_memory_view memory, const sxml_schematron_configuration *configuration,
    sxml_schematron **schematron);

/*
 * Releases the schematron's compiled form. Validations with it must have
 * returned. Does nothing when schematron is NULL.
 */
SXML_EXPORT void sxml_schematron_finalize(sxml_schematron *schematron);

/*
 * Allocates a schematron through allocator and compiles it as
 * sxml_schematron_initialize does. allocator is stored and must outlive
 * the schematron. Returns the failures of sxml_schematron_initialize and
 * sxml_allocator_allocate.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_schematron_create(
    sxml_allocator allocator, const sxml_schematron_configuration *configuration,
    sxml_schematron **schematron);

/*
 * Finalizes a schematron made by sxml_schematron_create and releases its
 * memory. Does nothing when schematron is NULL.
 */
SXML_EXPORT void sxml_schematron_destroy(sxml_schematron *schematron);

/* ----------------------------------------------------------- Validation */

/* One validation. Everything is borrowed for the call. */
typedef struct sxml_schematron_validation_request {
    /* The node to validate: a document node usually, or any node, in the
       item heap of the schema's processor. A tree validated against an
       XSD schema (include/sxml/schema.h) is read with its type
       annotations. Rules apply to the node and its descendants and
       attributes; the root of its tree is the global context item of the
       schema's variables. Locations in reports come from its tree. */
    sxml_item node;
    /* The values of the schema's sch:let variables (the children of
       sch:schema), by name, each in place of the variable's value
       expression; a name that is not such a variable is ignored. Names
       must be distinct. */
    sxml_variable_binding_view parameters;
    /* Receives every report; the null sink only counts them. */
    sxml_schematron_report_sink report_sink;
    /* When its operations are not NULL, receives the reports as one SVRL
       document (schematron-output with the schema's title, phase and ns
       prefixes, then each active pattern with its fired rules and
       findings), UTF-8, written as the validation goes. The fired rules
       are written whether or not reports_fired_rules is set. */
    sxml_output_stream svrl_output_stream;
    /* Receives a dynamic or type error, or the location of a limit
       reached; may be NULL. */
    sxml_error_record *error_record;
    /* When set, the report sink also receives a FIRED_RULE report for
       every rule that fires. */
    bool reports_fired_rules;
} sxml_schematron_validation_request;

/* What a validation found. */
typedef struct sxml_schematron_outcome {
    /* Failed asserts and successful reports. */
    size_t finding_count;
    /* Of those, the error findings (sxml_schematron_report.is_error): the
       node is valid when there are none. */
    size_t error_finding_count;
} sxml_schematron_outcome;

/*
 * Validates request->node: applies every active pattern, in the order of
 * the schema, to the node and its descendants and attributes, in
 * document order (a pattern with a documents attribute to the documents
 * it names instead, through the processor's resolver), and sends each
 * report to the report sink and to the SVRL output as it is made. Reports
 * come pattern by pattern; within a pattern, by context node in document
 * order (attributes before children), and for one node by rule and then
 * by assertion in the order of the schema. Writes the counts to *outcome.
 * Finding error findings is not a failure: the call returns
 * SXML_STATUS_OK and the outcome says so.
 *
 * Returns the statuses of sxml_xslt_stylesheet_transform (a dynamic or
 * type error of an expression, with the error record filled and located
 * in its schematron document; SXML_STATUS_LIMIT_EXCEEDED at a limit of
 * the processor, its evaluation step limit included), the report sink's
 * and the SVRL output stream's failures, and SXML_STATUS_INVALID_ARGUMENT
 * for a request that is not a node of the schema's processor or a
 * parameter of another processor. On failure *outcome is untouched;
 * reports already received stay received, and part of the SVRL may have
 * been written.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_schematron_validate_node(
    const sxml_schematron *schematron, const sxml_schematron_validation_request *request,
    sxml_schematron_outcome *outcome);
