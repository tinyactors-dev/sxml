#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/export.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/view.h"
#include "sxml/xdm.h"

/*
 * XSD 1.1: compiling schemas and validating trees (docs/DECISIONS.md,
 * D165).
 *
 * A schema is compiled once, from one or more schema documents and every
 * document they include, import, redefine or override, into an
 * sxml_schema (XSD 1.1 Part 1, 4 "Schemas and Namespaces: Access and
 * Composition"). Compiling checks the XML representation of every
 * component (Part 1, appendix C.3 "Schema Representation Constraints")
 * and the components themselves (C.4 "Schema Component Constraints"),
 * and registers the schema's simple and complex type definitions in the
 * processor's type registry (include/sxml/types.h), where type
 * annotations, casting and the subtype relation find them.
 *
 * A schema validator then assesses one tree against the schema (Part 1,
 * 5.2 "Assessing Schema-Validity"). It is a tree builder: it receives the
 * events of the tree to validate, from the XML parser (include/sxml/xml.h,
 * so a document is validated while it is parsed), from
 * sxml_tree_builder_copy_node (an existing node), or from XQuery and XSLT
 * node construction, and passes them on to a result builder with what the
 * post-schema-validation infoset adds as XDM 4.0, 3.3 "Construction from a
 * PSVI" reads it: the type annotation of every element and attribute,
 * is-id, is-idrefs and nilled, attributes added from attribute defaults,
 * the default or fixed value of an element with empty content, and
 * attribute values as their schema normalized values. The result builder
 * is usually an arena tree; with the null builder nothing is built and
 * the validator only checks:
 *
 *     sxml_schema *schema = NULL;
 *     status = sxml_schema_create(
 *         allocator,
 *         &(sxml_schema_configuration){
 *             .processor = processor,
 *             .schema_documents = {.schema_documents = &(sxml_schema_document){
 *                                      .document_source =
 *                                          SXML_SCHEMA_DOCUMENT_SOURCE_TEXT,
 *                                      .document_text = schema_bytes,
 *                                      .document_uri = schema_uri},
 *                                  .schema_document_count = 1},
 *             .error_record = &error_record,
 *         },
 *         &schema);
 *     ...
 *     sxml_schema_validator *validator = NULL;
 *     status = sxml_schema_validator_create(
 *         allocator,
 *         &(sxml_schema_validator_configuration){
 *             .schema = schema,
 *             .validation_mode = SXML_VALIDATION_MODE_STRICT,
 *             .report_sink = report_sink,
 *             .error_record = &error_record,
 *         },
 *         &validator);
 *     status = sxml_xml_parser_parse(
 *         parser, &parse_request,
 *         sxml_schema_validator_as_tree_builder(validator));
 *     sxml_validation_outcome outcome;
 *     status = sxml_schema_validator_get_outcome(validator, &outcome);
 *
 * sxml_schema_validate_node does all of this for an existing node and
 * returns the validated copy, a new tree: XQuery's validate expression,
 * XSLT's validation attributes and fn:xsd-validator use it.
 *
 * Typed values are not stored: the typed value of a validated element or
 * attribute is derived from its string value and its type annotation when
 * it is asked for (include/sxml/xdm.h, sxml_node_make_typed_value; D165),
 * so a validated tree needs nothing beyond the annotations the tree
 * builder events already carry, and the types live in the registry after
 * the schema is destroyed.
 *
 * Schema documents are read with the XML parser into a tree and read from
 * there; they are XSD 1.1 documents, whatever their version attribute
 * says, and conditional inclusion (vc:minVersion, vc:maxVersion,
 * vc:typeAvailable, vc:typeUnavailable, vc:facetAvailable,
 * vc:facetUnavailable; Part 1, 4.2.2) selects as for an XSD 1.1
 * processor. Every xs:include, xs:import, xs:redefine and xs:override
 * asks the processor's resource resolver with kind
 * SXML_RESOURCE_KIND_SCHEMA_DOCUMENT: schemaLocation as uri_reference, it
 * resolved against the base URI of the referring document as
 * absolute_uri, and the namespace as target_namespace_uri (the imported
 * one for xs:import, the referring document's for the others). A document
 * met twice (by absolute URI) is read once; include cycles are allowed
 * (4.2.3). A document the resolver does not deliver is not an error by
 * itself, for include, redefine and override (4.2.3 clause 2.4) as for
 * import (4.2.6.2): its components are then missing, and src-resolve is
 * reported where they are referred to. Schema
 * location hints in instances (xsi:schemaLocation,
 * xsi:noNamespaceSchemaLocation) are not followed (4.3.2 allows it): the
 * schema is the one the configuration names. Nothing else is read.
 *
 * Errors carry XSD 1.1's constraint names as their codes (glossary
 * constraint_name; Part 1, appendix C "Outcome Tabulations"): the local
 * name, in no namespace, of the rule that failed, such as "src-resolve",
 * "cos-ct-extends.1.1" or "cvc-complex-type.2.4", with a description and
 * the location of the schema document's element or of the instance node
 * where it was found. Compilation raises them as static errors and
 * validation as dynamic errors. XQuery and XSLT raise their own codes
 * around them (XQST0059, XQDY0027, XTSE0220, XTTE1510, ...) and keep the
 * description. A report sink receives every error, so that one
 * compilation or validation can report them all; without one the first
 * error stops it.
 *
 * Thread safety: the thread of the processor (D35).
 */

