/*
 * QT4 environments (environment.h). Element and attribute names follow
 * third_party/qt4tests/catalog-schema.xsd. Each list is read in two passes
 * over the element's children: one to count, one to fill an array of that
 * size in the arena.
 */
#include "environment.h"

#include <stdalign.h>
#include <string.h>

#include "suite_path.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_qt4_empty_text = {.bytes = NULL,
                                                   .byte_count = 0};

/* The value of an undefined static base URI (catalog-schema.xsd,
   "static-base-uri"). */
#define SXML_QT4_UNDEFINED_URI "#UNDEFINED"

sxml_qt4_environment sxml_qt4_environment_absent(void) {
    return (sxml_qt4_environment){
        .origin = SXML_QT4_ENVIRONMENT_ORIGIN_ABSENT,
        .name = sxml_qt4_empty_text,
        .sources = {.sources = NULL, .source_count = 0},
        .parameters = {.parameters = NULL, .parameter_count = 0},
        .namespace_declarations = {.namespace_declarations = NULL,
                                   .namespace_declaration_count = 0},
        .resources = {.resources = NULL, .resource_count = 0},
        .collections = {.collections = NULL, .collection_count = 0},
        .collations = {.collations = NULL, .collation_count = 0},
        .schemas = {.schemas = NULL, .schema_count = 0},
        .has_context_item = false,
        .context_item_select = sxml_qt4_empty_text,
        .static_base_uri_kind = SXML_QT4_STATIC_BASE_URI_TEST_SET_FILE,
        .static_base_uri = sxml_qt4_empty_text,
        .decimal_formats = {.decimal_formats = NULL,
                            .decimal_format_count = 0},
        .has_function_library = false,
        .has_sandpit = false,
        .sandpit_path = {.bytes = NULL, .byte_count = 0},
    };
}

/* What every read below needs: the document, the arena and the file that
   relative paths start from. */
typedef struct sxml_qt4_environment_reader {
    const sxml_qt4_document *document;
    sxml_arena *arena;
    sxml_byte_view containing_file;
} sxml_qt4_environment_reader;

/* One attribute of one element. */
typedef struct sxml_qt4_attribute_name {
    sxml_node_index element;
    sxml_byte_view local_name;
} sxml_qt4_attribute_name;

/* Writes the attribute's value, empty when absent. */
static sxml_status
sxml_qt4_read_attribute(const sxml_qt4_environment_reader *reader,
                        sxml_qt4_attribute_name name, sxml_byte_view *value) {
    sxml_qt4_attribute_value found = {.is_present = false,
                                      .value = sxml_qt4_empty_text};
    const sxml_status status = sxml_qt4_document_find_attribute(
        reader->document,
        (sxml_qt4_attribute_lookup){.element = name.element,
                                    .local_name = name.local_name},
        &found);
    if (status == SXML_STATUS_OK) {
        *value = found.value;
    }
    return status;
}

/* xs:boolean "true" or "1" (XSD 1.1 Part 2, section 3.3.2); an absent
   attribute is false. */
static bool sxml_qt4_is_true(sxml_byte_view text) {
    return sxml_byte_view_equals(text, SXML_TEXT("true")) ||
           sxml_byte_view_equals(text, SXML_TEXT("1"));
}

/* Reads the file attribute of element as a suite path; empty when
   absent. */
static sxml_status
sxml_qt4_read_file_attribute(const sxml_qt4_environment_reader *reader,
                             sxml_node_index element,
                             sxml_byte_view *file_path) {
    sxml_byte_view reference = sxml_qt4_empty_text;
    sxml_status status = sxml_qt4_read_attribute(
        reader,
        (sxml_qt4_attribute_name){.element = element,
                                  .local_name = SXML_TEXT("file")},
        &reference);
    if (status != SXML_STATUS_OK || reference.byte_count == 0) {
        *file_path = sxml_qt4_empty_text;
        return status;
    }
    return sxml_qt4_suite_path_resolve(
        &(sxml_qt4_suite_path_request){.containing_file =
                                           reader->containing_file,
                                       .reference = reference,
                                       .arena = reader->arena},
        file_path);
}

