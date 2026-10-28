/*
 * Schemas (include/sxml/schema.h): the lifecycle quintet, the compile
 * stages run in order, and the find functions (src/schema_internal.h;
 * docs/DECISIONS.md D165, D167, D170).
 *
 * Golden file: src/schema_components.c (the fixed part in the caller's
 * memory, create and destroy through an allocator) and src/xslt.c (a
 * compilation that fails leaves nothing allocated).
 *
 * Specification: XSD 1.1 Part 1, 4 "Schemas and Namespaces: Access and
 * Composition"; 3.17 "Schemas as a Whole".
 */
#include "schema_internal.h"

#include "processor_internal.h"

#include <stdalign.h>
#include <string.h>

/* One compile stage (src/schema_internal.h, "The compile stages"). */
typedef sxml_status (*compile_stage)(sxml_schema_compilation *compilation);

/* The stages in the order they run; each reads only what those before it
   wrote. A table of plain functions, called in a loop: no stage calls
   another. */
static const compile_stage compile_stages[] = {
    sxml_schema_read_documents,
    sxml_schema_resolve_references,
    sxml_schema_compile_simple_types,
    sxml_schema_compile_complex_types,
    sxml_schema_compile_identity_constraints,
    sxml_schema_compile_assertions,
};

static sxml_status check_configuration(const sxml_schema_configuration *configuration) {
    if (configuration == NULL || configuration->processor == NULL ||
        (configuration->schema_documents.schema_document_count != 0 &&
         configuration->schema_documents.schema_documents == NULL)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    for (size_t document_index = 0;
         document_index < configuration->schema_documents.schema_document_count;
         document_index += 1) {
        const sxml_schema_document *document =
            &configuration->schema_documents.schema_documents[document_index];
        if (document->document_source != SXML_SCHEMA_DOCUMENT_SOURCE_TEXT &&
            document->document_source != SXML_SCHEMA_DOCUMENT_SOURCE_NODE &&
            document->document_source != SXML_SCHEMA_DOCUMENT_SOURCE_RESOLVED) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        if (document->document_source == SXML_SCHEMA_DOCUMENT_SOURCE_TEXT &&
            document->document_text.byte_count != 0 && document->document_text.bytes == NULL) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_schema_memory_requirements(const sxml_schema_configuration *configuration,
                                            sxml_memory_requirements *requirements) {
    sxml_status status = check_configuration(configuration);
    if (status != SXML_STATUS_OK || requirements == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){.byte_count = sizeof(sxml_schema),
                                               .alignment = alignof(sxml_schema)};
    return SXML_STATUS_OK;
}

/* Runs the stages over components (src/schema_internal.h): a stage that
   reported an error ends the compilation after it. */
static sxml_status compile(const sxml_schema_configuration *configuration,
                           sxml_schema_components *components) {
    sxml_schema_compilation compilation = {
        .configuration = configuration, .components = components, .error_count = 0};
    sxml_status status = sxml_schema_components_add_builtin_components(components);
    for (size_t stage_index = 0; status == SXML_STATUS_OK &&
                                 stage_index < sizeof(compile_stages) / sizeof(compile_stages[0]);
         stage_index += 1) {
        status = compile_stages[stage_index](&compilation);
        if (status == SXML_STATUS_OK && compilation.error_count != 0) {
            status = SXML_STATUS_STATIC_ERROR;
        }
    }
    return status;
}

sxml_status sxml_schema_initialize(sxml_memory_view memory,
                                   const sxml_schema_configuration *configuration,
                                   sxml_schema **schema) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_schema_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK && schema == NULL) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_view_check_requirements(memory, requirements);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* The memory was checked for the schema's size and alignment. */
    void *schema_memory = memory.bytes;
    sxml_schema *new_schema = schema_memory;
    memset(new_schema, 0, sizeof(*new_schema));
    new_schema->processor = configuration->processor;
    sxml_schema_components *components = NULL;
    status = sxml_schema_components_initialize(
        (sxml_memory_view){.bytes = (uint8_t *)&new_schema->components,
                           .byte_count = sizeof(new_schema->components)},
        &(sxml_schema_components_configuration){.processor = configuration->processor},
        &components);
    if (status == SXML_STATUS_OK) {
        status = compile(configuration, components);
    }
    if (status != SXML_STATUS_OK) {
        sxml_schema_components_finalize(&new_schema->components);
        return status;
    }
    sxml_schema_components_seal(components);
    /* A schema is a validation, which reads source locations (D313):
       documents parsed after it keep them under AUTOMATIC. */
    sxml_processor_note_program(configuration->processor, true);
    *schema = new_schema;
    return SXML_STATUS_OK;
}

void sxml_schema_finalize(sxml_schema *schema) {
    if (schema == NULL) {
        return;
    }
    sxml_schema_components_finalize(&schema->components);
    const sxml_allocator creating_allocator = schema->creating_allocator;
    memset(schema, 0, sizeof(*schema));
    schema->creating_allocator = creating_allocator;
}

sxml_status sxml_schema_create(sxml_allocator allocator,
                               const sxml_schema_configuration *configuration,
                               sxml_schema **schema) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status = sxml_schema_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK && schema == NULL) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status =
            sxml_allocator_allocate(allocator,
                                    (sxml_allocation_request){.byte_count = requirements.byte_count,
                                                              .alignment = requirements.alignment},
                                    &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_schema *new_schema = NULL;
    status = sxml_schema_initialize(
        (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count}, configuration,
        &new_schema);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_schema->creating_allocator = allocator;
    *schema = new_schema;
    return SXML_STATUS_OK;
}

