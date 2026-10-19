/*
 * The in-scope schema definitions of a static context
 * (src/schema_scope_internal.h, D174). Golden: src/schema.c (the find
 * functions), src/function_library_error.c (raised errors).
 */
#include "schema_scope_internal.h"

#include "processor_internal.h"
#include "memory_internal.h"
#include "schema_internal.h"
#include "xdm_internal.h"

#include <stdalign.h>
#include <string.h>

bool sxml_schema_scope_find_element(sxml_schema_view schemas, sxml_name_identifier element_name,
                                    sxml_schema_scope_element *found) {
    for (size_t schema_index = 0; schema_index < schemas.schema_count; schema_index += 1) {
        sxml_element_declaration_description description;
        if (sxml_schema_find_element_declaration(schemas.schemas[schema_index], element_name,
                                                 &description)) {
            *found = (sxml_schema_scope_element){.schema = schemas.schemas[schema_index],
                                                 .description = description};
            return true;
        }
    }
    return false;
}

bool sxml_schema_scope_find_attribute(sxml_schema_view schemas,
                                      sxml_name_identifier attribute_name,
                                      sxml_schema_scope_attribute *found) {
    for (size_t schema_index = 0; schema_index < schemas.schema_count; schema_index += 1) {
        sxml_attribute_declaration_description description;
        if (sxml_schema_find_attribute_declaration(schemas.schemas[schema_index],
                                                   attribute_name, &description)) {
            *found = (sxml_schema_scope_attribute){.schema = schemas.schemas[schema_index],
                                                   .description = description};
            return true;
        }
    }
    return false;
}

bool sxml_schema_scope_find_type(sxml_schema_view schemas, sxml_name_identifier type_name,
                                 sxml_schema_scope_type *found) {
    for (size_t schema_index = 0; schema_index < schemas.schema_count; schema_index += 1) {
        sxml_type_identifier type = {.index = SXML_TYPE_NONE};
        if (sxml_schema_find_type(schemas.schemas[schema_index], type_name, &type)) {
            *found = (sxml_schema_scope_type){.schema = schemas.schemas[schema_index],
                                              .type = type};
            return true;
        }
    }
    return false;
}

const sxml_schema *sxml_schema_scope_find_type_owner(sxml_schema_view schemas,
                                                     sxml_type_identifier type) {
    for (size_t schema_index = 0; schema_index < schemas.schema_count; schema_index += 1) {
        const sxml_schema_components *components = &schemas.schemas[schema_index]->components;
        const uint32_t type_count =
            components->tables[SXML_SCHEMA_TABLE_TYPE_DEFINITION].entry_count;
        for (uint32_t type_index = 1; type_index < type_count; type_index += 1) {
            if (sxml_schema_get_type_definition(
                    components, (sxml_schema_type_definition_index){.value = type_index})
                    ->registered_type.index == type.index) {
                return schemas.schemas[schema_index];
            }
        }
    }
    return NULL;
}

/* ------------------------------------------- Content for element-to-map */

/* The complex type definition registered as type in a schema of
   schemas, and the schema's components; NULL when there is none. */
typedef struct registered_complex_type {
    const sxml_schema_components *components;
    const sxml_schema_type_definition *definition;
} registered_complex_type;

static registered_complex_type find_registered_complex_type(sxml_schema_view schemas,
                                                            sxml_type_identifier type) {
    for (size_t schema_index = 0; schema_index < schemas.schema_count; schema_index += 1) {
        const sxml_schema_components *components = &schemas.schemas[schema_index]->components;
        const uint32_t type_count =
            components->tables[SXML_SCHEMA_TABLE_TYPE_DEFINITION].entry_count;
        for (uint32_t type_index = 1; type_index < type_count; type_index += 1) {
            const sxml_schema_type_definition *definition = sxml_schema_get_type_definition(
                components, (sxml_schema_type_definition_index){.value = type_index});
            if (definition->registered_type.index == type.index &&
                definition->variety == SXML_TYPE_VARIETY_COMPLEX) {
                return (registered_complex_type){.components = components,
                                                 .definition = definition};
            }
        }
    }
    return (registered_complex_type){.components = NULL, .definition = NULL};
}

