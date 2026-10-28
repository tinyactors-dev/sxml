/*
 * Building the stylesheet AST of a Schematron schema
 * (src/schematron_internal.h, "Building"; D315 "The path", steps 2 and 3,
 * "The stylesheet the builder makes", "The mapping, construct by
 * construct", "Variables"). One walk per compiled schematron pattern emits
 * the nodes of its mode; every expression is parsed alone, after its
 * placeholders are replaced, so a syntax error in one is reported and the
 * walk goes on. No text is ever spliced: synthesized expressions are fixed
 * texts that name synthesized variables in the namespace
 * urn:sxml:schematron by EQName.
 *
 * Golden file: src/xslt_read.c (the AST functions of
 * src/xslt_ast_internal.h called as the stylesheet reader calls them,
 * expressions parsed by the front ends and handed over with their
 * spans). Clause numbers are of ISO/IEC 19757-3:2025.
 */
#include "schematron_internal.h"

#include "memory_internal.h"

#include "processor_internal.h"
#include "xpath_ast_internal.h"
#include "xslt_read_internal.h"
#include "xslt_value_template_internal.h"

#include <stdalign.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)
#define SXML_PRIVATE(local) "Q{" SXML_SCHEMATRON_NAMESPACE_URI "}" local

static const sxml_byte_view no_text = {.bytes = NULL, .byte_count = 0};
static const sxml_ast_source_span no_span = {.byte_offset = 0,
                                             .byte_count = 0,
                                             .line_number = 0,
                                             .column_number = 0};

/* The element value, pushed by copy; aligned for any type, as alignof of
   an expression is not standard C. */
#define SXML_SCHEMATRON_ELEMENT_OF(value)                                                     \
    ((sxml_schematron_array_element){                                                         \
        .element = &(value), .element_size = sizeof(value),                                   \
        .element_alignment = alignof(max_align_t)})

/* ------------------------------------------------------------- Builder */

/* A variable in scope of the rule being built, for 5.4.7's one
   definition. */
typedef struct scoped_variable {
    sxml_byte_view name;
    sxml_schematron_place place;
} scoped_variable;

/* A pattern let compiled as a global: its written name, and the global's
   name when two compiled patterns define the name (3.23). */
typedef struct pattern_variable {
    uint32_t pattern;
    sxml_name_identifier written_name;
    sxml_name_identifier global_name;
} pattern_variable;

/* One schematron pattern to compile: the element, its abstract pattern
   for an instance (else the element itself), and its number among the
   compiled patterns (from 1); 0 when only checked. */
typedef struct compiled_pattern {
    sxml_schematron_place place;
    sxml_schematron_place rules_place;
    uint32_t number;
    bool is_instance;
    bool is_group;
    bool has_documents;
} compiled_pattern;

typedef struct schematron_builder {
    sxml_schematron_compilation *compilation;
    sxml_xslt_ast *ast;
    sxml_xslt_ast_node_index root;
    sxml_xslt_ast_static_context_index root_context;
    /* sxml_schematron_parameter_value: schema parameters, and the
       instance's while one is built. */
    sxml_schematron_array schema_parameters;
    sxml_schematron_array instance_parameters;
    bool has_instance;
    /* compiled_pattern, in schema order. */
    sxml_schematron_array patterns;
    /* sxml_schematron_identified: the phases "#ANY" can choose (those with
       when), or the one phase compiled. */
    sxml_schematron_array chosen_phases;
    bool selects_phase;
    bool is_all;
    /* scoped_variable: schema lets and parameters, the phase's lets. */
    sxml_schematron_array global_variables;
    /* scoped_variable: the pattern's lets, then the rule's. */
    sxml_schematron_array local_variables;
    /* pattern_variable. */
    sxml_schematron_array pattern_variables;
    /* sxml_schematron_parameter_value: while a pattern is built, its
       renamed variables, each "$name" replaced by a reference to its
       global, as a placeholder is (3.23, D323); the values are in
       renamed_text. */
    sxml_schematron_array renamed_variables;
    sxml_xml_buffer renamed_text;
    /* The global holding the start nodes of expression rule contexts
       (made when the first is met). */
    bool has_start;
    uint32_t context_count;
    /* Names. */
    sxml_name_identifier validate_name;
    sxml_name_identifier fired_rules_name;
    sxml_name_identifier phase_name;
    sxml_name_identifier start_name;
    sxml_name_identifier context_name;
    sxml_name_identifier subject_name;
    sxml_name_identifier value_name;
    sxml_name_identifier base_name;
    sxml_name_identifier root_name;
    /* The cursor of the walks. */
    sxml_schematron_cursor cursor;
    /* Scratch text for synthesized names and expressions. */
    sxml_xml_buffer scratch;
    /* The error of the last parse. */
    sxml_error_record parse_record;
} schematron_builder;


static sxml_schematron_document *document_of(const schematron_builder *builder, uint32_t document) {
    return sxml_schematron_document_at(builder->compilation, document);
}

static sxml_schematron_attribute attribute_of(const schematron_builder *builder, sxml_schematron_place place,
                                              sxml_schematron_attribute_kind kind) {
    return sxml_schematron_find_attribute(
        builder->compilation, (sxml_schematron_attribute_query){.place = place, .attribute_kind = kind});
}

static sxml_byte_view trimmed_of(const schematron_builder *builder, sxml_schematron_place place,
                                 sxml_schematron_attribute_kind kind) {
    return sxml_xml_text_trim_whitespace(attribute_of(builder, place, kind).value);
}

static sxml_status intern_private_name(schematron_builder *builder, sxml_byte_view local_name,
                                       sxml_name_identifier *name) {
    return sxml_name_pool_intern_name(
        builder->compilation->name_pool,
        &(sxml_name_request){.namespace_uri = builder->compilation->names.private_namespace,
                             .local_name = local_name},
        name);
}

/* A private name made of a stem and a number: "pattern-3". */
typedef struct numbered_name {
    const char *stem;
    uint32_t number;
    /* Appended after the number with a "-": a variable's local name. */
    sxml_byte_view suffix;
} numbered_name;

