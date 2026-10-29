#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sxml/export.h"
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/schema.h"
#include "sxml/serialize.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"

/*
 * Schematron: compiling a Schematron schema and validating documents with
 * it (ISO/IEC 19757-3:2025, the fourth edition; docs/DECISIONS.md, D313,
 * D315). Clause numbers below are those of the standard.
 *
 * A Schematron schema is compiled once, for one phase, from its
 * schematron document and every document its sch:include and
 * sch:extends href elements reach, into an sxml_schematron. Compiling
 * reads the documents into the stylesheet AST of XSLT (one mode per
 * active schematron pattern, one template rule per schematron rule, the
 * assertions as conditions in its body) and compiles that as a
 * stylesheet is compiled: no XSLT text is generated, and errors point
 * into the schematron documents. Inclusions (sch:include, sch:extends,
 * sch:library), abstract patterns, abstract rules and schema parameters
 * are resolved while reading (6.2), without an expanded copy of the
 * documents.
 *
 * The schema is then run over one node at a time (a document node
 * usually), any number of times: every finding (a failed assert or a
 * successful report) goes to a report sink as an sxml_schematron_report
 * with the assertion's attributes, its message, its diagnostics and
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
 *             .report_sink = compile_sink,
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
 * Query bindings (the queryBinding attribute, 6.5; D313 as the owner
 * amended it): the names are compared ignoring case (6.5). A schema
 * without the attribute, and xslt, xslt4, xpath and xpath4, are read as
 * XSLT 4.0 and XPath 4.0, the languages sxml implements, not as XSLT 1.0
 * and XPath 1.0 (Annex C): no XPath 1.0 compatibility mode. xslt2,
 * xslt3, xpath2, xpath3 and xpath31 are read too; their expressions run
 * on the same 4.0 processor. Expressions are XPath 4.0 under every
 * binding; a rule context is an XSLT 4.0 pattern, or, when it is no
 * pattern but an XPath 4.0 expression (Annexes I, K, L), matches the
 * nodes that expression selects from the instance root (from the nodes
 * a phase's from selects, from a subordinate document's root under
 * documents; D315). Under the xslt bindings (xslt,
 * xslt2, xslt3, xslt4) the xsl:function, xsl:key, xsl:include,
 * xsl:import and xsl:import-schema children of sch:schema, and except
 * under xslt2 also xsl:accumulator and xsl:use-package, are read as
 * XSLT 4.0 declarations of the compiled stylesheet, xsl:copy-of in a
 * sch:property as an instruction, and the flag, role and severity
 * attributes as attribute value templates; other XSLT elements, and
 * all of them under the xpath bindings, are foreign elements, which
 * Schematron ignores. Any other binding (xquery, xquery3, xquery31, stx,
 * ...) fails the compilation with schematron:query-binding at the
 * attribute (6.5: an implementation fails with an error).
 *
 * Expressions see the prefixes of the function library, xs, fn, math,
 * map, array and err (every binding makes them available, Annexes H to
 * L), xml, and the namespaces the sch:ns elements declare, which win for
 * a prefix both bind; not the XML namespace declarations of the
 * schematron documents (5.4.10; D331). An XSLT declaration sees its own
 * namespace declarations, and for a prefix they leave unbound the ones
 * expressions see.
 *
 * Errors: errors of Schematron itself carry codes in the namespace
 * urn:sxml:schematron, shown with the prefix "schematron" (the standard
 * names none):
 * - schematron:query-binding: a query binding sxml does not read;
 * - schematron:not-schematron: a compiled document whose document
 *   element is not sch:schema (a sch:library is only ever included,
 *   by sch:extends href, 5.4.8);
 * - schematron:include: a document that sch:include or sch:extends href
 *   names cannot be read, is not well-formed, has no element of the
 *   fragment identifier, includes itself, or gives an element its place
 *   does not allow (5.4.4, 5.4.6);
 * - schematron:reference: an id that names nothing of its kind (an
 *   active pattern, is-a, extends rule, a diagnostic, a property, the
 *   defaultPhase);
 * - schematron:phase: the configuration names a phase the schema does
 *   not have;
 * - schematron:variable: a variable (sch:let or a schema's sch:param)
 *   defined twice in the scopes of one rule: its schema, phase, pattern
 *   and rule (5.4.7);
 * - schematron:parameter: a sch:param named twice in one scope, a
 *   pattern instance's parameter that its abstract pattern does not
 *   declare, an abstract pattern's parameter that neither the instance
 *   nor the abstract pattern gives a value, a schema parameter without a
 *   value, or a configured parameter that names no schema parameter
 *   (5.4.11, Annex B);
 * - schematron:syntax: an element of the Schematron namespace where the
 *   schema does not allow it, without an attribute it needs or with a
 *   value its grammar does not allow (Annex A).
 * Errors in expressions keep the codes of XPath and XSLT (XPST0003,
 * XPST0008, XTSE0010, ...). Every error's location names the schematron
 * document's absolute URI with the line and column of the element or
 * attribute, or of the position inside the expression; a syntax error in
 * text a placeholder put into an expression points into the sch:param's
 * value. Dynamic errors of a validation (FORG0001, XPTY0004, FODC0002,
 * ...) stop it, located at the expression in its schematron document.
 *
 * Thread safety: the thread of the processor (D35).
 */