/* A particle to visit and the product of the maxOccurs above it. */
typedef struct content_visit {
    sxml_schema_particle_index particle;
    uint64_t occurrences;
} content_visit;

/* One element name of the particle and how often it may occur. */
typedef struct content_name {
    sxml_name_identifier name;
    uint64_t occurrences;
} content_name;

static uint64_t multiply_occurrences(uint64_t first, uint64_t second) {
    if (first == SXML_SCHEMA_UNBOUNDED || second == SXML_SCHEMA_UNBOUNDED) {
        return first == 0 || second == 0 ? 0 : SXML_SCHEMA_UNBOUNDED;
    }
    return second != 0 && first > (SXML_SCHEMA_UNBOUNDED - 1) / second ? SXML_SCHEMA_UNBOUNDED
                                                                        : first * second;
}

static uint64_t add_occurrences(uint64_t first, uint64_t second) {
    return first >= SXML_SCHEMA_UNBOUNDED - second ? SXML_SCHEMA_UNBOUNDED : first + second;
}

/* The work of describe_content: the visits to make and the names seen,
   each array as large as its table (no model group contains itself, so
   no particle is pending twice on one path). */
typedef struct content_walk {
    const sxml_schema_components *components;
    content_visit *visits;
    size_t visit_count;
    size_t visit_capacity;
    content_name *names;
    size_t name_count;
    size_t name_capacity;
} content_walk;

/* Visits one particle: an element term adds to its name, a model group
   pushes its particles, a wildcard is noted. */
static void visit_particle(content_walk *walk, content_visit visit,
                           sxml_schema_scope_content *content) {
    const sxml_schema_particle *particle = sxml_schema_get_particle(walk->components, visit.particle);
    const uint64_t occurrences =
        multiply_occurrences(visit.occurrences, particle->maximum_occurrences);
    if (particle->term_kind == SXML_SCHEMA_TERM_WILDCARD) {
        content->has_wildcards = true;
    } else if (particle->term_kind == SXML_SCHEMA_TERM_ELEMENT_DECLARATION) {
        const sxml_name_identifier name =
            sxml_schema_get_element_declaration(
                walk->components, (sxml_schema_element_declaration_index){.value = particle->term})
                ->element_name;
        for (size_t name_index = 0; name_index < walk->name_count; name_index += 1) {
            if (walk->names[name_index].name.index == name.index) {
                walk->names[name_index].occurrences =
                    add_occurrences(walk->names[name_index].occurrences, occurrences);
                return;
            }
        }
        if (walk->name_count < walk->name_capacity) {
            walk->names[walk->name_count] =
                (content_name){.name = name, .occurrences = occurrences};
            walk->name_count += 1;
        }
    } else if (particle->term_kind == SXML_SCHEMA_TERM_MODEL_GROUP) {
        const sxml_schema_index_view particles = sxml_schema_get_list(
            walk->components,
            sxml_schema_get_model_group(walk->components,
                                        (sxml_schema_model_group_index){.value = particle->term})
                ->particles);
        for (size_t particle_index = 0; particle_index < particles.index_count &&
                                        walk->visit_count < walk->visit_capacity;
             particle_index += 1) {
            walk->visits[walk->visit_count] = (content_visit){
                .particle = {.value = particles.indexes[particle_index]},
                .occurrences = occurrences};
            walk->visit_count += 1;
        }
    }
}

