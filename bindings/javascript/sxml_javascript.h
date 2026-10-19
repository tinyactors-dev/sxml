#pragma once

/*
 * The JavaScript shim's interface (docs/GLOSSARY.md, "javascript_shim"):
 * the structs the package (src/index.ts) reads and writes in WebAssembly
 * memory, and the functions sxml.wasm exports to it (D138). Not a public
 * header: it belongs to the package, which is its only caller. Every field
 * of a struct the package touches is one 32-bit word on wasm32 (pointers,
 * sizes, uint32_t; a byte view is two), checked by static assertions in
 * sxml_javascript.c.
 */
#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/serialize.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"

#include <stddef.h>
#include <stdint.h>

/* The last failure of an operation on a processor, flat for JavaScript. */
typedef struct sxml_javascript_error {
    uint32_t status;
    sxml_byte_view status_name;
    /* The error code; empty views when the status has no error record. */
    sxml_byte_view code_namespace_uri;
    sxml_byte_view code_local_name;
    sxml_byte_view code_namespace_prefix;
    sxml_byte_view description;
    sxml_byte_view source_name;
    uint32_t line_number;
    uint32_t column_number;
} sxml_javascript_error;

/* A resource request as the host import sees it. */
typedef struct sxml_javascript_resource_request {
    /* sxml_resource_kind. */
    uint32_t resource_kind;
    sxml_byte_view uri_reference;
    sxml_byte_view absolute_uri;
} sxml_javascript_resource_request;

/* The host's answer: form 0 (not available) or 1 (bytes in content,
   allocated with sxml_javascript_allocate on the same processor). */
typedef struct sxml_javascript_resource {
    uint32_t resource_form;
    sxml_byte_view content;
} sxml_javascript_resource;

/* The handles (docs/GLOSSARY.md): opaque to the package. */
typedef struct sxml_javascript_processor sxml_javascript_processor;
typedef struct sxml_javascript_value sxml_javascript_value;
typedef struct sxml_javascript_expression sxml_javascript_expression;

typedef struct sxml_javascript_parse_request {
    /* The document, in its encoding. */
    sxml_byte_view input;
    /* The document URI and base URI; may be empty. */
    sxml_byte_view document_uri;
} sxml_javascript_parse_request;

/* A namespace binding as text: prefix (empty for the default element
   namespace) and URI. */
typedef struct sxml_javascript_namespace {
    sxml_byte_view namespace_prefix;
    sxml_byte_view namespace_uri;
} sxml_javascript_namespace;

/* An external variable's expanded name. */
typedef struct sxml_javascript_variable {
    sxml_byte_view namespace_uri;
    sxml_byte_view local_name;
} sxml_javascript_variable;

typedef struct sxml_javascript_compile_request {
    sxml_byte_view expression_text;
    sxml_byte_view static_base_uri;
    const sxml_javascript_namespace *namespaces;
    size_t namespace_count;
    /* Declared with type item()*, in this order; evaluations give one
       value per variable in the same order. */
    const sxml_javascript_variable *variables;
    size_t variable_count;
} sxml_javascript_compile_request;

typedef struct sxml_javascript_evaluate_request {
    /* NULL for an absent focus. */
    const sxml_javascript_value *context_value;
    /* One per declared variable; NULL leaves it unbound (XPDY0002). */
    const sxml_javascript_value *const *variable_values;
    size_t variable_value_count;
} sxml_javascript_evaluate_request;

typedef struct sxml_javascript_serialize_request {
    const sxml_javascript_value *value;
    /* The $options of fn:serialize (a map); NULL for the adaptive method
       with its defaults. */
    const sxml_javascript_value *options;
} sxml_javascript_serialize_request;

/* ------------------------------------------------- Full module: requests */

/*
 * The structs of the full module's entry points (the section "Full module"
 * below): XQuery, XSLT and XSD, linked into sxml-full.wasm only (D189's two
 * links). Every field is still one 32-bit word on wasm32.
 */

/* An expanded name: an empty local name means none. */
typedef struct sxml_javascript_name {
    sxml_byte_view namespace_uri;
    sxml_byte_view local_name;
} sxml_javascript_name;

/* A value bound to a name: an external variable, a stylesheet, template
   or tunnel parameter. */
typedef struct sxml_javascript_binding {
    sxml_javascript_name name;
    const sxml_javascript_value *value;
} sxml_javascript_binding;

typedef struct sxml_javascript_binding_list {
    const sxml_javascript_binding *bindings;
    size_t binding_count;
} sxml_javascript_binding_list;

