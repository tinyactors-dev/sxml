/*
 * The schema components: lifecycle, tables, name index, blocks, built-in
 * components and error reports (src/schema_internal.h; docs/DECISIONS.md
 * D165, D167).
 *
 * Golden file: src/xslt_ast.c (the fixed part in the caller's memory;
 * arrays that grow geometrically through an allocator with entry 0
 * reserved; sealing). The tables differ only in their entry type, so one
 * function grows them all, by the table's entry size.
 *
 * Specification: XSD 1.1 Part 1, 2.6 "Names and Symbol Spaces", 3.2.7
 * "Built-in Attribute Declarations", 3.4.7 "Built-in Complex Type
 * Definition", 3.16.7 "Built-in Simple Type Definitions".
 */
#include "schema_internal.h"

#include "processor_internal.h"
#include "types_internal.h"

#include <stdalign.h>
#include <string.h>

/* --------------------------------------------------------------- Tables */

/* The entry type of a table, by size and alignment. */
typedef struct table_layout {
    size_t entry_byte_count;
    size_t entry_alignment;
} table_layout;

#define TABLE_LAYOUT(entry_type) {sizeof(entry_type), alignof(entry_type)}

static const table_layout table_layouts[SXML_SCHEMA_TABLE_COUNT] = {
    [SXML_SCHEMA_TABLE_TYPE_DEFINITION] = TABLE_LAYOUT(sxml_schema_type_definition),
    [SXML_SCHEMA_TABLE_ELEMENT_DECLARATION] =
        TABLE_LAYOUT(sxml_schema_element_declaration),
    [SXML_SCHEMA_TABLE_ATTRIBUTE_DECLARATION] =
        TABLE_LAYOUT(sxml_schema_attribute_declaration),
    [SXML_SCHEMA_TABLE_ATTRIBUTE_USE] = TABLE_LAYOUT(sxml_schema_attribute_use),
    [SXML_SCHEMA_TABLE_ATTRIBUTE_GROUP] = TABLE_LAYOUT(sxml_schema_attribute_group),
    [SXML_SCHEMA_TABLE_MODEL_GROUP_DEFINITION] =
        TABLE_LAYOUT(sxml_schema_model_group_definition),
    [SXML_SCHEMA_TABLE_MODEL_GROUP] = TABLE_LAYOUT(sxml_schema_model_group),
    [SXML_SCHEMA_TABLE_PARTICLE] = TABLE_LAYOUT(sxml_schema_particle),
    [SXML_SCHEMA_TABLE_WILDCARD] = TABLE_LAYOUT(sxml_schema_wildcard),
    [SXML_SCHEMA_TABLE_IDENTITY_CONSTRAINT] =
        TABLE_LAYOUT(sxml_schema_identity_constraint),
    [SXML_SCHEMA_TABLE_TYPE_ALTERNATIVE] = TABLE_LAYOUT(sxml_schema_type_alternative),
    [SXML_SCHEMA_TABLE_ASSERTION] = TABLE_LAYOUT(sxml_schema_assertion),
    [SXML_SCHEMA_TABLE_NOTATION_DECLARATION] =
        TABLE_LAYOUT(sxml_schema_notation_declaration),
    [SXML_SCHEMA_TABLE_FACET] = TABLE_LAYOUT(sxml_schema_facet),
    [SXML_SCHEMA_TABLE_XPATH_RECORD] = TABLE_LAYOUT(sxml_schema_xpath_record),
    [SXML_SCHEMA_TABLE_NAMESPACE_CONTEXT] =
        TABLE_LAYOUT(sxml_schema_namespace_context),
    [SXML_SCHEMA_TABLE_REFERENCE] = TABLE_LAYOUT(sxml_schema_reference),
    [SXML_SCHEMA_TABLE_DOCUMENT] = TABLE_LAYOUT(sxml_schema_document_entry),
    [SXML_SCHEMA_TABLE_LIST_ENTRY] = TABLE_LAYOUT(uint32_t),
    [SXML_SCHEMA_TABLE_NAMESPACE_BINDING] = TABLE_LAYOUT(sxml_namespace_binding),
    [SXML_SCHEMA_TABLE_TEXT_BYTE] = TABLE_LAYOUT(uint8_t),
};