sxml_status sxml_schema_scope_describe_content(const sxml_schema_scope_content_request *request,
                                               sxml_schema_scope_content *content) {
    *content = (sxml_schema_scope_content){.is_found = false};
    const registered_complex_type found =
        find_registered_complex_type(request->schemas, request->type);
    if (found.definition == NULL) {
        return SXML_STATUS_OK;
    }
    const sxml_schema_type_definition *definition = found.definition;
    content->is_found = true;
    content->has_attributes =
        definition->attribute_uses.entry_count > 0 || definition->attribute_wildcard.value != 0;
    if (definition->content_type_variety != SXML_CONTENT_TYPE_VARIETY_ELEMENT_ONLY) {
        return SXML_STATUS_OK;
    }
    content->has_wildcards = definition->open_content_wildcard.value != 0;
    content_walk walk = {
        .components = found.components,
        .visit_capacity = found.components->tables[SXML_SCHEMA_TABLE_PARTICLE].entry_count + 1,
        .name_capacity =
            found.components->tables[SXML_SCHEMA_TABLE_ELEMENT_DECLARATION].entry_count + 1};
    void *visit_memory = NULL;
    void *name_memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        request->allocator,
        (sxml_array_allocation_request){.element_count = walk.visit_capacity,
                                        .element_size = sizeof(content_visit),
                                        .element_alignment = alignof(content_visit)},
        &visit_memory);
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(
            request->allocator,
            (sxml_array_allocation_request){.element_count = walk.name_capacity,
                                            .element_size = sizeof(content_name),
                                            .element_alignment = alignof(content_name)},
            &name_memory);
    }
    walk.visits = visit_memory;
    walk.names = name_memory;
    if (status == SXML_STATUS_OK && definition->particle.value != 0) {
        walk.visits[0] = (content_visit){.particle = definition->particle, .occurrences = 1};
        walk.visit_count = 1;
    }
    while (status == SXML_STATUS_OK && walk.visit_count > 0) {
        walk.visit_count -= 1;
        visit_particle(&walk, walk.visits[walk.visit_count], content);
    }
    if (status == SXML_STATUS_OK) {
        content->child_name_count = walk.name_count;
        content->has_names_at_most_once = true;
        for (size_t name_index = 0; name_index < walk.name_count; name_index += 1) {
            if (walk.names[name_index].occurrences > 1) {
                content->has_repeated_name = true;
                content->has_names_at_most_once = false;
            }
        }
        if (walk.name_count == 1) {
            content->child_name = walk.names[0].name;
        }
    }
    sxml_allocator_release(request->allocator, visit_memory);
    sxml_allocator_release(request->allocator, name_memory);
    return status;
}

/* The one parameter of every constructor function: xs:T($value as
   xs:anyAtomicType? := .), as src/xpath_lower.c names it. */
static const sxml_function_parameter constructor_parameters[] = {
    {.parameter_name = SXML_BYTE_VIEW_INITIALIZER("value"),
     .parameter_type = SXML_BYTE_VIEW_INITIALIZER("xs:anyAtomicType?"),
     .parameter_default = SXML_PARAMETER_DEFAULT_CONTEXT_VALUE}};

static const sxml_sequence_type constructor_parameter_type = {
    .item_type = {.index = SXML_TYPE_XS_ANY_ATOMIC_TYPE},
    .occurrence = SXML_OCCURRENCE_ZERO_OR_ONE};