/* What a side result is (docs/GLOSSARY.md, "side_result"). */
typedef enum sxml_javascript_side_result_kind {
    /* A tree an updating query changed: first_value its original root,
       second_value its updated root (include/sxml/xquery.h). */
    SXML_JAVASCRIPT_SIDE_RESULT_KIND_UPDATED_TREE = 0,
    /* A document fn:put stores: uri, first_value the node, second_value
       its serialization parameters or NULL. */
    SXML_JAVASCRIPT_SIDE_RESULT_KIND_PUT_DOCUMENT = 1,
    /* An xsl:result-document: uri, and text (serialized) or first_value
       (a value) as the transformation asked (include/sxml/xslt.h). */
    SXML_JAVASCRIPT_SIDE_RESULT_KIND_RESULT_DOCUMENT = 2,
    /* An xsl:message, failed xsl:assert, fn:message or fn:trace: text as
       the text method writes the value; code and is_terminating for
       xsl:message; uri the source name, with line and column. */
    SXML_JAVASCRIPT_SIDE_RESULT_KIND_MESSAGE = 3,
    /* A schema report (include/sxml/schema.h): code the constraint name,
       text the description, uri the source name, with line and column. */
    SXML_JAVASCRIPT_SIDE_RESULT_KIND_SCHEMA_REPORT = 4,
} sxml_javascript_side_result_kind;

/*
 * Something a call delivers besides its result, kept by the handle that
 * made it until that handle's next call. Views are the handle's copies.
 * The values are handles (NULL when the kind has none) that the package
 * takes over: it adopts every one after the call, failed or not, and
 * releases it as any other value.
 */
typedef struct sxml_javascript_side_result {
    uint32_t side_result_kind;
    sxml_byte_view uri;
    sxml_byte_view text;
    sxml_byte_view code;
    sxml_javascript_value *first_value;
    sxml_javascript_value *second_value;
    uint32_t line_number;
    uint32_t column_number;
    uint32_t is_terminating;
} sxml_javascript_side_result;

/* The side results of a call, written by the call into its request. */
typedef struct sxml_javascript_side_result_list {
    const sxml_javascript_side_result *side_results;
    size_t side_result_count;
} sxml_javascript_side_result_list;

typedef struct sxml_javascript_query sxml_javascript_query;
typedef struct sxml_javascript_stylesheet sxml_javascript_stylesheet;
typedef struct sxml_javascript_schema sxml_javascript_schema;

typedef struct sxml_javascript_query_compile_request {
    /* The main module (XQuery 4.0, 5). */
    sxml_byte_view query_text;
    sxml_byte_view static_base_uri;
    /* Added to XQuery's predeclared prefixes; the prolog may rebind
       them. */
    const sxml_javascript_namespace *namespaces;
    size_t namespace_count;
} sxml_javascript_query_compile_request;

typedef struct sxml_javascript_query_evaluate_request {
    /* NULL for an absent context value. */
    const sxml_javascript_value *context_value;
    /* The external variables (XQuery 4.0, 5.16); names the query does
       not declare are ignored. */
    sxml_javascript_binding_list variables;
    /* Nonzero when the package takes documents fn:put stores; zero makes
       fn:put fail with FOUP0002 (include/sxml/xquery.h). */
    uint32_t accepts_put_documents;
    /* Written by the call: updated trees, put documents, messages. */
    sxml_javascript_side_result_list side_results;
} sxml_javascript_query_evaluate_request;

typedef struct sxml_javascript_stylesheet_compile_request {
    /* The principal stylesheet module, an XML document. */
    sxml_byte_view stylesheet_text;
    /* Its base URI (module URI); may be empty. */
    sxml_byte_view static_base_uri;
    /* The static parameters (XSLT 4.0, 9.6). */
    sxml_javascript_binding_list static_parameters;
} sxml_javascript_stylesheet_compile_request;

typedef struct sxml_javascript_transform_request {
    /* The global context item, or NULL (XSLT 4.0, 2.4.2). */
    const sxml_javascript_value *global_context_item;
    /* APPLY_TEMPLATES: the initial match selection, or NULL for the
       global context item. */
    const sxml_javascript_value *initial_match_selection;
    sxml_javascript_binding_list stylesheet_parameters;
    sxml_javascript_binding_list template_parameters;
    sxml_javascript_binding_list tunnel_parameters;
    /* CALL_FUNCTION: the arguments, by position. */
    const sxml_javascript_value *const *function_arguments;
    size_t function_argument_count;
    sxml_byte_view base_output_uri;
    /* The $options of fn:serialize (a map) replacing the parameters of
       the unnamed output definition, method included; NULL keeps them. */
    const sxml_javascript_value *serialization_options;
    /* Empty local names: the default mode, xsl:initial-template. */
    sxml_javascript_name initial_mode;
    sxml_javascript_name initial_template;
    sxml_javascript_name initial_function;
    /* sxml_xslt_invocation_kind. */
    uint32_t invocation_kind;
    /* sxml_xslt_result_delivery of the principal result; SERIALIZED
       writes it to the processor's output. */
    uint32_t result_delivery;
    /* sxml_xslt_result_delivery of the result documents. */
    uint32_t result_document_delivery;
    /* Nonzero selects the unnamed mode (initial_mode must be empty). */
    uint32_t selects_unnamed_mode;
    /* Written by the call: result documents and messages. */
    sxml_javascript_side_result_list side_results;
} sxml_javascript_transform_request;

