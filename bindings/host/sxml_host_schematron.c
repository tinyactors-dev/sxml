/*
 * The full module's Schematron entry points (include/sxml/schematron.h;
 * D315, "JavaScript"; D323): compile a Schematron schema once, describe
 * it, and validate documents with it. Compile errors are SCHEMA_REPORT
 * side results, their codes with their prefix (schematron:syntax,
 * err:XPST0003); a validation's result is one JSON object in the
 * processor's output, which the package parses:
 *
 *   {"findings":[{"kind","id","role","flag","severity","isError",
 *     "message","diagnostics":[{"id","language","text"}],
 *     "properties":[{"id","role","scheme","text"}],"patternId","test",
 *     "path","sourceName","document","lineNumber","columnNumber","schemaSourceName",
 *     "schemaLineNumber","schemaColumnNumber"}...],
 *    "valid":true|false,"phase":"...","svrl":"..."}
 *
 * Golden file: sxml_host_schema.c (a handle made empty and compiled once,
 * reports as side results).
 */
#include "sxml_host_internal.h"

#include "sxml/memory.h"
#include "sxml/processor.h"
#include "sxml/schematron.h"
#include "sxml/serialize.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xml.h"

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* A Schematron handle: made empty, compiled once; and the reports of its
   compilation. */
struct sxml_host_schematron {
    sxml_host_processor *owner;
    sxml_schematron *schematron;
    sxml_host_side_results side_results;
};

static sxml_host_side_result_target sxml_host_schematron_target(sxml_host_schematron *schematron) {
    return (sxml_host_side_result_target){.processor = schematron->owner,
                                          .side_results = &schematron->side_results};
}

sxml_host_schematron *sxml_host_schematron_create(sxml_host_processor *processor) {
    /* Results are JSON text here, never value handles. */
    (void)sxml_host_value_make;
    sxml_host_schematron *schematron = sxml_host_allocate_bytes(
        processor->allocator, sizeof(sxml_host_schematron), alignof(sxml_host_schematron));
    if (schematron != NULL) {
        *schematron = (sxml_host_schematron){
            .owner = processor,
            .schematron = NULL,
            .side_results = {.side_results = NULL, .side_result_count = 0, .side_result_capacity = 0}};
    }
    return schematron;
}

void sxml_host_schematron_destroy(sxml_host_schematron *schematron) {
    if (schematron == NULL) {
        return;
    }
    const sxml_host_side_result_target target = sxml_host_schematron_target(schematron);
    sxml_host_side_results_finalize(&target);
    sxml_schematron_destroy(schematron->schematron);
    sxml_allocator_release(schematron->owner->allocator, schematron);
}

/* ------------------------------------------------------------- Compiling */

/* A compile error as a SCHEMA_REPORT side result, its code with its
   prefix. */
static sxml_status sxml_host_receive_compile_error(void *state, const sxml_schema_report *report) {
    const sxml_host_side_result_target *target = state;
    uint8_t code[256];
    size_t code_byte_count = 0;
    const sxml_byte_view prefix = report->code.namespace_prefix;
    const sxml_byte_view local_name = report->code.local_name;
    if (prefix.byte_count > 0 && prefix.byte_count + local_name.byte_count + 1 <= sizeof(code)) {
        memcpy(code, prefix.bytes, prefix.byte_count);
        code[prefix.byte_count] = ':';
        code_byte_count = prefix.byte_count + 1;
    }
    if (code_byte_count + local_name.byte_count <= sizeof(code) && local_name.byte_count > 0) {
        memcpy(code + code_byte_count, local_name.bytes, local_name.byte_count);
        code_byte_count += local_name.byte_count;
    }
    return sxml_host_side_results_add(
        target, &(sxml_host_side_result){
                    .side_result_kind = SXML_HOST_SIDE_RESULT_KIND_SCHEMA_REPORT,
                    .uri = report->location.source_name,
                    .text = report->description,
                    .code = {.bytes = code, .byte_count = code_byte_count},
                    .first_value = NULL,
                    .second_value = NULL,
                    .line_number = report->location.line_number,
                    .column_number = report->location.column_number,
                    .is_terminating = 0});
}

const sxml_schema_report_sink_operations sxml_host_prefixed_report_operations = {
    .receive = sxml_host_receive_compile_error,
};