/* The most entries of one table: every index fits uint32_t with room for
   one past the end. */
#define MAXIMUM_TABLE_ENTRY_COUNT ((uint64_t)UINT32_MAX - 1U)

/* The first capacity of a table that grows. */
#define FIRST_TABLE_CAPACITY 16U

/* Makes room in storage for required_count entries, doubling. */
static sxml_status grow_table(sxml_schema_components *components,
                              sxml_schema_table table, uint32_t required_count) {
    sxml_schema_table_storage *storage = &components->tables[table];
    if (required_count <= storage->entry_capacity) {
        return SXML_STATUS_OK;
    }
    uint64_t capacity = storage->entry_capacity < FIRST_TABLE_CAPACITY
                            ? FIRST_TABLE_CAPACITY
                            : storage->entry_capacity;
    while (capacity < required_count) {
        capacity *= 2U;
    }
    if (capacity > MAXIMUM_TABLE_ENTRY_COUNT + 1U) {
        capacity = MAXIMUM_TABLE_ENTRY_COUNT + 1U;
    }
    const table_layout layout = table_layouts[table];
    size_t byte_count = 0;
    sxml_status status =
        sxml_size_multiply((size_t)capacity, layout.entry_byte_count, &byte_count);
    void *entries = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            components->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = layout.entry_alignment},
            &entries);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (storage->entries != NULL) {
        memcpy(entries, storage->entries,
               (size_t)storage->entry_count * layout.entry_byte_count);
        sxml_allocator_release(components->allocator, storage->entries);
    }
    storage->entries = entries;
    storage->entry_capacity = (uint32_t)capacity;
    return SXML_STATUS_OK;
}

