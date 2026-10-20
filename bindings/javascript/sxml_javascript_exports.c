/*
 * The exports of sxml.wasm for XPath, values and the processor (docs/
 * GLOSSARY.md, "javascript_shim"; D138, D238): each the host shim
 * function of the same name with sxml_host_ in place of sxml_javascript_,
 * so that src/index.ts keeps the names it imports. Golden file: the
 * export attributes of the JavaScript shim this replaces, now
 * bindings/host/sxml_host.c.
 */
#include "sxml_javascript.h"

#include "sxml_host.h"

#include "sxml/view.h"

#include <stddef.h>
#include <stdint.h>

SXML_JAVASCRIPT_EXPORT("sxml_javascript_feature_count")
uint32_t sxml_javascript_feature_count(void) {
    return sxml_host_feature_count();
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_feature_name_bytes")
const uint8_t *sxml_javascript_feature_name_bytes(uint32_t feature_index) {
    return sxml_host_feature_name_bytes(feature_index);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_feature_name_byte_count")
uint32_t sxml_javascript_feature_name_byte_count(uint32_t feature_index) {
    return sxml_host_feature_name_byte_count(feature_index);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_processor_create")
sxml_host_processor *sxml_javascript_processor_create(uint32_t failing_allocation_number,
                                                      uint32_t feature_mask) {
    return sxml_host_processor_create(&(sxml_host_processor_configuration){
        .failing_allocation_number = failing_allocation_number,
        .feature_mask = feature_mask,
        .resolver = sxml_javascript_resolver()});
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_processor_destroy")
uint32_t sxml_javascript_processor_destroy(sxml_host_processor *processor) {
    return sxml_host_processor_destroy(processor);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_allocate")
void *sxml_javascript_allocate(sxml_host_processor *processor, size_t byte_count) {
    return sxml_host_allocate(processor, byte_count);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_release")
void sxml_javascript_release(sxml_host_processor *processor, void *memory) {
    sxml_host_release(processor, memory);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_live_allocation_count")
uint32_t sxml_javascript_live_allocation_count(const sxml_host_processor *processor) {
    return sxml_host_live_allocation_count(processor);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_error_of")
const sxml_host_error *sxml_javascript_error_of(const sxml_host_processor *processor) {
    return sxml_host_error_of(processor);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_output_of")
const sxml_byte_view *sxml_javascript_output_of(const sxml_host_processor *processor) {
    return sxml_host_output_of(processor);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_version_text")
const char *sxml_javascript_version_text(void) {
    return sxml_host_version_text();
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_parse")
sxml_host_value *sxml_javascript_parse(sxml_host_processor *processor,
                                       const sxml_host_parse_request *request) {
    return sxml_host_parse(processor, request);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_string")
sxml_host_value *sxml_javascript_string(sxml_host_processor *processor,
                                        const sxml_byte_view *text) {
    return sxml_host_string(processor, text);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_value_release")
void sxml_javascript_value_release(sxml_host_value *value) {
    sxml_host_value_release(value);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_compile")
sxml_host_expression *sxml_javascript_compile(sxml_host_processor *processor,
                                              const sxml_host_compile_request *request) {
    return sxml_host_compile(processor, request);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_expression_destroy")
void sxml_javascript_expression_destroy(sxml_host_expression *expression) {
    sxml_host_expression_destroy(expression);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_evaluate")
sxml_host_value *sxml_javascript_evaluate(sxml_host_expression *expression,
                                          const sxml_host_evaluate_request *request) {
    return sxml_host_evaluate(expression, request);
}

SXML_JAVASCRIPT_EXPORT("sxml_javascript_serialize")
uint32_t sxml_javascript_serialize(sxml_host_processor *processor,
                                   const sxml_host_serialize_request *request) {
    return sxml_host_serialize(processor, request);
}