/* ------------------------------------------------------------- Reports */

/* One error of a compilation or a validation. Borrowed for the call. */
typedef struct sxml_schema_report {
    /* The constraint name, in no namespace (see the top of this header). */
    sxml_error_code code;
    /* Human-readable UTF-8 text. */
    sxml_byte_view description;
    /* Compilation: the element of the schema document where the error
       was found, with the document's absolute URI as source_name.
       Validation: the instance node's location as its tree builder event
       gave it; line_number 0 when the events carried none. */
    sxml_source_location location;
} sxml_schema_report;

/* The operations of a report sink. Tables are static const. */
typedef struct sxml_schema_report_sink_operations {
    /* Receives one error, in the order found. Returning SXML_STATUS_OK
       lets the compilation or validation go on; any other status stops
       it with that status. */
    sxml_status (*receive)(void *state, const sxml_schema_report *report);
} sxml_schema_report_sink_operations;

/*
 * The report sink interface, passed by value. A sink whose operations is
 * NULL is the null sink: the first error stops the compilation or
 * validation with its error record filled.
 */
typedef struct sxml_schema_report_sink {
    void *state;
    const sxml_schema_report_sink_operations *operations;
} sxml_schema_report_sink;

/* --------------------------------------------------------------- Schema */

/*
 * A schema compiled for one processor. Its fixed part lives in the memory
 * given to initialize (or allocated by create); its components are
 * allocated from the processor's allocator and live until the schema is
 * finalized. The type definitions it registered stay in the processor's
 * type registry, valid for the trees validated with it, until the
 * processor is finalized.
 */
typedef struct sxml_schema sxml_schema;

/* Where a schema document comes from. */
typedef enum sxml_schema_document_source {
    /* document_text holds it. */
    SXML_SCHEMA_DOCUMENT_SOURCE_TEXT = 0,
    /* The resource resolver delivers it: one request with kind
       SXML_RESOURCE_KIND_SCHEMA_DOCUMENT, document_uri as uri_reference,
       resolved against the configuration's base_uri as absolute_uri, and
       target_namespace_uri. A document the resolver does not deliver
       makes the compilation fail with src-import ("the schema document
       cannot be read"). */
    SXML_SCHEMA_DOCUMENT_SOURCE_RESOLVED,
    /* document_node holds it, already parsed: a document node whose
       document element is xs:schema, or an xs:schema element (XSLT 4.0's
       inline schema, a child of xsl:import-schema), read where it is. */
    SXML_SCHEMA_DOCUMENT_SOURCE_NODE,
} sxml_schema_document_source;

/* One schema document the schema is assembled from. Views are borrowed
   for the call. */
