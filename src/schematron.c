/*
 * Schematron through the public API (include/sxml/schematron.h; D313, D315,
 * D323): compiling a schema with src/schematron_read.c and
 * src/schematron_build.c into a stylesheet AST compiled as XSLT, and
 * validating nodes with it, turning the transformation's finding items
 * into schematron reports.
 *
 * Golden file: src/schema.c (the lifecycle of a compiled schema, a report
 * sink, compile once and validate many). Clause numbers are of ISO/IEC
 * 19757-3:2025.
 */
#include "sxml/schematron.h"

#include "schematron_internal.h"

#include "memory_internal.h"
#include "processor_internal.h"
#include "xslt_transform_internal.h"

#include <stdalign.h>
#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view no_text = {.bytes = NULL, .byte_count = 0};

struct sxml_schematron {
    sxml_processor *processor;
    /* The allocator of sxml_schematron_create; the null allocator after
       initialize. */
    sxml_allocator creating_allocator;
    /* Owned; made from the AST. */
    sxml_xslt_stylesheet *stylesheet;
    /* Owned: the static table and its texts. */
    sxml_schematron_table table;
    /* The schematron documents' URIs, owned (the schema locations of
       reports). */
    sxml_byte_view *document_uris;
    size_t document_count;
    /* Q{urn:sxml:schematron}fired-rules, the stylesheet parameter. */
    sxml_name_identifier fired_rules_name;
};

/* ------------------------------------------------------------ Lifecycle */

static bool sxml_schematron_configuration_is_valid(const sxml_schematron_configuration *configuration) {
    if (configuration == NULL || configuration->processor == NULL) {
        return false;
    }
    switch (configuration->document_source) {
    case SXML_SCHEMATRON_DOCUMENT_SOURCE_TEXT:
    case SXML_SCHEMATRON_DOCUMENT_SOURCE_RESOLVED:
    case SXML_SCHEMATRON_DOCUMENT_SOURCE_NODE:
        break;
    default:
        return false;
    }
    const sxml_schematron_parameter_view parameters = configuration->parameters;
    if (parameters.parameter_count > 0 && parameters.parameters == NULL) {
        return false;
    }
    for (size_t parameter_index = 0; parameter_index < parameters.parameter_count; parameter_index += 1) {
        for (size_t earlier_index = 0; earlier_index < parameter_index; earlier_index += 1) {
            if (sxml_byte_view_equals(parameters.parameters[earlier_index].name,
                                      parameters.parameters[parameter_index].name)) {
                return false;
            }
        }
    }
    return true;
}

sxml_status sxml_schematron_memory_requirements(const sxml_schematron_configuration *configuration,
                                                sxml_memory_requirements *requirements) {
    if (configuration == NULL || configuration->processor == NULL || requirements == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){.byte_count = sizeof(sxml_schematron),
                                               .alignment = alignof(sxml_schematron)};
    return SXML_STATUS_OK;
}

/* Gives an error of the XSLT compile stage to the compilation (its sink,
   and the record when first). */
static sxml_status sxml_schematron_forward_report(void *state, const sxml_schema_report *report) {
    sxml_schematron_compilation *compilation = state;
    const sxml_error_record record = {.code = report->code,
                                      .value = NULL,
                                      .diagnostic = {.description = report->description,
                                                     .location = report->location}};
    return sxml_schematron_report_record(compilation, &record);
}

static const sxml_schema_report_sink_operations sxml_schematron_forwarding_sink = {
    .receive = sxml_schematron_forward_report};