uint32_t sxml_host_schematron_compile(sxml_host_schematron *schematron,
                                      sxml_host_schematron_compile_request *request) {
    sxml_host_processor *processor = schematron->owner;
    sxml_host_begin(processor);
    sxml_host_side_result_target target = sxml_host_schematron_target(schematron);
    sxml_host_side_results_clear(&target);
    sxml_status status =
        schematron->schematron == NULL ? SXML_STATUS_OK : SXML_STATUS_INVALID_ARGUMENT;
    void *memory = NULL;
    if (status == SXML_STATUS_OK && request->parameter_count > 0) {
        status = sxml_host_allocate_array(
            processor->allocator,
            (sxml_host_array_request){.element = {.byte_count = sizeof(sxml_schematron_parameter),
                                                  .alignment = alignof(sxml_schematron_parameter)},
                                      .element_count = request->parameter_count},
            &memory);
    }
    sxml_schematron_parameter *parameters = memory;
    for (size_t parameter_index = 0;
         status == SXML_STATUS_OK && parameter_index < request->parameter_count;
         parameter_index += 1) {
        parameters[parameter_index] = (sxml_schematron_parameter){
            .name = request->parameters[parameter_index].name,
            .value = request->parameters[parameter_index].value};
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_schematron_create(
            processor->allocator,
            &(sxml_schematron_configuration){
                .processor = processor->processor,
                .document_source = request->text.bytes != NULL
                                       ? SXML_SCHEMATRON_DOCUMENT_SOURCE_TEXT
                                       : SXML_SCHEMATRON_DOCUMENT_SOURCE_RESOLVED,
                .document_text = request->text,
                .document_node = {.representation = 0},
                .document_uri = request->uri,
                .base_uri = request->base_uri,
                .phase_id = request->phase,
                .parameters = {.parameters = parameters,
                               .parameter_count = request->parameter_count},
                .report_sink = {.state = &target, .operations = &sxml_host_prefixed_report_operations},
                .error_record = &processor->error_record},
            &schematron->schematron);
    }
    sxml_allocator_release(processor->allocator, parameters);
    if (status != SXML_STATUS_OK) {
        sxml_host_fail(processor, status);
    }
    request->side_results = sxml_host_side_results_view(&schematron->side_results);
    return (uint32_t)status;
}

void sxml_host_schematron_describe(const sxml_host_schematron *schematron,
                                   sxml_host_schematron_description *description) {
    sxml_schematron_description described;
    memset(&described, 0, sizeof(described));
    if (schematron->schematron != NULL) {
        sxml_schematron_describe(schematron->schematron, &described);
    }
    *description = (sxml_host_schematron_description){.title = described.title,
                                                      .schema_version = described.schema_version,
                                                      .schematron_edition =
                                                          described.schematron_edition,
                                                      .query_binding = described.query_binding,
                                                      .phase = described.phase_id};
}

/* ------------------------------------------------------------ Validating */

/* The JSON of one validation, written to the processor's output. */
typedef struct sxml_host_json {
    sxml_output_stream stream;
    sxml_status status;
    size_t finding_count;
} sxml_host_json;

static void sxml_host_json_write(sxml_host_json *json, sxml_byte_view bytes) {
    if (json->status == SXML_STATUS_OK && bytes.byte_count > 0) {
        json->status = json->stream.operations->write(json->stream.state, bytes);
    }
}

static void sxml_host_json_literal(sxml_host_json *json, const char *text) {
    sxml_host_json_write(json, sxml_byte_view_from_c_string(text));
}

/* text as a JSON string (RFC 8259, 7): '"', '\' and control bytes
   escaped. */
static void sxml_host_json_string(sxml_host_json *json, sxml_byte_view text) {
    static const char hexadecimal_digits[] = "0123456789abcdef";
    sxml_host_json_literal(json, "\"");
    size_t run_start = 0;
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        if (byte >= 0x20 && byte != '"' && byte != '\\') {
            continue;
        }
        sxml_host_json_write(json, (sxml_byte_view){.bytes = text.bytes + run_start,
                                                    .byte_count = byte_index - run_start});
        uint8_t escape[6] = {'\\', byte, 0, 0, 0, 0};
        size_t escape_byte_count = 2;
        if (byte < 0x20) {
            escape[1] = 'u';
            escape[2] = '0';
            escape[3] = '0';
            escape[4] = (uint8_t)hexadecimal_digits[byte >> 4U];
            escape[5] = (uint8_t)hexadecimal_digits[byte & 0x0FU];
            escape_byte_count = 6;
        }
        sxml_host_json_write(json, (sxml_byte_view){.bytes = escape, .byte_count = escape_byte_count});
        run_start = byte_index + 1;
    }
    sxml_host_json_write(json, (sxml_byte_view){.bytes = text.bytes + run_start,
                                                .byte_count = text.byte_count - run_start});
    sxml_host_json_literal(json, "\"");
}