/* ------------------------------------------------------------- Reports */

/* What a report reports. The list only grows. */
typedef enum sxml_schematron_report_kind {
    /* An assert whose test was false for a context item: a finding. */
    SXML_SCHEMATRON_REPORT_KIND_FAILED_ASSERT = 0,
    /* A report whose test was true for a context item: a finding. */
    SXML_SCHEMATRON_REPORT_KIND_SUCCESSFUL_REPORT,
    /* A rule fired for a context node; only when the validation request
       asks (reports_fired_rules). */
    SXML_SCHEMATRON_REPORT_KIND_FIRED_RULE,
} sxml_schematron_report_kind;

/* A diagnostic the assertion names, evaluated for its context item. */
typedef struct sxml_schematron_diagnostic {
    /* The sch:diagnostic's id. */
    sxml_byte_view diagnostic_id;
    /* xml:lang in scope of the sch:diagnostic in its own document
       (language fixup, 6.7); empty when none. */
    sxml_byte_view language;
    /* Its content: text, with sch:value-of replaced by fn:string of its
       select (more than one item is XPTY0004, a dynamic error of the
       validation, Annexes C, H to N), sch:name by its name, the
       markup of sch:emph, sch:dir, sch:span and foreign elements
       reduced to its text; whitespace collapsed (5.3) unless
       xml:space="preserve" is in scope. */
    sxml_byte_view text;
} sxml_schematron_diagnostic;

typedef struct sxml_schematron_diagnostic_view {
    const sxml_schematron_diagnostic *diagnostics;
    size_t diagnostic_count;
} sxml_schematron_diagnostic_view;

/* A property the assertion names, evaluated for its context item. */
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
    /* The attributes of the assertion (FIRED_RULE: of the rule), leading
       and trailing whitespace stripped (5.3); each empty when absent.
       Not inherited from the rule or the pattern. */
    sxml_byte_view id;
    /* role, flag and severity are their values for this finding: as
       written, or, when the value is a variable reference (5.5.6,
       5.5.14, 5.5.16) or, under an xslt binding, an attribute value
       template (Annexes C, H, J), the string it evaluates to for the
       context item. */
    sxml_byte_view role;
    sxml_byte_view flag;
    /* The severity attribute (5.5.16; reserved values fatal, error,
       warning, info); empty for FIRED_RULE. */
    sxml_byte_view severity;
    sxml_byte_view see;
    sxml_byte_view icon;
    /* The fpi attribute. */
    sxml_byte_view formal_public_identifier;
    /* xml:lang in scope of the assertion (the rule) in its own document
       (language fixup, 6.7); empty when none. */
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
    /* The message: the assertion's content, evaluated with the message
       context (6.3: the subject node when a subject attribute applies,
       else the context item) and reduced as sxml_schematron_diagnostic.
       text is. Empty for FIRED_RULE. */
    sxml_byte_view message;
    /* The diagnostics and properties the assertion names, in the order
       its diagnostics and properties attributes give them, evaluated
       for the context item; empty for FIRED_RULE. */
    sxml_schematron_diagnostic_view diagnostics;
    sxml_schematron_property_view properties;
    /* The assertion's (FIRED_RULE: the rule's) element in its schematron
       document: the document's absolute URI as source_name, line and
       column of its start tag. Inside an abstract pattern or an abstract
       rule, the element there; pattern_id names the instance. */
    sxml_source_location schema_location;
    /* The subject node (5.5.19): the node the assertion's subject
       attribute, else its rule's, selects for the rule's context node;
       else the context item when it is a node; else (a visit-each item
       that is not a node, 5.5.21) the rule's context node. FIRED_RULE:
       the context node. In the item heap of the schema's processor. */
    sxml_item node;
    /* The node's location as its tree keeps it: the document URI as
       source_name, line and column; line_number 0 when the tree keeps no
       locations. */
    sxml_source_location node_location;
    /* The node's path, as fn:path gives it (F&O 4.0, "fn:path"): SVRL's
       location attribute. */
    sxml_byte_view node_path;
    /* An error finding (D313, D315): a FAILED_ASSERT or
       SUCCESSFUL_REPORT whose severity is "fatal" or "error"; or, without
       a severity, whose role or one of whose flag tokens is "fatal" or
       "error", or that has neither role nor flag. false for FIRED_RULE. */
    bool is_error;
    /* The document the node is in: its document URI as its tree keeps
       it, so the subordinate document's absolute URI for a pattern with
       documents (5.4.12; SVRL's fired-rule document, Annex D) and the
       URI of a document a rule reached with fn:doc; else the validated
       document's; empty when its tree has none. node_location's line and
       column are of this document (D331). */
    sxml_byte_view document_uri;
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

