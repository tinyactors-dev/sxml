/*
 * The type registry: built-in and registered types, derivation and the
 * subtype relation (include/sxml/types.h, docs/DECISIONS.md D32 and D58).
 *
 * Imitates the golden module src/memory.c, and src/name.c for its hash
 * indexes and the arena placed in the object's own memory.
 *
 * Layout. Built-in types are static descriptions (src/types_builtin.c);
 * their names are interned into the name pool at initialization and found
 * through a fixed hash index inside the registry. Registered types are an
 * entry array in issue order (identifier SXML_TYPE_FIRST_REGISTERED plus
 * position) with two open-addressing hash indexes: one over the structure
 * of interned item types, one over the names of named types (nominative
 * record types). Everything a description points to (member lists, field
 * lists, strings) is copied into an arena that is released at finalize; a
 * registration that fails after copying leaves its copies there unused.
 *
 * Normal form. Registered descriptions are normalized before they are
 * compared, so that fields a description's flags make meaningless never
 * tell two equal types apart (see sxml_type_registry_normalize).
 *
 * Strings of a type. Record types (their field names) and enumeration
 * types (their values) get a hash index over their strings, built when the
 * type is registered, so that sxml_type_registry_find_record_field and
 * enumeration membership are O(1) on average and duplicates are found.
 *
 * Subtyping. sxml_type_registry_is_subtype implements XPath 4.0 section
 * 3.3.2 rule by rule; the transitivity rule (3.3.2.1) is built into the
 * individual rules where it adds pairs (maps, arrays and records as
 * functions, records as maps). The rules reduce a question to an and/or
 * tree of subtype goals, evaluated on an explicit stack (D71), as is the
 * search through union members for derivation. Every rule is monotone, so
 * a search that runs out of its budget answers false, never a wrong true.
 */
#include "types_internal.h"

#include "name_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------- Constants */

/* Size of each arena block taken from the allocator. */
#define SXML_TYPE_REGISTRY_ARENA_BLOCK_BYTE_COUNT 4096U
/* Slot count of the fixed index over built-in type names: a power of two
   at least twice the number of built-in types. */
#define SXML_TYPE_BUILTIN_NAME_SLOT_COUNT 256U
/* Slot count of a hash index when it first holds an entry. */
#define SXML_TYPE_MINIMUM_SLOT_COUNT 16U
/* Entry capacity of the entry array when it first holds an entry. */
#define SXML_TYPE_MINIMUM_ENTRY_CAPACITY 16U
/* Most registered types, so that every identifier fits in 32 bits. */
#define SXML_TYPE_MAXIMUM_ENTRY_COUNT                                          \
    ((size_t)UINT32_MAX - SXML_TYPE_FIRST_REGISTERED)
/* Nesting depth of item goals and number of rule applications one subtype
   question may use, and the frames of its explicit stack, which live in
   the caller's C stack frame (the subtype queries never allocate); see D58.
   Reaching any of them answers false. */
#define SXML_TYPE_SUBTYPE_MAXIMUM_DEPTH 128U
#define SXML_TYPE_SUBTYPE_MAXIMUM_STEP_COUNT 1000000U
#define SXML_TYPE_SUBTYPE_MAXIMUM_FRAME_COUNT 192U
/* Most base types followed in one derivation question; deeper hierarchies
   do not exist in a registry. */
#define SXML_TYPE_DERIVATION_MAXIMUM_DEPTH 256U

/* ------------------------------------------------------------ Structures */

/* One slot of a hash index. */
typedef struct sxml_type_slot {
    /* Entry position plus one; 0 marks an empty slot. */
    uint32_t entry_number;
    uint32_t hash;
} sxml_type_slot;

typedef struct sxml_type_hash_index {
    /* slot_count slots; NULL while slot_count is 0. */
    sxml_type_slot *slots;
    /* 0 or a power of two. */
    size_t slot_count;
} sxml_type_hash_index;

/* A registered type. */
typedef struct sxml_type_entry {
    /* Normalized; views point into the registry's arena. */
    sxml_type_description description;
    /* Hash of the description, for the structure index. */
    uint32_t hash;
    /* For a nominative record type: its fields have been defined. */
    bool is_defined;
    /* Hash index over the field names of a record type or the values of
       an enumeration type; entry_number is the string's position plus one.
       Lives in the arena. */
    sxml_type_hash_index string_index;
} sxml_type_entry;

struct sxml_type_registry {
    sxml_type_registry_configuration configuration;
    /* The allocator that sxml_type_registry_create allocated the registry
       with; the null allocator for a registry placed by initialize. */
    sxml_allocator owning_allocator;
    /* Holds the copies of registered descriptions; placed in the
       registry's own memory. */
    sxml_arena *arena;
    /* The interned names of the built-in types, 0 for unnamed ones. */
    sxml_name_identifier builtin_names[SXML_TYPE_BUILTIN_COUNT];
    /* Built-in type names to built-in types (entry_number is the
       built-in type itself). */
    sxml_type_slot builtin_name_slots[SXML_TYPE_BUILTIN_NAME_SLOT_COUNT];
    sxml_type_entry *entries;
    size_t entry_count;
    size_t entry_capacity;
    /* Interned item types by the hash of their description, and how many
       entries it holds. */
    sxml_type_hash_index structure_index;
    size_t structural_count;
    /* Named registered types by the hash of their name, and how many. */
    sxml_type_hash_index name_index;
    size_t named_count;
};

/* ---------------------------------------------------------------- Hashing */

/* 32-bit FNV-1a, as in src/name.c. */
#define SXML_TYPE_HASH_OFFSET_BASIS 2166136261U
#define SXML_TYPE_HASH_PRIME 16777619U

static uint32_t sxml_type_hash_bytes(uint32_t hash, sxml_byte_view bytes) {
    for (size_t byte_index = 0; byte_index < bytes.byte_count;
         byte_index += 1) {
        hash ^= bytes.bytes[byte_index];
        hash *= SXML_TYPE_HASH_PRIME;
    }
    return hash;
}

/* Continues hash with the low four bytes of number. */
static uint32_t sxml_type_hash_number(uint32_t hash, uint64_t number) {
    for (size_t byte_index = 0; byte_index < sizeof(uint32_t);
         byte_index += 1) {
        hash ^= (uint32_t)((number >> (byte_index * 8U)) & 0xFFU);
        hash *= SXML_TYPE_HASH_PRIME;
    }
    return hash;
}

static uint32_t sxml_type_hash_string(uint32_t hash, sxml_byte_view text) {
    return sxml_type_hash_bytes(sxml_type_hash_number(hash, text.byte_count),
                                text);
}

static uint32_t sxml_type_hash_of_string(sxml_byte_view text) {
    return sxml_type_hash_bytes(SXML_TYPE_HASH_OFFSET_BASIS, text);
}

static uint32_t sxml_type_hash_of_name(sxml_name_identifier name) {
    return sxml_type_hash_number(SXML_TYPE_HASH_OFFSET_BASIS, name.index);
}

/* ------------------------------------------------------------ Hash index */

static size_t sxml_type_first_slot(size_t slot_count, uint32_t hash) {
    return (size_t)hash & (slot_count - 1);
}

static size_t sxml_type_next_slot(size_t slot_count, size_t slot_index) {
    return (slot_index + 1) & (slot_count - 1);
}

/* Puts slot into the first free slot of index, which has room. */
static void sxml_type_hash_index_insert(sxml_type_hash_index *index,
                                        sxml_type_slot slot) {
    size_t slot_index = sxml_type_first_slot(index->slot_count, slot.hash);
    while (index->slots[slot_index].entry_number != 0) {
        slot_index = sxml_type_next_slot(index->slot_count, slot_index);
    }
    index->slots[slot_index] = slot;
}

/* ------------------------------------------------------------ Allocation */

static sxml_status sxml_type_registry_allocate(
    const sxml_type_registry *registry, sxml_allocation_request request,
    void **memory) {
    return sxml_allocator_allocate(registry->configuration.allocator, request,
                                   memory);
}

static void sxml_type_registry_release(const sxml_type_registry *registry,
                                       void *memory) {
    if (memory != NULL) {
        sxml_allocator_release(registry->configuration.allocator, memory);
    }
}

/* Allocates element_count elements of element_byte_count bytes from the
   arena; nothing for a count of 0. */
typedef struct sxml_type_array_request {
    size_t element_count;
    size_t element_byte_count;
    size_t alignment;
} sxml_type_array_request;