/* An array of element_count elements of element_byte_count bytes. */
typedef struct sxml_qt4_array_request {
    size_t element_count;
    size_t element_byte_count;
    size_t alignment;
} sxml_qt4_array_request;

static sxml_status sxml_qt4_allocate_array(sxml_arena *arena,
                                           sxml_qt4_array_request request,
                                           void **memory) {
    if (request.element_count == 0) {
        *memory = NULL;
        return SXML_STATUS_OK;
    }
    size_t byte_count = 0;
    const sxml_status status = sxml_size_multiply(
        request.element_count, request.element_byte_count, &byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_arena_allocate(
        arena,
        (sxml_allocation_request){.byte_count = byte_count,
                                  .alignment = request.alignment},
        memory);
}

/* How many children of each kind an element has. */
typedef struct sxml_qt4_child_counts {
    size_t source_count;
    size_t parameter_count;
    size_t namespace_declaration_count;
    size_t resource_count;
    size_t collection_count;
    size_t collation_count;
    size_t schema_count;
    size_t decimal_format_count;
    size_t query_count;
} sxml_qt4_child_counts;

static bool sxml_qt4_is_named(sxml_qt4_element_step step,
                              sxml_byte_view local_name) {
    return sxml_byte_view_equals(step.local_name, local_name);
}

static sxml_status
sxml_qt4_count_children(const sxml_qt4_environment_reader *reader,
                        sxml_node_index parent, sxml_qt4_child_counts *counts) {
    *counts = (sxml_qt4_child_counts){0};
    sxml_qt4_element_cursor cursor = {.document = reader->document,
                                      .has_next_node = false,
                                      .next_node = {.index = 0}};
    sxml_status status =
        sxml_qt4_element_cursor_start(reader->document, parent, &cursor);
    while (status == SXML_STATUS_OK) {
        sxml_qt4_element_step step = {.has_element = false,
                                      .element = {.index = 0},
                                      .local_name = sxml_qt4_empty_text};
        status = sxml_qt4_element_cursor_next(&cursor, &step);
        if (status != SXML_STATUS_OK || !step.has_element) {
            break;
        }
        if (sxml_qt4_is_named(step, SXML_TEXT("source"))) {
            counts->source_count += 1;
        } else if (sxml_qt4_is_named(step, SXML_TEXT("param"))) {
            counts->parameter_count += 1;
        } else if (sxml_qt4_is_named(step, SXML_TEXT("namespace"))) {
            counts->namespace_declaration_count += 1;
        } else if (sxml_qt4_is_named(step, SXML_TEXT("resource"))) {
            counts->resource_count += 1;
        } else if (sxml_qt4_is_named(step, SXML_TEXT("collection"))) {
            counts->collection_count += 1;
        } else if (sxml_qt4_is_named(step, SXML_TEXT("collation"))) {
            counts->collation_count += 1;
        } else if (sxml_qt4_is_named(step, SXML_TEXT("schema"))) {
            counts->schema_count += 1;
        } else if (sxml_qt4_is_named(step, SXML_TEXT("decimal-format"))) {
            counts->decimal_format_count += 1;
        } else if (sxml_qt4_is_named(step, SXML_TEXT("query"))) {
            counts->query_count += 1;
        }
    }
    return status;
}

static sxml_status
sxml_qt4_read_source(const sxml_qt4_environment_reader *reader,
                     sxml_node_index element, sxml_qt4_source *source) {
    sxml_status status = sxml_qt4_read_attribute(
        reader,
        (sxml_qt4_attribute_name){.element = element,
                                  .local_name = SXML_TEXT("role")},
        &source->role);
    if (status == SXML_STATUS_OK) {
        status =
            sxml_qt4_read_file_attribute(reader, element, &source->file_path);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_read_attribute(
            reader,
            (sxml_qt4_attribute_name){.element = element,
                                      .local_name = SXML_TEXT("uri")},
            &source->uri);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_read_attribute(
            reader,
            (sxml_qt4_attribute_name){.element = element,
                                      .local_name = SXML_TEXT("validation")},
            &source->validation);
    }
    if (status == SXML_STATUS_OK) {
        sxml_byte_view flag_text = sxml_qt4_empty_text;
        status = sxml_qt4_read_attribute(
            reader,
            (sxml_qt4_attribute_name){.element = element,
                                      .local_name = SXML_TEXT("declared")},
            &flag_text);
        source->is_declared = sxml_qt4_is_true(flag_text);
    }
    return status;
}

static sxml_status
sxml_qt4_read_parameter(const sxml_qt4_environment_reader *reader,
                        sxml_node_index element,
                        sxml_qt4_parameter *parameter) {
    sxml_status status = sxml_qt4_read_attribute(
        reader,
        (sxml_qt4_attribute_name){.element = element,
                                  .local_name = SXML_TEXT("name")},
        &parameter->name);
    parameter->has_name_namespace = false;
    parameter->name_namespace_uri = sxml_qt4_empty_text;
    const uint8_t *colon =
        status != SXML_STATUS_OK || parameter->name.byte_count == 0
            ? NULL
            : memchr(parameter->name.bytes, ':', parameter->name.byte_count);
    if (colon != NULL) {
        sxml_qt4_attribute_value bound = {.is_present = false,
                                          .value = sxml_qt4_empty_text};
        status = sxml_qt4_document_resolve_prefix(
            reader->document,
            (sxml_qt4_prefix_lookup){
                .element = element,
                .namespace_prefix = {.bytes = parameter->name.bytes,
                                     .byte_count = (size_t)(colon - parameter->name.bytes)}},
            &bound);
        parameter->has_name_namespace = bound.is_present;
        parameter->name_namespace_uri = bound.value;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_read_attribute(
            reader,
            (sxml_qt4_attribute_name){.element = element,
                                      .local_name = SXML_TEXT("select")},
            &parameter->select);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_read_attribute(
            reader,
            (sxml_qt4_attribute_name){.element = element,
                                      .local_name = SXML_TEXT("as")},
            &parameter->declared_type);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_read_attribute(
            reader,
            (sxml_qt4_attribute_name){.element = element,
                                      .local_name = SXML_TEXT("source")},
            &parameter->source);
    }
    if (status == SXML_STATUS_OK) {
        sxml_byte_view flag_text = sxml_qt4_empty_text;
        status = sxml_qt4_read_attribute(
            reader,
            (sxml_qt4_attribute_name){.element = element,
                                      .local_name = SXML_TEXT("declared")},
            &flag_text);
        parameter->is_declared = sxml_qt4_is_true(flag_text);
    }
    return status;
}

static sxml_status
sxml_qt4_read_resource(const sxml_qt4_environment_reader *reader,
                       sxml_node_index element, sxml_qt4_resource *resource) {
    sxml_status status = sxml_qt4_read_attribute(
        reader,
        (sxml_qt4_attribute_name){.element = element,
                                  .local_name = SXML_TEXT("uri")},
        &resource->uri);
    if (status == SXML_STATUS_OK) {
        status =
            sxml_qt4_read_file_attribute(reader, element, &resource->file_path);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_read_attribute(
            reader,
            (sxml_qt4_attribute_name){.element = element,
                                      .local_name = SXML_TEXT("media-type")},
            &resource->media_type);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_read_attribute(
            reader,
            (sxml_qt4_attribute_name){.element = element,
                                      .local_name = SXML_TEXT("encoding")},
            &resource->encoding);
    }
    return status;
}

/* The arrays of one element's children, sized by a count pass. */
typedef struct sxml_qt4_child_arrays {
    sxml_qt4_source *sources;
    sxml_qt4_parameter *parameters;
    sxml_qt4_namespace_declaration *namespace_declarations;
    sxml_qt4_resource *resources;
    sxml_qt4_collection *collections;
    sxml_qt4_collation *collations;
    sxml_qt4_schema *schemas;
    sxml_qt4_decimal_format *decimal_formats;
    sxml_byte_view *queries;
} sxml_qt4_child_arrays;

static sxml_status
sxml_qt4_allocate_child_arrays(sxml_arena *arena,
                               const sxml_qt4_child_counts *counts,
                               sxml_qt4_child_arrays *arrays) {
    void *memory[9] = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
    const sxml_qt4_array_request requests[9] = {
        {counts->source_count, sizeof(sxml_qt4_source),
         alignof(sxml_qt4_source)},
        {counts->parameter_count, sizeof(sxml_qt4_parameter),
         alignof(sxml_qt4_parameter)},
        {counts->namespace_declaration_count,
         sizeof(sxml_qt4_namespace_declaration),
         alignof(sxml_qt4_namespace_declaration)},
        {counts->resource_count, sizeof(sxml_qt4_resource),
         alignof(sxml_qt4_resource)},
        {counts->collection_count, sizeof(sxml_qt4_collection),
         alignof(sxml_qt4_collection)},
        {counts->collation_count, sizeof(sxml_qt4_collation),
         alignof(sxml_qt4_collation)},
        {counts->schema_count, sizeof(sxml_qt4_schema),
         alignof(sxml_qt4_schema)},
        {counts->decimal_format_count, sizeof(sxml_qt4_decimal_format),
         alignof(sxml_qt4_decimal_format)},
        {counts->query_count, sizeof(sxml_byte_view), alignof(sxml_byte_view)},
    };
    for (size_t array_index = 0; array_index < 9; array_index += 1) {
        const sxml_status status = sxml_qt4_allocate_array(
            arena, requests[array_index], &memory[array_index]);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    *arrays = (sxml_qt4_child_arrays){
        .sources = memory[0],
        .parameters = memory[1],
        .namespace_declarations = memory[2],
        .resources = memory[3],
        .collections = memory[4],
        .collations = memory[5],
        .schemas = memory[6],
        .decimal_formats = memory[7],
        .queries = memory[8],
    };
    return SXML_STATUS_OK;
}

/* The attributes of <decimal-format> that are properties, in the order of
   catalog-schema.xsd, with the property each one sets. */
typedef struct sxml_qt4_decimal_format_attribute {
    sxml_byte_view local_name;
    sxml_decimal_format_property_kind property_kind;
} sxml_qt4_decimal_format_attribute;

static const sxml_qt4_decimal_format_attribute
    sxml_qt4_decimal_format_attributes[] = {
        {SXML_BYTE_VIEW_INITIALIZER("decimal-separator"),
         SXML_DECIMAL_FORMAT_PROPERTY_DECIMAL_SEPARATOR},
        {SXML_BYTE_VIEW_INITIALIZER("grouping-separator"),
         SXML_DECIMAL_FORMAT_PROPERTY_GROUPING_SEPARATOR},
        {SXML_BYTE_VIEW_INITIALIZER("zero-digit"),
         SXML_DECIMAL_FORMAT_PROPERTY_ZERO_DIGIT},
        {SXML_BYTE_VIEW_INITIALIZER("digit"),
         SXML_DECIMAL_FORMAT_PROPERTY_DIGIT},
        {SXML_BYTE_VIEW_INITIALIZER("minus-sign"),
         SXML_DECIMAL_FORMAT_PROPERTY_MINUS_SIGN},
        {SXML_BYTE_VIEW_INITIALIZER("percent"),
         SXML_DECIMAL_FORMAT_PROPERTY_PERCENT},
        {SXML_BYTE_VIEW_INITIALIZER("per-mille"),
         SXML_DECIMAL_FORMAT_PROPERTY_PER_MILLE},
        {SXML_BYTE_VIEW_INITIALIZER("pattern-separator"),
         SXML_DECIMAL_FORMAT_PROPERTY_PATTERN_SEPARATOR},
        {SXML_BYTE_VIEW_INITIALIZER("exponent-separator"),
         SXML_DECIMAL_FORMAT_PROPERTY_EXPONENT_SEPARATOR},
        {SXML_BYTE_VIEW_INITIALIZER("infinity"),
         SXML_DECIMAL_FORMAT_PROPERTY_INFINITY},
        {SXML_BYTE_VIEW_INITIALIZER("NaN"), SXML_DECIMAL_FORMAT_PROPERTY_NAN},
};

enum {
    SXML_QT4_DECIMAL_FORMAT_ATTRIBUTE_COUNT =
        sizeof sxml_qt4_decimal_format_attributes /
        sizeof sxml_qt4_decimal_format_attributes[0]
};

/* Splits the xs:QName of a decimal-format name and binds its prefix by the
   element's in-scope namespaces (XSD 1.1 Part 2, section 3.3.18). */
static sxml_status
sxml_qt4_read_decimal_format_name(const sxml_qt4_environment_reader *reader,
                                  sxml_node_index element,
                                  sxml_qt4_decimal_format *decimal_format) {
    const sxml_byte_view name = decimal_format->name;
    decimal_format->local_name = name;
    for (size_t byte_index = 0; byte_index < name.byte_count;
         byte_index += 1) {
        if (name.bytes[byte_index] != ':') {
            continue;
        }
        decimal_format->local_name =
            (sxml_byte_view){.bytes = name.bytes + byte_index + 1,
                             .byte_count = name.byte_count - byte_index - 1};
        sxml_qt4_attribute_value namespace_uri = {.is_present = false,
                                                  .value = sxml_qt4_empty_text};
        const sxml_status status = sxml_qt4_document_resolve_prefix(
            reader->document,
            (sxml_qt4_prefix_lookup){
                .element = element,
                .namespace_prefix = {.bytes = name.bytes,
                                     .byte_count = byte_index}},
            &namespace_uri);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!namespace_uri.is_present || byte_index == 0) {
            return SXML_STATUS_MALFORMED_INPUT;
        }
        decimal_format->namespace_uri = namespace_uri.value;
        break;
    }
    return SXML_STATUS_OK;
}

/* Reads a <decimal-format>: its name and one property per attribute. */
static sxml_status
sxml_qt4_read_decimal_format(const sxml_qt4_environment_reader *reader,
                             sxml_node_index element,
                             sxml_qt4_decimal_format *decimal_format) {
    *decimal_format = (sxml_qt4_decimal_format){
        .namespace_uri = sxml_qt4_empty_text,
        .local_name = sxml_qt4_empty_text,
        .name = sxml_qt4_empty_text,
        .properties = {.decimal_format_properties = NULL,
                       .decimal_format_property_count = 0}};
    sxml_status status = sxml_qt4_read_attribute(
        reader,
        (sxml_qt4_attribute_name){.element = element,
                                  .local_name = SXML_TEXT("name")},
        &decimal_format->name);
    if (status == SXML_STATUS_OK) {
        status =
            sxml_qt4_read_decimal_format_name(reader, element, decimal_format);
    }
    sxml_qt4_attribute_value values[SXML_QT4_DECIMAL_FORMAT_ATTRIBUTE_COUNT] = {
        {.is_present = false, .value = {.bytes = NULL, .byte_count = 0}}};
    size_t property_count = 0;
    for (size_t attribute_index = 0;
         status == SXML_STATUS_OK &&
         attribute_index < SXML_QT4_DECIMAL_FORMAT_ATTRIBUTE_COUNT;
         attribute_index += 1) {
        status = sxml_qt4_document_find_attribute(
            reader->document,
            (sxml_qt4_attribute_lookup){
                .element = element,
                .local_name = sxml_qt4_decimal_format_attributes[attribute_index]
                                  .local_name},
            &values[attribute_index]);
        if (status == SXML_STATUS_OK && values[attribute_index].is_present) {
            property_count += 1;
        }
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_allocate_array(
            reader->arena,
            (sxml_qt4_array_request){
                .element_count = property_count,
                .element_byte_count = sizeof(sxml_decimal_format_property),
                .alignment = alignof(sxml_decimal_format_property)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_decimal_format_property *properties = memory;
    size_t filled_count = 0;
    for (size_t attribute_index = 0;
         attribute_index < SXML_QT4_DECIMAL_FORMAT_ATTRIBUTE_COUNT;
         attribute_index += 1) {
        if (values[attribute_index].is_present) {
            properties[filled_count] = (sxml_decimal_format_property){
                .property_kind =
                    sxml_qt4_decimal_format_attributes[attribute_index]
                        .property_kind,
                .property_value = values[attribute_index].value};
            filled_count += 1;
        }
    }
    decimal_format->properties = (sxml_decimal_format_property_view){
        .decimal_format_properties = properties,
        .decimal_format_property_count = filled_count};
    return SXML_STATUS_OK;
}

/* Reads a <collection>: its sources, resources and queries. A collection
   holds no collection, so this is the deepest level. */
static sxml_status
sxml_qt4_read_collection(const sxml_qt4_environment_reader *reader,
                         sxml_node_index element,
                         sxml_qt4_collection *collection) {
    sxml_qt4_child_counts counts = {0};
    sxml_status status = sxml_qt4_read_attribute(
        reader,
        (sxml_qt4_attribute_name){.element = element,
                                  .local_name = SXML_TEXT("uri")},
        &collection->uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_count_children(reader, element, &counts);
    }
    /* Only the lists a collection may hold (catalog-schema.xsd). */
    const sxml_qt4_child_counts collection_counts = {
        .source_count = counts.source_count,
        .resource_count = counts.resource_count,
        .query_count = counts.query_count};
    sxml_qt4_child_arrays arrays = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_allocate_child_arrays(reader->arena,
                                                &collection_counts, &arrays);
    }
    sxml_qt4_element_cursor cursor = {.document = reader->document,
                                      .has_next_node = false,
                                      .next_node = {.index = 0}};
    if (status == SXML_STATUS_OK) {
        status =
            sxml_qt4_element_cursor_start(reader->document, element, &cursor);
    }
    sxml_qt4_child_counts filled = {0};
    while (status == SXML_STATUS_OK) {
        sxml_qt4_element_step step = {.has_element = false,
                                      .element = {.index = 0},
                                      .local_name = sxml_qt4_empty_text};
        status = sxml_qt4_element_cursor_next(&cursor, &step);
        if (status != SXML_STATUS_OK || !step.has_element) {
            break;
        }
        if (sxml_qt4_is_named(step, SXML_TEXT("source"))) {
            status = sxml_qt4_read_source(reader, step.element,
                                          &arrays.sources[filled.source_count]);
            filled.source_count += 1;
        } else if (sxml_qt4_is_named(step, SXML_TEXT("resource"))) {
            status = sxml_qt4_read_resource(
                reader, step.element, &arrays.resources[filled.resource_count]);
            filled.resource_count += 1;
        } else if (sxml_qt4_is_named(step, SXML_TEXT("query"))) {
            status = sxml_qt4_document_get_text(
                reader->document,
                &(sxml_qt4_text_request){.element = step.element,
                                         .arena = reader->arena},
                &arrays.queries[filled.query_count]);
            filled.query_count += 1;
        }
    }
    if (status == SXML_STATUS_OK) {
        collection->sources = (sxml_qt4_source_view){
            .sources = arrays.sources, .source_count = filled.source_count};
        collection->resources =
            (sxml_qt4_resource_view){.resources = arrays.resources,
                                     .resource_count = filled.resource_count};
        collection->queries = (sxml_qt4_text_view){
            .texts = arrays.queries, .text_count = filled.query_count};
    }
    return status;
}

/* Reads one child of an environment that is not a list entry (context
   item, static base URI, function library, sandpit) into *environment;
   children the driver does not need are skipped. */
static sxml_status
sxml_qt4_read_environment_child(const sxml_qt4_environment_reader *reader,
                                sxml_qt4_element_step step,
                                sxml_qt4_environment *environment) {
    const sxml_node_index element = step.element;
    if (sxml_qt4_is_named(step, SXML_TEXT("context-item"))) {
        environment->has_context_item = true;
        return sxml_qt4_read_attribute(
            reader,
            (sxml_qt4_attribute_name){.element = element,
                                      .local_name = SXML_TEXT("select")},
            &environment->context_item_select);
    }
    if (sxml_qt4_is_named(step, SXML_TEXT("static-base-uri"))) {
        sxml_qt4_attribute_value uri = {.is_present = false,
                                        .value = sxml_qt4_empty_text};
        const sxml_status status = sxml_qt4_document_find_attribute(
            reader->document,
            (sxml_qt4_attribute_lookup){.element = element,
                                        .local_name = SXML_TEXT("uri")},
            &uri);
        if (status == SXML_STATUS_OK && uri.is_present) {
            const bool is_undefined = sxml_byte_view_equals(
                uri.value, SXML_TEXT(SXML_QT4_UNDEFINED_URI));
            environment->static_base_uri_kind =
                is_undefined ? SXML_QT4_STATIC_BASE_URI_ABSENT
                             : SXML_QT4_STATIC_BASE_URI_GIVEN;
            environment->static_base_uri =
                is_undefined ? sxml_qt4_empty_text : uri.value;
        }
        return status;
    }
    if (sxml_qt4_is_named(step, SXML_TEXT("function-library"))) {
        environment->has_function_library = true;
    } else if (sxml_qt4_is_named(step, SXML_TEXT("sandpit"))) {
        environment->has_sandpit = true;
        sxml_byte_view reference = sxml_qt4_empty_text;
        sxml_status status = sxml_qt4_read_attribute(
            reader,
            (sxml_qt4_attribute_name){.element = element,
                                      .local_name = SXML_TEXT("path")},
            &reference);
        if (status == SXML_STATUS_OK && reference.byte_count > 0) {
            status = sxml_qt4_suite_path_resolve(
                &(sxml_qt4_suite_path_request){.containing_file =
                                                   reader->containing_file,
                                               .reference = reference,
                                               .arena = reader->arena},
                &environment->sandpit_path);
        }
        return status;
    }
    return SXML_STATUS_OK;
}

sxml_status
sxml_qt4_environment_read(const sxml_qt4_environment_request *request,
                          sxml_qt4_environment *environment) {
    const sxml_qt4_environment_reader reader = {.document = request->document,
                                                .arena = request->arena,
                                                .containing_file =
                                                    request->containing_file};
    sxml_qt4_child_counts counts = {0};
    sxml_status status =
        sxml_qt4_count_children(&reader, request->element, &counts);
    counts.query_count = 0;
    sxml_qt4_child_arrays arrays = {0};
    if (status == SXML_STATUS_OK) {
        status =
            sxml_qt4_allocate_child_arrays(request->arena, &counts, &arrays);
    }
    sxml_qt4_environment result = sxml_qt4_environment_absent();
    result.origin = request->origin;
    result.name = request->name;
    sxml_qt4_element_cursor cursor = {.document = request->document,
                                      .has_next_node = false,
                                      .next_node = {.index = 0}};
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_element_cursor_start(request->document,
                                               request->element, &cursor);
    }
    sxml_qt4_child_counts filled = {0};
    while (status == SXML_STATUS_OK) {
        sxml_qt4_element_step step = {.has_element = false,
                                      .element = {.index = 0},
                                      .local_name = sxml_qt4_empty_text};
        status = sxml_qt4_element_cursor_next(&cursor, &step);
        if (status != SXML_STATUS_OK || !step.has_element) {
            break;
        }
        const sxml_node_index element = step.element;
        if (sxml_qt4_is_named(step, SXML_TEXT("source"))) {
            status = sxml_qt4_read_source(&reader, element,
                                          &arrays.sources[filled.source_count]);
            filled.source_count += 1;
        } else if (sxml_qt4_is_named(step, SXML_TEXT("param"))) {
            status = sxml_qt4_read_parameter(
                &reader, element, &arrays.parameters[filled.parameter_count]);
            filled.parameter_count += 1;
        } else if (sxml_qt4_is_named(step, SXML_TEXT("namespace"))) {
            sxml_qt4_namespace_declaration *declaration =
                &arrays.namespace_declarations
                     [filled.namespace_declaration_count];
            filled.namespace_declaration_count += 1;
            status = sxml_qt4_read_attribute(
                &reader,
                (sxml_qt4_attribute_name){.element = element,
                                          .local_name = SXML_TEXT("prefix")},
                &declaration->namespace_prefix);
            if (status == SXML_STATUS_OK) {
                status = sxml_qt4_read_attribute(
                    &reader,
                    (sxml_qt4_attribute_name){.element = element,
                                              .local_name = SXML_TEXT("uri")},
                    &declaration->namespace_uri);
            }
        } else if (sxml_qt4_is_named(step, SXML_TEXT("resource"))) {
            status = sxml_qt4_read_resource(
                &reader, element, &arrays.resources[filled.resource_count]);
            filled.resource_count += 1;
        } else if (sxml_qt4_is_named(step, SXML_TEXT("collection"))) {
            status = sxml_qt4_read_collection(
                &reader, element, &arrays.collections[filled.collection_count]);
            filled.collection_count += 1;
        } else if (sxml_qt4_is_named(step, SXML_TEXT("collation"))) {
            sxml_qt4_collation *collation =
                &arrays.collations[filled.collation_count];
            filled.collation_count += 1;
            status = sxml_qt4_read_attribute(
                &reader,
                (sxml_qt4_attribute_name){.element = element,
                                          .local_name = SXML_TEXT("uri")},
                &collation->uri);
            if (status == SXML_STATUS_OK) {
                sxml_byte_view flag_text = sxml_qt4_empty_text;
                status = sxml_qt4_read_attribute(
                    &reader,
                    (sxml_qt4_attribute_name){
                        .element = element, .local_name = SXML_TEXT("default")},
                    &flag_text);
                collation->is_default = sxml_qt4_is_true(flag_text);
            }
        } else if (sxml_qt4_is_named(step, SXML_TEXT("schema"))) {
            sxml_qt4_schema *schema = &arrays.schemas[filled.schema_count];
            filled.schema_count += 1;
            status = sxml_qt4_read_attribute(
                &reader,
                (sxml_qt4_attribute_name){.element = element,
                                          .local_name = SXML_TEXT("uri")},
                &schema->uri);
            if (status == SXML_STATUS_OK) {
                status = sxml_qt4_read_file_attribute(&reader, element,
                                                      &schema->file_path);
            }
        } else if (sxml_qt4_is_named(step, SXML_TEXT("decimal-format"))) {
            status = sxml_qt4_read_decimal_format(
                &reader, element,
                &arrays.decimal_formats[filled.decimal_format_count]);
            filled.decimal_format_count += 1;
        } else {
            status = sxml_qt4_read_environment_child(&reader, step, &result);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    result.sources = (sxml_qt4_source_view){
        .sources = arrays.sources, .source_count = filled.source_count};
    result.parameters =
        (sxml_qt4_parameter_view){.parameters = arrays.parameters,
                                  .parameter_count = filled.parameter_count};
    result.namespace_declarations = (sxml_qt4_namespace_declaration_view){
        .namespace_declarations = arrays.namespace_declarations,
        .namespace_declaration_count = filled.namespace_declaration_count};
    result.resources = (sxml_qt4_resource_view){
        .resources = arrays.resources, .resource_count = filled.resource_count};
    result.collections =
        (sxml_qt4_collection_view){.collections = arrays.collections,
                                   .collection_count = filled.collection_count};
    result.collations =
        (sxml_qt4_collation_view){.collations = arrays.collations,
                                  .collation_count = filled.collation_count};
    result.schemas = (sxml_qt4_schema_view){
        .schemas = arrays.schemas, .schema_count = filled.schema_count};
    result.decimal_formats = (sxml_qt4_decimal_format_view){
        .decimal_formats = arrays.decimal_formats,
        .decimal_format_count = filled.decimal_format_count};
    *environment = result;
    return SXML_STATUS_OK;
}