static sxml_status intern_numbered_name(schematron_builder *builder, numbered_name request,
                                        sxml_name_identifier *name) {
    char digits[32];
    const int written = snprintf(digits, sizeof(digits), "%s-%u", request.stem, request.number);
    if (written < 0 || (size_t)written >= sizeof(digits)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    builder->scratch.byte_count = 0;
    sxml_status status = sxml_xml_buffer_append(
        &builder->scratch, (sxml_byte_view){.bytes = (const uint8_t *)digits,
                                            .byte_count = (size_t)written});
    if (status == SXML_STATUS_OK && request.suffix.byte_count > 0) {
        status = sxml_xml_buffer_append_byte(&builder->scratch, '-');
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(&builder->scratch, request.suffix);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return intern_private_name(builder, sxml_schematron_buffer_text(&builder->scratch), name);
}

/* The EQName text of a name, for a fixed expression: "Q{uri}local". */
static sxml_status append_eqname(schematron_builder *builder, sxml_xml_buffer *buffer,
                                 sxml_name_identifier name) {
    sxml_name_parts parts = {.namespace_uri = {.index = 0}, .local_name = no_text};
    sxml_status status = sxml_name_pool_get_name_parts(builder->compilation->name_pool, name, &parts);
    sxml_byte_view uri = no_text;
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_get_namespace_uri(builder->compilation->name_pool,
                                                  parts.namespace_uri, &uri);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append(buffer, SXML_TEXT("Q{"));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append(buffer, uri);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append_byte(buffer, '}');
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append(buffer, parts.local_name);
    }
    return status;
}

/* --------------------------------------------------------------- Table */

static sxml_status add_table_text(schematron_builder *builder, sxml_byte_view bytes,
                                  sxml_schematron_text *text) {
    sxml_schematron_table *table = &builder->compilation->table;
    *text = (sxml_schematron_text){.byte_offset = 0, .byte_count = 0};
    if (bytes.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    if (bytes.byte_count > UINT32_MAX || table->text_byte_count > UINT32_MAX - bytes.byte_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (table->text_byte_count + bytes.byte_count > table->text_byte_capacity) {
        size_t new_capacity = table->text_byte_capacity < 4096 ? 4096 : table->text_byte_capacity;
        while (new_capacity < table->text_byte_count + bytes.byte_count) {
            new_capacity *= 2U;
        }
        void *memory = NULL;
        const sxml_status status = sxml_allocator_allocate(
            builder->compilation->allocator,
            (sxml_allocation_request){.byte_count = new_capacity, .alignment = 1}, &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (table->text_byte_count > 0) {
            memcpy(memory, table->text_bytes, table->text_byte_count);
        }
        sxml_allocator_release(builder->compilation->allocator, table->text_bytes);
        table->text_bytes = memory;
        table->text_byte_capacity = new_capacity;
    }
    memcpy(table->text_bytes + table->text_byte_count, bytes.bytes, bytes.byte_count);
    *text = (sxml_schematron_text){.byte_offset = (uint32_t)table->text_byte_count,
                                   .byte_count = (uint32_t)bytes.byte_count};
    table->text_byte_count += bytes.byte_count;
    return SXML_STATUS_OK;
}

/* Grows one of the table's arrays by one entry. */
typedef struct table_growth {
    void **elements;
    size_t *count;
    size_t element_size;
    size_t element_alignment;
} table_growth;

static sxml_status grow_table(schematron_builder *builder, table_growth growth) {
    const size_t count = *growth.count;
    /* Capacities are 16, then powers of two: room is made when the count
       reaches one. */
    const bool needs_room = count == 0 || (count >= 16 && (count & (count - 1U)) == 0);
    if (!needs_room) {
        *growth.count = count + 1U;
        return SXML_STATUS_OK;
    }
    const size_t new_capacity = count == 0 ? 16 : count * 2U;
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate_array(
        builder->compilation->allocator,
        (sxml_array_allocation_request){.element_count = new_capacity,
                                        .element_size = growth.element_size,
                                        .element_alignment = growth.element_alignment},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (count > 0) {
        memcpy(memory, *growth.elements, count * growth.element_size);
    }
    sxml_allocator_release(builder->compilation->allocator, *growth.elements);
    *growth.elements = memory;
    *growth.count = count + 1U;
    return SXML_STATUS_OK;
}

#define SXML_GROW(grower, array_lvalue, count_lvalue, element_type)                         \
    grow_table((grower), (table_growth){.elements = (void **)&(array_lvalue),                \
                                        .count = &(count_lvalue),                            \
                                        .element_size = sizeof(element_type),                \
                                        .element_alignment = alignof(element_type)})

void sxml_schematron_table_release(sxml_allocator allocator, sxml_schematron_table *table) {
    sxml_allocator_release(allocator, table->entries);
    sxml_allocator_release(allocator, table->patterns);
    sxml_allocator_release(allocator, table->diagnostics);
    sxml_allocator_release(allocator, table->properties);
    sxml_allocator_release(allocator, table->references);
    sxml_allocator_release(allocator, table->phases);
    sxml_allocator_release(allocator, table->namespace_texts);
    sxml_allocator_release(allocator, table->text_bytes);
    memset(table, 0, sizeof(*table));
}

/* ------------------------------------------------------------- Errors */

/* Gives an error a front end wrote into the builder's record to the
   compilation, at document and span instead of its own location. */
typedef struct relocated_error {
    uint32_t document;
    sxml_ast_source_span source_span;
    /* Replaces the record's description when not NULL. */
    const char *description;
    sxml_byte_view subject;
} relocated_error;

static sxml_status report_relocated(schematron_builder *builder, relocated_error error) {
    sxml_schematron_compilation *compilation = builder->compilation;
    const sxml_error_record *record = &builder->parse_record;
    sxml_error_record relocated = *record;
    relocated.diagnostic.location = (sxml_source_location){
        .source_name = error.document < compilation->documents.count
                           ? document_of(builder, error.document)->uri
                           : compilation->configuration->document_uri,
        .line_number = error.source_span.line_number,
        .column_number = error.source_span.column_number,
        .byte_offset = error.source_span.byte_offset};
    if (error.description != NULL) {
        relocated.diagnostic.description = sxml_byte_view_from_c_string(error.description);
    }
    if (relocated.code.local_name.byte_count == 0) {
        relocated.code = (sxml_error_code){.namespace_uri = SXML_TEXT(SXML_ERROR_NAMESPACE_URI),
                                           .local_name = SXML_TEXT("XPST0003"),
                                           .namespace_prefix = SXML_TEXT("err")};
    }
    if (error.subject.byte_count == 0) {
        return sxml_schematron_report_record(compilation, &relocated);
    }
    compilation->description.byte_count = 0;
    sxml_status status =
        sxml_xml_buffer_append(&compilation->description, relocated.diagnostic.description);
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append(&compilation->description, SXML_TEXT(": "));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append(&compilation->description, error.subject);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    relocated.diagnostic.description = sxml_schematron_buffer_text(&compilation->description);
    return sxml_schematron_report_record(compilation, &relocated);
}

/* An error of Schematron's own at an element (its attribute's span when
   given). */
typedef struct builder_error {
    const char *code;
    const char *description;
    sxml_schematron_place place;
    sxml_ast_source_span attribute_span;
    sxml_byte_view subject;
} builder_error;

static sxml_status report_at(schematron_builder *builder, builder_error error) {
    return sxml_schematron_report_error(
        builder->compilation,
        &(sxml_schematron_error){
            .code = error.code,
            .is_xpath = false,
            .description = error.description,
            .subject = error.subject,
            .document = error.place.document,
            .source_span = error.attribute_span.line_number != 0
                               ? error.attribute_span
                               : sxml_schematron_element_span(builder->compilation, error.place)});
}

/* The line and column of offset in text, counted as an XPath front end
   counts them (1-based, columns in characters). */
static sxml_ast_source_span span_in_text(sxml_byte_view text, uint32_t offset) {
    uint32_t line = 1;
    uint32_t column = 1;
    for (uint32_t byte_index = 0; byte_index < offset && byte_index < text.byte_count;
         byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        if (byte == '\n') {
            line += 1;
            column = 1;
        } else if ((byte & 0xC0U) != 0x80U) {
            column += 1;
        }
    }
    return (sxml_ast_source_span){
        .byte_offset = offset, .byte_count = 0, .line_number = line, .column_number = column};
}

/* ------------------------------------------------------- Parameters */

/* The parameter tables an expression is expanded with. */
static sxml_schematron_parameter_table table_of(const sxml_schematron_array *array) {
    return (sxml_schematron_parameter_table){.values = array->elements, .value_count = array->count};
}

/* Fills builder->schema_parameters: each schema parameter's value, the
   configured one when given (5.4.11). */
static sxml_status collect_schema_parameters(schematron_builder *builder) {
    sxml_schematron_compilation *compilation = builder->compilation;
    const sxml_schematron_place *places = compilation->index.schema_parameters.elements;
    const sxml_schematron_parameter_view configured = compilation->configuration->parameters;
    sxml_status status = SXML_STATUS_OK;
    for (size_t place_index = 0;
         status == SXML_STATUS_OK && place_index < compilation->index.schema_parameters.count;
         place_index += 1) {
        const sxml_schematron_place place = places[place_index];
        const sxml_byte_view name = trimmed_of(builder, place, SXML_SCHEMATRON_ATTRIBUTE_NAME);
        const sxml_schematron_attribute value = attribute_of(builder, place, SXML_SCHEMATRON_ATTRIBUTE_VALUE);
        sxml_schematron_parameter_value entry = {.name = name,
                                                 .value = value.value,
                                                 .document = place.document,
                                                 .source_span = value.source_span,
                                                 .is_configured = false};
        for (size_t configured_index = 0; configured_index < configured.parameter_count;
             configured_index += 1) {
            if (sxml_byte_view_equals(configured.parameters[configured_index].name, name)) {
                entry.value = configured.parameters[configured_index].value;
                entry.is_configured = true;
                entry.source_span = no_span;
            }
        }
        status = sxml_schematron_array_push(compilation, &builder->schema_parameters,
                                            SXML_SCHEMATRON_ELEMENT_OF(entry));
    }
    return status;
}

/* Fills builder->instance_parameters for a pattern instance: its own
   parameters, then the abstract pattern's defaults it does not give
   (5.4.11). */
static sxml_status collect_instance_parameters(schematron_builder *builder, const compiled_pattern *pattern) {
    sxml_schematron_compilation *compilation = builder->compilation;
    builder->instance_parameters.count = 0;
    builder->has_instance = pattern->is_instance;
    if (!pattern->is_instance) {
        return SXML_STATUS_OK;
    }
    const sxml_schematron_place sources[] = {pattern->place, pattern->rules_place};
    sxml_status status = SXML_STATUS_OK;
    for (size_t source_index = 0; status == SXML_STATUS_OK && source_index < 2; source_index += 1) {
        sxml_schematron_cursor cursor = {.compilation = compilation,
                                         .frames = {.elements = NULL, .count = 0, .capacity = 0},
                                         .expands_rules = false};
        status = sxml_schematron_cursor_start(&cursor, sources[source_index]);
        for (;;) {
            sxml_schematron_child child = {.has_child = false,
                                           .place = {.document = 0, .element = {.index = 0}},
                                           .element_kind = SXML_SCHEMATRON_ELEMENT_NONE,
                                           .is_inserted = false};
            if (status == SXML_STATUS_OK) {
                status = sxml_schematron_cursor_next(&cursor, &child);
            }
            if (status != SXML_STATUS_OK || !child.has_child) {
                break;
            }
            if (child.element_kind != SXML_SCHEMATRON_ELEMENT_PARAM) {
                continue;
            }
            const sxml_schematron_attribute value =
                attribute_of(builder, child.place, SXML_SCHEMATRON_ATTRIBUTE_VALUE);
            if (!value.is_present) {
                continue;
            }
            const sxml_byte_view name = trimmed_of(builder, child.place, SXML_SCHEMATRON_ATTRIBUTE_NAME);
            bool is_given = false;
            const sxml_schematron_parameter_value *given = builder->instance_parameters.elements;
            for (size_t given_index = 0; given_index < builder->instance_parameters.count;
                 given_index += 1) {
                if (sxml_byte_view_equals(given[given_index].name, name)) {
                    is_given = true;
                }
            }
            if (is_given) {
                continue;
            }
            const sxml_schematron_parameter_value entry = {.name = name,
                                                           .value = value.value,
                                                           .document = child.place.document,
                                                           .source_span = value.source_span,
                                                           .is_configured = false};
            status = sxml_schematron_array_push(compilation, &builder->instance_parameters,
                                                SXML_SCHEMATRON_ELEMENT_OF(entry));
        }
        sxml_schematron_cursor_finish(&cursor);
    }
    return status;
}

/* -------------------------------------------------------- Expressions */

/* An attribute to parse into an expression AST. */
typedef struct expression_request {
    sxml_schematron_place place;
    sxml_schematron_attribute_kind attribute_kind;
    /* EXPRESSION, PATTERN, VALUE_TEMPLATE or SEQUENCE_TYPE. */
    sxml_xslt_ast_value_kind value_kind;
    /* Placeholders are replaced (every attribute that holds a query,
       6.2; not as). */
    bool expands;
    /* Receives the expanded text in the table; may be NULL. */
    sxml_schematron_text *table_text;
} expression_request;

/* A parsed attribute. */
typedef struct parsed_expression {
    bool is_present;
    /* Owned; NULL when absent or in error (the error is reported). */
    sxml_ast *ast;
    sxml_ast_source_span source_span;
    /* The attribute's value after placeholders, trimmed; borrowed until
       the next parse. */
    bool is_dynamic_variable;
} parsed_expression;

/* The source name of a document's expressions: its module's URI, which
   never moves (D102). */
static sxml_byte_view source_name_of(const schematron_builder *builder, uint32_t document) {
    const sxml_xslt_ast_module_index module = document_of(builder, document)->module;
    return sxml_xslt_ast_get_module(builder->ast, module)->module_uri;
}

/* Parses text with the front end of kind; on failure the error is in
   builder->parse_record (location inside text). */
typedef struct front_end_request {
    sxml_xslt_ast_value_kind value_kind;
    sxml_byte_view text;
    sxml_byte_view source_name;
} front_end_request;

static sxml_status parse_with_front_end(schematron_builder *builder, front_end_request request,
                                        sxml_ast **ast) {
    memset(&builder->parse_record, 0, sizeof(builder->parse_record));
    const sxml_ast_build_request build = {
        .expression_text = request.text,
        .source_name = request.source_name,
        .allocator = builder->compilation->allocator,
        .maximum_expression_depth =
            sxml_processor_get_maximum_expression_depth(builder->compilation->processor),
        .error_record = &builder->parse_record,
        .reads_xml_1_1 = false};
    switch ((uint32_t)request.value_kind) {
    case SXML_XSLT_AST_VALUE_KIND_EXPRESSION:
        return sxml_xpath_build_ast(&build, ast);
    case SXML_XSLT_AST_VALUE_KIND_PATTERN:
        return sxml_xslt_pattern_build_ast(&build, ast);
    case SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE:
        return sxml_xslt_build_value_template_ast(&build, ast);
    case SXML_XSLT_AST_VALUE_KIND_SEQUENCE_TYPE:
        return sxml_xpath_build_sequence_type_ast(&build, ast);
    default:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
}

/* Every span of an expression whose text placeholders changed is put at
   the attribute (D315 "Findings and their two locations"). */
static void place_spans_at(sxml_ast *ast, sxml_ast_source_span origin) {
    for (uint32_t node_number = 1; node_number < ast->node_count; node_number += 1) {
        ast->nodes[node_number].source_span =
            (sxml_ast_source_span){.byte_offset = origin.byte_offset,
                                   .byte_count = 0,
                                   .line_number = origin.line_number,
                                   .column_number = origin.column_number};
    }
}

/* Reports a front end's error in expanded text where it was written: in
   the attribute, or in the sch:param value a placeholder brought in
   (expansion segments, D315). */
typedef struct expansion_error {
    const sxml_schematron_expansion *expansion;
    sxml_byte_view attribute_text;
    sxml_schematron_place place;
    sxml_ast_source_span attribute_span;
} expansion_error;

static sxml_status report_expansion_error(schematron_builder *builder, expansion_error error) {
    const sxml_source_location location = builder->parse_record.diagnostic.location;
    const uint32_t offset =
        location.byte_offset > UINT32_MAX ? UINT32_MAX : (uint32_t)location.byte_offset;
    const sxml_schematron_expansion_origin origin =
        sxml_schematron_expansion_origin_of(error.expansion, offset);
    if (location.line_number == 0) {
        return report_relocated(builder, (relocated_error){.document = error.place.document,
                                                           .source_span = error.attribute_span,
                                                           .description = NULL,
                                                           .subject = no_text});
    }
    if (origin.parameter == NULL) {
        const sxml_ast_source_span inside = span_in_text(error.attribute_text, origin.source_offset);
        return report_relocated(
            builder, (relocated_error){.document = error.place.document,
                                       .source_span = sxml_ast_moved_source_span(inside, error.attribute_span),
                                       .description = NULL,
                                       .subject = no_text});
    }
    if (origin.parameter->is_configured) {
        return report_relocated(
            builder,
            (relocated_error){.document = UINT32_MAX,
                              .source_span = no_span,
                              .description = "a syntax error in the configured value of a schema "
                                             "parameter",
                              .subject = origin.parameter->name});
    }
    const sxml_ast_source_span inside = span_in_text(origin.parameter->value, origin.source_offset);
    return report_relocated(
        builder,
        (relocated_error){.document = origin.parameter->document,
                          .source_span = sxml_ast_moved_source_span(inside, origin.parameter->source_span),
                          .description = NULL,
                          .subject = no_text});
}

/* Parses an attribute alone, placeholders replaced: the AST with its
   spans in the schematron document, or an error reported (the walk goes
   on). */
static sxml_status parse_attribute(schematron_builder *builder, const expression_request *request,
                                   parsed_expression *parsed) {
    *parsed = (parsed_expression){.is_present = false,
                                  .ast = NULL,
                                  .source_span = no_span,
                                  .is_dynamic_variable = false};
    const sxml_schematron_attribute attribute =
        attribute_of(builder, request->place, request->attribute_kind);
    if (!attribute.is_present) {
        return SXML_STATUS_OK;
    }
    parsed->is_present = true;
    parsed->source_span = attribute.source_span;
    sxml_schematron_expansion expansion;
    sxml_status status = sxml_schematron_expand(
        builder->compilation->allocator,
        &(sxml_schematron_expansion_request){
            .text = attribute.value,
            .instance_parameters = builder->has_instance && request->expands
                                       ? table_of(&builder->instance_parameters)
                                       : (sxml_schematron_parameter_table){.values = NULL, .value_count = 0},
            .schema_parameters = request->expands
                                     ? table_of(&builder->schema_parameters)
                                     : (sxml_schematron_parameter_table){.values = NULL, .value_count = 0},
            .renamed_variables = request->expands
                                     ? table_of(&builder->renamed_variables)
                                     : (sxml_schematron_parameter_table){.values = NULL, .value_count = 0}},
        &expansion);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_byte_view expanded = sxml_schematron_buffer_text(&expansion.text);
    if (request->table_text != NULL) {
        status = add_table_text(builder, sxml_xml_text_trim_whitespace(expanded), request->table_text);
    }
    if (status == SXML_STATUS_OK) {
        status = parse_with_front_end(
            builder, (front_end_request){.value_kind = request->value_kind,
                                         .text = expanded,
                                         .source_name = source_name_of(builder, request->place.document)},
            &parsed->ast);
        if (status == SXML_STATUS_OK) {
            if (expansion.has_placeholder) {
                place_spans_at(parsed->ast, attribute.source_span);
            } else {
                sxml_ast_move_source_spans(parsed->ast, attribute.source_span);
            }
        } else if (sxml_status_has_error_record(status) && status != SXML_STATUS_LIMIT_EXCEEDED) {
            parsed->ast = NULL;
            status = report_expansion_error(builder, (expansion_error){.expansion = &expansion,
                                                                       .attribute_text = attribute.value,
                                                                       .place = request->place,
                                                                       .attribute_span =
                                                                           attribute.source_span});
        }
    }
    sxml_schematron_expansion_release(&expansion);
    return status;
}

/* Parses a fixed, synthesized text (never in error), its spans at origin
   in document. */
typedef struct fixed_expression {
    sxml_xslt_ast_value_kind value_kind;
    sxml_byte_view text;
    uint32_t document;
    sxml_ast_source_span origin;
} fixed_expression;

static sxml_status parse_fixed(schematron_builder *builder, fixed_expression fixed, sxml_ast **ast) {
    const sxml_status status = parse_with_front_end(
        builder, (front_end_request){.value_kind = fixed.value_kind,
                                     .text = fixed.text,
                                     .source_name = source_name_of(builder, fixed.document)},
        ast);
    if (status == SXML_STATUS_OK) {
        place_spans_at(*ast, fixed.origin);
    }
    return status;
}

/* --------------------------------------------------------------- Nodes */

/* A node to add: synthesized (no place) nodes belong to module 1 and the
   root's static context; others to their element's document. */
typedef struct node_request {
    sxml_xslt_ast_kind kind;
    sxml_xslt_ast_node_index parent;
    sxml_schematron_place place;
    bool has_place;
    bool is_declaration;
} node_request;

static sxml_status add_node(schematron_builder *builder, node_request request,
                            sxml_xslt_ast_node_index *node) {
    const sxml_schematron_document *document =
        request.has_place ? document_of(builder, request.place.document) : document_of(builder, 0);
    sxml_status status = sxml_xslt_ast_add_node(
        builder->ast,
        &(sxml_xslt_ast_node_request){
            .kind = request.kind,
            .flags = request.is_declaration ? (uint32_t)SXML_XSLT_AST_NODE_FLAG_DECLARATION : 0U,
            .module = document->module,
            .source_node = request.has_place ? request.place.element : (sxml_node_index){.index = 0},
            .static_context = document->static_context,
            .element_name = {.index = 0},
            .namespace_prefix = {.index = 0},
            .content = {.kind = SXML_XSLT_AST_VALUE_KIND_NONE},
            .source_span = request.has_place
                               ? sxml_schematron_element_span(builder->compilation, request.place)
                               : no_span},
        node);
    if (status == SXML_STATUS_OK && request.parent.value != 0) {
        status = sxml_xslt_ast_append_child(
            builder->ast, (sxml_xslt_ast_child_request){.parent = request.parent, .child = *node});
    }
    return status;
}

/* A text node child with text content. */
static sxml_status add_text_node(schematron_builder *builder, sxml_xslt_ast_node_index parent,
                                 sxml_byte_view text) {
    sxml_xslt_ast_text stored = {.byte_offset = 0, .byte_count = 0};
    sxml_status status = sxml_xslt_ast_append_text(builder->ast, text, &stored);
    sxml_xslt_ast_node_index node = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_ast_add_node(
            builder->ast,
            &(sxml_xslt_ast_node_request){
                .kind = SXML_XSLT_AST_KIND_TEXT_NODE,
                .flags = 0,
                .module = document_of(builder, 0)->module,
                .source_node = {.index = 0},
                .static_context = builder->root_context,
                .element_name = {.index = 0},
                .namespace_prefix = {.index = 0},
                .content = {.kind = SXML_XSLT_AST_VALUE_KIND_TEXT, .payload = {.text = stored}},
                .source_span = no_span},
            &node);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xslt_ast_append_child(builder->ast,
                                            (sxml_xslt_ast_child_request){.parent = parent, .child = node});
    }
    return status;
}

/* An attribute of the node added last. */
/* An attribute with a value already made. */
typedef struct value_attribute {
    sxml_xslt_ast_node_index node;
    sxml_xslt_ast_attribute_kind kind;
    sxml_xslt_ast_value value;
} value_attribute;

static sxml_status add_value_attribute(schematron_builder *builder, value_attribute request) {
    const sxml_xslt_ast_node_index node = request.node;
    const sxml_xslt_ast_attribute_kind kind = request.kind;
    const sxml_xslt_ast_value value = request.value;
    sxml_xslt_ast_attribute_index attribute = {.value = 0};
    return sxml_xslt_ast_add_attribute(
        builder->ast,
        &(sxml_xslt_ast_attribute_request){.node = node,
                                           .kind = kind,
                                           .flags = 0,
                                           .attribute_name = {.index = 0},
                                           .namespace_prefix = {.index = 0},
                                           .value = value,
                                           .source_span = no_span},
        &attribute);
}

/* An expression attribute: takes ast (owned; destroyed on failure). */
typedef struct expression_attribute {
    sxml_xslt_ast_node_index node;
    sxml_xslt_ast_attribute_kind kind;
    sxml_xslt_ast_value_kind value_kind;
    sxml_ast *ast;
    uint32_t document;
    sxml_ast_source_span source_span;
} expression_attribute;

static sxml_status add_expression_attribute(schematron_builder *builder, expression_attribute attribute) {
    sxml_xslt_ast_expression_index expression = {.value = 0};
    sxml_status status = sxml_xslt_ast_take_expression(
        builder->ast,
        &(sxml_xslt_ast_expression_request){
            .kind = attribute.value_kind == SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE
                        ? SXML_XSLT_AST_VALUE_KIND_EXPRESSION
                        : attribute.value_kind,
            .expression_ast = attribute.ast,
            .static_context = document_of(builder, attribute.document)->static_context,
            .node = attribute.node,
            .source_span = attribute.source_span},
        &expression);
    if (status != SXML_STATUS_OK) {
        sxml_ast_destroy(attribute.ast);
        return status;
    }
    return add_value_attribute(builder, (value_attribute){.node = attribute.node, .kind = attribute.kind, .value = (sxml_xslt_ast_value){.kind = attribute.value_kind == SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE
                                          ? SXML_XSLT_AST_VALUE_KIND_EXPRESSION
                                          : attribute.value_kind,
                              .payload = {.expression = expression}}});
}

/* A fixed expression attribute of the node added last. */
typedef struct fixed_attribute {
    sxml_xslt_ast_node_index node;
    sxml_xslt_ast_attribute_kind kind;
    sxml_xslt_ast_value_kind value_kind;
    sxml_byte_view text;
    uint32_t document;
    sxml_ast_source_span origin;
} fixed_attribute;

static sxml_status add_fixed_attribute(schematron_builder *builder, fixed_attribute attribute) {
    sxml_ast *ast = NULL;
    const sxml_status status = parse_fixed(builder, (fixed_expression){.value_kind = attribute.value_kind,
                                                                       .text = attribute.text,
                                                                       .document = attribute.document,
                                                                       .origin = attribute.origin},
                                           &ast);
    if (status != SXML_STATUS_OK) {
        /* A synthesized text that does not parse is sxml's fault. */
        return status == SXML_STATUS_STATIC_ERROR ? SXML_STATUS_INVALID_ARGUMENT : status;
    }
    return add_expression_attribute(builder, (expression_attribute){.node = attribute.node,
                                                                    .kind = attribute.kind,
                                                                    .value_kind = attribute.value_kind,
                                                                    .ast = ast,
                                                                    .document = attribute.document,
                                                                    .source_span = attribute.origin});
}

/* A name attribute (NAME value). */
static sxml_status add_name_attribute(schematron_builder *builder, sxml_xslt_ast_node_index node,
                                      sxml_name_identifier name) {
    return add_value_attribute(builder, (value_attribute){.node = node, .kind = SXML_XSLT_AST_ATTRIBUTE_NAME, .value = (sxml_xslt_ast_value){.kind = SXML_XSLT_AST_VALUE_KIND_NAME,
                                                     .payload = {.name = name}}});
}

/* A mode attribute of one token: a name, or #current. */
static sxml_status add_mode_attribute(schematron_builder *builder, sxml_xslt_ast_node_index node,
                                      sxml_xslt_ast_name_token token) {
    sxml_xslt_ast_name_token_list list = {.first_name_token_index = 0, .name_token_count = 0};
    const sxml_status status = sxml_xslt_ast_add_name_tokens(
        builder->ast, (sxml_xslt_ast_name_token_view){.name_tokens = &token, .name_token_count = 1},
        &list);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_value_attribute(builder, (value_attribute){.node = node, .kind = SXML_XSLT_AST_ATTRIBUTE_MODE, .value = (sxml_xslt_ast_value){.kind = SXML_XSLT_AST_VALUE_KIND_NAME_LIST,
                                                     .payload = {.name_tokens = list}}});
}

static sxml_xslt_ast_name_token mode_token(sxml_name_identifier mode) {
    return (sxml_xslt_ast_name_token){.kind = SXML_XSLT_AST_NAME_TOKEN_KIND_NAME,
                                      .name = mode,
                                      .namespace_uri = {.index = 0},
                                      .has_arity = false,
                                      .arity = 0};
}

/* A synthesized node with one fixed expression attribute (select, test). */
typedef struct fixed_node {
    sxml_xslt_ast_kind kind;
    sxml_xslt_ast_node_index parent;
    sxml_xslt_ast_attribute_kind attribute_kind;
    sxml_byte_view text;
    /* Where errors of the text point: a document and a span. */
    uint32_t document;
    sxml_ast_source_span origin;
} fixed_node;

static sxml_status add_fixed_node(schematron_builder *builder, fixed_node request,
                                  sxml_xslt_ast_node_index *node) {
    sxml_status status = add_node(builder,
                                  (node_request){.kind = request.kind,
                                                 .parent = request.parent,
                                                 .place = {.document = request.document,
                                                           .element = {.index = 0}},
                                                 .has_place = false,
                                                 .is_declaration = false},
                                  node);
    if (status == SXML_STATUS_OK && request.attribute_kind != SXML_XSLT_AST_ATTRIBUTE_NONE) {
        status = add_fixed_attribute(builder,
                                     (fixed_attribute){.node = *node,
                                                       .kind = request.attribute_kind,
                                                       .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                       .text = request.text,
                                                       .document = request.document,
                                                       .origin = request.origin});
    }
    return status;
}

/* A local xsl:variable of a synthesized name with a fixed select. */
typedef struct fixed_variable {
    sxml_xslt_ast_node_index parent;
    sxml_name_identifier name;
    sxml_byte_view select;
    uint32_t document;
    sxml_ast_source_span origin;
} fixed_variable;

static sxml_status add_fixed_variable(schematron_builder *builder, fixed_variable request) {
    sxml_xslt_ast_node_index node = {.value = 0};
    sxml_status status = add_node(builder,
                                  (node_request){.kind = SXML_XSLT_AST_KIND_VARIABLE,
                                                 .parent = request.parent,
                                                 .place = {.document = 0, .element = {.index = 0}},
                                                 .has_place = false,
                                                 .is_declaration = false},
                                  &node);
    if (status == SXML_STATUS_OK) {
        status = add_name_attribute(builder, node, request.name);
    }
    if (status == SXML_STATUS_OK) {
        status = add_fixed_attribute(builder,
                                     (fixed_attribute){.node = node,
                                                       .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                                       .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                       .text = request.select,
                                                       .document = request.document,
                                                       .origin = request.origin});
    }
    return status;
}

/* "$Q{urn:sxml:schematron}name" and the like, in builder->scratch: the
   reference to a synthesized variable, between a prefix and a suffix. */
typedef struct reference_text {
    const char *before;
    sxml_name_identifier name;
    const char *after;
} reference_text;

static sxml_status make_reference_text(schematron_builder *builder, reference_text request,
                                       sxml_byte_view *text) {
    builder->scratch.byte_count = 0;
    sxml_status status =
        sxml_xml_buffer_append(&builder->scratch, sxml_byte_view_from_c_string(request.before));
    if (status == SXML_STATUS_OK) {
        status = append_eqname(builder, &builder->scratch, request.name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append(&builder->scratch, sxml_byte_view_from_c_string(request.after));
    }
    *text = sxml_schematron_buffer_text(&builder->scratch);
    return status;
}

/* ------------------------------------------------------------- Setup */

/* The sch:ns bindings (5.4.10), the last for a prefix that two bind. */
static sxml_status collect_namespaces(schematron_builder *builder, sxml_schematron_array *bindings) {
    sxml_schematron_compilation *compilation = builder->compilation;
    const sxml_schematron_place *places = compilation->index.namespaces.elements;
    sxml_status status = SXML_STATUS_OK;
    for (size_t place_index = 0;
         status == SXML_STATUS_OK && place_index < compilation->index.namespaces.count;
         place_index += 1) {
        const sxml_byte_view prefix_text =
            trimmed_of(builder, places[place_index], SXML_SCHEMATRON_ATTRIBUTE_PREFIX);
        const sxml_byte_view uri_text =
            trimmed_of(builder, places[place_index], SXML_SCHEMATRON_ATTRIBUTE_URI);
        if (!sxml_xml_text_is_ncname(prefix_text) ||
            sxml_byte_view_equals(prefix_text, SXML_TEXT("xml")) ||
            sxml_byte_view_equals(prefix_text, SXML_TEXT("xmlns"))) {
            continue;
        }
        sxml_namespace_binding binding = {.namespace_prefix = {.index = 0},
                                          .namespace_uri = {.index = 0}};
        status = sxml_name_pool_intern_namespace_prefix(compilation->name_pool, prefix_text,
                                                        &binding.namespace_prefix);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_intern_namespace_uri(compilation->name_pool, uri_text,
                                                         &binding.namespace_uri);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        sxml_namespace_binding *existing = bindings->elements;
        bool is_replaced = false;
        for (size_t binding_index = 0; binding_index < bindings->count; binding_index += 1) {
            if (existing[binding_index].namespace_prefix.index == binding.namespace_prefix.index) {
                existing[binding_index] = binding;
                is_replaced = true;
            }
        }
        if (!is_replaced) {
            status = sxml_schematron_array_push(compilation, bindings,
                                                SXML_SCHEMATRON_ELEMENT_OF(binding));
        }
    }
    return status;
}

/* The two static contexts of a document: its expressions' (default mode
   the validation's, so the stylesheet's default mode is) and its XSLT
   declarations' (the unnamed mode). */
typedef struct context_request {
    uint32_t document;
    sxml_namespace_binding_view bindings;
} context_request;

static sxml_status add_document_contexts(schematron_builder *builder, context_request request) {
    sxml_schematron_document *document = document_of(builder, request.document);
    const sxml_namespace_uri_identifier excluded = {.index = SXML_NAMESPACE_XSLT};
    sxml_xslt_ast_static_context_request context = {
        .parent = {.value = 0},
        .namespace_bindings = request.bindings,
        .xpath_default_namespace = {.index = SXML_NAMESPACE_NONE},
        .is_any_namespace = false,
        .default_collation = no_text,
        .default_mode = mode_token(builder->validate_name),
        .default_validation = SXML_XSLT_AST_KEYWORD_STRIP,
        .expands_text = false,
        .version = builder->compilation->binding.version,
        .excluded_namespaces = {.namespace_uris = &excluded, .namespace_uri_count = 1},
        .extension_namespaces = {.namespace_uris = NULL, .namespace_uri_count = 0},
        .static_base_uri = document->uri};
    sxml_xslt_ast_static_context_index expression_context = {.value = 0};
    sxml_status status = sxml_xslt_ast_add_static_context(builder->ast, &context, &expression_context);
    sxml_xslt_ast_static_context_index declaration_context = {.value = 0};
    if (status == SXML_STATUS_OK) {
        context.default_mode = (sxml_xslt_ast_name_token){.kind = SXML_XSLT_AST_NAME_TOKEN_KIND_UNNAMED,
                                                          .name = {.index = 0},
                                                          .namespace_uri = {.index = 0},
                                                          .has_arity = false,
                                                          .arity = 0};
        status = sxml_xslt_ast_add_static_context(builder->ast, &context, &declaration_context);
    }
    if (status == SXML_STATUS_OK) {
        document = document_of(builder, request.document);
        document->static_context = expression_context;
        document->declaration_context = declaration_context;
    }
    return status;
}

/* Gives an error the stylesheet reader or lowering reported to the
   compilation's sink (D320's collector, forwarding). */
static sxml_status forward_xslt_report(void *state, const sxml_schema_report *report) {
    sxml_schematron_compilation *compilation = state;
    sxml_error_record record = {.code = report->code,
                                .value = NULL,
                                .diagnostic = {.description = report->description,
                                               .location = report->location}};
    return sxml_schematron_report_record(compilation, &record);
}

static const sxml_schema_report_sink_operations forwarding_sink = {.receive = forward_xslt_report};

/* The XSLT elements Annexes C, H and J let a schema hold, under the xslt
   bindings: function, key, include, import, import-schema, and except
   under xslt2 accumulator and use-package (D315 "Foreign XSLT"). */
static bool is_read_declaration(const schematron_builder *builder, sxml_schematron_place place) {
    if (!builder->compilation->binding.is_xslt) {
        return false;
    }
    sxml_node_description description;
    memset(&description, 0, sizeof(description));
    if (sxml_node_describe((sxml_node_reference){.tree = document_of(builder, place.document)->tree,
                                                 .node_index = place.element,
                                                 .namespace_number = 0},
                           &description) != SXML_STATUS_OK) {
        return false;
    }
    sxml_name_parts parts = {.namespace_uri = {.index = 0}, .local_name = no_text};
    if (sxml_name_pool_get_name_parts(builder->compilation->name_pool, description.node_name,
                                      &parts) != SXML_STATUS_OK ||
        parts.namespace_uri.index != SXML_NAMESPACE_XSLT) {
        return false;
    }
    static const char *const names[] = {"function", "key", "include", "import", "import-schema",
                                        "accumulator", "use-package"};
    const size_t allowed_count = builder->compilation->binding.is_xslt_2 ? 5U : 7U;
    for (size_t name_index = 0; name_index < allowed_count; name_index += 1) {
        if (sxml_byte_view_equals(parts.local_name, sxml_byte_view_from_c_string(names[name_index]))) {
            return true;
        }
    }
    return false;
}

/* Reads one XSLT element into the AST: a declaration of the stylesheet,
   or an instruction under parent (src/xslt_read.c). */
typedef struct foreign_read {
    sxml_schematron_place place;
    sxml_xslt_ast_node_index parent;
    bool reads_instruction;
    sxml_namespace_binding_view bindings;
} foreign_read;

static sxml_status read_foreign(schematron_builder *builder, foreign_read request) {
    sxml_schematron_compilation *compilation = builder->compilation;
    const sxml_schematron_document *document = document_of(builder, request.place.document);
    sxml_node_description description;
    memset(&description, 0, sizeof(description));
    sxml_status status =
        sxml_node_describe((sxml_node_reference){.tree = document->tree,
                                                 .node_index = request.place.element,
                                                 .namespace_number = 0},
                           &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xslt_error_collector collector;
    memset(&collector, 0, sizeof(collector));
    collector.report_sink =
        (sxml_schema_report_sink){.state = compilation, .operations = &forwarding_sink};
    status = sxml_xslt_read_declaration(&(sxml_xslt_declaration_request){
        .processor = compilation->processor,
        .ast = builder->ast,
        .module = document->module,
        .parent = request.parent,
        .parent_element = description.parent,
        .parent_static_context = request.reads_instruction ? document->static_context
                                                           : document->declaration_context,
        .element = request.place.element,
        .extra_namespace_bindings = request.bindings,
        .error_record = &collector.current,
        .error_collector = &collector,
        .reads_instruction = request.reads_instruction});
    if (status != SXML_STATUS_OK && collector.sink_status == SXML_STATUS_OK &&
        (status == SXML_STATUS_STATIC_ERROR || status == SXML_STATUS_TYPE_ERROR ||
         status == SXML_STATUS_DYNAMIC_ERROR)) {
        /* An error that stopped the reader: given to the sink too. */
        status = sxml_schematron_report_record(compilation, &collector.current);
    } else if (collector.sink_status != SXML_STATUS_OK) {
        status = collector.sink_status;
    }
    return status;
}

/* ----------------------------------------------------------- Variables */

/* A let's or param's name as an expanded name: its prefix bound by sch:ns
   (5.4.10), as in the expressions that refer to it. */
/* A variable's name as written (a QName) and the bindings it is read
   with. */
typedef struct written_variable {
    sxml_byte_view text;
    sxml_namespace_binding_view bindings;
} written_variable;

static sxml_status resolve_variable_name(schematron_builder *builder, written_variable written_name,
                                         sxml_name_identifier *name) {
    const sxml_byte_view text = written_name.text;
    const sxml_namespace_binding_view bindings = written_name.bindings;
    sxml_schematron_compilation *compilation = builder->compilation;
    size_t colon_index = 0;
    while (colon_index < text.byte_count && text.bytes[colon_index] != ':') {
        colon_index += 1;
    }
    sxml_namespace_uri_identifier namespace_uri = {.index = SXML_NAMESPACE_NONE};
    sxml_byte_view local_name = text;
    if (colon_index < text.byte_count) {
        sxml_namespace_prefix_identifier prefix = {.index = 0};
        const sxml_status status = sxml_name_pool_intern_namespace_prefix(
            compilation->name_pool, (sxml_byte_view){.bytes = text.bytes, .byte_count = colon_index},
            &prefix);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        bool is_bound = false;
        for (size_t binding_index = 0; binding_index < bindings.namespace_binding_count;
             binding_index += 1) {
            if (bindings.namespace_bindings[binding_index].namespace_prefix.index == prefix.index) {
                namespace_uri = bindings.namespace_bindings[binding_index].namespace_uri;
                is_bound = true;
            }
        }
        if (!is_bound) {
            return SXML_STATUS_INDEX_OUT_OF_RANGE;
        }
        local_name = (sxml_byte_view){.bytes = text.bytes + colon_index + 1,
                                      .byte_count = text.byte_count - colon_index - 1};
    }
    return sxml_name_pool_intern_name(
        compilation->name_pool,
        &(sxml_name_request){.namespace_uri = namespace_uri, .local_name = local_name}, name);
}

/* Checks 5.4.7's one definition of a variable among the scopes around a
   rule: the global ones, then the local ones. A clash is reported; the
   name is added to scope either way. */
typedef struct variable_definition {
    sxml_schematron_array *scope;
    sxml_byte_view name;
    sxml_schematron_place place;
} variable_definition;

static sxml_status define_variable(schematron_builder *builder, variable_definition definition) {
    const sxml_schematron_array *scopes[] = {&builder->global_variables, &builder->local_variables};
    for (size_t scope_index = 0; scope_index < 2; scope_index += 1) {
        const scoped_variable *variables = scopes[scope_index]->elements;
        for (size_t variable_index = 0; variable_index < scopes[scope_index]->count;
             variable_index += 1) {
            if (sxml_byte_view_equals(variables[variable_index].name, definition.name)) {
                const sxml_status status =
                    report_at(builder, (builder_error){.code = "variable",
                                                       .description = "a variable is defined twice "
                                                                      "among its schema, phase, "
                                                                      "pattern and rule (5.4.7, 7.2)",
                                                       .place = definition.place,
                                                       .attribute_span = no_span,
                                                       .subject = definition.name});
                if (status != SXML_STATUS_OK) {
                    return status;
                }
                scope_index = 2;
                break;
            }
        }
    }
    const scoped_variable entry = {.name = definition.name, .place = definition.place};
    return sxml_schematron_array_push(builder->compilation, definition.scope,
                                      SXML_SCHEMATRON_ELEMENT_OF(entry));
}

/* An xsl:variable for a sch:let (5.4.7): global or in a template. */
typedef struct let_request {
    sxml_schematron_place place;
    sxml_xslt_ast_node_index parent;
    bool is_global;
    /* The variable's name; 0 for the let's own. */
    sxml_name_identifier name;
    sxml_namespace_binding_view bindings;
} let_request;

static sxml_status emit_let(schematron_builder *builder, let_request request) {
    sxml_name_identifier name = request.name;
    sxml_status status = SXML_STATUS_OK;
    if (name.index == 0) {
        const sxml_schematron_attribute written =
            attribute_of(builder, request.place, SXML_SCHEMATRON_ATTRIBUTE_NAME);
        status = resolve_variable_name(builder, (written_variable){.text = sxml_xml_text_trim_whitespace(written.value), .bindings = request.bindings}, &name);
        if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
            return report_at(builder,
                             (builder_error){.code = "syntax",
                                             .description = "no ns binds the prefix of a variable's "
                                                            "name (5.4.10)",
                                             .place = request.place,
                                             .attribute_span = written.source_span,
                                             .subject = sxml_xml_text_trim_whitespace(written.value)});
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    parsed_expression value = {.is_present = false, .ast = NULL, .source_span = no_span};
    status = parse_attribute(builder,
                             &(expression_request){.place = request.place,
                                                   .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_VALUE,
                                                   .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                   .expands = true,
                                                   .table_text = NULL},
                             &value);
    parsed_expression type = {.is_present = false, .ast = NULL, .source_span = no_span};
    if (status == SXML_STATUS_OK) {
        /* as: applied as XSLT's (9.1) under every binding (D315). */
        status = parse_attribute(builder,
                                 &(expression_request){.place = request.place,
                                                       .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_AS,
                                                       .value_kind = SXML_XSLT_AST_VALUE_KIND_SEQUENCE_TYPE,
                                                       .expands = false,
                                                       .table_text = NULL},
                                 &type);
    }
    sxml_xslt_ast_node_index node = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = add_node(builder,
                          (node_request){.kind = SXML_XSLT_AST_KIND_VARIABLE,
                                         .parent = request.is_global ? builder->root : request.parent,
                                         .place = request.place,
                                         .has_place = true,
                                         .is_declaration = request.is_global},
                          &node);
    }
    if (status == SXML_STATUS_OK) {
        status = add_name_attribute(builder, node, name);
    }
    if (status == SXML_STATUS_OK && value.ast != NULL) {
        status = add_expression_attribute(builder,
                                          (expression_attribute){.node = node,
                                                                 .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                                                 .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                                 .ast = value.ast,
                                                                 .document = request.place.document,
                                                                 .source_span = value.source_span});
        value.ast = NULL;
    }
    if (status == SXML_STATUS_OK && type.ast != NULL) {
        status = add_expression_attribute(builder,
                                          (expression_attribute){.node = node,
                                                                 .kind = SXML_XSLT_AST_ATTRIBUTE_AS,
                                                                 .value_kind = SXML_XSLT_AST_VALUE_KIND_SEQUENCE_TYPE,
                                                                 .ast = type.ast,
                                                                 .document = request.place.document,
                                                                 .source_span = type.source_span});
        type.ast = NULL;
    }
    sxml_ast_destroy(value.ast);
    sxml_ast_destroy(type.ast);
    if (status != SXML_STATUS_OK || value.is_present) {
        return status;
    }
    /* Without value, the content is the value: a temporary tree, its
       foreign elements read as literal result elements (D315). */
    sxml_schematron_cursor cursor = {.compilation = builder->compilation,
                                     .frames = {.elements = NULL, .count = 0, .capacity = 0},
                                     .expands_rules = false};
    status = sxml_schematron_cursor_start(&cursor, request.place);
    for (;;) {
        sxml_schematron_child child = {.has_child = false,
                                       .place = {.document = 0, .element = {.index = 0}},
                                       .element_kind = SXML_SCHEMATRON_ELEMENT_NONE,
                                       .is_inserted = false};
        if (status == SXML_STATUS_OK) {
            status = sxml_schematron_cursor_next(&cursor, &child);
        }
        if (status != SXML_STATUS_OK || !child.has_child) {
            break;
        }
        if (child.element_kind == SXML_SCHEMATRON_ELEMENT_FOREIGN) {
            status = read_foreign(builder, (foreign_read){.place = child.place,
                                                          .parent = node,
                                                          .reads_instruction = true,
                                                          .bindings = request.bindings});
        }
    }
    sxml_schematron_cursor_finish(&cursor);
    return status;
}

/* ------------------------------------------------------------ Messages */

/* The text of a foreign element in a message: its descendant text nodes
   in order (D315: foreign elements by their text). */
static sxml_status append_element_text(schematron_builder *builder, sxml_schematron_place place,
                                       sxml_xml_buffer *text) {
    const sxml_schematron_document *document = document_of(builder, place.document);
    sxml_node_description description;
    memset(&description, 0, sizeof(description));
    sxml_status status = sxml_node_describe(
        (sxml_node_reference){.tree = document->tree, .node_index = place.element, .namespace_number = 0},
        &description);
    for (uint32_t index = place.element.index + 1;
         status == SXML_STATUS_OK && index <= description.last_descendant.index; index += 1) {
        const sxml_node_reference node = {.tree = document->tree,
                                          .node_index = {.index = index},
                                          .namespace_number = 0};
        sxml_node_description node_description;
        memset(&node_description, 0, sizeof(node_description));
        status = sxml_node_describe(node, &node_description);
        if (status != SXML_STATUS_OK || node_description.node_kind != SXML_NODE_KIND_TEXT) {
            continue;
        }
        sxml_byte_view content = no_text;
        status = sxml_node_get_content(node, &content);
        if (status == SXML_STATUS_OK) {
            status = sxml_xml_buffer_append(text, content);
        }
    }
    return status;
}

/* One element whose message content is being emitted. */
typedef struct message_frame {
    sxml_schematron_place element;
    bool has_next;
    sxml_node_index next_child;
} message_frame;

/* A message to emit: the content of element (an assert, report,
   diagnostic or property) as the sequence constructor of an xsl:value-of
   child of parent, whose string the C side collapses (5.3). */
typedef struct message_request {
    sxml_schematron_place element;
    sxml_xslt_ast_node_index parent;
    /* Properties under the xslt bindings: xsl:copy-of is read as an
       instruction (Annexes C, H, J). */
    bool reads_copy_of;
    sxml_namespace_binding_view bindings;
} message_request;

/* Emits text collected so far as a text node. */
static sxml_status flush_message_text(schematron_builder *builder, sxml_xslt_ast_node_index parent,
                                      sxml_xml_buffer *text) {
    if (text->byte_count == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_status status = add_text_node(builder, parent, sxml_schematron_buffer_text(text));
    text->byte_count = 0;
    return status;
}

/* sch:value-of (5.4.18): fn:string of its select (Annexes C, H to N), a
   result of several items XPTY0004 at the select (D315, D317). */
static sxml_status emit_value_of(schematron_builder *builder, sxml_schematron_place place,
                                 sxml_xslt_ast_node_index parent) {
    parsed_expression select = {.is_present = false, .ast = NULL, .source_span = no_span};
    sxml_status status =
        parse_attribute(builder,
                        &(expression_request){.place = place,
                                              .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_SELECT,
                                              .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                              .expands = true,
                                              .table_text = NULL},
                        &select);
    if (status != SXML_STATUS_OK || select.ast == NULL) {
        return status;
    }
    sxml_xslt_ast_node_index variable = {.value = 0};
    status = add_node(builder,
                      (node_request){.kind = SXML_XSLT_AST_KIND_VARIABLE,
                                     .parent = parent,
                                     .place = place,
                                     .has_place = true,
                                     .is_declaration = false},
                      &variable);
    if (status == SXML_STATUS_OK) {
        status = add_name_attribute(builder, variable, builder->value_name);
    }
    if (status == SXML_STATUS_OK) {
        status = add_expression_attribute(builder,
                                          (expression_attribute){.node = variable,
                                                                 .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                                                 .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                                 .ast = select.ast,
                                                                 .document = place.document,
                                                                 .source_span = select.source_span});
    } else {
        sxml_ast_destroy(select.ast);
    }
    sxml_byte_view text = no_text;
    if (status == SXML_STATUS_OK) {
        status = make_reference_text(
            builder, (reference_text){.before = "string($", .name = builder->value_name, .after = ")"},
            &text);
    }
    sxml_xslt_ast_node_index value_of = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = add_fixed_node(builder,
                                (fixed_node){.kind = SXML_XSLT_AST_KIND_VALUE_OF,
                                             .parent = parent,
                                             .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                             .text = text,
                                             .document = place.document,
                                             .origin = select.source_span},
                                &value_of);
    }
    return status;
}

/* sch:name (5.4.9): fn:node-name of its path's result, or of the context
   item, as a string (Annex J); a path that selects more than one item is
   XPTY0004, as fn:node-name's argument would be. */
static sxml_status emit_name(schematron_builder *builder, sxml_schematron_place place,
                             sxml_xslt_ast_node_index parent) {
    parsed_expression path = {.is_present = false, .ast = NULL, .source_span = no_span};
    sxml_status status =
        parse_attribute(builder,
                        &(expression_request){.place = place,
                                              .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_PATH,
                                              .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                              .expands = true,
                                              .table_text = NULL},
                        &path);
    if (status != SXML_STATUS_OK || (path.is_present && path.ast == NULL)) {
        return status;
    }
    sxml_xslt_ast_node_index container = parent;
    const sxml_ast_source_span origin =
        path.is_present ? path.source_span : sxml_schematron_element_span(builder->compilation, place);
    if (path.ast != NULL) {
        status = add_node(builder,
                          (node_request){.kind = SXML_XSLT_AST_KIND_FOR_EACH,
                                         .parent = parent,
                                         .place = place,
                                         .has_place = true,
                                         .is_declaration = false},
                          &container);
        if (status == SXML_STATUS_OK) {
            status = add_expression_attribute(
                builder, (expression_attribute){.node = container,
                                                .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                                .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                .ast = path.ast,
                                                .document = place.document,
                                                .source_span = path.source_span});
        } else {
            sxml_ast_destroy(path.ast);
        }
    }
    sxml_xslt_ast_node_index value_of = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = add_fixed_node(builder,
                                (fixed_node){.kind = SXML_XSLT_AST_KIND_VALUE_OF,
                                             .parent = container,
                                             .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                             .text = path.is_present ? SXML_TEXT(
                                                         "if (last() gt 1) then error(QName("
                                                         "'http://www.w3.org/2005/xqt-errors', "
                                                         "'err:XPTY0004'), 'the path of sch:name "
                                                         "selects more than one item, which "
                                                         "fn:node-name does not accept') else "
                                                         "node-name(.)")
                                                                     : SXML_TEXT("node-name(.)"),
                                             .document = place.document,
                                             .origin = origin},
                                &value_of);
    }
    return status;
}

static sxml_status emit_message(schematron_builder *builder, message_request request) {
    sxml_schematron_compilation *compilation = builder->compilation;
    sxml_xslt_ast_node_index value_of = {.value = 0};
    sxml_status status = add_node(builder,
                                  (node_request){.kind = SXML_XSLT_AST_KIND_VALUE_OF,
                                                 .parent = request.parent,
                                                 .place = request.element,
                                                 .has_place = true,
                                                 .is_declaration = false},
                                  &value_of);
    sxml_schematron_array frames = {.elements = NULL, .count = 0, .capacity = 0};
    sxml_xml_buffer text = sxml_xml_buffer_empty(compilation->allocator);
    const sxml_schematron_document *document = document_of(builder, request.element.document);
    sxml_node_description description;
    memset(&description, 0, sizeof(description));
    if (status == SXML_STATUS_OK) {
        status = sxml_node_describe((sxml_node_reference){.tree = document->tree,
                                                          .node_index = request.element.element,
                                                          .namespace_number = 0},
                                    &description);
    }
    if (status == SXML_STATUS_OK) {
        const message_frame frame = {.element = request.element,
                                     .has_next = description.has_children,
                                     .next_child = description.first_child};
        status = sxml_schematron_array_push(compilation, &frames, SXML_SCHEMATRON_ELEMENT_OF(frame));
    }
    while (status == SXML_STATUS_OK && frames.count > 0) {
        message_frame *top = &((message_frame *)frames.elements)[frames.count - 1U];
        if (!top->has_next) {
            frames.count -= 1;
            continue;
        }
        const sxml_schematron_place child = {.document = top->element.document,
                                             .element = top->next_child};
        sxml_node_description child_description;
        memset(&child_description, 0, sizeof(child_description));
        status = sxml_node_describe((sxml_node_reference){.tree = document->tree,
                                                          .node_index = child.element,
                                                          .namespace_number = 0},
                                    &child_description);
        if (status != SXML_STATUS_OK) {
            break;
        }
        top->has_next = child_description.has_next_sibling;
        top->next_child = child_description.next_sibling;
        if (child_description.node_kind == SXML_NODE_KIND_TEXT) {
            sxml_byte_view content = no_text;
            status = sxml_node_get_content((sxml_node_reference){.tree = document->tree,
                                                                 .node_index = child.element,
                                                                 .namespace_number = 0},
                                           &content);
            if (status == SXML_STATUS_OK) {
                status = sxml_xml_buffer_append(&text, content);
            }
            continue;
        }
        if (child_description.node_kind != SXML_NODE_KIND_ELEMENT) {
            continue;
        }
        const sxml_schematron_element_kind kind = sxml_schematron_element_kind_of(compilation, child);
        switch ((uint32_t)kind) {
        case SXML_SCHEMATRON_ELEMENT_VALUE_OF:
            status = flush_message_text(builder, value_of, &text);
            if (status == SXML_STATUS_OK) {
                status = emit_value_of(builder, child, value_of);
            }
            break;
        case SXML_SCHEMATRON_ELEMENT_NAME:
            status = flush_message_text(builder, value_of, &text);
            if (status == SXML_STATUS_OK) {
                status = emit_name(builder, child, value_of);
            }
            break;
        case SXML_SCHEMATRON_ELEMENT_EMPH:
        case SXML_SCHEMATRON_ELEMENT_DIR:
        case SXML_SCHEMATRON_ELEMENT_SPAN: {
            /* Their markup is reduced to its text (D315 "SVRL"). */
            const message_frame frame = {.element = child,
                                         .has_next = child_description.has_children,
                                         .next_child = child_description.first_child};
            status = sxml_schematron_array_push(compilation, &frames, SXML_SCHEMATRON_ELEMENT_OF(frame));
            break;
        }
        case SXML_SCHEMATRON_ELEMENT_FOREIGN: {
            sxml_name_parts parts = {.namespace_uri = {.index = 0}, .local_name = no_text};
            status = sxml_name_pool_get_name_parts(compilation->name_pool, child_description.node_name,
                                                   &parts);
            if (status == SXML_STATUS_OK && request.reads_copy_of &&
                parts.namespace_uri.index == SXML_NAMESPACE_XSLT &&
                sxml_byte_view_equals(parts.local_name, SXML_TEXT("copy-of"))) {
                status = flush_message_text(builder, value_of, &text);
                if (status == SXML_STATUS_OK) {
                    status = read_foreign(builder, (foreign_read){.place = child,
                                                                  .parent = value_of,
                                                                  .reads_instruction = true,
                                                                  .bindings = request.bindings});
                }
            } else if (status == SXML_STATUS_OK) {
                status = append_element_text(builder, child, &text);
            }
            break;
        }
        default:
            break;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = flush_message_text(builder, value_of, &text);
    }
    sxml_xml_buffer_release(&text);
    sxml_schematron_array_release(compilation, &frames);
    return status;
}

/* ------------------------------------------------------------ Findings */

/* The table's index of a diagnostic or property (added when first
   named). */
typedef struct annotation_lookup {
    const sxml_schematron_identified *identified;
    bool is_property;
} annotation_lookup;

static sxml_status find_annotation(schematron_builder *builder, annotation_lookup lookup, uint32_t *index) {
    sxml_schematron_table *table = &builder->compilation->table;
    sxml_schematron_annotation_entry *entries =
        lookup.is_property ? table->properties : table->diagnostics;
    const size_t count = lookup.is_property ? table->property_count : table->diagnostic_count;
    for (size_t entry_index = 0; entry_index < count; entry_index += 1) {
        if (sxml_byte_view_equals(sxml_schematron_table_text(table, entries[entry_index].identifier),
                                  lookup.identified->identifier)) {
            *index = (uint32_t)entry_index;
            return SXML_STATUS_OK;
        }
    }
    const sxml_schematron_place place = lookup.identified->place;
    const sxml_schematron_inherited inherited =
        sxml_schematron_inherited_of(builder->compilation, place);
    sxml_schematron_annotation_entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.preserves_space = inherited.preserves_space;
    sxml_status status = add_table_text(builder, lookup.identified->identifier, &entry.identifier);
    if (status == SXML_STATUS_OK && !lookup.is_property) {
        status = add_table_text(builder, inherited.language, &entry.language);
    }
    if (status == SXML_STATUS_OK) {
        status = add_table_text(builder, trimmed_of(builder, place, SXML_SCHEMATRON_ATTRIBUTE_ROLE),
                                &entry.role);
    }
    if (status == SXML_STATUS_OK && lookup.is_property) {
        status = add_table_text(builder, trimmed_of(builder, place, SXML_SCHEMATRON_ATTRIBUTE_SCHEME),
                                &entry.scheme);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (lookup.is_property) {
        status = SXML_GROW(builder, table->properties, table->property_count,
                           sxml_schematron_annotation_entry);
        if (status == SXML_STATUS_OK) {
            table->properties[table->property_count - 1U] = entry;
            *index = (uint32_t)(table->property_count - 1U);
        }
    } else {
        status = SXML_GROW(builder, table->diagnostics, table->diagnostic_count,
                           sxml_schematron_annotation_entry);
        if (status == SXML_STATUS_OK) {
            table->diagnostics[table->diagnostic_count - 1U] = entry;
            *index = (uint32_t)(table->diagnostic_count - 1U);
        }
    }
    return status;
}

/* The places of the diagnostics or properties an assertion names, in
   order, as table references; unknown ids were reported by reading. */
typedef struct annotation_list_request {
    sxml_schematron_place assertion;
    bool is_property;
    sxml_schematron_reference_list *list;
    /* sxml_schematron_identified, out. */
    sxml_schematron_array *places;
} annotation_list_request;

static sxml_status collect_annotations(schematron_builder *builder, annotation_list_request request) {
    sxml_schematron_compilation *compilation = builder->compilation;
    sxml_schematron_table *table = &compilation->table;
    const sxml_schematron_attribute attribute = attribute_of(
        builder, request.assertion,
        request.is_property ? SXML_SCHEMATRON_ATTRIBUTE_PROPERTIES : SXML_SCHEMATRON_ATTRIBUTE_DIAGNOSTICS);
    *request.list = (sxml_schematron_reference_list){.first_reference = (uint32_t)table->reference_count,
                                                     .reference_count = 0};
    request.places->count = 0;
    sxml_byte_view rest = attribute.value;
    sxml_byte_view token = no_text;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && sxml_xml_text_next_token(&rest, &token)) {
        const sxml_schematron_identified *identified = sxml_schematron_find_identified(
            request.is_property ? &compilation->index.properties : &compilation->index.diagnostics, token);
        if (identified == NULL) {
            continue;
        }
        uint32_t index = 0;
        status = find_annotation(builder, (annotation_lookup){.identified = identified,
                                                              .is_property = request.is_property},
                                 &index);
        if (status == SXML_STATUS_OK) {
            status = SXML_GROW(builder, table->references, table->reference_count, uint32_t);
        }
        if (status == SXML_STATUS_OK) {
            table->references[table->reference_count - 1U] = index;
            request.list->reference_count += 1;
            status = sxml_schematron_array_push(compilation, request.places,
                                                SXML_SCHEMATRON_ELEMENT_OF(*identified));
        }
    }
    return status;
}

/* A role, flag or severity: static (as written, trimmed, into the
   entry) or dynamic (5.5.6, 5.5.14, 5.5.16: a variable reference under
   every binding, an attribute value template under the xslt bindings),
   parsed for the item. */
typedef struct dynamic_request {
    sxml_schematron_place place;
    sxml_schematron_attribute_kind attribute_kind;
    uint32_t dynamic_bit;
    sxml_schematron_entry *entry;
    sxml_schematron_text *static_text;
    /* Out: the parsed value of a dynamic one (owned), or NULL. */
    parsed_expression *parsed;
} dynamic_request;

static sxml_status read_dynamic_attribute(schematron_builder *builder, dynamic_request request) {
    *request.parsed = (parsed_expression){.is_present = false, .ast = NULL, .source_span = no_span};
    const sxml_schematron_attribute attribute =
        attribute_of(builder, request.place, request.attribute_kind);
    if (!attribute.is_present) {
        return SXML_STATUS_OK;
    }
    const sxml_byte_view text = sxml_xml_text_trim_whitespace(attribute.value);
    bool is_template = false;
    for (size_t byte_index = 0; builder->compilation->binding.is_xslt && byte_index < text.byte_count;
         byte_index += 1) {
        if (text.bytes[byte_index] == '{') {
            is_template = true;
        }
    }
    /* A variable reference: "$" and a name, after placeholders. */
    sxml_schematron_expansion expansion;
    sxml_status status = sxml_schematron_expand(
        builder->compilation->allocator,
        &(sxml_schematron_expansion_request){
            .text = text,
            .instance_parameters = builder->has_instance
                                       ? table_of(&builder->instance_parameters)
                                       : (sxml_schematron_parameter_table){.values = NULL, .value_count = 0},
            .schema_parameters = table_of(&builder->schema_parameters),
            .renamed_variables = table_of(&builder->renamed_variables)},
        &expansion);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_byte_view expanded = sxml_xml_text_trim_whitespace(sxml_schematron_buffer_text(&expansion.text));
    const bool is_variable = expanded.byte_count > 1 && expanded.bytes[0] == '$';
    if (!is_variable && !is_template) {
        status = add_table_text(builder, expanded, request.static_text);
        sxml_schematron_expansion_release(&expansion);
        return status;
    }
    sxml_schematron_expansion_release(&expansion);
    request.entry->dynamic_mask |= request.dynamic_bit;
    return parse_attribute(builder,
                           &(expression_request){.place = request.place,
                                                 .attribute_kind = request.attribute_kind,
                                                 .value_kind = is_template && !is_variable
                                                                   ? SXML_XSLT_AST_VALUE_KIND_VALUE_TEMPLATE
                                                                   : SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                 .expands = true,
                                                 .table_text = NULL},
                           request.parsed);
}

/* An array member holding the string of a dynamic value: an xsl:value-of
   of the parsed expression. */
/* A member of a finding's array for a dynamic attribute value. */
typedef struct dynamic_member {
    sxml_xslt_ast_node_index array;
    parsed_expression *parsed;
    uint32_t document;
} dynamic_member;

static sxml_status emit_dynamic_member(schematron_builder *builder, dynamic_member request) {
    const sxml_xslt_ast_node_index array = request.array;
    parsed_expression *parsed = request.parsed;
    const uint32_t document = request.document;
    sxml_xslt_ast_node_index member = {.value = 0};
    sxml_status status = add_node(builder,
                                  (node_request){.kind = SXML_XSLT_AST_KIND_ARRAY_MEMBER,
                                                 .parent = array,
                                                 .place = {.document = 0, .element = {.index = 0}},
                                                 .has_place = false,
                                                 .is_declaration = false},
                                  &member);
    sxml_xslt_ast_node_index value_of = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = add_node(builder,
                          (node_request){.kind = SXML_XSLT_AST_KIND_VALUE_OF,
                                         .parent = member,
                                         .place = {.document = 0, .element = {.index = 0}},
                                         .has_place = false,
                                         .is_declaration = false},
                          &value_of);
    }
    if (status == SXML_STATUS_OK && parsed->ast != NULL) {
        status = add_expression_attribute(builder,
                                          (expression_attribute){.node = value_of,
                                                                 .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                                                 .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                                 .ast = parsed->ast,
                                                                 .document = document,
                                                                 .source_span = parsed->source_span});
        parsed->ast = NULL;
    }
    return status;
}

/* An xsl:array-member with a fixed select. */
static sxml_status emit_fixed_member(schematron_builder *builder, sxml_xslt_ast_node_index array,
                                     sxml_byte_view select) {
    sxml_xslt_ast_node_index member = {.value = 0};
    return add_fixed_node(builder,
                          (fixed_node){.kind = SXML_XSLT_AST_KIND_ARRAY_MEMBER,
                                       .parent = array,
                                       .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                       .text = select,
                                       .document = 0,
                                       .origin = no_span},
                          &member);
}

/* The common attributes of an entry: see, icon, fpi, xml:lang in scope,
   the element's place. */
static sxml_status fill_entry(schematron_builder *builder, sxml_schematron_place place,
                              sxml_schematron_entry *entry) {
    const sxml_schematron_inherited inherited = sxml_schematron_inherited_of(builder->compilation, place);
    entry->document = place.document;
    entry->source_span = sxml_schematron_element_span(builder->compilation, place);
    entry->preserves_space = inherited.preserves_space;
    sxml_status status = add_table_text(builder, trimmed_of(builder, place, SXML_SCHEMATRON_ATTRIBUTE_ID),
                                        &entry->identifier);
    if (status == SXML_STATUS_OK) {
        status = add_table_text(builder, trimmed_of(builder, place, SXML_SCHEMATRON_ATTRIBUTE_SEE),
                                &entry->see);
    }
    if (status == SXML_STATUS_OK) {
        status = add_table_text(builder, trimmed_of(builder, place, SXML_SCHEMATRON_ATTRIBUTE_ICON),
                                &entry->icon);
    }
    if (status == SXML_STATUS_OK) {
        status = add_table_text(builder, trimmed_of(builder, place, SXML_SCHEMATRON_ATTRIBUTE_FPI),
                                &entry->formal_public_identifier);
    }
    if (status == SXML_STATUS_OK) {
        status = add_table_text(builder, inherited.language, &entry->language);
    }
    return status;
}

/* Appends an entry to the table and writes its index. */
static sxml_status add_entry(schematron_builder *builder, const sxml_schematron_entry *entry, uint32_t *index) {
    sxml_schematron_table *table = &builder->compilation->table;
    const sxml_status status =
        SXML_GROW(builder, table->entries, table->entry_count, sxml_schematron_entry);
    if (status == SXML_STATUS_OK) {
        table->entries[table->entry_count - 1U] = *entry;
        *index = (uint32_t)(table->entry_count - 1U);
    }
    return status;
}

/* "INDEX" as text, in builder->scratch. */
static sxml_byte_view number_text(schematron_builder *builder, uint32_t number) {
    char digits[16];
    const int written = snprintf(digits, sizeof(digits), "%u", number);
    builder->scratch.byte_count = 0;
    if (written > 0 &&
        sxml_xml_buffer_append(&builder->scratch, (sxml_byte_view){.bytes = (const uint8_t *)digits,
                                                                   .byte_count = (size_t)written}) ==
            SXML_STATUS_OK) {
        return sxml_schematron_buffer_text(&builder->scratch);
    }
    return SXML_TEXT("0");
}

/* The rule an assertion belongs to, as the assertion's item needs it. */
typedef struct rule_context {
    sxml_schematron_place rule;
    uint32_t pattern;
    sxml_schematron_text context_text;
    sxml_schematron_text visit_each_text;
    sxml_namespace_binding_view bindings;
} rule_context;

/* The subject (5.5.19, D315): what the assertion's subject attribute,
   else the rule's, selects for the rule's context node; else the context
   item when it is a node, else the rule's context node. Writes the
   variable Q{urn:sxml:schematron}subject under parent, and whether a
   subject attribute applies. */
typedef struct subject_request {
    sxml_schematron_place assertion;
    const rule_context *rule;
    sxml_xslt_ast_node_index parent;
    bool *has_subject;
    sxml_schematron_entry *entry;
} subject_request;

static sxml_status emit_subject(schematron_builder *builder, subject_request request) {
    sxml_schematron_place subject_place = request.assertion;
    if (!attribute_of(builder, request.assertion, SXML_SCHEMATRON_ATTRIBUTE_SUBJECT).is_present) {
        subject_place = request.rule->rule;
    }
    parsed_expression subject = {.is_present = false, .ast = NULL, .source_span = no_span};
    sxml_status status =
        parse_attribute(builder,
                        &(expression_request){.place = subject_place,
                                              .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_SUBJECT,
                                              .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                              .expands = true,
                                              .table_text = NULL},
                        &subject);
    *request.has_subject = subject.is_present;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_byte_view text = no_text;
    if (!subject.is_present) {
        status = make_reference_text(
            builder,
            (reference_text){.before = "if (. instance of node()) then . else $",
                             .name = builder->context_name,
                             .after = ""},
            &text);
        if (status == SXML_STATUS_OK) {
            status = add_fixed_variable(builder, (fixed_variable){.parent = request.parent,
                                                                  .name = builder->subject_name,
                                                                  .select = text,
                                                                  .document = 0,
                                                                  .origin = no_span});
        }
        return status;
    }
    request.entry->subject_span = subject.source_span;
    request.entry->subject_document = subject_place.document;
    sxml_xslt_ast_node_index variable = {.value = 0};
    status = add_node(builder,
                      (node_request){.kind = SXML_XSLT_AST_KIND_VARIABLE,
                                     .parent = request.parent,
                                     .place = subject_place,
                                     .has_place = true,
                                     .is_declaration = false},
                      &variable);
    if (status == SXML_STATUS_OK) {
        status = add_name_attribute(builder, variable, builder->subject_name);
    }
    if (status == SXML_STATUS_OK) {
        status = add_fixed_attribute(builder,
                                     (fixed_attribute){.node = variable,
                                                       .kind = SXML_XSLT_AST_ATTRIBUTE_AS,
                                                       .value_kind = SXML_XSLT_AST_VALUE_KIND_SEQUENCE_TYPE,
                                                       .text = SXML_TEXT("item()*"),
                                                       .document = subject_place.document,
                                                       .origin = subject.source_span});
    }
    sxml_xslt_ast_node_index for_each = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = make_reference_text(
            builder, (reference_text){.before = "$", .name = builder->context_name, .after = ""}, &text);
    }
    if (status == SXML_STATUS_OK) {
        status = add_fixed_node(builder,
                                (fixed_node){.kind = SXML_XSLT_AST_KIND_FOR_EACH,
                                             .parent = variable,
                                             .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                             .text = text,
                                             .document = subject_place.document,
                                             .origin = subject.source_span},
                                &for_each);
    }
    sxml_xslt_ast_node_index sequence = {.value = 0};
    if (status == SXML_STATUS_OK && subject.ast != NULL) {
        status = add_node(builder,
                          (node_request){.kind = SXML_XSLT_AST_KIND_SEQUENCE,
                                         .parent = for_each,
                                         .place = subject_place,
                                         .has_place = true,
                                         .is_declaration = false},
                          &sequence);
        if (status == SXML_STATUS_OK) {
            status = add_expression_attribute(builder,
                                              (expression_attribute){.node = sequence,
                                                                     .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                                                     .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                                     .ast = subject.ast,
                                                                     .document = subject_place.document,
                                                                     .source_span = subject.source_span});
            subject.ast = NULL;
        }
    }
    sxml_ast_destroy(subject.ast);
    return status;
}

/* One sch:assert or sch:report (5.4.3, 5.4.14): a condition in the rule
   template whose failure (assert) or success (report) adds a finding
   item to the raw result (D315 "A finding item"). */
typedef struct assertion_request {
    sxml_schematron_place assertion;
    const rule_context *rule;
    sxml_xslt_ast_node_index parent;
} assertion_request;

static sxml_status emit_assertion(schematron_builder *builder, assertion_request request) {
    sxml_schematron_compilation *compilation = builder->compilation;
    const bool is_report = sxml_schematron_element_kind_of(compilation, request.assertion) ==
                           SXML_SCHEMATRON_ELEMENT_REPORT;
    sxml_schematron_entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.entry_kind = is_report ? SXML_SCHEMATRON_ENTRY_REPORT : SXML_SCHEMATRON_ENTRY_ASSERT;
    entry.pattern = request.rule->pattern;
    entry.context = request.rule->context_text;
    entry.visit_each = request.rule->visit_each_text;
    sxml_status status = fill_entry(builder, request.assertion, &entry);
    parsed_expression test = {.is_present = false, .ast = NULL, .source_span = no_span};
    if (status == SXML_STATUS_OK) {
        status = parse_attribute(builder,
                                 &(expression_request){.place = request.assertion,
                                                       .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_TEST,
                                                       .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                       .expands = true,
                                                       .table_text = &entry.test},
                                 &test);
    }
    parsed_expression dynamics[3] = {{.is_present = false, .ast = NULL},
                                     {.is_present = false, .ast = NULL},
                                     {.is_present = false, .ast = NULL}};
    const sxml_schematron_attribute_kind dynamic_kinds[3] = {SXML_SCHEMATRON_ATTRIBUTE_ROLE,
                                                             SXML_SCHEMATRON_ATTRIBUTE_FLAG,
                                                             SXML_SCHEMATRON_ATTRIBUTE_SEVERITY};
    sxml_schematron_text *static_texts[3] = {&entry.role, &entry.flag, &entry.severity};
    const uint32_t bits[3] = {SXML_SCHEMATRON_DYNAMIC_ROLE, SXML_SCHEMATRON_DYNAMIC_FLAG,
                              SXML_SCHEMATRON_DYNAMIC_SEVERITY};
    for (size_t dynamic_index = 0; status == SXML_STATUS_OK && dynamic_index < 3; dynamic_index += 1) {
        status = read_dynamic_attribute(builder, (dynamic_request){.place = request.assertion,
                                                                   .attribute_kind = dynamic_kinds[dynamic_index],
                                                                   .dynamic_bit = bits[dynamic_index],
                                                                   .entry = &entry,
                                                                   .static_text = static_texts[dynamic_index],
                                                                   .parsed = &dynamics[dynamic_index]});
    }
    sxml_schematron_array diagnostic_places = {.elements = NULL, .count = 0, .capacity = 0};
    sxml_schematron_array property_places = {.elements = NULL, .count = 0, .capacity = 0};
    if (status == SXML_STATUS_OK) {
        status = collect_annotations(builder, (annotation_list_request){.assertion = request.assertion,
                                                                        .is_property = false,
                                                                        .list = &entry.diagnostics,
                                                                        .places = &diagnostic_places});
    }
    if (status == SXML_STATUS_OK) {
        status = collect_annotations(builder, (annotation_list_request){.assertion = request.assertion,
                                                                        .is_property = true,
                                                                        .list = &entry.properties,
                                                                        .places = &property_places});
    }
    uint32_t entry_index = 0;
    if (status == SXML_STATUS_OK) {
        status = add_entry(builder, &entry, &entry_index);
    }
    /* assert: xsl:choose with an empty xsl:when (the test is never
       negated in text); report: xsl:if. */
    sxml_xslt_ast_node_index condition = {.value = 0};
    sxml_xslt_ast_node_index branch = {.value = 0};
    if (status == SXML_STATUS_OK && test.ast != NULL) {
        sxml_xslt_ast_node_index choose = request.parent;
        if (!is_report) {
            status = add_node(builder,
                              (node_request){.kind = SXML_XSLT_AST_KIND_CHOOSE,
                                             .parent = request.parent,
                                             .place = request.assertion,
                                             .has_place = true,
                                             .is_declaration = false},
                              &choose);
        }
        if (status == SXML_STATUS_OK) {
            status = add_node(builder,
                              (node_request){.kind = is_report ? SXML_XSLT_AST_KIND_IF
                                                               : SXML_XSLT_AST_KIND_WHEN,
                                             .parent = choose,
                                             .place = request.assertion,
                                             .has_place = true,
                                             .is_declaration = false},
                              &condition);
        }
        if (status == SXML_STATUS_OK) {
            status = add_expression_attribute(builder,
                                              (expression_attribute){.node = condition,
                                                                     .kind = SXML_XSLT_AST_ATTRIBUTE_TEST,
                                                                     .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                                     .ast = test.ast,
                                                                     .document = request.assertion.document,
                                                                     .source_span = test.source_span});
            test.ast = NULL;
        }
        branch = condition;
        if (status == SXML_STATUS_OK && !is_report) {
            status = add_node(builder,
                              (node_request){.kind = SXML_XSLT_AST_KIND_OTHERWISE,
                                             .parent = choose,
                                             .place = request.assertion,
                                             .has_place = true,
                                             .is_declaration = false},
                              &branch);
        }
    }
    bool has_subject = false;
    if (status == SXML_STATUS_OK && branch.value != 0) {
        status = emit_subject(builder, (subject_request){.assertion = request.assertion,
                                                         .rule = request.rule,
                                                         .parent = branch,
                                                         .has_subject = &has_subject,
                                                         .entry = &compilation->table.entries[entry_index]});
    }
    sxml_xslt_ast_node_index array = {.value = 0};
    if (status == SXML_STATUS_OK && branch.value != 0) {
        status = add_node(builder,
                          (node_request){.kind = SXML_XSLT_AST_KIND_ARRAY,
                                         .parent = branch,
                                         .place = request.assertion,
                                         .has_place = true,
                                         .is_declaration = false},
                          &array);
    }
    sxml_byte_view text = no_text;
    if (status == SXML_STATUS_OK && array.value != 0) {
        status = emit_fixed_member(builder, array, number_text(builder, entry_index));
        if (status == SXML_STATUS_OK) {
            status = make_reference_text(
                builder, (reference_text){.before = "$", .name = builder->subject_name, .after = ""},
                &text);
        }
        if (status == SXML_STATUS_OK) {
            status = emit_fixed_member(builder, array, text);
        }
        if (status == SXML_STATUS_OK) {
            status = make_reference_text(builder,
                                         (reference_text){.before = "$",
                                                          .name = builder->subject_name,
                                                          .after = "[. instance of node()] ! path(.)"},
                                         &text);
        }
        if (status == SXML_STATUS_OK) {
            status = emit_fixed_member(builder, array, text);
        }
        for (size_t dynamic_index = 0; status == SXML_STATUS_OK && dynamic_index < 3;
             dynamic_index += 1) {
            if ((entry.dynamic_mask & bits[dynamic_index]) != 0) {
                status = emit_dynamic_member(builder, (dynamic_member){.array = array, .parsed = &dynamics[dynamic_index], .document = request.assertion.document});
            }
        }
        /* The message, with the message context (6.3): the subject when a
           subject attribute applies. */
        sxml_xslt_ast_node_index member = {.value = 0};
        if (status == SXML_STATUS_OK) {
            status = add_node(builder,
                              (node_request){.kind = SXML_XSLT_AST_KIND_ARRAY_MEMBER,
                                             .parent = array,
                                             .place = request.assertion,
                                             .has_place = true,
                                             .is_declaration = false},
                              &member);
        }
        sxml_xslt_ast_node_index message_parent = member;
        if (status == SXML_STATUS_OK && has_subject) {
            status = make_reference_text(
                builder, (reference_text){.before = "$", .name = builder->subject_name, .after = ""},
                &text);
            if (status == SXML_STATUS_OK) {
                status = add_fixed_node(builder,
                                        (fixed_node){.kind = SXML_XSLT_AST_KIND_FOR_EACH,
                                                     .parent = member,
                                                     .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                                     .text = text,
                                                     .document = request.assertion.document,
                                                     .origin = entry.source_span},
                                        &message_parent);
            }
        }
        if (status == SXML_STATUS_OK) {
            status = emit_message(builder, (message_request){.element = request.assertion,
                                                             .parent = message_parent,
                                                             .reads_copy_of = false,
                                                             .bindings = request.rule->bindings});
        }
        const sxml_schematron_array *annotation_places[2] = {&diagnostic_places, &property_places};
        for (size_t list_index = 0; status == SXML_STATUS_OK && list_index < 2; list_index += 1) {
            const sxml_schematron_identified *places = annotation_places[list_index]->elements;
            for (size_t place_index = 0;
                 status == SXML_STATUS_OK && place_index < annotation_places[list_index]->count;
                 place_index += 1) {
                sxml_xslt_ast_node_index annotation_member = {.value = 0};
                status = add_node(builder,
                                  (node_request){.kind = SXML_XSLT_AST_KIND_ARRAY_MEMBER,
                                                 .parent = array,
                                                 .place = places[place_index].place,
                                                 .has_place = true,
                                                 .is_declaration = false},
                                  &annotation_member);
                if (status == SXML_STATUS_OK) {
                    status = emit_message(builder,
                                          (message_request){.element = places[place_index].place,
                                                            .parent = annotation_member,
                                                            .reads_copy_of = list_index == 1 &&
                                                                             compilation->binding.is_xslt,
                                                            .bindings = request.rule->bindings});
                }
            }
        }
    }
    for (size_t dynamic_index = 0; dynamic_index < 3; dynamic_index += 1) {
        sxml_ast_destroy(dynamics[dynamic_index].ast);
    }
    sxml_ast_destroy(test.ast);
    sxml_schematron_array_release(compilation, &diagnostic_places);
    sxml_schematron_array_release(compilation, &property_places);
    return status;
}

/* --------------------------------------------------------------- Rules */

/* A rule context parsed (D315, the owner's amendment of 3a): an XSLT 4.0
   pattern, else an expression rule context; NULL both when in error. */
typedef struct parsed_context {
    sxml_ast *pattern;
    sxml_ast *expression;
    sxml_ast_source_span source_span;
} parsed_context;

/* A rule whose context to parse, and where the table keeps its text
   (NULL for none). */
typedef struct rule_context_request {
    sxml_schematron_place rule;
    sxml_schematron_text *table_text;
} rule_context_request;

static sxml_status parse_rule_context(schematron_builder *builder, rule_context_request request,
                                      parsed_context *parsed) {
    const sxml_schematron_place rule = request.rule;
    sxml_schematron_text *table_text = request.table_text;
    *parsed = (parsed_context){.pattern = NULL, .expression = NULL, .source_span = no_span};
    const sxml_schematron_attribute attribute =
        attribute_of(builder, rule, SXML_SCHEMATRON_ATTRIBUTE_CONTEXT);
    parsed->source_span = attribute.source_span;
    sxml_schematron_expansion expansion;
    sxml_status status = sxml_schematron_expand(
        builder->compilation->allocator,
        &(sxml_schematron_expansion_request){
            .text = attribute.value,
            .instance_parameters = builder->has_instance
                                       ? table_of(&builder->instance_parameters)
                                       : (sxml_schematron_parameter_table){.values = NULL, .value_count = 0},
            .schema_parameters = table_of(&builder->schema_parameters),
            .renamed_variables = table_of(&builder->renamed_variables)},
        &expansion);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_byte_view expanded = sxml_schematron_buffer_text(&expansion.text);
    if (table_text != NULL) {
        status = add_table_text(builder, sxml_xml_text_trim_whitespace(expanded), table_text);
    }
    const sxml_byte_view source_name = source_name_of(builder, rule.document);
    if (status == SXML_STATUS_OK) {
        status = parse_with_front_end(builder,
                                      (front_end_request){.value_kind = SXML_XSLT_AST_VALUE_KIND_PATTERN,
                                                          .text = expanded,
                                                          .source_name = source_name},
                                      &parsed->pattern);
    }
    sxml_ast **made = &parsed->pattern;
    if (status == SXML_STATUS_STATIC_ERROR) {
        const sxml_error_record pattern_error = builder->parse_record;
        status = parse_with_front_end(builder,
                                      (front_end_request){.value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                          .text = expanded,
                                                          .source_name = source_name},
                                      &parsed->expression);
        made = &parsed->expression;
        if (status == SXML_STATUS_STATIC_ERROR) {
            parsed->pattern = NULL;
            parsed->expression = NULL;
            /* The pattern parser's error under the xslt bindings, the
               expression parser's under the xpath bindings (D315). */
            if (builder->compilation->binding.is_xslt) {
                builder->parse_record = pattern_error;
            }
            status = report_expansion_error(builder, (expansion_error){.expansion = &expansion,
                                                                       .attribute_text = attribute.value,
                                                                       .place = rule,
                                                                       .attribute_span =
                                                                           attribute.source_span});
            made = NULL;
        }
    }
    if (status == SXML_STATUS_OK && made != NULL && *made != NULL) {
        if (expansion.has_placeholder) {
            place_spans_at(*made, attribute.source_span);
        } else {
            sxml_ast_move_source_spans(*made, attribute.source_span);
        }
    }
    sxml_schematron_expansion_release(&expansion);
    return status;
}

/* The match pattern of an expression rule context (D315 "Rule
   contexts"): ".[. intersect $s:context-K]" over a global holding the
   expression's value from each start node, or, in a pattern with
   documents, ".[. intersect s:context-K(root(.))]" over a function of the
   subordinate document's root. Takes expression. */
typedef struct expression_context_request {
    sxml_ast *expression;
    sxml_schematron_place rule;
    sxml_ast_source_span source_span;
    bool has_documents;
} expression_context_request;

static sxml_status make_expression_context(schematron_builder *builder, expression_context_request request,
                                           sxml_ast **pattern) {
    builder->context_count += 1;
    sxml_name_identifier context_name = {.index = 0};
    sxml_status status = intern_numbered_name(
        builder, (numbered_name){.stem = "context", .number = builder->context_count, .suffix = no_text},
        &context_name);
    sxml_xslt_ast_node_index declaration = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = add_node(builder,
                          (node_request){.kind = request.has_documents ? SXML_XSLT_AST_KIND_FUNCTION
                                                                       : SXML_XSLT_AST_KIND_VARIABLE,
                                         .parent = builder->root,
                                         .place = request.rule,
                                         .has_place = true,
                                         .is_declaration = true},
                          &declaration);
    }
    if (status == SXML_STATUS_OK) {
        status = add_name_attribute(builder, declaration, context_name);
    }
    if (status == SXML_STATUS_OK) {
        status = add_fixed_attribute(builder,
                                     (fixed_attribute){.node = declaration,
                                                       .kind = SXML_XSLT_AST_ATTRIBUTE_AS,
                                                       .value_kind = SXML_XSLT_AST_VALUE_KIND_SEQUENCE_TYPE,
                                                       .text = SXML_TEXT("node()*"),
                                                       .document = request.rule.document,
                                                       .origin = request.source_span});
    }
    sxml_byte_view text = no_text;
    if (status == SXML_STATUS_OK && request.has_documents) {
        sxml_xslt_ast_node_index parameter = {.value = 0};
        status = add_node(builder,
                          (node_request){.kind = SXML_XSLT_AST_KIND_PARAM,
                                         .parent = declaration,
                                         .place = request.rule,
                                         .has_place = true,
                                         .is_declaration = false},
                          &parameter);
        if (status == SXML_STATUS_OK) {
            status = add_name_attribute(builder, parameter, builder->root_name);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = make_reference_text(
            builder,
            (reference_text){.before = "$",
                             .name = request.has_documents ? builder->root_name : builder->start_name,
                             .after = ""},
            &text);
    }
    sxml_xslt_ast_node_index for_each = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = add_fixed_node(builder,
                                (fixed_node){.kind = SXML_XSLT_AST_KIND_FOR_EACH,
                                             .parent = declaration,
                                             .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                             .text = text,
                                             .document = request.rule.document,
                                             .origin = request.source_span},
                                &for_each);
    }
    sxml_xslt_ast_node_index sequence = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = add_node(builder,
                          (node_request){.kind = SXML_XSLT_AST_KIND_SEQUENCE,
                                         .parent = for_each,
                                         .place = request.rule,
                                         .has_place = true,
                                         .is_declaration = false},
                          &sequence);
    }
    if (status == SXML_STATUS_OK) {
        status = add_expression_attribute(builder,
                                          (expression_attribute){.node = sequence,
                                                                 .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                                                 .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                                 .ast = request.expression,
                                                                 .document = request.rule.document,
                                                                 .source_span = request.source_span});
    } else {
        sxml_ast_destroy(request.expression);
    }
    if (status == SXML_STATUS_OK) {
        status = request.has_documents
                     ? make_reference_text(builder,
                                           (reference_text){.before = ".[. intersect ",
                                                            .name = context_name,
                                                            .after = "(root(.))]"},
                                           &text)
                     : make_reference_text(builder,
                                           (reference_text){.before = ".[. intersect $",
                                                            .name = context_name,
                                                            .after = "]"},
                                           &text);
    }
    if (status == SXML_STATUS_OK) {
        status = parse_fixed(builder, (fixed_expression){.value_kind = SXML_XSLT_AST_VALUE_KIND_PATTERN,
                                                          .text = text,
                                                          .document = request.rule.document,
                                                          .origin = request.source_span},
                             pattern);
    }
    return status;
}

/* A rule to compile into a template rule of its pattern's mode. */
typedef struct rule_request {
    const compiled_pattern *pattern;
    sxml_schematron_place rule;
    /* Its position among the pattern's rules (from 1) and their count:
       priority count - position, so the first matching rule wins (6.3). */
    uint32_t rule_number;
    uint32_t rule_count;
    sxml_name_identifier mode;
    uint32_t table_pattern;
    sxml_namespace_binding_view bindings;
} rule_request;

/* The renamed variables of a pattern, bound to their written names at the
   start of each of its templates (3.23, D315). */
static sxml_status bind_pattern_variables(schematron_builder *builder, const rule_request *request,
                                          sxml_xslt_ast_node_index template_node) {
    const pattern_variable *variables = builder->pattern_variables.elements;
    sxml_status status = SXML_STATUS_OK;
    for (size_t variable_index = 0;
         status == SXML_STATUS_OK && variable_index < builder->pattern_variables.count;
         variable_index += 1) {
        const pattern_variable variable = variables[variable_index];
        if (variable.pattern != request->pattern->number ||
            variable.global_name.index == variable.written_name.index) {
            continue;
        }
        sxml_byte_view text = no_text;
        status = make_reference_text(
            builder, (reference_text){.before = "$", .name = variable.global_name, .after = ""}, &text);
        if (status == SXML_STATUS_OK) {
            status = add_fixed_variable(builder, (fixed_variable){.parent = template_node,
                                                                  .name = variable.written_name,
                                                                  .select = text,
                                                                  .document = 0,
                                                                  .origin = no_span});
        }
    }
    return status;
}

/* The fired-rule item (D315): if $s:fired-rules, [entry, ., path(.),
   dynamic role and flag]. */
typedef struct fired_rule_request {
    const rule_request *request;
    const rule_context *context;
    sxml_xslt_ast_node_index parent;
} fired_rule_request;

static sxml_status emit_fired_rule(schematron_builder *builder, fired_rule_request fired) {
    const rule_request *request = fired.request;
    const rule_context *context = fired.context;
    const sxml_xslt_ast_node_index parent = fired.parent;
    sxml_schematron_entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.entry_kind = SXML_SCHEMATRON_ENTRY_RULE;
    entry.pattern = request->table_pattern;
    entry.context = context->context_text;
    entry.visit_each = context->visit_each_text;
    sxml_status status = fill_entry(builder, request->rule, &entry);
    parsed_expression dynamics[2] = {{.is_present = false, .ast = NULL},
                                     {.is_present = false, .ast = NULL}};
    if (status == SXML_STATUS_OK) {
        status = read_dynamic_attribute(builder, (dynamic_request){.place = request->rule,
                                                                   .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_ROLE,
                                                                   .dynamic_bit = SXML_SCHEMATRON_DYNAMIC_ROLE,
                                                                   .entry = &entry,
                                                                   .static_text = &entry.role,
                                                                   .parsed = &dynamics[0]});
    }
    if (status == SXML_STATUS_OK) {
        status = read_dynamic_attribute(builder, (dynamic_request){.place = request->rule,
                                                                   .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_FLAG,
                                                                   .dynamic_bit = SXML_SCHEMATRON_DYNAMIC_FLAG,
                                                                   .entry = &entry,
                                                                   .static_text = &entry.flag,
                                                                   .parsed = &dynamics[1]});
    }
    /* The rule's title is SVRL's name of the fired rule (Annex D). */
    if (status == SXML_STATUS_OK) {
        sxml_schematron_cursor cursor = {.compilation = builder->compilation,
                                         .frames = {.elements = NULL, .count = 0, .capacity = 0},
                                         .expands_rules = false};
        status = sxml_schematron_cursor_start(&cursor, request->rule);
        sxml_xml_buffer title = sxml_xml_buffer_empty(builder->compilation->allocator);
        for (;;) {
            sxml_schematron_child child = {.has_child = false,
                                           .place = {.document = 0, .element = {.index = 0}},
                                           .element_kind = SXML_SCHEMATRON_ELEMENT_NONE,
                                           .is_inserted = false};
            if (status == SXML_STATUS_OK) {
                status = sxml_schematron_cursor_next(&cursor, &child);
            }
            if (status != SXML_STATUS_OK || !child.has_child) {
                break;
            }
            if (child.element_kind == SXML_SCHEMATRON_ELEMENT_TITLE && title.byte_count == 0) {
                status = append_element_text(builder, child.place, &title);
            }
        }
        if (status == SXML_STATUS_OK) {
            status = add_table_text(builder, sxml_xml_text_trim_whitespace(sxml_schematron_buffer_text(&title)),
                                    &entry.test);
        }
        sxml_xml_buffer_release(&title);
        sxml_schematron_cursor_finish(&cursor);
    }
    uint32_t entry_index = 0;
    if (status == SXML_STATUS_OK) {
        status = add_entry(builder, &entry, &entry_index);
    }
    sxml_byte_view text = no_text;
    sxml_xslt_ast_node_index condition = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = make_reference_text(
            builder, (reference_text){.before = "$", .name = builder->fired_rules_name, .after = ""},
            &text);
    }
    if (status == SXML_STATUS_OK) {
        status = add_fixed_node(builder,
                                (fixed_node){.kind = SXML_XSLT_AST_KIND_IF,
                                             .parent = parent,
                                             .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_TEST,
                                             .text = text,
                                             .document = 0,
                                             .origin = no_span},
                                &condition);
    }
    sxml_xslt_ast_node_index array = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = add_node(builder,
                          (node_request){.kind = SXML_XSLT_AST_KIND_ARRAY,
                                         .parent = condition,
                                         .place = request->rule,
                                         .has_place = true,
                                         .is_declaration = false},
                          &array);
    }
    if (status == SXML_STATUS_OK) {
        status = emit_fixed_member(builder, array, number_text(builder, entry_index));
    }
    if (status == SXML_STATUS_OK) {
        status = emit_fixed_member(builder, array, SXML_TEXT("."));
    }
    if (status == SXML_STATUS_OK) {
        status = emit_fixed_member(builder, array, SXML_TEXT(".[. instance of node()] ! path(.)"));
    }
    for (size_t dynamic_index = 0; status == SXML_STATUS_OK && dynamic_index < 2; dynamic_index += 1) {
        const uint32_t bit = dynamic_index == 0 ? SXML_SCHEMATRON_DYNAMIC_ROLE : SXML_SCHEMATRON_DYNAMIC_FLAG;
        if ((entry.dynamic_mask & bit) != 0) {
            status = emit_dynamic_member(builder, (dynamic_member){.array = array, .parsed = &dynamics[dynamic_index], .document = request->rule.document});
        }
    }
    sxml_ast_destroy(dynamics[0].ast);
    sxml_ast_destroy(dynamics[1].ast);
    return status;
}

static sxml_status emit_rule(schematron_builder *builder, const rule_request *request) {
    sxml_schematron_compilation *compilation = builder->compilation;
    /* The rule's lets and assertions in order, those of extends in place
       (6.2). */
    sxml_schematron_array lets = {.elements = NULL, .count = 0, .capacity = 0};
    sxml_schematron_array assertions = {.elements = NULL, .count = 0, .capacity = 0};
    sxml_schematron_cursor cursor = {.compilation = compilation,
                                     .frames = {.elements = NULL, .count = 0, .capacity = 0},
                                     .expands_rules = true};
    sxml_status status = sxml_schematron_cursor_start(&cursor, request->rule);
    for (;;) {
        sxml_schematron_child child = {.has_child = false,
                                       .place = {.document = 0, .element = {.index = 0}},
                                       .element_kind = SXML_SCHEMATRON_ELEMENT_NONE,
                                       .is_inserted = false};
        if (status == SXML_STATUS_OK) {
            status = sxml_schematron_cursor_next(&cursor, &child);
        }
        if (status != SXML_STATUS_OK || !child.has_child) {
            break;
        }
        if (child.element_kind == SXML_SCHEMATRON_ELEMENT_LET) {
            status = sxml_schematron_array_push(compilation, &lets, SXML_SCHEMATRON_ELEMENT_OF(child.place));
        } else if (child.element_kind == SXML_SCHEMATRON_ELEMENT_ASSERT ||
                   child.element_kind == SXML_SCHEMATRON_ELEMENT_REPORT) {
            status = sxml_schematron_array_push(compilation, &assertions,
                                                SXML_SCHEMATRON_ELEMENT_OF(child.place));
        }
    }
    sxml_schematron_cursor_finish(&cursor);
    rule_context context = {.rule = request->rule,
                            .pattern = request->table_pattern,
                            .context_text = {.byte_offset = 0, .byte_count = 0},
                            .visit_each_text = {.byte_offset = 0, .byte_count = 0},
                            .bindings = request->bindings};
    parsed_context match = {.pattern = NULL, .expression = NULL, .source_span = no_span};
    if (status == SXML_STATUS_OK) {
        status = parse_rule_context(builder, (rule_context_request){.rule = request->rule, .table_text = &context.context_text}, &match);
    }
    if (status == SXML_STATUS_OK && match.expression != NULL) {
        status = make_expression_context(builder,
                                         (expression_context_request){.expression = match.expression,
                                                                      .rule = request->rule,
                                                                      .source_span = match.source_span,
                                                                      .has_documents =
                                                                          request->pattern->has_documents},
                                         &match.pattern);
        match.expression = NULL;
    }
    parsed_expression visit_each = {.is_present = false, .ast = NULL, .source_span = no_span};
    if (status == SXML_STATUS_OK) {
        status = parse_attribute(builder,
                                 &(expression_request){.place = request->rule,
                                                       .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_VISIT_EACH,
                                                       .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                       .expands = true,
                                                       .table_text = &context.visit_each_text},
                                 &visit_each);
    }
    sxml_xslt_ast_node_index template_node = {.value = 0};
    if (status == SXML_STATUS_OK && match.pattern != NULL) {
        status = add_node(builder,
                          (node_request){.kind = SXML_XSLT_AST_KIND_TEMPLATE,
                                         .parent = builder->root,
                                         .place = request->rule,
                                         .has_place = true,
                                         .is_declaration = true},
                          &template_node);
        if (status == SXML_STATUS_OK) {
            status = add_expression_attribute(builder,
                                              (expression_attribute){.node = template_node,
                                                                     .kind = SXML_XSLT_AST_ATTRIBUTE_MATCH,
                                                                     .value_kind = SXML_XSLT_AST_VALUE_KIND_PATTERN,
                                                                     .ast = match.pattern,
                                                                     .document = request->rule.document,
                                                                     .source_span = match.source_span});
            match.pattern = NULL;
        }
        if (status == SXML_STATUS_OK) {
            status = add_mode_attribute(builder, template_node, mode_token(request->mode));
        }
        if (status == SXML_STATUS_OK) {
            status = add_value_attribute(builder, (value_attribute){.node = template_node, .kind = SXML_XSLT_AST_ATTRIBUTE_PRIORITY, .value = (sxml_xslt_ast_value){
                    .kind = SXML_XSLT_AST_VALUE_KIND_NUMBER,
                    .payload = {.number = (double)request->rule_count - (double)request->rule_number}}});
        }
    }
    /* Without a template (its context in error) the rule's expressions are
       still parsed, under a node no mode reaches. */
    sxml_xslt_ast_node_index body = template_node;
    if (status == SXML_STATUS_OK && body.value == 0) {
        status = add_node(builder,
                          (node_request){.kind = SXML_XSLT_AST_KIND_SEQUENCE,
                                         .parent = {.value = 0},
                                         .place = request->rule,
                                         .has_place = true,
                                         .is_declaration = false},
                          &body);
    }
    if (status == SXML_STATUS_OK) {
        status = bind_pattern_variables(builder, request, body);
    }
    const size_t pattern_scope_count = builder->local_variables.count;
    const sxml_schematron_place *let_places = lets.elements;
    for (size_t let_index = 0; status == SXML_STATUS_OK && let_index < lets.count; let_index += 1) {
        status = define_variable(builder,
                                 (variable_definition){.scope = &builder->local_variables,
                                                       .name = trimmed_of(builder, let_places[let_index],
                                                                          SXML_SCHEMATRON_ATTRIBUTE_NAME),
                                                       .place = let_places[let_index]});
        if (status == SXML_STATUS_OK) {
            status = emit_let(builder, (let_request){.place = let_places[let_index],
                                                     .parent = body,
                                                     .is_global = false,
                                                     .name = {.index = 0},
                                                     .bindings = request->bindings});
        }
    }
    builder->local_variables.count = pattern_scope_count;
    if (status == SXML_STATUS_OK) {
        status = add_fixed_variable(builder, (fixed_variable){.parent = body,
                                                              .name = builder->context_name,
                                                              .select = SXML_TEXT("."),
                                                              .document = 0,
                                                              .origin = no_span});
    }
    if (status == SXML_STATUS_OK) {
        status = emit_fired_rule(builder, (fired_rule_request){.request = request, .context = &context, .parent = body});
    }
    /* visit-each (5.5.21): its items are the assertions' context items;
       the rule's lets are evaluated once, before (the owner, 6b). */
    sxml_xslt_ast_node_index container = body;
    if (status == SXML_STATUS_OK && visit_each.ast != NULL) {
        status = add_node(builder,
                          (node_request){.kind = SXML_XSLT_AST_KIND_FOR_EACH,
                                         .parent = body,
                                         .place = request->rule,
                                         .has_place = true,
                                         .is_declaration = false},
                          &container);
        if (status == SXML_STATUS_OK) {
            status = add_expression_attribute(builder,
                                              (expression_attribute){.node = container,
                                                                     .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                                                     .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                                     .ast = visit_each.ast,
                                                                     .document = request->rule.document,
                                                                     .source_span = visit_each.source_span});
            visit_each.ast = NULL;
        }
    }
    const sxml_schematron_place *assertion_places = assertions.elements;
    for (size_t assertion_index = 0; status == SXML_STATUS_OK && assertion_index < assertions.count;
         assertion_index += 1) {
        status = emit_assertion(builder, (assertion_request){.assertion = assertion_places[assertion_index],
                                                             .rule = &context,
                                                             .parent = container});
    }
    if (status == SXML_STATUS_OK && template_node.value != 0) {
        sxml_xslt_ast_node_index walk = {.value = 0};
        if (request->pattern->is_group) {
            /* 5.4.5, 6.6: in a group every matching rule fires. */
            status = add_node(builder,
                              (node_request){.kind = SXML_XSLT_AST_KIND_NEXT_MATCH,
                                             .parent = template_node,
                                             .place = {.document = 0, .element = {.index = 0}},
                                             .has_place = false,
                                             .is_declaration = false},
                              &walk);
        } else {
            status = add_fixed_node(builder,
                                    (fixed_node){.kind = SXML_XSLT_AST_KIND_APPLY_TEMPLATES,
                                                 .parent = template_node,
                                                 .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                                 .text = SXML_TEXT("@* | node()"),
                                                 .document = 0,
                                                 .origin = no_span},
                                    &walk);
            if (status == SXML_STATUS_OK) {
                status = add_mode_attribute(
                    builder, walk,
                    (sxml_xslt_ast_name_token){.kind = SXML_XSLT_AST_NAME_TOKEN_KIND_CURRENT,
                                               .name = {.index = 0},
                                               .namespace_uri = {.index = 0},
                                               .has_arity = false,
                                               .arity = 0});
            }
        }
    }
    sxml_ast_destroy(match.pattern);
    sxml_ast_destroy(match.expression);
    sxml_ast_destroy(visit_each.ast);
    sxml_schematron_array_release(compilation, &lets);
    sxml_schematron_array_release(compilation, &assertions);
    return status;
}

/* ------------------------------------------------------------ Patterns */

/* A pattern's title (SVRL's name), as text. */
static sxml_status title_text(schematron_builder *builder, sxml_schematron_place place, sxml_schematron_text *text) {
    sxml_schematron_cursor cursor = {.compilation = builder->compilation,
                                     .frames = {.elements = NULL, .count = 0, .capacity = 0},
                                     .expands_rules = false};
    sxml_status status = sxml_schematron_cursor_start(&cursor, place);
    sxml_xml_buffer title = sxml_xml_buffer_empty(builder->compilation->allocator);
    *text = (sxml_schematron_text){.byte_offset = 0, .byte_count = 0};
    for (;;) {
        sxml_schematron_child child = {.has_child = false,
                                       .place = {.document = 0, .element = {.index = 0}},
                                       .element_kind = SXML_SCHEMATRON_ELEMENT_NONE,
                                       .is_inserted = false};
        if (status == SXML_STATUS_OK) {
            status = sxml_schematron_cursor_next(&cursor, &child);
        }
        if (status != SXML_STATUS_OK || !child.has_child) {
            break;
        }
        if (child.element_kind == SXML_SCHEMATRON_ELEMENT_TITLE && title.byte_count == 0) {
            status = append_element_text(builder, child.place, &title);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = add_table_text(builder, sxml_xml_text_trim_whitespace(sxml_schematron_buffer_text(&title)), text);
    }
    sxml_xml_buffer_release(&title);
    sxml_schematron_cursor_finish(&cursor);
    return status;
}

/* 5.4.12: the pattern's own variables are not in scope in documents: a
   reference to one is XPST0008 (D315). */
static sxml_status check_documents_variables(schematron_builder *builder, const sxml_ast *ast,
                                             sxml_schematron_place pattern) {
    for (uint32_t node_number = 1; node_number < ast->node_count; node_number += 1) {
        if (ast->nodes[node_number].kind != SXML_AST_KIND_VARIABLE_REFERENCE) {
            continue;
        }
        const sxml_ast_node *name = sxml_ast_get_node(ast, ast->nodes[node_number].first_child);
        if (name->kind != SXML_AST_KIND_NCNAME) {
            continue;
        }
        const sxml_byte_view local_name = sxml_ast_get_text(ast, name->payload.name.local_name);
        const scoped_variable *variables = builder->local_variables.elements;
        for (size_t variable_index = 0; variable_index < builder->local_variables.count;
             variable_index += 1) {
            if (sxml_byte_view_equals(variables[variable_index].name, local_name)) {
                return report_at(builder,
                                 (builder_error){.code = "XPST0008",
                                                 .description = "a pattern's variable is not in scope "
                                                                "in its documents attribute (5.4.12)",
                                                 .place = pattern,
                                                 .attribute_span = attribute_of(builder, pattern,
                                                                                SXML_SCHEMATRON_ATTRIBUTE_DOCUMENTS)
                                                                       .source_span,
                                                 .subject = local_name});
            }
        }
    }
    return SXML_STATUS_OK;
}

/* The traversal template rules of a mode (D315): document and element
   nodes walk their attributes and children, other nodes stop; priority
   -1, below every rule. */
static sxml_status emit_traversal(schematron_builder *builder, sxml_name_identifier mode) {
    static const char *const matches[] = {"document-node() | element()",
                                          "attribute() | text() | comment() | processing-instruction()"};
    sxml_status status = SXML_STATUS_OK;
    for (size_t match_index = 0; status == SXML_STATUS_OK && match_index < 2; match_index += 1) {
        sxml_xslt_ast_node_index template_node = {.value = 0};
        status = add_node(builder,
                          (node_request){.kind = SXML_XSLT_AST_KIND_TEMPLATE,
                                         .parent = builder->root,
                                         .place = {.document = 0, .element = {.index = 0}},
                                         .has_place = false,
                                         .is_declaration = true},
                          &template_node);
        if (status == SXML_STATUS_OK) {
            status = add_fixed_attribute(builder,
                                         (fixed_attribute){.node = template_node,
                                                           .kind = SXML_XSLT_AST_ATTRIBUTE_MATCH,
                                                           .value_kind = SXML_XSLT_AST_VALUE_KIND_PATTERN,
                                                           .text = sxml_byte_view_from_c_string(matches[match_index]),
                                                           .document = 0,
                                                           .origin = no_span});
        }
        if (status == SXML_STATUS_OK) {
            status = add_mode_attribute(builder, template_node, mode_token(mode));
        }
        if (status == SXML_STATUS_OK) {
            status = add_value_attribute(builder, (value_attribute){.node = template_node, .kind = SXML_XSLT_AST_ATTRIBUTE_PRIORITY, .value = (sxml_xslt_ast_value){.kind = SXML_XSLT_AST_VALUE_KIND_NUMBER,
                                                               .payload = {.number = -1.0}}});
        }
        if (status == SXML_STATUS_OK && match_index == 0) {
            sxml_xslt_ast_node_index walk = {.value = 0};
            status = add_fixed_node(builder,
                                    (fixed_node){.kind = SXML_XSLT_AST_KIND_APPLY_TEMPLATES,
                                                 .parent = template_node,
                                                 .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                                 .text = SXML_TEXT("@* | node()"),
                                                 .document = 0,
                                                 .origin = no_span},
                                    &walk);
            if (status == SXML_STATUS_OK) {
                status = add_mode_attribute(
                    builder, walk,
                    (sxml_xslt_ast_name_token){.kind = SXML_XSLT_AST_NAME_TOKEN_KIND_CURRENT,
                                               .name = {.index = 0},
                                               .namespace_uri = {.index = 0},
                                               .has_arity = false,
                                               .arity = 0});
            }
        }
    }
    return status;
}

/* The lets of a pattern (or of an instance's abstract pattern): their
   places, in order. */
/* The effective children of a kind to collect. */
typedef struct children_request {
    sxml_schematron_place parent;
    sxml_schematron_element_kind kind;
} children_request;

static sxml_status collect_children(schematron_builder *builder, children_request request,
                                    sxml_schematron_array *places) {
    const sxml_schematron_place parent = request.parent;
    const sxml_schematron_element_kind kind = request.kind;
    sxml_schematron_cursor cursor = {.compilation = builder->compilation,
                                     .frames = {.elements = NULL, .count = 0, .capacity = 0},
                                     .expands_rules = false};
    sxml_status status = sxml_schematron_cursor_start(&cursor, parent);
    places->count = 0;
    for (;;) {
        sxml_schematron_child child = {.has_child = false,
                                       .place = {.document = 0, .element = {.index = 0}},
                                       .element_kind = SXML_SCHEMATRON_ELEMENT_NONE,
                                       .is_inserted = false};
        if (status == SXML_STATUS_OK) {
            status = sxml_schematron_cursor_next(&cursor, &child);
        }
        if (status != SXML_STATUS_OK || !child.has_child) {
            break;
        }
        const bool is_wanted =
            child.element_kind == kind &&
            !(kind == SXML_SCHEMATRON_ELEMENT_RULE &&
              sxml_byte_view_equals(trimmed_of(builder, child.place, SXML_SCHEMATRON_ATTRIBUTE_ABSTRACT),
                                    SXML_TEXT("true")));
        if (is_wanted) {
            status = sxml_schematron_array_push(builder->compilation, places,
                                                SXML_SCHEMATRON_ELEMENT_OF(child.place));
        }
    }
    sxml_schematron_cursor_finish(&cursor);
    return status;
}

/* One compiled pattern: its lets as globals, its mode's traversal
   templates and one template rule per rule (D315). */
/* Fills builder->renamed_variables with the variables of a pattern whose
   globals have synthesized names (another compiled pattern defines the
   same name): its expressions, the rule contexts and the lets that are
   globals among them, then refer to the pattern's own (3.23). */
static sxml_status collect_renamed_variables(schematron_builder *builder, uint32_t pattern_number) {
    sxml_schematron_compilation *compilation = builder->compilation;
    const pattern_variable *variables = builder->pattern_variables.elements;
    builder->renamed_variables.count = 0;
    builder->renamed_text.byte_count = 0;
    sxml_status status = SXML_STATUS_OK;
    /* Two passes: the texts first, as the buffer may move while it
       grows; then the views into it. */
    for (int pass_number = 0; pass_number < 2 && status == SXML_STATUS_OK; pass_number += 1) {
        size_t offset = 0;
        for (size_t variable_index = 0;
             status == SXML_STATUS_OK && variable_index < builder->pattern_variables.count;
             variable_index += 1) {
            const pattern_variable variable = variables[variable_index];
            if (variable.pattern != pattern_number ||
                variable.global_name.index == variable.written_name.index) {
                continue;
            }
            sxml_name_parts parts = {.namespace_uri = {.index = 0}, .local_name = no_text};
            status = sxml_name_pool_get_name_parts(compilation->name_pool, variable.written_name, &parts);
            if (status != SXML_STATUS_OK || parts.namespace_uri.index != 0) {
                continue;
            }
            sxml_byte_view text = no_text;
            status = make_reference_text(
                builder, (reference_text){.before = "$", .name = variable.global_name, .after = ""}, &text);
            if (status != SXML_STATUS_OK) {
                break;
            }
            if (pass_number == 0) {
                status = sxml_xml_buffer_append(&builder->renamed_text, text);
                continue;
            }
            const sxml_byte_view stored = sxml_schematron_buffer_text(&builder->renamed_text);
            const sxml_schematron_parameter_value value = {
                .name = parts.local_name,
                .value = {.bytes = stored.bytes + offset, .byte_count = text.byte_count},
                .document = 0,
                .source_span = no_span,
                .is_configured = true};
            offset += text.byte_count;
            status = sxml_schematron_array_push(compilation, &builder->renamed_variables,
                                                SXML_SCHEMATRON_ELEMENT_OF(value));
        }
    }
    return status;
}

static sxml_status emit_pattern(schematron_builder *builder, const compiled_pattern *pattern,
                                sxml_namespace_binding_view bindings) {
    sxml_schematron_compilation *compilation = builder->compilation;
    sxml_status status = collect_instance_parameters(builder, pattern);
    if (status == SXML_STATUS_OK) {
        status = collect_renamed_variables(builder, pattern->number);
    }
    sxml_name_identifier mode = {.index = 0};
    if (status == SXML_STATUS_OK) {
        status = intern_numbered_name(
            builder, (numbered_name){.stem = "pattern", .number = pattern->number, .suffix = no_text},
            &mode);
    }
    /* The table's pattern. */
    sxml_schematron_table *table = &compilation->table;
    sxml_schematron_pattern_entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.is_group = pattern->is_group;
    if (status == SXML_STATUS_OK) {
        status = add_table_text(builder, trimmed_of(builder, pattern->place, SXML_SCHEMATRON_ATTRIBUTE_ID),
                                &entry.identifier);
    }
    if (status == SXML_STATUS_OK) {
        status = title_text(builder, pattern->place, &entry.name);
    }
    if (status == SXML_STATUS_OK && entry.name.byte_count == 0 && pattern->is_instance) {
        status = title_text(builder, pattern->rules_place, &entry.name);
    }
    if (status == SXML_STATUS_OK) {
        status = add_table_text(builder, trimmed_of(builder, pattern->place, SXML_SCHEMATRON_ATTRIBUTE_ROLE),
                                &entry.role);
    }
    if (status == SXML_STATUS_OK) {
        status = SXML_GROW(builder, table->patterns, table->pattern_count, sxml_schematron_pattern_entry);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    table->patterns[table->pattern_count - 1U] = entry;
    const uint32_t table_pattern = (uint32_t)(table->pattern_count - 1U);
    /* Its lets: globals under synthesized names (3.23). */
    builder->local_variables.count = 0;
    sxml_schematron_array places = {.elements = NULL, .count = 0, .capacity = 0};
    status = collect_children(builder, (children_request){.parent = pattern->rules_place, .kind = SXML_SCHEMATRON_ELEMENT_LET}, &places);
    const sxml_schematron_place *let_places = places.elements;
    for (size_t let_index = 0; status == SXML_STATUS_OK && let_index < places.count; let_index += 1) {
        const sxml_schematron_place let = let_places[let_index];
        const sxml_byte_view written = trimmed_of(builder, let, SXML_SCHEMATRON_ATTRIBUTE_NAME);
        status = define_variable(builder, (variable_definition){.scope = &builder->local_variables,
                                                                .name = written,
                                                                .place = let});
        sxml_name_identifier written_name = {.index = 0};
        if (status == SXML_STATUS_OK &&
            resolve_variable_name(builder, (written_variable){.text = written, .bindings = bindings}, &written_name) != SXML_STATUS_OK) {
            written_name.index = 0;
        }
        sxml_name_identifier global_name = written_name;
        const pattern_variable *variables = builder->pattern_variables.elements;
        for (size_t variable_index = 0; status == SXML_STATUS_OK &&
                                        variable_index < builder->pattern_variables.count;
             variable_index += 1) {
            if (variables[variable_index].pattern == pattern->number &&
                variables[variable_index].written_name.index == written_name.index) {
                global_name = variables[variable_index].global_name;
            }
        }
        if (status == SXML_STATUS_OK) {
            status = emit_let(builder, (let_request){.place = let,
                                                     .parent = builder->root,
                                                     .is_global = true,
                                                     .name = global_name,
                                                     .bindings = bindings});
        }
    }
    if (status == SXML_STATUS_OK && pattern->has_documents) {
        /* The instance's documents, else its abstract pattern's (6.4),
           placeholders replaced: the table's text (SVRL). The pattern's
           variables are not in scope there (5.4.12), so they are not
           renamed. */
        const sxml_schematron_place documents_place =
            attribute_of(builder, pattern->place, SXML_SCHEMATRON_ATTRIBUTE_DOCUMENTS).is_present
                ? pattern->place
                : pattern->rules_place;
        const size_t renamed_count = builder->renamed_variables.count;
        builder->renamed_variables.count = 0;
        sxml_schematron_text documents_text = {.byte_offset = 0, .byte_count = 0};
        parsed_expression documents = {.is_present = false, .ast = NULL, .source_span = no_span};
        status = parse_attribute(builder,
                                 &(expression_request){.place = documents_place,
                                                       .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_DOCUMENTS,
                                                       .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                       .expands = true,
                                                       .table_text = &documents_text},
                                 &documents);
        builder->renamed_variables.count = renamed_count;
        table->patterns[table_pattern].documents = documents_text;
        if (status == SXML_STATUS_OK && documents.ast != NULL) {
            status = check_documents_variables(builder, documents.ast, documents_place);
        }
        sxml_ast_destroy(documents.ast);
    }
    if (status == SXML_STATUS_OK) {
        status = emit_traversal(builder, mode);
    }
    if (status == SXML_STATUS_OK) {
        status = collect_children(builder, (children_request){.parent = pattern->rules_place, .kind = SXML_SCHEMATRON_ELEMENT_RULE}, &places);
    }
    const sxml_schematron_place *rule_places = places.elements;
    const uint32_t rule_count = (uint32_t)places.count;
    for (uint32_t rule_index = 0; status == SXML_STATUS_OK && rule_index < rule_count; rule_index += 1) {
        status = emit_rule(builder, &(rule_request){.pattern = pattern,
                                                    .rule = rule_places[rule_index],
                                                    .rule_number = rule_index + 1U,
                                                    .rule_count = rule_count,
                                                    .mode = mode,
                                                    .table_pattern = table_pattern,
                                                    .bindings = bindings});
    }
    sxml_schematron_array_release(compilation, &places);
    return status;
}

/* The lets of the compiled patterns become globals under synthesized
   names, so that a pattern's variables are in scope in that pattern only
   (3.23, 5.4.7; D315, D323): its expressions refer to them through
   builder->renamed_variables, its templates through local bindings. */
static sxml_status plan_pattern_variables(schematron_builder *builder, sxml_namespace_binding_view bindings) {
    sxml_schematron_compilation *compilation = builder->compilation;
    const compiled_pattern *patterns = builder->patterns.elements;
    sxml_schematron_array places = {.elements = NULL, .count = 0, .capacity = 0};
    sxml_status status = SXML_STATUS_OK;
    for (size_t pattern_index = 0; status == SXML_STATUS_OK && pattern_index < builder->patterns.count;
         pattern_index += 1) {
        if (patterns[pattern_index].number == 0) {
            continue;
        }
        status = collect_children(builder, (children_request){.parent = patterns[pattern_index].rules_place, .kind = SXML_SCHEMATRON_ELEMENT_LET}, &places);
        const sxml_schematron_place *let_places = places.elements;
        for (size_t let_index = 0; status == SXML_STATUS_OK && let_index < places.count; let_index += 1) {
            sxml_name_identifier name = {.index = 0};
            if (resolve_variable_name(builder, (written_variable){.text = trimmed_of(builder, let_places[let_index], SXML_SCHEMATRON_ATTRIBUTE_NAME), .bindings = bindings}, &name) != SXML_STATUS_OK) {
                continue;
            }
            const pattern_variable variable = {.pattern = patterns[pattern_index].number,
                                               .written_name = name,
                                               .global_name = name};
            status = sxml_schematron_array_push(compilation, &builder->pattern_variables,
                                                SXML_SCHEMATRON_ELEMENT_OF(variable));
        }
    }
    sxml_schematron_array_release(compilation, &places);
    pattern_variable *variables = builder->pattern_variables.elements;
    for (size_t variable_index = 0; status == SXML_STATUS_OK &&
                                    variable_index < builder->pattern_variables.count;
         variable_index += 1) {
        sxml_name_parts parts = {.namespace_uri = {.index = 0}, .local_name = no_text};
        status = sxml_name_pool_get_name_parts(compilation->name_pool,
                                               variables[variable_index].written_name, &parts);
        if (status == SXML_STATUS_OK) {
            status = intern_numbered_name(builder,
                                          (numbered_name){.stem = "pattern",
                                                          .number = variables[variable_index].pattern,
                                                          .suffix = parts.local_name},
                                          &variables[variable_index].global_name);
        }
    }
    return status;
}

/* -------------------------------------------------------------- Phases */

/* The patterns a phase makes active, as compiled pattern numbers, in
   schema order (5.4.13). */
static sxml_status phase_patterns(schematron_builder *builder, sxml_schematron_place phase,
                                  sxml_schematron_array *numbers) {
    sxml_schematron_array actives = {.elements = NULL, .count = 0, .capacity = 0};
    sxml_status status = collect_children(builder, (children_request){.parent = phase, .kind = SXML_SCHEMATRON_ELEMENT_ACTIVE}, &actives);
    const compiled_pattern *patterns = builder->patterns.elements;
    const sxml_schematron_place *active_places = actives.elements;
    numbers->count = 0;
    for (size_t pattern_index = 0; status == SXML_STATUS_OK && pattern_index < builder->patterns.count;
         pattern_index += 1) {
        const sxml_byte_view identifier =
            trimmed_of(builder, patterns[pattern_index].place, SXML_SCHEMATRON_ATTRIBUTE_ID);
        bool is_active = false;
        for (size_t active_index = 0; active_index < actives.count; active_index += 1) {
            if (identifier.byte_count > 0 &&
                sxml_byte_view_equals(identifier, trimmed_of(builder, active_places[active_index],
                                                             SXML_SCHEMATRON_ATTRIBUTE_PATTERN))) {
                is_active = true;
            }
        }
        if (is_active) {
            const uint32_t number = (uint32_t)pattern_index + 1U;
            status = sxml_schematron_array_push(builder->compilation, numbers,
                                                SXML_SCHEMATRON_ELEMENT_OF(number));
        }
    }
    sxml_schematron_array_release(builder->compilation, &actives);
    return status;
}

/* The phase compiled (D315 "The path", step 2): its id, whether every
   pattern is compiled, and the phases "#ANY" chooses from. */
static sxml_status choose_phase(schematron_builder *builder) {
    sxml_schematron_compilation *compilation = builder->compilation;
    const sxml_schematron_index *index = &compilation->index;
    sxml_byte_view phase = sxml_xml_text_trim_whitespace(compilation->configuration->phase_id);
    if (phase.byte_count == 0 || sxml_byte_view_equals(phase, SXML_TEXT("#DEFAULT"))) {
        const sxml_byte_view default_phase =
            trimmed_of(builder, index->schema, SXML_SCHEMATRON_ATTRIBUTE_DEFAULT_PHASE);
        phase = default_phase.byte_count > 0 ? default_phase : SXML_TEXT("#ALL");
    }
    compilation->phase_id = phase;
    builder->is_all = sxml_byte_view_equals(phase, SXML_TEXT("#ALL")) ||
                      sxml_byte_view_equals(phase, SXML_TEXT("#ANY"));
    builder->selects_phase = sxml_byte_view_equals(phase, SXML_TEXT("#ANY"));
    const sxml_schematron_identified *phases = index->phases.elements;
    sxml_status status = SXML_STATUS_OK;
    if (builder->selects_phase) {
        for (size_t phase_index = 0; status == SXML_STATUS_OK && phase_index < index->phases.count;
             phase_index += 1) {
            if (attribute_of(builder, phases[phase_index].place, SXML_SCHEMATRON_ATTRIBUTE_WHEN).is_present) {
                status = sxml_schematron_array_push(compilation, &builder->chosen_phases,
                                                    SXML_SCHEMATRON_ELEMENT_OF(phases[phase_index]));
            }
        }
        return status;
    }
    if (builder->is_all) {
        return SXML_STATUS_OK;
    }
    const sxml_schematron_identified *chosen = sxml_schematron_find_identified(&index->phases, phase);
    if (chosen == NULL) {
        status = sxml_schematron_report_error(
            compilation,
            &(sxml_schematron_error){.code = "phase",
                                     .is_xpath = false,
                                     .description = "the schema has no phase of this id (5.4.13)",
                                     .subject = phase,
                                     .document = 0,
                                     .source_span = sxml_schematron_element_span(compilation, index->schema)});
        return status == SXML_STATUS_OK ? SXML_STATUS_STATIC_ERROR : status;
    }
    return sxml_schematron_array_push(compilation, &builder->chosen_phases,
                                      SXML_SCHEMATRON_ELEMENT_OF(*chosen));
}

/* The schema's patterns: every one that is not abstract, numbered when
   compiled (the phase can make it active). */
static sxml_status list_patterns(schematron_builder *builder) {
    sxml_schematron_compilation *compilation = builder->compilation;
    const sxml_schematron_place *places = compilation->index.patterns.elements;
    sxml_status status = SXML_STATUS_OK;
    sxml_schematron_array active = {.elements = NULL, .count = 0, .capacity = 0};
    for (size_t place_index = 0; status == SXML_STATUS_OK && place_index < compilation->index.patterns.count;
         place_index += 1) {
        const sxml_schematron_place place = places[place_index];
        if (sxml_byte_view_equals(trimmed_of(builder, place, SXML_SCHEMATRON_ATTRIBUTE_ABSTRACT),
                                  SXML_TEXT("true"))) {
            continue;
        }
        compiled_pattern pattern = {
            .place = place,
            .rules_place = place,
            .number = 0,
            .is_instance = false,
            .is_group = sxml_schematron_element_kind_of(compilation, place) == SXML_SCHEMATRON_ELEMENT_GROUP,
            .has_documents = attribute_of(builder, place, SXML_SCHEMATRON_ATTRIBUTE_DOCUMENTS).is_present};
        const sxml_byte_view is_a = trimmed_of(builder, place, SXML_SCHEMATRON_ATTRIBUTE_IS_A);
        if (attribute_of(builder, place, SXML_SCHEMATRON_ATTRIBUTE_IS_A).is_present) {
            const sxml_schematron_identified *abstract_pattern =
                sxml_schematron_find_identified(&compilation->index.abstract_patterns, is_a);
            if (abstract_pattern == NULL) {
                /* schematron:reference, reported by reading. */
                continue;
            }
            pattern.is_instance = true;
            pattern.rules_place = abstract_pattern->place;
            /* The abstract pattern's kind and documents decide (6.4). */
            pattern.is_group = sxml_schematron_element_kind_of(compilation, abstract_pattern->place) ==
                               SXML_SCHEMATRON_ELEMENT_GROUP;
            pattern.has_documents = pattern.has_documents ||
                                    attribute_of(builder, abstract_pattern->place,
                                                 SXML_SCHEMATRON_ATTRIBUTE_DOCUMENTS)
                                        .is_present;
        }
        status = sxml_schematron_array_push(compilation, &builder->patterns,
                                            SXML_SCHEMATRON_ELEMENT_OF(pattern));
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    compiled_pattern *patterns = builder->patterns.elements;
    if (!builder->is_all && builder->chosen_phases.count == 1) {
        const sxml_schematron_identified *phase = builder->chosen_phases.elements;
        status = phase_patterns(builder, phase->place, &active);
    }
    uint32_t number = 0;
    const uint32_t *active_numbers = active.elements;
    for (size_t pattern_index = 0; status == SXML_STATUS_OK && pattern_index < builder->patterns.count;
         pattern_index += 1) {
        bool is_compiled = builder->is_all;
        for (size_t active_index = 0; active_index < active.count; active_index += 1) {
            if (active_numbers[active_index] == pattern_index + 1U) {
                is_compiled = true;
            }
        }
        if (is_compiled) {
            number += 1;
            patterns[pattern_index].number = number;
        }
    }
    sxml_schematron_array_release(compilation, &active);
    return status;
}

/* The compiled pattern of a number (from 1). */
static const compiled_pattern *pattern_of_number(const schematron_builder *builder, uint32_t number) {
    const compiled_pattern *patterns = builder->patterns.elements;
    for (size_t pattern_index = 0; pattern_index < builder->patterns.count; pattern_index += 1) {
        if (patterns[pattern_index].number == number) {
            return &patterns[pattern_index];
        }
    }
    return NULL;
}

/* A global with a fixed select, or one whose content the caller adds. */
typedef struct global_request {
    sxml_name_identifier name;
    sxml_byte_view select;
    /* With no select: as, then the content under *node. */
    sxml_byte_view as;
    sxml_xslt_ast_kind kind;
} global_request;

static sxml_status add_global(schematron_builder *builder, global_request request, sxml_xslt_ast_node_index *node) {
    sxml_status status = add_node(builder,
                                  (node_request){.kind = request.kind,
                                                 .parent = builder->root,
                                                 .place = {.document = 0, .element = {.index = 0}},
                                                 .has_place = false,
                                                 .is_declaration = true},
                                  node);
    if (status == SXML_STATUS_OK) {
        status = add_name_attribute(builder, *node, request.name);
    }
    if (status == SXML_STATUS_OK && request.select.byte_count > 0) {
        status = add_fixed_attribute(builder,
                                     (fixed_attribute){.node = *node,
                                                       .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                                       .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                       .text = request.select,
                                                       .document = 0,
                                                       .origin = no_span});
    }
    if (status == SXML_STATUS_OK && request.as.byte_count > 0) {
        status = add_fixed_attribute(builder,
                                     (fixed_attribute){.node = *node,
                                                       .kind = SXML_XSLT_AST_ATTRIBUTE_AS,
                                                       .value_kind = SXML_XSLT_AST_VALUE_KIND_SEQUENCE_TYPE,
                                                       .text = request.as,
                                                       .document = 0,
                                                       .origin = no_span});
    }
    return status;
}

/* The start nodes of phase number (from 1; 0 for none): its from's nodes
   less those inside another (5.5.7, D315), or the instance root. */
static sxml_status start_text(schematron_builder *builder, uint32_t phase_number, sxml_byte_view *text) {
    const sxml_schematron_identified *phases = builder->chosen_phases.elements;
    if (phase_number == 0 ||
        !attribute_of(builder, phases[phase_number - 1U].place, SXML_SCHEMATRON_ATTRIBUTE_FROM).is_present) {
        *text = SXML_TEXT(".");
        return SXML_STATUS_OK;
    }
    sxml_name_identifier from = {.index = 0};
    sxml_status status = intern_numbered_name(
        builder, (numbered_name){.stem = "from", .number = phase_number, .suffix = no_text}, &from);
    builder->scratch.byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append_byte(&builder->scratch, '$');
    }
    if (status == SXML_STATUS_OK) {
        status = append_eqname(builder, &builder->scratch, from);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append(&builder->scratch, SXML_TEXT(" except $"));
    }
    if (status == SXML_STATUS_OK) {
        status = append_eqname(builder, &builder->scratch, from);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_buffer_append(&builder->scratch, SXML_TEXT("/descendant::node()"));
    }
    *text = sxml_schematron_buffer_text(&builder->scratch);
    return status;
}

/* "$Q{...}phase eq N" in builder->scratch. */
static sxml_status phase_test_text(schematron_builder *builder, uint32_t phase_number, sxml_byte_view *text) {
    char digits[24];
    const int written = snprintf(digits, sizeof(digits), " eq %u", phase_number);
    builder->scratch.byte_count = 0;
    sxml_status status = sxml_xml_buffer_append_byte(&builder->scratch, '$');
    if (status == SXML_STATUS_OK) {
        status = append_eqname(builder, &builder->scratch, builder->phase_name);
    }
    if (status == SXML_STATUS_OK && written > 0) {
        status = sxml_xml_buffer_append(&builder->scratch, (sxml_byte_view){.bytes = (const uint8_t *)digits,
                                                                           .byte_count = (size_t)written});
    }
    *text = sxml_schematron_buffer_text(&builder->scratch);
    return status;
}

/* A sequence node with a fixed select under parent. */
static sxml_status add_fixed_sequence(schematron_builder *builder, sxml_xslt_ast_node_index parent,
                                      sxml_byte_view select) {
    sxml_xslt_ast_node_index node = {.value = 0};
    return add_fixed_node(builder,
                          (fixed_node){.kind = SXML_XSLT_AST_KIND_SEQUENCE,
                                       .parent = parent,
                                       .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                       .text = select,
                                       .document = 0,
                                       .origin = no_span},
                          &node);
}

/* The globals of the phases: each from (5.5.7), "#ANY"'s choice
   (5.5.22), the phases' lets (5.4.7), and the start nodes of expression
   rule contexts (D315). */
static sxml_status emit_phases(schematron_builder *builder, sxml_namespace_binding_view bindings) {
    sxml_schematron_compilation *compilation = builder->compilation;
    const sxml_schematron_identified *phases = builder->chosen_phases.elements;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t phase_index = 0; status == SXML_STATUS_OK && phase_index < builder->chosen_phases.count;
         phase_index += 1) {
        parsed_expression from = {.is_present = false, .ast = NULL, .source_span = no_span};
        status = parse_attribute(builder,
                                 &(expression_request){.place = phases[phase_index].place,
                                                       .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_FROM,
                                                       .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                       .expands = true,
                                                       .table_text = NULL},
                                 &from);
        if (status != SXML_STATUS_OK || from.ast == NULL) {
            continue;
        }
        sxml_name_identifier name = {.index = 0};
        status = intern_numbered_name(
            builder, (numbered_name){.stem = "from", .number = phase_index + 1U, .suffix = no_text}, &name);
        sxml_xslt_ast_node_index node = {.value = 0};
        if (status == SXML_STATUS_OK) {
            status = add_node(builder,
                              (node_request){.kind = SXML_XSLT_AST_KIND_VARIABLE,
                                             .parent = builder->root,
                                             .place = phases[phase_index].place,
                                             .has_place = true,
                                             .is_declaration = true},
                              &node);
        }
        if (status == SXML_STATUS_OK) {
            status = add_name_attribute(builder, node, name);
        }
        if (status == SXML_STATUS_OK) {
            status = add_expression_attribute(builder,
                                              (expression_attribute){.node = node,
                                                                     .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                                                     .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                                     .ast = from.ast,
                                                                     .document = phases[phase_index].place.document,
                                                                     .source_span = from.source_span});
            from.ast = NULL;
        }
        sxml_ast_destroy(from.ast);
    }
    sxml_xslt_ast_node_index node = {.value = 0};
    if (status == SXML_STATUS_OK && builder->selects_phase) {
        /* "#ANY": the number of the first phase whose when is true, 0 for
           none (5.5.22). */
        status = add_global(builder, (global_request){.name = builder->phase_name,
                                                      .select = no_text,
                                                      .as = SXML_TEXT("Q{http://www.w3.org/2001/XMLSchema}integer"),
                                                      .kind = SXML_XSLT_AST_KIND_VARIABLE},
                            &node);
        sxml_xslt_ast_node_index choose = {.value = 0};
        if (status == SXML_STATUS_OK) {
            status = add_node(builder,
                              (node_request){.kind = SXML_XSLT_AST_KIND_CHOOSE,
                                             .parent = node,
                                             .place = {.document = 0, .element = {.index = 0}},
                                             .has_place = false,
                                             .is_declaration = false},
                              &choose);
        }
        for (uint32_t phase_index = 0; status == SXML_STATUS_OK && phase_index < builder->chosen_phases.count;
             phase_index += 1) {
            parsed_expression when = {.is_present = false, .ast = NULL, .source_span = no_span};
            status = parse_attribute(builder,
                                     &(expression_request){.place = phases[phase_index].place,
                                                           .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_WHEN,
                                                           .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                           .expands = true,
                                                           .table_text = NULL},
                                     &when);
            if (status != SXML_STATUS_OK || when.ast == NULL) {
                continue;
            }
            sxml_xslt_ast_node_index branch = {.value = 0};
            status = add_node(builder,
                              (node_request){.kind = SXML_XSLT_AST_KIND_WHEN,
                                             .parent = choose,
                                             .place = phases[phase_index].place,
                                             .has_place = true,
                                             .is_declaration = false},
                              &branch);
            if (status == SXML_STATUS_OK) {
                status = add_expression_attribute(builder,
                                                  (expression_attribute){.node = branch,
                                                                         .kind = SXML_XSLT_AST_ATTRIBUTE_TEST,
                                                                         .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                                         .ast = when.ast,
                                                                         .document = phases[phase_index].place.document,
                                                                         .source_span = when.source_span});
                when.ast = NULL;
            }
            sxml_ast_destroy(when.ast);
            if (status == SXML_STATUS_OK) {
                status = add_fixed_sequence(builder, branch, number_text(builder, phase_index + 1U));
            }
        }
        sxml_xslt_ast_node_index otherwise = {.value = 0};
        if (status == SXML_STATUS_OK) {
            status = add_node(builder,
                              (node_request){.kind = SXML_XSLT_AST_KIND_OTHERWISE,
                                             .parent = choose,
                                             .place = {.document = 0, .element = {.index = 0}},
                                             .has_place = false,
                                             .is_declaration = false},
                              &otherwise);
        }
        if (status == SXML_STATUS_OK) {
            status = add_fixed_sequence(builder, otherwise, SXML_TEXT("0"));
        }
    }
    /* The phases' lets: globals (5.4.7). Under "#ANY" each phase's under a
       synthesized name, and the written name a choice on the phase. */
    sxml_schematron_array places = {.elements = NULL, .count = 0, .capacity = 0};
    const size_t schema_scope_count = builder->global_variables.count;
    for (uint32_t phase_index = 0; status == SXML_STATUS_OK && phase_index < builder->chosen_phases.count;
         phase_index += 1) {
        builder->global_variables.count = schema_scope_count;
        status = collect_children(builder, (children_request){.parent = phases[phase_index].place, .kind = SXML_SCHEMATRON_ELEMENT_LET}, &places);
        const sxml_schematron_place *let_places = places.elements;
        for (size_t let_index = 0; status == SXML_STATUS_OK && let_index < places.count; let_index += 1) {
            const sxml_byte_view written = trimmed_of(builder, let_places[let_index], SXML_SCHEMATRON_ATTRIBUTE_NAME);
            status = define_variable(builder, (variable_definition){.scope = &builder->global_variables,
                                                                    .name = written,
                                                                    .place = let_places[let_index]});
            sxml_name_identifier name = {.index = 0};
            if (status == SXML_STATUS_OK && builder->selects_phase) {
                status = intern_numbered_name(builder,
                                              (numbered_name){.stem = "phase", .number = phase_index + 1U,
                                                              .suffix = written},
                                              &name);
            }
            if (status == SXML_STATUS_OK) {
                status = emit_let(builder, (let_request){.place = let_places[let_index],
                                                         .parent = builder->root,
                                                         .is_global = true,
                                                         .name = name,
                                                         .bindings = bindings});
            }
        }
    }
    if (!builder->selects_phase) {
        /* The one phase's lets stay in scope of every rule. */
    } else {
        builder->global_variables.count = schema_scope_count;
        /* One global per name the candidate phases define. */
        sxml_schematron_array names = {.elements = NULL, .count = 0, .capacity = 0};
        for (uint32_t phase_index = 0; status == SXML_STATUS_OK && phase_index < builder->chosen_phases.count;
             phase_index += 1) {
            status = collect_children(builder, (children_request){.parent = phases[phase_index].place, .kind = SXML_SCHEMATRON_ELEMENT_LET}, &places);
            const sxml_schematron_place *let_places = places.elements;
            for (size_t let_index = 0; status == SXML_STATUS_OK && let_index < places.count; let_index += 1) {
                const sxml_byte_view written =
                    trimmed_of(builder, let_places[let_index], SXML_SCHEMATRON_ATTRIBUTE_NAME);
                bool is_known = false;
                const sxml_byte_view *known = names.elements;
                for (size_t name_index = 0; name_index < names.count; name_index += 1) {
                    if (sxml_byte_view_equals(known[name_index], written)) {
                        is_known = true;
                    }
                }
                if (!is_known) {
                    status = sxml_schematron_array_push(compilation, &names, SXML_SCHEMATRON_ELEMENT_OF(written));
                }
            }
        }
        const sxml_byte_view *known = names.elements;
        for (size_t name_index = 0; status == SXML_STATUS_OK && name_index < names.count; name_index += 1) {
            sxml_name_identifier written_name = {.index = 0};
            if (resolve_variable_name(builder, (written_variable){.text = known[name_index], .bindings = bindings}, &written_name) != SXML_STATUS_OK) {
                continue;
            }
            status = add_global(builder, (global_request){.name = written_name,
                                                          .select = no_text,
                                                          .as = SXML_TEXT("item()*"),
                                                          .kind = SXML_XSLT_AST_KIND_VARIABLE},
                                &node);
            sxml_xslt_ast_node_index choose = {.value = 0};
            if (status == SXML_STATUS_OK) {
                status = add_node(builder,
                                  (node_request){.kind = SXML_XSLT_AST_KIND_CHOOSE,
                                                 .parent = node,
                                                 .place = {.document = 0, .element = {.index = 0}},
                                                 .has_place = false,
                                                 .is_declaration = false},
                                  &choose);
            }
            for (uint32_t phase_index = 0;
                 status == SXML_STATUS_OK && phase_index < builder->chosen_phases.count; phase_index += 1) {
                status = collect_children(builder, (children_request){.parent = phases[phase_index].place, .kind = SXML_SCHEMATRON_ELEMENT_LET}, &places);
                const sxml_schematron_place *let_places = places.elements;
                bool defines = false;
                for (size_t let_index = 0; let_index < places.count; let_index += 1) {
                    if (sxml_byte_view_equals(trimmed_of(builder, let_places[let_index],
                                                         SXML_SCHEMATRON_ATTRIBUTE_NAME),
                                              known[name_index])) {
                        defines = true;
                    }
                }
                if (status != SXML_STATUS_OK || !defines) {
                    continue;
                }
                sxml_byte_view text = no_text;
                status = phase_test_text(builder, phase_index + 1U, &text);
                sxml_xslt_ast_node_index branch = {.value = 0};
                if (status == SXML_STATUS_OK) {
                    status = add_fixed_node(builder,
                                            (fixed_node){.kind = SXML_XSLT_AST_KIND_WHEN,
                                                         .parent = choose,
                                                         .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_TEST,
                                                         .text = text,
                                                         .document = 0,
                                                         .origin = no_span},
                                            &branch);
                }
                sxml_name_identifier inner = {.index = 0};
                if (status == SXML_STATUS_OK) {
                    status = intern_numbered_name(builder,
                                                  (numbered_name){.stem = "phase", .number = phase_index + 1U,
                                                                  .suffix = known[name_index]},
                                                  &inner);
                }
                if (status == SXML_STATUS_OK) {
                    status = make_reference_text(
                        builder, (reference_text){.before = "$", .name = inner, .after = ""}, &text);
                }
                if (status == SXML_STATUS_OK) {
                    status = add_fixed_sequence(builder, branch, text);
                }
            }
        }
        sxml_schematron_array_release(compilation, &names);
    }
    sxml_schematron_array_release(compilation, &places);
    /* The start nodes (D315 "Rule contexts"). */
    if (status == SXML_STATUS_OK && !builder->selects_phase) {
        sxml_byte_view text = no_text;
        status = start_text(builder, builder->chosen_phases.count == 1 ? 1U : 0U, &text);
        if (status == SXML_STATUS_OK) {
            status = add_global(builder, (global_request){.name = builder->start_name,
                                                          .select = text,
                                                          .as = no_text,
                                                          .kind = SXML_XSLT_AST_KIND_VARIABLE},
                                &node);
        }
    } else if (status == SXML_STATUS_OK) {
        status = add_global(builder, (global_request){.name = builder->start_name,
                                                      .select = no_text,
                                                      .as = SXML_TEXT("item()*"),
                                                      .kind = SXML_XSLT_AST_KIND_VARIABLE},
                            &node);
        sxml_xslt_ast_node_index choose = {.value = 0};
        if (status == SXML_STATUS_OK) {
            status = add_node(builder,
                              (node_request){.kind = SXML_XSLT_AST_KIND_CHOOSE,
                                             .parent = node,
                                             .place = {.document = 0, .element = {.index = 0}},
                                             .has_place = false,
                                             .is_declaration = false},
                              &choose);
        }
        for (uint32_t phase_index = 0; status == SXML_STATUS_OK && phase_index < builder->chosen_phases.count;
             phase_index += 1) {
            sxml_byte_view text = no_text;
            status = phase_test_text(builder, phase_index + 1U, &text);
            sxml_xslt_ast_node_index branch = {.value = 0};
            if (status == SXML_STATUS_OK) {
                status = add_fixed_node(builder,
                                        (fixed_node){.kind = SXML_XSLT_AST_KIND_WHEN,
                                                     .parent = choose,
                                                     .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_TEST,
                                                     .text = text,
                                                     .document = 0,
                                                     .origin = no_span},
                                        &branch);
            }
            if (status == SXML_STATUS_OK) {
                status = start_text(builder, phase_index + 1U, &text);
            }
            if (status == SXML_STATUS_OK) {
                status = add_fixed_sequence(builder, branch, text);
            }
        }
        sxml_xslt_ast_node_index otherwise = {.value = 0};
        if (status == SXML_STATUS_OK) {
            status = add_node(builder,
                              (node_request){.kind = SXML_XSLT_AST_KIND_OTHERWISE,
                                             .parent = choose,
                                             .place = {.document = 0, .element = {.index = 0}},
                                             .has_place = false,
                                             .is_declaration = false},
                              &otherwise);
        }
        if (status == SXML_STATUS_OK) {
            status = add_fixed_sequence(builder, otherwise, SXML_TEXT("."));
        }
    }
    return status;
}

/* ------------------------------------------------------------ Validate */

/* Applies one compiled pattern under parent (D315 "The stylesheet the
   builder makes"): to the node, or the phase's from nodes, or each
   subordinate document (5.4.12). */
typedef struct application_request {
    const compiled_pattern *pattern;
    sxml_xslt_ast_node_index parent;
    /* The phase whose from applies (from 1); 0 for none. */
    uint32_t phase_number;
} application_request;

static sxml_status emit_application(schematron_builder *builder, application_request request) {
    sxml_name_identifier mode = {.index = 0};
    sxml_status status = intern_numbered_name(
        builder, (numbered_name){.stem = "pattern", .number = request.pattern->number, .suffix = no_text},
        &mode);
    sxml_xslt_ast_node_index apply = {.value = 0};
    sxml_byte_view text = no_text;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!request.pattern->has_documents) {
        status = start_text(builder, request.phase_number, &text);
        if (status == SXML_STATUS_OK) {
            status = add_fixed_node(builder,
                                    (fixed_node){.kind = SXML_XSLT_AST_KIND_APPLY_TEMPLATES,
                                                 .parent = request.parent,
                                                 .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                                 .text = text,
                                                 .document = 0,
                                                 .origin = no_span},
                                    &apply);
        }
        if (status == SXML_STATUS_OK) {
            status = add_mode_attribute(builder, apply, mode_token(mode));
        }
        return status;
    }
    /* documents: at the instance root, its strings resolved against the
       instance's base URI. */
    parsed_expression documents = {.is_present = false, .ast = NULL, .source_span = no_span};
    sxml_schematron_place documents_place = request.pattern->place;
    if (!attribute_of(builder, documents_place, SXML_SCHEMATRON_ATTRIBUTE_DOCUMENTS).is_present) {
        documents_place = request.pattern->rules_place;
    }
    status = collect_instance_parameters(builder, request.pattern);
    if (status == SXML_STATUS_OK) {
        status = parse_attribute(builder,
                                 &(expression_request){.place = documents_place,
                                                       .attribute_kind = SXML_SCHEMATRON_ATTRIBUTE_DOCUMENTS,
                                                       .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                       .expands = true,
                                                       .table_text = NULL},
                                 &documents);
    }
    if (status != SXML_STATUS_OK || documents.ast == NULL) {
        return status;
    }
    sxml_xslt_ast_node_index root = {.value = 0};
    status = add_fixed_node(builder,
                            (fixed_node){.kind = SXML_XSLT_AST_KIND_FOR_EACH,
                                         .parent = request.parent,
                                         .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                         .text = SXML_TEXT("root(.)"),
                                         .document = 0,
                                         .origin = no_span},
                            &root);
    if (status == SXML_STATUS_OK) {
        status = add_fixed_variable(builder, (fixed_variable){.parent = root,
                                                              .name = builder->base_name,
                                                              .select = SXML_TEXT("base-uri(.)"),
                                                              .document = 0,
                                                              .origin = no_span});
    }
    sxml_xslt_ast_node_index for_each = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = add_node(builder,
                          (node_request){.kind = SXML_XSLT_AST_KIND_FOR_EACH,
                                         .parent = root,
                                         .place = documents_place,
                                         .has_place = true,
                                         .is_declaration = false},
                          &for_each);
    }
    if (status == SXML_STATUS_OK) {
        status = add_expression_attribute(builder,
                                          (expression_attribute){.node = for_each,
                                                                 .kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                                                 .value_kind = SXML_XSLT_AST_VALUE_KIND_EXPRESSION,
                                                                 .ast = documents.ast,
                                                                 .document = documents_place.document,
                                                                 .source_span = documents.source_span});
        documents.ast = NULL;
    }
    sxml_ast_destroy(documents.ast);
    if (status == SXML_STATUS_OK) {
        status = make_reference_text(builder,
                                     (reference_text){.before = "doc(resolve-uri(string(.), ($",
                                                      .name = builder->base_name,
                                                      .after = ", static-base-uri())[1]))"},
                                     &text);
    }
    if (status == SXML_STATUS_OK) {
        status = add_fixed_node(builder,
                                (fixed_node){.kind = SXML_XSLT_AST_KIND_APPLY_TEMPLATES,
                                             .parent = for_each,
                                             .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_SELECT,
                                             .text = text,
                                             .document = documents_place.document,
                                             .origin = documents.source_span},
                                &apply);
    }
    if (status == SXML_STATUS_OK) {
        status = add_mode_attribute(builder, apply, mode_token(mode));
    }
    return status;
}