typedef struct sxml_schema_document {
    sxml_schema_document_source document_source;
    /* TEXT: the document, an XML document in any encoding the XML parser
       reads. */
    sxml_byte_view document_text;
    /* NODE: the node, in the item heap of the processor; borrowed for the
       call. Its base URI is the base URI of its relative schemaLocation
       attributes. */
    sxml_item document_node;
    /* TEXT and NODE: the document's absolute URI, the source name of its
       errors and, for TEXT, the base URI of its relative schemaLocation
       attributes; may be empty. RESOLVED: the URI reference to ask for;
       may be empty when checks_target_namespace is set (an XQuery
       "import schema" without a location hint), so that the resolver
       chooses by namespace. */
    sxml_byte_view document_uri;
    /* When set, the document's targetNamespace must be
       target_namespace_uri (empty: no targetNamespace), as XQuery's
       "import schema" and XSLT's xsl:import-schema require; otherwise
       src-import. When not set, any target namespace is taken. */
    bool checks_target_namespace;
    sxml_byte_view target_namespace_uri;
} sxml_schema_document;

typedef struct sxml_schema_document_view {
    const sxml_schema_document *schema_documents;
    size_t schema_document_count;
} sxml_schema_document_view;

/* A schema to compile. */
typedef struct sxml_schema_configuration {
    /* Kept by reference; must outlive the schema. Its allocator gives the
       components, its resolver the referred schema documents, its type
       registry receives the type definitions, its limits bound the
       schema documents' trees and the XPath expressions of assertions,
       type alternatives and identity constraints. */
    sxml_processor *processor;
    /* The documents to assemble into one schema, in order: their
       components together, with those of every document they refer to
       (Part 1, 4.2 and 4.3). No documents gives the schema of the
       built-in components alone. Borrowed for the call. */
    sxml_schema_document_view schema_documents;
    /* The base URI that relative document_uri values of RESOLVED
       documents are resolved against; empty when absent. Borrowed for the
       call. */
    sxml_byte_view base_uri;
    /* Receives every error compilation finds; the null sink stops at the
       first. Borrowed for the call. */
    sxml_schema_report_sink report_sink;
    /* Most states the content model of one complex type may compile to:
       the bound on work and memory for hostile maxOccurs values and
       nested groups (content models run as automata with counters, so a
       large maxOccurs costs a counter, not states). 0 means sxml's
       default, 65,536. Exceeding it gives SXML_STATUS_LIMIT_EXCEEDED. */
    uint32_t maximum_content_model_state_count;
    /* Receives the first error (the error record of a static error, its
       code the constraint name), or the location of a limit reached; may
       be NULL. Borrowed for the call. */
    sxml_error_record *error_record;
} sxml_schema_configuration;

/*
 * Writes the memory requirements of a schema's fixed part, which do not
 * depend on the documents, to *requirements. Returns
 * SXML_STATUS_INVALID_ARGUMENT for an invalid configuration (no
 * processor).
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_schema_memory_requirements(
    const sxml_schema_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Reads every schema document of configuration->schema_documents and
 * those they refer to, compiles their components into a schema placed in
 * memory, registers its type definitions and writes its address to
 * *schema. memory must stay valid, unmoved, until sxml_schema_finalize.
 * Compilation keeps nothing of the documents' texts.
 *
 * Returns:
 * - SXML_STATUS_STATIC_ERROR, with the error record filled, when a
 *   document breaks a schema representation or component constraint, or
 *   is not a schema document at all (not well-formed, with the XML
 *   parser's description, or a document element other than xs:schema:
 *   constraint "schema_reference", Part 1, 4.3.2), and when the report
 *   sink received any error;
 * - SXML_STATUS_LIMIT_EXCEEDED at maximum_content_model_state_count or a
 *   limit of the processor, with only the record's diagnostic filled;
 * - SXML_STATUS_INVALID_ARGUMENT for an invalid configuration, and for a
 *   document the resolver delivered in a form its kind does not allow;
 * - SXML_STATUS_INSUFFICIENT_MEMORY or SXML_STATUS_MISALIGNED_MEMORY when
 *   memory does not meet the memory requirements, the report sink's
 *   status when it stopped compilation, and the allocator's and the
 *   resolver's failures.
 * On failure *schema is untouched and nothing is left allocated, except
 * type definitions already registered, which stay valid and unused.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_schema_initialize(
    sxml_memory_view memory, const sxml_schema_configuration *configuration,
    sxml_schema **schema);

/*
 * Releases the schema's components. Validators of the schema must have
 * been finalized before; trees validated with it stay valid. Does nothing
 * when schema is NULL.
 */
