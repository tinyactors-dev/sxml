#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/export.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"

/*
 * XSLT 4.0: compiling a stylesheet and running transformations
 * (docs/DECISIONS.md, D47, D51, D104, D106, D112).
 *
 * A stylesheet is compiled once, with the modules it includes and imports
 * and the packages it uses, and with the values of its static parameters
 * (XSLT 4.0, 2.4.1 "Information needed for Static Analysis"), into an
 * sxml_xslt_stylesheet. Each transformation then primes it (stylesheet
 * parameters and the global context item, 2.4.2) and invokes it once: by
 * applying templates to an initial match selection in an initial mode
 * (2.4.3), by calling an initial template (2.4.4) or by calling an
 * initial function (2.4.5). The principal result is delivered raw, as a
 * tree or serialized (2.4.6); result documents of xsl:result-document go
 * to a result document handler the host provides (26.2):
 *
 *     sxml_xslt_stylesheet *stylesheet = NULL;
 *     status = sxml_xslt_stylesheet_create(
 *         allocator,
 *         &(sxml_xslt_stylesheet_configuration){
 *             .processor = processor,
 *             .principal_module_text = stylesheet_bytes,
 *             .static_context = {.static_base_uri = stylesheet_uri},
 *             .error_record = &error_record,
 *         },
 *         &stylesheet);
 *     ...
 *     status = sxml_xslt_stylesheet_transform(
 *         stylesheet,
 *         &(sxml_xslt_transformation_request){
 *             .dynamic_context = {.has_context_value = true,
 *                                 .context_value = source_document,
 *                                 .variable_bindings = parameters},
 *             .invocation_kind = SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES,
 *             .result_delivery = SXML_XSLT_RESULT_DELIVERY_SERIALIZED,
 *             .output_stream = output_stream,
 *             .base_output_uri = output_uri,
 *             .result_document_handler = result_document_handler,
 *             .error_record = &error_record,
 *         },
 *         &principal_result);
 *
 * The static and dynamic contexts are those of XPath
 * (include/sxml/xpath.h): what the stylesheet declares (namespaces,
 * xpath-default-namespace, default collations, decimal formats, global
 * variables) is its own; the host gives what the stylesheet cannot.
 *
 * Modules and packages: every xsl:include and xsl:import asks the
 * processor's resource resolver with kind SXML_RESOURCE_KIND_XSLT_MODULE,
 * the href as uri_reference and absolute_uri the href resolved against the
 * including module's base URI (XSLT 4.0, 3.10.2, 3.10.3); every
 * xsl:use-package asks with kind SXML_RESOURCE_KIND_XSLT_PACKAGE, the
 * package name as uri_reference and absolute_uri and its package-version
 * as package_version (3.4.2 "Dependencies between Packages",
 * 3.4.3 "Locating Packages"). A module the resolver does not
 * deliver is XTSE0165, a package XTSE3000; a module that includes or
 * imports itself, directly or not, is XTSE0180. A module or package used
 * twice is read once. Compilation reads nothing else, except documents
 * that static expressions read (use-when, static variables) and the
 * parameter documents of xsl:output, asked for with kind
 * SXML_RESOURCE_KIND_XML_DOCUMENT.
 *
 * Errors: static errors (XTSE*, and XPST* in expressions), type errors
 * (XTTE*, XPTY*) and dynamic errors (XTDE*, XTMM*, XPDY*, FO*, SE*) are
 * reported through the caller's sxml_error_record with code, description
 * and location; the location of an error in a stylesheet module names the
 * module's URI as its source (D102: module_uri). The code is XSLT's where
 * XSLT has its own (XTDE0430 rather than XQDY0102, for example).
 *
 * Thread safety: the thread of the processor (D35).
 */

/* ------------------------------------------------------ Output definitions */

/*
 * An output definition (XSLT 4.0, 26.1.1 "The xsl:output declaration"):
 * the serialization parameters of one named or unnamed xsl:output,
 * merged over every module and import precedence as 26.1.1 describes, or
 * of one xsl:result-document instruction (its format's definition with
 * the instruction's attributes over it, 26.2), with build-tree.
 *
 * For a parameter the definition does not give, serialization_parameters
 * holds XSLT's default where it differs from include/sxml/serialize.h's:
 * omit_xml_declaration is NO. indent stays DEFAULT unless given; a
 * transformation that serializes takes DEFAULT as yes for the html and
 * xhtml methods and no for the others (26.1.4). A host that serializes
 * a raw result itself with sxml_serialize_sequence gets serialize.h's
 * defaults for what is still DEFAULT.
 */