/* Keeps the documents' URIs (taken from the compilation). */
static sxml_status sxml_schematron_keep_uris(sxml_schematron_compilation *compilation,
                                             sxml_schematron *compiled) {
    const size_t count = compilation->documents.count;
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate_array(
        compilation->allocator,
        (sxml_array_allocation_request){.element_count = count == 0 ? 1 : count,
                                        .element_size = sizeof(sxml_byte_view),
                                        .element_alignment = alignof(sxml_byte_view)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    compiled->document_uris = memory;
    compiled->document_count = count;
    for (uint32_t document = 0; document < count; document += 1) {
        sxml_schematron_document *entry = sxml_schematron_document_at(compilation, document);
        compiled->document_uris[document] = entry->uri;
        entry->uri = no_text;
    }
    return SXML_STATUS_OK;
}

static void sxml_schematron_release(sxml_schematron *schematron) {
    sxml_allocator allocator = schematron->processor->allocator;
    sxml_xslt_stylesheet_destroy(schematron->stylesheet);
    schematron->stylesheet = NULL;
    sxml_schematron_table_release(allocator, &schematron->table);
    for (size_t document = 0; document < schematron->document_count; document += 1) {
        sxml_allocator_release(allocator, (void *)schematron->document_uris[document].bytes);
    }
    sxml_allocator_release(allocator, schematron->document_uris);
    schematron->document_uris = NULL;
    schematron->document_count = 0;
}

/* Reads, builds and compiles into *compiled (zeroed by the caller). */
static sxml_status sxml_schematron_compile(const sxml_schematron_configuration *configuration,
                                           sxml_schematron *compiled) {
    sxml_processor *processor = configuration->processor;
    sxml_schematron_compilation compilation;
    memset(&compilation, 0, sizeof(compilation));
    compilation.processor = processor;
    compilation.allocator = processor->allocator;
    compilation.name_pool = processor->name_pool;
    compilation.configuration = configuration;
    compilation.description = sxml_xml_buffer_empty(processor->allocator);
    sxml_status status = sxml_schematron_read(&compilation);
    /* After errors of the grammar, a report sink still receives those of
       the expressions, which building finds (D315 "Compile errors"); the
       XSLT compile then does not run. */
    if (status == SXML_STATUS_OK &&
        (compilation.error_count == 0 || configuration->report_sink.operations != NULL)) {
        status = sxml_schematron_build(&compilation);
    }
    if (status == SXML_STATUS_OK && compilation.error_count == 0) {
        status = sxml_name_pool_intern_name(
            processor->name_pool,
            &(sxml_name_request){.namespace_uri = compilation.names.private_namespace,
                                 .local_name = SXML_TEXT("fired-rules")},
            &compiled->fired_rules_name);
    }
    if (status == SXML_STATUS_OK && compilation.error_count == 0) {
        /* The AST goes to the stylesheet, made or not. */
        sxml_xslt_ast *ast = compilation.ast;
        compilation.ast = NULL;
        sxml_error_record compile_record;
        memset(&compile_record, 0, sizeof(compile_record));
        status = sxml_xslt_stylesheet_create_from_ast(
            processor->allocator,
            &(sxml_xslt_ast_stylesheet_request){
                .processor = processor,
                .ast = ast,
                .error_record = &compile_record,
                .source_name = configuration->document_uri,
                .report_sink = {.state = &compilation, .operations = &sxml_schematron_forwarding_sink},
                .reads_source_locations = true},
            &compiled->stylesheet);
        if (status != SXML_STATUS_OK && compilation.error_count == 0 &&
            sxml_status_has_error_record(status) && configuration->error_record != NULL &&
            status != SXML_STATUS_LIMIT_EXCEEDED) {
            /* An error no sink saw (a stage that stops at its first). */
            *configuration->error_record = compile_record;
        }
    }
    if (status == SXML_STATUS_OK && compilation.error_count > 0) {
        status = SXML_STATUS_STATIC_ERROR;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_schematron_keep_uris(&compilation, compiled);
    }
    if (status == SXML_STATUS_OK) {
        compiled->table = compilation.table;
        memset(&compilation.table, 0, sizeof(compilation.table));
    }
    sxml_xslt_ast_destroy(compilation.ast);
    sxml_schematron_table_release(compilation.allocator, &compilation.table);
    sxml_schematron_read_release(&compilation);
    sxml_xml_buffer_release(&compilation.description);
    if (status != SXML_STATUS_OK) {
        sxml_schematron_release(compiled);
    }
    return status;
}

sxml_status sxml_schematron_initialize(sxml_memory_view memory,
                                       const sxml_schematron_configuration *configuration,
                                       sxml_schematron **schematron) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_schematron_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK &&
        (schematron == NULL || !sxml_schematron_configuration_is_valid(configuration))) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_view_check_requirements(memory, requirements);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_schematron compiled;
    memset(&compiled, 0, sizeof(compiled));
    compiled.processor = configuration->processor;
    status = sxml_schematron_compile(configuration, &compiled);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_schematron *new_schematron = placement;
    *new_schematron = compiled;
    *schematron = new_schematron;
    return SXML_STATUS_OK;
}

