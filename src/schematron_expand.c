/*
 * Placeholders (src/schematron_internal.h, "Placeholders"; D315
 * "Abstract patterns, schema parameters and placeholders"): one
 * left-to-right scan of an expression's text that replaces each "$name"
 * whose whole name token is a parameter in scope by the parameter's
 * value, recording where each run of the result was written (the
 * expansion segments), so that an error inside a value points into the
 * sch:param that gave it.
 *
 * Annexes C, H and J of ISO/IEC 19757-3:2025 give the convention: "$"
 * followed by the parameter name token; a "$" not followed by the name of
 * a parameter in scope is not a delimiter. 6.2 replaces parameters in
 * every attribute that holds a query. Golden file: src/xslt_value_template.c
 * (a scan of attribute text into parts, with the offset of each).
 */
#include "schematron_internal.h"

#include "memory_internal.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <string.h>

/* The length of the NCName run at text[offset...] (XML 1.0 Namespaces,
   NCName: NameStartChar minus ':', then NameChar minus ':'). */
static size_t name_token_length(sxml_byte_view text, size_t offset) {
    size_t end = offset;
    while (end < text.byte_count) {
        const sxml_xml_decoded_codepoint decoded = sxml_xml_decode_codepoint(text, end);
        if (decoded.encoded_byte_count == 0 ||
            decoded.encoded_byte_count > text.byte_count - end) {
            break;
        }
        const bool is_allowed =
            decoded.codepoint != ':' && (end == offset
                                             ? sxml_xml_codepoint_is_name_start(decoded.codepoint)
                                             : sxml_xml_codepoint_is_name(decoded.codepoint));
        if (!is_allowed) {
            break;
        }
        end += decoded.encoded_byte_count;
    }
    return end - offset;
}

/* The parameter of a table named name; NULL when none. */
static const sxml_schematron_parameter_value *find_parameter(sxml_schematron_parameter_table table,
                                                            sxml_byte_view name) {
    for (size_t value_index = 0; value_index < table.value_count; value_index += 1) {
        if (sxml_byte_view_equals(table.values[value_index].name, name)) {
            return &table.values[value_index];
        }
    }
    return NULL;
}