sxml_status sxml_schema_components_add_entries(sxml_schema_components *components,
                                               const sxml_schema_entry_request *request,
                                               uint32_t *first_index) {
    if (components->is_sealed || request->entry_count == 0 ||
        request->table >= SXML_SCHEMA_TABLE_COUNT || request->entries == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_schema_table_storage *storage = &components->tables[request->table];
    /* Entry 0 is reserved on the first addition. */
    const uint32_t start_index = storage->entry_count == 0 ? 1U : storage->entry_count;
    const uint64_t required_count = (uint64_t)start_index + request->entry_count;
    if (required_count > MAXIMUM_TABLE_ENTRY_COUNT) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    sxml_status status = grow_table(components, request->table, (uint32_t)required_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const size_t entry_byte_count = table_layouts[request->table].entry_byte_count;
    uint8_t *entry_bytes = storage->entries;
    if (storage->entry_count == 0) {
        memset(entry_bytes, 0, entry_byte_count);
    }
    memcpy(entry_bytes + (size_t)start_index * entry_byte_count, request->entries,
           (size_t)request->entry_count * entry_byte_count);
    storage->entry_count = (uint32_t)required_count;
    *first_index = start_index;
    return SXML_STATUS_OK;
}

void *sxml_schema_components_edit_entry(sxml_schema_components *components,
                                        sxml_schema_entry_key key) {
    if (components->is_sealed || key.table >= SXML_SCHEMA_TABLE_COUNT) {
        return NULL;
    }
    sxml_schema_table_storage *storage = &components->tables[key.table];
    if (key.index == 0 || key.index >= storage->entry_count) {
        return NULL;
    }
    uint8_t *entry_bytes = storage->entries;
    return entry_bytes + (size_t)key.index * table_layouts[key.table].entry_byte_count;
}

/* A run of entries for one of the run tables. */
typedef struct run_request {
    sxml_schema_table table;
    const void *entries;
    size_t entry_count;
} run_request;

/* Appends a run; an empty one is index 0 and adds nothing. */
static sxml_status add_run(sxml_schema_components *components, run_request request,
                           uint32_t *first_index) {
    if (request.entry_count == 0) {
        *first_index = 0;
        return SXML_STATUS_OK;
    }
    if (request.entry_count > MAXIMUM_TABLE_ENTRY_COUNT) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    return sxml_schema_components_add_entries(
        components,
        &(sxml_schema_entry_request){.table = request.table,
                                     .entries = request.entries,
                                     .entry_count = (uint32_t)request.entry_count},
        first_index);
}

sxml_status sxml_schema_components_add_list(sxml_schema_components *components,
                                            sxml_schema_index_view indexes,
                                            sxml_schema_list *list) {
    uint32_t first_index = 0;
    sxml_status status = add_run(components,
                                 (run_request){.table = SXML_SCHEMA_TABLE_LIST_ENTRY,
                                               .entries = indexes.indexes,
                                               .entry_count = indexes.index_count},
                                 &first_index);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *list = (sxml_schema_list){.first_entry_index = first_index,
                               .entry_count = (uint32_t)indexes.index_count};
    return SXML_STATUS_OK;
}

sxml_status sxml_schema_components_add_text(sxml_schema_components *components,
                                            sxml_byte_view bytes, sxml_schema_text *text) {
    uint32_t first_index = 0;
    sxml_status status = add_run(components,
                                 (run_request){.table = SXML_SCHEMA_TABLE_TEXT_BYTE,
                                               .entries = bytes.bytes,
                                               .entry_count = bytes.byte_count},
                                 &first_index);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *text = (sxml_schema_text){.byte_offset = first_index,
                               .byte_count = (uint32_t)bytes.byte_count};
    return SXML_STATUS_OK;
}

sxml_status sxml_schema_components_add_namespace_bindings(
    sxml_schema_components *components, sxml_namespace_binding_view bindings,
    sxml_schema_namespace_binding_list *list) {
    uint32_t first_index = 0;
    sxml_status status =
        add_run(components,
                (run_request){.table = SXML_SCHEMA_TABLE_NAMESPACE_BINDING,
                              .entries = bindings.namespace_bindings,
                              .entry_count = bindings.namespace_binding_count},
                &first_index);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *list = (sxml_schema_namespace_binding_list){
        .first_binding_index = first_index,
        .binding_count = (uint32_t)bindings.namespace_binding_count};
    return SXML_STATUS_OK;
}

/* ----------------------------------------------------------- Name index */

/* The first slot to probe for a key. */
static uint32_t hash_name_key(sxml_schema_name_key key, uint32_t slot_count) {
    const uint32_t hash = (key.name.index * 2654435761U) ^
                          ((uint32_t)key.symbol_space * 2246822519U);
    return hash & (slot_count - 1U);
}

/* The slot of key, or of the free slot where it would go. slot_count is
   never 0 here and some slot is always free (at most half are used). */
static uint32_t find_name_slot(const sxml_schema_components *components,
                               sxml_schema_name_key key) {
    uint32_t slot = hash_name_key(key, components->name_slot_count);
    for (;;) {
        const sxml_schema_name_entry *entry = &components->name_entries[slot];
        if (entry->component == 0 ||
            (entry->symbol_space == key.symbol_space && entry->name.index == key.name.index)) {
            return slot;
        }
        slot = (slot + 1U) & (components->name_slot_count - 1U);
    }
}

/* Doubles the name index (16 slots at first) and enters every entry
   again. */
static sxml_status grow_name_index(sxml_schema_components *components) {
    const uint32_t old_slot_count = components->name_slot_count;
    if (old_slot_count > UINT32_MAX / 2U) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    const uint32_t new_slot_count = old_slot_count == 0 ? 16U : old_slot_count * 2U;
    size_t byte_count = 0;
    sxml_status status =
        sxml_size_multiply(new_slot_count, sizeof(sxml_schema_name_entry), &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            components->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(sxml_schema_name_entry)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(memory, 0, byte_count);
    sxml_schema_name_entry *old_entries = components->name_entries;
    components->name_entries = memory;
    components->name_slot_count = new_slot_count;
    for (uint32_t slot_index = 0; slot_index < old_slot_count; slot_index += 1) {
        const sxml_schema_name_entry entry = old_entries[slot_index];
        if (entry.component != 0) {
            const uint32_t new_slot = find_name_slot(
                components,
                (sxml_schema_name_key){.symbol_space = entry.symbol_space, .name = entry.name});
            components->name_entries[new_slot] = entry;
        }
    }
    sxml_allocator_release(components->allocator, old_entries);
    return SXML_STATUS_OK;
}

static bool is_valid_name_entry(const sxml_schema_name_entry *entry) {
    return entry->symbol_space != SXML_SCHEMA_SYMBOL_SPACE_NONE &&
           entry->name.index != 0 && entry->component != 0;
}

sxml_status sxml_schema_components_enter_name(sxml_schema_components *components,
                                              const sxml_schema_name_entry *entry,
                                              uint32_t *existing_component) {
    if (components->is_sealed || !is_valid_name_entry(entry)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_schema_name_key key = {.symbol_space = entry->symbol_space,
                                      .name = entry->name};
    uint32_t component = 0;
    if (sxml_schema_components_find_name(components, key, &component)) {
        *existing_component = component;
        return SXML_STATUS_OK;
    }
    /* At most half the slots in use, so probing always ends. */
    if (((uint64_t)components->name_entry_count + 1U) * 2U >
        (uint64_t)components->name_slot_count) {
        sxml_status status = grow_name_index(components);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    const uint32_t slot = find_name_slot(components, key);
    components->name_entries[slot] = *entry;
    components->name_entry_count += 1;
    *existing_component = 0;
    return SXML_STATUS_OK;
}

sxml_status sxml_schema_components_replace_name(sxml_schema_components *components,
                                                const sxml_schema_name_entry *entry) {
    if (components->is_sealed || !is_valid_name_entry(entry) ||
        components->name_slot_count == 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const uint32_t slot = find_name_slot(
        components,
        (sxml_schema_name_key){.symbol_space = entry->symbol_space, .name = entry->name});
    if (components->name_entries[slot].component == 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    components->name_entries[slot].component = entry->component;
    return SXML_STATUS_OK;
}

bool sxml_schema_components_find_name(const sxml_schema_components *components,
                                      sxml_schema_name_key key, uint32_t *component) {
    if (components->name_slot_count == 0) {
        return false;
    }
    const sxml_schema_name_entry *entry =
        &components->name_entries[find_name_slot(components, key)];
    if (entry->component == 0) {
        return false;
    }
    *component = entry->component;
    return true;
}

/* --------------------------------------------------------------- Blocks */

/* What precedes each block: the block allocated before it. */
typedef struct block_header {
    void *previous_block;
} block_header;

sxml_status sxml_schema_components_allocate_block(sxml_schema_components *components,
                                                  sxml_allocation_request request,
                                                  void **memory) {
    if (components->is_sealed || request.byte_count == 0 ||
        !sxml_alignment_is_valid(request.alignment)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    size_t header_offset = 0;
    size_t payload_offset = 0;
    sxml_status status = sxml_memory_requirements_append(
        &requirements,
        (sxml_memory_requirements){.byte_count = sizeof(block_header),
                                   .alignment = alignof(block_header)},
        &header_offset);
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_requirements_append(
            &requirements,
            (sxml_memory_requirements){.byte_count = request.byte_count,
                                       .alignment = request.alignment},
            &payload_offset);
    }
    void *block = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            components->allocator,
            (sxml_allocation_request){.byte_count = requirements.byte_count,
                                      .alignment = requirements.alignment},
            &block);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *block_bytes = block;
    const block_header header = {.previous_block = components->last_block};
    memcpy(block_bytes + header_offset, &header, sizeof(header));
    components->last_block = block;
    *memory = block_bytes + payload_offset;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Lifecycle */

static sxml_status check_configuration(
    const sxml_schema_components_configuration *configuration) {
    if (configuration == NULL || configuration->processor == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_schema_components_memory_requirements(
    const sxml_schema_components_configuration *configuration,
    sxml_memory_requirements *requirements) {
    sxml_status status = check_configuration(configuration);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *requirements = (sxml_memory_requirements){.byte_count = sizeof(sxml_schema_components),
                                               .alignment = alignof(sxml_schema_components)};
    return SXML_STATUS_OK;
}

sxml_status sxml_schema_components_initialize(
    sxml_memory_view memory, const sxml_schema_components_configuration *configuration,
    sxml_schema_components **components) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_schema_components_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_view_check_requirements(memory, requirements);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* The memory was checked for the components' size and alignment. */
    void *components_memory = memory.bytes;
    sxml_schema_components *new_components = components_memory;
    memset(new_components, 0, sizeof(*new_components));
    new_components->processor = configuration->processor;
    new_components->allocator = configuration->processor->allocator;
    *components = new_components;
    return SXML_STATUS_OK;
}

void sxml_schema_components_finalize(sxml_schema_components *components) {
    if (components == NULL) {
        return;
    }
    const sxml_allocator allocator = components->allocator;
    const sxml_schema_table_storage *records =
        &components->tables[SXML_SCHEMA_TABLE_XPATH_RECORD];
    sxml_schema_xpath_record *record_entries = records->entries;
    for (uint32_t record_index = 1; record_index < records->entry_count; record_index += 1) {
        sxml_xpath_expression_destroy(record_entries[record_index].compiled_expression);
    }
    for (size_t table_index = 0; table_index < SXML_SCHEMA_TABLE_COUNT; table_index += 1) {
        sxml_allocator_release(allocator, components->tables[table_index].entries);
    }
    sxml_allocator_release(allocator, components->name_entries);
    void *block = components->last_block;
    while (block != NULL) {
        block_header header = {.previous_block = NULL};
        memcpy(&header, block, sizeof(header));
        sxml_allocator_release(allocator, block);
        block = header.previous_block;
    }
    const sxml_allocator creating_allocator = components->creating_allocator;
    memset(components, 0, sizeof(*components));
    components->creating_allocator = creating_allocator;
}

sxml_status sxml_schema_components_create(
    sxml_allocator allocator, const sxml_schema_components_configuration *configuration,
    sxml_schema_components **components) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_schema_components_memory_requirements(configuration, &requirements);
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
    sxml_schema_components *new_components = NULL;
    status = sxml_schema_components_initialize(
        (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count},
        configuration, &new_components);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_components->creating_allocator = allocator;
    *components = new_components;
    return SXML_STATUS_OK;
}

void sxml_schema_components_destroy(sxml_schema_components *components) {
    if (components == NULL) {
        return;
    }
    const sxml_allocator creating_allocator = components->creating_allocator;
    sxml_schema_components_finalize(components);
    sxml_allocator_release(creating_allocator, components);
}

void sxml_schema_components_seal(sxml_schema_components *components) {
    components->is_sealed = true;
}

/* -------------------------------------------------- Built-in components */

/* The built-in type definitions are 1 up to this (sxml_builtin_type). */
#define LAST_BUILTIN_TYPE_DEFINITION SXML_TYPE_XS_ERROR

/* The anonymous type of xsi:schemaLocation, a list of xs:anyURI (3.2.7). */
#define SCHEMA_LOCATION_TYPE_DEFINITION (LAST_BUILTIN_TYPE_DEFINITION + 1U)

/* Adds one entry and checks that it got the index the header promises. */
typedef struct builtin_entry {
    sxml_schema_table table;
    const void *entry;
    uint32_t expected_index;
} builtin_entry;

static sxml_status add_builtin_entry(sxml_schema_components *components,
                                     builtin_entry request) {
    uint32_t index = 0;
    sxml_status status = sxml_schema_components_add_entries(
        components,
        &(sxml_schema_entry_request){.table = request.table,
                                     .entries = request.entry,
                                     .entry_count = 1},
        &index);
    if (status == SXML_STATUS_OK && index != request.expected_index) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return status;
}

/* A global name in a well-known namespace to enter. */
typedef struct builtin_name {
    sxml_schema_symbol_space symbol_space;
    sxml_well_known_namespace namespace_uri;
    sxml_byte_view local_name;
    uint32_t component;
} builtin_name;

static sxml_status intern_builtin_name(sxml_schema_components *components,
                                       builtin_name request,
                                       sxml_name_identifier *name) {
    return sxml_name_pool_intern_name(
        components->processor->name_pool,
        &(sxml_name_request){
            .namespace_uri = sxml_well_known_namespace_identifier(request.namespace_uri),
            .local_name = request.local_name},
        name);
}

static sxml_status enter_builtin_name(sxml_schema_components *components,
                                      builtin_name request) {
    sxml_name_identifier name = {.index = 0};
    sxml_status status = intern_builtin_name(components, request, &name);
    uint32_t existing_component = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_components_enter_name(
            components,
            &(sxml_schema_name_entry){.symbol_space = request.symbol_space,
                                      .name = name,
                                      .component = request.component},
            &existing_component);
    }
    return status;
}

/* xs:anyType's content (3.4.7): particle 1 (1..1, model group 1), model
   group 1 (a sequence of particle 2), particle 2 (0..unbounded, wildcard
   1); wildcards 1 and 2 (any, lax) for elements and attributes. */
static sxml_status add_any_type_content(sxml_schema_components *components) {
    const sxml_schema_wildcard any_lax = {
        .namespace_constraint_variety = SXML_SCHEMA_NAMESPACE_CONSTRAINT_ANY,
        .process_contents = SXML_SCHEMA_PROCESS_CONTENTS_LAX};
    sxml_status status = add_builtin_entry(
        components, (builtin_entry){.table = SXML_SCHEMA_TABLE_WILDCARD,
                                    .entry = &any_lax,
                                    .expected_index = 1});
    if (status == SXML_STATUS_OK) {
        status = add_builtin_entry(components,
                                   (builtin_entry){.table = SXML_SCHEMA_TABLE_WILDCARD,
                                                   .entry = &any_lax,
                                                   .expected_index = 2});
    }
    const sxml_schema_particle outer_particle = {
        .minimum_occurrences = 1,
        .maximum_occurrences = 1,
        .term_kind = SXML_SCHEMA_TERM_MODEL_GROUP,
        .term = 1};
    const sxml_schema_particle inner_particle = {
        .minimum_occurrences = 0,
        .maximum_occurrences = SXML_SCHEMA_UNBOUNDED,
        .term_kind = SXML_SCHEMA_TERM_WILDCARD,
        .term = 1};
    if (status == SXML_STATUS_OK) {
        status = add_builtin_entry(components,
                                   (builtin_entry){.table = SXML_SCHEMA_TABLE_PARTICLE,
                                                   .entry = &outer_particle,
                                                   .expected_index = 1});
    }
    if (status == SXML_STATUS_OK) {
        status = add_builtin_entry(components,
                                   (builtin_entry){.table = SXML_SCHEMA_TABLE_PARTICLE,
                                                   .entry = &inner_particle,
                                                   .expected_index = 2});
    }
    const uint32_t inner_particle_index = 2;
    sxml_schema_model_group model_group = {.compositor = SXML_SCHEMA_COMPOSITOR_SEQUENCE};
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_components_add_list(
            components,
            (sxml_schema_index_view){.indexes = &inner_particle_index, .index_count = 1},
            &model_group.particles);
    }
    if (status == SXML_STATUS_OK) {
        status = add_builtin_entry(components,
                                   (builtin_entry){.table = SXML_SCHEMA_TABLE_MODEL_GROUP,
                                                   .entry = &model_group,
                                                   .expected_index = 1});
    }
    return status;
}

/* How a built-in simple type is made from its base. */
static sxml_schema_derivation builtin_derivation_method(sxml_type_variety variety) {
    if (variety == SXML_TYPE_VARIETY_LIST) {
        return SXML_SCHEMA_DERIVATION_LIST;
    }
    if (variety == SXML_TYPE_VARIETY_UNION) {
        return SXML_SCHEMA_DERIVATION_UNION;
    }
    return SXML_SCHEMA_DERIVATION_RESTRICTION;
}

/* XDM's types among the registry's built-ins, which XSD does not have. */
static bool is_xdm_only_type(sxml_builtin_type builtin_type) {
    return builtin_type == SXML_TYPE_XS_UNTYPED ||
           builtin_type == SXML_TYPE_XS_UNTYPED_ATOMIC ||
           builtin_type == SXML_TYPE_XS_NUMERIC;
}

/* The most member types of a built-in union (xs:numeric has three). */
#define BUILTIN_MEMBER_CAPACITY 8U

/* The member types of a built-in union, as a list. */
static sxml_status add_builtin_member_types(sxml_schema_components *components,
                                            const sxml_type_description *description,
                                            sxml_schema_list *member_types) {
    const sxml_type_identifier_view members = description->variety_details.member_types;
    if (members.type_identifier_count > BUILTIN_MEMBER_CAPACITY) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    uint32_t member_indexes[BUILTIN_MEMBER_CAPACITY] = {0};
    for (size_t member_index = 0; member_index < members.type_identifier_count;
         member_index += 1) {
        member_indexes[member_index] = members.type_identifiers[member_index].index;
    }
    return sxml_schema_components_add_list(
        components,
        (sxml_schema_index_view){.indexes = member_indexes,
                                 .index_count = members.type_identifier_count},
        member_types);
}

/* Type definition builtin_type (3.16.7, 3.4.7), from the registry's
   static description. */
static sxml_status add_builtin_type_definition(sxml_schema_components *components,
                                               sxml_builtin_type builtin_type) {
    const sxml_type_description *description = sxml_type_builtin_description(builtin_type);
    if (description == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const builtin_name name_request = {
        .symbol_space = SXML_SCHEMA_SYMBOL_SPACE_TYPE_DEFINITION,
        .namespace_uri = SXML_NAMESPACE_XML_SCHEMA,
        .local_name = sxml_type_builtin_local_name(builtin_type),
        .component = (uint32_t)builtin_type};
    sxml_schema_type_definition definition = {
        .variety = description->variety,
        .is_builtin = true,
        .base_type = {.value = description->base_type.index},
        .derivation_method = builtin_derivation_method(description->variety),
        .registered_type = sxml_builtin_type_identifier(builtin_type)};
    /* xs:anyType's base is itself (3.4.7). */
    if (builtin_type == SXML_TYPE_XS_ANY_TYPE) {
        definition.base_type.value = (uint32_t)SXML_TYPE_XS_ANY_TYPE;
    }
    sxml_status status = intern_builtin_name(components, name_request, &definition.type_name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description->variety == SXML_TYPE_VARIETY_COMPLEX) {
        definition.content_type_variety = SXML_CONTENT_TYPE_VARIETY_MIXED;
        definition.particle.value = 1;
        definition.attribute_wildcard.value = 2;
    } else if (description->variety == SXML_TYPE_VARIETY_ATOMIC) {
        definition.primitive_type.value = description->primitive_type.index;
    } else if (description->variety == SXML_TYPE_VARIETY_LIST) {
        definition.item_type.value = description->variety_details.list_item_type.index;
    } else if (description->variety == SXML_TYPE_VARIETY_UNION) {
        status = add_builtin_member_types(components, description, &definition.member_types);
    }
    if (status == SXML_STATUS_OK) {
        status = add_builtin_entry(
            components, (builtin_entry){.table = SXML_SCHEMA_TABLE_TYPE_DEFINITION,
                                        .entry = &definition,
                                        .expected_index = (uint32_t)builtin_type});
    }
    if (status == SXML_STATUS_OK && !is_xdm_only_type(builtin_type)) {
        status = enter_builtin_name(components, name_request);
    }
    return status;
}

/* The four xsi: attribute declarations (3.2.7), and the anonymous type of
   xsi:schemaLocation. */
static sxml_status add_builtin_attribute_declarations(sxml_schema_components *components) {
    const sxml_schema_type_definition schema_location_type = {
        .variety = SXML_TYPE_VARIETY_LIST,
        .base_type = {.value = (uint32_t)SXML_TYPE_XS_ANY_SIMPLE_TYPE},
        .derivation_method = SXML_SCHEMA_DERIVATION_LIST,
        .item_type = {.value = (uint32_t)SXML_TYPE_XS_ANY_URI}};
    sxml_status status = add_builtin_entry(
        components, (builtin_entry){.table = SXML_SCHEMA_TABLE_TYPE_DEFINITION,
                                    .entry = &schema_location_type,
                                    .expected_index = SCHEMA_LOCATION_TYPE_DEFINITION});
    static const struct {
        sxml_byte_view local_name;
        uint32_t type_definition;
    } declarations[] = {
        {SXML_BYTE_VIEW_INITIALIZER("type"), (uint32_t)SXML_TYPE_XS_QNAME},
        {SXML_BYTE_VIEW_INITIALIZER("nil"), (uint32_t)SXML_TYPE_XS_BOOLEAN},
        {SXML_BYTE_VIEW_INITIALIZER("schemaLocation"), SCHEMA_LOCATION_TYPE_DEFINITION},
        {SXML_BYTE_VIEW_INITIALIZER("noNamespaceSchemaLocation"),
         (uint32_t)SXML_TYPE_XS_ANY_URI},
    };
    for (uint32_t declaration_index = 0;
         status == SXML_STATUS_OK &&
         declaration_index < sizeof(declarations) / sizeof(declarations[0]);
         declaration_index += 1) {
        const builtin_name name_request = {
            .symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_DECLARATION,
            .namespace_uri = SXML_NAMESPACE_XML_SCHEMA_INSTANCE,
            .local_name = declarations[declaration_index].local_name,
            .component = declaration_index + 1U};
        sxml_schema_attribute_declaration declaration = {
            .scope_variety = SXML_SCHEMA_SCOPE_GLOBAL,
            .type_definition = {.value = declarations[declaration_index].type_definition}};
        status = intern_builtin_name(components, name_request, &declaration.attribute_name);
        if (status == SXML_STATUS_OK) {
            status = add_builtin_entry(
                components,
                (builtin_entry){.table = SXML_SCHEMA_TABLE_ATTRIBUTE_DECLARATION,
                                .entry = &declaration,
                                .expected_index = declaration_index + 1U});
        }
        if (status == SXML_STATUS_OK) {
            status = enter_builtin_name(components, name_request);
        }
    }
    return status;
}

sxml_status sxml_schema_components_add_builtin_components(
    sxml_schema_components *components) {
    for (size_t table_index = 0; table_index < SXML_SCHEMA_TABLE_COUNT; table_index += 1) {
        if (components->tables[table_index].entry_count != 0) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
    }
    sxml_status status = add_any_type_content(components);
    for (uint32_t builtin_type = 1;
         status == SXML_STATUS_OK && builtin_type <= (uint32_t)LAST_BUILTIN_TYPE_DEFINITION;
         builtin_type += 1) {
        status = add_builtin_type_definition(components, (sxml_builtin_type)builtin_type);
    }
    if (status == SXML_STATUS_OK) {
        status = add_builtin_attribute_declarations(components);
    }
    return status;
}

/* --------------------------------------------------------------- Reports */

sxml_status sxml_schema_compilation_report(sxml_schema_compilation *compilation,
                                           const sxml_schema_report_request *request) {
    const sxml_schema_configuration *configuration = compilation->configuration;
    const sxml_schema_document_entry *document =
        sxml_schema_get_document(compilation->components, request->position.document);
    const sxml_source_location location = {
        .source_name = sxml_schema_get_text(compilation->components, document->document_uri),
        .line_number = request->position.line_number,
        .column_number = request->position.column_number,
        .byte_offset = request->position.byte_offset};
    const sxml_error_code code = {
        .namespace_uri = {.bytes = NULL, .byte_count = 0},
        .local_name = request->constraint_name,
        .namespace_prefix = {.bytes = NULL, .byte_count = 0}};
    compilation->error_count += 1;
    if (compilation->error_count == 1 && configuration->error_record != NULL) {
        sxml_source_location record_location = location;
        /* The record outlives the components; the configuration's view
           outlives the call. */
        record_location.source_name = document->configured_uri;
        *configuration->error_record = (sxml_error_record){
            .code = code,
            .value = NULL,
            .diagnostic = {.description = request->description,
                           .location = record_location}};
    }
    const sxml_schema_report_sink sink = configuration->report_sink;
    if (sink.operations == NULL) {
        return SXML_STATUS_STATIC_ERROR;
    }
    const sxml_schema_report report = {
        .code = code,
        .description = request->detailed_description.byte_count != 0
                           ? request->detailed_description
                           : request->description,
        .location = location};
    return sink.operations->receive(sink.state, &report);
}