static sxml_status sxml_type_registry_allocate_array(
    sxml_type_registry *registry, sxml_type_array_request request,
    void **memory) {
    if (request.element_count == 0) {
        *memory = NULL;
        return SXML_STATUS_OK;
    }
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(
        request.element_count, request.element_byte_count, &byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_arena_allocate(
        registry->arena,
        (sxml_allocation_request){.byte_count = byte_count,
                                  .alignment = request.alignment},
        memory);
}

/* Copies text into the arena; empty text needs no copy. */
static sxml_status sxml_type_registry_copy_string(sxml_type_registry *registry,
                                                  sxml_byte_view text,
                                                  sxml_byte_view *copy) {
    void *memory = NULL;
    sxml_status status = sxml_type_registry_allocate_array(
        registry,
        (sxml_type_array_request){.element_count = text.byte_count,
                                  .element_byte_count = 1,
                                  .alignment = 1},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (memory != NULL) {
        memcpy(memory, text.bytes, text.byte_count);
    }
    *copy = (sxml_byte_view){.bytes = memory, .byte_count = text.byte_count};
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------- Identifiers */

static bool sxml_type_is_builtin(sxml_type_identifier type) {
    return type.index > SXML_TYPE_NONE && type.index < SXML_TYPE_BUILTIN_COUNT;
}

static const sxml_type_entry *sxml_type_registry_entry_of(
    const sxml_type_registry *registry, sxml_type_identifier type) {
    if (type.index < SXML_TYPE_FIRST_REGISTERED) {
        return NULL;
    }
    size_t position = (size_t)type.index - SXML_TYPE_FIRST_REGISTERED;
    if (position >= registry->entry_count) {
        return NULL;
    }
    return &registry->entries[position];
}

/* The description of an issued type, NULL for one not issued. type_name of
   a built-in description is 0. */
static const sxml_type_description *sxml_type_registry_lookup(
    const sxml_type_registry *registry, sxml_type_identifier type) {
    if (sxml_type_is_builtin(type)) {
        return sxml_type_builtin_description((sxml_builtin_type)type.index);
    }
    const sxml_type_entry *entry = sxml_type_registry_entry_of(registry, type);
    return entry == NULL ? NULL : &entry->description;
}

static bool sxml_type_registry_issued(const sxml_type_registry *registry,
                                      sxml_type_identifier type) {
    return sxml_type_registry_lookup(registry, type) != NULL;
}

static sxml_type_identifier sxml_type_of_builtin(sxml_builtin_type builtin) {
    return sxml_builtin_type_identifier(builtin);
}

static bool sxml_type_is(sxml_type_identifier type, sxml_builtin_type builtin) {
    return type.index == (uint32_t)builtin;
}

/* True for the varieties that are item types (XPath 4.0, section 3.2). */
static bool sxml_type_variety_is_item_type(sxml_type_variety variety) {
    switch (variety) {
    case SXML_TYPE_VARIETY_ATOMIC:
    case SXML_TYPE_VARIETY_UNION:
    case SXML_TYPE_VARIETY_ANY_ITEM:
    case SXML_TYPE_VARIETY_NODE_TEST:
    case SXML_TYPE_VARIETY_JSON_NODE_TEST:
    case SXML_TYPE_VARIETY_FUNCTION:
    case SXML_TYPE_VARIETY_MAP:
    case SXML_TYPE_VARIETY_ARRAY:
    case SXML_TYPE_VARIETY_RECORD:
    case SXML_TYPE_VARIETY_ENUMERATION:
    case SXML_TYPE_VARIETY_CHOICE:
        return true;
    case SXML_TYPE_VARIETY_COMPLEX:
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_LIST:
        return false;
    }
    return false;
}

static bool sxml_type_registry_is_item_type(const sxml_type_registry *registry,
                                            sxml_type_identifier type) {
    const sxml_type_description *description =
        sxml_type_registry_lookup(registry, type);
    return description != NULL &&
           sxml_type_variety_is_item_type(description->variety);
}

/* ------------------------------------------------------------ Equality */

static bool sxml_type_sequence_types_equal(sxml_sequence_type first,
                                           sxml_sequence_type second) {
    if (first.occurrence != second.occurrence) {
        return false;
    }
    /* empty-sequence() ignores its item type. */
    return first.occurrence == SXML_OCCURRENCE_ZERO ||
           sxml_type_identifier_equals(first.item_type, second.item_type);
}

static bool sxml_type_name_tests_equal(const sxml_name_test *first,
                                       const sxml_name_test *second) {
    return first->name_test_kind == second->name_test_kind &&
           first->expanded_name.index == second->expanded_name.index &&
           first->namespace_uri.index == second->namespace_uri.index &&
           sxml_byte_view_equals(first->local_name, second->local_name);
}

static bool sxml_type_node_tests_equal(const sxml_node_test *first,
                                       const sxml_node_test *second) {
    return first->node_kind == second->node_kind &&
           sxml_type_name_tests_equal(&first->name_test, &second->name_test) &&
           sxml_type_identifier_equals(first->type_annotation,
                                       second->type_annotation) &&
           first->allows_nilled == second->allows_nilled &&
           first->is_schema_declaration == second->is_schema_declaration &&
           sxml_type_identifier_equals(first->document_element_test,
                                       second->document_element_test);
}

static bool sxml_type_json_node_tests_equal(
    const sxml_json_node_test *first, const sxml_json_node_test *second) {
    return first->key_constraint == second->key_constraint &&
           sxml_type_identifier_equals(first->key_type, second->key_type) &&
           sxml_byte_view_equals(first->key_lexical_form,
                                 second->key_lexical_form) &&
           first->has_value_type == second->has_value_type &&
           sxml_type_sequence_types_equal(first->value_type,
                                          second->value_type);
}

static bool sxml_type_function_types_equal(const sxml_function_type *first,
                                           const sxml_function_type *second) {
    if (first->is_any_function != second->is_any_function ||
        first->parameter_types.sequence_type_count !=
            second->parameter_types.sequence_type_count ||
        !sxml_type_sequence_types_equal(first->result_type,
                                        second->result_type)) {
        return false;
    }
    for (size_t parameter_index = 0;
         parameter_index < first->parameter_types.sequence_type_count;
         parameter_index += 1) {
        if (!sxml_type_sequence_types_equal(
                first->parameter_types.sequence_types[parameter_index],
                second->parameter_types.sequence_types[parameter_index])) {
            return false;
        }
    }
    return true;
}

static bool sxml_type_record_types_equal(const sxml_record_type *first,
                                         const sxml_record_type *second) {
    if (first->is_any_record != second->is_any_record ||
        first->is_nominative != second->is_nominative ||
        first->record_fields.record_field_count !=
            second->record_fields.record_field_count) {
        return false;
    }
    for (size_t field_index = 0;
         field_index < first->record_fields.record_field_count;
         field_index += 1) {
        const sxml_record_field *first_field =
            &first->record_fields.record_fields[field_index];
        const sxml_record_field *second_field =
            &second->record_fields.record_fields[field_index];
        if (!sxml_byte_view_equals(first_field->field_name,
                                   second_field->field_name) ||
            !sxml_type_sequence_types_equal(first_field->field_type,
                                            second_field->field_type)) {
            return false;
        }
    }
    return true;
}

static bool sxml_type_identifier_views_equal(sxml_type_identifier_view first,
                                             sxml_type_identifier_view second) {
    if (first.type_identifier_count != second.type_identifier_count) {
        return false;
    }
    for (size_t member_index = 0; member_index < first.type_identifier_count;
         member_index += 1) {
        if (!sxml_type_identifier_equals(first.type_identifiers[member_index],
                                         second.type_identifiers[member_index])) {
            return false;
        }
    }
    return true;
}

static bool sxml_type_byte_view_lists_equal(sxml_byte_view_list first,
                                            sxml_byte_view_list second) {
    if (first.byte_view_count != second.byte_view_count) {
        return false;
    }
    for (size_t value_index = 0; value_index < first.byte_view_count;
         value_index += 1) {
        if (!sxml_byte_view_equals(first.byte_views[value_index],
                                   second.byte_views[value_index])) {
            return false;
        }
    }
    return true;
}

/* Equality of two normalized item type descriptions. */
static bool sxml_type_descriptions_equal(const sxml_type_description *first,
                                         const sxml_type_description *second) {
    if (first->variety != second->variety) {
        return false;
    }
    const sxml_type_description *left = first;
    const sxml_type_description *right = second;
    switch (left->variety) {
    case SXML_TYPE_VARIETY_NODE_TEST:
        return sxml_type_node_tests_equal(&left->variety_details.node_test,
                                          &right->variety_details.node_test);
    case SXML_TYPE_VARIETY_JSON_NODE_TEST:
        return sxml_type_json_node_tests_equal(
            &left->variety_details.json_node_test,
            &right->variety_details.json_node_test);
    case SXML_TYPE_VARIETY_FUNCTION:
        return sxml_type_function_types_equal(
            &left->variety_details.function_type,
            &right->variety_details.function_type);
    case SXML_TYPE_VARIETY_MAP:
        return left->variety_details.map_type.is_any_map ==
                   right->variety_details.map_type.is_any_map &&
               sxml_type_identifier_equals(
                   left->variety_details.map_type.key_type,
                   right->variety_details.map_type.key_type) &&
               sxml_type_sequence_types_equal(
                   left->variety_details.map_type.value_type,
                   right->variety_details.map_type.value_type);
    case SXML_TYPE_VARIETY_ARRAY:
        return left->variety_details.array_type.is_any_array ==
                   right->variety_details.array_type.is_any_array &&
               sxml_type_sequence_types_equal(
                   left->variety_details.array_type.member_type,
                   right->variety_details.array_type.member_type);
    case SXML_TYPE_VARIETY_RECORD:
        return sxml_type_record_types_equal(&left->variety_details.record_type,
                                            &right->variety_details.record_type);
    case SXML_TYPE_VARIETY_ENUMERATION:
        return sxml_type_byte_view_lists_equal(
            left->variety_details.enumeration_values,
            right->variety_details.enumeration_values);
    case SXML_TYPE_VARIETY_CHOICE:
        return sxml_type_identifier_views_equal(
            left->variety_details.member_types,
            right->variety_details.member_types);
    case SXML_TYPE_VARIETY_COMPLEX:
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_ATOMIC:
    case SXML_TYPE_VARIETY_LIST:
    case SXML_TYPE_VARIETY_UNION:
    case SXML_TYPE_VARIETY_ANY_ITEM:
        return false;
    }
    return false;
}

/* ---------------------------------------------------- Structural hashing */

static uint32_t sxml_type_hash_sequence_type(uint32_t hash,
                                             sxml_sequence_type sequence_type) {
    hash = sxml_type_hash_number(hash, (uint64_t)sequence_type.occurrence);
    if (sequence_type.occurrence == SXML_OCCURRENCE_ZERO) {
        return hash;
    }
    return sxml_type_hash_number(hash, sequence_type.item_type.index);
}

static uint32_t sxml_type_hash_node_test(uint32_t hash,
                                         const sxml_node_test *node_test) {
    hash = sxml_type_hash_number(hash, (uint64_t)node_test->node_kind);
    hash = sxml_type_hash_number(hash,
                                 (uint64_t)node_test->name_test.name_test_kind);
    hash = sxml_type_hash_number(hash, node_test->name_test.expanded_name.index);
    hash = sxml_type_hash_number(hash, node_test->name_test.namespace_uri.index);
    hash = sxml_type_hash_string(hash, node_test->name_test.local_name);
    hash = sxml_type_hash_number(hash, node_test->type_annotation.index);
    hash = sxml_type_hash_number(hash, node_test->allows_nilled ? 1U : 0U);
    hash = sxml_type_hash_number(hash,
                                 node_test->is_schema_declaration ? 1U : 0U);
    return sxml_type_hash_number(hash, node_test->document_element_test.index);
}

static uint32_t sxml_type_hash_details(uint32_t hash,
                                       const sxml_type_description *description) {
    const sxml_type_description *item = description;
    switch (item->variety) {
    case SXML_TYPE_VARIETY_NODE_TEST:
        return sxml_type_hash_node_test(hash, &item->variety_details.node_test);
    case SXML_TYPE_VARIETY_JSON_NODE_TEST: {
        const sxml_json_node_test *test = &item->variety_details.json_node_test;
        hash = sxml_type_hash_number(hash, (uint64_t)test->key_constraint);
        hash = sxml_type_hash_number(hash, test->key_type.index);
        hash = sxml_type_hash_string(hash, test->key_lexical_form);
        hash = sxml_type_hash_number(hash, test->has_value_type ? 1U : 0U);
        return sxml_type_hash_sequence_type(hash, test->value_type);
    }
    case SXML_TYPE_VARIETY_FUNCTION: {
        const sxml_function_type *function = &item->variety_details.function_type;
        hash = sxml_type_hash_number(hash, function->is_any_function ? 1U : 0U);
        hash = sxml_type_hash_number(
            hash, function->parameter_types.sequence_type_count);
        for (size_t parameter_index = 0;
             parameter_index < function->parameter_types.sequence_type_count;
             parameter_index += 1) {
            hash = sxml_type_hash_sequence_type(
                hash, function->parameter_types.sequence_types[parameter_index]);
        }
        return sxml_type_hash_sequence_type(hash, function->result_type);
    }
    case SXML_TYPE_VARIETY_MAP:
        hash = sxml_type_hash_number(
            hash, item->variety_details.map_type.is_any_map ? 1U : 0U);
        hash = sxml_type_hash_number(
            hash, item->variety_details.map_type.key_type.index);
        return sxml_type_hash_sequence_type(
            hash, item->variety_details.map_type.value_type);
    case SXML_TYPE_VARIETY_ARRAY:
        hash = sxml_type_hash_number(
            hash, item->variety_details.array_type.is_any_array ? 1U : 0U);
        return sxml_type_hash_sequence_type(
            hash, item->variety_details.array_type.member_type);
    case SXML_TYPE_VARIETY_RECORD: {
        const sxml_record_type *record = &item->variety_details.record_type;
        hash = sxml_type_hash_number(hash, record->is_any_record ? 1U : 0U);
        hash = sxml_type_hash_number(hash,
                                     record->record_fields.record_field_count);
        for (size_t field_index = 0;
             field_index < record->record_fields.record_field_count;
             field_index += 1) {
            const sxml_record_field *field =
                &record->record_fields.record_fields[field_index];
            hash = sxml_type_hash_string(hash, field->field_name);
            hash = sxml_type_hash_sequence_type(hash, field->field_type);
        }
        return hash;
    }
    case SXML_TYPE_VARIETY_ENUMERATION: {
        sxml_byte_view_list values = item->variety_details.enumeration_values;
        hash = sxml_type_hash_number(hash, values.byte_view_count);
        for (size_t value_index = 0; value_index < values.byte_view_count;
             value_index += 1) {
            hash = sxml_type_hash_string(hash, values.byte_views[value_index]);
        }
        return hash;
    }
    case SXML_TYPE_VARIETY_CHOICE: {
        sxml_type_identifier_view members = item->variety_details.member_types;
        hash = sxml_type_hash_number(hash, members.type_identifier_count);
        for (size_t member_index = 0;
             member_index < members.type_identifier_count; member_index += 1) {
            hash = sxml_type_hash_number(
                hash, members.type_identifiers[member_index].index);
        }
        return hash;
    }
    case SXML_TYPE_VARIETY_COMPLEX:
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_ATOMIC:
    case SXML_TYPE_VARIETY_LIST:
    case SXML_TYPE_VARIETY_UNION:
    case SXML_TYPE_VARIETY_ANY_ITEM:
        return hash;
    }
    return hash;
}

static uint32_t sxml_type_hash_description(
    const sxml_type_description *description) {
    uint32_t hash = sxml_type_hash_number(SXML_TYPE_HASH_OFFSET_BASIS,
                                          (uint64_t)description->variety);
    return sxml_type_hash_details(hash, description);
}

/* ------------------------------------------------------------- Lifecycle */

static sxml_arena_configuration sxml_type_registry_arena_configuration(
    const sxml_type_registry_configuration *configuration) {
    bool can_grow = !sxml_allocator_is_null(configuration->allocator);
    return (sxml_arena_configuration){
        .block_allocator = configuration->allocator,
        .block_byte_count =
            can_grow ? SXML_TYPE_REGISTRY_ARENA_BLOCK_BYTE_COUNT : 0,
        .maximum_block_byte_count = 0,
        .initial_memory = {.bytes = NULL, .byte_count = 0},
    };
}

/* The requirements of the registry and the offset of its arena, placed
   right after it in the same memory. */
typedef struct sxml_type_registry_layout {
    sxml_memory_requirements requirements;
    size_t arena_offset;
} sxml_type_registry_layout;

static sxml_status sxml_type_registry_compute_layout(
    const sxml_type_registry_configuration *configuration,
    sxml_type_registry_layout *layout) {
    if (configuration->name_pool == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_arena_configuration arena_configuration =
        sxml_type_registry_arena_configuration(configuration);
    sxml_memory_requirements arena_requirements = {.byte_count = 0,
                                                   .alignment = 1};
    sxml_status status = sxml_arena_memory_requirements(&arena_configuration,
                                                        &arena_requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_memory_requirements requirements = {
        .byte_count = sizeof(struct sxml_type_registry),
        .alignment = alignof(struct sxml_type_registry),
    };
    size_t arena_offset = 0;
    status = sxml_memory_requirements_append(&requirements, arena_requirements,
                                             &arena_offset);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *layout = (sxml_type_registry_layout){.requirements = requirements,
                                          .arena_offset = arena_offset};
    return SXML_STATUS_OK;
}

sxml_status sxml_type_registry_memory_requirements(
    const sxml_type_registry_configuration *configuration,
    sxml_memory_requirements *requirements) {
    sxml_type_registry_layout layout = {
        .requirements = {.byte_count = 0, .alignment = 1}, .arena_offset = 0};
    sxml_status status = sxml_type_registry_compute_layout(configuration, &layout);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *requirements = layout.requirements;
    return SXML_STATUS_OK;
}

/* Interns the names of the built-in types (all in the XML Schema
   namespace, XDM 4.0 section 4.1.2) and indexes them. */
static sxml_status sxml_type_registry_intern_builtin_names(
    sxml_type_registry *registry) {
    for (uint32_t builtin_index = 1; builtin_index < SXML_TYPE_BUILTIN_COUNT;
         builtin_index += 1) {
        sxml_byte_view local_name =
            sxml_type_builtin_local_name((sxml_builtin_type)builtin_index);
        if (local_name.byte_count == 0) {
            continue;
        }
        sxml_name_identifier name = {.index = 0};
        sxml_status status = sxml_name_pool_intern_name(
            registry->configuration.name_pool,
            &(sxml_name_request){
                .namespace_uri = sxml_well_known_namespace_identifier(
                    SXML_NAMESPACE_XML_SCHEMA),
                .local_name = local_name},
            &name);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        registry->builtin_names[builtin_index] = name;
        sxml_type_hash_index builtin_index_table = {
            .slots = registry->builtin_name_slots,
            .slot_count = SXML_TYPE_BUILTIN_NAME_SLOT_COUNT};
        sxml_type_hash_index_insert(
            &builtin_index_table,
            (sxml_type_slot){.entry_number = builtin_index,
                             .hash = sxml_type_hash_of_name(name)});
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_type_registry_initialize(
    sxml_memory_view memory,
    const sxml_type_registry_configuration *configuration,
    sxml_type_registry **registry) {
    sxml_type_registry_layout layout = {
        .requirements = {.byte_count = 0, .alignment = 1}, .arena_offset = 0};
    sxml_status status = sxml_type_registry_compute_layout(configuration, &layout);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_memory_view_check_requirements(memory, layout.requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* memory_view_check_requirements has checked size and alignment. */
    void *placement = memory.bytes;
    sxml_type_registry *new_registry = placement;
    memset(new_registry, 0, sizeof(*new_registry));
    new_registry->configuration = *configuration;
    new_registry->owning_allocator =
        (sxml_allocator){.state = NULL, .operations = NULL};
    sxml_arena_configuration arena_configuration =
        sxml_type_registry_arena_configuration(configuration);
    /* The layout reserved the arena's bytes within memory. */
    status = sxml_arena_initialize(
        (sxml_memory_view){
            .bytes = memory.bytes + layout.arena_offset,
            .byte_count = layout.requirements.byte_count - layout.arena_offset},
        &arena_configuration, &new_registry->arena);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_type_registry_intern_builtin_names(new_registry);
    if (status != SXML_STATUS_OK) {
        sxml_arena_finalize(new_registry->arena);
        return status;
    }
    *registry = new_registry;
    return SXML_STATUS_OK;
}

void sxml_type_registry_finalize(sxml_type_registry *registry) {
    if (registry == NULL) {
        return;
    }
    sxml_type_registry_release(registry, registry->entries);
    sxml_type_registry_release(registry, registry->structure_index.slots);
    sxml_type_registry_release(registry, registry->name_index.slots);
    sxml_arena_finalize(registry->arena);
    registry->entries = NULL;
    registry->entry_count = 0;
    registry->entry_capacity = 0;
    registry->structure_index =
        (sxml_type_hash_index){.slots = NULL, .slot_count = 0};
    registry->structural_count = 0;
    registry->name_index =
        (sxml_type_hash_index){.slots = NULL, .slot_count = 0};
    registry->named_count = 0;
}

sxml_status sxml_type_registry_create(
    sxml_allocator allocator,
    const sxml_type_registry_configuration *configuration,
    sxml_type_registry **registry) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_type_registry_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = requirements.byte_count,
                                  .alignment = requirements.alignment},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_type_registry *new_registry = NULL;
    status = sxml_type_registry_initialize(
        (sxml_memory_view){.bytes = memory,
                           .byte_count = requirements.byte_count},
        configuration, &new_registry);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_registry->owning_allocator = allocator;
    *registry = new_registry;
    return SXML_STATUS_OK;
}

void sxml_type_registry_destroy(sxml_type_registry *registry) {
    if (registry == NULL) {
        return;
    }
    /* Finalizing may not touch the registry afterwards. */
    sxml_allocator owning_allocator = registry->owning_allocator;
    sxml_type_registry_finalize(registry);
    sxml_allocator_release(owning_allocator, registry);
}

/* ------------------------------------------------------------ Description */

sxml_status sxml_type_registry_describe_type(
    const sxml_type_registry *registry, sxml_type_identifier type,
    sxml_type_description *description) {
    const sxml_type_description *found = sxml_type_registry_lookup(registry, type);
    if (found == NULL) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    *description = *found;
    if (sxml_type_is_builtin(type)) {
        description->type_name = registry->builtin_names[type.index];
    }
    return SXML_STATUS_OK;
}

/* Finds a registered type named name in the name index. */
static bool sxml_type_registry_find_registered_name(
    const sxml_type_registry *registry, sxml_name_identifier name,
    size_t *entry_position) {
    const sxml_type_hash_index *index = &registry->name_index;
    if (index->slot_count == 0) {
        return false;
    }
    uint32_t hash = sxml_type_hash_of_name(name);
    size_t slot_index = sxml_type_first_slot(index->slot_count, hash);
    while (index->slots[slot_index].entry_number != 0) {
        sxml_type_slot slot = index->slots[slot_index];
        size_t position = (size_t)slot.entry_number - 1;
        if (slot.hash == hash &&
            registry->entries[position].description.type_name.index ==
                name.index) {
            *entry_position = position;
            return true;
        }
        slot_index = sxml_type_next_slot(index->slot_count, slot_index);
    }
    return false;
}

bool sxml_type_registry_find_type(const sxml_type_registry *registry,
                                  sxml_name_identifier type_name,
                                  sxml_type_identifier *type) {
    if (type_name.index == 0) {
        return false;
    }
    uint32_t hash = sxml_type_hash_of_name(type_name);
    size_t slot_index =
        sxml_type_first_slot(SXML_TYPE_BUILTIN_NAME_SLOT_COUNT, hash);
    while (registry->builtin_name_slots[slot_index].entry_number != 0) {
        sxml_type_slot slot = registry->builtin_name_slots[slot_index];
        if (slot.hash == hash &&
            registry->builtin_names[slot.entry_number].index == type_name.index) {
            *type = (sxml_type_identifier){.index = slot.entry_number};
            return true;
        }
        slot_index =
            sxml_type_next_slot(SXML_TYPE_BUILTIN_NAME_SLOT_COUNT, slot_index);
    }
    size_t position = 0;
    if (!sxml_type_registry_find_registered_name(registry, type_name,
                                                 &position)) {
        return false;
    }
    *type = (sxml_type_identifier){
        .index = (uint32_t)(position + SXML_TYPE_FIRST_REGISTERED)};
    return true;
}

/* -------------------------------------------------------- String indexes */

/* A string to find in the string index of a record or enumeration type. */
typedef struct sxml_type_string_query {
    const sxml_type_entry *entry;
    sxml_byte_view text;
} sxml_type_string_query;

/* The string at position in the strings of a record or enumeration type. */
static sxml_byte_view sxml_type_entry_string_at(const sxml_type_entry *entry,
                                                size_t position) {
    if (entry->description.variety == SXML_TYPE_VARIETY_RECORD) {
        return entry->description.variety_details.record_type.record_fields
            .record_fields[position]
            .field_name;
    }
    return entry->description.variety_details.enumeration_values
        .byte_views[position];
}

static size_t sxml_type_entry_string_count(const sxml_type_entry *entry) {
    if (entry->description.variety == SXML_TYPE_VARIETY_RECORD) {
        return entry->description.variety_details.record_type.record_fields
            .record_field_count;
    }
    return entry->description.variety_details.enumeration_values
        .byte_view_count;
}

static bool sxml_type_entry_find_string(sxml_type_string_query query,
                                        size_t *position) {
    const sxml_type_hash_index *index = &query.entry->string_index;
    if (index->slot_count == 0) {
        return false;
    }
    uint32_t hash = sxml_type_hash_of_string(query.text);
    size_t slot_index = sxml_type_first_slot(index->slot_count, hash);
    while (index->slots[slot_index].entry_number != 0) {
        sxml_type_slot slot = index->slots[slot_index];
        size_t candidate = (size_t)slot.entry_number - 1;
        if (slot.hash == hash &&
            sxml_byte_view_equals(
                sxml_type_entry_string_at(query.entry, candidate),
                query.text)) {
            *position = candidate;
            return true;
        }
        slot_index = sxml_type_next_slot(index->slot_count, slot_index);
    }
    return false;
}

/*
 * Builds the string index of entry (whose strings are already copied) in
 * the arena. Returns SXML_STATUS_INVALID_ARGUMENT when two strings are
 * equal; the index is then left unset.
 */
static sxml_status sxml_type_registry_index_strings(sxml_type_registry *registry,
                                                    sxml_type_entry *entry) {
    size_t string_count = sxml_type_entry_string_count(entry);
    if (string_count == 0) {
        entry->string_index = (sxml_type_hash_index){.slots = NULL,
                                                     .slot_count = 0};
        return SXML_STATUS_OK;
    }
    /* At most half full; string_count is bounded by memory, so doubling
       stops long before SIZE_MAX. */
    size_t slot_count = 4;
    while (slot_count < string_count * 2) {
        slot_count *= 2;
    }
    void *memory = NULL;
    sxml_status status = sxml_type_registry_allocate_array(
        registry,
        (sxml_type_array_request){.element_count = slot_count,
                                  .element_byte_count = sizeof(sxml_type_slot),
                                  .alignment = alignof(sxml_type_slot)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_type_hash_index index = {.slots = memory, .slot_count = slot_count};
    memset(index.slots, 0, slot_count * sizeof(sxml_type_slot));
    entry->string_index = index;
    for (size_t position = 0; position < string_count; position += 1) {
        sxml_byte_view text = sxml_type_entry_string_at(entry, position);
        size_t existing = 0;
        if (sxml_type_entry_find_string(
                (sxml_type_string_query){.entry = entry, .text = text},
                &existing)) {
            entry->string_index =
                (sxml_type_hash_index){.slots = NULL, .slot_count = 0};
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        sxml_type_hash_index_insert(
            &entry->string_index,
            (sxml_type_slot){.entry_number = (uint32_t)(position + 1),
                             .hash = sxml_type_hash_of_string(text)});
    }
    return SXML_STATUS_OK;
}

bool sxml_type_registry_find_record_field(const sxml_type_registry *registry,
                                          sxml_record_field_query query,
                                          size_t *field_index) {
    const sxml_type_entry *entry =
        sxml_type_registry_entry_of(registry, query.record_type);
    if (entry == NULL ||
        entry->description.variety != SXML_TYPE_VARIETY_RECORD) {
        return false;
    }
    return sxml_type_entry_find_string(
        (sxml_type_string_query){.entry = entry, .text = query.field_name},
        field_index);
}

/* ------------------------------------------------------------ Derivation */

/*
 * XPath 4.0 "derives from" over schema types (XSD 1.1 Part 1, sections
 * 3.4.6.5 and 3.16.6.3): derived is base, or its base type derives from
 * base, or base is a union with a member that derived derives from.
 *
 * The members of union base types are tried depth first on an explicit
 * stack of derivation frames (D71); a member nested
 * SXML_TYPE_DERIVATION_MAXIMUM_DEPTH unions deep is not tried.
 */

/* What following the base types of the derived type found. */
typedef enum sxml_type_base_chain_outcome {
    /* The base type is on the chain. */
    SXML_TYPE_BASE_CHAIN_FOUND,
    /* It is not; the members of a union base type may still be. */
    SXML_TYPE_BASE_CHAIN_ABSENT,
    /* A type on the chain is not issued: this base type is not reached. */
    SXML_TYPE_BASE_CHAIN_BROKEN
} sxml_type_base_chain_outcome;

static sxml_type_base_chain_outcome sxml_type_follow_base_chain(
    const sxml_type_registry *registry, sxml_type_pair pair) {
    sxml_type_identifier current = pair.derived_type;
    for (uint32_t base_step = 0;
         base_step < SXML_TYPE_DERIVATION_MAXIMUM_DEPTH && current.index != 0;
         base_step += 1) {
        if (sxml_type_identifier_equals(current, pair.base_type)) {
            return SXML_TYPE_BASE_CHAIN_FOUND;
        }
        const sxml_type_description *description =
            sxml_type_registry_lookup(registry, current);
        if (description == NULL) {
            return SXML_TYPE_BASE_CHAIN_BROKEN;
        }
        current = description->base_type;
    }
    return SXML_TYPE_BASE_CHAIN_ABSENT;
}

/* A union base type whose members are being tried, and the next one. */
typedef struct sxml_type_derivation_frame {
    sxml_type_identifier union_type;
    size_t next_member_index;
} sxml_type_derivation_frame;

static bool sxml_type_registry_derives(const sxml_type_registry *registry,
                                       sxml_type_pair pair) {
    sxml_type_derivation_frame frames[SXML_TYPE_DERIVATION_MAXIMUM_DEPTH];
    /* The base type tried next is nested frame_count unions deep. */
    size_t frame_count = 0;
    sxml_type_identifier base = pair.base_type;
    for (;;) {
        sxml_type_base_chain_outcome outcome = sxml_type_follow_base_chain(
            registry, (sxml_type_pair){.derived_type = pair.derived_type,
                                       .base_type = base});
        if (outcome == SXML_TYPE_BASE_CHAIN_FOUND) {
            return true;
        }
        const sxml_type_description *base_description =
            sxml_type_registry_lookup(registry, base);
        if (outcome == SXML_TYPE_BASE_CHAIN_ABSENT &&
            base_description != NULL &&
            base_description->variety == SXML_TYPE_VARIETY_UNION) {
            /* frame_count is below the maximum depth here: base was tried
               at that depth. */
            frames[frame_count] = (sxml_type_derivation_frame){
                .union_type = base, .next_member_index = 0};
            frame_count += 1;
        }
        bool has_next_base = false;
        while (!has_next_base && frame_count != 0) {
            sxml_type_derivation_frame *top = &frames[frame_count - 1];
            sxml_type_identifier_view members =
                sxml_type_registry_lookup(registry, top->union_type)
                    ->variety_details.member_types;
            if (frame_count < SXML_TYPE_DERIVATION_MAXIMUM_DEPTH &&
                top->next_member_index < members.type_identifier_count) {
                base = members.type_identifiers[top->next_member_index];
                top->next_member_index += 1;
                has_next_base = true;
            } else {
                frame_count -= 1;
            }
        }
        if (!has_next_base) {
            return false;
        }
    }
}

bool sxml_type_registry_is_derived_from(const sxml_type_registry *registry,
                                        sxml_type_pair pair) {
    if (!sxml_type_registry_issued(registry, pair.derived_type) ||
        !sxml_type_registry_issued(registry, pair.base_type)) {
        return false;
    }
    return sxml_type_registry_derives(registry, pair);
}

/* ------------------------------------------------------------- Subtyping */

/*
 * A subtype question is an and/or tree of smaller questions, subtype
 * goals: the rules of XPath 4.0, section 3.3.2 reduce a goal to an answer,
 * to another goal, or to a conjunction or disjunction of sub-goals, which
 * are produced one at a time by their position. The tree is evaluated
 * depth first and left to right, with short-circuiting, on an explicit
 * stack of subtype frames (D71): a frame holds a conjunction or
 * disjunction waiting for the answer of its current sub-goal. The last
 * sub-goal of a frame replaces the frame's goal, since its answer is the
 * frame's answer; so array(array(...)) and the like take one frame however
 * deep they nest.
 *
 * Budget (D58): item goals nest at most SXML_TYPE_SUBTYPE_MAXIMUM_DEPTH
 * deep and at most SXML_TYPE_SUBTYPE_MAXIMUM_STEP_COUNT are reduced; an
 * item goal beyond either is false, and a frame that would need more than
 * SXML_TYPE_SUBTYPE_MAXIMUM_FRAME_COUNT frames is false. All rules are
 * monotone, so running out answers false, never a wrong true.
 */

/* enum(value) ⊆ base (XPath 4.0, sections 3.3.2.2 and 3.3.2.4). */
typedef struct sxml_type_value_question {
    sxml_byte_view value;
    sxml_type_identifier base_type;
} sxml_type_value_question;

/*
 * document-node(element_test) ⊆ base (XPath 4.0, section 3.3.2.5.2), for an
 * element test of SXML_TYPE_NONE (document-node()) or an element test that
 * may be a choice: document-node(element(a|b)) is a subtype when
 * document-node(element(a)) and document-node(element(b)) both are.
 */
typedef struct sxml_type_document_question {
    sxml_type_identifier element_test;
    sxml_type_identifier base_type;
} sxml_type_document_question;

/* Every member of members ⊆ base. */
typedef struct sxml_type_members_question {
    sxml_type_identifier_view members;
    sxml_type_identifier base_type;
} sxml_type_members_question;

/* Every field type of a record type that is not record(*) ⊆
   base_value_type: what a record contributes as the value type of
   map(xs:string, V) (XPath 4.0, section 3.3.2.9). */
typedef struct sxml_type_record_values {
    const sxml_record_type *record_type;
    sxml_sequence_type base_value_type;
} sxml_type_record_values;

typedef struct sxml_type_description_pair {
    const sxml_type_description *derived;
    const sxml_type_description *base;
} sxml_type_description_pair;

/* The kinds of subtype goal, and the member of question each one uses. */
typedef enum sxml_type_subtype_goal_kind {
    /* Answered: truth. */
    SXML_TYPE_GOAL_TRUTH,
    /* Reduced by the rules. pair: derived ⊆ base, item types. */
    SXML_TYPE_GOAL_ITEM,
    /* sequence_pair: derived ⊆ base, sequence types (3.3.1). */
    SXML_TYPE_GOAL_SEQUENCE,
    /* value. */
    SXML_TYPE_GOAL_ENUMERATION_VALUE,
    /* document. */
    SXML_TYPE_GOAL_DOCUMENT,
    /* Conjunctions. members: ITEM(member, base) for each member. */
    SXML_TYPE_GOAL_EVERY_MEMBER,
    /* pair, derived an enumeration type: ENUMERATION_VALUE(value, base)
       for each value. */
    SXML_TYPE_GOAL_EVERY_ENUMERATION_VALUE,
    /* document, element_test a choice: DOCUMENT(member, base) for each
       member. */
    SXML_TYPE_GOAL_EVERY_DOCUMENT_ELEMENT,
    /* record_values: SEQUENCE(field type, base value type) per field. */
    SXML_TYPE_GOAL_RECORD_VALUES,
    /* pair, two record types with equally many fields, the base
       structural: each field is in base with a supertype. */
    SXML_TYPE_GOAL_RECORD_FIELDS,
    /* descriptions, function types of equal arity: result covariant,
       parameters contravariant. */
    SXML_TYPE_GOAL_FUNCTION_SIGNATURE,
    /* descriptions, a map, array or record type as a one-parameter
       function type. */
    SXML_TYPE_GOAL_MAP_AS_FUNCTION,
    SXML_TYPE_GOAL_ARRAY_AS_FUNCTION,
    SXML_TYPE_GOAL_RECORD_AS_FUNCTION,
    /* descriptions, a map or record type ⊆ a map type with key and value
       types. */
    SXML_TYPE_GOAL_MAP_AS_MAP,
    SXML_TYPE_GOAL_RECORD_AS_MAP,
    /* Disjunctions. pair, base a choice: ITEM(derived, member) for each
       member, then EVERY_MEMBER of the abstract expansion of derived. */
    SXML_TYPE_GOAL_SOME_MEMBER,
    /* value, base a choice: ENUMERATION_VALUE(value, member) per member. */
    SXML_TYPE_GOAL_SOME_ENUMERATION_MEMBER,
    /* document, base a choice: DOCUMENT(element test, member) per
       member. */
    SXML_TYPE_GOAL_SOME_DOCUMENT_MEMBER
} sxml_type_subtype_goal_kind;

typedef struct sxml_type_subtype_goal {
    sxml_type_subtype_goal_kind kind;
    union {
        bool truth;
        sxml_type_pair pair;
        sxml_sequence_type_pair sequence_pair;
        sxml_type_value_question value;
        sxml_type_document_question document;
        sxml_type_members_question members;
        sxml_type_record_values record_values;
        sxml_type_description_pair descriptions;
    } question;
} sxml_type_subtype_goal;

/* A conjunction or disjunction waiting for the answer of a sub-goal. */
typedef struct sxml_type_subtype_frame {
    sxml_type_subtype_goal goal;
    /* Position of the sub-goal evaluated next. */
    size_t next_goal_index;
    /* Item goals this frame has entered; they count towards the depth
       until the frame is answered. */
    uint32_t item_depth;
} sxml_type_subtype_frame;

/* The state of one subtype question (D58). */
typedef struct sxml_type_subtype_search {
    const sxml_type_registry *registry;
    /* Item goals entered and not yet answered. */
    uint32_t depth;
    uint32_t remaining_step_count;
    size_t frame_count;
    sxml_type_subtype_frame frames[SXML_TYPE_SUBTYPE_MAXIMUM_FRAME_COUNT];
} sxml_type_subtype_search;

static sxml_type_subtype_goal sxml_type_true_goal(void) {
    return (sxml_type_subtype_goal){.kind = SXML_TYPE_GOAL_TRUTH,
                                    .question = {.truth = true}};
}

static sxml_type_subtype_goal sxml_type_false_goal(void) {
    return (sxml_type_subtype_goal){.kind = SXML_TYPE_GOAL_TRUTH,
                                    .question = {.truth = false}};
}

static sxml_type_subtype_goal sxml_type_item_goal(sxml_type_identifier derived,
                                                  sxml_type_identifier base) {
    return (sxml_type_subtype_goal){
        .kind = SXML_TYPE_GOAL_ITEM,
        .question = {.pair = {.derived_type = derived, .base_type = base}}};
}

static sxml_type_subtype_goal sxml_type_sequence_goal(
    sxml_sequence_type derived, sxml_sequence_type base) {
    return (sxml_type_subtype_goal){
        .kind = SXML_TYPE_GOAL_SEQUENCE,
        .question = {.sequence_pair = {.derived_type = derived,
                                       .base_type = base}}};
}

/* A goal over two item types. */
static sxml_type_subtype_goal sxml_type_pair_goal(
    sxml_type_subtype_goal_kind kind, sxml_type_pair pair) {
    return (sxml_type_subtype_goal){.kind = kind, .question = {.pair = pair}};
}

/* A goal over two descriptions. */
static sxml_type_subtype_goal sxml_type_descriptions_goal(
    sxml_type_subtype_goal_kind kind, sxml_type_description_pair pair) {
    return (sxml_type_subtype_goal){.kind = kind,
                                    .question = {.descriptions = pair}};
}

static sxml_type_subtype_goal sxml_type_members_goal(
    sxml_type_members_question question) {
    return (sxml_type_subtype_goal){.kind = SXML_TYPE_GOAL_EVERY_MEMBER,
                                    .question = {.members = question}};
}

static sxml_type_subtype_goal sxml_type_value_goal(
    sxml_type_subtype_goal_kind kind, sxml_type_value_question question) {
    return (sxml_type_subtype_goal){.kind = kind,
                                    .question = {.value = question}};
}

static sxml_type_subtype_goal sxml_type_document_goal(
    sxml_type_subtype_goal_kind kind, sxml_type_document_question question) {
    return (sxml_type_subtype_goal){.kind = kind,
                                    .question = {.document = question}};
}

static const sxml_sequence_type sxml_type_any_sequence = {
    .item_type = {.index = SXML_TYPE_ITEM},
    .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};

/* Every value of the record ⊆ base_value_type (XPath 4.0, section
   3.3.2.9); record(*) has values of item()*. */
static sxml_type_subtype_goal sxml_type_record_values_goal(
    const sxml_record_type *record, sxml_sequence_type base_value_type) {
    if (record->is_any_record) {
        return sxml_type_sequence_goal(sxml_type_any_sequence,
                                       base_value_type);
    }
    return (sxml_type_subtype_goal){
        .kind = SXML_TYPE_GOAL_RECORD_VALUES,
        .question = {.record_values = {.record_type = record,
                                       .base_value_type = base_value_type}}};
}

/* Whether a sequence type allows the empty sequence, and whether it is
   one of the void types xs:error and xs:error+ (XPath 4.0, 3.3.1). */
static bool sxml_type_sequence_allows_empty(sxml_sequence_type sequence_type) {
    return ((uint32_t)sequence_type.occurrence &
            (uint32_t)SXML_OCCURRENCE_ZERO) != 0;
}

static bool sxml_type_sequence_is_void(sxml_sequence_type sequence_type) {
    return sxml_type_is(sequence_type.item_type, SXML_TYPE_XS_ERROR) &&
           !sxml_type_sequence_allows_empty(sequence_type);
}

static bool sxml_type_sequence_is_empty(sxml_sequence_type sequence_type) {
    return sequence_type.occurrence == SXML_OCCURRENCE_ZERO ||
           (sxml_type_is(sequence_type.item_type, SXML_TYPE_XS_ERROR) &&
            sxml_type_sequence_allows_empty(sequence_type));
}

/* XPath 4.0, section 3.3.1 "Subtypes of Sequence Types", by category. */
static sxml_type_subtype_goal sxml_type_sequence_rules(
    sxml_sequence_type_pair pair) {
    sxml_sequence_type derived = pair.derived_type;
    sxml_sequence_type base = pair.base_type;
    if (sxml_type_sequence_is_void(derived)) {
        return sxml_type_true_goal();
    }
    if (sxml_type_sequence_is_void(base)) {
        return sxml_type_false_goal();
    }
    if (sxml_type_sequence_is_empty(derived)) {
        return sxml_type_sequence_allows_empty(base) ? sxml_type_true_goal()
                                                     : sxml_type_false_goal();
    }
    if (sxml_type_sequence_is_empty(base)) {
        return sxml_type_false_goal();
    }
    if (((uint32_t)derived.occurrence & ~(uint32_t)base.occurrence) != 0) {
        return sxml_type_false_goal();
    }
    return sxml_type_item_goal(derived.item_type, base.item_type);
}

/* -------------------------------------------- Subtyping: enumerations */

static sxml_type_subtype_goal sxml_type_enumeration_value_rules(
    const sxml_type_subtype_search *search, sxml_type_value_question question) {
    const sxml_type_description *base =
        sxml_type_registry_lookup(search->registry, question.base_type);
    if (base == NULL) {
        return sxml_type_false_goal();
    }
    if (base->variety == SXML_TYPE_VARIETY_ENUMERATION) {
        size_t position = 0;
        bool is_member = sxml_type_entry_find_string(
            (sxml_type_string_query){
                .entry = sxml_type_registry_entry_of(search->registry,
                                                     question.base_type),
                .text = question.value},
            &position);
        return is_member ? sxml_type_true_goal() : sxml_type_false_goal();
    }
    if (base->variety == SXML_TYPE_VARIETY_CHOICE) {
        return sxml_type_value_goal(SXML_TYPE_GOAL_SOME_ENUMERATION_MEMBER,
                                    question);
    }
    /* A singleton enumeration is a subtype of xs:string, and through the
       transitivity rule of everything xs:string is a subtype of. */
    return sxml_type_item_goal(sxml_type_of_builtin(SXML_TYPE_XS_STRING),
                               question.base_type);
}

/* ---------------------------------------------------- Subtyping: nodes */

/* "wildcard-matches" (XPath 4.0, section 3.3.2.5.3) for single tests;
   NameTestUnions are choices of node tests. */
typedef struct sxml_type_name_test_pair {
    const sxml_name_test *derived;
    const sxml_name_test *base;
} sxml_type_name_test_pair;

static bool sxml_type_name_test_matches(const sxml_type_registry *registry,
                                        sxml_type_name_test_pair pair) {
    const sxml_name_test *derived = pair.derived;
    const sxml_name_test *base = pair.base;
    if (base->name_test_kind == SXML_NAME_TEST_KIND_ANY_NAME) {
        return true;
    }
    if (sxml_type_name_tests_equal(derived, base)) {
        return true;
    }
    if (derived->name_test_kind != SXML_NAME_TEST_KIND_EXPANDED_NAME) {
        return false;
    }
    sxml_name_parts parts = {.namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    if (sxml_name_pool_get_name_parts(registry->configuration.name_pool,
                                      derived->expanded_name,
                                      &parts) != SXML_STATUS_OK) {
        return false;
    }
    switch (base->name_test_kind) {
    case SXML_NAME_TEST_KIND_NAMESPACE_WILDCARD:
        return parts.namespace_uri.index == base->namespace_uri.index;
    case SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD:
        return sxml_byte_view_equals(parts.local_name, base->local_name);
    case SXML_NAME_TEST_KIND_ANY_NAME:
    case SXML_NAME_TEST_KIND_EXPANDED_NAME:
        return false;
    }
    return false;
}

typedef struct sxml_type_node_test_pair {
    const sxml_node_test *derived;
    const sxml_node_test *base;
} sxml_type_node_test_pair;

/* XPath 4.0, section 3.3.2.5.3 "Subtyping Nodes: Elements". */
static bool sxml_type_element_subtype(const sxml_type_registry *registry,
                                      sxml_type_node_test_pair pair) {
    const sxml_node_test *derived = pair.derived;
    const sxml_node_test *base = pair.base;
    bool base_is_any_element =
        base->name_test.name_test_kind == SXML_NAME_TEST_KIND_ANY_NAME &&
        base->type_annotation.index == SXML_TYPE_NONE &&
        !base->is_schema_declaration;
    if (derived->is_schema_declaration || base->is_schema_declaration) {
        /* Without element declarations (Phase 5) only these hold: a
           schema-element test is an element() and equal to itself. */
        return derived->is_schema_declaration && base_is_any_element;
    }
    if (!sxml_type_name_test_matches(
            registry, (sxml_type_name_test_pair){.derived = &derived->name_test,
                                                 .base = &base->name_test})) {
        return false;
    }
    if (base->type_annotation.index == SXML_TYPE_NONE ||
        (sxml_type_is(base->type_annotation, SXML_TYPE_XS_ANY_TYPE) &&
         base->allows_nilled)) {
        return true;
    }
    /* element(N) is element(N, xs:anyType?). */
    bool derived_has_type = derived->type_annotation.index != SXML_TYPE_NONE;
    sxml_type_identifier derived_type =
        derived_has_type ? derived->type_annotation
                         : sxml_type_of_builtin(SXML_TYPE_XS_ANY_TYPE);
    bool derived_allows_nilled = !derived_has_type || derived->allows_nilled;
    if (derived_allows_nilled && !base->allows_nilled) {
        return false;
    }
    return sxml_type_registry_derives(
        registry, (sxml_type_pair){.derived_type = derived_type,
                                   .base_type = base->type_annotation});
}

/* XPath 4.0, section 3.3.2.5.4 "Subtyping Nodes: Attributes". */
static bool sxml_type_attribute_subtype(const sxml_type_registry *registry,
                                        sxml_type_node_test_pair pair) {
    const sxml_node_test *derived = pair.derived;
    const sxml_node_test *base = pair.base;
    if (base->is_schema_declaration) {
        return false;
    }
    if (derived->is_schema_declaration) {
        /* Without attribute declarations (Phase 5): attribute(),
           attribute(*) and attribute(N) for the same name. */
        if (base->type_annotation.index != SXML_TYPE_NONE) {
            return false;
        }
        return base->name_test.name_test_kind ==
                   SXML_NAME_TEST_KIND_ANY_NAME ||
               (base->name_test.name_test_kind ==
                    SXML_NAME_TEST_KIND_EXPANDED_NAME &&
                base->name_test.expanded_name.index ==
                    derived->name_test.expanded_name.index);
    }
    if (!sxml_type_name_test_matches(
            registry, (sxml_type_name_test_pair){.derived = &derived->name_test,
                                                 .base = &base->name_test})) {
        return false;
    }
    if (base->type_annotation.index == SXML_TYPE_NONE ||
        sxml_type_is(base->type_annotation, SXML_TYPE_XS_ANY_ATOMIC_TYPE)) {
        return true;
    }
    if (derived->type_annotation.index == SXML_TYPE_NONE) {
        return false;
    }
    return sxml_type_registry_derives(
        registry, (sxml_type_pair){.derived_type = derived->type_annotation,
                                   .base_type = base->type_annotation});
}

static sxml_type_subtype_goal sxml_type_document_rules(
    const sxml_type_subtype_search *search,
    sxml_type_document_question question) {
    const sxml_type_registry *registry = search->registry;
    if (question.element_test.index != SXML_TYPE_NONE) {
        const sxml_type_description *element_test =
            sxml_type_registry_lookup(registry, question.element_test);
        if (element_test == NULL) {
            return sxml_type_false_goal();
        }
        if (element_test->variety == SXML_TYPE_VARIETY_CHOICE) {
            return sxml_type_document_goal(
                SXML_TYPE_GOAL_EVERY_DOCUMENT_ELEMENT, question);
        }
    }
    sxml_type_identifier base_type = question.base_type;
    if (sxml_type_is(base_type, SXML_TYPE_ITEM) ||
        sxml_type_is(base_type, SXML_TYPE_GENERIC_NODE) ||
        sxml_type_is(base_type, SXML_TYPE_NODE)) {
        return sxml_type_true_goal();
    }
    const sxml_type_description *base = sxml_type_registry_lookup(registry,
                                                                  base_type);
    if (base == NULL) {
        return sxml_type_false_goal();
    }
    if (base->variety == SXML_TYPE_VARIETY_CHOICE) {
        return sxml_type_document_goal(SXML_TYPE_GOAL_SOME_DOCUMENT_MEMBER,
                                       question);
    }
    if (base->variety != SXML_TYPE_VARIETY_NODE_TEST ||
        base->variety_details.node_test.node_kind != SXML_NODE_KIND_DOCUMENT) {
        return sxml_type_false_goal();
    }
    sxml_type_identifier base_element_test =
        base->variety_details.node_test.document_element_test;
    if (base_element_test.index == SXML_TYPE_NONE) {
        return sxml_type_true_goal();
    }
    if (question.element_test.index == SXML_TYPE_NONE) {
        return sxml_type_false_goal();
    }
    return sxml_type_item_goal(question.element_test, base_element_test);
}

/* XPath 4.0, section 3.3.2.5 for a node test other than a document test;
   base is not a choice. */
static bool sxml_type_node_subtype(const sxml_type_registry *registry,
                                   sxml_type_pair pair) {
    if (sxml_type_is(pair.base_type, SXML_TYPE_GENERIC_NODE)) {
        return true;
    }
    if (sxml_type_is(pair.derived_type, SXML_TYPE_GENERIC_NODE)) {
        return false;
    }
    if (sxml_type_is(pair.base_type, SXML_TYPE_NODE)) {
        return true;
    }
    const sxml_type_description *derived =
        sxml_type_registry_lookup(registry, pair.derived_type);
    const sxml_type_description *base =
        sxml_type_registry_lookup(registry, pair.base_type);
    if (base->variety != SXML_TYPE_VARIETY_NODE_TEST) {
        return false;
    }
    const sxml_node_test *derived_test = &derived->variety_details.node_test;
    const sxml_node_test *base_test = &base->variety_details.node_test;
    if (derived_test->node_kind != base_test->node_kind) {
        return false;
    }
    sxml_type_node_test_pair tests = {.derived = derived_test,
                                      .base = base_test};
    switch (derived_test->node_kind) {
    case SXML_NODE_KIND_ELEMENT:
        return sxml_type_element_subtype(registry, tests);
    case SXML_NODE_KIND_ATTRIBUTE:
        return sxml_type_attribute_subtype(registry, tests);
    case SXML_NODE_KIND_PROCESSING_INSTRUCTION:
        return base_test->name_test.name_test_kind ==
                   SXML_NAME_TEST_KIND_ANY_NAME ||
               sxml_type_name_tests_equal(&derived_test->name_test,
                                          &base_test->name_test);
    case SXML_NODE_KIND_TEXT:
    case SXML_NODE_KIND_COMMENT:
    case SXML_NODE_KIND_NAMESPACE:
        return true;
    case SXML_NODE_KIND_DOCUMENT:
    case SXML_NODE_KIND_ANY:
        return false;
    }
    return false;
}

/* XPath 4.0, section 3.3.2.5.5 "Subtyping JNodes"; base is not a choice. */
static sxml_type_subtype_goal sxml_type_json_node_rules(
    const sxml_type_subtype_search *search, sxml_type_pair pair) {
    if (sxml_type_is(pair.base_type, SXML_TYPE_GENERIC_NODE)) {
        return sxml_type_true_goal();
    }
    const sxml_type_description *derived =
        sxml_type_registry_lookup(search->registry, pair.derived_type);
    const sxml_type_description *base =
        sxml_type_registry_lookup(search->registry, pair.base_type);
    if (base->variety != SXML_TYPE_VARIETY_JSON_NODE_TEST) {
        return sxml_type_false_goal();
    }
    const sxml_json_node_test *derived_test =
        &derived->variety_details.json_node_test;
    const sxml_json_node_test *base_test = &base->variety_details.json_node_test;
    switch (base_test->key_constraint) {
    case SXML_JSON_KEY_CONSTRAINT_ANY:
        break;
    case SXML_JSON_KEY_CONSTRAINT_ROOT:
        if (derived_test->key_constraint != SXML_JSON_KEY_CONSTRAINT_ROOT) {
            return sxml_type_false_goal();
        }
        break;
    case SXML_JSON_KEY_CONSTRAINT_CONSTANT:
        if (derived_test->key_constraint != SXML_JSON_KEY_CONSTRAINT_CONSTANT ||
            !sxml_type_identifier_equals(derived_test->key_type,
                                         base_test->key_type) ||
            !sxml_byte_view_equals(derived_test->key_lexical_form,
                                   base_test->key_lexical_form)) {
            return sxml_type_false_goal();
        }
        break;
    }
    if (!base_test->has_value_type) {
        return sxml_type_true_goal();
    }
    /* jnode(C) constrains no value: its value type is item()*. */
    sxml_sequence_type derived_value = derived_test->has_value_type
                                           ? derived_test->value_type
                                           : sxml_type_any_sequence;
    return sxml_type_sequence_goal(derived_value, base_test->value_type);
}

/* ---------------------------------------- Subtyping: function items */

/* The same sequence type, also allowing the empty sequence. */
static sxml_sequence_type sxml_type_or_empty(sxml_sequence_type sequence_type) {
    sequence_type.occurrence = (sxml_occurrence)((uint32_t)sequence_type.occurrence |
                                                 (uint32_t)SXML_OCCURRENCE_ZERO);
    return sequence_type;
}

static sxml_sequence_type sxml_type_exactly_one(sxml_builtin_type builtin) {
    return (sxml_sequence_type){.item_type = sxml_type_of_builtin(builtin),
                                .occurrence = SXML_OCCURRENCE_EXACTLY_ONE};
}

/*
 * A function, map, array or record type ⊆ function(b1, ..., bN) as RB
 * (XPath 4.0, sections 3.3.2.6 to 3.3.2.9 with the transitivity rule):
 * maps and records are function(xs:anyAtomicType) as V?, arrays are
 * function(xs:integer) as X. The sub-goals are in sxml_type_next_goal.
 */
static sxml_type_subtype_goal sxml_type_function_signature_rules(
    sxml_type_description_pair pair) {
    const sxml_function_type *base = &pair.base->variety_details.function_type;
    size_t base_parameter_count = base->parameter_types.sequence_type_count;
    switch (pair.derived->variety) {
    case SXML_TYPE_VARIETY_FUNCTION: {
        const sxml_function_type *derived =
            &pair.derived->variety_details.function_type;
        if (derived->is_any_function ||
            derived->parameter_types.sequence_type_count !=
                base_parameter_count) {
            return sxml_type_false_goal();
        }
        return sxml_type_descriptions_goal(SXML_TYPE_GOAL_FUNCTION_SIGNATURE,
                                           pair);
    }
    case SXML_TYPE_VARIETY_MAP:
        return base_parameter_count == 1
                   ? sxml_type_descriptions_goal(
                         SXML_TYPE_GOAL_MAP_AS_FUNCTION, pair)
                   : sxml_type_false_goal();
    case SXML_TYPE_VARIETY_ARRAY:
        return base_parameter_count == 1
                   ? sxml_type_descriptions_goal(
                         SXML_TYPE_GOAL_ARRAY_AS_FUNCTION, pair)
                   : sxml_type_false_goal();
    case SXML_TYPE_VARIETY_RECORD:
        return base_parameter_count == 1
                   ? sxml_type_descriptions_goal(
                         SXML_TYPE_GOAL_RECORD_AS_FUNCTION, pair)
                   : sxml_type_false_goal();
    case SXML_TYPE_VARIETY_COMPLEX:
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_ATOMIC:
    case SXML_TYPE_VARIETY_LIST:
    case SXML_TYPE_VARIETY_UNION:
    case SXML_TYPE_VARIETY_ANY_ITEM:
    case SXML_TYPE_VARIETY_NODE_TEST:
    case SXML_TYPE_VARIETY_JSON_NODE_TEST:
    case SXML_TYPE_VARIETY_ENUMERATION:
    case SXML_TYPE_VARIETY_CHOICE:
        return sxml_type_false_goal();
    }
    return sxml_type_false_goal();
}

/* A map or record type ⊆ map(K, V) (XPath 4.0, sections 3.3.2.7 and
   3.3.2.9; record(*) is map(xs:string, item()*)). */
static sxml_type_subtype_goal sxml_type_map_rules(
    sxml_type_description_pair pair) {
    const sxml_map_type *base = &pair.base->variety_details.map_type;
    if (pair.derived->variety == SXML_TYPE_VARIETY_MAP) {
        return base->is_any_map ? sxml_type_true_goal()
                                : sxml_type_descriptions_goal(
                                      SXML_TYPE_GOAL_MAP_AS_MAP, pair);
    }
    if (pair.derived->variety != SXML_TYPE_VARIETY_RECORD) {
        return sxml_type_false_goal();
    }
    return base->is_any_map ? sxml_type_true_goal()
                            : sxml_type_descriptions_goal(
                                  SXML_TYPE_GOAL_RECORD_AS_MAP, pair);
}

/* A record type ⊆ a record type (XPath 4.0, 3.3.2.9 and 3.3.2.10: a
   nominative type is a subtype of the structural type with its fields, and
   through transitivity of that type's supertypes). */
static sxml_type_subtype_goal sxml_type_record_rules(
    const sxml_type_subtype_search *search, sxml_type_pair pair) {
    const sxml_type_description *derived_description =
        sxml_type_registry_lookup(search->registry, pair.derived_type);
    const sxml_record_type *base =
        &sxml_type_registry_lookup(search->registry, pair.base_type)
             ->variety_details.record_type;
    if (derived_description->variety != SXML_TYPE_VARIETY_RECORD) {
        return sxml_type_false_goal();
    }
    const sxml_record_type *derived =
        &derived_description->variety_details.record_type;
    if (base->is_any_record) {
        return sxml_type_true_goal();
    }
    /* Nominative record types are subtypes only of themselves (3.3.2.10,
       iso-recursive); identity was checked before. */
    if (base->is_nominative || derived->is_any_record ||
        derived->record_fields.record_field_count !=
            base->record_fields.record_field_count) {
        return sxml_type_false_goal();
    }
    return sxml_type_pair_goal(SXML_TYPE_GOAL_RECORD_FIELDS, pair);
}

/* A function, map, array or record type ⊆ base, which is not a choice. */
static sxml_type_subtype_goal sxml_type_function_item_rules(
    const sxml_type_subtype_search *search, sxml_type_pair pair) {
    const sxml_type_description *derived =
        sxml_type_registry_lookup(search->registry, pair.derived_type);
    const sxml_type_description *base =
        sxml_type_registry_lookup(search->registry, pair.base_type);
    if (derived == NULL || base == NULL) {
        /* Not issued by this registry: the caller looked both up already,
           so this does not happen, but the analyzer cannot tell. */
        return sxml_type_false_goal();
    }
    sxml_type_description_pair descriptions = {.derived = derived,
                                               .base = base};
    switch (base->variety) {
    case SXML_TYPE_VARIETY_FUNCTION:
        /* Maps, arrays and records are function items (3.3.2.7 to 3.3.2.9). */
        if (base->variety_details.function_type.is_any_function) {
            return sxml_type_true_goal();
        }
        return sxml_type_function_signature_rules(descriptions);
    case SXML_TYPE_VARIETY_MAP:
        return sxml_type_map_rules(descriptions);
    case SXML_TYPE_VARIETY_ARRAY:
        if (derived->variety != SXML_TYPE_VARIETY_ARRAY) {
            return sxml_type_false_goal();
        }
        if (base->variety_details.array_type.is_any_array) {
            return sxml_type_true_goal();
        }
        return sxml_type_sequence_goal(
            derived->variety_details.array_type.member_type,
            base->variety_details.array_type.member_type);
    case SXML_TYPE_VARIETY_RECORD:
        return sxml_type_record_rules(search, pair);
    case SXML_TYPE_VARIETY_COMPLEX:
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_ATOMIC:
    case SXML_TYPE_VARIETY_LIST:
    case SXML_TYPE_VARIETY_UNION:
    case SXML_TYPE_VARIETY_ANY_ITEM:
    case SXML_TYPE_VARIETY_NODE_TEST:
    case SXML_TYPE_VARIETY_JSON_NODE_TEST:
    case SXML_TYPE_VARIETY_ENUMERATION:
    case SXML_TYPE_VARIETY_CHOICE:
        return sxml_type_false_goal();
    }
    return sxml_type_false_goal();
}

/* --------------------------------------------- Subtyping: item types */

/*
 * The abstract item types that 3.3.2.2 expands into a choice of their
 * concrete subtypes when compared with a choice: xs:anyAtomicType, xnode(),
 * node() and item().
 */
static const sxml_type_identifier sxml_type_any_atomic_expansion[] = {
    {.index = SXML_TYPE_XS_UNTYPED_ATOMIC}, {.index = SXML_TYPE_XS_STRING},
    {.index = SXML_TYPE_XS_BOOLEAN},        {.index = SXML_TYPE_XS_DECIMAL},
    {.index = SXML_TYPE_XS_FLOAT},          {.index = SXML_TYPE_XS_DOUBLE},
    {.index = SXML_TYPE_XS_DURATION},       {.index = SXML_TYPE_XS_DATE_TIME},
    {.index = SXML_TYPE_XS_TIME},           {.index = SXML_TYPE_XS_DATE},
    {.index = SXML_TYPE_XS_G_YEAR_MONTH},   {.index = SXML_TYPE_XS_G_YEAR},
    {.index = SXML_TYPE_XS_G_MONTH_DAY},    {.index = SXML_TYPE_XS_G_DAY},
    {.index = SXML_TYPE_XS_G_MONTH},        {.index = SXML_TYPE_XS_HEX_BINARY},
    {.index = SXML_TYPE_XS_BASE64_BINARY},  {.index = SXML_TYPE_XS_ANY_URI},
    {.index = SXML_TYPE_XS_QNAME},          {.index = SXML_TYPE_XS_NOTATION},
};

static const sxml_type_identifier sxml_type_node_expansion[] = {
    {.index = SXML_TYPE_DOCUMENT_NODE},
    {.index = SXML_TYPE_ELEMENT_NODE},
    {.index = SXML_TYPE_ATTRIBUTE_NODE},
    {.index = SXML_TYPE_TEXT_NODE},
    {.index = SXML_TYPE_COMMENT_NODE},
    {.index = SXML_TYPE_PROCESSING_INSTRUCTION_NODE},
    {.index = SXML_TYPE_NAMESPACE_NODE},
};

static const sxml_type_identifier sxml_type_generic_node_expansion[] = {
    {.index = SXML_TYPE_NODE},
    {.index = SXML_TYPE_JSON_NODE},
};

static const sxml_type_identifier sxml_type_item_expansion[] = {
    {.index = SXML_TYPE_GENERIC_NODE},
    {.index = SXML_TYPE_XS_ANY_ATOMIC_TYPE},
    {.index = SXML_TYPE_FUNCTION},
};

#define SXML_TYPE_VIEW_OF(array)                                               \
    ((sxml_type_identifier_view){                                              \
        .type_identifiers = (array),                                           \
        .type_identifier_count = sizeof(array) / sizeof((array)[0])})

/* The expansion of an abstract item type; an empty view for others. */
static sxml_type_identifier_view sxml_type_abstract_expansion(
    sxml_type_identifier type) {
    switch (type.index) {
    case SXML_TYPE_XS_ANY_ATOMIC_TYPE:
        return SXML_TYPE_VIEW_OF(sxml_type_any_atomic_expansion);
    case SXML_TYPE_NODE:
        return SXML_TYPE_VIEW_OF(sxml_type_node_expansion);
    case SXML_TYPE_GENERIC_NODE:
        return SXML_TYPE_VIEW_OF(sxml_type_generic_node_expansion);
    case SXML_TYPE_ITEM:
        return SXML_TYPE_VIEW_OF(sxml_type_item_expansion);
    default:
        return (sxml_type_identifier_view){.type_identifiers = NULL,
                                           .type_identifier_count = 0};
    }
}

static bool sxml_type_is_generalized_atomic(sxml_type_variety variety) {
    return variety == SXML_TYPE_VARIETY_ATOMIC ||
           variety == SXML_TYPE_VARIETY_UNION;
}

/* The rules of XPath 4.0, section 3.3.2, for issued types. */
static sxml_type_subtype_goal sxml_type_item_rules(
    const sxml_type_subtype_search *search, sxml_type_pair pair) {
    /* 3.3.2.1 General Subtyping Rules. */
    if (sxml_type_identifier_equals(pair.derived_type, pair.base_type) ||
        sxml_type_is(pair.base_type, SXML_TYPE_ITEM) ||
        sxml_type_is(pair.derived_type, SXML_TYPE_XS_ERROR)) {
        return sxml_type_true_goal();
    }
    const sxml_type_description *derived =
        sxml_type_registry_lookup(search->registry, pair.derived_type);
    const sxml_type_description *base =
        sxml_type_registry_lookup(search->registry, pair.base_type);
    /* Rules that take the derived type apart hold for any base type. */
    switch (derived->variety) {
    case SXML_TYPE_VARIETY_CHOICE:
        /* 3.3.2.2: every member. */
        return sxml_type_members_goal((sxml_type_members_question){
            .members = derived->variety_details.member_types,
            .base_type = pair.base_type});
    case SXML_TYPE_VARIETY_ENUMERATION:
        /* 3.3.2.2 and 3.3.2.4: a choice of singleton enumerations. */
        return sxml_type_pair_goal(SXML_TYPE_GOAL_EVERY_ENUMERATION_VALUE,
                                   pair);
    case SXML_TYPE_VARIETY_UNION:
        /* 3.3.2.3: derives from, or every transitive member. */
        if (sxml_type_is_generalized_atomic(base->variety) &&
            sxml_type_registry_derives(search->registry, pair)) {
            return sxml_type_true_goal();
        }
        return sxml_type_members_goal((sxml_type_members_question){
            .members = derived->variety_details.member_types,
            .base_type = pair.base_type});
    case SXML_TYPE_VARIETY_NODE_TEST:
        if (derived->variety_details.node_test.node_kind ==
            SXML_NODE_KIND_DOCUMENT) {
            /* Handles a base choice itself, to split element(a|b). */
            return sxml_type_document_goal(
                SXML_TYPE_GOAL_DOCUMENT,
                (sxml_type_document_question){
                    .element_test =
                        derived->variety_details.node_test.document_element_test,
                    .base_type = pair.base_type});
        }
        break;
    case SXML_TYPE_VARIETY_COMPLEX:
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_ATOMIC:
    case SXML_TYPE_VARIETY_LIST:
    case SXML_TYPE_VARIETY_ANY_ITEM:
    case SXML_TYPE_VARIETY_JSON_NODE_TEST:
    case SXML_TYPE_VARIETY_FUNCTION:
    case SXML_TYPE_VARIETY_MAP:
    case SXML_TYPE_VARIETY_ARRAY:
    case SXML_TYPE_VARIETY_RECORD:
        break;
    }
    if (base->variety == SXML_TYPE_VARIETY_CHOICE) {
        /* 3.3.2.2: some member, else the abstract expansion. */
        return sxml_type_pair_goal(SXML_TYPE_GOAL_SOME_MEMBER, pair);
    }
    switch (derived->variety) {
    case SXML_TYPE_VARIETY_ATOMIC:
        /* 3.3.2.3: generalized atomic types by derivation. */
        return sxml_type_is_generalized_atomic(base->variety) &&
                       sxml_type_registry_derives(search->registry, pair)
                   ? sxml_type_true_goal()
                   : sxml_type_false_goal();
    case SXML_TYPE_VARIETY_NODE_TEST:
        return sxml_type_node_subtype(search->registry, pair)
                   ? sxml_type_true_goal()
                   : sxml_type_false_goal();
    case SXML_TYPE_VARIETY_JSON_NODE_TEST:
        return sxml_type_json_node_rules(search, pair);
    case SXML_TYPE_VARIETY_FUNCTION:
    case SXML_TYPE_VARIETY_MAP:
    case SXML_TYPE_VARIETY_ARRAY:
    case SXML_TYPE_VARIETY_RECORD:
        return sxml_type_function_item_rules(search, pair);
    case SXML_TYPE_VARIETY_COMPLEX:
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_LIST:
    case SXML_TYPE_VARIETY_UNION:
    case SXML_TYPE_VARIETY_ANY_ITEM:
    case SXML_TYPE_VARIETY_ENUMERATION:
    case SXML_TYPE_VARIETY_CHOICE:
        return sxml_type_false_goal();
    }
    return sxml_type_false_goal();
}

/* ------------------------------------------- Subtyping: the search */

/* True for a conjunction, false for a disjunction. */
static bool sxml_type_goal_is_conjunction(sxml_type_subtype_goal_kind kind) {
    switch (kind) {
    case SXML_TYPE_GOAL_SOME_MEMBER:
    case SXML_TYPE_GOAL_SOME_ENUMERATION_MEMBER:
    case SXML_TYPE_GOAL_SOME_DOCUMENT_MEMBER:
        return false;
    case SXML_TYPE_GOAL_TRUTH:
    case SXML_TYPE_GOAL_ITEM:
    case SXML_TYPE_GOAL_SEQUENCE:
    case SXML_TYPE_GOAL_ENUMERATION_VALUE:
    case SXML_TYPE_GOAL_DOCUMENT:
    case SXML_TYPE_GOAL_EVERY_MEMBER:
    case SXML_TYPE_GOAL_EVERY_ENUMERATION_VALUE:
    case SXML_TYPE_GOAL_EVERY_DOCUMENT_ELEMENT:
    case SXML_TYPE_GOAL_RECORD_VALUES:
    case SXML_TYPE_GOAL_RECORD_FIELDS:
    case SXML_TYPE_GOAL_FUNCTION_SIGNATURE:
    case SXML_TYPE_GOAL_MAP_AS_FUNCTION:
    case SXML_TYPE_GOAL_ARRAY_AS_FUNCTION:
    case SXML_TYPE_GOAL_RECORD_AS_FUNCTION:
    case SXML_TYPE_GOAL_MAP_AS_MAP:
    case SXML_TYPE_GOAL_RECORD_AS_MAP:
        return true;
    }
    return true;
}

/* The members of the choice type, of an issued identifier. */
static sxml_type_identifier_view sxml_type_choice_members(
    const sxml_type_subtype_search *search, sxml_type_identifier choice) {
    return sxml_type_registry_lookup(search->registry, choice)
        ->variety_details.member_types;
}

/* How many sub-goals a conjunction or disjunction has. */
static size_t sxml_type_goal_count(const sxml_type_subtype_search *search,
                                   const sxml_type_subtype_goal *goal) {
    switch (goal->kind) {
    case SXML_TYPE_GOAL_EVERY_MEMBER:
        return goal->question.members.members.type_identifier_count;
    case SXML_TYPE_GOAL_EVERY_ENUMERATION_VALUE:
        return sxml_type_registry_lookup(search->registry,
                                         goal->question.pair.derived_type)
            ->variety_details.enumeration_values.byte_view_count;
    case SXML_TYPE_GOAL_EVERY_DOCUMENT_ELEMENT:
        return sxml_type_choice_members(search,
                                        goal->question.document.element_test)
            .type_identifier_count;
    case SXML_TYPE_GOAL_RECORD_VALUES:
        return goal->question.record_values.record_type->record_fields
            .record_field_count;
    case SXML_TYPE_GOAL_RECORD_FIELDS:
        return sxml_type_registry_lookup(search->registry,
                                         goal->question.pair.derived_type)
            ->variety_details.record_type.record_fields.record_field_count;
    case SXML_TYPE_GOAL_FUNCTION_SIGNATURE:
        return 1 + goal->question.descriptions.base->variety_details
                       .function_type.parameter_types.sequence_type_count;
    case SXML_TYPE_GOAL_MAP_AS_FUNCTION:
    case SXML_TYPE_GOAL_ARRAY_AS_FUNCTION:
    case SXML_TYPE_GOAL_MAP_AS_MAP:
    case SXML_TYPE_GOAL_RECORD_AS_MAP:
        return 2;
    case SXML_TYPE_GOAL_RECORD_AS_FUNCTION:
        return 3;
    case SXML_TYPE_GOAL_SOME_MEMBER:
        return sxml_type_choice_members(search, goal->question.pair.base_type)
                   .type_identifier_count +
               (sxml_type_abstract_expansion(goal->question.pair.derived_type)
                            .type_identifier_count != 0
                    ? 1U
                    : 0U);
    case SXML_TYPE_GOAL_SOME_ENUMERATION_MEMBER:
        return sxml_type_choice_members(search,
                                        goal->question.value.base_type)
            .type_identifier_count;
    case SXML_TYPE_GOAL_SOME_DOCUMENT_MEMBER:
        return sxml_type_choice_members(search,
                                        goal->question.document.base_type)
            .type_identifier_count;
    case SXML_TYPE_GOAL_TRUTH:
    case SXML_TYPE_GOAL_ITEM:
    case SXML_TYPE_GOAL_SEQUENCE:
    case SXML_TYPE_GOAL_ENUMERATION_VALUE:
    case SXML_TYPE_GOAL_DOCUMENT:
        return 0;
    }
    return 0;
}

/* The sub-goal of a function signature or a map, array or record as a
   function or map, at position goal_index. */
static sxml_type_subtype_goal sxml_type_signature_goal(
    const sxml_type_subtype_frame *frame) {
    const sxml_type_subtype_goal *goal = &frame->goal;
    size_t goal_index = frame->next_goal_index;
    const sxml_type_description *derived = goal->question.descriptions.derived;
    const sxml_type_description *base = goal->question.descriptions.base;
    switch (goal->kind) {
    case SXML_TYPE_GOAL_FUNCTION_SIGNATURE: {
        const sxml_function_type *derived_function =
            &derived->variety_details.function_type;
        const sxml_function_type *base_function =
            &base->variety_details.function_type;
        if (goal_index == 0) {
            return sxml_type_sequence_goal(derived_function->result_type,
                                           base_function->result_type);
        }
        /* Parameters are contravariant. */
        return sxml_type_sequence_goal(
            base_function->parameter_types.sequence_types[goal_index - 1],
            derived_function->parameter_types.sequence_types[goal_index - 1]);
    }
    case SXML_TYPE_GOAL_MAP_AS_FUNCTION: {
        const sxml_function_type *base_function =
            &base->variety_details.function_type;
        if (goal_index == 0) {
            return sxml_type_sequence_goal(
                base_function->parameter_types.sequence_types[0],
                sxml_type_exactly_one(SXML_TYPE_XS_ANY_ATOMIC_TYPE));
        }
        return sxml_type_sequence_goal(
            sxml_type_or_empty(derived->variety_details.map_type.value_type),
            base_function->result_type);
    }
    case SXML_TYPE_GOAL_ARRAY_AS_FUNCTION: {
        const sxml_function_type *base_function =
            &base->variety_details.function_type;
        if (goal_index == 0) {
            return sxml_type_sequence_goal(
                base_function->parameter_types.sequence_types[0],
                sxml_type_exactly_one(SXML_TYPE_XS_INTEGER));
        }
        return sxml_type_sequence_goal(
            derived->variety_details.array_type.member_type,
            base_function->result_type);
    }
    case SXML_TYPE_GOAL_RECORD_AS_FUNCTION: {
        const sxml_function_type *base_function =
            &base->variety_details.function_type;
        if (goal_index == 0) {
            return sxml_type_sequence_goal(
                base_function->parameter_types.sequence_types[0],
                sxml_type_exactly_one(SXML_TYPE_XS_ANY_ATOMIC_TYPE));
        }
        if (goal_index == 1) {
            return sxml_type_sequence_allows_empty(base_function->result_type)
                       ? sxml_type_true_goal()
                       : sxml_type_false_goal();
        }
        return sxml_type_record_values_goal(
            &derived->variety_details.record_type, base_function->result_type);
    }
    case SXML_TYPE_GOAL_MAP_AS_MAP: {
        const sxml_map_type *derived_map = &derived->variety_details.map_type;
        const sxml_map_type *base_map = &base->variety_details.map_type;
        if (goal_index == 0) {
            return sxml_type_item_goal(derived_map->key_type,
                                       base_map->key_type);
        }
        return sxml_type_sequence_goal(derived_map->value_type,
                                       base_map->value_type);
    }
    case SXML_TYPE_GOAL_RECORD_AS_MAP: {
        const sxml_map_type *base_map = &base->variety_details.map_type;
        if (goal_index == 0) {
            return sxml_type_item_goal(
                sxml_type_of_builtin(SXML_TYPE_XS_STRING), base_map->key_type);
        }
        return sxml_type_record_values_goal(
            &derived->variety_details.record_type, base_map->value_type);
    }
    case SXML_TYPE_GOAL_TRUTH:
    case SXML_TYPE_GOAL_ITEM:
    case SXML_TYPE_GOAL_SEQUENCE:
    case SXML_TYPE_GOAL_ENUMERATION_VALUE:
    case SXML_TYPE_GOAL_DOCUMENT:
    case SXML_TYPE_GOAL_EVERY_MEMBER:
    case SXML_TYPE_GOAL_EVERY_ENUMERATION_VALUE:
    case SXML_TYPE_GOAL_EVERY_DOCUMENT_ELEMENT:
    case SXML_TYPE_GOAL_RECORD_VALUES:
    case SXML_TYPE_GOAL_RECORD_FIELDS:
    case SXML_TYPE_GOAL_SOME_MEMBER:
    case SXML_TYPE_GOAL_SOME_ENUMERATION_MEMBER:
    case SXML_TYPE_GOAL_SOME_DOCUMENT_MEMBER:
        break;
    }
    return sxml_type_false_goal();
}

/* Field goal_index of the derived record is in the base record, with a
   supertype (3.3.2.9). */
static sxml_type_subtype_goal sxml_type_record_field_goal(
    const sxml_type_subtype_search *search,
    const sxml_type_subtype_frame *frame) {
    sxml_type_pair pair = frame->goal.question.pair;
    const sxml_record_field *field =
        &sxml_type_registry_lookup(search->registry, pair.derived_type)
             ->variety_details.record_type.record_fields
             .record_fields[frame->next_goal_index];
    const sxml_record_type *base =
        &sxml_type_registry_lookup(search->registry, pair.base_type)
             ->variety_details.record_type;
    size_t base_position = 0;
    if (!sxml_type_entry_find_string(
            (sxml_type_string_query){
                .entry = sxml_type_registry_entry_of(search->registry,
                                                     pair.base_type),
                .text = field->field_name},
            &base_position)) {
        return sxml_type_false_goal();
    }
    return sxml_type_sequence_goal(
        field->field_type,
        base->record_fields.record_fields[base_position].field_type);
}

/* The sub-goal at frame->next_goal_index, below the goal count. */
static sxml_type_subtype_goal sxml_type_next_goal(
    const sxml_type_subtype_search *search,
    const sxml_type_subtype_frame *frame) {
    const sxml_type_subtype_goal *goal = &frame->goal;
    size_t goal_index = frame->next_goal_index;
    switch (goal->kind) {
    case SXML_TYPE_GOAL_EVERY_MEMBER:
        return sxml_type_item_goal(
            goal->question.members.members.type_identifiers[goal_index],
            goal->question.members.base_type);
    case SXML_TYPE_GOAL_EVERY_ENUMERATION_VALUE:
        return sxml_type_value_goal(
            SXML_TYPE_GOAL_ENUMERATION_VALUE,
            (sxml_type_value_question){
                .value = sxml_type_registry_lookup(
                             search->registry, goal->question.pair.derived_type)
                             ->variety_details.enumeration_values
                             .byte_views[goal_index],
                .base_type = goal->question.pair.base_type});
    case SXML_TYPE_GOAL_EVERY_DOCUMENT_ELEMENT:
        return sxml_type_document_goal(
            SXML_TYPE_GOAL_DOCUMENT,
            (sxml_type_document_question){
                .element_test =
                    sxml_type_choice_members(
                        search, goal->question.document.element_test)
                        .type_identifiers[goal_index],
                .base_type = goal->question.document.base_type});
    case SXML_TYPE_GOAL_RECORD_VALUES:
        return sxml_type_sequence_goal(
            goal->question.record_values.record_type->record_fields
                .record_fields[goal_index]
                .field_type,
            goal->question.record_values.base_value_type);
    case SXML_TYPE_GOAL_RECORD_FIELDS:
        return sxml_type_record_field_goal(search, frame);
    case SXML_TYPE_GOAL_FUNCTION_SIGNATURE:
    case SXML_TYPE_GOAL_MAP_AS_FUNCTION:
    case SXML_TYPE_GOAL_ARRAY_AS_FUNCTION:
    case SXML_TYPE_GOAL_RECORD_AS_FUNCTION:
    case SXML_TYPE_GOAL_MAP_AS_MAP:
    case SXML_TYPE_GOAL_RECORD_AS_MAP:
        return sxml_type_signature_goal(frame);
    case SXML_TYPE_GOAL_SOME_MEMBER: {
        sxml_type_pair pair = goal->question.pair;
        sxml_type_identifier_view members =
            sxml_type_choice_members(search, pair.base_type);
        if (goal_index < members.type_identifier_count) {
            return sxml_type_item_goal(pair.derived_type,
                                       members.type_identifiers[goal_index]);
        }
        return sxml_type_members_goal((sxml_type_members_question){
            .members = sxml_type_abstract_expansion(pair.derived_type),
            .base_type = pair.base_type});
    }
    case SXML_TYPE_GOAL_SOME_ENUMERATION_MEMBER:
        return sxml_type_value_goal(
            SXML_TYPE_GOAL_ENUMERATION_VALUE,
            (sxml_type_value_question){
                .value = goal->question.value.value,
                .base_type = sxml_type_choice_members(
                                 search, goal->question.value.base_type)
                                 .type_identifiers[goal_index]});
    case SXML_TYPE_GOAL_SOME_DOCUMENT_MEMBER:
        return sxml_type_document_goal(
            SXML_TYPE_GOAL_DOCUMENT,
            (sxml_type_document_question){
                .element_test = goal->question.document.element_test,
                .base_type = sxml_type_choice_members(
                                 search, goal->question.document.base_type)
                                 .type_identifiers[goal_index]});
    case SXML_TYPE_GOAL_TRUTH:
    case SXML_TYPE_GOAL_ITEM:
    case SXML_TYPE_GOAL_SEQUENCE:
    case SXML_TYPE_GOAL_ENUMERATION_VALUE:
    case SXML_TYPE_GOAL_DOCUMENT:
        break;
    }
    return sxml_type_false_goal();
}

/* Enters the item goal of frame: false, and the goal is false, when the
   budget is spent or a type was not issued. */
static bool sxml_type_enter_item_goal(sxml_type_subtype_search *search,
                                      sxml_type_subtype_frame *frame) {
    sxml_type_pair pair = frame->goal.question.pair;
    if (search->remaining_step_count == 0 ||
        search->depth >= SXML_TYPE_SUBTYPE_MAXIMUM_DEPTH ||
        !sxml_type_registry_issued(search->registry, pair.derived_type) ||
        !sxml_type_registry_issued(search->registry, pair.base_type)) {
        return false;
    }
    search->remaining_step_count -= 1;
    search->depth += 1;
    frame->item_depth += 1;
    return true;
}

/* What reducing a goal gave: an answer, or a conjunction or
   disjunction. */
typedef struct sxml_type_reduction {
    bool is_answered;
    bool answer;
} sxml_type_reduction;

/*
 * Applies the rules to the goal of frame until it is answered or is a
 * conjunction or disjunction, whose first sub-goal is then next.
 */
static sxml_type_reduction sxml_type_reduce_goal(
    sxml_type_subtype_search *search, sxml_type_subtype_frame *frame) {
    frame->next_goal_index = 0;
    sxml_type_subtype_goal *goal = &frame->goal;
    for (;;) {
        switch (goal->kind) {
        case SXML_TYPE_GOAL_TRUTH:
            return (sxml_type_reduction){.is_answered = true,
                                         .answer = goal->question.truth};
        case SXML_TYPE_GOAL_ITEM:
            if (!sxml_type_enter_item_goal(search, frame)) {
                return (sxml_type_reduction){.is_answered = true,
                                             .answer = false};
            }
            *goal = sxml_type_item_rules(search, goal->question.pair);
            break;
        case SXML_TYPE_GOAL_SEQUENCE:
            *goal = sxml_type_sequence_rules(goal->question.sequence_pair);
            break;
        case SXML_TYPE_GOAL_ENUMERATION_VALUE:
            *goal = sxml_type_enumeration_value_rules(search,
                                                      goal->question.value);
            break;
        case SXML_TYPE_GOAL_DOCUMENT:
            *goal = sxml_type_document_rules(search, goal->question.document);
            break;
        case SXML_TYPE_GOAL_EVERY_MEMBER:
        case SXML_TYPE_GOAL_EVERY_ENUMERATION_VALUE:
        case SXML_TYPE_GOAL_EVERY_DOCUMENT_ELEMENT:
        case SXML_TYPE_GOAL_RECORD_VALUES:
        case SXML_TYPE_GOAL_RECORD_FIELDS:
        case SXML_TYPE_GOAL_FUNCTION_SIGNATURE:
        case SXML_TYPE_GOAL_MAP_AS_FUNCTION:
        case SXML_TYPE_GOAL_ARRAY_AS_FUNCTION:
        case SXML_TYPE_GOAL_RECORD_AS_FUNCTION:
        case SXML_TYPE_GOAL_MAP_AS_MAP:
        case SXML_TYPE_GOAL_RECORD_AS_MAP:
        case SXML_TYPE_GOAL_SOME_MEMBER:
        case SXML_TYPE_GOAL_SOME_ENUMERATION_MEMBER:
        case SXML_TYPE_GOAL_SOME_DOCUMENT_MEMBER:
            return (sxml_type_reduction){.is_answered = false,
                                         .answer = false};
        }
    }
}

/* The answer to goal, within the budget of search. */
static bool sxml_type_answer_goal(sxml_type_subtype_search *search,
                                  sxml_type_subtype_goal goal) {
    search->frames[0] = (sxml_type_subtype_frame){
        .goal = goal, .next_goal_index = 0, .item_depth = 0};
    search->frame_count = 1;
    sxml_type_reduction reduction =
        sxml_type_reduce_goal(search, &search->frames[0]);
    for (;;) {
        sxml_type_subtype_frame *top = &search->frames[search->frame_count - 1];
        if (reduction.is_answered) {
            /* The goal of top is answered: give the answer to the frame
               below, which is decided when the answer short-circuits it. */
            search->depth -= top->item_depth;
            search->frame_count -= 1;
            if (search->frame_count == 0) {
                return reduction.answer;
            }
            top = &search->frames[search->frame_count - 1];
            if (reduction.answer !=
                sxml_type_goal_is_conjunction(top->goal.kind)) {
                continue;
            }
            reduction.is_answered = false;
        }
        size_t goal_count = sxml_type_goal_count(search, &top->goal);
        if (top->next_goal_index == goal_count) {
            /* Every sub-goal held (conjunction) or none did. */
            reduction = (sxml_type_reduction){
                .is_answered = true,
                .answer = sxml_type_goal_is_conjunction(top->goal.kind)};
            continue;
        }
        sxml_type_subtype_goal next_goal = sxml_type_next_goal(search, top);
        top->next_goal_index += 1;
        if (top->next_goal_index == goal_count) {
            /* The last sub-goal answers for the frame. */
            top->goal = next_goal;
            reduction = sxml_type_reduce_goal(search, top);
            continue;
        }
        if (search->frame_count == SXML_TYPE_SUBTYPE_MAXIMUM_FRAME_COUNT) {
            /* Out of frames: the frame is false (D58). */
            reduction = (sxml_type_reduction){.is_answered = true,
                                              .answer = false};
            continue;
        }
        sxml_type_subtype_frame *frame = &search->frames[search->frame_count];
        search->frame_count += 1;
        *frame = (sxml_type_subtype_frame){
            .goal = next_goal, .next_goal_index = 0, .item_depth = 0};
        reduction = sxml_type_reduce_goal(search, frame);
    }
}

static bool sxml_type_answer(const sxml_type_registry *registry,
                             sxml_type_subtype_goal goal) {
    sxml_type_subtype_search search;
    search.registry = registry;
    search.depth = 0;
    search.remaining_step_count = SXML_TYPE_SUBTYPE_MAXIMUM_STEP_COUNT;
    search.frame_count = 0;
    return sxml_type_answer_goal(&search, goal);
}

bool sxml_type_registry_is_subtype(const sxml_type_registry *registry,
                                   sxml_type_pair pair) {
    return sxml_type_answer(
        registry, sxml_type_item_goal(pair.derived_type, pair.base_type));
}

static bool sxml_type_occurrence_is_valid(sxml_occurrence occurrence) {
    switch (occurrence) {
    case SXML_OCCURRENCE_ZERO:
    case SXML_OCCURRENCE_EXACTLY_ONE:
    case SXML_OCCURRENCE_ZERO_OR_ONE:
    case SXML_OCCURRENCE_ONE_OR_MORE:
    case SXML_OCCURRENCE_ZERO_OR_MORE:
        return true;
    }
    return false;
}

/* A sequence type with a valid occurrence and, unless it is
   empty-sequence(), an issued item type. */
static bool sxml_type_registry_sequence_type_is_valid(
    const sxml_type_registry *registry, sxml_sequence_type sequence_type) {
    return sxml_type_occurrence_is_valid(sequence_type.occurrence) &&
           (sequence_type.occurrence == SXML_OCCURRENCE_ZERO ||
            sxml_type_registry_is_item_type(registry, sequence_type.item_type));
}

bool sxml_type_registry_is_sequence_subtype(const sxml_type_registry *registry,
                                            sxml_sequence_type_pair pair) {
    if (!sxml_type_registry_sequence_type_is_valid(registry,
                                                   pair.derived_type) ||
        !sxml_type_registry_sequence_type_is_valid(registry, pair.base_type)) {
        return false;
    }
    return sxml_type_answer(
        registry, sxml_type_sequence_goal(pair.derived_type, pair.base_type));
}

/* ---------------------------------------------------------- Registration */

/* Doubles the entry array when it is full; on failure changes nothing. */
static sxml_status sxml_type_registry_grow_entries(sxml_type_registry *registry) {
    if (registry->entry_count < registry->entry_capacity) {
        return SXML_STATUS_OK;
    }
    size_t new_capacity = SXML_TYPE_MINIMUM_ENTRY_CAPACITY;
    sxml_status status = SXML_STATUS_OK;
    if (registry->entry_capacity != 0) {
        status = sxml_size_multiply(registry->entry_capacity, 2, &new_capacity);
    }
    size_t byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(new_capacity, sizeof(sxml_type_entry),
                                    &byte_count);
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_allocate(
            registry,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(sxml_type_entry)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (registry->entry_count != 0) {
        memcpy(memory, registry->entries,
               registry->entry_count * sizeof(sxml_type_entry));
    }
    sxml_type_registry_release(registry, registry->entries);
    registry->entries = memory;
    registry->entry_capacity = new_capacity;
    return SXML_STATUS_OK;
}

/* Rebuilds index with twice the slots when one more entry would fill it
   beyond three quarters. On failure changes nothing. */
typedef struct sxml_type_index_growth {
    sxml_type_hash_index *index;
    /* Entries the index holds now. */
    size_t indexed_count;
} sxml_type_index_growth;

static sxml_status sxml_type_registry_grow_index(sxml_type_registry *registry,
                                                 sxml_type_index_growth growth) {
    sxml_type_hash_index *index = growth.index;
    /* Entries take far more memory than 4 bytes, so no product wraps. */
    if ((growth.indexed_count + 1) * 4 <= index->slot_count * 3) {
        return SXML_STATUS_OK;
    }
    size_t new_slot_count = SXML_TYPE_MINIMUM_SLOT_COUNT;
    sxml_status status = SXML_STATUS_OK;
    if (index->slot_count != 0) {
        status = sxml_size_multiply(index->slot_count, 2, &new_slot_count);
    }
    size_t byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(new_slot_count, sizeof(sxml_type_slot),
                                    &byte_count);
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_allocate(
            registry,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(sxml_type_slot)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_type_hash_index new_index = {.slots = memory,
                                      .slot_count = new_slot_count};
    memset(new_index.slots, 0, byte_count);
    for (size_t slot_index = 0; slot_index < index->slot_count;
         slot_index += 1) {
        if (index->slots[slot_index].entry_number != 0) {
            sxml_type_hash_index_insert(&new_index, index->slots[slot_index]);
        }
    }
    sxml_type_registry_release(registry, index->slots);
    *index = new_index;
    return SXML_STATUS_OK;
}

/*
 * Checks that one more type may be registered and makes room for it in the
 * entry array and in index. On failure the registry is unchanged.
 */
static sxml_status sxml_type_registry_prepare_entry(
    sxml_type_registry *registry, sxml_type_index_growth growth) {
    if (sxml_allocator_is_null(registry->configuration.allocator)) {
        return SXML_STATUS_CAPACITY_EXCEEDED;
    }
    uint32_t maximum = registry->configuration.maximum_registered_type_count;
    if ((maximum != 0 && registry->entry_count >= maximum) ||
        registry->entry_count >= SXML_TYPE_MAXIMUM_ENTRY_COUNT) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    sxml_status status = sxml_type_registry_grow_entries(registry);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_type_registry_grow_index(registry, growth);
}

static sxml_type_identifier sxml_type_identifier_at(size_t entry_position) {
    return (sxml_type_identifier){
        .index = (uint32_t)(entry_position + SXML_TYPE_FIRST_REGISTERED)};
}

/* ----------------------------------------------- Validation and normal form */

static bool sxml_type_registry_name_test_is_valid(
    const sxml_type_registry *registry, const sxml_name_test *name_test) {
    const sxml_name_pool *name_pool = registry->configuration.name_pool;
    switch (name_test->name_test_kind) {
    case SXML_NAME_TEST_KIND_ANY_NAME:
        return true;
    case SXML_NAME_TEST_KIND_EXPANDED_NAME:
        return name_test->expanded_name.index != 0 &&
               name_test->expanded_name.index <
                   sxml_name_pool_name_limit(name_pool);
    case SXML_NAME_TEST_KIND_NAMESPACE_WILDCARD:
        return name_test->namespace_uri.index <
               sxml_name_pool_namespace_uri_limit(name_pool);
    case SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD:
        return name_test->local_name.byte_count != 0 &&
               name_test->local_name.bytes != NULL;
    }
    return false;
}

/* Keeps only the fields the kind of a name test uses. */
static sxml_name_test sxml_type_normal_name_test(sxml_name_test name_test) {
    sxml_name_test normal = {.name_test_kind = name_test.name_test_kind,
                             .expanded_name = {.index = 0},
                             .namespace_uri = {.index = 0},
                             .local_name = {.bytes = NULL, .byte_count = 0}};
    switch (name_test.name_test_kind) {
    case SXML_NAME_TEST_KIND_ANY_NAME:
        break;
    case SXML_NAME_TEST_KIND_EXPANDED_NAME:
        normal.expanded_name = name_test.expanded_name;
        break;
    case SXML_NAME_TEST_KIND_NAMESPACE_WILDCARD:
        normal.namespace_uri = name_test.namespace_uri;
        break;
    case SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD:
        normal.local_name = name_test.local_name;
        break;
    }
    return normal;
}

/* An element test, or a choice of element tests: what document-node()
   may contain (XPath 4.0, section 3.2.7.1). */
static bool sxml_type_registry_is_element_test(
    const sxml_type_registry *registry, sxml_type_identifier type) {
    const sxml_type_description *description =
        sxml_type_registry_lookup(registry, type);
    if (description == NULL) {
        return false;
    }
    if (description->variety == SXML_TYPE_VARIETY_NODE_TEST) {
        return description->variety_details.node_test.node_kind ==
               SXML_NODE_KIND_ELEMENT;
    }
    if (description->variety != SXML_TYPE_VARIETY_CHOICE) {
        return false;
    }
    sxml_type_identifier_view members =
        description->variety_details.member_types;
    for (size_t member_index = 0; member_index < members.type_identifier_count;
         member_index += 1) {
        const sxml_type_description *member = sxml_type_registry_lookup(
            registry, members.type_identifiers[member_index]);
        if (member->variety != SXML_TYPE_VARIETY_NODE_TEST ||
            member->variety_details.node_test.node_kind !=
                SXML_NODE_KIND_ELEMENT) {
            return false;
        }
    }
    return true;
}

static bool sxml_type_registry_is_schema_type(
    const sxml_type_registry *registry, sxml_type_identifier type) {
    const sxml_type_description *description =
        sxml_type_registry_lookup(registry, type);
    if (description == NULL) {
        return false;
    }
    switch (description->variety) {
    case SXML_TYPE_VARIETY_COMPLEX:
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_ATOMIC:
    case SXML_TYPE_VARIETY_LIST:
    case SXML_TYPE_VARIETY_UNION:
        return true;
    case SXML_TYPE_VARIETY_ANY_ITEM:
    case SXML_TYPE_VARIETY_NODE_TEST:
    case SXML_TYPE_VARIETY_JSON_NODE_TEST:
    case SXML_TYPE_VARIETY_FUNCTION:
    case SXML_TYPE_VARIETY_MAP:
    case SXML_TYPE_VARIETY_ARRAY:
    case SXML_TYPE_VARIETY_RECORD:
    case SXML_TYPE_VARIETY_ENUMERATION:
    case SXML_TYPE_VARIETY_CHOICE:
        return false;
    }
    return false;
}

/* Validates a node test (XPath 4.0, section 3.2.7) and writes its normal
   form; false for an invalid one. */
static bool sxml_type_registry_normalize_node_test(
    const sxml_type_registry *registry, sxml_node_test *node_test) {
    if (!sxml_type_registry_name_test_is_valid(registry,
                                               &node_test->name_test)) {
        return false;
    }
    node_test->name_test = sxml_type_normal_name_test(node_test->name_test);
    sxml_name_test_kind name_kind = node_test->name_test.name_test_kind;
    bool has_type = node_test->type_annotation.index != SXML_TYPE_NONE;
    bool has_document_element =
        node_test->document_element_test.index != SXML_TYPE_NONE;
    if (has_type &&
        !sxml_type_registry_is_schema_type(registry,
                                           node_test->type_annotation)) {
        return false;
    }
    switch (node_test->node_kind) {
    case SXML_NODE_KIND_ELEMENT:
    case SXML_NODE_KIND_ATTRIBUTE:
        if (has_document_element ||
            (node_test->is_schema_declaration &&
             (name_kind != SXML_NAME_TEST_KIND_EXPANDED_NAME || has_type))) {
            return false;
        }
        /* Only element(N, T?) can match nilled elements. */
        node_test->allows_nilled = node_test->node_kind ==
                                       SXML_NODE_KIND_ELEMENT &&
                                   has_type && node_test->allows_nilled;
        return true;
    case SXML_NODE_KIND_DOCUMENT:
        node_test->allows_nilled = false;
        return name_kind == SXML_NAME_TEST_KIND_ANY_NAME && !has_type &&
               !node_test->is_schema_declaration &&
               (!has_document_element ||
                sxml_type_registry_is_element_test(
                    registry, node_test->document_element_test));
    case SXML_NODE_KIND_PROCESSING_INSTRUCTION:
        node_test->allows_nilled = false;
        return (name_kind == SXML_NAME_TEST_KIND_ANY_NAME ||
                name_kind == SXML_NAME_TEST_KIND_EXPANDED_NAME) &&
               !has_type && !has_document_element &&
               !node_test->is_schema_declaration;
    case SXML_NODE_KIND_NAMESPACE:
        /* namespace::x (XPath 4.0, section 4.6.5.1; D77): a name test on
           the namespace axis, whose principal node kind is the namespace
           node; the node's name is its prefix, in no namespace. */
        node_test->allows_nilled = false;
        return !has_type && !has_document_element &&
               !node_test->is_schema_declaration;
    case SXML_NODE_KIND_TEXT:
    case SXML_NODE_KIND_COMMENT:
    case SXML_NODE_KIND_ANY:
        node_test->allows_nilled = false;
        return name_kind == SXML_NAME_TEST_KIND_ANY_NAME && !has_type &&
               !has_document_element && !node_test->is_schema_declaration;
    }
    return false;
}

/* The key types a JNode key constant may have (XPath 4.0, 3.2.12). */
static bool sxml_type_is_json_key_type(sxml_type_identifier type) {
    switch (type.index) {
    case SXML_TYPE_XS_STRING:
    case SXML_TYPE_XS_INTEGER:
    case SXML_TYPE_XS_DECIMAL:
    case SXML_TYPE_XS_DOUBLE:
    case SXML_TYPE_XS_QNAME:
    case SXML_TYPE_XS_BOOLEAN:
        return true;
    default:
        return false;
    }
}

static bool sxml_type_json_key_constraint_is_valid(
    sxml_json_key_constraint key_constraint) {
    switch (key_constraint) {
    case SXML_JSON_KEY_CONSTRAINT_ANY:
    case SXML_JSON_KEY_CONSTRAINT_ROOT:
    case SXML_JSON_KEY_CONSTRAINT_CONSTANT:
        return true;
    }
    return false;
}

static bool sxml_type_registry_normalize_json_node_test(
    const sxml_type_registry *registry, sxml_json_node_test *test) {
    switch (test->key_constraint) {
    case SXML_JSON_KEY_CONSTRAINT_ANY:
    case SXML_JSON_KEY_CONSTRAINT_ROOT:
        test->key_type = sxml_type_of_builtin(SXML_TYPE_NONE);
        test->key_lexical_form = (sxml_byte_view){.bytes = NULL,
                                                  .byte_count = 0};
        break;
    case SXML_JSON_KEY_CONSTRAINT_CONSTANT:
        if (!sxml_type_is_json_key_type(test->key_type) ||
            (test->key_lexical_form.bytes == NULL &&
             test->key_lexical_form.byte_count != 0)) {
            return false;
        }
        break;
    }
    if (!sxml_type_json_key_constraint_is_valid(test->key_constraint)) {
        return false;
    }
    if (!test->has_value_type) {
        test->value_type = sxml_type_any_sequence;
        return true;
    }
    return sxml_type_registry_sequence_type_is_valid(registry,
                                                     test->value_type);
}

static bool sxml_type_registry_sequence_types_are_valid(
    const sxml_type_registry *registry, sxml_sequence_type_view view) {
    if (view.sequence_type_count != 0 && view.sequence_types == NULL) {
        return false;
    }
    for (size_t type_index = 0; type_index < view.sequence_type_count;
         type_index += 1) {
        if (!sxml_type_registry_sequence_type_is_valid(
                registry, view.sequence_types[type_index])) {
            return false;
        }
    }
    return true;
}

/* map(K, V) keys: a generalized atomic type (XPath 4.0, section 3.2.9.2):
   an atomic, union or enumeration type, or a choice of them, such as
   map((xs:integer | xs:string), V) in the signature of fn:replace (found by
   tests/fuzz/xpath_compile_fuzz.c, packet 1.26). */
static bool sxml_type_registry_is_map_key_type(
    const sxml_type_registry *registry, sxml_type_identifier type) {
    const sxml_type_description *description =
        sxml_type_registry_lookup(registry, type);
    if (description == NULL) {
        return false;
    }
    if (description->variety == SXML_TYPE_VARIETY_CHOICE) {
        return sxml_type_registry_is_subtype(
            registry, (sxml_type_pair){
                          .derived_type = type,
                          .base_type = sxml_type_of_builtin(
                              SXML_TYPE_XS_ANY_ATOMIC_TYPE)});
    }
    return sxml_type_is_generalized_atomic(description->variety) ||
           description->variety == SXML_TYPE_VARIETY_ENUMERATION;
}

static bool sxml_type_registry_normalize_record(
    const sxml_type_registry *registry, sxml_record_type *record) {
    if (record->is_nominative) {
        return false;
    }
    if (record->is_any_record) {
        record->record_fields = (sxml_record_field_view){.record_fields = NULL,
                                                         .record_field_count = 0};
        return true;
    }
    sxml_record_field_view fields = record->record_fields;
    if (fields.record_field_count != 0 && fields.record_fields == NULL) {
        return false;
    }
    for (size_t field_index = 0; field_index < fields.record_field_count;
         field_index += 1) {
        const sxml_record_field *field = &fields.record_fields[field_index];
        if ((field->field_name.bytes == NULL &&
             field->field_name.byte_count != 0) ||
            !sxml_type_registry_sequence_type_is_valid(registry,
                                                       field->field_type)) {
            return false;
        }
    }
    return true;
}

static bool sxml_type_registry_normalize_choice(
    const sxml_type_registry *registry, sxml_type_identifier_view members) {
    if (members.type_identifier_count == 0 ||
        members.type_identifiers == NULL) {
        return false;
    }
    for (size_t member_index = 0; member_index < members.type_identifier_count;
         member_index += 1) {
        if (!sxml_type_registry_is_item_type(
                registry, members.type_identifiers[member_index])) {
            return false;
        }
    }
    return true;
}

static bool sxml_type_registry_normalize_enumeration(sxml_byte_view_list values) {
    if (values.byte_view_count == 0 || values.byte_views == NULL) {
        return false;
    }
    for (size_t value_index = 0; value_index < values.byte_view_count;
         value_index += 1) {
        if (values.byte_views[value_index].bytes == NULL &&
            values.byte_views[value_index].byte_count != 0) {
            return false;
        }
    }
    return true;
}

/*
 * Validates an item type description and turns it into its normal form:
 * name, base and primitive type cleared, and every field the description's
 * flags make meaningless set as in the built-in descriptions. Views still
 * point to the caller's data. False for an invalid description.
 */
static bool sxml_type_registry_normalize(const sxml_type_registry *registry,
                                         sxml_type_description *description) {
    description->type_name = (sxml_name_identifier){.index = 0};
    description->base_type = sxml_type_of_builtin(SXML_TYPE_NONE);
    description->primitive_type = sxml_type_of_builtin(SXML_TYPE_NONE);
    description->is_builtin = false;
    /* Item types have no facets (include/sxml/types.h, D165). */
    description->facets = (sxml_facet_view){.facets = NULL, .facet_count = 0};
    switch (description->variety) {
    case SXML_TYPE_VARIETY_NODE_TEST:
        return sxml_type_registry_normalize_node_test(
            registry, &description->variety_details.node_test);
    case SXML_TYPE_VARIETY_JSON_NODE_TEST:
        return sxml_type_registry_normalize_json_node_test(
            registry, &description->variety_details.json_node_test);
    case SXML_TYPE_VARIETY_FUNCTION: {
        sxml_function_type *function = &description->variety_details.function_type;
        if (function->is_any_function) {
            function->parameter_types = (sxml_sequence_type_view){
                .sequence_types = NULL, .sequence_type_count = 0};
            function->result_type = sxml_type_any_sequence;
            return true;
        }
        return sxml_type_registry_sequence_types_are_valid(
                   registry, function->parameter_types) &&
               sxml_type_registry_sequence_type_is_valid(registry,
                                                         function->result_type);
    }
    case SXML_TYPE_VARIETY_MAP: {
        sxml_map_type *map = &description->variety_details.map_type;
        if (map->is_any_map) {
            map->key_type = sxml_type_of_builtin(SXML_TYPE_XS_ANY_ATOMIC_TYPE);
            map->value_type = sxml_type_any_sequence;
            return true;
        }
        return sxml_type_registry_is_map_key_type(registry, map->key_type) &&
               sxml_type_registry_sequence_type_is_valid(registry,
                                                         map->value_type);
    }
    case SXML_TYPE_VARIETY_ARRAY: {
        sxml_array_type *array = &description->variety_details.array_type;
        if (array->is_any_array) {
            array->member_type = sxml_type_any_sequence;
            return true;
        }
        return sxml_type_registry_sequence_type_is_valid(registry,
                                                         array->member_type);
    }
    case SXML_TYPE_VARIETY_RECORD:
        return sxml_type_registry_normalize_record(
            registry, &description->variety_details.record_type);
    case SXML_TYPE_VARIETY_ENUMERATION:
        return sxml_type_registry_normalize_enumeration(
            description->variety_details.enumeration_values);
    case SXML_TYPE_VARIETY_CHOICE:
        return sxml_type_registry_normalize_choice(
            registry, description->variety_details.member_types);
    case SXML_TYPE_VARIETY_COMPLEX:
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_ATOMIC:
    case SXML_TYPE_VARIETY_LIST:
    case SXML_TYPE_VARIETY_UNION:
    case SXML_TYPE_VARIETY_ANY_ITEM:
        return false;
    }
    return false;
}

/* ------------------------------------------------------------- Interning */

/* The built-in item type equal to a normalized description, if any.
   node() is skipped: a registered node test is never node(). */
static bool sxml_type_find_builtin_item_type(
    const sxml_type_description *description, sxml_type_identifier *type) {
    for (uint32_t builtin_index = SXML_TYPE_NODE;
         builtin_index < SXML_TYPE_BUILTIN_COUNT; builtin_index += 1) {
        if (sxml_type_descriptions_equal(
                description,
                sxml_type_builtin_description((sxml_builtin_type)builtin_index))) {
            *type = (sxml_type_identifier){.index = builtin_index};
            return true;
        }
    }
    return false;
}

/* A normalized description to find among the interned item types. */
typedef struct sxml_type_structure_key {
    const sxml_type_description *description;
    uint32_t hash;
} sxml_type_structure_key;

static bool sxml_type_registry_find_structure(
    const sxml_type_registry *registry, sxml_type_structure_key key,
    size_t *entry_position) {
    const sxml_type_description *description = key.description;
    uint32_t hash = key.hash;
    const sxml_type_hash_index *index = &registry->structure_index;
    if (index->slot_count == 0) {
        return false;
    }
    size_t slot_index = sxml_type_first_slot(index->slot_count, hash);
    while (index->slots[slot_index].entry_number != 0) {
        sxml_type_slot slot = index->slots[slot_index];
        size_t position = (size_t)slot.entry_number - 1;
        if (slot.hash == hash &&
            sxml_type_descriptions_equal(description,
                                         &registry->entries[position].description)) {
            *entry_position = position;
            return true;
        }
        slot_index = sxml_type_next_slot(index->slot_count, slot_index);
    }
    return false;
}

/* Copies the record fields into the arena. */
static sxml_status sxml_type_registry_copy_fields(
    sxml_type_registry *registry, sxml_record_field_view fields,
    sxml_record_field_view *copy) {
    void *memory = NULL;
    sxml_status status = sxml_type_registry_allocate_array(
        registry,
        (sxml_type_array_request){.element_count = fields.record_field_count,
                                  .element_byte_count = sizeof(sxml_record_field),
                                  .alignment = alignof(sxml_record_field)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_record_field *copied_fields = memory;
    for (size_t field_index = 0; field_index < fields.record_field_count;
         field_index += 1) {
        copied_fields[field_index].field_type =
            fields.record_fields[field_index].field_type;
        status = sxml_type_registry_copy_string(
            registry, fields.record_fields[field_index].field_name,
            &copied_fields[field_index].field_name);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    *copy = (sxml_record_field_view){.record_fields = copied_fields,
                                     .record_field_count =
                                         fields.record_field_count};
    return SXML_STATUS_OK;
}

static sxml_status sxml_type_registry_copy_values(
    sxml_type_registry *registry, sxml_byte_view_list values,
    sxml_byte_view_list *copy) {
    void *memory = NULL;
    sxml_status status = sxml_type_registry_allocate_array(
        registry,
        (sxml_type_array_request){.element_count = values.byte_view_count,
                                  .element_byte_count = sizeof(sxml_byte_view),
                                  .alignment = alignof(sxml_byte_view)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_byte_view *copied_values = memory;
    for (size_t value_index = 0; value_index < values.byte_view_count;
         value_index += 1) {
        status = sxml_type_registry_copy_string(registry,
                                                values.byte_views[value_index],
                                                &copied_values[value_index]);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    *copy = (sxml_byte_view_list){.byte_views = copied_values,
                                  .byte_view_count = values.byte_view_count};
    return SXML_STATUS_OK;
}

/* Copies an array of plain values (identifiers, sequence types). */
typedef struct sxml_type_plain_copy {
    const void *source;
    sxml_type_array_request request;
} sxml_type_plain_copy;

static sxml_status sxml_type_registry_copy_plain(sxml_type_registry *registry,
                                                 sxml_type_plain_copy copy,
                                                 void **target) {
    sxml_status status =
        sxml_type_registry_allocate_array(registry, copy.request, target);
    if (status == SXML_STATUS_OK && *target != NULL) {
        memcpy(*target, copy.source,
               copy.request.element_count * copy.request.element_byte_count);
    }
    return status;
}

/* Replaces every view of a normalized description with an arena copy. */
static sxml_status sxml_type_registry_copy_views(
    sxml_type_registry *registry, sxml_type_description *description) {
    switch (description->variety) {
    case SXML_TYPE_VARIETY_NODE_TEST: {
        sxml_name_test *name_test =
            &description->variety_details.node_test.name_test;
        return sxml_type_registry_copy_string(registry, name_test->local_name,
                                              &name_test->local_name);
    }
    case SXML_TYPE_VARIETY_JSON_NODE_TEST: {
        sxml_json_node_test *test = &description->variety_details.json_node_test;
        return sxml_type_registry_copy_string(registry, test->key_lexical_form,
                                              &test->key_lexical_form);
    }
    case SXML_TYPE_VARIETY_FUNCTION: {
        sxml_sequence_type_view *parameters =
            &description->variety_details.function_type.parameter_types;
        void *memory = NULL;
        sxml_status status = sxml_type_registry_copy_plain(
            registry,
            (sxml_type_plain_copy){
                .source = parameters->sequence_types,
                .request = {.element_count = parameters->sequence_type_count,
                            .element_byte_count = sizeof(sxml_sequence_type),
                            .alignment = alignof(sxml_sequence_type)}},
            &memory);
        if (status == SXML_STATUS_OK) {
            parameters->sequence_types = memory;
        }
        return status;
    }
    case SXML_TYPE_VARIETY_RECORD: {
        sxml_record_field_view *fields =
            &description->variety_details.record_type.record_fields;
        return sxml_type_registry_copy_fields(registry, *fields, fields);
    }
    case SXML_TYPE_VARIETY_ENUMERATION: {
        sxml_byte_view_list *values =
            &description->variety_details.enumeration_values;
        return sxml_type_registry_copy_values(registry, *values, values);
    }
    case SXML_TYPE_VARIETY_CHOICE: {
        sxml_type_identifier_view *members =
            &description->variety_details.member_types;
        void *memory = NULL;
        sxml_status status = sxml_type_registry_copy_plain(
            registry,
            (sxml_type_plain_copy){
                .source = members->type_identifiers,
                .request = {.element_count = members->type_identifier_count,
                            .element_byte_count = sizeof(sxml_type_identifier),
                            .alignment = alignof(sxml_type_identifier)}},
            &memory);
        if (status == SXML_STATUS_OK) {
            members->type_identifiers = memory;
        }
        return status;
    }
    case SXML_TYPE_VARIETY_MAP:
    case SXML_TYPE_VARIETY_ARRAY:
    case SXML_TYPE_VARIETY_COMPLEX:
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_ATOMIC:
    case SXML_TYPE_VARIETY_LIST:
    case SXML_TYPE_VARIETY_UNION:
    case SXML_TYPE_VARIETY_ANY_ITEM:
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_OK;
}

static bool sxml_type_has_strings(const sxml_type_description *description) {
    return description->variety == SXML_TYPE_VARIETY_RECORD ||
           description->variety == SXML_TYPE_VARIETY_ENUMERATION;
}

sxml_status sxml_type_registry_register_item_type(
    sxml_type_registry *registry, const sxml_type_description *description,
    sxml_type_identifier *type) {
    sxml_type_description normal = *description;
    if (!sxml_type_registry_normalize(registry, &normal)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* "(A)" is A: a choice of one member is that member. */
    if (normal.variety == SXML_TYPE_VARIETY_CHOICE &&
        normal.variety_details.member_types.type_identifier_count == 1) {
        *type = normal.variety_details.member_types.type_identifiers[0];
        return SXML_STATUS_OK;
    }
    if (sxml_type_find_builtin_item_type(&normal, type)) {
        return SXML_STATUS_OK;
    }
    uint32_t hash = sxml_type_hash_description(&normal);
    size_t position = 0;
    if (sxml_type_registry_find_structure(
            registry,
            (sxml_type_structure_key){.description = &normal, .hash = hash},
            &position)) {
        *type = sxml_type_identifier_at(position);
        return SXML_STATUS_OK;
    }
    sxml_status status = sxml_type_registry_prepare_entry(
        registry,
        (sxml_type_index_growth){
            .index = &registry->structure_index,
            .indexed_count = registry->structural_count});
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_copy_views(registry, &normal);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_type_entry entry = {
        .description = normal,
        .hash = hash,
        .is_defined = true,
        .string_index = {.slots = NULL, .slot_count = 0},
    };
    if (sxml_type_has_strings(&normal)) {
        /* Rejects duplicate field names and enumeration values. */
        status = sxml_type_registry_index_strings(registry, &entry);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    position = registry->entry_count;
    registry->entries[position] = entry;
    registry->entry_count += 1;
    registry->structural_count += 1;
    sxml_type_hash_index_insert(
        &registry->structure_index,
        (sxml_type_slot){.entry_number = (uint32_t)(position + 1),
                         .hash = hash});
    *type = sxml_type_identifier_at(position);
    return SXML_STATUS_OK;
}

/* ------------------------------------------------ Nominative record types */

sxml_status sxml_type_registry_declare_nominative_record_type(
    sxml_type_registry *registry, sxml_name_identifier type_name,
    sxml_type_identifier *type) {
    sxml_type_identifier existing = {.index = 0};
    if (type_name.index == 0 ||
        type_name.index >=
            sxml_name_pool_name_limit(registry->configuration.name_pool) ||
        sxml_type_registry_find_type(registry, type_name, &existing)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_status status = sxml_type_registry_prepare_entry(
        registry,
        (sxml_type_index_growth){
            .index = &registry->name_index,
            .indexed_count = registry->named_count});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t position = registry->entry_count;
    registry->entries[position] = (sxml_type_entry){
        .description =
            {.variety = SXML_TYPE_VARIETY_RECORD,
             .type_name = type_name,
             .base_type = sxml_type_of_builtin(SXML_TYPE_NONE),
             .primitive_type = sxml_type_of_builtin(SXML_TYPE_NONE),
             .is_builtin = false,
             .variety_details = {.record_type =
                                     {.is_any_record = false,
                                      .is_nominative = true,
                                      .record_fields = {.record_fields = NULL,
                                                        .record_field_count =
                                                            0}}}},
        .hash = sxml_type_hash_of_name(type_name),
        .is_defined = false,
        .string_index = {.slots = NULL, .slot_count = 0},
    };
    registry->entry_count += 1;
    registry->named_count += 1;
    sxml_type_hash_index_insert(
        &registry->name_index,
        (sxml_type_slot){.entry_number = (uint32_t)(position + 1),
                         .hash = sxml_type_hash_of_name(type_name)});
    *type = sxml_type_identifier_at(position);
    return SXML_STATUS_OK;
}

sxml_status sxml_type_registry_define_record_fields(
    sxml_type_registry *registry, const sxml_record_definition *definition) {
    const sxml_type_entry *found =
        sxml_type_registry_entry_of(registry, definition->record_type);
    if (found == NULL ||
        found->description.variety != SXML_TYPE_VARIETY_RECORD ||
        !found->description.variety_details.record_type.is_nominative ||
        found->is_defined) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_type_entry *entry =
        &registry->entries[definition->record_type.index -
                           SXML_TYPE_FIRST_REGISTERED];
    sxml_record_type candidate = {.is_any_record = false,
                                  .is_nominative = false,
                                  .record_fields = definition->record_fields};
    if (!sxml_type_registry_normalize_record(registry, &candidate)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_type_entry defined = *entry;
    sxml_status status = sxml_type_registry_copy_fields(
        registry, definition->record_fields,
        &defined.description.variety_details.record_type.record_fields);
    if (status == SXML_STATUS_OK) {
        status = sxml_type_registry_index_strings(registry, &defined);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    defined.is_defined = true;
    *entry = defined;
    return SXML_STATUS_OK;
}