typedef struct sxml_xslt_output_definition {
    /* Text and lists are borrowed from the stylesheet or the request
       that gave the definition, as the function that writes it says. */
    sxml_serialization_parameters serialization_parameters;
    /* build-tree: whether a result that is not serialized becomes a
       document node by sequence normalization (Serialization 4.0, section
       2). DEFAULT means the definition does not give it: yes when the
       method is xml, xhtml, html or text, given or chosen from the
       result, no for json, csv and adaptive (26.1). Ignored when the
       result is serialized: the method then decides. */
    sxml_serialization_flag build_tree;
    /* false when the definition gives no method:
       serialization_parameters.serialization_method is then ignored and
       the method is chosen from the raw result (26.1.2 "Choosing a
       Serialization Method": json when its first item is a map, an array
       or a JNode; else, by the first element child of the result tree,
       xhtml for an XHTML html element, html for an html element in no
       namespace, xml otherwise). */
    bool has_serialization_method;
} sxml_xslt_output_definition;

/* ------------------------------------------------------- Result delivery */

/*
 * How a principal result or a result document reaches the host (XSLT 4.0,
 * 2.4.6 "Post-processing the Raw Result", 26.2.1 "Delivering the
 * Result"). The list only grows.
 */
typedef enum sxml_xslt_result_delivery {
    /* As the output definition's build-tree says: TREE when its effective
       value is yes, RAW otherwise. */
    SXML_XSLT_RESULT_DELIVERY_DEFAULT = 0,
    /* The raw result as it is, any sequence. */
    SXML_XSLT_RESULT_DELIVERY_RAW,
    /* One document node, made from the raw result by sequence
       normalization with the output definition's item-separator
       (2.4.6.1 "Result Tree Construction"; a serialization error such as
       SENR0001 for an attribute node, a namespace node, a map or a
       function item). Its base URI is the result's URI: the base output
       URI for the principal result. */
    SXML_XSLT_RESULT_DELIVERY_TREE,
    /* The raw result serialized with the output definition to an output
       stream, which receives the bytes in pieces as they are produced
       (2.4.6.2, 26.1). Nothing is delivered as a value. */
    SXML_XSLT_RESULT_DELIVERY_SERIALIZED,
} sxml_xslt_result_delivery;

/* ------------------------------------------------- Result document handler */

/* A result document about to be delivered. Borrowed for the call. */
typedef struct sxml_xslt_result_document_request {
    /* The effective href of the instruction, as it evaluated (XSLT 4.0,
       26.2.2 "The Result Document URI"); empty when it has none. */
    sxml_byte_view href;
    /* href resolved against the base output URI (RFC 3986, section 5);
       href itself when the base output URI is absent. It identifies the
       result document: no two of one transformation have the same, and
       none has the principal result's unless that result is empty
       (XTDE1490, checked before open is called). */
    sxml_byte_view absolute_uri;
    /* The effective output definition of the instruction. */
    const sxml_xslt_output_definition *output_definition;
    /* The processor of the transformation, for a handler that reads or
       serializes the value itself. */
    sxml_processor *processor;
} sxml_xslt_result_document_request;

/*
 * Where the handler wants a result document. sxml hands open a
 * zero-initialized target: the DEFAULT delivery, no output stream.
 */
typedef struct sxml_xslt_result_document_target {
    /* SERIALIZED: receives the bytes; it must stay usable until close
       is called. */
    sxml_output_stream output_stream;
    sxml_xslt_result_delivery result_delivery;
} sxml_xslt_result_document_target;

/* A result document as it is handed over. Borrowed for the call. */
typedef struct sxml_xslt_result_document {
    /* The URI open received. */
    sxml_byte_view absolute_uri;
    /* RAW: the raw result; TREE (and DEFAULT with build-tree yes): one
       document node; SERIALIZED, and when is_abandoned: the empty
       sequence. A handler that keeps it retains it
       (sxml_sequence_retain). */
    sxml_sequence value;
    /* True when the transformation failed between open and close (a
       serialization error, a failed write): the handler should discard
       what it received for this document. */
    bool is_abandoned;
} sxml_xslt_result_document;

