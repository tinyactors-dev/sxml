#pragma once

/*
 * The JavaScript shim (docs/GLOSSARY.md, "javascript_shim"): the names
 * under which sxml.wasm exports the host shim
 * (bindings/host/sxml_host.h, D238) to the package's src/index.ts (D138).
 * Each export calls the host shim function of the same name with
 * sxml_host_ in place of sxml_javascript_, so the structs the package
 * reads and writes are the host shim's. The names stay those of D138 and
 * D216, so the package does not change. Not a public header: the export
 * files are its only users.
 *
 * sxml_javascript_exports.c holds the exports of XPath, values and the
 * processor, sxml_javascript_full_exports.c those of XQuery, XSLT and XSD
 * (one module links both, D272), and sxml_javascript_host.c the resolver
 * that calls the package's import sxml_host.resolve.
 */
#include "sxml_host.h"

#include "sxml/processor.h"
#include "sxml/view.h"

#include <stddef.h>
#include <stdint.h>

/* Gives a definition its export name under WASI; nothing natively. */
#if defined(__wasm__)
#define SXML_JAVASCRIPT_EXPORT(name) __attribute__((export_name(name), used))
#else
#define SXML_JAVASCRIPT_EXPORT(name)
#endif

/* The resolver of every processor the package makes: under WASI it calls
   the import sxml_host.resolve with the requesting processor; natively
   (lint, -Werror) none is available. */
sxml_host_resolver sxml_javascript_resolver(void);

/* ---------------------------------------------------------- Both modules */

uint32_t sxml_javascript_feature_count(void);
const uint8_t *sxml_javascript_feature_name_bytes(uint32_t feature_index);
uint32_t sxml_javascript_feature_name_byte_count(uint32_t feature_index);
/* sxml_host_processor_create with sxml_javascript_resolver() and the
   languages of *code_languages (NULL for none), which the package's
   module linker loaded (src/linker.ts, D293). */
sxml_host_processor *sxml_javascript_processor_create(
    uint32_t failing_allocation_number, uint32_t feature_mask,
    const sxml_source_code_language_view *code_languages);
/* sxml_source_code_check_language, as a status number. */
uint32_t sxml_javascript_check_code_language(const sxml_source_code_language *language,
                                             sxml_source_code_language_report *report);
uint32_t sxml_javascript_processor_destroy(sxml_host_processor *processor);
void *sxml_javascript_allocate(sxml_host_processor *processor, size_t byte_count);
void sxml_javascript_release(sxml_host_processor *processor, void *memory);
uint32_t sxml_javascript_live_allocation_count(const sxml_host_processor *processor);
const sxml_host_error *sxml_javascript_error_of(const sxml_host_processor *processor);
const sxml_byte_view *sxml_javascript_output_of(const sxml_host_processor *processor);
const char *sxml_javascript_version_text(void);
sxml_host_value *sxml_javascript_parse(sxml_host_processor *processor,
                                       const sxml_host_parse_request *request);
sxml_host_value *sxml_javascript_string(sxml_host_processor *processor,
                                        const sxml_byte_view *text);
void sxml_javascript_value_release(sxml_host_value *value);
sxml_host_expression *sxml_javascript_compile(sxml_host_processor *processor,
                                              const sxml_host_compile_request *request);
void sxml_javascript_expression_destroy(sxml_host_expression *expression);
sxml_host_value *sxml_javascript_evaluate(sxml_host_expression *expression,
                                          const sxml_host_evaluate_request *request);
uint32_t sxml_javascript_serialize(sxml_host_processor *processor,
                                   const sxml_host_serialize_request *request);

/* ------------------------------------------------------ Full module only */

sxml_host_query *sxml_javascript_query_compile(sxml_host_processor *processor,
                                               const sxml_host_query_compile_request *request);
void sxml_javascript_query_destroy(sxml_host_query *query);
uint32_t sxml_javascript_query_is_updating(const sxml_host_query *query);
sxml_host_value *sxml_javascript_query_evaluate(sxml_host_query *query,
                                                sxml_host_query_evaluate_request *request);
uint32_t sxml_javascript_query_serialize(sxml_host_query *query, const sxml_host_value *value);
sxml_host_stylesheet *sxml_javascript_stylesheet_compile(
    sxml_host_processor *processor, const sxml_host_stylesheet_compile_request *request);
void sxml_javascript_stylesheet_destroy(sxml_host_stylesheet *stylesheet);
sxml_host_value *sxml_javascript_stylesheet_transform(sxml_host_stylesheet *stylesheet,
                                                      sxml_host_transform_request *request);
sxml_host_schema *sxml_javascript_schema_create(sxml_host_processor *processor);
void sxml_javascript_schema_destroy(sxml_host_schema *schema);
uint32_t sxml_javascript_schema_compile(sxml_host_schema *schema,
                                        sxml_host_schema_compile_request *request);
sxml_host_value *sxml_javascript_schema_validate(sxml_host_schema *schema,
                                                 sxml_host_validate_request *request);
