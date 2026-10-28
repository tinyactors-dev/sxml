/*
 * SVRL, the Schematron Validation Report Language (ISO/IEC 19757-3:2025,
 * Annex D, informative; D313, D315 "SVRL"): one document per validation,
 * written from its schematron reports and the compiled schema's static
 * table as the validation goes. Choices (D315): markup of messages reduced
 * to its text, no sch:p as svrl:text, no line or column, no svrl:error (a
 * dynamic error stops the validation).
 *
 * Golden file: src/serialize_xml.c (escaping of text and attribute values
 * as XML 1.0 requires, to an output stream).
 */
#include "schematron_internal.h"

#include "memory_internal.h"

#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view no_text = {.bytes = NULL, .byte_count = 0};

/* Appends text escaped for an attribute value (is_attribute) or element
   content (XML 1.0, 2.4, 3.3.3). */
typedef struct escaped_text {
    sxml_byte_view text;
    bool is_attribute;
} escaped_text;

static sxml_status append_escaped(sxml_xml_buffer *buffer, escaped_text request) {
    sxml_status status = SXML_STATUS_OK;
    size_t run_start = 0;
    for (size_t byte_index = 0; status == SXML_STATUS_OK && byte_index < request.text.byte_count;
         byte_index += 1) {
        const uint8_t byte = request.text.bytes[byte_index];
        const char *replacement = NULL;
        switch (byte) {
        case '&':
            replacement = "&amp;";
            break;
        case '<':
            replacement = "&lt;";
            break;
        case '>':
            replacement = request.is_attribute ? NULL : "&gt;";
            break;
        case '"':
            replacement = request.is_attribute ? "&quot;" : NULL;
            break;
        case '\t':
            replacement = request.is_attribute ? "&#9;" : NULL;
            break;
        case '\n':
            replacement = request.is_attribute ? "&#10;" : NULL;
            break;
        case '\r':
            replacement = "&#13;";
            break;
        default:
            break;
        }
        if (replacement == NULL) {
            continue;
        }
        status = sxml_xml_buffer_append(buffer, (sxml_byte_view){.bytes = request.text.bytes + run_start,
                                                                 .byte_count = byte_index - run_start});
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(buffer, sxml_byte_view_from_c_string(replacement));
        }
        run_start = byte_index + 1U;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append(buffer, (sxml_byte_view){.bytes = request.text.bytes + run_start,
                                                                 .byte_count = request.text.byte_count - run_start});
    }
    return status;
}

/* Appends ' name="value"' when value is not empty (or always). */
typedef struct svrl_attribute {
    const char *name;
    sxml_byte_view value;
    bool is_written_when_empty;
} svrl_attribute;

static sxml_status append_attribute(sxml_xml_buffer *buffer, svrl_attribute attribute) {
    if (attribute.value.byte_count == 0 && !attribute.is_written_when_empty) {
        return SXML_STATUS_OK;
    }
    sxml_status status = sxml_xml_buffer_append_byte(buffer, ' ');
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append(buffer, sxml_byte_view_from_c_string(attribute.name));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append(buffer, SXML_TEXT("=\""));
    }
    if (status == SXML_STATUS_OK) {
        status = append_escaped(buffer, (escaped_text){.text = attribute.value, .is_attribute = true});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append_byte(buffer, '"');
    }
    return status;
}

/* Appends a C string (a literal, or a choice of literals). */
#define SXML_LITERAL(buffer, text) sxml_xml_buffer_append((buffer), sxml_byte_view_from_c_string(text))

/* Writes what the buffer holds to the stream and empties it. */
static sxml_status flush(sxml_schematron_svrl_writer *writer) {
    if (writer->status != SXML_STATUS_OK) {
        return writer->status;
    }
    if (writer->buffer.byte_count > 0) {
        writer->status = writer->output_stream.operations->write(writer->output_stream.state,
                                                                 sxml_schematron_buffer_text(&writer->buffer));
    }
    writer->buffer.byte_count = 0;
    return writer->status;
}