/* One schema document: its text, or (text.bytes NULL) a URI reference the
   resolver delivers (include/sxml/schema.h, TEXT and RESOLVED). */
typedef struct sxml_javascript_schema_document {
    sxml_byte_view text;
    /* TEXT: the document's URI; RESOLVED: the reference to resolve. */
    sxml_byte_view uri;
} sxml_javascript_schema_document;

typedef struct sxml_javascript_schema_compile_request {
    const sxml_javascript_schema_document *documents;
    size_t document_count;
    sxml_byte_view base_uri;
    /* Written by the call: every schema report. */
    sxml_javascript_side_result_list side_results;
} sxml_javascript_schema_compile_request;

typedef struct sxml_javascript_validate_request {
    /* A document or element node of the schema's processor, or NULL to
       validate text, an XML document parsed and validated in one pass, so
       that reports have its locations. */
    const sxml_javascript_value *node;
    sxml_byte_view text;
    /* The text's document URI, and its reports' source name. */
    sxml_byte_view document_uri;
    /* sxml_validation_mode. */
    uint32_t validation_mode;
    /* A type of the schema to validate the root against; an empty local
       name for none. */
    sxml_javascript_name governing_type;
    /* Written by the call: sxml_validity. */
    uint32_t validity;
    /* Written by the call: every validation report. */
    sxml_javascript_side_result_list side_results;
} sxml_javascript_validate_request;

/* --------------------------------------------------------------- Exports */

/*
 * The optional features this module links (include/sxml/processor.h,
 * "Optional features"; D189), as static data. Defined by the one of
 * sxml_javascript_core_features.c (none: sxml.wasm) and
 * sxml_javascript_full_features.c (every feature: sxml-full.wasm) that the
 * module links. At most 32, so a feature mask is one word.
 */
sxml_optional_feature_view sxml_javascript_module_features(void);

/* The number of the module's features, and the name of the one at
   feature_index (ASCII; NULL and 0 past the last). */
uint32_t sxml_javascript_feature_count(void);
const uint8_t *sxml_javascript_feature_name_bytes(uint32_t feature_index);
uint32_t sxml_javascript_feature_name_byte_count(uint32_t feature_index);

/*
 * Makes a processor (include/sxml/processor.h) whose resource resolver is
 * the host import sxml_host.resolve. failing_allocation_number is that of
 * sxml_counting_allocator_configuration (0 fails none), counted from the
 * processor's own first allocation. feature_mask names the module's
 * features the processor may use: bit i for the feature at index i; bits
 * past the last feature are ignored. Returns NULL when out of memory.
 */
sxml_javascript_processor *sxml_javascript_processor_create(uint32_t failing_allocation_number,
                                                            uint32_t feature_mask);

/* Destroys a processor and returns the allocations of its allocator that
   were still live afterwards: 0 unless something leaked. Every value and
   expression made with it must have been released first (the package does
   so). NULL does nothing and returns 0. */
uint32_t sxml_javascript_processor_destroy(sxml_javascript_processor *processor);

/* Allocates byte_count bytes (at least 1) from the processor's allocator,
   aligned for any request struct; NULL when out of memory. */
void *sxml_javascript_allocate(sxml_javascript_processor *processor, size_t byte_count);

/* Releases memory from sxml_javascript_allocate; NULL does nothing. */
void sxml_javascript_release(sxml_javascript_processor *processor, void *memory);

/* The allocations of the processor's allocator not yet released. */
uint32_t sxml_javascript_live_allocation_count(const sxml_javascript_processor *processor);

/* The failure of the processor's last operation; valid until its next. */
const sxml_javascript_error *sxml_javascript_error_of(const sxml_javascript_processor *processor);

/* The bytes of the processor's last serialization; valid until its next
   operation. */
const sxml_byte_view *sxml_javascript_output_of(const sxml_javascript_processor *processor);

/* The library version, NUL-terminated ASCII ("0.2.0"). */
const char *sxml_javascript_version_text(void);

/* Parses an XML document (include/sxml/xml.h) and returns its document
   node as a value, or NULL with the processor's error set. */
sxml_javascript_value *sxml_javascript_parse(sxml_javascript_processor *processor,
                                             const sxml_javascript_parse_request *request);

