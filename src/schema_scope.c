/*
 * The in-scope schema definitions of a static context
 * (src/schema_scope_internal.h, D174). Golden: src/schema.c (the find
 * functions), src/function_library_error.c (raised errors).
 */
#include "schema_scope_internal.h"

#include "schema_internal.h"

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
    const sxml_byte_view constraint = record->code.local_name;
    const sxml_byte_view description = record->diagnostic.description;
    static const sxml_byte_view separator = SXML_BYTE_VIEW_INITIALIZER(": ");
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