/* A value for one schema parameter (a sch:param child of sch:schema or
   of an included sch:library, 5.4.11). */
typedef struct sxml_schematron_parameter {
    /* The parameter's name, an NCName. */
    sxml_byte_view name;
    /* The query fragment that replaces the parameter's value attribute:
       text, put into every expression in place of $name as the value
       attribute's text would be. */
    sxml_byte_view value;
} sxml_schematron_parameter;

typedef struct sxml_schematron_parameter_view {
    const sxml_schematron_parameter *parameters;
    size_t parameter_count;
} sxml_schematron_parameter_view;

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
    /* The phase to compile: the id of a sch:phase; "#ALL" (every
       pattern); "#DEFAULT" or empty (the schema's defaultPhase
       attribute, else "#ALL"); or "#ANY" (each validation takes the
       first phase, in schema order, whose when attribute is true for
       the instance, else every pattern; 5.4.13, 5.5.22). Only the
       patterns the phase can make active, and the variables of the
       schema, of those phases and of those patterns, are compiled.
       Borrowed for the call. */
    sxml_byte_view phase_id;
    /* Values for schema parameters, each in place of its sch:param's
       value attribute (5.4.11: overridden at user option); names must
       be distinct. A name that is no schema parameter is
       schematron:parameter. Borrowed for the call. */
    sxml_schematron_parameter_view parameters;
    /* Receives every error compilation finds (code, description, the
       location in a schematron document; see the top of this header);
       the null sink stops at the first. Borrowed for the call. */
    sxml_schema_report_sink report_sink;
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
 * the patterns of the configured phase into a schematron placed in
 * memory, and writes its address to *schematron. memory must stay valid,
 * unmoved, until sxml_schematron_finalize. Compilation keeps nothing of
 * the documents' texts.
 *
 * Every error of Schematron (the codes above) and every syntax error of
 * an expression or a rule context (each is parsed alone) goes to the
 * report sink, and reading goes on; the stylesheet AST is compiled only
 * when none was found, and the errors of that compilation (names,
 * types, XSLT declarations) go to the sink too, one per declaration or
 * template rule.
 *
 * Returns:
 * - SXML_STATUS_STATIC_ERROR or SXML_STATUS_TYPE_ERROR, with the error
 *   record filled with the first error (code, description, location in
 *   a schematron document), when the report sink received any error or
 *   the null sink met one;
 * - SXML_STATUS_DYNAMIC_ERROR, with the error record filled, for a
 *   dynamic error of a static expression of an XSLT declaration;
 * - SXML_STATUS_MALFORMED_INPUT, SXML_STATUS_INVALID_UTF8 or
 *   SXML_STATUS_UNSUPPORTED_FEATURE for a compiled document that is not
 *   namespace-well-formed XML, with the record's diagnostic filled as
 *   sxml_xml_parser_parse fills it (an included one is
 *   schematron:include);
 * - SXML_STATUS_LIMIT_EXCEEDED at a limit of the processor, with only the
 *   record's diagnostic filled;
 * - SXML_STATUS_INVALID_ARGUMENT for an invalid configuration, and for a
 *   document the resolver delivered in a form its kind does not allow;
 * - SXML_STATUS_INSUFFICIENT_MEMORY or SXML_STATUS_MISALIGNED_MEMORY when
 *   memory does not meet the memory requirements, the report sink's
 *   status when it stopped compilation, and the allocator's and the
 *   resolver's failures.
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

