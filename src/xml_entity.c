/*
 * Growable buffers and arrays for the XML parser, and the tables of a
 * document type declaration: entities and attribute definitions
 * (src/xml_internal.h).
 *
 * Golden file: src/memory.c (layout, naming, checked size arithmetic,
 * outputs untouched on failure).
 *
 * The allocator interface has no reallocate (D22), so growing allocates a
 * larger block, copies and releases the old one; capacities double, so the
 * copying is linear in all. Names are found through open-addressing hash
 * indices of entry numbers, kept at most half full, so a document with
 * many declarations is not quadratic.
 */
#include "xml_internal.h"

#include <stdalign.h>
#include <string.h>

/* The smallest capacity a buffer or array grows to. */
enum { SXML_XML_MINIMUM_CAPACITY = 16 };

/* ------------------------------------------------------------ Buffers */

sxml_xml_buffer sxml_xml_buffer_empty(sxml_allocator allocator) {
    return (sxml_xml_buffer){.allocator = allocator,
                             .bytes = NULL,
                             .byte_count = 0,
                             .byte_capacity = 0};
}

void sxml_xml_buffer_release(sxml_xml_buffer *buffer) {
    if (buffer->bytes != NULL) {
        sxml_allocator_release(buffer->allocator, buffer->bytes);
    }
    *buffer = sxml_xml_buffer_empty(buffer->allocator);
}