sxml_status sxml_schematron_svrl_start(sxml_schematron_svrl_writer *writer) {
    const sxml_schematron_table *table = writer->table;
    sxml_xml_buffer *buffer = &writer->buffer;
    sxml_status status = SXML_LITERAL(
        buffer, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<svrl:schematron-output xmlns:svrl=\"http://purl.oclc.org/dsdl/svrl\"");
    if (status == SXML_STATUS_OK) {
        status = append_attribute(buffer, (svrl_attribute){.name = "title",
                                                           .value = sxml_schematron_table_text(table, table->title),
                                                           .is_written_when_empty = false});
    }
    if (status == SXML_STATUS_OK) {
        status = append_attribute(buffer, (svrl_attribute){.name = "phase",
                                                           .value = writer->phase_id,
                                                           .is_written_when_empty = false});
    }
    if (status == SXML_STATUS_OK) {
        status = append_attribute(buffer, (svrl_attribute){.name = "schemaVersion",
                                                           .value = sxml_schematron_table_text(
                                                               table, table->schema_version),
                                                           .is_written_when_empty = false});
    }
    if (status == SXML_STATUS_OK && table->has_schematron_edition) {
        /* 5.5.15: reported at user option. */
        status = append_attribute(buffer, (svrl_attribute){.name = "schematronEdition",
                                                           .value = sxml_schematron_table_text(
                                                               table, table->schematron_edition),
                                                           .is_written_when_empty = true});
    }
    if (status == SXML_STATUS_OK) {
        status = SXML_LITERAL(buffer, ">\n");
    }
    for (size_t namespace_index = 0; status == SXML_STATUS_OK && namespace_index < table->namespace_count;
         namespace_index += 1) {
        status = SXML_LITERAL(buffer, "  <svrl:ns-prefix-in-attribute-values");
        if (status == SXML_STATUS_OK) {
            status = append_attribute(
                buffer, (svrl_attribute){.name = "uri",
                                         .value = sxml_schematron_table_text(
                                             table, table->namespace_texts[namespace_index * 2U + 1U]),
                                         .is_written_when_empty = true});
        }
        if (status == SXML_STATUS_OK) {
            status = append_attribute(
                buffer, (svrl_attribute){.name = "prefix",
                                         .value = sxml_schematron_table_text(
                                             table, table->namespace_texts[namespace_index * 2U]),
                                         .is_written_when_empty = true});
        }
        if (status == SXML_STATUS_OK) {
            status = SXML_LITERAL(buffer, "/>\n");
        }
    }
    writer->open_pattern = 0;
    if (status != SXML_STATUS_OK) {
        writer->status = status;
        return status;
    }
    return flush(writer);
}

/* The number of active patterns and the table index of the n-th. */
static size_t active_count(const sxml_schematron_svrl_writer *writer) {
    return writer->has_phase_patterns ? writer->phase_patterns.reference_count
                                      : writer->table->pattern_count;
}

static size_t active_pattern(const sxml_schematron_svrl_writer *writer, size_t position) {
    return writer->has_phase_patterns
               ? writer->table->references[writer->phase_patterns.first_reference + position]
               : position;
}

/* Writes the active-pattern (or active-group) elements of the active
   patterns before position limit not written yet. */
static sxml_status open_patterns(sxml_schematron_svrl_writer *writer, size_t limit) {
    const sxml_schematron_table *table = writer->table;
    sxml_xml_buffer *buffer = &writer->buffer;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && writer->open_pattern < limit) {
        const sxml_schematron_pattern_entry *pattern =
            &table->patterns[active_pattern(writer, writer->open_pattern)];
        status = SXML_LITERAL(buffer, pattern->is_group ? "  <svrl:active-group" : "  <svrl:active-pattern");
        if (status == SXML_STATUS_OK) {
            status = append_attribute(buffer, (svrl_attribute){.name = "documents",
                                                               .value = sxml_schematron_table_text(
                                                                   table, pattern->documents),
                                                               .is_written_when_empty = false});
        }
        if (status == SXML_STATUS_OK) {
            status = append_attribute(buffer, (svrl_attribute){.name = "id",
                                                               .value = sxml_schematron_table_text(
                                                                   table, pattern->identifier),
                                                               .is_written_when_empty = false});
        }
        if (status == SXML_STATUS_OK) {
            status = append_attribute(buffer, (svrl_attribute){.name = "name",
                                                               .value = sxml_schematron_table_text(
                                                                   table, pattern->name),
                                                               .is_written_when_empty = false});
        }
        if (status == SXML_STATUS_OK) {
            status = append_attribute(buffer, (svrl_attribute){.name = "role",
                                                               .value = sxml_schematron_table_text(
                                                                   table, pattern->role),
                                                               .is_written_when_empty = false});
        }
        if (status == SXML_STATUS_OK) {
            status = SXML_LITERAL(buffer, "/>\n");
        }
        writer->open_pattern += 1;
    }
    return status;
}