/* The template of the validation's mode: each compiled pattern in turn,
   under "#ANY" first the phase's number and a choice on it (D315). */
static sxml_status emit_validate(schematron_builder *builder) {
    sxml_schematron_compilation *compilation = builder->compilation;
    sxml_xslt_ast_node_index template_node = {.value = 0};
    sxml_status status = add_node(builder,
                                  (node_request){.kind = SXML_XSLT_AST_KIND_TEMPLATE,
                                                 .parent = builder->root,
                                                 .place = {.document = 0, .element = {.index = 0}},
                                                 .has_place = false,
                                                 .is_declaration = true},
                                  &template_node);
    if (status == SXML_STATUS_OK) {
        status = add_fixed_attribute(builder,
                                     (fixed_attribute){.node = template_node,
                                                       .kind = SXML_XSLT_AST_ATTRIBUTE_MATCH,
                                                       .value_kind = SXML_XSLT_AST_VALUE_KIND_PATTERN,
                                                       .text = SXML_TEXT("."),
                                                       .document = 0,
                                                       .origin = no_span});
    }
    if (status == SXML_STATUS_OK) {
        status = add_mode_attribute(builder, template_node, mode_token(builder->validate_name));
    }
    const size_t pattern_count = builder->patterns.count;
    if (!builder->selects_phase) {
        const uint32_t phase_number = builder->chosen_phases.count == 1 ? 1U : 0U;
        for (uint32_t number = 1; status == SXML_STATUS_OK && number <= pattern_count; number += 1) {
            const compiled_pattern *pattern = pattern_of_number(builder, number);
            if (pattern != NULL) {
                status = emit_application(builder, (application_request){.pattern = pattern,
                                                                         .parent = template_node,
                                                                         .phase_number = phase_number});
            }
        }
        return status;
    }
    sxml_byte_view text = no_text;
    if (status == SXML_STATUS_OK) {
        status = make_reference_text(
            builder, (reference_text){.before = "$", .name = builder->phase_name, .after = ""}, &text);
    }
    if (status == SXML_STATUS_OK) {
        status = add_fixed_sequence(builder, template_node, text);
    }
    sxml_xslt_ast_node_index choose = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = add_node(builder,
                          (node_request){.kind = SXML_XSLT_AST_KIND_CHOOSE,
                                         .parent = template_node,
                                         .place = {.document = 0, .element = {.index = 0}},
                                         .has_place = false,
                                         .is_declaration = false},
                          &choose);
    }
    const sxml_schematron_identified *phases = builder->chosen_phases.elements;
    sxml_schematron_array numbers = {.elements = NULL, .count = 0, .capacity = 0};
    sxml_schematron_table *table = &compilation->table;
    for (uint32_t phase_index = 0; status == SXML_STATUS_OK && phase_index < builder->chosen_phases.count;
         phase_index += 1) {
        status = phase_test_text(builder, phase_index + 1U, &text);
        sxml_xslt_ast_node_index branch = {.value = 0};
        if (status == SXML_STATUS_OK) {
            status = add_fixed_node(builder,
                                    (fixed_node){.kind = SXML_XSLT_AST_KIND_WHEN,
                                                 .parent = choose,
                                                 .attribute_kind = SXML_XSLT_AST_ATTRIBUTE_TEST,
                                                 .text = text,
                                                 .document = 0,
                                                 .origin = no_span},
                                    &branch);
        }
        if (status == SXML_STATUS_OK) {
            status = phase_patterns(builder, phases[phase_index].place, &numbers);
        }
        sxml_schematron_phase_entry entry = {
            .identifier = {.byte_offset = 0, .byte_count = 0},
            .patterns = {.first_reference = (uint32_t)table->reference_count, .reference_count = 0}};
        if (status == SXML_STATUS_OK) {
            status = add_table_text(builder, phases[phase_index].identifier, &entry.identifier);
        }
        const uint32_t *pattern_numbers = numbers.elements;
        for (size_t number_index = 0; status == SXML_STATUS_OK && number_index < numbers.count;
             number_index += 1) {
            const compiled_pattern *pattern = pattern_of_number(builder, pattern_numbers[number_index]);
            if (pattern == NULL) {
                continue;
            }
            status = emit_application(builder, (application_request){.pattern = pattern,
                                                                     .parent = branch,
                                                                     .phase_number = phase_index + 1U});
            if (status == SXML_STATUS_OK) {
                status = SXML_GROW(builder, table->references, table->reference_count, uint32_t);
            }
            if (status == SXML_STATUS_OK) {
                table->references[table->reference_count - 1U] = pattern->number - 1U;
                entry.patterns.reference_count += 1;
            }
        }
        if (status == SXML_STATUS_OK) {
            status = SXML_GROW(builder, table->phases, table->phase_count, sxml_schematron_phase_entry);
        }
        if (status == SXML_STATUS_OK) {
            table->phases[table->phase_count - 1U] = entry;
        }
    }
    sxml_schematron_array_release(compilation, &numbers);
    sxml_xslt_ast_node_index otherwise = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = add_node(builder,
                          (node_request){.kind = SXML_XSLT_AST_KIND_OTHERWISE,
                                         .parent = choose,
                                         .place = {.document = 0, .element = {.index = 0}},
                                         .has_place = false,
                                         .is_declaration = false},
                          &otherwise);
    }
    for (uint32_t number = 1; status == SXML_STATUS_OK && number <= pattern_count; number += 1) {
        const compiled_pattern *pattern = pattern_of_number(builder, number);
        if (pattern != NULL) {
            status = emit_application(builder, (application_request){.pattern = pattern,
                                                                     .parent = otherwise,
                                                                     .phase_number = 0});
        }
    }
    return status;
}