SXML_EXPORT void sxml_schema_finalize(sxml_schema *schema);

/*
 * Allocates a schema through allocator and compiles it as
 * sxml_schema_initialize does. allocator is stored and must outlive the
 * schema. Returns the failures of sxml_schema_initialize and
 * sxml_allocator_allocate.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_schema_create(
    sxml_allocator allocator, const sxml_schema_configuration *configuration,
    sxml_schema **schema);

/*
 * Finalizes a schema made by sxml_schema_create and releases its memory.
 * Does nothing when schema is NULL.
 */
SXML_EXPORT void sxml_schema_destroy(sxml_schema *schema);

/* ----------------------------------------------------------- Components */

/*
 * Finds the type definition named type_name among the schema's global
 * type definitions and the built-in types (every schema has the built-in
 * components, Part 1, 3.4.7 and 3.16.7) and writes its identifier to
 * *type. Returns false, leaving *type untouched, when there is none.
 * Schema types are found here, not by sxml_type_registry_find_type:
 * names belong to a schema, so two schemas of one processor may each
 * define a type of the same name (D165). O(1) average.
 */
SXML_EXPORT bool sxml_schema_find_type(const sxml_schema *schema,
                                       sxml_name_identifier type_name,
                                       sxml_type_identifier *type);

/* What a global element declaration says (Part 1, 3.3.1). */
typedef struct sxml_element_declaration_description {
    sxml_name_identifier element_name;
    /* {type definition}: the declared type, a type the schema registered
       or a built-in type. */
    sxml_type_identifier declared_type;
    /* {nillable}: xsi:nil="true" is allowed. */
    bool is_nillable;
    /* {abstract}: only members of its substitution group may appear. */
    bool is_abstract;
} sxml_element_declaration_description;

/*
 * Finds the global element declaration named element_name and writes what
 * it says to *description. Returns false, leaving *description untouched,
 * when there is none. O(1) average.
 */
SXML_EXPORT bool sxml_schema_find_element_declaration(
    const sxml_schema *schema, sxml_name_identifier element_name,
    sxml_element_declaration_description *description);

/* What a global attribute declaration says (Part 1, 3.2.1). */
typedef struct sxml_attribute_declaration_description {
    sxml_name_identifier attribute_name;
    /* {type definition}: a simple type. */
    sxml_type_identifier declared_type;
} sxml_attribute_declaration_description;

/*
 * Finds the global attribute declaration named attribute_name and writes
 * what it says to *description. Returns false, leaving *description
 * untouched, when there is none. O(1) average.
 */
SXML_EXPORT bool sxml_schema_find_attribute_declaration(
    const sxml_schema *schema, sxml_name_identifier attribute_name,
    sxml_attribute_declaration_description *description);

/* ----------------------------------------------------------- Validation */

/*
 * How the validation root is assessed when no governing type is given
 * (Part 1, 5.2 "Assessing Schema-Validity"; XQuery 4.0, "Validate
 * Expressions"; XSLT 4.0, "Validation").
 */
typedef enum sxml_validation_mode {
    /* The validation root must have a global declaration of its name
       (cvc-elt.1, cvc-assess-attr.1 otherwise). */
    SXML_VALIDATION_MODE_STRICT = 0,
    /* A validation root without a declaration is assessed laxly: its
       attributes and children are validated where declarations exist,
       and it is annotated xs:anyType (an attribute xs:untypedAtomic) with
       validity not known. */
    SXML_VALIDATION_MODE_LAX,
} sxml_validation_mode;

/* The [validity] of the validation root (Part 1, 3.3.5.1). */
typedef enum sxml_validity {
    /* Assessed, and every assessed part is valid. */
    SXML_VALIDITY_VALID = 0,
    /* Something assessed is invalid: the report sink received at least
       one error. */
    SXML_VALIDITY_INVALID,
    /* Lax assessment found nothing to assess it against; nothing assessed
       is invalid. */
    SXML_VALIDITY_NOT_KNOWN,
} sxml_validity;