/* ,"name":"text" */
typedef struct sxml_host_json_member {
    const char *name;
    sxml_byte_view text;
} sxml_host_json_member;

static void sxml_host_json_text_member(sxml_host_json *json, sxml_host_json_member member) {
    sxml_host_json_literal(json, ",\"");
    sxml_host_json_literal(json, member.name);
    sxml_host_json_literal(json, "\":");
    sxml_host_json_string(json, member.text);
}

/* ,"name":NUMBER */
typedef struct sxml_host_json_number {
    const char *name;
    uint32_t value;
} sxml_host_json_number;

static void sxml_host_json_number_member(sxml_host_json *json, sxml_host_json_number member) {
    char digits[16];
    size_t digit_count = 0;
    uint32_t rest = member.value;
    do {
        digits[sizeof(digits) - 1U - digit_count] = (char)('0' + (rest % 10U));
        digit_count += 1;
        rest /= 10U;
    } while (rest > 0 && digit_count < sizeof(digits));
    sxml_host_json_literal(json, ",\"");
    sxml_host_json_literal(json, member.name);
    sxml_host_json_literal(json, "\":");
    sxml_host_json_write(json, (sxml_byte_view){.bytes = (const uint8_t *)digits + sizeof(digits) -
                                                         digit_count,
                                                .byte_count = digit_count});
}

static const char *const sxml_host_report_kinds[] = {"failed-assert", "successful-report",
                                                     "fired-rule"};

static void sxml_host_json_diagnostics(sxml_host_json *json, const sxml_schematron_report *report) {
    sxml_host_json_literal(json, ",\"diagnostics\":[");
    for (size_t index = 0; index < report->diagnostics.diagnostic_count; index += 1) {
        const sxml_schematron_diagnostic *diagnostic = &report->diagnostics.diagnostics[index];
        sxml_host_json_literal(json, index == 0 ? "{\"id\":" : ",{\"id\":");
        sxml_host_json_string(json, diagnostic->diagnostic_id);
        sxml_host_json_text_member(json, (sxml_host_json_member){"language", diagnostic->language});
        sxml_host_json_text_member(json, (sxml_host_json_member){"text", diagnostic->text});
        sxml_host_json_literal(json, "}");
    }
    sxml_host_json_literal(json, "],\"properties\":[");
    for (size_t index = 0; index < report->properties.property_count; index += 1) {
        const sxml_schematron_property *property = &report->properties.properties[index];
        sxml_host_json_literal(json, index == 0 ? "{\"id\":" : ",{\"id\":");
        sxml_host_json_string(json, property->property_id);
        sxml_host_json_text_member(json, (sxml_host_json_member){"role", property->role});
        sxml_host_json_text_member(json, (sxml_host_json_member){"scheme", property->scheme});
        sxml_host_json_text_member(json, (sxml_host_json_member){"text", property->text});
        sxml_host_json_literal(json, "}");
    }
    sxml_host_json_literal(json, "]");
}

static sxml_status sxml_host_receive_finding(void *state, const sxml_schematron_report *report) {
    sxml_host_json *json = state;
    sxml_host_json_literal(json, json->finding_count == 0 ? "{\"kind\":" : ",{\"kind\":");
    json->finding_count += 1;
    sxml_host_json_literal(json, "\"");
    sxml_host_json_literal(json, sxml_host_report_kinds[(uint32_t)report->report_kind % 3U]);
    sxml_host_json_literal(json, "\"");
    const sxml_host_json_member members[] = {
        {"id", report->id},
        {"role", report->role},
        {"flag", report->flag},
        {"severity", report->severity},
        {"message", report->message},
        {"patternId", report->pattern_id},
        {"test", report->test_text},
        {"path", report->node_path},
        {"sourceName", report->node_location.source_name},
        {"document", report->document_uri},
        {"schemaSourceName", report->schema_location.source_name}};
    for (size_t member_index = 0; member_index < sizeof(members) / sizeof(members[0]);
         member_index += 1) {
        sxml_host_json_text_member(json, members[member_index]);
    }
    sxml_host_json_literal(json, report->is_error ? ",\"isError\":true" : ",\"isError\":false");
    const sxml_host_json_number numbers[] = {
        {"lineNumber", report->node_location.line_number},
        {"columnNumber", report->node_location.column_number},
        {"schemaLineNumber", report->schema_location.line_number},
        {"schemaColumnNumber", report->schema_location.column_number}};
    for (size_t number_index = 0; number_index < sizeof(numbers) / sizeof(numbers[0]);
         number_index += 1) {
        sxml_host_json_number_member(json, numbers[number_index]);
    }
    sxml_host_json_diagnostics(json, report);
    sxml_host_json_literal(json, "}");
    return json->status;
}