/* --------------------------------------------------------- Syntax only */

/* Parses the expressions of what is not compiled, so that their syntax
   errors are found (7.2: a schema with one is not correct): patterns no
   phase compiled, abstract rules, phases not chosen. Nothing is added to
   the AST. Walks the effective tree from place with a stack of cursor
   levels (D71). */
static sxml_status check_syntax(schematron_builder *builder, sxml_schematron_place place) {
    sxml_schematron_compilation *compilation = builder->compilation;
    typedef struct expression_attribute_kind {
        sxml_schematron_element_kind element_kind;
        sxml_schematron_attribute_kind attribute_kind;
        sxml_xslt_ast_value_kind value_kind;
    } expression_attribute_kind;
    static const expression_attribute_kind kinds[] = {
        {SXML_SCHEMATRON_ELEMENT_RULE, SXML_SCHEMATRON_ATTRIBUTE_VISIT_EACH, SXML_XSLT_AST_VALUE_KIND_EXPRESSION},
        {SXML_SCHEMATRON_ELEMENT_RULE, SXML_SCHEMATRON_ATTRIBUTE_SUBJECT, SXML_XSLT_AST_VALUE_KIND_EXPRESSION},
        {SXML_SCHEMATRON_ELEMENT_ASSERT, SXML_SCHEMATRON_ATTRIBUTE_TEST, SXML_XSLT_AST_VALUE_KIND_EXPRESSION},
        {SXML_SCHEMATRON_ELEMENT_ASSERT, SXML_SCHEMATRON_ATTRIBUTE_SUBJECT, SXML_XSLT_AST_VALUE_KIND_EXPRESSION},
        {SXML_SCHEMATRON_ELEMENT_REPORT, SXML_SCHEMATRON_ATTRIBUTE_TEST, SXML_XSLT_AST_VALUE_KIND_EXPRESSION},
        {SXML_SCHEMATRON_ELEMENT_REPORT, SXML_SCHEMATRON_ATTRIBUTE_SUBJECT, SXML_XSLT_AST_VALUE_KIND_EXPRESSION},
        {SXML_SCHEMATRON_ELEMENT_LET, SXML_SCHEMATRON_ATTRIBUTE_VALUE, SXML_XSLT_AST_VALUE_KIND_EXPRESSION},
        {SXML_SCHEMATRON_ELEMENT_LET, SXML_SCHEMATRON_ATTRIBUTE_AS, SXML_XSLT_AST_VALUE_KIND_SEQUENCE_TYPE},
        {SXML_SCHEMATRON_ELEMENT_VALUE_OF, SXML_SCHEMATRON_ATTRIBUTE_SELECT, SXML_XSLT_AST_VALUE_KIND_EXPRESSION},
        {SXML_SCHEMATRON_ELEMENT_NAME, SXML_SCHEMATRON_ATTRIBUTE_PATH, SXML_XSLT_AST_VALUE_KIND_EXPRESSION},
        {SXML_SCHEMATRON_ELEMENT_PATTERN, SXML_SCHEMATRON_ATTRIBUTE_DOCUMENTS, SXML_XSLT_AST_VALUE_KIND_EXPRESSION},
        {SXML_SCHEMATRON_ELEMENT_GROUP, SXML_SCHEMATRON_ATTRIBUTE_DOCUMENTS, SXML_XSLT_AST_VALUE_KIND_EXPRESSION},
        {SXML_SCHEMATRON_ELEMENT_PHASE, SXML_SCHEMATRON_ATTRIBUTE_FROM, SXML_XSLT_AST_VALUE_KIND_EXPRESSION},
        {SXML_SCHEMATRON_ELEMENT_PHASE, SXML_SCHEMATRON_ATTRIBUTE_WHEN, SXML_XSLT_AST_VALUE_KIND_EXPRESSION},
    };
    sxml_schematron_cursor cursor = {.compilation = compilation,
                                     .frames = {.elements = NULL, .count = 0, .capacity = 0},
                                     .expands_rules = false};
    size_t depth = 1;
    sxml_status status = sxml_schematron_cursor_start(&cursor, place);
    sxml_schematron_child child = {.has_child = true,
                                   .place = place,
                                   .element_kind = sxml_schematron_element_kind_of(compilation, place),
                                   .is_inserted = false};
    bool is_first = true;
    while (status == SXML_STATUS_OK && depth > 0) {
        if (!is_first) {
            status = sxml_schematron_cursor_next(&cursor, &child);
            if (status != SXML_STATUS_OK) {
                break;
            }
            if (!child.has_child) {
                depth -= 1;
                continue;
            }
        }
        is_first = false;
        for (size_t kind_index = 0; status == SXML_STATUS_OK && kind_index < sizeof(kinds) / sizeof(kinds[0]);
             kind_index += 1) {
            if (kinds[kind_index].element_kind != child.element_kind) {
                continue;
            }
            parsed_expression parsed = {.is_present = false, .ast = NULL, .source_span = no_span};
            status = parse_attribute(builder,
                                     &(expression_request){.place = child.place,
                                                           .attribute_kind = kinds[kind_index].attribute_kind,
                                                           .value_kind = kinds[kind_index].value_kind,
                                                           .expands = kinds[kind_index].attribute_kind !=
                                                                      SXML_SCHEMATRON_ATTRIBUTE_AS,
                                                           .table_text = NULL},
                                     &parsed);
            sxml_ast_destroy(parsed.ast);
        }
        if (status == SXML_STATUS_OK && child.element_kind == SXML_SCHEMATRON_ELEMENT_RULE &&
            attribute_of(builder, child.place, SXML_SCHEMATRON_ATTRIBUTE_CONTEXT).is_present) {
            parsed_context context = {.pattern = NULL, .expression = NULL, .source_span = no_span};
            status = parse_rule_context(builder, (rule_context_request){.rule = child.place, .table_text = NULL}, &context);
            sxml_ast_destroy(context.pattern);
            sxml_ast_destroy(context.expression);
        }
        const bool descends = child.element_kind == SXML_SCHEMATRON_ELEMENT_PATTERN ||
                              child.element_kind == SXML_SCHEMATRON_ELEMENT_GROUP ||
                              child.element_kind == SXML_SCHEMATRON_ELEMENT_RULE ||
                              child.element_kind == SXML_SCHEMATRON_ELEMENT_ASSERT ||
                              child.element_kind == SXML_SCHEMATRON_ELEMENT_REPORT ||
                              child.element_kind == SXML_SCHEMATRON_ELEMENT_EMPH ||
                              child.element_kind == SXML_SCHEMATRON_ELEMENT_DIR ||
                              child.element_kind == SXML_SCHEMATRON_ELEMENT_SPAN ||
                              child.element_kind == SXML_SCHEMATRON_ELEMENT_PHASE ||
                              child.element_kind == SXML_SCHEMATRON_ELEMENT_DIAGNOSTIC ||
                              child.element_kind == SXML_SCHEMATRON_ELEMENT_PROPERTY;
        if (status == SXML_STATUS_OK && descends && !(child.place.document == place.document &&
                                                      child.place.element.index == place.element.index)) {
            status = sxml_schematron_cursor_start(&cursor, child.place);
            depth += 1;
        }
    }
    sxml_schematron_cursor_finish(&cursor);
    return status;
}