sxml_status sxml_schema_scope_find_constructor(
    const sxml_schema_scope_constructor_request *request, sxml_function_entry *entry) {
    sxml_schema_scope_type found;
    if (request->query->arity != 1 ||
        !sxml_schema_scope_find_type(request->schemas, request->query->function_name, &found) ||
        found.type.index < SXML_TYPE_ITEM) {
        return SXML_STATUS_OK;
    }
    sxml_type_description description;
    sxml_status status =
        sxml_type_registry_describe_type(request->type_registry, found.type, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence_type result_type = {.item_type = found.type,
                                      .occurrence = SXML_OCCURRENCE_ZERO_OR_ONE};
    if (description.variety == SXML_TYPE_VARIETY_LIST) {
        result_type = (sxml_sequence_type){.item_type = description.variety_details.list_item_type,
                                           .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};
    } else if (description.variety != SXML_TYPE_VARIETY_ATOMIC &&
               description.variety != SXML_TYPE_VARIETY_UNION) {
        return SXML_STATUS_OK;
    }
    sxml_function_entry made = {
        .entry_kind = SXML_FUNCTION_ENTRY_KIND_CONSTRUCTOR,
        .native_function = NULL,
        .constructor_type = found.type,
        .ir_function_number = 0,
        .parameters = {.function_parameters = constructor_parameters,
                       .function_parameter_count = 1},
        .signature = {.index = SXML_TYPE_NONE},
        .parameter_types = {.sequence_types = &constructor_parameter_type,
                            .sequence_type_count = 1},
        .result_type = result_type};
    status = sxml_type_registry_register_item_type(
        request->type_registry,
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_FUNCTION,
            .variety_details = {.function_type = {.is_any_function = false,
                                                  .parameter_types = made.parameter_types,
                                                  .result_type = made.result_type}}},
        &made.signature);
    if (status == SXML_STATUS_OK) {
        *entry = made;
    }
    return status;
}

bool sxml_schema_scope_defines_namespace(const sxml_schema *schema,
                                         sxml_namespace_uri_identifier namespace_uri) {
    const sxml_schema_components *components = &schema->components;
    const uint32_t document_count = components->tables[SXML_SCHEMA_TABLE_DOCUMENT].entry_count;
    for (uint32_t document_index = 1; document_index < document_count; document_index += 1) {
        if (sxml_schema_get_document(components,
                                     (sxml_schema_document_index){.value = document_index})
                ->target_namespace.index == namespace_uri.index) {
            return true;
        }
    }
    return false;
}