/* What a validator reports once its tree has ended. */
typedef struct sxml_validation_outcome {
    sxml_validity validity;
    /* The errors found: reports received by the sink. */
    size_t error_count;
} sxml_validation_outcome;

/*
 * A schema validator: a tree builder that validates the one tree whose
 * events it receives and passes them on, annotated, to its result
 * builder. Its working memory (the element stack, the ID table, the
 * identity-constraint tables, the subtrees that assertions and identity
 * constraints read) comes from the schema's processor's allocator and
 * grows with the tree, within the processor's tree limits.
 */
typedef struct sxml_schema_validator sxml_schema_validator;

typedef struct sxml_schema_validator_configuration {
    /* Kept by reference; must outlive the validator. */
    const sxml_schema *schema;
    sxml_validation_mode validation_mode;
    /* SXML_TYPE_NONE: the validation root is assessed by the global
       declaration of its name, as validation_mode says. Otherwise the
       type to assess the validation root against (XQuery 4.0 "validate
       type", XSLT 4.0 type attributes): a type sxml_schema_find_type
       gives; an attribute needs a simple type. */
    sxml_type_identifier governing_type;
    /* Receives the annotated events. The builder whose operations is NULL
       receives nothing: the validator only checks. When the outcome is
       not valid or not known, the builder has received events that do
       not form a validated tree, and its caller discards them. The state
       must outlive the validator. */
    sxml_tree_builder result_builder;
    /* Receives every validation error; the null sink makes the first one
       stop the validation. The state must outlive the validator. */
    sxml_schema_report_sink report_sink;
    /* Receives the first error (code the constraint name); may be NULL.
       Borrowed: must outlive the validator. */
    sxml_error_record *error_record;
} sxml_schema_validator_configuration;

/*
 * Writes the memory requirements of a validator's fixed part to
 * *requirements. Returns SXML_STATUS_INVALID_ARGUMENT for an invalid
 * configuration (no schema, a governing type the schema's registry did
 * not issue).
 */