/* Every expression the compiled stylesheet does not hold is parsed:
   patterns not compiled (an instance with its parameters), abstract
   rules outside abstract patterns, the phases not chosen (7.2). */
static sxml_status check_uncompiled(schematron_builder *builder) {
    sxml_schematron_compilation *compilation = builder->compilation;
    const compiled_pattern *patterns = builder->patterns.elements;
    sxml_status status = SXML_STATUS_OK;
    for (size_t pattern_index = 0; status == SXML_STATUS_OK && pattern_index < builder->patterns.count;
         pattern_index += 1) {
        if (patterns[pattern_index].number != 0) {
            continue;
        }
        status = collect_instance_parameters(builder, &patterns[pattern_index]);
        if (status == SXML_STATUS_OK) {
            status = check_syntax(builder, patterns[pattern_index].rules_place);
        }
    }
    builder->has_instance = false;
    const sxml_schematron_identified *abstract_rules = compilation->index.abstract_rules.elements;
    for (size_t rule_index = 0; status == SXML_STATUS_OK && rule_index < compilation->index.abstract_rules.count;
         rule_index += 1) {
        /* An abstract rule inside an abstract pattern holds placeholders:
           checked through the instances. */
        bool is_in_abstract_pattern = false;
        const sxml_schematron_identified *abstract_patterns = compilation->index.abstract_patterns.elements;
        for (size_t abstract_index = 0; abstract_index < compilation->index.abstract_patterns.count;
             abstract_index += 1) {
            const sxml_schematron_place pattern = abstract_patterns[abstract_index].place;
            if (pattern.document != abstract_rules[rule_index].place.document) {
                continue;
            }
            sxml_node_description description;
            memset(&description, 0, sizeof(description));
            if (sxml_node_describe((sxml_node_reference){.tree = document_of(builder, pattern.document)->tree,
                                                         .node_index = pattern.element,
                                                         .namespace_number = 0},
                                   &description) == SXML_STATUS_OK &&
                abstract_rules[rule_index].place.element.index > pattern.element.index &&
                abstract_rules[rule_index].place.element.index <= description.last_descendant.index) {
                is_in_abstract_pattern = true;
            }
        }
        if (!is_in_abstract_pattern) {
            status = check_syntax(builder, abstract_rules[rule_index].place);
        }
    }
    const sxml_schematron_identified *phases = compilation->index.phases.elements;
    const sxml_schematron_identified *chosen = builder->chosen_phases.elements;
    for (size_t phase_index = 0; status == SXML_STATUS_OK && phase_index < compilation->index.phases.count;
         phase_index += 1) {
        bool is_chosen = false;
        for (size_t chosen_index = 0; chosen_index < builder->chosen_phases.count; chosen_index += 1) {
            if (chosen[chosen_index].place.element.index == phases[phase_index].place.element.index &&
                chosen[chosen_index].place.document == phases[phase_index].place.document) {
                is_chosen = true;
            }
        }
        if (!is_chosen) {
            status = check_syntax(builder, phases[phase_index].place);
        }
    }
    return status;
}