/* Makes an xs:string value of text (UTF-8 of XML characters), or returns
   NULL with the processor's error set (SXML_STATUS_INVALID_UTF8,
   SXML_STATUS_INVALID_ARGUMENT for a character XML does not allow). */
sxml_javascript_value *sxml_javascript_string(sxml_javascript_processor *processor,
                                              const sxml_byte_view *text);

/* Releases a value; NULL does nothing. */
void sxml_javascript_value_release(sxml_javascript_value *value);

/* Compiles an XPath 4.0 expression (include/sxml/xpath.h) with the
   namespaces and external variables of request, or returns NULL with the
   processor's error set. */
sxml_javascript_expression *sxml_javascript_compile(
    sxml_javascript_processor *processor, const sxml_javascript_compile_request *request);

/* Destroys an expression; NULL does nothing. */
void sxml_javascript_expression_destroy(sxml_javascript_expression *expression);

/* Evaluates an expression and returns its result as a value, or NULL with
   the processor's error set. request->variable_value_count must be the
   expression's variable count (SXML_STATUS_INVALID_ARGUMENT otherwise). */
sxml_javascript_value *sxml_javascript_evaluate(sxml_javascript_expression *expression,
                                                const sxml_javascript_evaluate_request *request);

/* Serializes request->value into the processor's output: by the adaptive
   method without options, else as fn:serialize with them (method xml
   unless they name another). Returns the status, with the processor's
   error set on failure. */
uint32_t sxml_javascript_serialize(sxml_javascript_processor *processor,
                                   const sxml_javascript_serialize_request *request);

/* ----------------------------------------------------------- Full module */

/*
 * Linked into sxml-full.wasm only (sxml_javascript_xquery.c,
 * sxml_javascript_xslt.c, sxml_javascript_schema.c): sxml.wasm does not
 * export them, which is how the package tells the modules apart. Each
 * failure returns NULL (or its status) with the processor's error set, as
 * above. A call that writes side results into its request writes them
 * also when it fails.
 */

/* Compiles an XQuery 4.0 main module (include/sxml/xquery.h), with the
   library modules it imports through the resolver. */
sxml_javascript_query *sxml_javascript_query_compile(
    sxml_javascript_processor *processor, const sxml_javascript_query_compile_request *request);

/* Destroys a query and its side results; NULL does nothing. */
void sxml_javascript_query_destroy(sxml_javascript_query *query);

/* 1 when the query's body is an updating expression (XQUF 3.0, 2.2). */
uint32_t sxml_javascript_query_is_updating(const sxml_javascript_query *query);

/* Evaluates a query and returns its result (the empty sequence for an
   updating query, whose changes are side results). */
sxml_javascript_value *sxml_javascript_query_evaluate(
    sxml_javascript_query *query, sxml_javascript_query_evaluate_request *request);

/* Serializes value into the processor's output with the serialization
   parameters the query's output declarations give (XQuery 4.0, 5.22).
   Returns the status. */
uint32_t sxml_javascript_query_serialize(sxml_javascript_query *query,
                                         const sxml_javascript_value *value);

/* Compiles an XSLT 4.0 stylesheet (include/sxml/xslt.h), with the modules
   and packages it uses through the resolver. */
sxml_javascript_stylesheet *sxml_javascript_stylesheet_compile(
    sxml_javascript_processor *processor,
    const sxml_javascript_stylesheet_compile_request *request);

/* Destroys a stylesheet and its side results; NULL does nothing. */
void sxml_javascript_stylesheet_destroy(sxml_javascript_stylesheet *stylesheet);

/* Runs one transformation and returns its principal result as a value
   (the empty sequence when it is serialized to the processor's output). */
sxml_javascript_value *sxml_javascript_stylesheet_transform(
    sxml_javascript_stylesheet *stylesheet, sxml_javascript_transform_request *request);

/* Makes an empty schema handle, to compile into; NULL when out of
   memory. */
sxml_javascript_schema *sxml_javascript_schema_create(sxml_javascript_processor *processor);

/* Destroys a schema handle and its side results; NULL does nothing. */
void sxml_javascript_schema_destroy(sxml_javascript_schema *schema);

/* Compiles the schema documents of request into schema (include/sxml/
   schema.h), every report a side result. Returns the status; a schema
   compiles once (SXML_STATUS_INVALID_ARGUMENT after). */
uint32_t sxml_javascript_schema_compile(sxml_javascript_schema *schema,
                                        sxml_javascript_schema_compile_request *request);

/* Validates request->node (XSD 1.1 Part 1, 5.2) and returns its validated
   copy, or NULL: with the processor's error set when validation failed,
   without it when the node is invalid (request->validity). */
sxml_javascript_value *sxml_javascript_schema_validate(sxml_javascript_schema *schema,
                                                       sxml_javascript_validate_request *request);