/*
 * The operations of a result document handler. Tables are static const.
 *
 * For every xsl:result-document instruction that a transformation
 * evaluates, sxml evaluates its raw result, calls open, delivers the
 * result as the target asks, and calls close once, also when delivering
 * fails. Result documents of one transformation do not overlap: open and
 * close alternate, in the order the instructions complete (which is not
 * always the order of the stylesheet's text: XSLT 4.0 leaves the order of
 * evaluation open). The principal result is not a result document: it is
 * delivered by sxml_xslt_stylesheet_transform itself.
 */
typedef struct sxml_xslt_result_document_handler_operations {
    /* Chooses the delivery of the result document request describes,
       and opens what it is delivered to (a file, a buffer). A failure,
       for example D50's SXML_STATUS_INPUT_OUTPUT_FAILED for a file that
       cannot be created, stops the transformation with the status
       returned, and close is not called. */
    sxml_status (*open)(void *state,
                        const sxml_xslt_result_document_request *request,
                        sxml_xslt_result_document_target *target);
    /* Receives the result document; called once for every open that
       returned SXML_STATUS_OK. A failure stops the transformation with
       the status returned. */
    sxml_status (*close)(void *state,
                         const sxml_xslt_result_document *result_document);
} sxml_xslt_result_document_handler_operations;

/*
 * The result document handler interface, passed by value. A handler
 * whose operations is NULL is the null handler: result documents are
 * evaluated and checked (XTDE1490 included) as for any handler, and then
 * discarded.
 */
typedef struct sxml_xslt_result_document_handler {
    void *state;
    const sxml_xslt_result_document_handler_operations *operations;
} sxml_xslt_result_document_handler;

/* --------------------------------------------- Result document collector */

/*
 * The built-in result document handler: it keeps every result document
 * of the transformations it handles in memory, in the order they are
 * closed, for tests (the XSLT 4.0 driver's assert-result-document), for
 * the WebAssembly package and for hosts that want them all at once.
 * Values it keeps live in the processor's item heap: clear or finalize
 * the collector before the processor is finalized.
 *
 * Thread safety: the thread of the processor whose results it keeps
 * (D35).
 */
typedef struct sxml_xslt_result_document_collector
    sxml_xslt_result_document_collector;

typedef struct sxml_xslt_result_document_collector_configuration {
    /* Provides the memory for URIs, serialized bytes and the list. Must
       not be the null allocator; must outlive the collector. */
    sxml_allocator allocator;
    /* Most bytes of serialized output the collector holds in all; 0
       means no bound. A write that would pass it gives
       SXML_STATUS_LIMIT_EXCEEDED, which stops the transformation. */
    size_t maximum_byte_count;
    /* The delivery the collector asks for every result document: values
       for DEFAULT, RAW and TREE, bytes for SERIALIZED. */
    sxml_xslt_result_delivery result_delivery;
} sxml_xslt_result_document_collector_configuration;

/*
 * Writes the memory requirements of a collector, without what it holds,
 * to *requirements. Returns SXML_STATUS_INVALID_ARGUMENT for an invalid
 * configuration (the null allocator, a delivery outside the list).
 */