/* --------------------------------------------------------------- Entry */

/* The AST, its modules and static contexts, and the schema's XSLT
   declarations: one module per schematron document, the compiled one
   first, each document's declarations read right after its module is
   added (the AST's modules are added in pre-order). */
static sxml_status create_stylesheet(schematron_builder *builder, sxml_namespace_binding_view bindings) {
    sxml_schematron_compilation *compilation = builder->compilation;
    sxml_processor *processor = compilation->processor;
    const uint64_t node_limit = processor->limits.maximum_tree_node_count;
    sxml_status status = sxml_xslt_ast_create(
        compilation->allocator,
        &(sxml_xslt_ast_configuration){
            .allocator = compilation->allocator,
            .item_heap = processor->item_heap,
            .maximum_module_count = 0,
            .maximum_node_count = node_limit > UINT32_MAX ? UINT32_MAX : (uint32_t)node_limit},
        &builder->ast);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    compilation->ast = builder->ast;
    const sxml_schematron_place *declarations = compilation->index.foreign_declarations.elements;
    for (uint32_t document = 0; status == SXML_STATUS_OK && document < compilation->documents.count;
         document += 1) {
        sxml_xslt_ast_node_index include = {.value = 0};
        if (document > 0) {
            /* An included module needs an INCLUDE of module 1. */
            status = sxml_xslt_ast_add_node(
                builder->ast,
                &(sxml_xslt_ast_node_request){.kind = SXML_XSLT_AST_KIND_INCLUDE,
                                              .flags = SXML_XSLT_AST_NODE_FLAG_DECLARATION,
                                              .module = {.value = 1},
                                              .source_node = {.index = 0},
                                              .static_context = builder->root_context,
                                              .element_name = {.index = 0},
                                              .namespace_prefix = {.index = 0},
                                              .content = {.kind = SXML_XSLT_AST_VALUE_KIND_NONE},
                                              .source_span = no_span},
                &include);
            if (status == SXML_STATUS_OK) {
                status = sxml_xslt_ast_append_child(
                    builder->ast, (sxml_xslt_ast_child_request){.parent = builder->root, .child = include});
            }
        }
        sxml_xslt_ast_module_index module = {.value = 0};
        if (status == SXML_STATUS_OK) {
            const sxml_schematron_document *entry = document_of(builder, document);
            status = sxml_xslt_ast_add_module(
                builder->ast,
                &(sxml_xslt_ast_module_request){
                    .relation = document == 0 ? SXML_XSLT_AST_MODULE_RELATION_PRINCIPAL
                                              : SXML_XSLT_AST_MODULE_RELATION_INCLUDED,
                    .parent_module = {.value = document == 0 ? 0U : 1U},
                    .referencing_node = include,
                    .document = entry->document_node,
                    .module_uri = entry->uri},
                &module);
        }
        if (status == SXML_STATUS_OK) {
            document_of(builder, document)->module = module;
            status = add_document_contexts(builder, (context_request){.document = document, .bindings = bindings});
        }
        if (status == SXML_STATUS_OK) {
            /* The module's root: the schema for module 1, a synthesized
               stylesheet for the others. */
            sxml_xslt_ast_node_index root = {.value = 0};
            const sxml_schematron_document *entry = document_of(builder, document);
            status = sxml_xslt_ast_add_node(
                builder->ast,
                &(sxml_xslt_ast_node_request){
                    .kind = SXML_XSLT_AST_KIND_STYLESHEET,
                    .flags = SXML_XSLT_AST_NODE_FLAG_SYNTHESIZED,
                    .module = module,
                    .source_node = document == 0 ? compilation->index.schema.element
                                                 : (sxml_node_index){.index = 0},
                    .static_context = entry->static_context,
                    .element_name = {.index = 0},
                    .namespace_prefix = {.index = 0},
                    .content = {.kind = SXML_XSLT_AST_VALUE_KIND_NONE},
                    .source_span = document == 0 ? sxml_schematron_element_span(compilation,
                                                                                 compilation->index.schema)
                                                 : no_span},
                &root);
            if (document == 0) {
                builder->root = root;
                builder->root_context = entry->static_context;
            }
        }
        for (size_t declaration_index = 0;
             status == SXML_STATUS_OK && declaration_index < compilation->index.foreign_declarations.count;
             declaration_index += 1) {
            if (declarations[declaration_index].document != document ||
                !is_read_declaration(builder, declarations[declaration_index])) {
                continue;
            }
            status = read_foreign(builder, (foreign_read){.place = declarations[declaration_index],
                                                          .parent = builder->root,
                                                          .reads_instruction = false,
                                                          .bindings = bindings});
        }
    }
    return status;
}

