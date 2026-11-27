/*
 * The EXI string table (EXI 1.0, 7.3 "String Table" and Appendix D; D333):
 * the URI partition, a prefix and a local-name partition per URI, a local
 * value partition per qname and the global value partition, with one hash
 * index over all of them for the encoder's lookups.
 */
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

#include "exi_internal.h"
#include "memory_internal.h"

/* The kinds of key of the hash index. */
enum {
    SXML_EXI_KEY_URI = 1,
    SXML_EXI_KEY_PREFIX = 2,
    SXML_EXI_KEY_LOCAL_NAME = 3,
    SXML_EXI_KEY_GLOBAL_VALUE = 4,
    SXML_EXI_KEY_LOCAL_VALUE = 5,
};

/* The XML Schema URI and its type names, schema-informed only (D.1, D.5). */
static const sxml_byte_view sxml_exi_xml_schema_type_names[] = {
    SXML_BYTE_VIEW_INITIALIZER("ENTITIES"),
    SXML_BYTE_VIEW_INITIALIZER("ENTITY"),
    SXML_BYTE_VIEW_INITIALIZER("ID"),
    SXML_BYTE_VIEW_INITIALIZER("IDREF"),
    SXML_BYTE_VIEW_INITIALIZER("IDREFS"),
    SXML_BYTE_VIEW_INITIALIZER("NCName"),
    SXML_BYTE_VIEW_INITIALIZER("NMTOKEN"),
    SXML_BYTE_VIEW_INITIALIZER("NMTOKENS"),
    SXML_BYTE_VIEW_INITIALIZER("NOTATION"),
    SXML_BYTE_VIEW_INITIALIZER("Name"),
    SXML_BYTE_VIEW_INITIALIZER("QName"),
    SXML_BYTE_VIEW_INITIALIZER("anySimpleType"),
    SXML_BYTE_VIEW_INITIALIZER("anyType"),
    SXML_BYTE_VIEW_INITIALIZER("anyURI"),
    SXML_BYTE_VIEW_INITIALIZER("base64Binary"),
    SXML_BYTE_VIEW_INITIALIZER("boolean"),
    SXML_BYTE_VIEW_INITIALIZER("byte"),
    SXML_BYTE_VIEW_INITIALIZER("date"),
    SXML_BYTE_VIEW_INITIALIZER("dateTime"),
    SXML_BYTE_VIEW_INITIALIZER("decimal"),
    SXML_BYTE_VIEW_INITIALIZER("double"),
    SXML_BYTE_VIEW_INITIALIZER("duration"),
    SXML_BYTE_VIEW_INITIALIZER("float"),
    SXML_BYTE_VIEW_INITIALIZER("gDay"),
    SXML_BYTE_VIEW_INITIALIZER("gMonth"),
    SXML_BYTE_VIEW_INITIALIZER("gMonthDay"),
    SXML_BYTE_VIEW_INITIALIZER("gYear"),
    SXML_BYTE_VIEW_INITIALIZER("gYearMonth"),
    SXML_BYTE_VIEW_INITIALIZER("hexBinary"),
    SXML_BYTE_VIEW_INITIALIZER("int"),
    SXML_BYTE_VIEW_INITIALIZER("integer"),
    SXML_BYTE_VIEW_INITIALIZER("language"),
    SXML_BYTE_VIEW_INITIALIZER("long"),
    SXML_BYTE_VIEW_INITIALIZER("negativeInteger"),
    SXML_BYTE_VIEW_INITIALIZER("nonNegativeInteger"),
    SXML_BYTE_VIEW_INITIALIZER("nonPositiveInteger"),
    SXML_BYTE_VIEW_INITIALIZER("normalizedString"),
    SXML_BYTE_VIEW_INITIALIZER("positiveInteger"),
    SXML_BYTE_VIEW_INITIALIZER("short"),
    SXML_BYTE_VIEW_INITIALIZER("string"),
    SXML_BYTE_VIEW_INITIALIZER("time"),
    SXML_BYTE_VIEW_INITIALIZER("token"),
    SXML_BYTE_VIEW_INITIALIZER("unsignedByte"),
    SXML_BYTE_VIEW_INITIALIZER("unsignedInt"),
    SXML_BYTE_VIEW_INITIALIZER("unsignedLong"),
    SXML_BYTE_VIEW_INITIALIZER("unsignedShort"),
};