/* An svrl:text element (human-text, D.3) with its rich attributes. */
typedef struct human_text {
    sxml_byte_view text;
    sxml_byte_view language;
    sxml_byte_view see;
    sxml_byte_view icon;
    sxml_byte_view formal_public_identifier;
    const char *indent;
} human_text;

static sxml_status append_human_text(sxml_xml_buffer *buffer, human_text request) {
    sxml_status status = SXML_LITERAL(buffer, request.indent);
    if (status == SXML_STATUS_OK) {
        status = SXML_LITERAL(buffer, "<svrl:text");
    }
    if (status == SXML_STATUS_OK) {
        status = append_attribute(buffer, (svrl_attribute){.name = "xml:lang",
                                                           .value = request.language,
                                                           .is_written_when_empty = false});
    }
    if (status == SXML_STATUS_OK) {
        status = append_attribute(buffer, (svrl_attribute){.name = "see",
                                                           .value = request.see,
                                                           .is_written_when_empty = false});
    }
    if (status == SXML_STATUS_OK) {
        status = append_attribute(buffer, (svrl_attribute){.name = "icon",
                                                           .value = request.icon,
                                                           .is_written_when_empty = false});
    }
    if (status == SXML_STATUS_OK) {
        status = append_attribute(buffer, (svrl_attribute){.name = "fpi",
                                                           .value = request.formal_public_identifier,
                                                           .is_written_when_empty = false});
    }
    if (status == SXML_STATUS_OK) {
        status = SXML_LITERAL(buffer, ">");
    }
    if (status == SXML_STATUS_OK) {
        status = append_escaped(buffer, (escaped_text){.text = request.text, .is_attribute = false});
    }
    if (status == SXML_STATUS_OK) {
        status = SXML_LITERAL(buffer, "</svrl:text>\n");
    }
    return status;
}