/* The capacity to grow to for at least required_count units. */
static sxml_status sxml_xml_grown_capacity(size_t current_capacity,
                                           size_t required_count,
                                           size_t *capacity) {
    size_t grown = current_capacity < SXML_XML_MINIMUM_CAPACITY
                       ? SXML_XML_MINIMUM_CAPACITY
                       : current_capacity;
    while (grown < required_count) {
        sxml_status status = sxml_size_multiply(grown, 2, &grown);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    *capacity = grown;
    return SXML_STATUS_OK;
}

sxml_status sxml_xml_buffer_reserve(sxml_xml_buffer *buffer,
                                    size_t additional_byte_count) {
    size_t required = 0;
    sxml_status status =
        sxml_size_add(buffer->byte_count, additional_byte_count, &required);
    if (status != SXML_STATUS_OK || required <= buffer->byte_capacity) {
        return status;
    }
    size_t capacity = 0;
    status =
        sxml_xml_grown_capacity(buffer->byte_capacity, required, &capacity);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(
        buffer->allocator,
        (sxml_allocation_request){.byte_count = capacity, .alignment = 1},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (buffer->byte_count != 0) {
        memcpy(memory, buffer->bytes, buffer->byte_count);
    }
    if (buffer->bytes != NULL) {
        sxml_allocator_release(buffer->allocator, buffer->bytes);
    }
    buffer->bytes = memory;
    buffer->byte_capacity = capacity;
    return SXML_STATUS_OK;
}

sxml_status sxml_xml_buffer_append(sxml_xml_buffer *buffer,
                                   sxml_byte_view bytes) {
    if (bytes.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    sxml_status status = sxml_xml_buffer_reserve(buffer, bytes.byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memcpy(buffer->bytes + buffer->byte_count, bytes.bytes, bytes.byte_count);
    buffer->byte_count += bytes.byte_count;
    return SXML_STATUS_OK;
}

sxml_status sxml_xml_buffer_append_byte(sxml_xml_buffer *buffer, uint8_t byte) {
    sxml_status status = sxml_xml_buffer_reserve(buffer, 1);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    buffer->bytes[buffer->byte_count] = byte;
    buffer->byte_count += 1;
    return SXML_STATUS_OK;
}

sxml_status sxml_xml_buffer_append_codepoint(sxml_xml_buffer *buffer,
                                             uint32_t codepoint) {
    uint8_t encoded[4] = {0, 0, 0, 0};
    size_t encoded_byte_count = 0;
    /* Unicode 16.0, section 3.9, table 3-6. */
    if (codepoint < 0x80) {
        encoded[0] = (uint8_t)codepoint;
        encoded_byte_count = 1;
    } else if (codepoint < 0x800) {
        encoded[0] = (uint8_t)(0xC0 | (codepoint >> 6));
        encoded[1] = (uint8_t)(0x80 | (codepoint & 0x3F));
        encoded_byte_count = 2;
    } else if (codepoint < 0x10000) {
        encoded[0] = (uint8_t)(0xE0 | (codepoint >> 12));
        encoded[1] = (uint8_t)(0x80 | ((codepoint >> 6) & 0x3F));
        encoded[2] = (uint8_t)(0x80 | (codepoint & 0x3F));
        encoded_byte_count = 3;
    } else {
        encoded[0] = (uint8_t)(0xF0 | (codepoint >> 18));
        encoded[1] = (uint8_t)(0x80 | ((codepoint >> 12) & 0x3F));
        encoded[2] = (uint8_t)(0x80 | ((codepoint >> 6) & 0x3F));
        encoded[3] = (uint8_t)(0x80 | (codepoint & 0x3F));
        encoded_byte_count = 4;
    }
    return sxml_xml_buffer_append(
        buffer,
        (sxml_byte_view){.bytes = encoded, .byte_count = encoded_byte_count});
}

sxml_byte_view sxml_xml_buffer_view(const sxml_xml_buffer *buffer,
                                    sxml_byte_range range) {
    if (range.byte_count == 0) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return (sxml_byte_view){.bytes = buffer->bytes + range.byte_offset,
                            .byte_count = range.byte_count};
}

/* ------------------------------------------------------------- Arrays */

sxml_xml_array
sxml_xml_array_empty(sxml_xml_array_configuration configuration) {
    return (sxml_xml_array){
        .allocator = configuration.allocator,
        .element_byte_count = configuration.element_byte_count,
        .elements = NULL,
        .element_count = 0,
        .element_capacity = 0,
    };
}

void sxml_xml_array_release(sxml_xml_array *array) {
    if (array->elements != NULL) {
        sxml_allocator_release(array->allocator, array->elements);
    }
    array->elements = NULL;
    array->element_count = 0;
    array->element_capacity = 0;
}

/* Grows array to hold at least required_count elements. */
static sxml_status sxml_xml_array_reserve(sxml_xml_array *array,
                                          size_t required_count) {
    if (required_count <= array->element_capacity) {
        return SXML_STATUS_OK;
    }
    size_t capacity = 0;
    sxml_status status = sxml_xml_grown_capacity(array->element_capacity,
                                                 required_count, &capacity);
    size_t byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(capacity, array->element_byte_count,
                                    &byte_count);
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            array->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(max_align_t)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (array->element_count != 0) {
        memcpy(memory, array->elements,
               array->element_count * array->element_byte_count);
    }
    if (array->elements != NULL) {
        sxml_allocator_release(array->allocator, array->elements);
    }
    array->elements = memory;
    array->element_capacity = capacity;
    return SXML_STATUS_OK;
}

sxml_status sxml_xml_array_push(sxml_xml_array *array, void **element) {
    sxml_status status =
        sxml_xml_array_reserve(array, array->element_count + 1);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *slot =
        array->elements + array->element_count * array->element_byte_count;
    memset(slot, 0, array->element_byte_count);
    array->element_count += 1;
    *element = slot;
    return SXML_STATUS_OK;
}

void *sxml_xml_array_at(const sxml_xml_array *array, size_t element_index) {
    return array->elements + element_index * array->element_byte_count;
}

/* -------------------------------------------------------- Hash index */

/* An element with attribute definitions. */
typedef struct sxml_xml_element_entry {
    sxml_byte_range element_name;
    /* Definition numbers (index plus one); 0 for none. */
    uint32_t first_definition;
    uint32_t last_definition;
} sxml_xml_element_entry;

/* FNV-1a, 32 bits: names are short and the index only needs spread. */
static uint32_t sxml_xml_hash_name(sxml_byte_view name) {
    uint32_t hash = 2166136261U;
    for (size_t byte_index = 0; byte_index < name.byte_count; byte_index++) {
        hash ^= name.bytes[byte_index];
        hash *= 16777619U;
    }
    return hash;
}

/* What one hash index covers: its slots and how to name an entry. */
typedef struct sxml_xml_name_index {
    sxml_xml_array *slots;
    const sxml_xml_dtd *dtd;
    /* Entries are entities when true, elements otherwise. */
    bool indexes_entities;
} sxml_xml_name_index;

static sxml_byte_view
sxml_xml_index_entry_name(const sxml_xml_name_index *index,
                          uint32_t entry_index) {
    if (index->indexes_entities) {
        const sxml_xml_entity *entity =
            sxml_xml_array_at(&index->dtd->entities, entry_index);
        return sxml_xml_dtd_text(index->dtd, entity->entity_name);
    }
    const sxml_xml_element_entry *element =
        sxml_xml_array_at(&index->dtd->elements, entry_index);
    return sxml_xml_dtd_text(index->dtd, element->element_name);
}

static sxml_xml_lookup sxml_xml_index_find(const sxml_xml_name_index *index,
                                           sxml_byte_view name) {
    const sxml_xml_array *slots = index->slots;
    if (slots->element_count == 0) {
        return (sxml_xml_lookup){.is_found = false, .entry_index = 0};
    }
    size_t mask = slots->element_count - 1;
    size_t slot_index = sxml_xml_hash_name(name) & mask;
    for (;;) {
        const uint32_t *slot = sxml_xml_array_at(slots, slot_index);
        if (*slot == 0) {
            return (sxml_xml_lookup){.is_found = false, .entry_index = 0};
        }
        if (sxml_byte_view_equals(sxml_xml_index_entry_name(index, *slot - 1),
                                  name)) {
            return (sxml_xml_lookup){.is_found = true,
                                     .entry_index = *slot - 1};
        }
        slot_index = (slot_index + 1) & mask;
    }
}

/* Places entry_index in the first free slot of its name's probe sequence. */
static void sxml_xml_index_place(const sxml_xml_name_index *index,
                                 uint32_t entry_index) {
    const sxml_xml_array *slots = index->slots;
    size_t mask = slots->element_count - 1;
    size_t slot_index =
        sxml_xml_hash_name(sxml_xml_index_entry_name(index, entry_index)) &
        mask;
    for (;;) {
        uint32_t *slot = sxml_xml_array_at(slots, slot_index);
        if (*slot == 0) {
            *slot = entry_index + 1;
            return;
        }
        slot_index = (slot_index + 1) & mask;
    }
}

/*
 * Makes the index hold entry_count entries at most half full, rebuilding
 * it from the entries that satisfy the index (all entities of the kind,
 * or all elements) when it grows.
 */
static sxml_status sxml_xml_index_prepare(const sxml_xml_name_index *index,
                                          size_t entry_count) {
    sxml_xml_array *slots = index->slots;
    size_t needed = 0;
    sxml_status status = sxml_size_multiply(entry_count, 2, &needed);
    if (status != SXML_STATUS_OK || needed <= slots->element_count) {
        return status;
    }
    size_t slot_count = SXML_XML_MINIMUM_CAPACITY;
    while (slot_count < needed) {
        status = sxml_size_multiply(slot_count, 2, &slot_count);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_xml_array grown = sxml_xml_array_empty((sxml_xml_array_configuration){
        .allocator = slots->allocator, .element_byte_count = sizeof(uint32_t)});
    status = sxml_xml_array_reserve(&grown, slot_count);
    if (status != SXML_STATUS_OK || grown.elements == NULL) {
        return status == SXML_STATUS_OK ? SXML_STATUS_OUT_OF_MEMORY : status;
    }
    memset(grown.elements, 0, slot_count * sizeof(uint32_t));
    grown.element_count = slot_count;
    sxml_xml_array previous = *slots;
    *slots = grown;
    for (size_t slot_index = 0; slot_index < previous.element_count;
         slot_index++) {
        const uint32_t *slot = sxml_xml_array_at(&previous, slot_index);
        if (*slot != 0) {
            sxml_xml_index_place(index, *slot - 1);
        }
    }
    sxml_xml_array_release(&previous);
    return SXML_STATUS_OK;
}

/* --------------------------------------------------------------- DTD */

sxml_xml_dtd sxml_xml_dtd_empty(sxml_allocator allocator) {
    const sxml_xml_array_configuration slot_configuration = {
        .allocator = allocator, .element_byte_count = sizeof(uint32_t)};
    return (sxml_xml_dtd){
        .text = sxml_xml_buffer_empty(allocator),
        .entities = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = allocator,
            .element_byte_count = sizeof(sxml_xml_entity)}),
        .attribute_definitions =
            sxml_xml_array_empty((sxml_xml_array_configuration){
                .allocator = allocator,
                .element_byte_count = sizeof(sxml_xml_attribute_definition)}),
        .elements = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = allocator,
            .element_byte_count = sizeof(sxml_xml_element_entry)}),
        .general_entity_slots = sxml_xml_array_empty(slot_configuration),
        .parameter_entity_slots = sxml_xml_array_empty(slot_configuration),
        .element_slots = sxml_xml_array_empty(slot_configuration),
        .general_entity_count = 0,
        .parameter_entity_count = 0,
    };
}

static void sxml_xml_clear_slots(sxml_xml_array *slots) {
    if (slots->element_count != 0) {
        memset(slots->elements, 0, slots->element_count * sizeof(uint32_t));
    }
}

void sxml_xml_dtd_clear(sxml_xml_dtd *dtd) {
    dtd->text.byte_count = 0;
    dtd->entities.element_count = 0;
    dtd->attribute_definitions.element_count = 0;
    dtd->elements.element_count = 0;
    dtd->general_entity_count = 0;
    dtd->parameter_entity_count = 0;
    sxml_xml_clear_slots(&dtd->general_entity_slots);
    sxml_xml_clear_slots(&dtd->parameter_entity_slots);
    sxml_xml_clear_slots(&dtd->element_slots);
}

void sxml_xml_dtd_release(sxml_xml_dtd *dtd) {
    sxml_xml_buffer_release(&dtd->text);
    sxml_xml_array_release(&dtd->entities);
    sxml_xml_array_release(&dtd->attribute_definitions);
    sxml_xml_array_release(&dtd->elements);
    sxml_xml_array_release(&dtd->general_entity_slots);
    sxml_xml_array_release(&dtd->parameter_entity_slots);
    sxml_xml_array_release(&dtd->element_slots);
}

sxml_status sxml_xml_dtd_store_text(sxml_xml_dtd *dtd, sxml_byte_view text,
                                    sxml_byte_range *range) {
    const size_t byte_offset = dtd->text.byte_count;
    sxml_status status = sxml_xml_buffer_append(&dtd->text, text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *range = (sxml_byte_range){.byte_offset = byte_offset,
                               .byte_count = text.byte_count};
    return SXML_STATUS_OK;
}

sxml_byte_view sxml_xml_dtd_text(const sxml_xml_dtd *dtd,
                                 sxml_byte_range range) {
    return sxml_xml_buffer_view(&dtd->text, range);
}

/* The entity index of one kind; the slots are the DTD's own. */
static sxml_xml_name_index sxml_xml_entity_index(const sxml_xml_dtd *dtd,
                                                 sxml_xml_entity_key key) {
    const bool is_parameter_entity = key.is_parameter_entity;
    /* The index writes only its slots, never the DTD the const pointer
       reaches entries through. */
    sxml_xml_dtd *writable = (sxml_xml_dtd *)dtd;
    return (sxml_xml_name_index){
        .slots = is_parameter_entity ? &writable->parameter_entity_slots
                                     : &writable->general_entity_slots,
        .dtd = dtd,
        .indexes_entities = true,
    };
}

static sxml_xml_name_index sxml_xml_element_index(const sxml_xml_dtd *dtd) {
    sxml_xml_dtd *writable = (sxml_xml_dtd *)dtd;
    return (sxml_xml_name_index){.slots = &writable->element_slots,
                                 .dtd = dtd,
                                 .indexes_entities = false};
}

sxml_xml_lookup sxml_xml_dtd_find_entity(const sxml_xml_dtd *dtd,
                                         sxml_xml_entity_key key) {
    const sxml_xml_name_index index = sxml_xml_entity_index(dtd, key);
    return sxml_xml_index_find(&index, key.entity_name);
}

sxml_status sxml_xml_dtd_add_entity(sxml_xml_dtd *dtd,
                                    const sxml_xml_entity *entity,
                                    sxml_xml_lookup *added) {
    const sxml_xml_name_index index = sxml_xml_entity_index(
        dtd, (sxml_xml_entity_key){
                 .entity_name = {.bytes = NULL, .byte_count = 0},
                 .is_parameter_entity = entity->is_parameter_entity});
    sxml_xml_lookup existing = sxml_xml_index_find(
        &index, sxml_xml_dtd_text(dtd, entity->entity_name));
    if (existing.is_found) {
        *added = (sxml_xml_lookup){.is_found = false,
                                   .entry_index = existing.entry_index};
        return SXML_STATUS_OK;
    }
    if (dtd->entities.element_count >= UINT32_MAX - 1) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    size_t *kind_count = entity->is_parameter_entity
                             ? &dtd->parameter_entity_count
                             : &dtd->general_entity_count;
    sxml_status status = sxml_xml_index_prepare(&index, *kind_count + 1);
    void *element = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_array_push(&dtd->entities, &element);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *(sxml_xml_entity *)element = *entity;
    *kind_count += 1;
    const uint32_t entry_index = (uint32_t)(dtd->entities.element_count - 1);
    sxml_xml_index_place(&index, entry_index);
    *added = (sxml_xml_lookup){.is_found = true, .entry_index = entry_index};
    return SXML_STATUS_OK;
}

sxml_xml_entity *sxml_xml_dtd_get_entity(const sxml_xml_dtd *dtd,
                                         uint32_t entry_index) {
    return sxml_xml_array_at(&dtd->entities, entry_index);
}

/* Finds or adds the element entry of element_name. */
static sxml_status sxml_xml_dtd_get_element(sxml_xml_dtd *dtd,
                                            sxml_byte_view element_name,
                                            uint32_t *element_index) {
    const sxml_xml_name_index index = sxml_xml_element_index(dtd);
    sxml_xml_lookup existing = sxml_xml_index_find(&index, element_name);
    if (existing.is_found) {
        *element_index = existing.entry_index;
        return SXML_STATUS_OK;
    }
    if (dtd->elements.element_count >= UINT32_MAX - 1) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    sxml_byte_range name_range = {.byte_offset = 0, .byte_count = 0};
    sxml_status status =
        sxml_xml_index_prepare(&index, dtd->elements.element_count + 1);
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_dtd_store_text(dtd, element_name, &name_range);
    }
    void *element = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_array_push(&dtd->elements, &element);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *(sxml_xml_element_entry *)element =
        (sxml_xml_element_entry){.element_name = name_range,
                                 .first_definition = 0,
                                 .last_definition = 0};
    *element_index = (uint32_t)(dtd->elements.element_count - 1);
    sxml_xml_index_place(&index, *element_index);
    return SXML_STATUS_OK;
}

sxml_status sxml_xml_dtd_add_attribute_definition(
    sxml_xml_dtd *dtd, const sxml_xml_attribute_definition_request *request,
    sxml_xml_addition *addition) {
    uint32_t element_index = 0;
    sxml_status status =
        sxml_xml_dtd_get_element(dtd, request->element_name, &element_index);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_xml_element_entry *element =
        sxml_xml_array_at(&dtd->elements, element_index);
    const sxml_byte_view attribute_name =
        sxml_xml_dtd_text(dtd, request->definition.attribute_name);
    for (uint32_t number = element->first_definition; number != 0;) {
        const sxml_xml_attribute_definition *definition =
            sxml_xml_dtd_get_definition(dtd, number);
        if (sxml_byte_view_equals(
                sxml_xml_dtd_text(dtd, definition->attribute_name),
                attribute_name)) {
            addition->is_added = false;
            return SXML_STATUS_OK;
        }
        number = definition->next_definition;
    }
    if (dtd->attribute_definitions.element_count >= UINT32_MAX - 1) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    void *slot = NULL;
    status = sxml_xml_array_push(&dtd->attribute_definitions, &slot);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_xml_attribute_definition *definition = slot;
    *definition = request->definition;
    definition->next_definition = 0;
    const uint32_t number = (uint32_t)dtd->attribute_definitions.element_count;
    sxml_xml_element_entry *owner =
        sxml_xml_array_at(&dtd->elements, element_index);
    if (owner->last_definition == 0) {
        owner->first_definition = number;
    } else {
        sxml_xml_attribute_definition *last = sxml_xml_array_at(
            &dtd->attribute_definitions, (size_t)owner->last_definition - 1);
        last->next_definition = number;
    }
    owner->last_definition = number;
    addition->is_added = true;
    return SXML_STATUS_OK;
}

uint32_t sxml_xml_dtd_first_definition(const sxml_xml_dtd *dtd,
                                       sxml_byte_view element_name) {
    const sxml_xml_name_index index = sxml_xml_element_index(dtd);
    sxml_xml_lookup element = sxml_xml_index_find(&index, element_name);
    if (!element.is_found) {
        return 0;
    }
    const sxml_xml_element_entry *entry =
        sxml_xml_array_at(&dtd->elements, element.entry_index);
    return entry->first_definition;
}

const sxml_xml_attribute_definition *
sxml_xml_dtd_get_definition(const sxml_xml_dtd *dtd,
                            uint32_t definition_number) {
    return sxml_xml_array_at(&dtd->attribute_definitions,
                             (size_t)definition_number - 1);
}