void sxml_schematron_finalize(sxml_schematron *schematron) {
    if (schematron == NULL) {
        return;
    }
    sxml_schematron_release(schematron);
}

sxml_status sxml_schematron_create(sxml_allocator allocator,
                                   const sxml_schematron_configuration *configuration,
                                   sxml_schematron **schematron) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_schematron_memory_requirements(configuration, &requirements);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = requirements.byte_count,
                                      .alignment = requirements.alignment},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_schematron *new_schematron = NULL;
    status = sxml_schematron_initialize(
        (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count}, configuration,
        &new_schematron);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_schematron->creating_allocator = allocator;
    *schematron = new_schematron;
    return SXML_STATUS_OK;
}

void sxml_schematron_destroy(sxml_schematron *schematron) {
    if (schematron == NULL) {
        return;
    }
    const sxml_allocator allocator = schematron->creating_allocator;
    sxml_schematron_finalize(schematron);
    sxml_allocator_release(allocator, schematron);
}

void sxml_schematron_describe(const sxml_schematron *schematron,
                              sxml_schematron_description *description) {
    const sxml_schematron_table *table = &schematron->table;
    *description = (sxml_schematron_description){
        .title = sxml_schematron_table_text(table, table->title),
        .schema_version = sxml_schematron_table_text(table, table->schema_version),
        .schematron_edition = sxml_schematron_table_text(table, table->schematron_edition),
        .query_binding = sxml_schematron_table_text(table, table->query_binding),
        .phase_id = sxml_schematron_table_text(table, table->phase)};
}

/* ----------------------------------------------------------- Validation */

/* The texts of one report, gathered in one buffer and turned into views
   once complete (the buffer may move while it grows). */
typedef struct report_texts {
    sxml_xml_buffer buffer;
    /* Offsets and lengths: role, flag, severity, path, message, then the
       diagnostics' and the properties'. */
    size_t offsets[64];
    size_t lengths[64];
    size_t text_count;
} report_texts;

/* Appends the string of a member: each item's string value (a text
   node's content, an atomic item's string), concatenated. */
static sxml_status append_member_text(report_texts *texts, sxml_sequence member) {
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(member);
    for (;;) {
        sxml_iteration_step step;
        memset(&step, 0, sizeof(step));
        sxml_status status = sxml_sequence_iterator_next(&iterator, NULL, &step);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!step.has_item) {
            return SXML_STATUS_OK;
        }
        sxml_node_reference node = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0};
        if (sxml_item_get_node(step.item, &node) == SXML_STATUS_OK) {
            sxml_byte_view content = no_text;
            sxml_node_description description;
            memset(&description, 0, sizeof(description));
            status = sxml_node_describe(node, &description);
            if (status == SXML_STATUS_OK && description.node_kind == SXML_NODE_KIND_TEXT) {
                status = sxml_node_get_content(node, &content);
                if (status == SXML_STATUS_OK) {
                    status = sxml_xml_buffer_append(&texts->buffer, content);
                }
            }
        } else {
            sxml_atomic_datum datum;
            memset(&datum, 0, sizeof(datum));
            if (sxml_atomic_item_get_datum(step.item, &datum) == SXML_STATUS_OK &&
                datum.datum_kind == SXML_DATUM_KIND_STRING) {
                status = sxml_xml_buffer_append(&texts->buffer, datum.datum_details.string.utf8);
            }
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
}