static sxml_status add_segment(sxml_schematron_expansion *expansion,
                               sxml_schematron_segment segment) {
    if (expansion->segment_count > 0) {
        const sxml_schematron_segment *last = &expansion->segments[expansion->segment_count - 1U];
        if (last->expanded_offset == segment.expanded_offset) {
            /* An empty run before: replaced. */
            expansion->segments[expansion->segment_count - 1U] = segment;
            return SXML_STATUS_OK;
        }
    }
    if (expansion->segment_count == expansion->segment_capacity) {
        const size_t new_capacity =
            expansion->segment_capacity < 8 ? 8 : expansion->segment_capacity * 2U;
        void *memory = NULL;
        const sxml_status status = sxml_allocator_allocate_array(
            expansion->allocator,
            (sxml_array_allocation_request){.element_count = new_capacity,
                                            .element_size = sizeof(sxml_schematron_segment),
                                            .element_alignment = alignof(sxml_schematron_segment)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (expansion->segment_count > 0) {
            memcpy(memory, expansion->segments,
                   expansion->segment_count * sizeof(sxml_schematron_segment));
        }
        sxml_allocator_release(expansion->allocator, expansion->segments);
        expansion->segments = memory;
        expansion->segment_capacity = new_capacity;
    }
    expansion->segments[expansion->segment_count] = segment;
    expansion->segment_count += 1;
    return SXML_STATUS_OK;
}

/* One text to scan: the attribute's (parameter NULL) or a value's. */
typedef struct scan_source {
    sxml_byte_view text;
    const sxml_schematron_parameter_value *parameter;
    /* The tables in force: an instance parameter's value is scanned for
       schema parameters only. */
    sxml_schematron_parameter_table instance_parameters;
    sxml_schematron_parameter_table schema_parameters;
    sxml_schematron_parameter_table renamed_variables;
} scan_source;

/* Copies source into the expansion; a placeholder's value is copied, or,
   for an instance parameter, scanned again for schema parameters (no
   deeper: values of schema parameters are not scanned). The nesting is
   at most two, so this is a loop over two levels, not a recursion. */
static sxml_status scan(sxml_schematron_expansion *expansion, const scan_source *source) {
    scan_source levels[2] = {*source, *source};
    size_t positions[2] = {0, 0};
    size_t depth = 1;
    sxml_status status = add_segment(
        expansion, (sxml_schematron_segment){.expanded_offset = (uint32_t)expansion->text.byte_count,
                                             .parameter = source->parameter,
                                             .source_offset = 0});
    while (status == SXML_STATUS_OK && depth > 0) {
        const scan_source *level = &levels[depth - 1U];
        size_t *position = &positions[depth - 1U];
        const sxml_byte_view text = level->text;
        if (*position >= text.byte_count) {
            depth -= 1;
            if (depth > 0) {
                /* Back in the outer text, after the placeholder. */
                status = add_segment(
                    expansion,
                    (sxml_schematron_segment){.expanded_offset = (uint32_t)expansion->text.byte_count,
                                              .parameter = levels[depth - 1U].parameter,
                                              .source_offset = (uint32_t)positions[depth - 1U]});
            }
            continue;
        }
        size_t run_end = *position;
        while (run_end < text.byte_count && text.bytes[run_end] != '$') {
            run_end += 1;
        }
        status = sxml_xml_buffer_append(
            &expansion->text, (sxml_byte_view){.bytes = text.bytes + *position,
                                               .byte_count = run_end - *position});
        *position = run_end;
        if (status != SXML_STATUS_OK || run_end >= text.byte_count) {
            continue;
        }
        const size_t token_length = name_token_length(text, run_end + 1U);
        const sxml_byte_view name = {.bytes = text.bytes + run_end + 1U, .byte_count = token_length};
        const sxml_schematron_parameter_value *instance =
            token_length > 0 ? find_parameter(level->instance_parameters, name) : NULL;
        const sxml_schematron_parameter_value *schema =
            token_length > 0 && instance == NULL ? find_parameter(level->schema_parameters, name)
                                                 : NULL;
        const sxml_schematron_parameter_value *renamed =
            token_length > 0 && instance == NULL && schema == NULL
                ? find_parameter(level->renamed_variables, name)
                : NULL;
        if (instance == NULL && schema == NULL && renamed == NULL) {
            /* Not a placeholder: "$" stays, a variable reference or a
               literal character. */
            status = sxml_xml_buffer_append_byte(&expansion->text, '$');
            *position = run_end + 1U;
            continue;
        }
        expansion->has_placeholder = true;
        *position = run_end + 1U + token_length;
        const sxml_schematron_parameter_value *parameter =
            instance != NULL ? instance : (schema != NULL ? schema : renamed);
        status = add_segment(expansion,
                             (sxml_schematron_segment){
                                 .expanded_offset = (uint32_t)expansion->text.byte_count,
                                 .parameter = parameter,
                                 .source_offset = 0});
        if (status != SXML_STATUS_OK) {
            continue;
        }
        if (instance != NULL && depth == 1) {
            /* An instance parameter's value is scanned once for schema
               parameters, where it is written (D315). */
            levels[1] = (scan_source){.text = instance->value,
                                      .parameter = instance,
                                      .instance_parameters = {.values = NULL, .value_count = 0},
                                      .schema_parameters = level->schema_parameters,
                                      .renamed_variables = level->renamed_variables};
            positions[1] = 0;
            depth = 2;
            continue;
        }
        status = sxml_xml_buffer_append(&expansion->text, parameter->value);
        if (status == SXML_STATUS_OK) {
            status = add_segment(
                expansion,
                (sxml_schematron_segment){.expanded_offset = (uint32_t)expansion->text.byte_count,
                                          .parameter = level->parameter,
                                          .source_offset = (uint32_t)*position});
        }
    }
    return status;
}

sxml_status sxml_schematron_expand(sxml_allocator allocator,
                                   const sxml_schematron_expansion_request *request,
                                   sxml_schematron_expansion *expansion) {
    *expansion = (sxml_schematron_expansion){.text = sxml_xml_buffer_empty(allocator),
                                             .segments = NULL,
                                             .segment_count = 0,
                                             .segment_capacity = 0,
                                             .has_placeholder = false,
                                             .allocator = allocator};
    const sxml_status status =
        scan(expansion, &(scan_source){.text = request->text,
                                       .parameter = NULL,
                                       .instance_parameters = request->instance_parameters,
                                       .schema_parameters = request->schema_parameters,
                                       .renamed_variables = request->renamed_variables});
    if (status != SXML_STATUS_OK) {
        sxml_schematron_expansion_release(expansion);
    }
    return status;
}

void sxml_schematron_expansion_release(sxml_schematron_expansion *expansion) {
    sxml_xml_buffer_release(&expansion->text);
    sxml_allocator_release(expansion->allocator, expansion->segments);
    expansion->segments = NULL;
    expansion->segment_count = 0;
    expansion->segment_capacity = 0;
}

sxml_schematron_expansion_origin sxml_schematron_expansion_origin_of(
    const sxml_schematron_expansion *expansion, uint32_t expanded_offset) {
    sxml_schematron_expansion_origin origin = {.parameter = NULL, .source_offset = expanded_offset};
    for (size_t segment_index = 0; segment_index < expansion->segment_count; segment_index += 1) {
        const sxml_schematron_segment *segment = &expansion->segments[segment_index];
        if (segment->expanded_offset > expanded_offset) {
            break;
        }
        origin = (sxml_schematron_expansion_origin){
            .parameter = segment->parameter,
            .source_offset = segment->source_offset + (expanded_offset - segment->expanded_offset)};
    }
    return origin;
}
