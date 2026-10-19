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