/* Collapses whitespace (5.3): runs of the four whitespace characters to
   one space, none at either end; in place, from offset. */
static void collapse_whitespace(sxml_xml_buffer *buffer, size_t offset) {
    uint8_t *bytes = buffer->bytes;
    size_t written = offset;
    bool is_pending_space = false;
    for (size_t byte_index = offset; byte_index < buffer->byte_count; byte_index += 1) {
        const uint8_t byte = bytes[byte_index];
        if (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r') {
            is_pending_space = written > offset;
            continue;
        }
        if (is_pending_space) {
            bytes[written] = ' ';
            written += 1;
            is_pending_space = false;
        }
        bytes[written] = byte;
        written += 1;
    }
    buffer->byte_count = written;
}

/* One text of the report from a member; collapsed when asked. */
typedef struct member_text {
    sxml_sequence member;
    bool collapses;
    bool trims;
} member_text;

static sxml_status add_member_text(report_texts *texts, member_text request) {
    if (texts->text_count >= 64) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    const size_t offset = texts->buffer.byte_count;
    sxml_status status = append_member_text(texts, request.member);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (request.collapses || request.trims) {
        collapse_whitespace(&texts->buffer, offset);
    }
    texts->offsets[texts->text_count] = offset;
    texts->lengths[texts->text_count] = texts->buffer.byte_count - offset;
    texts->text_count += 1;
    return SXML_STATUS_OK;
}

static sxml_byte_view text_at(const report_texts *texts, size_t text_index) {
    return (sxml_byte_view){.bytes = texts->buffer.bytes + texts->offsets[text_index],
                            .byte_count = texts->lengths[text_index]};
}

static bool is_error_word(sxml_byte_view word) {
    return sxml_byte_view_equals(word, SXML_TEXT("fatal")) ||
           sxml_byte_view_equals(word, SXML_TEXT("error"));
}

/* D313 item 4, D315 "Error findings" (the owner, 1a): a severity decides
   when present; else the role or a flag token is fatal or error, or there
   is neither role nor flag. */
static bool is_error_finding(const sxml_schematron_report *report) {
    if (report->severity.byte_count > 0) {
        return is_error_word(report->severity);
    }
    if (report->role.byte_count == 0 && report->flag.byte_count == 0) {
        return true;
    }
    if (is_error_word(report->role)) {
        return true;
    }
    sxml_byte_view rest = report->flag;
    sxml_byte_view token = no_text;
    while (sxml_xml_text_next_token(&rest, &token)) {
        if (is_error_word(token)) {
            return true;
        }
    }
    return false;
}

/* What one validation counts and where it writes. */
typedef struct schematron_validation {
    const sxml_schematron *schematron;
    const sxml_schematron_validation_request *request;
    sxml_schematron_outcome *outcome;
    sxml_schematron_svrl_writer svrl;
    bool writes_svrl;
    report_texts texts;
} schematron_validation;

/* A dynamic error of the validation located at a schematron document
   (XPTY0004 for a subject that is not one node, D315). */
static sxml_status raise_subject_error(schematron_validation *validation, const sxml_schematron_entry *entry) {
    sxml_error_record *record = validation->request->error_record;
    if (record != NULL) {
        const sxml_schematron *schematron = validation->schematron;
        *record = (sxml_error_record){
            .code = {.namespace_uri = SXML_TEXT(SXML_ERROR_NAMESPACE_URI),
                     .local_name = SXML_TEXT("XPTY0004"),
                     .namespace_prefix = SXML_TEXT("err")},
            .value = NULL,
            .diagnostic = {
                .description = SXML_TEXT("the subject of a finding is not one node (5.5.19)"),
                .location = {.source_name = entry->subject_document < schematron->document_count
                                                ? schematron->document_uris[entry->subject_document]
                                                : no_text,
                             .line_number = entry->subject_span.line_number,
                             .column_number = entry->subject_span.column_number,
                             .byte_offset = entry->subject_span.byte_offset}}};
    }
    return SXML_STATUS_TYPE_ERROR;
}

