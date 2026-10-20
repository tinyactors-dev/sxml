/*
 * The exports of the full module only, sxml-full.wasm (D189's two links,
 * D216, D238): XQuery, XSLT and XSD, each the host shim function of the
 * same name with sxml_host_ in place of sxml_javascript_. sxml.wasm does
 * not link this file, which is how the package tells the modules apart.
 * Golden file: sxml_javascript_exports.c.
 */
#include "sxml_javascript.h"

#include "sxml_host.h"

#include <stdint.h>

SXML_JAVASCRIPT_EXPORT("sxml_javascript_query_compile")
sxml_host_query *sxml_javascript_query_compile(sxml_host_processor *processor,
                                               const sxml_host_query_compile_request *request) {
    return sxml_host_query_compile(processor, request);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_query_destroy")
void sxml_javascript_query_destroy(sxml_host_query *query) {
    sxml_host_query_destroy(query);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_query_is_updating")
uint32_t sxml_javascript_query_is_updating(const sxml_host_query *query) {
    return sxml_host_query_is_updating(query);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_query_evaluate")
sxml_host_value *sxml_javascript_query_evaluate(sxml_host_query *query,
                                                sxml_host_query_evaluate_request *request) {
    return sxml_host_query_evaluate(query, request);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_query_serialize")
uint32_t sxml_javascript_query_serialize(sxml_host_query *query, const sxml_host_value *value) {
    return sxml_host_query_serialize(query, value);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_stylesheet_compile")
sxml_host_stylesheet *sxml_javascript_stylesheet_compile(
    sxml_host_processor *processor, const sxml_host_stylesheet_compile_request *request) {
    return sxml_host_stylesheet_compile(processor, request);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_stylesheet_destroy")
void sxml_javascript_stylesheet_destroy(sxml_host_stylesheet *stylesheet) {
    sxml_host_stylesheet_destroy(stylesheet);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_stylesheet_transform")
sxml_host_value *sxml_javascript_stylesheet_transform(sxml_host_stylesheet *stylesheet,
                                                      sxml_host_transform_request *request) {
    return sxml_host_stylesheet_transform(stylesheet, request);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_schema_create")
sxml_host_schema *sxml_javascript_schema_create(sxml_host_processor *processor) {
    return sxml_host_schema_create(processor);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_schema_destroy")
void sxml_javascript_schema_destroy(sxml_host_schema *schema) {
    sxml_host_schema_destroy(schema);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_schema_compile")
uint32_t sxml_javascript_schema_compile(sxml_host_schema *schema,
                                        sxml_host_schema_compile_request *request) {
    return sxml_host_schema_compile(schema, request);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_schema_validate")
sxml_host_value *sxml_javascript_schema_validate(sxml_host_schema *schema,
                                                 sxml_host_validate_request *request) {
    return sxml_host_schema_validate(schema, request);
}