static const sxml_schematron_report_sink_operations sxml_host_finding_operations = {
    .receive = sxml_host_receive_finding,
};

/* The node to validate: request->node's first item, or request->text
   parsed (with the processor's location policy) into *parsed. */
static sxml_status sxml_host_schematron_node(sxml_host_processor *processor,
                                             const sxml_host_schematron_validate_request *request,
                                             sxml_item *parsed) {
    if (request->node != NULL) {
        return sxml_sequence_get_item(request->node->sequence,
                                      (sxml_item_request){.item_index = 0, .error_record = NULL},
                                      parsed);
    }
    return sxml_processor_parse_document(
        processor->processor,
        &(sxml_xml_parse_request){.input = request->text,
                                  .source_name = request->document_uri,
                                  .document_uri = request->document_uri,
                                  .base_uri = request->document_uri,
                                  .diagnostic = &processor->error_record.diagnostic},
        parsed);
}

/* The SVRL of one validation, collected and written as a JSON string. */
static sxml_status sxml_host_schematron_run(sxml_host_schematron *schematron,
                                            const sxml_host_schematron_validate_request *request,
                                            sxml_host_json *json) {
    sxml_host_processor *processor = schematron->owner;
    sxml_item node = {.representation = 0};
    sxml_status status = sxml_host_schematron_node(processor, request, &node);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_output_buffer *svrl = NULL;
    if (request->writes_svrl != 0) {
        status = sxml_output_buffer_create(
            processor->allocator,
            &(sxml_output_buffer_configuration){.allocator = processor->allocator,
                                                .maximum_byte_count = 0},
            &svrl);
    }
    sxml_schematron_outcome outcome;
    memset(&outcome, 0, sizeof(outcome));
    if (status == SXML_STATUS_OK) {
        sxml_host_json_literal(json, "{\"findings\":[");
        status = sxml_schematron_validate_node(
            schematron->schematron,
            &(sxml_schematron_validation_request){
                .node = node,
                .report_sink = {.state = json, .operations = &sxml_host_finding_operations},
                .svrl_output_stream = svrl != NULL
                                          ? sxml_output_buffer_as_output_stream(svrl)
                                          : (sxml_output_stream){.state = NULL, .operations = NULL},
                .error_record = &processor->error_record,
                .reports_fired_rules = request->reports_fired_rules != 0},
            &outcome);
    }
    if (status == SXML_STATUS_OK) {
        sxml_host_json_literal(json, outcome.error_finding_count == 0 ? "],\"valid\":true"
                                                                       : "],\"valid\":false");
        sxml_host_json_text_member(json, (sxml_host_json_member){"phase", outcome.phase_id});
        if (svrl != NULL) {
            sxml_host_json_text_member(json,
                                       (sxml_host_json_member){"svrl", sxml_output_buffer_get_bytes(svrl)});
        }
        sxml_host_json_literal(json, "}");
        status = json->status;
    }
    sxml_output_buffer_destroy(svrl);
    if (request->node == NULL) {
        sxml_item_release(node);
    }
    return status;
}

uint32_t sxml_host_schematron_validate(sxml_host_schematron *schematron,
                                       const sxml_host_schematron_validate_request *request) {
    sxml_host_processor *processor = schematron->owner;
    sxml_host_begin(processor);
    if (schematron->schematron == NULL) {
        sxml_host_fail(processor, SXML_STATUS_INVALID_ARGUMENT);
        return (uint32_t)SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_host_json json = {.stream = {.state = NULL, .operations = NULL},
                           .status = SXML_STATUS_OK,
                           .finding_count = 0};
    sxml_status status = sxml_host_output_begin(processor, &json.stream);
    if (status == SXML_STATUS_OK) {
        status = sxml_host_schematron_run(schematron, request, &json);
    }
    status = sxml_host_output_end(processor, status);
    if (status != SXML_STATUS_OK) {
        sxml_host_fail(processor, status);
    }
    return (uint32_t)status;
}