sxml_byte_view sxml_exi_string_table_view(const sxml_exi_string_table *table, sxml_exi_text text) {
    return (sxml_byte_view){.bytes = table->text_bytes + text.offset, .byte_count = text.byte_count};
}

/* ------------------------------------------------------------- Memory */

/* Grows an array to hold required elements. */
typedef struct sxml_exi_growth {
    void **elements;
    size_t *capacity;
    size_t element_count;
    size_t element_size;
} sxml_exi_growth;

static sxml_status sxml_exi_reserve(sxml_allocator allocator, const sxml_exi_growth *growth) {
    if (growth->element_count < *growth->capacity) {
        return SXML_STATUS_OK;
    }
    return sxml_allocator_reserve_array(allocator, &(sxml_array_reservation_request){
                                                       .elements = growth->elements,
                                                       .capacity = growth->capacity,
                                                       .element_count = growth->element_count,
                                                       .required_count = growth->element_count + 1,
                                                       .element_size = growth->element_size,
                                                       .element_alignment = alignof(max_align_t),
                                                       .minimum_capacity = 8});
}

/* Copies text into the table's store. */
static sxml_status sxml_exi_store_text(sxml_exi_string_table *table, sxml_byte_view text, sxml_exi_text *stored) {
    if (text.byte_count > UINT32_MAX) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (table->text_byte_count + text.byte_count > table->text_capacity) {
        void *elements = table->text_bytes;
        sxml_status status = sxml_allocator_reserve_array(
            table->allocator, &(sxml_array_reservation_request){.elements = &elements,
                                                                .capacity = &table->text_capacity,
                                                                .element_count = table->text_byte_count,
                                                                .required_count = table->text_byte_count + text.byte_count,
                                                                .element_size = 1,
                                                                .element_alignment = 1,
                                                                .minimum_capacity = 4096});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        table->text_bytes = elements;
    }
    if (text.byte_count > 0) {
        memcpy(table->text_bytes + table->text_byte_count, text.bytes, text.byte_count);
    }
    *stored = (sxml_exi_text){.offset = table->text_byte_count,
                              .byte_count = (uint32_t)text.byte_count,
                              .codepoint_count = (uint32_t)sxml_exi_utf8_codepoint_count(text)};
    table->text_byte_count += text.byte_count;
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------- Hash index */

typedef struct sxml_exi_hash_key {
    uint32_t key_kind;
    uint32_t scope_uri;
    uint32_t scope_local_name;
    sxml_byte_view text;
} sxml_exi_hash_key;

static uint64_t sxml_exi_hash_of(const sxml_exi_hash_key *key) {
    uint64_t hash = UINT64_C(1469598103934665603);
    const uint32_t scope[3] = {key->key_kind, key->scope_uri, key->scope_local_name};
    for (size_t scope_index = 0; scope_index < 3; scope_index++) {
        for (uint32_t shift = 0; shift < 32; shift += 8) {
            hash = (hash ^ ((scope[scope_index] >> shift) & 0xFFU)) * UINT64_C(1099511628211);
        }
    }
    for (size_t byte_index = 0; byte_index < key->text.byte_count; byte_index++) {
        hash = (hash ^ key->text.bytes[byte_index]) * UINT64_C(1099511628211);
    }
    return hash;
}

static bool sxml_exi_slot_matches(const sxml_exi_string_table *table, const sxml_exi_hash_slot *slot,
                                  const sxml_exi_hash_key *key) {
    if (slot->key_kind != key->key_kind || slot->scope_uri != key->scope_uri ||
        slot->scope_local_name != key->scope_local_name || slot->text.byte_count != key->text.byte_count) {
        return false;
    }
    return key->text.byte_count == 0 ||
           memcmp(table->text_bytes + slot->text.offset, key->text.bytes, key->text.byte_count) == 0;
}

/* The slot of key, or of the empty slot where it would go. */
static size_t sxml_exi_hash_probe(const sxml_exi_string_table *table, const sxml_exi_hash_key *key, uint64_t hash) {
    const size_t mask = table->slot_count - 1;
    size_t slot_index = (size_t)hash & mask;
    while (table->slots[slot_index].is_used &&
           !(table->slots[slot_index].hash == hash && sxml_exi_slot_matches(table, &table->slots[slot_index], key))) {
        slot_index = (slot_index + 1) & mask;
    }
    return slot_index;
}

static sxml_status sxml_exi_hash_grow(sxml_exi_string_table *table) {
    const size_t new_count = table->slot_count == 0 ? 256 : table->slot_count * 2;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        table->allocator,
        (sxml_array_allocation_request){
            .element_count = new_count, .element_size = sizeof(sxml_exi_hash_slot), .element_alignment = alignof(sxml_exi_hash_slot)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_exi_hash_slot *new_slots = memory;
    memset(new_slots, 0, new_count * sizeof *new_slots);
    for (size_t slot_index = 0; slot_index < table->slot_count; slot_index++) {
        const sxml_exi_hash_slot *slot = &table->slots[slot_index];
        if (!slot->is_used) {
            continue;
        }
        size_t target = (size_t)slot->hash & (new_count - 1);
        while (new_slots[target].is_used) {
            target = (target + 1) & (new_count - 1);
        }
        new_slots[target] = *slot;
    }
    sxml_allocator_release(table->allocator, table->slots);
    table->slots = new_slots;
    table->slot_count = new_count;
    return SXML_STATUS_OK;
}

static sxml_exi_lookup sxml_exi_hash_find(const sxml_exi_string_table *table, const sxml_exi_hash_key *key) {
    if (table->slot_count == 0) {
        return (sxml_exi_lookup){.is_found = false, .index = 0};
    }
    const sxml_exi_hash_slot *slot = &table->slots[sxml_exi_hash_probe(table, key, sxml_exi_hash_of(key))];
    if (!slot->is_used || slot->target == SXML_EXI_NONE) {
        return (sxml_exi_lookup){.is_found = false, .index = 0};
    }
    return (sxml_exi_lookup){.is_found = true, .index = slot->target};
}

/* Maps key, whose text is already in the store as text, to target. */
typedef struct sxml_exi_hash_entry {
    sxml_exi_hash_key key;
    sxml_exi_text text;
    uint32_t target;
} sxml_exi_hash_entry;

static sxml_status sxml_exi_hash_put(sxml_exi_string_table *table, const sxml_exi_hash_entry *entry) {
    if ((table->used_slot_count + 1) * 4 > table->slot_count * 3) {
        sxml_status status = sxml_exi_hash_grow(table);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    const uint64_t hash = sxml_exi_hash_of(&entry->key);
    sxml_exi_hash_slot *slot = &table->slots[sxml_exi_hash_probe(table, &entry->key, hash)];
    if (!slot->is_used) {
        table->used_slot_count++;
    }
    *slot = (sxml_exi_hash_slot){.hash = hash,
                                 .key_kind = entry->key.key_kind,
                                 .scope_uri = entry->key.scope_uri,
                                 .scope_local_name = entry->key.scope_local_name,
                                 .text = entry->text,
                                 .target = entry->target,
                                 .is_used = true};
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------- Partitions */

sxml_status sxml_exi_find_uri(sxml_exi_string_table *table, const sxml_exi_string_key *key, sxml_exi_lookup *lookup) {
    *lookup = sxml_exi_hash_find(table, &(sxml_exi_hash_key){.key_kind = SXML_EXI_KEY_URI, .text = key->text});
    return SXML_STATUS_OK;
}

sxml_status sxml_exi_add_uri(sxml_exi_string_table *table, const sxml_exi_string_key *key, uint32_t *uri) {
    void *elements = table->uris;
    sxml_status status = sxml_exi_reserve(
        table->allocator, &(sxml_exi_growth){.elements = &elements,
                                             .capacity = &table->uri_capacity,
                                             .element_count = table->uri_count,
                                             .element_size = sizeof(sxml_exi_uri)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    table->uris = elements;
    sxml_exi_text text = {0};
    status = sxml_exi_store_text(table, key->text, &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint32_t index = (uint32_t)table->uri_count;
    status = sxml_exi_hash_put(table, &(sxml_exi_hash_entry){
                                          .key = {.key_kind = SXML_EXI_KEY_URI, .text = key->text},
                                          .text = text,
                                          .target = index});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    table->uris[index] = (sxml_exi_uri){.text = text};
    table->uri_count++;
    *uri = index;
    return SXML_STATUS_OK;
}

sxml_status sxml_exi_find_prefix(sxml_exi_string_table *table, const sxml_exi_string_key *key,
                                 sxml_exi_lookup *lookup) {
    *lookup = sxml_exi_hash_find(table, &(sxml_exi_hash_key){
                                            .key_kind = SXML_EXI_KEY_PREFIX, .scope_uri = key->uri, .text = key->text});
    return SXML_STATUS_OK;
}

sxml_status sxml_exi_add_prefix(sxml_exi_string_table *table, const sxml_exi_string_key *key, uint32_t *prefix) {
    if (key->uri >= table->uri_count) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_exi_uri *uri = &table->uris[key->uri];
    void *elements = uri->prefixes;
    sxml_status status = sxml_exi_reserve(
        table->allocator, &(sxml_exi_growth){.elements = &elements,
                                             .capacity = &uri->prefix_capacity,
                                             .element_count = uri->prefix_count,
                                             .element_size = sizeof(sxml_exi_text)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uri->prefixes = elements;
    sxml_exi_text text = {0};
    status = sxml_exi_store_text(table, key->text, &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint32_t index = (uint32_t)uri->prefix_count;
    status = sxml_exi_hash_put(table, &(sxml_exi_hash_entry){
                                          .key = {.key_kind = SXML_EXI_KEY_PREFIX, .scope_uri = key->uri, .text = key->text},
                                          .text = text,
                                          .target = index});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uri = &table->uris[key->uri];
    uri->prefixes[index] = text;
    uri->prefix_count++;
    *prefix = index;
    return SXML_STATUS_OK;
}

sxml_status sxml_exi_find_local_name(sxml_exi_string_table *table, const sxml_exi_string_key *key,
                                     sxml_exi_lookup *lookup) {
    *lookup = sxml_exi_hash_find(table, &(sxml_exi_hash_key){
                                            .key_kind = SXML_EXI_KEY_LOCAL_NAME, .scope_uri = key->uri, .text = key->text});
    return SXML_STATUS_OK;
}

sxml_status sxml_exi_add_local_name(sxml_exi_string_table *table, const sxml_exi_string_key *key,
                                    uint32_t *local_name) {
    if (key->uri >= table->uri_count) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_exi_uri *uri = &table->uris[key->uri];
    void *elements = uri->local_names;
    sxml_status status = sxml_exi_reserve(
        table->allocator, &(sxml_exi_growth){.elements = &elements,
                                             .capacity = &uri->local_name_capacity,
                                             .element_count = uri->local_name_count,
                                             .element_size = sizeof(sxml_exi_local_name)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uri->local_names = elements;
    sxml_exi_text text = {0};
    status = sxml_exi_store_text(table, key->text, &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint32_t index = (uint32_t)uri->local_name_count;
    status = sxml_exi_hash_put(table, &(sxml_exi_hash_entry){
                                          .key = {.key_kind = SXML_EXI_KEY_LOCAL_NAME, .scope_uri = key->uri, .text = key->text},
                                          .text = text,
                                          .target = index});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uri = &table->uris[key->uri];
    uri->local_names[index] = (sxml_exi_local_name){.text = text,
                                                    .element_grammar = SXML_EXI_NONE,
                                                    .schema_element_grammar = SXML_EXI_NONE,
                                                    .type_grammar = SXML_EXI_NONE,
                                                    .attribute_datatype = SXML_EXI_NONE};
    uri->local_name_count++;
    *local_name = index;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------- Values */

sxml_status sxml_exi_find_local_value(sxml_exi_string_table *table, const sxml_exi_string_key *key,
                                      sxml_exi_lookup *lookup) {
    *lookup = sxml_exi_hash_find(table, &(sxml_exi_hash_key){.key_kind = SXML_EXI_KEY_LOCAL_VALUE,
                                                              .scope_uri = key->uri,
                                                              .scope_local_name = key->local_name,
                                                              .text = key->text});
    return SXML_STATUS_OK;
}

sxml_status sxml_exi_find_global_value(sxml_exi_string_table *table, const sxml_exi_string_key *key,
                                       sxml_exi_lookup *lookup) {
    *lookup = sxml_exi_hash_find(table, &(sxml_exi_hash_key){.key_kind = SXML_EXI_KEY_GLOBAL_VALUE, .text = key->text});
    return SXML_STATUS_OK;
}

uint32_t sxml_exi_local_value_entry(const sxml_exi_string_table *table, sxml_exi_value_reference reference) {
    if (reference.uri >= table->uri_count) {
        return SXML_EXI_NONE;
    }
    const sxml_exi_uri *uri = &table->uris[reference.uri];
    if (reference.local_name >= uri->local_name_count) {
        return SXML_EXI_NONE;
    }
    const sxml_exi_local_name *local_name = &uri->local_names[reference.local_name];
    if (reference.compact_identifier >= local_name->local_value_count) {
        return SXML_EXI_NONE;
    }
    return local_name->local_values[reference.compact_identifier];
}

uint32_t sxml_exi_global_value_entry(const sxml_exi_string_table *table, uint32_t compact_identifier) {
    if (compact_identifier >= table->global_value_count) {
        return SXML_EXI_NONE;
    }
    return table->global_values[compact_identifier];
}

/* Removes the value in global slot from its local partition: its compact
   identifier there stays assigned to nothing (7.3.3). */
static void sxml_exi_evict_global(sxml_exi_string_table *table, uint32_t global_slot) {
    const uint32_t evicted = table->global_values[global_slot];
    const sxml_exi_value *value = &table->values[evicted];
    sxml_exi_local_name *owner = &table->uris[value->uri].local_names[value->local_name];
    owner->local_values[value->local_identifier] = SXML_EXI_NONE;
    const sxml_byte_view text = sxml_exi_string_table_view(table, value->text);
    const sxml_exi_hash_key local_key = {.key_kind = SXML_EXI_KEY_LOCAL_VALUE,
                                         .scope_uri = value->uri,
                                         .scope_local_name = value->local_name,
                                         .text = text};
    const sxml_exi_hash_key global_key = {.key_kind = SXML_EXI_KEY_GLOBAL_VALUE, .text = text};
    const sxml_exi_hash_key *keys[2] = {&local_key, &global_key};
    const uint32_t expected[2] = {value->local_identifier, global_slot};
    for (size_t key_index = 0; key_index < 2; key_index++) {
        sxml_exi_hash_slot *slot = &table->slots[sxml_exi_hash_probe(table, keys[key_index], sxml_exi_hash_of(keys[key_index]))];
        if (slot->is_used && slot->target == expected[key_index]) {
            slot->target = SXML_EXI_NONE;
        }
    }
}

sxml_status sxml_exi_add_value(sxml_exi_string_table *table, const sxml_exi_string_key *key) {
    const uint64_t codepoint_count = sxml_exi_utf8_codepoint_count(key->text);
    /* 7.3.3: added when the capacity is not zero and 0 < L <= valueMaxLength. */
    if (table->value_partition_capacity == 0 || codepoint_count == 0 || codepoint_count > table->value_maximum_length) {
        return SXML_STATUS_OK;
    }
    if (key->uri >= table->uri_count || key->local_name >= table->uris[key->uri].local_name_count) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    void *elements = table->values;
    sxml_status status = sxml_exi_reserve(
        table->allocator, &(sxml_exi_growth){.elements = &elements,
                                             .capacity = &table->value_capacity,
                                             .element_count = table->value_count,
                                             .element_size = sizeof(sxml_exi_value)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    table->values = elements;
    sxml_exi_local_name *owner = &table->uris[key->uri].local_names[key->local_name];
    elements = owner->local_values;
    status = sxml_exi_reserve(table->allocator, &(sxml_exi_growth){.elements = &elements,
                                                                   .capacity = &owner->local_value_capacity,
                                                                   .element_count = owner->local_value_count,
                                                                   .element_size = sizeof(uint32_t)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    owner->local_values = elements;
    const bool replaces = (uint64_t)table->global_value_count >= table->value_partition_capacity;
    if (!replaces) {
        elements = table->global_values;
        status = sxml_exi_reserve(table->allocator, &(sxml_exi_growth){.elements = &elements,
                                                                       .capacity = &table->global_value_capacity,
                                                                       .element_count = table->global_value_count,
                                                                       .element_size = sizeof(uint32_t)});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        table->global_values = elements;
    }
    sxml_exi_text text = {0};
    status = sxml_exi_store_text(table, key->text, &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Two hash entries may be needed; reserve room for both first, so a
       failure leaves the partitions as they were. */
    if ((table->used_slot_count + 2) * 4 > table->slot_count * 3) {
        status = sxml_exi_hash_grow(table);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    const uint32_t entry = (uint32_t)table->value_count;
    const uint32_t global_slot = table->global_identifier;
    if (replaces) {
        sxml_exi_evict_global(table, global_slot);
    }
    owner = &table->uris[key->uri].local_names[key->local_name];
    const uint32_t local_identifier = (uint32_t)owner->local_value_count;
    table->values[entry] = (sxml_exi_value){
        .text = text, .uri = key->uri, .local_name = key->local_name, .local_identifier = local_identifier};
    table->value_count++;
    owner->local_values[local_identifier] = entry;
    owner->local_value_count++;
    if (replaces) {
        table->global_values[global_slot] = entry;
    } else {
        table->global_values[table->global_value_count++] = entry;
    }
    table->global_identifier++;
    if ((uint64_t)table->global_identifier == table->value_partition_capacity) {
        table->global_identifier = 0;
    }
    status = sxml_exi_hash_put(table, &(sxml_exi_hash_entry){.key = {.key_kind = SXML_EXI_KEY_LOCAL_VALUE,
                                                                      .scope_uri = key->uri,
                                                                      .scope_local_name = key->local_name,
                                                                      .text = key->text},
                                                              .text = text,
                                                              .target = local_identifier});
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_hash_put(table, &(sxml_exi_hash_entry){
                                              .key = {.key_kind = SXML_EXI_KEY_GLOBAL_VALUE, .text = key->text},
                                              .text = text,
                                              .target = global_slot});
    }
    return status;
}

/* ------------------------------------------------------------ Lifecycle */

/* Adds a URI with its initial prefix and local-names, and the schema's
   local-names of the same URI: all sorted lexicographically (D.3). */
typedef struct sxml_exi_initial_uri {
    sxml_byte_view uri;
    bool has_prefix;
    sxml_byte_view prefix;
    const sxml_byte_view *local_names;
    size_t local_name_count;
} sxml_exi_initial_uri;

static bool sxml_exi_views_equal(sxml_byte_view first, sxml_byte_view second) {
    return first.byte_count == second.byte_count &&
           (first.byte_count == 0 || memcmp(first.bytes, second.bytes, first.byte_count) == 0);
}

/* Lexicographical order of UTF-8, which is the order of code points. */
static int sxml_exi_compare_views(const void *first, const void *second) {
    const sxml_byte_view *left = first;
    const sxml_byte_view *right = second;
    const size_t common = left->byte_count < right->byte_count ? left->byte_count : right->byte_count;
    const int order = common == 0 ? 0 : memcmp(left->bytes, right->bytes, common);
    if (order != 0) {
        return order;
    }
    return left->byte_count < right->byte_count ? -1 : left->byte_count > right->byte_count ? 1 : 0;
}

/* The initial URI and the schema partitions to add with it. */
typedef struct sxml_exi_uri_entries {
    const sxml_exi_initial_uri *initial;
    sxml_exi_schema_partition_view schema;
} sxml_exi_uri_entries;

static sxml_status sxml_exi_add_initial_uri(sxml_exi_string_table *table, const sxml_exi_uri_entries *entries) {
    const sxml_exi_initial_uri *initial = entries->initial;
    size_t name_count = initial->local_name_count;
    for (size_t partition_index = 0; partition_index < entries->schema.partition_count; partition_index++) {
        const sxml_exi_schema_partition *partition = &entries->schema.partitions[partition_index];
        if (sxml_exi_views_equal(partition->uri, initial->uri)) {
            name_count += partition->local_name_count;
        }
    }
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        table->allocator,
        (sxml_array_allocation_request){
            .element_count = name_count, .element_size = sizeof(sxml_byte_view), .element_alignment = alignof(sxml_byte_view)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_byte_view *names = memory;
    size_t filled = 0;
    for (size_t name_index = 0; name_index < initial->local_name_count; name_index++) {
        names[filled++] = initial->local_names[name_index];
    }
    for (size_t partition_index = 0; partition_index < entries->schema.partition_count; partition_index++) {
        const sxml_exi_schema_partition *partition = &entries->schema.partitions[partition_index];
        if (!sxml_exi_views_equal(partition->uri, initial->uri)) {
            continue;
        }
        for (size_t name_index = 0; name_index < partition->local_name_count; name_index++) {
            names[filled++] = partition->local_names[name_index];
        }
    }
    if (filled > 1) {
        qsort(names, filled, sizeof names[0], sxml_exi_compare_views);
    }
    uint32_t uri = 0;
    status = sxml_exi_add_uri(table, &(sxml_exi_string_key){.text = initial->uri}, &uri);
    if (status == SXML_STATUS_OK && initial->has_prefix) {
        uint32_t prefix = 0;
        status = sxml_exi_add_prefix(table, &(sxml_exi_string_key){.text = initial->prefix, .uri = uri}, &prefix);
    }
    for (size_t name_index = 0; name_index < filled && status == SXML_STATUS_OK; name_index++) {
        if (name_index > 0 && sxml_exi_views_equal(names[name_index], names[name_index - 1])) {
            continue;
        }
        uint32_t local_name = 0;
        status = sxml_exi_add_local_name(table, &(sxml_exi_string_key){.text = names[name_index], .uri = uri},
                                         &local_name);
    }
    sxml_allocator_release(table->allocator, names);
    return status;
}

/* The schema's URIs that are not initial ones, sorted and each added once
   (7.3.1). */
static sxml_status sxml_exi_add_schema_uris(sxml_exi_string_table *table, const sxml_exi_uri_entries *entries) {
    const sxml_exi_schema_partition_view schema = entries->schema;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        table->allocator,
        (sxml_array_allocation_request){.element_count = schema.partition_count,
                                        .element_size = sizeof(sxml_byte_view),
                                        .element_alignment = alignof(sxml_byte_view)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_byte_view *uris = memory;
    size_t uri_count = 0;
    for (size_t partition_index = 0; partition_index < schema.partition_count; partition_index++) {
        sxml_exi_lookup found = {0};
        status = sxml_exi_find_uri(table, &(sxml_exi_string_key){.text = schema.partitions[partition_index].uri}, &found);
        if (status == SXML_STATUS_OK && !found.is_found) {
            uris[uri_count++] = schema.partitions[partition_index].uri;
        }
    }
    if (uri_count > 1) {
        qsort(uris, uri_count, sizeof uris[0], sxml_exi_compare_views);
    }
    for (size_t uri_index = 0; uri_index < uri_count && status == SXML_STATUS_OK; uri_index++) {
        if (uri_index > 0 && sxml_exi_views_equal(uris[uri_index], uris[uri_index - 1])) {
            continue;
        }
        const sxml_exi_initial_uri initial = {.uri = uris[uri_index]};
        status = sxml_exi_add_initial_uri(table, &(sxml_exi_uri_entries){.initial = &initial, .schema = schema});
    }
    sxml_allocator_release(table->allocator, uris);
    return status;
}

sxml_status sxml_exi_string_table_initialize(sxml_exi_string_table *table,
                                             const sxml_exi_string_table_configuration *configuration) {
    *table = (sxml_exi_string_table){
        .allocator = configuration->allocator,
        .value_partition_capacity = UINT64_MAX,
        .value_maximum_length = UINT64_MAX,
    };
    if (configuration->options->has_value_partition_capacity) {
        table->value_partition_capacity = configuration->options->value_partition_capacity;
    }
    if (configuration->options->has_value_max_length) {
        table->value_maximum_length = configuration->options->value_max_length;
    }
    static const sxml_byte_view xml_names[] = {SXML_BYTE_VIEW_INITIALIZER("base"), SXML_BYTE_VIEW_INITIALIZER("id"),
                                               SXML_BYTE_VIEW_INITIALIZER("lang"), SXML_BYTE_VIEW_INITIALIZER("space")};
    static const sxml_byte_view xsi_names[] = {SXML_BYTE_VIEW_INITIALIZER("nil"), SXML_BYTE_VIEW_INITIALIZER("type")};
    const sxml_exi_initial_uri initial[] = {
        {.uri = SXML_BYTE_VIEW_INITIALIZER(""), .has_prefix = true, .prefix = SXML_BYTE_VIEW_INITIALIZER("")},
        {.uri = SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/XML/1998/namespace"),
         .has_prefix = true,
         .prefix = SXML_BYTE_VIEW_INITIALIZER("xml"),
         .local_names = xml_names,
         .local_name_count = 4},
        {.uri = SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/2001/XMLSchema-instance"),
         .has_prefix = true,
         .prefix = SXML_BYTE_VIEW_INITIALIZER("xsi"),
         .local_names = xsi_names,
         .local_name_count = 2},
        {.uri = SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/2001/XMLSchema"),
         .local_names = sxml_exi_xml_schema_type_names,
         .local_name_count = sizeof sxml_exi_xml_schema_type_names / sizeof sxml_exi_xml_schema_type_names[0]},
    };
    const size_t initial_count = configuration->is_schema_informed ? 4 : 3;
    const sxml_exi_schema_partition_view schema =
        configuration->is_schema_informed ? configuration->schema_partitions : (sxml_exi_schema_partition_view){0};
    sxml_status status = SXML_STATUS_OK;
    for (size_t uri_index = 0; uri_index < initial_count && status == SXML_STATUS_OK; uri_index++) {
        status = sxml_exi_add_initial_uri(table, &(sxml_exi_uri_entries){.initial = &initial[uri_index], .schema = schema});
    }
    if (status == SXML_STATUS_OK && schema.partition_count > 0) {
        status = sxml_exi_add_schema_uris(table, &(sxml_exi_uri_entries){.schema = schema});
    }
    if (status != SXML_STATUS_OK) {
        sxml_exi_string_table_finalize(table);
    }
    return status;
}

void sxml_exi_string_table_finalize(sxml_exi_string_table *table) {
    for (size_t uri_index = 0; uri_index < table->uri_count; uri_index++) {
        sxml_exi_uri *uri = &table->uris[uri_index];
        for (size_t name_index = 0; name_index < uri->local_name_count; name_index++) {
            sxml_allocator_release(table->allocator, uri->local_names[name_index].local_values);
        }
        sxml_allocator_release(table->allocator, uri->prefixes);
        sxml_allocator_release(table->allocator, uri->local_names);
    }
    sxml_allocator_release(table->allocator, table->uris);
    sxml_allocator_release(table->allocator, table->values);
    sxml_allocator_release(table->allocator, table->global_values);
    sxml_allocator_release(table->allocator, table->slots);
    sxml_allocator_release(table->allocator, table->text_bytes);
    *table = (sxml_exi_string_table){.allocator = table->allocator};
}