sxml_status sxml_schema_scope_probe_document(const sxml_schema_scope_probe *request,
                                             sxml_schema_scope_delivery *delivery) {
    delivery->is_delivered = false;
    sxml_processor *processor = request->processor;
    const sxml_schema_document *document = &request->document;
    if (document->document_uri.byte_count == 0 && document->checks_target_namespace &&
        (sxml_byte_view_equals(document->target_namespace_uri,
                               SXML_BYTE_VIEW_FROM_LITERAL(
                                   "http://www.w3.org/XML/1998/namespace")) ||
         sxml_byte_view_equals(document->target_namespace_uri,
                               SXML_BYTE_VIEW_FROM_LITERAL(
                                   "http://www.w3.org/2005/xpath-functions")))) {
        /* Every processor knows these documents (F&O 4.0, appendix D). */
        delivery->is_delivered = true;
        return SXML_STATUS_OK;
    }
    const sxml_resource_resolver resolver = processor->resource_resolver;
    if (resolver.operations == NULL || resolver.operations->resolve == NULL) {
        return SXML_STATUS_OK;
    }
    sxml_uri_resolution resolution = {.uri = {.bytes = NULL, .byte_count = 0}, .memory = NULL};
    sxml_status status = SXML_STATUS_OK;
    if (document->document_uri.byte_count > 0) {
        status = sxml_uri_resolve(&(sxml_uri_resolution_request){.reference = document->document_uri,
                                                                 .base_uri = request->base_uri,
                                                                 .allocator = processor->allocator},
                                  &resolution);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_resource resource;
    memset(&resource, 0, sizeof resource);
    status = resolver.operations->resolve(
        resolver.state,
        &(sxml_resource_request){.resource_kind = SXML_RESOURCE_KIND_SCHEMA_DOCUMENT,
                                 .uri_reference = document->document_uri,
                                 .absolute_uri = resolution.uri,
                                 .processor = processor,
                                 .target_namespace_uri = document->target_namespace_uri},
        &resource);
    if (status == SXML_STATUS_OK) {
        delivery->is_delivered = resource.resource_form != SXML_RESOURCE_FORM_NONE;
        if (resource.resource_form == SXML_RESOURCE_FORM_DOCUMENT_NODE) {
            sxml_item_release(resource.document_node);
        }
        if (resolver.operations->release != NULL) {
            resolver.operations->release(resolver.state, &resource);
        }
    }
    sxml_allocator_release(processor->allocator, resolution.memory);
    return status;
}

/* The global declaration of head, by its name. */
static const sxml_schema_element_declaration *
head_declaration(const sxml_schema_scope_element *head) {
    uint32_t component = 0;
    if (!sxml_schema_components_find_name(
            &head->schema->components,
            (sxml_schema_name_key){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION,
                                   .name = head->description.element_name},
            &component)) {
        return NULL;
    }
    return sxml_schema_get_element_declaration(
        &head->schema->components, (sxml_schema_element_declaration_index){.value = component});
}

size_t sxml_schema_scope_member_count(const sxml_schema_scope_element *head) {
    const sxml_schema_element_declaration *declaration = head_declaration(head);
    if (declaration == NULL) {
        return 0;
    }
    return 1 + declaration->substitution_group_members.entry_count;
}

bool sxml_schema_scope_get_member(const sxml_schema_scope_member_request *request,
                                  sxml_element_declaration_description *member) {
    const sxml_schema_element_declaration *head = head_declaration(&request->head);
    if (head == NULL) {
        return false;
    }
    const sxml_schema_components *components = &request->head.schema->components;
    const sxml_schema_element_declaration *declaration = head;
    if (request->member_index > 0) {
        /* XSD 1.1 Part 1, 3.3.4.3: {disallowed substitutions} with
           substitution blocks every member. */
        const sxml_schema_index_view members =
            sxml_schema_get_list(components, head->substitution_group_members);
        if ((head->disallowed_substitutions & SXML_SCHEMA_DERIVATION_SUBSTITUTION) != 0 ||
            request->member_index > members.index_count) {
            return false;
        }
        declaration = sxml_schema_get_element_declaration(
            components, (sxml_schema_element_declaration_index){
                            .value = members.indexes[request->member_index - 1]});
    }
    if (declaration->is_abstract) {
        return false;
    }
    return sxml_schema_find_element_declaration(request->head.schema, declaration->element_name,
                                                member);
}

sxml_status sxml_schema_scope_raise(const sxml_schema_scope_error *request) {
    const sxml_error_record *record = request->schema_error;
    sxml_byte_view constraint = record->code.local_name;
    const sxml_byte_view description = record->diagnostic.description;
    sxml_byte_view separator = SXML_BYTE_VIEW_FROM_LITERAL(": ");
    if (description.byte_count > constraint.byte_count &&
        memcmp(description.bytes, constraint.bytes, constraint.byte_count) == 0 &&
        description.bytes[constraint.byte_count] == ':') {
        /* The description names its constraint already (D173's facet
           errors): not twice. */
        constraint = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
        separator = constraint;
    }
    const size_t byte_count = constraint.byte_count + separator.byte_count + description.byte_count;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        request->allocator, (sxml_allocation_request){.byte_count = byte_count, .alignment = 1},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *bytes = memory;
    size_t written = 0;
    const sxml_byte_view parts[3] = {constraint, separator, description};
    for (size_t part_index = 0; part_index < 3; part_index += 1) {
        if (parts[part_index].byte_count > 0) {
            memcpy(bytes + written, parts[part_index].bytes, parts[part_index].byte_count);
            written += parts[part_index].byte_count;
        }
    }
    const sxml_source_location location =
        request->location.line_number != 0 ? request->location : record->diagnostic.location;
    status = sxml_item_heap_raise_error(
        request->item_heap,
        &(sxml_error_raise_request){
            .error_status = request->error_status,
            .code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(SXML_ERROR_NAMESPACE_URI),
                     .local_name = request->error_code,
                     .namespace_prefix = SXML_BYTE_VIEW_FROM_LITERAL("err")},
            .description = {.bytes = bytes, .byte_count = byte_count},
            .has_value = false,
            .value = sxml_sequence_empty(),
            .location = location},
        request->error_record);
    sxml_allocator_release(request->allocator, memory);
    return status;
}