/* The table's schema texts (title, versions, binding, phase, ns). */
static sxml_status fill_table(schematron_builder *builder, sxml_namespace_binding_view bindings) {
    sxml_schematron_compilation *compilation = builder->compilation;
    sxml_schematron_table *table = &compilation->table;
    const sxml_schematron_place schema = compilation->index.schema;
    sxml_status status = SXML_STATUS_OK;
    if (compilation->index.has_title) {
        sxml_xml_buffer title = sxml_xml_buffer_empty(compilation->allocator);
        status = append_element_text(builder, compilation->index.title, &title);
        if (status == SXML_STATUS_OK) {
            status = add_table_text(builder, sxml_xml_text_trim_whitespace(sxml_schematron_buffer_text(&title)),
                                    &table->title);
        }
        sxml_xml_buffer_release(&title);
    }
    if (status == SXML_STATUS_OK) {
        status = add_table_text(builder, trimmed_of(builder, schema, SXML_SCHEMATRON_ATTRIBUTE_SCHEMA_VERSION),
                                &table->schema_version);
    }
    const sxml_schematron_attribute edition =
        attribute_of(builder, schema, SXML_SCHEMATRON_ATTRIBUTE_SCHEMATRON_EDITION);
    table->has_schematron_edition = edition.is_present;
    if (status == SXML_STATUS_OK) {
        /* As written (5.5.15, the owner's 8b). */
        status = add_table_text(builder, edition.value, &table->schematron_edition);
    }
    if (status == SXML_STATUS_OK) {
        status = add_table_text(builder, trimmed_of(builder, schema, SXML_SCHEMATRON_ATTRIBUTE_QUERY_BINDING),
                                &table->query_binding);
    }
    if (status == SXML_STATUS_OK) {
        status = add_table_text(builder, compilation->phase_id, &table->phase);
    }
    table->selects_phase = builder->selects_phase;
    if (status == SXML_STATUS_OK && bindings.namespace_binding_count > 0) {
        status = sxml_allocator_allocate_array(
            compilation->allocator,
            (sxml_array_allocation_request){.element_count = bindings.namespace_binding_count * 2U,
                                            .element_size = sizeof(sxml_schematron_text),
                                            .element_alignment = alignof(sxml_schematron_text)},
            (void **)&table->namespace_texts);
    }
    for (size_t binding_index = 0; status == SXML_STATUS_OK && binding_index < bindings.namespace_binding_count;
         binding_index += 1) {
        sxml_byte_view prefix = no_text;
        sxml_byte_view uri = no_text;
        status = sxml_name_pool_get_namespace_prefix(
            compilation->name_pool, bindings.namespace_bindings[binding_index].namespace_prefix, &prefix);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_namespace_uri(
                compilation->name_pool, bindings.namespace_bindings[binding_index].namespace_uri, &uri);
        }
        if (status == SXML_STATUS_OK) {
            status = add_table_text(builder, prefix, &table->namespace_texts[binding_index * 2U]);
        }
        if (status == SXML_STATUS_OK) {
            status = add_table_text(builder, uri, &table->namespace_texts[binding_index * 2U + 1U]);
        }
        if (status == SXML_STATUS_OK) {
            table->namespace_count = binding_index + 1U;
        }
    }
    return status;
}