void sxml_schema_destroy(sxml_schema *schema) {
    if (schema == NULL) {
        return;
    }
    const sxml_allocator creating_allocator = schema->creating_allocator;
    sxml_schema_finalize(schema);
    sxml_allocator_release(creating_allocator, schema);
}

/* ---------------------------------------------------------- Components */

/* The global component of name in symbol_space, or 0. */
typedef struct global_lookup {
    sxml_schema_symbol_space symbol_space;
    sxml_name_identifier name;
} global_lookup;

static uint32_t find_global(const sxml_schema *schema, global_lookup lookup) {
    uint32_t component = 0;
    if (schema == NULL || lookup.name.index == 0 ||
        !sxml_schema_components_find_name(
            &schema->components,
            (sxml_schema_name_key){.symbol_space = lookup.symbol_space, .name = lookup.name},
            &component)) {
        return 0;
    }
    return component;
}

/* The registered type of a type definition; SXML_TYPE_NONE before
   SIMPLE_TYPES and COMPLEX_TYPES register it. */
static sxml_type_identifier registered_type_of(const sxml_schema *schema, uint32_t type) {
    return sxml_schema_get_type_definition(&schema->components,
                                           (sxml_schema_type_definition_index){.value = type})
        ->registered_type;
}

bool sxml_schema_find_type(const sxml_schema *schema, sxml_name_identifier type_name,
                           sxml_type_identifier *type) {
    const uint32_t component = find_global(
        schema, (global_lookup){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION,
                                .name = type_name});
    if (component == 0 || type == NULL) {
        return false;
    }
    const sxml_type_identifier registered = registered_type_of(schema, component);
    if (registered.index == (uint32_t)SXML_TYPE_NONE) {
        return false;
    }
    *type = registered;
    return true;
}

bool sxml_schema_find_element_declaration(const sxml_schema *schema,
                                          sxml_name_identifier element_name,
                                          sxml_element_declaration_description *description) {
    const uint32_t component = find_global(
        schema, (global_lookup){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION,
                                .name = element_name});
    if (component == 0 || description == NULL) {
        return false;
    }
    const sxml_schema_element_declaration *declaration = sxml_schema_get_element_declaration(
        &schema->components, (sxml_schema_element_declaration_index){.value = component});
    *description = (sxml_element_declaration_description){
        .element_name = declaration->element_name,
        .declared_type = registered_type_of(schema, declaration->type_definition.value),
        .is_nillable = declaration->is_nillable,
        .is_abstract = declaration->is_abstract};
    return true;
}

bool sxml_schema_find_attribute_declaration(const sxml_schema *schema,
                                            sxml_name_identifier attribute_name,
                                            sxml_attribute_declaration_description *description) {
    const uint32_t component = find_global(
        schema, (global_lookup){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_DECLARATION,
                                .name = attribute_name});
    if (component == 0 || description == NULL) {
        return false;
    }
    const sxml_schema_attribute_declaration *declaration = sxml_schema_get_attribute_declaration(
        &schema->components, (sxml_schema_attribute_declaration_index){.value = component});
    *description = (sxml_attribute_declaration_description){
        .attribute_name = declaration->attribute_name,
        .declared_type = registered_type_of(schema, declaration->type_definition.value)};
    return true;
}