SXML_NODISCARD SXML_EXPORT sxml_status
sxml_xslt_result_document_collector_memory_requirements(
    const sxml_xslt_result_document_collector_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places an empty collector in memory and writes its address to
 * *collector. memory must stay valid, unmoved, until
 * sxml_xslt_result_document_collector_finalize. Allocates nothing until
 * the first result document.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT for an invalid configuration, or
 * SXML_STATUS_INSUFFICIENT_MEMORY or SXML_STATUS_MISALIGNED_MEMORY when
 * memory does not meet the memory requirements.
 */
SXML_NODISCARD SXML_EXPORT sxml_status
sxml_xslt_result_document_collector_initialize(
    sxml_memory_view memory,
    const sxml_xslt_result_document_collector_configuration *configuration,
    sxml_xslt_result_document_collector **collector);

/*
 * Releases the values and the memory the collector holds. Does nothing
 * when collector is NULL.
 */
SXML_EXPORT void sxml_xslt_result_document_collector_finalize(
    sxml_xslt_result_document_collector *collector);

/*
 * Allocates a collector through allocator and initializes it. allocator
 * is stored and must outlive the collector. Returns the failures of
 * sxml_xslt_result_document_collector_initialize and
 * sxml_allocator_allocate.
 */
SXML_NODISCARD SXML_EXPORT sxml_status
sxml_xslt_result_document_collector_create(
    sxml_allocator allocator,
    const sxml_xslt_result_document_collector_configuration *configuration,
    sxml_xslt_result_document_collector **collector);

/*
 * Finalizes a collector made by sxml_xslt_result_document_collector_create
 * and releases its memory. Does nothing when collector is NULL.
 */
SXML_EXPORT void sxml_xslt_result_document_collector_destroy(
    sxml_xslt_result_document_collector *collector);

/*
 * The handler that adds to collector. Its open fails with the
 * allocator's failures; its close drops an abandoned document. Valid
 * until collector is finalized.
 */
SXML_EXPORT sxml_xslt_result_document_handler
sxml_xslt_result_document_collector_as_handler(
    sxml_xslt_result_document_collector *collector);

/* One collected result document. */
typedef struct sxml_xslt_collected_result_document {
    /* The result document's absolute URI. */
    sxml_byte_view absolute_uri;
    /* For values: the raw result or the document node; the empty
       sequence for bytes. */
    sxml_sequence value;
    /* For bytes: the serialized result; empty for values. */
    sxml_byte_view serialized_bytes;
} sxml_xslt_collected_result_document;

/* How many result documents the collector holds. */
SXML_EXPORT size_t sxml_xslt_result_document_collector_get_result_document_count(
    const sxml_xslt_result_document_collector *collector);

/*
 * Writes result document result_document_index (from 0, in the order
 * they were closed) to *result_document. Everything it refers to is
 * borrowed from the collector: valid until it is cleared or finalized.
 * Returns SXML_STATUS_INDEX_OUT_OF_RANGE when there is no such document;
 * *result_document is then untouched.
 */
SXML_NODISCARD SXML_EXPORT sxml_status
sxml_xslt_result_document_collector_get_result_document(
    const sxml_xslt_result_document_collector *collector,
    size_t result_document_index,
    sxml_xslt_collected_result_document *result_document);

/*
 * Releases every result document the collector holds, keeping its memory
 * for reuse.
 */
SXML_EXPORT void sxml_xslt_result_document_collector_clear(
    sxml_xslt_result_document_collector *collector);

/* ------------------------------------------------------------ Stylesheet */

/*
 * A stylesheet compiled for one processor: its principal module with every
 * module it includes and imports and every package it uses. Its fixed part
 * lives in the memory given to initialize (or allocated by create); its
 * compiled form is allocated from the processor's allocator and, as for an
 * XPath expression, lives until the stylesheet is finalized and every
 * function item a transformation returned is released, whichever comes
 * last (D38).
 */
typedef struct sxml_xslt_stylesheet sxml_xslt_stylesheet;

/* Where the principal module comes from. The list only grows. */
typedef enum sxml_xslt_principal_module_source {
    /* principal_module_text holds it. */
    SXML_XSLT_PRINCIPAL_MODULE_SOURCE_TEXT = 0,
    /* The resource resolver delivers it: one request with kind
       SXML_RESOURCE_KIND_XSLT_MODULE, principal_module_uri_reference as
       uri_reference, resolved against static_context.static_base_uri as
       absolute_uri. */
    SXML_XSLT_PRINCIPAL_MODULE_SOURCE_RESOLVED_MODULE,
    /* The resource resolver delivers it as a package: one request with
       kind SXML_RESOURCE_KIND_XSLT_PACKAGE, principal_module_uri_reference
       (the package name) as uri_reference and absolute_uri, and
       package_version. Its top element must be xsl:package, or
       xsl:stylesheet or xsl:transform, which XSLT 4.0, 3.4 makes an
       implicit package (D160); a simplified stylesheet module is
       XTSE3000. */
    SXML_XSLT_PRINCIPAL_MODULE_SOURCE_RESOLVED_PACKAGE,
} sxml_xslt_principal_module_source;

/* A stylesheet to compile. */
typedef struct sxml_xslt_stylesheet_configuration {
    /* Kept by reference; must outlive the stylesheet. */
    sxml_processor *processor;
    /* TEXT: the principal module, an XML document (the encoding as XML
       1.0 detects it; include/sxml/xml.h), parsed with the processor's
       limits. Borrowed for the call. */
    sxml_byte_view principal_module_text;
    /* RESOLVED_MODULE: a URI reference; RESOLVED_PACKAGE: a package name
       (an absolute URI, XSLT 4.0, 3.4). Borrowed for the call. */
    sxml_byte_view principal_module_uri_reference;
    /* RESOLVED_PACKAGE: the versions accepted, with the syntax of the
       package-version attribute of xsl:use-package (3.4.1 "Versions of a
       Package"); empty means "*". Borrowed for the call. */
    sxml_byte_view package_version;
    /* TEXT: the name to report in error locations of the principal
       module; empty means static_context.static_base_uri. Copied. A
       module the resolver delivered is reported by its resolved URI. */
    sxml_byte_view source_name;
    /*
     * The values of the top-level package's static parameters (XSLT 4.0,
     * 9.6 "Static Variables and Parameters"), by name: each is coerced to
     * the parameter's declared type (XTTE0590) and is in scope in
     * use-when, shadow attributes and every expression. A required static
     * parameter without a value is XTDE0050; a binding of a name that is
     * not a static parameter is ignored. Names must be distinct.
     * Borrowed for the call; compilation retains what it keeps.
     */
    sxml_variable_binding_view static_parameters;
    /*
     * What the host adds to the static context of every module, with the
     * rules of include/sxml/xpath.h; only these fields are read:
     * - static_base_uri is the base URI of the principal module given as
     *   TEXT (its module URI, for xsl:include and xsl:import and for
     *   XTSE0180), and what a RESOLVED_MODULE reference is resolved
     *   against; a delivered module's base URI is its resource_uri, or
     *   the request's absolute_uri;
     * - default_collation_uri is the default collation where no
     *   [xsl:]default-collation attribute gives one (3.6.2);
     * - collation_bindings are added to the statically known collations
     *   of every module;
     * - in_scope_schemas are in scope in every module, before the schema
     *   its xsl:import-schema declarations compile into, and an
     *   xsl:import-schema of a namespace one of them defines takes it
     *   from there (3.13; D179), as XQuery's import schema does
     *   (include/sxml/xpath.h).
     * namespace_bindings, variable_declarations and decimal_formats must
     * be empty (SXML_STATUS_INVALID_ARGUMENT): a stylesheet declares its
     * namespaces, variables and decimal formats itself.
     */
    sxml_static_context_configuration static_context;
    sxml_xslt_principal_module_source principal_module_source;
    /* Receives a static or type error found by compilation, a dynamic
       error raised while evaluating a static expression, or the location
       of a limit reached; may be NULL. Borrowed for the call. */
    sxml_error_record *error_record;
    /* The user option of XSLT 4.0, 19.10: when true, a construct
       declared streamable that is not guaranteed-streamable is processed
       without streaming instead of raising XTSE3430. False, the zero
       value, raises XTSE3430. Constructs that are guaranteed-streamable
       are streamed either way where sxml streams them (D219). Added by
       D219. */
    bool falls_back_to_unstreamed;
} sxml_xslt_stylesheet_configuration;

/*
 * Writes the memory requirements of a stylesheet's fixed part, which do
 * not depend on the stylesheet, to *requirements. Returns
 * SXML_STATUS_INVALID_ARGUMENT for an invalid configuration (no
 * processor).
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_xslt_stylesheet_memory_requirements(
    const sxml_xslt_stylesheet_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Compiles the principal module, every module it includes and imports and
 * every package it uses into a stylesheet placed in memory, and writes its
 * address to *stylesheet. memory must stay valid, unmoved, until
 * sxml_xslt_stylesheet_finalize. Compilation keeps nothing of the texts.
 *
 * Every static error of XSLT 4.0 is reported here, including those of
 * output definitions and character maps (XTSE1560 for conflicting
 * xsl:output declarations, SEPM0016 as a static error for a value a
 * serialization parameter does not permit).
 *
 * Returns:
 * - SXML_STATUS_STATIC_ERROR or SXML_STATUS_TYPE_ERROR, with the error
 *   record filled (code, description, location);
 * - SXML_STATUS_DYNAMIC_ERROR, with the error record filled, for XTDE0050
 *   and for a dynamic error raised by a static expression;
 * - SXML_STATUS_MALFORMED_INPUT, SXML_STATUS_INVALID_UTF8 or
 *   SXML_STATUS_UNSUPPORTED_FEATURE for a module that is not
 *   namespace-well-formed XML, with the record's diagnostic filled as
 *   sxml_xml_parser_parse fills it and its source naming the module;
 * - SXML_STATUS_LIMIT_EXCEEDED at a limit of the processor, with only
 *   the record's diagnostic filled;
 * - SXML_STATUS_INVALID_ARGUMENT for an invalid configuration (as for
 *   sxml_xpath_expression_initialize, and the rules above), and for a
 *   module or package the resolver delivered in a form its kind does not
 *   allow;
 * - SXML_STATUS_INSUFFICIENT_MEMORY or SXML_STATUS_MISALIGNED_MEMORY when
 *   memory does not meet the memory requirements, and the allocator's and
 *   the resolver's failures.
 * On failure *stylesheet is untouched and nothing is left allocated,
 * except item types the type registry interned, which stay valid.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_xslt_stylesheet_initialize(
    sxml_memory_view memory,
    const sxml_xslt_stylesheet_configuration *configuration,
    sxml_xslt_stylesheet **stylesheet);

/*
 * Drops the stylesheet's reference to its compiled form. Does nothing when
 * stylesheet is NULL.
 */
SXML_EXPORT void sxml_xslt_stylesheet_finalize(sxml_xslt_stylesheet *stylesheet);

/*
 * Allocates a stylesheet through allocator and compiles it as
 * sxml_xslt_stylesheet_initialize does. allocator is stored and must
 * outlive the stylesheet. Returns the failures of
 * sxml_xslt_stylesheet_initialize and sxml_allocator_allocate.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_xslt_stylesheet_create(
    sxml_allocator allocator,
    const sxml_xslt_stylesheet_configuration *configuration,
    sxml_xslt_stylesheet **stylesheet);

/*
 * Finalizes a stylesheet made by sxml_xslt_stylesheet_create and releases
 * its memory. Does nothing when stylesheet is NULL.
 */
SXML_EXPORT void sxml_xslt_stylesheet_destroy(sxml_xslt_stylesheet *stylesheet);

/*
 * Writes the output definition named output_definition_name to
 * *output_definition: name index 0 for the unnamed output definition,
 * which every stylesheet has (XSLT 4.0, 26.1.1: when no xsl:output is
 * unnamed, every parameter has its default and no method is given). Its
 * views are borrowed from the stylesheet: valid until it is finalized.
 *
 * The host reads it to override fields of the copy (the CLI's --method)
 * and passes the copy back in a transformation request.
 *
 * Returns SXML_STATUS_INVALID_ARGUMENT when the stylesheet has no output
 * definition of that name; *output_definition is then untouched.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_xslt_stylesheet_get_output_definition(
    const sxml_xslt_stylesheet *stylesheet,
    sxml_name_identifier output_definition_name,
    sxml_xslt_output_definition *output_definition);

/* -------------------------------------------------------- Transformation */

/*
 * How a transformation invokes the stylesheet (XSLT 4.0, 2.4.3 to 2.4.5).
 * The list only grows.
 */
typedef enum sxml_xslt_invocation_kind {
    /* Apply-templates invocation (2.4.3): the template rules of the
       initial mode process each item of the initial match selection, as
       xsl:apply-templates does, in final output state. */
    SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES = 0,
    /* Call-template invocation (2.4.4): the initial template is
       evaluated as xsl:call-template does, with a singleton focus on the
       global context item, or an absent focus without one (constrained
       by its xsl:context-item: XTTE0590, XTTE3090), in final output
       state. */
    SXML_XSLT_INVOCATION_KIND_CALL_TEMPLATE,
    /* Function call invocation (2.4.5): the initial function is called
       with function_arguments, with an absent focus, the whole
       transformation in temporary output state (xsl:result-document is
       XTDE1480). */
    SXML_XSLT_INVOCATION_KIND_CALL_FUNCTION,
} sxml_xslt_invocation_kind;

/*
 * One transformation. Fields are grouped by size (contexts, values and
 * views; interfaces and pointers; names and enumerations; flags) so the
 * struct has no padding; initialize it by field name. Everything is
 * borrowed for the call.
 */
typedef struct sxml_xslt_transformation_request {
    /*
     * The dynamic context, with the rules of include/sxml/xpath.h and
     * these for XSLT (2.4.2 "Priming a Stylesheet"):
     * - the context value is the global context item: a single item, or
     *   absent when has_context_value is false (more than one item is an
     *   invalid argument). It is the context item of global variables
     *   and parameters, coerced to the type of xsl:global-context-item
     *   (XTTE0590; XTDE3086 when that declares it required and it is
     *   absent), and the default initial match selection;
     * - variable_bindings give the stylesheet parameters, by name, each
     *   coerced to its declared type (XTTE0590). A required parameter
     *   without a value is XTDE0050; a binding of a name that is not a
     *   stylesheet parameter is ignored. Global variables and parameters
     *   are evaluated at most once per transformation, when first read
     *   (XTDE0640 for a circular definition);
     * - the trace sink receives xsl:message and failed xsl:assert output
     *   as SXML_TRACE_EVENT_KIND_XSLT_MESSAGE events; after a
     *   terminating event the transformation fails with its error code
     *   (XTMM9000 or XTMM9001 unless the instruction names another;
     *   XSLT 4.0, 23.1 "Messages", 23.2 "Assertions");
     * - fn:doc, fn:document and xsl:source-document read through the
     *   available documents and the resolver, kind
     *   SXML_RESOURCE_KIND_XML_DOCUMENT; reading a URI the
     *   transformation has written a result document to is XTDE1500.
     * The clock is read once per transformation: priming creates one
     * execution scope (2.4.2).
     */
    sxml_dynamic_context_configuration dynamic_context;
    /* APPLY_TEMPLATES, when has_initial_match_selection: the initial
       match selection, any sequence (the empty sequence makes the raw
       result empty). Otherwise the global context item is. */
    sxml_sequence initial_match_selection;
    /* APPLY_TEMPLATES and CALL_TEMPLATE: the non-tunnel and tunnel
       parameters passed to the initial template or to the template
       rules that process the initial match selection, as xsl:with-param
       passes them, coerced to their declared types (XTTE0590). A
       parameter no template declares is ignored; a required template
       parameter without a value is XTDE0700. Names must be distinct
       within each view. */
    sxml_variable_binding_view template_parameters;
    sxml_variable_binding_view tunnel_parameters;
    /* CALL_FUNCTION: the arguments, by position; their count is the
       arity of the initial function. Each is coerced to its parameter's
       declared type (XPTY0004). */
    sxml_sequence_view function_arguments;
    /* APPLY_TEMPLATES: the absolute URI of a principal source to stream;
       empty for none (D228). The initial match selection is then the
       document node of the document at this URI, read through the
       available documents and the resolver as xsl:source-document reads
       one (kind SXML_RESOURCE_KIND_XML_DOCUMENT; its BYTES form is what
       can be streamed), stripped as every source document is (4.2.2).
       When the initial mode is streamable and every template rule of it
       is guaranteed-streamable, and the accumulators expressions read can
       be computed as the document streams (D231;
       sxml_xslt_stylesheet_streams_source), its nodes are read as the
       parser delivers them, without building the document (XSLT 4.0,
       2.4.3 and 18; the streaming specification); otherwise the whole
       document is built first, with the same results. The global context
       item is the dynamic context's, absent when it gives none: a node
       of a streamed document is never one (2.4.2). With it,
       has_initial_match_selection must be false. */
    sxml_byte_view streamed_source_uri;
    /* The base output URI, an absolute URI; empty when absent (2.4.6).
       Relative hrefs of xsl:result-document resolve against it; it is
       the URI of the principal result and the base URI of its tree. */
    sxml_byte_view base_output_uri;
    /* The output definition of the principal result; NULL means the
       stylesheet's unnamed output definition. Views borrowed for the
       call. */
    const sxml_xslt_output_definition *output_definition;
    /* SERIALIZED: receives the serialized principal result. */
    sxml_output_stream output_stream;
    /* Receives the result documents; the null handler discards them. */
    sxml_xslt_result_document_handler result_document_handler;
    /* Receives a dynamic or type error, or the location of a limit
       reached; may be NULL. Borrowed for the call. */
    sxml_error_record *error_record;
    /* APPLY_TEMPLATES: the initial mode; index 0 means the stylesheet's
       default mode (the default-mode attribute of its top-level package,
       else the unnamed mode). A mode that is not eligible as an initial
       mode is XTDE0045 (2.4.3); an initial mode with neither an initial
       match selection nor a global context item is XTDE0044. */
    sxml_name_identifier initial_mode_name;
    /* CALL_TEMPLATE: the initial template; index 0 means
       xsl:initial-template. A name that is not a public or final named
       template is XTDE0040. */
    sxml_name_identifier initial_template_name;
    /* CALL_FUNCTION: the initial function. A name and arity that are not
       a public or final stylesheet function is XTDE0041. */
    sxml_name_identifier initial_function_name;
    sxml_xslt_invocation_kind invocation_kind;
    /* How the principal result is delivered: as a value written to
       *principal_result, or serialized to output_stream. */
    sxml_xslt_result_delivery result_delivery;
    /* APPLY_TEMPLATES: whether initial_match_selection is given. */
    bool has_initial_match_selection;
    /* APPLY_TEMPLATES: true selects the unnamed mode as the initial mode
       even when the stylesheet's default mode is another (the note in
       2.4.3); initial_mode_name must then be 0. */
    bool selects_unnamed_mode;
} sxml_xslt_transformation_request;

/*
 * Runs one transformation of stylesheet as request describes and writes
 * the principal result, owned, to *principal_result (release it with
 * sxml_sequence_release before the processor is finalized): the raw
 * result for RAW, one document node for TREE, the empty sequence for
 * SERIALIZED. As for sxml_xpath_expression_evaluate, the result is
 * complete, everything else the transformation made has been released
 * when this returns (apart from documents and function items the results
 * refer to), and transformations may nest (an extension function of the
 * host, a trace sink or a result document handler that runs another).
 * Result documents have been handed to the handler, in the order they
 * completed, before this returns; when the principal result is empty and
 * a result document has its URI, the principal result is discarded
 * (26.2.2, XTDE1490).
 *
 * Returns the statuses of sxml_xpath_expression_evaluate, with the
 * dynamic and type errors of XSLT 4.0 (XTDE*, XTTE*, XTMM*) and of
 * serialization (SE*) besides those of XPath and the function library,
 * the failures of the output stream, the result document handler and the
 * trace sink, and SXML_STATUS_INVALID_ARGUMENT for a request that breaks
 * the rules above (an enumeration constant outside its list, fields of
 * another invocation kind that are set, a global context item of more
 * than one item, values from another processor). On failure
 * *principal_result is untouched; part of a serialized principal result
 * may have been written, and result documents already closed stay with
 * the handler.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_xslt_stylesheet_transform(
    const sxml_xslt_stylesheet *stylesheet,
    const sxml_xslt_transformation_request *request,
    sxml_sequence *principal_result);

/*
 * Writes to *streams whether request, given a streamed_source_uri, would
 * stream it (D228): an APPLY_TEMPLATES invocation whose initial mode
 * (initial_mode_name, selects_unnamed_mode, else the default mode) is
 * declared streamable="yes" with every template rule guaranteed-
 * streamable (XSLT 4.0, 19.10), in a stylesheet whose expressions read no
 * accumulator that streaming cannot compute (an end rule with
 * capture="yes" of one declared streamable, D231). Only the invocation
 * fields of request are read; false for
 * the other invocation kinds. A host can so pass a large source as a URI
 * when it streams and as a tree, also the global context item, when it
 * does not. Returns SXML_STATUS_INVALID_ARGUMENT for a NULL argument and
 * the allocator's failures.
 */
SXML_NODISCARD SXML_EXPORT sxml_status sxml_xslt_stylesheet_streams_source(
    const sxml_xslt_stylesheet *stylesheet, const sxml_xslt_transformation_request *request,
    bool *streams);

/* ------------------------------------------------------------ fn:transform */

/*
 * The optional feature (include/sxml/processor.h, "Optional features")
 * that adds fn:transform (F&O 4.0, 18.3) to XPath, XQuery and XSLT: a
 * stylesheet compiled at run time, as sxml_xslt_stylesheet_initialize
 * compiles one, and run as a unit evaluation of the calling evaluation,
 * with fn:system-properties()?supports-dynamic-xslt true. Without it the
 * function is absent (XPST0017), and a program that names neither it nor
 * the rest of this header links no XSLT. sxml_all_optional_features()
 * includes it. Its name is "xslt-transform" (packet 7.11, D203).
 */
SXML_EXPORT sxml_optional_feature sxml_xslt_transform_feature(void);