static sxml_status intern_builder_names(schematron_builder *builder) {
    typedef struct private_name {
        const char *local_name;
        sxml_name_identifier *name;
    } private_name;
    const private_name names[] = {
        {"validate", &builder->validate_name}, {"fired-rules", &builder->fired_rules_name},
        {"phase", &builder->phase_name},       {"start", &builder->start_name},
        {"context", &builder->context_name},   {"subject", &builder->subject_name},
        {"value", &builder->value_name},       {"base", &builder->base_name},
        {"root", &builder->root_name},
    };
    sxml_status status = SXML_STATUS_OK;
    for (size_t name_index = 0; status == SXML_STATUS_OK && name_index < sizeof(names) / sizeof(names[0]);
         name_index += 1) {
        status = intern_private_name(builder, sxml_byte_view_from_c_string(names[name_index].local_name),
                                     names[name_index].name);
    }
    return status;
}

sxml_status sxml_schematron_build(sxml_schematron_compilation *compilation) {
    schematron_builder builder;
    memset(&builder, 0, sizeof(builder));
    builder.compilation = compilation;
    builder.cursor = (sxml_schematron_cursor){.compilation = compilation,
                                              .frames = {.elements = NULL, .count = 0, .capacity = 0},
                                              .expands_rules = true};
    builder.scratch = sxml_xml_buffer_empty(compilation->allocator);
    builder.renamed_text = sxml_xml_buffer_empty(compilation->allocator);
    sxml_schematron_array bindings_array = {.elements = NULL, .count = 0, .capacity = 0};
    sxml_status status = intern_builder_names(&builder);
    if (status == SXML_STATUS_OK) {
        status = collect_namespaces(&builder, &bindings_array);
    }
    const sxml_namespace_binding_view bindings = {.namespace_bindings = bindings_array.elements,
                                                  .namespace_binding_count = bindings_array.count};
    if (status == SXML_STATUS_OK) {
        status = create_stylesheet(&builder, bindings);
    }
    if (status == SXML_STATUS_OK) {
        status = collect_schema_parameters(&builder);
    }
    if (status == SXML_STATUS_OK) {
        status = choose_phase(&builder);
    }
    if (status == SXML_STATUS_OK) {
        status = list_patterns(&builder);
    }
    /* The schema's variables: its parameters' names and its lets
       (globals, 5.4.7). */
    const sxml_schematron_place *parameters = compilation->index.schema_parameters.elements;
    for (size_t parameter_index = 0;
         status == SXML_STATUS_OK && parameter_index < compilation->index.schema_parameters.count;
         parameter_index += 1) {
        status = define_variable(&builder,
                                 (variable_definition){.scope = &builder.global_variables,
                                                       .name = trimmed_of(&builder, parameters[parameter_index],
                                                                          SXML_SCHEMATRON_ATTRIBUTE_NAME),
                                                       .place = parameters[parameter_index]});
    }
    const sxml_schematron_place *lets = compilation->index.schema_lets.elements;
    for (size_t let_index = 0; status == SXML_STATUS_OK && let_index < compilation->index.schema_lets.count;
         let_index += 1) {
        status = define_variable(&builder,
                                 (variable_definition){.scope = &builder.global_variables,
                                                       .name = trimmed_of(&builder, lets[let_index],
                                                                          SXML_SCHEMATRON_ATTRIBUTE_NAME),
                                                       .place = lets[let_index]});
        if (status == SXML_STATUS_OK) {
            status = emit_let(&builder, (let_request){.place = lets[let_index],
                                                      .parent = builder.root,
                                                      .is_global = true,
                                                      .name = {.index = 0},
                                                      .bindings = bindings});
        }
    }
    sxml_xslt_ast_node_index node = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = add_global(&builder, (global_request){.name = builder.fired_rules_name,
                                                       .select = SXML_TEXT("false()"),
                                                       .as = no_text,
                                                       .kind = SXML_XSLT_AST_KIND_PARAM},
                            &node);
    }
    if (status == SXML_STATUS_OK) {
        status = emit_phases(&builder, bindings);
    }
    if (status == SXML_STATUS_OK) {
        status = plan_pattern_variables(&builder, bindings);
    }
    const compiled_pattern *patterns = builder.patterns.elements;
    for (uint32_t number = 1; status == SXML_STATUS_OK && number <= builder.patterns.count; number += 1) {
        const compiled_pattern *pattern = pattern_of_number(&builder, number);
        if (pattern != NULL) {
            status = emit_pattern(&builder, pattern, bindings);
        }
    }
    (void)patterns;
    builder.renamed_variables.count = 0;
    builder.local_variables.count = 0;
    builder.has_instance = false;
    if (status == SXML_STATUS_OK) {
        status = emit_validate(&builder);
    }
    if (status == SXML_STATUS_OK) {
        status = check_uncompiled(&builder);
    }
    if (status == SXML_STATUS_OK) {
        status = fill_table(&builder, bindings);
    }
    if (builder.ast != NULL) {
        sxml_xslt_ast_seal(builder.ast);
    }
    sxml_schematron_array_release(compilation, &bindings_array);
    sxml_schematron_array_release(compilation, &builder.schema_parameters);
    sxml_schematron_array_release(compilation, &builder.instance_parameters);
    sxml_schematron_array_release(compilation, &builder.patterns);
    sxml_schematron_array_release(compilation, &builder.chosen_phases);
    sxml_schematron_array_release(compilation, &builder.global_variables);
    sxml_schematron_array_release(compilation, &builder.local_variables);
    sxml_schematron_array_release(compilation, &builder.pattern_variables);
    sxml_schematron_array_release(compilation, &builder.renamed_variables);
    sxml_schematron_cursor_finish(&builder.cursor);
    sxml_xml_buffer_release(&builder.scratch);
    sxml_xml_buffer_release(&builder.renamed_text);
    return status;
}