/* What a compiled schema says of itself. Views into the schematron,
   valid while it lives; each empty when absent. */
typedef struct sxml_schematron_description {
    /* The schema's sch:title, its text. */
    sxml_byte_view title;
    /* The schemaVersion attribute (5.4.17). */
    sxml_byte_view schema_version;
    /* The schematronEdition attribute as written, any value (5.5.15:
       reported at user option; never an error). */
    sxml_byte_view schematron_edition;
    /* The queryBinding attribute as written. */
    sxml_byte_view query_binding;
    /* The phase compiled: a sch:phase id, "#ALL" or "#ANY" ("#DEFAULT"
       resolved). */
    sxml_byte_view phase_id;
} sxml_schematron_description;

/* Writes what schematron says of itself to *description. */
SXML_EXPORT void sxml_schematron_describe(const sxml_schematron *schematron,
                                          sxml_schematron_description *description);

/* ----------------------------------------------------------- Validation */

/* One validation. Everything is borrowed for the call. */
typedef struct sxml_schematron_validation_request {
    /* The node to validate: a document node usually, or any node, in the
       item heap of the schema's processor. A tree validated against an
       XSD schema (include/sxml/schema.h) is read with its type
       annotations. Rules apply to the node and its descendants and
       attributes (under a phase with a from attribute, to the nodes it
       selects instead, 5.5.7); the root of its tree is the instance
       document root: the context of the variables outside rules
       (5.4.7), of from, when and documents. Locations in reports come
       from its tree. */
    sxml_item node;
    /* Receives every report; the null sink only counts them. */
    sxml_schematron_report_sink report_sink;
    /* When its operations are not NULL, receives the reports as one SVRL
       document (schematron-output with the schema's title, phase,
       schemaVersion, schematronEdition and ns prefixes, then each active
       pattern or group with its fired rules and findings; Annex D),
       UTF-8, written as the validation goes. The fired rules are written
       whether or not reports_fired_rules is set. */
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
    /* The phase in force: the compiled one, or under "#ANY" the phase
       its when attribute chose, or "#ALL" when none did. A view into
       the schematron. */
    sxml_byte_view phase_id;
} sxml_schematron_outcome;

/*
 * Validates request->node: applies every active pattern, in the order of
 * the schema, to the node and its descendants and attributes, in
 * document order (a pattern with a documents attribute to the documents
 * it names instead, through the processor's resolver, 5.4.12), and
 * sends each report to the report sink and to the SVRL output as it is
 * made. Reports come pattern by pattern; within a pattern, by context
 * node in document order (attributes before children), then by
 * visit-each item (5.5.21), and for one item by rule and then by
 * assertion in the order of the schema. Writes the counts to *outcome.
 * Finding error findings is not a failure: the call returns
 * SXML_STATUS_OK and the outcome says so.
 *
 * Returns the statuses of sxml_xslt_stylesheet_transform (a dynamic or
 * type error of an expression, with the error record filled and located
 * in its schematron document, a subordinate document that cannot be
 * read included; SXML_STATUS_LIMIT_EXCEEDED at a limit of the processor,
 * its evaluation step limit included), the report sink's and the SVRL
 * output stream's failures, and SXML_STATUS_INVALID_ARGUMENT for a
 * request that is not a node of the schema's processor. On failure
 * *outcome is untouched; reports already received stay received, and
 * part of the SVRL may have been written.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_schematron_validate_node(
    const sxml_schematron *schematron, const sxml_schematron_validation_request *request,
    sxml_schematron_outcome *outcome);