SXML_NODISCARD SXML_EXPORT sxml_status
sxml_schema_validator_memory_requirements(
    const sxml_schema_validator_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places a validator, ready for the events of one tree, in memory and
 * writes its address to *validator. memory must stay valid, unmoved,
 * until sxml_schema_validator_finalize. Allocates nothing until the first
 * event.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for an invalid configuration, or
 * SXML_STATUS_INSUFFICIENT_MEMORY or SXML_STATUS_MISALIGNED_MEMORY when
 * memory does not meet the memory requirements.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_schema_validator_initialize(
    sxml_memory_view memory,
    const sxml_schema_validator_configuration *configuration,
    sxml_schema_validator **validator);

/*
 * Releases the validator's working memory. Does nothing when validator is
 * NULL.
 */
SXML_EXPORT void sxml_schema_validator_finalize(
    sxml_schema_validator *validator);

/*
 * Allocates a validator through allocator and initializes it. allocator
 * is stored and must outlive the validator. Returns the failures of
 * sxml_schema_validator_initialize and sxml_allocator_allocate.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_schema_validator_create(
    sxml_allocator allocator,
    const sxml_schema_validator_configuration *configuration,
    sxml_schema_validator **validator);

/*
 * Finalizes a validator made by sxml_schema_validator_create and releases
 * its memory. Does nothing when validator is NULL.
 */
SXML_EXPORT void sxml_schema_validator_destroy(
    sxml_schema_validator *validator);

/*
 * The builder interface that feeds validator, with the event grammar of
 * include/sxml/tree.h. The tree is one document (its document element is
 * the validation root; XQuery's XQDY0061 rule of one element child is
 * XQuery's to check), one parentless element or one parentless attribute
 * (the validation root); a parentless text, comment or processing
 * instruction passes on unchanged. Type annotations, is-id, is-idrefs and
 * nilled in the events received are ignored: XSD assesses the infoset,
 * not an earlier validation's annotations.
 *
 * Events reach the result builder in the same order, an element's start
 * held back until its attributes are known (xsi:type, xsi:nil and type
 * alternatives decide its type). Each operation returns the result
 * builder's failures, the allocator's, SXML_STATUS_LIMIT_EXCEEDED at a
 * limit, SXML_STATUS_INVALID_ARGUMENT for an event out of order, and,
 * with the null report sink, SXML_STATUS_DYNAMIC_ERROR with the error
 * record filled at the first validation error (so the XML parser or
 * sxml_tree_builder_copy_node stops); with a report sink, an error goes
 * to the sink and the operation returns its status.
 *
 * Valid until the validator is finalized.
 */
SXML_EXPORT sxml_tree_builder sxml_schema_validator_as_tree_builder(
    sxml_schema_validator *validator);

/*
 * Writes the outcome of the validation to *outcome, once the tree's last
 * event (end_document, or the validation root's end) has been received:
 * IDREFs are resolved against the IDs of the validation root then
 * (cvc-id). Returns SXML_STATUS_INVALID_ARGUMENT, leaving *outcome
 * untouched, when the tree is not complete.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_schema_validator_get_outcome(
    const sxml_schema_validator *validator, sxml_validation_outcome *outcome);

/* An existing node to validate. */
typedef struct sxml_schema_validation_request {
    /* A document, element or attribute node in the item heap of the
       schema's processor. Borrowed for the call. */
    sxml_item node;
    sxml_validation_mode validation_mode;
    /* As sxml_schema_validator_configuration.governing_type. */
    sxml_type_identifier governing_type;
    /* Receives every validation error; the null sink stops at the first.
       Borrowed for the call. */
    sxml_schema_report_sink report_sink;
    /* Receives the first error; may be NULL. Borrowed for the call. */
    sxml_error_record *error_record;
} sxml_schema_validation_request;

/*
 * Validates request->node and writes its validated copy, owned, to
 * *validated_node: a new document, parentless element or parentless
 * attribute node in a new arena tree, built with the processor's tree
 * limits and destroyed when the last item referring to it is released.
 * The copy keeps the node's in-scope namespaces and base URI; its
 * annotations are the validation's, whatever the node's were. A node
 * whose validity is not known (lax) is copied too, annotated as lax
 * assessment says.
 *
 * Returns SXML_STATUS_DYNAMIC_ERROR with the error record filled (the
 * first error's constraint name) when the node is not valid, and the
 * failures of the validator, the tree and the heap; *validated_node is
 * then untouched and nothing is left allocated.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_schema_validate_node(
    const sxml_schema *schema, const sxml_schema_validation_request *request,
    sxml_item *validated_node);

/* ------------------------------------------------------- DTD validation */

/*
 * A DTD validator: the validator of include/sxml/xml.h's
 * sxml_xml_validator interface that sxml provides (D199). DTD validation
 * is schema validation: the parser writes the DTD as XSD schema
 * documents, which this validator compiles with its processor and checks
 * the document's events against with a schema validator; the validity
 * constraints the parser checks itself arrive as reports too. The tree the
 * parse builds is the one it would build without validation (attributes
 * untyped, is-id and is-idrefs from the DTD, XDM 4.0 section 6): DTD
 * validation adds no type annotations.
 *
 * Reports carry the schema validator's constraint names (cvc-...) for
 * what the schema checks, and "xml-validity" for the constraints the
 * parser checks, whose descriptions name them as XML 1.0 does ("VC: Root
 * Element Type: ..."). A DTD that does not compile (a default value its
 * type does not allow, a content model that is not deterministic, XML
 * 1.0 appendix E) is reported, with the schema's constraint name, as an
 * error of the document. A validator serves one parse.
 */
typedef struct sxml_dtd_validator sxml_dtd_validator;

typedef struct sxml_dtd_validator_configuration {
    /* Compiles the schema; kept by reference, must outlive the validator. */
    sxml_processor *processor;
    /* Receives every error; the null sink makes the first one stop the
       parse, with SXML_STATUS_DYNAMIC_ERROR. The state must outlive the
       validator. */
    sxml_schema_report_sink report_sink;
    /* Receives the first error; may be NULL. Must outlive the validator. */
    sxml_error_record *error_record;
} sxml_dtd_validator_configuration;

/*
 * Writes the memory requirements of a DTD validator's fixed part to
 * *requirements. Returns SXML_STATUS_INVALID_ARGUMENT for an invalid
 * configuration (no processor).
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_dtd_validator_memory_requirements(
    const sxml_dtd_validator_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places a DTD validator in memory and writes its address to *validator.
 * Allocates nothing until the parse starts validation. Returns
 * SXML_STATUS_INVALID_ARGUMENT for an invalid configuration, or
 * SXML_STATUS_INSUFFICIENT_MEMORY or SXML_STATUS_MISALIGNED_MEMORY when
 * memory does not meet the memory requirements.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_dtd_validator_initialize(
    sxml_memory_view memory, const sxml_dtd_validator_configuration *configuration,
    sxml_dtd_validator **validator);

/* Releases the schema and the schema validator. Does nothing when
   validator is NULL. */
SXML_EXPORT void sxml_dtd_validator_finalize(sxml_dtd_validator *validator);

/*
 * Allocates a DTD validator through allocator and initializes it.
 * allocator is stored and must outlive the validator. Returns the failures
 * of sxml_dtd_validator_initialize and sxml_allocator_allocate.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_dtd_validator_create(
    sxml_allocator allocator, const sxml_dtd_validator_configuration *configuration,
    sxml_dtd_validator **validator);

/* Finalizes a validator made by sxml_dtd_validator_create and releases its
   memory. Does nothing when validator is NULL. */
SXML_EXPORT void sxml_dtd_validator_destroy(sxml_dtd_validator *validator);

/* The interface to name in sxml_xml_parse_request.validator. Valid until
   the validator is finalized. */
SXML_EXPORT sxml_xml_validator
sxml_dtd_validator_as_xml_validator(sxml_dtd_validator *validator);

/*
 * Writes the outcome of the validation to *outcome once the parse has
 * ended: valid when no error was reported and the schema validator found
 * the document valid (IDREFs resolved, cvc-id). Returns
 * SXML_STATUS_INVALID_ARGUMENT, leaving *outcome untouched, when the
 * parse did not reach the end of the document and reported no error.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_dtd_validator_get_outcome(
    const sxml_dtd_validator *validator, sxml_validation_outcome *outcome);

/*
 * The optional feature (include/sxml/processor.h, "Optional features")
 * that gives fn:doc and fn:parse-xml their dtd-validation option (F&O
 * 4.0, 17.1.1 and 17.2.1): the document is validated against its DTD by
 * the DTD validator above, an invalid one raising FODC0007. Without it the
 * option raises FODC0013 (no parser with the requested options), and a
 * program that names neither it nor the DTD validator links no schema
 * compiler for it; reading the DTD, its entities and attribute defaults
 * does not need the feature. sxml_all_optional_features() includes it.
 * Its name is "dtd-validation" (packet 7.2b, D211).
 */
SXML_EXPORT sxml_optional_feature sxml_dtd_validation_feature(void);

/*
 * The optional feature (include/sxml/processor.h, "Optional features")
 * that gives XPath's functions the processor's built-in schemas, which
 * the schema compiler makes once per processor: the schema of the fn:
 * namespace (F&O 4.0, appendix D), which fn:json-to-xml with validate
 * true and the typed results of fn:analyze-string are validated against,
 * and the schema of the built-in components alone (XSD 1.1 Part 1,
 * 3.4.7), which lax validation by fn:doc and fn:parse-xml uses when no
 * schema is in scope. Without it, and without an in-scope schema of the
 * fn: namespace, validate true raises FOJS0004, fn:analyze-string returns
 * an untyped element, and lax validation without an in-scope schema
 * raises FODC0014 as strict validation does; a program that names neither
 * it nor XQuery, XSLT or the schema API links no schema compiler for
 * XPath. XQuery's and XSLT's in-scope schemas do not need it.
 * sxml_all_optional_features() includes it. Its name is "builtin-schemas"
 * (packet health-3, D225).
 */
SXML_EXPORT sxml_optional_feature sxml_builtin_schemas_feature(void);