/* The URI of the document a node is in, for SVRL's fired-rule document. */
static sxml_byte_view document_uri_of(sxml_item node_item) {
    sxml_node_reference node = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0};
    sxml_tree_description description;
    memset(&description, 0, sizeof(description));
    if (sxml_item_get_node(node_item, &node) != SXML_STATUS_OK ||
        sxml_tree_describe(node.tree, &description) != SXML_STATUS_OK) {
        return no_text;
    }
    return description.document_uri;
}

/* Turns one finding item into a report and delivers it. */
static sxml_status deliver_item(schematron_validation *validation, sxml_item item) {
    const sxml_schematron *schematron = validation->schematron;
    const sxml_schematron_table *table = &schematron->table;
    size_t member_count = 0;
    sxml_status status = sxml_array_get_size(item, &member_count);
    if (status != SXML_STATUS_OK || member_count < 3) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_sequence members[3];
    for (size_t member_index = 0; member_index < 3; member_index += 1) {
        status = sxml_array_get_member(item, member_index, &members[member_index]);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_item index_item = {.representation = 0};
    sxml_atomic_datum datum;
    memset(&datum, 0, sizeof(datum));
    status = sxml_sequence_get_item(members[0], (sxml_item_request){.item_index = 0, .error_record = NULL},
                                    &index_item);
    if (status == SXML_STATUS_OK) {
        status = sxml_atomic_item_get_datum(index_item, &datum);
    }
    if (status != SXML_STATUS_OK || datum.datum_kind != SXML_DATUM_KIND_INTEGER ||
        datum.datum_details.integer < 0 || (uint64_t)datum.datum_details.integer >= table->entry_count) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_schematron_entry *entry = &table->entries[datum.datum_details.integer];
    const bool is_rule = entry->entry_kind == SXML_SCHEMATRON_ENTRY_RULE;
    /* The subject: exactly one node (5.5.19). */
    uint64_t subject_count = 0;
    status = sxml_sequence_count(members[1], NULL, &subject_count);
    sxml_item subject = {.representation = 0};
    if (status == SXML_STATUS_OK && subject_count == 1) {
        status = sxml_sequence_get_item(members[1], (sxml_item_request){.item_index = 0, .error_record = NULL},
                                        &subject);
    }
    sxml_node_reference subject_node = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0};
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (subject_count != 1 || sxml_item_get_node(subject, &subject_node) != SXML_STATUS_OK) {
        return raise_subject_error(validation, entry);
    }
    report_texts *texts = &validation->texts;
    texts->buffer.byte_count = 0;
    texts->text_count = 0;
    status = add_member_text(texts, (member_text){.member = members[2], .collapses = false, .trims = false});
    size_t next_member = 3;
    /* Dynamic role, flag, severity (5.5.6, 5.5.14, 5.5.16). */
    size_t dynamic_texts[3] = {SIZE_MAX, SIZE_MAX, SIZE_MAX};
    const uint32_t bits[3] = {SXML_SCHEMATRON_DYNAMIC_ROLE, SXML_SCHEMATRON_DYNAMIC_FLAG,
                              SXML_SCHEMATRON_DYNAMIC_SEVERITY};
    for (size_t dynamic_index = 0; status == SXML_STATUS_OK && dynamic_index < 3; dynamic_index += 1) {
        if ((entry->dynamic_mask & bits[dynamic_index]) == 0) {
            continue;
        }
        sxml_sequence member = sxml_sequence_empty();
        status = sxml_array_get_member(item, next_member, &member);
        next_member += 1;
        if (status == SXML_STATUS_OK) {
            dynamic_texts[dynamic_index] = texts->text_count;
            status = add_member_text(texts, (member_text){.member = member, .collapses = false, .trims = true});
        }
    }
    size_t message_text = SIZE_MAX;
    size_t first_annotation_text = 0;
    if (status == SXML_STATUS_OK && !is_rule) {
        sxml_sequence member = sxml_sequence_empty();
        status = sxml_array_get_member(item, next_member, &member);
        next_member += 1;
        if (status == SXML_STATUS_OK) {
            message_text = texts->text_count;
            status = add_member_text(
                texts, (member_text){.member = member, .collapses = !entry->preserves_space, .trims = false});
        }
        first_annotation_text = texts->text_count;
        const size_t annotation_count =
            (size_t)entry->diagnostics.reference_count + entry->properties.reference_count;
        for (size_t annotation_index = 0; status == SXML_STATUS_OK && annotation_index < annotation_count;
             annotation_index += 1) {
            const bool is_property = annotation_index >= entry->diagnostics.reference_count;
            const uint32_t reference =
                is_property ? table->references[entry->properties.first_reference + annotation_index -
                                                entry->diagnostics.reference_count]
                            : table->references[entry->diagnostics.first_reference + annotation_index];
            const sxml_schematron_annotation_entry *annotation =
                is_property ? &table->properties[reference] : &table->diagnostics[reference];
            status = sxml_array_get_member(item, next_member, &member);
            next_member += 1;
            if (status == SXML_STATUS_OK) {
                status = add_member_text(texts, (member_text){.member = member,
                                                              .collapses = !annotation->preserves_space,
                                                              .trims = false});
            }
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* The report: every view stays valid for the call. */
    sxml_schematron_diagnostic diagnostics[32];
    sxml_schematron_property properties[32];
    const size_t diagnostic_count =
        is_rule || entry->diagnostics.reference_count > 32 ? 0 : entry->diagnostics.reference_count;
    const size_t property_count =
        is_rule || entry->properties.reference_count > 32 ? 0 : entry->properties.reference_count;
    for (size_t diagnostic_index = 0; diagnostic_index < diagnostic_count; diagnostic_index += 1) {
        const sxml_schematron_annotation_entry *annotation =
            &table->diagnostics[table->references[entry->diagnostics.first_reference + diagnostic_index]];
        diagnostics[diagnostic_index] = (sxml_schematron_diagnostic){
            .diagnostic_id = sxml_schematron_table_text(table, annotation->identifier),
            .language = sxml_schematron_table_text(table, annotation->language),
            .text = text_at(texts, first_annotation_text + diagnostic_index)};
    }
    for (size_t property_index = 0; property_index < property_count; property_index += 1) {
        const sxml_schematron_annotation_entry *annotation =
            &table->properties[table->references[entry->properties.first_reference + property_index]];
        properties[property_index] = (sxml_schematron_property){
            .property_id = sxml_schematron_table_text(table, annotation->identifier),
            .role = sxml_schematron_table_text(table, annotation->role),
            .scheme = sxml_schematron_table_text(table, annotation->scheme),
            .text = text_at(texts, first_annotation_text + entry->diagnostics.reference_count + property_index)};
    }
    sxml_source_location node_location;
    memset(&node_location, 0, sizeof(node_location));
    if (sxml_node_get_location(subject_node, &node_location) != SXML_STATUS_OK) {
        memset(&node_location, 0, sizeof(node_location));
    }
    sxml_schematron_report report = {
        .report_kind = is_rule ? SXML_SCHEMATRON_REPORT_KIND_FIRED_RULE
                       : entry->entry_kind == SXML_SCHEMATRON_ENTRY_REPORT
                           ? SXML_SCHEMATRON_REPORT_KIND_SUCCESSFUL_REPORT
                           : SXML_SCHEMATRON_REPORT_KIND_FAILED_ASSERT,
        .id = sxml_schematron_table_text(table, entry->identifier),
        .role = dynamic_texts[0] != SIZE_MAX ? text_at(texts, dynamic_texts[0])
                                             : sxml_schematron_table_text(table, entry->role),
        .flag = dynamic_texts[1] != SIZE_MAX ? text_at(texts, dynamic_texts[1])
                                             : sxml_schematron_table_text(table, entry->flag),
        .severity = is_rule ? no_text
                    : dynamic_texts[2] != SIZE_MAX ? text_at(texts, dynamic_texts[2])
                                                   : sxml_schematron_table_text(table, entry->severity),
        .see = sxml_schematron_table_text(table, entry->see),
        .icon = sxml_schematron_table_text(table, entry->icon),
        .formal_public_identifier = sxml_schematron_table_text(table, entry->formal_public_identifier),
        .language = sxml_schematron_table_text(table, entry->language),
        .test_text = is_rule ? no_text : sxml_schematron_table_text(table, entry->test),
        .context_text = sxml_schematron_table_text(table, entry->context),
        .pattern_id = entry->pattern < table->pattern_count
                          ? sxml_schematron_table_text(table, table->patterns[entry->pattern].identifier)
                          : no_text,
        .pattern_index = entry->pattern,
        .message = message_text != SIZE_MAX ? text_at(texts, message_text) : no_text,
        .diagnostics = {.diagnostics = diagnostics, .diagnostic_count = diagnostic_count},
        .properties = {.properties = properties, .property_count = property_count},
        .schema_location = {.source_name = entry->document < schematron->document_count
                                               ? schematron->document_uris[entry->document]
                                               : no_text,
                            .line_number = entry->source_span.line_number,
                            .column_number = entry->source_span.column_number,
                            .byte_offset = entry->source_span.byte_offset},
        .node = subject,
        .node_location = node_location,
        .node_path = text_at(texts, 0),
        .is_error = false};
    if (!is_rule) {
        report.is_error = is_error_finding(&report);
        validation->outcome->finding_count += 1;
        if (report.is_error) {
            validation->outcome->error_finding_count += 1;
        }
    }
    const sxml_schematron_report_sink sink = validation->request->report_sink;
    if (sink.operations != NULL && sink.operations->receive != NULL &&
        (!is_rule || validation->request->reports_fired_rules)) {
        status = sink.operations->receive(sink.state, &report);
    }
    if (status == SXML_STATUS_OK && validation->writes_svrl) {
        const bool is_subordinate = entry->pattern < table->pattern_count &&
                                    table->patterns[entry->pattern].documents.byte_count > 0;
        status = sxml_schematron_svrl_write(
            &validation->svrl,
            &(sxml_schematron_svrl_report){.report = &report,
                                           .entry = entry,
                                           .document_uri = is_rule && is_subordinate
                                                               ? document_uri_of(subject)
                                                               : no_text});
    }
    return status;
}

sxml_status sxml_schematron_validate_node(const sxml_schematron *schematron,
                                          const sxml_schematron_validation_request *request,
                                          sxml_schematron_outcome *outcome) {
    sxml_node_reference node = {.tree = NULL, .node_index = {.index = 0}, .namespace_number = 0};
    if (schematron == NULL || request == NULL || outcome == NULL ||
        sxml_item_get_node(request->node, &node) != SXML_STATUS_OK) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_processor *processor = schematron->processor;
    const sxml_schematron_table *table = &schematron->table;
    sxml_schematron_outcome counts = {.finding_count = 0,
                                      .error_finding_count = 0,
                                      .phase_id = sxml_schematron_table_text(table, table->phase)};
    /* The instance document root: the global context item (5.4.7). */
    const sxml_item root = sxml_node_item_make(
        (sxml_node_reference){.tree = node.tree, .node_index = {.index = 0}, .namespace_number = 0});
    const bool writes_svrl = request->svrl_output_stream.operations != NULL;
    const sxml_variable_binding binding = {
        .variable_name = schematron->fired_rules_name,
        .value = sxml_sequence_from_item(request->reports_fired_rules || writes_svrl
                                             ? sxml_atomic_item_true()
                                             : sxml_atomic_item_false())};
    sxml_dynamic_context_configuration dynamic_context;
    memset(&dynamic_context, 0, sizeof(dynamic_context));
    dynamic_context.has_context_value = true;
    dynamic_context.context_value = sxml_sequence_from_item(root);
    dynamic_context.variable_bindings =
        (sxml_variable_binding_view){.variable_bindings = &binding, .variable_binding_count = 1};
    sxml_sequence raw_result = sxml_sequence_empty();
    sxml_status status = sxml_xslt_stylesheet_transform(
        schematron->stylesheet,
        &(sxml_xslt_transformation_request){
            .dynamic_context = dynamic_context,
            /* The root is the default initial match selection: the
               compiled unit serves it. Another node is given, which
               lowers the stylesheet for this invocation. */
            .initial_match_selection = sxml_sequence_from_item(request->node),
            .has_initial_match_selection = node.node_index.index != 0,
            .invocation_kind = SXML_XSLT_INVOCATION_KIND_APPLY_TEMPLATES,
            .result_delivery = SXML_XSLT_RESULT_DELIVERY_RAW,
            .error_record = request->error_record},
        &raw_result);
    sxml_item_release(root);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    schematron_validation run = {.schematron = schematron,
                      .request = request,
                      .outcome = &counts,
                      .svrl = {.output_stream = request->svrl_output_stream,
                               .table = table,
                               .phase_id = counts.phase_id,
                               .phase_patterns = {.first_reference = 0, .reference_count = 0},
                               .has_phase_patterns = false,
                               .open_pattern = SIZE_MAX,
                               .buffer = sxml_xml_buffer_empty(processor->allocator),
                               .status = SXML_STATUS_OK},
                      .writes_svrl = writes_svrl,
                      .texts = {.buffer = sxml_xml_buffer_empty(processor->allocator), .text_count = 0}};
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(raw_result);
    bool is_first = true;
    for (;;) {
        sxml_iteration_step step;
        memset(&step, 0, sizeof(step));
        status = sxml_sequence_iterator_next(&iterator, request->error_record, &step);
        if (status != SXML_STATUS_OK || !step.has_item) {
            break;
        }
        if (is_first && table->selects_phase) {
            /* "#ANY": the phase's number first (D315). */
            sxml_atomic_datum datum;
            memset(&datum, 0, sizeof(datum));
            if (sxml_atomic_item_get_datum(step.item, &datum) == SXML_STATUS_OK &&
                datum.datum_kind == SXML_DATUM_KIND_INTEGER && datum.datum_details.integer > 0 &&
                (uint64_t)datum.datum_details.integer <= table->phase_count) {
                const sxml_schematron_phase_entry *phase = &table->phases[datum.datum_details.integer - 1];
                counts.phase_id = sxml_schematron_table_text(table, phase->identifier);
                run.svrl.phase_patterns = phase->patterns;
                run.svrl.has_phase_patterns = true;
            } else {
                counts.phase_id = SXML_TEXT("#ALL");
            }
            run.svrl.phase_id = counts.phase_id;
            is_first = false;
            if (writes_svrl) {
                status = sxml_schematron_svrl_start(&run.svrl);
                if (status != SXML_STATUS_OK) {
                    break;
                }
            }
            continue;
        }
        if (is_first && writes_svrl) {
            status = sxml_schematron_svrl_start(&run.svrl);
            if (status != SXML_STATUS_OK) {
                break;
            }
        }
        is_first = false;
        status = deliver_item(&run, step.item);
        if (status != SXML_STATUS_OK) {
            break;
        }
    }
    if (status == SXML_STATUS_OK && writes_svrl) {
        if (is_first) {
            status = sxml_schematron_svrl_start(&run.svrl);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_schematron_svrl_finish(&run.svrl);
        }
    }
    sxml_schematron_svrl_release(&run.svrl);
    sxml_xml_buffer_release(&run.texts.buffer);
    sxml_sequence_release(raw_result);
    if (status == SXML_STATUS_OK) {
        *outcome = counts;
    }
    return status;
}