sxml_status sxml_schematron_svrl_write(sxml_schematron_svrl_writer *writer,
                                       const sxml_schematron_svrl_report *request) {
    const sxml_schematron_report *report = request->report;
    sxml_xml_buffer *buffer = &writer->buffer;
    /* The active patterns up to this report's (D.2: grouped by pattern). */
    size_t position = active_count(writer);
    for (size_t candidate = 0; candidate < active_count(writer); candidate += 1) {
        if (active_pattern(writer, candidate) == report->pattern_index) {
            position = candidate;
        }
    }
    sxml_status status = open_patterns(writer, position + 1U > active_count(writer) ? active_count(writer)
                                                                                     : position + 1U);
    if (status == SXML_STATUS_OK && report->report_kind == SXML_SCHEMATRON_REPORT_KIND_FIRED_RULE) {
        const sxml_schematron_table *table = writer->table;
        status = SXML_LITERAL(buffer, "  <svrl:fired-rule");
        const svrl_attribute attributes[] = {
            {"id", report->id, false},
            {"name", sxml_schematron_table_text(table, request->entry->test), false},
            {"context", report->context_text, true},
            {"visit-each", sxml_schematron_table_text(table, request->entry->visit_each), false},
            {"role", report->role, false},
            {"flag", report->flag, false},
            {"document", request->document_uri, false}};
        for (size_t attribute_index = 0;
             status == SXML_STATUS_OK && attribute_index < sizeof(attributes) / sizeof(attributes[0]);
             attribute_index += 1) {
            status = append_attribute(buffer, attributes[attribute_index]);
        }
        if (status == SXML_STATUS_OK) {
            status = SXML_LITERAL(buffer, "/>\n");
        }
        if (status != SXML_STATUS_OK) {
            writer->status = status;
            return status;
        }
        return flush(writer);
    }
    const bool is_report = report->report_kind == SXML_SCHEMATRON_REPORT_KIND_SUCCESSFUL_REPORT;
    if (status == SXML_STATUS_OK) {
        status = SXML_LITERAL(buffer, is_report ? "  <svrl:successful-report" : "  <svrl:failed-assert");
    }
    const svrl_attribute attributes[] = {{"id", report->id, false},
                                         {"location", report->node_path, true},
                                         {"test", report->test_text, true},
                                         {"role", report->role, false},
                                         {"flag", report->flag, false},
                                         {"severity", report->severity, false}};
    for (size_t attribute_index = 0;
         status == SXML_STATUS_OK && attribute_index < sizeof(attributes) / sizeof(attributes[0]);
         attribute_index += 1) {
        status = append_attribute(buffer, attributes[attribute_index]);
    }
    if (status == SXML_STATUS_OK) {
        status = SXML_LITERAL(buffer, ">\n");
    }
    for (size_t diagnostic_index = 0;
         status == SXML_STATUS_OK && diagnostic_index < report->diagnostics.diagnostic_count;
         diagnostic_index += 1) {
        const sxml_schematron_diagnostic *diagnostic = &report->diagnostics.diagnostics[diagnostic_index];
        const sxml_schematron_table *table = writer->table;
        sxml_byte_view role = no_text;
        for (size_t entry_index = 0; entry_index < table->diagnostic_count; entry_index += 1) {
            if (sxml_byte_view_equals(sxml_schematron_table_text(table, table->diagnostics[entry_index].identifier),
                                      diagnostic->diagnostic_id)) {
                role = sxml_schematron_table_text(table, table->diagnostics[entry_index].role);
            }
        }
        status = SXML_LITERAL(buffer, "    <svrl:diagnostic-reference");
        if (status == SXML_STATUS_OK) {
            status = append_attribute(buffer, (svrl_attribute){.name = "diagnostic",
                                                               .value = diagnostic->diagnostic_id,
                                                               .is_written_when_empty = true});
        }
        if (status == SXML_STATUS_OK) {
            status = append_attribute(buffer, (svrl_attribute){.name = "role",
                                                               .value = role,
                                                               .is_written_when_empty = false});
        }
        if (status == SXML_STATUS_OK) {
            status = SXML_LITERAL(buffer, ">\n");
        }
        if (status == SXML_STATUS_OK) {
            status = append_human_text(buffer, (human_text){.text = diagnostic->text,
                                                            .language = diagnostic->language,
                                                            .see = no_text,
                                                            .icon = no_text,
                                                            .formal_public_identifier = no_text,
                                                            .indent = "      "});
        }
        if (status == SXML_STATUS_OK) {
            status = SXML_LITERAL(buffer, "    </svrl:diagnostic-reference>\n");
        }
    }
    for (size_t property_index = 0;
         status == SXML_STATUS_OK && property_index < report->properties.property_count;
         property_index += 1) {
        const sxml_schematron_property *property = &report->properties.properties[property_index];
        status = SXML_LITERAL(buffer, "    <svrl:property-reference");
        const svrl_attribute property_attributes[] = {{"property", property->property_id, true},
                                                      {"role", property->role, false},
                                                      {"scheme", property->scheme, false}};
        for (size_t attribute_index = 0; status == SXML_STATUS_OK && attribute_index < 3;
             attribute_index += 1) {
            status = append_attribute(buffer, property_attributes[attribute_index]);
        }
        if (status == SXML_STATUS_OK) {
            status = SXML_LITERAL(buffer, ">\n");
        }
        if (status == SXML_STATUS_OK) {
            status = append_human_text(buffer, (human_text){.text = property->text,
                                                            .language = no_text,
                                                            .see = no_text,
                                                            .icon = no_text,
                                                            .formal_public_identifier = no_text,
                                                            .indent = "      "});
        }
        if (status == SXML_STATUS_OK) {
            status = SXML_LITERAL(buffer, "    </svrl:property-reference>\n");
        }
    }
    if (status == SXML_STATUS_OK) {
        status = append_human_text(buffer, (human_text){.text = report->message,
                                                        .language = report->language,
                                                        .see = report->see,
                                                        .icon = report->icon,
                                                        .formal_public_identifier =
                                                            report->formal_public_identifier,
                                                        .indent = "    "});
    }
    if (status == SXML_STATUS_OK) {
        status = SXML_LITERAL(buffer, is_report ? "  </svrl:successful-report>\n"
                                                  : "  </svrl:failed-assert>\n");
    }
    if (status != SXML_STATUS_OK) {
        writer->status = status;
        return status;
    }
    return flush(writer);
}

sxml_status sxml_schematron_svrl_finish(sxml_schematron_svrl_writer *writer) {
    sxml_status status = open_patterns(writer, active_count(writer));
    if (status == SXML_STATUS_OK) {
        status = SXML_LITERAL(&writer->buffer, "</svrl:schematron-output>\n");
    }
    if (status != SXML_STATUS_OK) {
        writer->status = status;
        return status;
    }
    return flush(writer);
}

void sxml_schematron_svrl_release(sxml_schematron_svrl_writer *writer) {
    sxml_xml_buffer_release(&writer->buffer);
}
