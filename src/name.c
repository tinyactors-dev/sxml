/*
 * The name pool: interned namespace URIs, prefixes and expanded names
 * (include/sxml/name.h, docs/DECISIONS.md D31).
 *
 * Imitates the golden module src/memory.c. Names follow XDM 4.0, section
 * 5.6 (QNames): an expanded name is a namespace URI (possibly absent) and a
 * local name; the prefix is not part of its identity, so prefixes live in a
 * table of their own.
 *
 * Layout. Three tables, each an entry array in issue order plus an
 * open-addressing hash index (linear probing, power-of-two slot count, at
 * most three quarters full). Slots store the entry position plus one (0 is
 * an empty slot) and the entry's hash, so the index can be rebuilt without
 * reading the entries. Interned text is copied into an arena and never
 * moves, so the views the pool hands out stay valid until finalize.
 *
 * Identifiers. Namespace URI identifiers below
 * SXML_NAMESPACE_WELL_KNOWN_COUNT are the well-known namespaces, described
 * by static data and never allocated; interned URIs follow them. Prefix and
 * name identifiers are the entry position plus one.
 *
 * Budget. Every allocation the pool makes (entry arrays, index slots and
 * text blocks) is counted against maximum_byte_count. The count only grows
 * when the pool holds more: a replaced table gives its bytes back, text
 * blocks are only released at finalize.
 */
#include "name_internal.h"

#include <stdalign.h>

/* ----------------------------------------------------- Well-known names */

/* The namespace URIs of sxml_well_known_namespace, indexed by its values. */
static const sxml_byte_view sxml_well_known_namespace_uris
    [SXML_NAMESPACE_WELL_KNOWN_COUNT] = {
        [SXML_NAMESPACE_NONE] = {.bytes = NULL, .byte_count = 0},
        [SXML_NAMESPACE_XML] = SXML_BYTE_VIEW_INITIALIZER(
            "http://www.w3.org/XML/1998/namespace"),
        [SXML_NAMESPACE_XMLNS] =
            SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/2000/xmlns/"),
        [SXML_NAMESPACE_XML_SCHEMA] =
            SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/2001/XMLSchema"),
        [SXML_NAMESPACE_XML_SCHEMA_INSTANCE] = SXML_BYTE_VIEW_INITIALIZER(
            "http://www.w3.org/2001/XMLSchema-instance"),
        [SXML_NAMESPACE_FUNCTIONS] = SXML_BYTE_VIEW_INITIALIZER(
            "http://www.w3.org/2005/xpath-functions"),
        [SXML_NAMESPACE_MAP_FUNCTIONS] = SXML_BYTE_VIEW_INITIALIZER(
            "http://www.w3.org/2005/xpath-functions/map"),
        [SXML_NAMESPACE_ARRAY_FUNCTIONS] = SXML_BYTE_VIEW_INITIALIZER(
            "http://www.w3.org/2005/xpath-functions/array"),
        [SXML_NAMESPACE_MATH_FUNCTIONS] = SXML_BYTE_VIEW_INITIALIZER(
            "http://www.w3.org/2005/xpath-functions/math"),
        [SXML_NAMESPACE_ERRORS] =
            SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/2005/xqt-errors"),
        [SXML_NAMESPACE_LOCAL_FUNCTIONS] = SXML_BYTE_VIEW_INITIALIZER(
            "http://www.w3.org/2005/xquery-local-functions"),
        [SXML_NAMESPACE_SERIALIZATION] = SXML_BYTE_VIEW_INITIALIZER(
            "http://www.w3.org/2010/xslt-xquery-serialization"),
        [SXML_NAMESPACE_XSLT] =
            SXML_BYTE_VIEW_INITIALIZER("http://www.w3.org/1999/XSL/Transform"),
        [SXML_NAMESPACE_EXPATH_BINARY] =
            SXML_BYTE_VIEW_INITIALIZER("http://expath.org/ns/binary"),
        [SXML_NAMESPACE_EXPATH_FILE] =
            SXML_BYTE_VIEW_INITIALIZER("http://expath.org/ns/file"),
        [SXML_NAMESPACE_EXSLT_COMMON] =
            SXML_BYTE_VIEW_INITIALIZER("http://exslt.org/common"),
        [SXML_NAMESPACE_SXML] = SXML_BYTE_VIEW_INITIALIZER("urn:sxml"),
};

/* --------------------------------------------------------------- Tables */

/* Size of each text block taken from the allocator; see the budget note. */
#define SXML_NAME_POOL_TEXT_BLOCK_BYTE_COUNT 1024U
/* Slot count of an index when it first holds an entry; a power of two. */
#define SXML_NAME_POOL_MINIMUM_SLOT_COUNT 16U
/* Entry capacity of an entry array when it first holds an entry. */
#define SXML_NAME_POOL_MINIMUM_ENTRY_CAPACITY 8U
/* Most entries of a table, so that every identifier fits in 32 bits. */
#define SXML_NAME_POOL_MAXIMUM_ENTRY_COUNT                                   \
    ((size_t)UINT32_MAX - SXML_NAMESPACE_WELL_KNOWN_COUNT)

/* One slot of a hash index. */
typedef struct sxml_name_slot {
    /* Entry position plus one; 0 marks an empty slot. */
    uint32_t entry_number;
    uint32_t hash;
} sxml_name_slot;

typedef struct sxml_name_hash_index {
    /* slot_count slots; NULL while slot_count is 0. */
    sxml_name_slot *slots;
    /* 0 or a power of two. */
    size_t slot_count;
} sxml_name_hash_index;

/* An interned namespace URI or prefix. text is owned by the pool. */
typedef struct sxml_name_text_entry {
    sxml_byte_view text;
    uint32_t hash;
} sxml_name_text_entry;

/* Interned text without further structure: namespace URIs or prefixes. */
typedef struct sxml_name_text_table {
    sxml_name_text_entry *entries;
    size_t entry_count;
    size_t entry_capacity;
    sxml_name_hash_index index;
} sxml_name_text_table;

/* An interned expanded name. local_name is owned by the pool. */
typedef struct sxml_name_entry {
    uint32_t namespace_uri_index;
    uint32_t hash;
    sxml_byte_view local_name;
} sxml_name_entry;

typedef struct sxml_name_table {
    sxml_name_entry *entries;
    size_t entry_count;
    size_t entry_capacity;
    sxml_name_hash_index index;
} sxml_name_table;

struct sxml_name_pool {
    sxml_name_pool_configuration configuration;
    /* The allocator that sxml_name_pool_create allocated the pool with; the
       null allocator for a pool placed by sxml_name_pool_initialize. */
    sxml_allocator owning_allocator;
    /* Bytes taken from configuration.allocator and still held. */
    size_t held_byte_count;
    /* Holds the interned text; placed in the pool's own memory. */
    sxml_arena *text_arena;
    /* Interned namespace URIs other than the well-known ones. */
    sxml_name_text_table namespace_uris;
    sxml_name_text_table namespace_prefixes;
    sxml_name_table names;
};

/* ---------------------------------------------------------------- Hashing */

/* 32-bit FNV-1a: small, and good enough for names, which are short. */
#define SXML_NAME_HASH_OFFSET_BASIS 2166136261U
#define SXML_NAME_HASH_PRIME 16777619U

static uint32_t sxml_name_hash_continue(uint32_t hash, sxml_byte_view text) {
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index += 1) {
        hash ^= text.bytes[byte_index];
        hash *= SXML_NAME_HASH_PRIME;
    }
    return hash;
}

static uint32_t sxml_name_hash_text(sxml_byte_view text) {
    return sxml_name_hash_continue(SXML_NAME_HASH_OFFSET_BASIS, text);
}

static uint32_t sxml_name_hash_name(uint32_t namespace_uri_index,
                                    sxml_byte_view local_name) {
    uint8_t index_bytes[sizeof(uint32_t)];
    for (size_t byte_index = 0; byte_index < sizeof(index_bytes);
         byte_index += 1) {
        index_bytes[byte_index] =
            (uint8_t)(namespace_uri_index >> (byte_index * 8U));
    }
    uint32_t hash = sxml_name_hash_continue(
        SXML_NAME_HASH_OFFSET_BASIS,
        (sxml_byte_view){.bytes = index_bytes,
                         .byte_count = sizeof(index_bytes)});
    return sxml_name_hash_continue(hash, local_name);
}

/* ---------------------------------------------------------- Byte copies */

/*
 * Copies byte_count bytes between memory that does not overlap. A loop,
 * not memcpy, which clang-tidy's security checks reject; compilers turn
 * the loop into the same code.
 */
typedef struct sxml_name_byte_copy {
    uint8_t *target;
    const uint8_t *source;
    size_t byte_count;
} sxml_name_byte_copy;

static void sxml_name_copy_bytes(sxml_name_byte_copy copy) {
    for (size_t byte_index = 0; byte_index < copy.byte_count;
         byte_index += 1) {
        copy.target[byte_index] = copy.source[byte_index];
    }
}

/* ----------------------------------------------------- Counted allocation */

/*
 * Allocates through the pool's allocator when the budget allows it and
 * counts the bytes as held.
 */
static sxml_status sxml_name_pool_allocate(sxml_name_pool *name_pool,
                                           sxml_allocation_request request,
                                           void **memory) {
    size_t maximum = name_pool->configuration.maximum_byte_count;
    /* held_byte_count never exceeds maximum, so the subtraction is safe. */
    if (maximum != 0 &&
        request.byte_count > maximum - name_pool->held_byte_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    sxml_status status = sxml_allocator_allocate(
        name_pool->configuration.allocator, request, memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    name_pool->held_byte_count += request.byte_count;
    return SXML_STATUS_OK;
}

/* A held allocation of byte_count bytes to give back. */
typedef struct sxml_name_pool_allocation {
    void *memory;
    size_t byte_count;
} sxml_name_pool_allocation;

static void sxml_name_pool_release(sxml_name_pool *name_pool,
                                   sxml_name_pool_allocation allocation) {
    if (allocation.memory == NULL) {
        return;
    }
    sxml_allocator_release(name_pool->configuration.allocator,
                           allocation.memory);
    name_pool->held_byte_count -= allocation.byte_count;
}

/*
 * The text arena's block allocator: counted allocation from the pool. The
 * arena releases blocks only when the pool is finalized, when the count no
 * longer matters, so release does not uncount them.
 */
static sxml_status sxml_name_pool_allocate_text_block(
    void *state, sxml_allocation_request request, void **memory) {
    return sxml_name_pool_allocate(state, request, memory);
}

static void sxml_name_pool_release_text_block(void *state, void *memory) {
    const sxml_name_pool *name_pool = state;
    sxml_allocator_release(name_pool->configuration.allocator, memory);
}

static const sxml_allocator_operations sxml_name_pool_text_block_operations =
    {
        .allocate = sxml_name_pool_allocate_text_block,
        .release = sxml_name_pool_release_text_block,
};

static sxml_arena_configuration sxml_name_pool_text_arena_configuration(
    sxml_name_pool *name_pool) {
    return (sxml_arena_configuration){
        .block_allocator = {.state = name_pool,
                            .operations = &sxml_name_pool_text_block_operations},
        .block_byte_count = SXML_NAME_POOL_TEXT_BLOCK_BYTE_COUNT,
        .maximum_block_byte_count = 0,
        .initial_memory = {.bytes = NULL, .byte_count = 0},
    };
}

/* Copies text into the text arena; empty text needs no copy. */
static sxml_status sxml_name_pool_copy_text(sxml_name_pool *name_pool,
                                            sxml_byte_view text,
                                            sxml_byte_view *copy) {
    if (text.byte_count == 0) {
        *copy = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
        return SXML_STATUS_OK;
    }
    void *memory = NULL;
    sxml_status status = sxml_arena_allocate(
        name_pool->text_arena,
        (sxml_allocation_request){.byte_count = text.byte_count,
                                  .alignment = 1},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_name_copy_bytes((sxml_name_byte_copy){
        .target = memory, .source = text.bytes, .byte_count = text.byte_count});
    *copy = (sxml_byte_view){.bytes = memory, .byte_count = text.byte_count};
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Hash index */

static size_t sxml_name_hash_index_first_slot(
    const sxml_name_hash_index *index, uint32_t hash) {
    return (size_t)hash & (index->slot_count - 1);
}

static size_t sxml_name_hash_index_next_slot(
    const sxml_name_hash_index *index, size_t slot_index) {
    return (slot_index + 1) & (index->slot_count - 1);
}

/* Puts entry_number into the first free slot; the index has room. */
static void sxml_name_hash_index_insert(sxml_name_hash_index *index,
                                        sxml_name_slot slot) {
    size_t slot_index = sxml_name_hash_index_first_slot(index, slot.hash);
    while (index->slots[slot_index].entry_number != 0) {
        slot_index = sxml_name_hash_index_next_slot(index, slot_index);
    }
    index->slots[slot_index] = slot;
}

/* The growth one insertion may need from a table. */
typedef struct sxml_name_table_growth {
    /* In/out: the table's entry array and its capacity in entries. */
    void **entries;
    size_t *entry_capacity;
    size_t entry_count;
    size_t entry_byte_count;
    size_t entry_alignment;
    /* In/out: the table's hash index. */
    sxml_name_hash_index *index;
} sxml_name_table_growth;

/* Doubles the entry array when it is full; on failure changes nothing. */
static sxml_status sxml_name_pool_grow_entries(
    sxml_name_pool *name_pool, const sxml_name_table_growth *growth) {
    size_t old_capacity = *growth->entry_capacity;
    if (growth->entry_count < old_capacity) {
        return SXML_STATUS_OK;
    }
    if (growth->entry_count >= SXML_NAME_POOL_MAXIMUM_ENTRY_COUNT) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    size_t new_capacity = SXML_NAME_POOL_MINIMUM_ENTRY_CAPACITY;
    sxml_status status = SXML_STATUS_OK;
    if (old_capacity != 0) {
        status = sxml_size_multiply(old_capacity, 2, &new_capacity);
    }
    size_t new_byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(new_capacity, growth->entry_byte_count,
                                    &new_byte_count);
    }
    void *new_entries = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_allocate(
            name_pool,
            (sxml_allocation_request){.byte_count = new_byte_count,
                                      .alignment = growth->entry_alignment},
            &new_entries);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_name_copy_bytes((sxml_name_byte_copy){
        .target = new_entries,
        .source = *growth->entries,
        .byte_count = growth->entry_count * growth->entry_byte_count});
    sxml_name_pool_release(
        name_pool,
        (sxml_name_pool_allocation){
            .memory = *growth->entries,
            .byte_count = old_capacity * growth->entry_byte_count});
    *growth->entries = new_entries;
    *growth->entry_capacity = new_capacity;
    return SXML_STATUS_OK;
}

/*
 * Doubles the slot count when one more entry would fill the index beyond
 * three quarters, and rehashes from the stored hashes. On failure changes
 * nothing.
 */
static sxml_status sxml_name_pool_grow_index(
    sxml_name_pool *name_pool, const sxml_name_table_growth *growth) {
    sxml_name_hash_index *index = growth->index;
    /* Each entry and slot takes at least 8 bytes of memory, so entry_count
       and slot_count stay far below SIZE_MAX / 4 and no product wraps. */
    if ((growth->entry_count + 1) * 4 <= index->slot_count * 3) {
        return SXML_STATUS_OK;
    }
    size_t new_slot_count = SXML_NAME_POOL_MINIMUM_SLOT_COUNT;
    sxml_status status = SXML_STATUS_OK;
    if (index->slot_count != 0) {
        status = sxml_size_multiply(index->slot_count, 2, &new_slot_count);
    }
    size_t new_byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(new_slot_count, sizeof(sxml_name_slot),
                                    &new_byte_count);
    }
    void *new_memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_allocate(
            name_pool,
            (sxml_allocation_request){.byte_count = new_byte_count,
                                      .alignment = alignof(sxml_name_slot)},
            &new_memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_name_hash_index new_index = {.slots = new_memory,
                                      .slot_count = new_slot_count};
    for (size_t slot_index = 0; slot_index < new_slot_count; slot_index += 1) {
        new_index.slots[slot_index] =
            (sxml_name_slot){.entry_number = 0, .hash = 0};
    }
    for (size_t slot_index = 0; slot_index < index->slot_count;
         slot_index += 1) {
        if (index->slots[slot_index].entry_number != 0) {
            sxml_name_hash_index_insert(&new_index, index->slots[slot_index]);
        }
    }
    sxml_name_pool_release(
        name_pool,
        (sxml_name_pool_allocation){
            .memory = index->slots,
            .byte_count = index->slot_count * sizeof(sxml_name_slot)});
    *index = new_index;
    return SXML_STATUS_OK;
}

/* Makes room for one more entry in a table. */
static sxml_status sxml_name_pool_prepare_insertion(
    sxml_name_pool *name_pool, const sxml_name_table_growth *growth) {
    sxml_status status = sxml_name_pool_grow_entries(name_pool, growth);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_name_pool_grow_index(name_pool, growth);
}

/* Releases the entry array and index of a table. */
typedef struct sxml_name_table_memory {
    void *entries;
    size_t entry_byte_count;
    sxml_name_hash_index index;
} sxml_name_table_memory;

static void sxml_name_pool_release_table(sxml_name_pool *name_pool,
                                         sxml_name_table_memory table) {
    sxml_name_pool_release(name_pool,
                           (sxml_name_pool_allocation){
                               .memory = table.entries,
                               .byte_count = table.entry_byte_count});
    sxml_name_pool_release(
        name_pool,
        (sxml_name_pool_allocation){
            .memory = table.index.slots,
            .byte_count = table.index.slot_count * sizeof(sxml_name_slot)});
}

/* ----------------------------------------------------------- Text tables */

/* A lookup in a text table: the text and its hash. */
typedef struct sxml_name_text_key {
    sxml_byte_view text;
    uint32_t hash;
} sxml_name_text_key;

/* Writes the position of the entry holding key's text, if any. */
static bool sxml_name_text_table_find(const sxml_name_text_table *table,
                                      sxml_name_text_key key,
                                      size_t *entry_position) {
    const sxml_name_hash_index *index = &table->index;
    if (index->slot_count == 0) {
        return false;
    }
    size_t slot_index = sxml_name_hash_index_first_slot(index, key.hash);
    while (index->slots[slot_index].entry_number != 0) {
        sxml_name_slot slot = index->slots[slot_index];
        size_t position = (size_t)slot.entry_number - 1;
        if (slot.hash == key.hash &&
            sxml_byte_view_equals(table->entries[position].text, key.text)) {
            *entry_position = position;
            return true;
        }
        slot_index = sxml_name_hash_index_next_slot(index, slot_index);
    }
    return false;
}

/*
 * Interns text into table and writes its entry position. On failure the
 * table holds the same entries as before.
 */
typedef struct sxml_name_text_insertion {
    sxml_name_text_table *table;
    sxml_byte_view text;
} sxml_name_text_insertion;

static sxml_status sxml_name_pool_intern_text(
    sxml_name_pool *name_pool, sxml_name_text_insertion insertion,
    size_t *entry_position) {
    sxml_name_text_table *table = insertion.table;
    sxml_name_text_key key = {.text = insertion.text,
                              .hash = sxml_name_hash_text(insertion.text)};
    if (sxml_name_text_table_find(table, key, entry_position)) {
        return SXML_STATUS_OK;
    }
    void *entries = table->entries;
    sxml_status status = sxml_name_pool_prepare_insertion(
        name_pool, &(sxml_name_table_growth){
                       .entries = &entries,
                       .entry_capacity = &table->entry_capacity,
                       .entry_count = table->entry_count,
                       .entry_byte_count = sizeof(sxml_name_text_entry),
                       .entry_alignment = alignof(sxml_name_text_entry),
                       .index = &table->index,
                   });
    table->entries = entries;
    sxml_byte_view copy = {.bytes = NULL, .byte_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_copy_text(name_pool, insertion.text, &copy);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t position = table->entry_count;
    table->entries[position] =
        (sxml_name_text_entry){.text = copy, .hash = key.hash};
    table->entry_count += 1;
    sxml_name_hash_index_insert(
        &table->index,
        (sxml_name_slot){.entry_number = (uint32_t)(position + 1),
                         .hash = key.hash});
    *entry_position = position;
    return SXML_STATUS_OK;
}

static void sxml_name_pool_release_text_table(sxml_name_pool *name_pool,
                                              sxml_name_text_table *table) {
    sxml_name_pool_release_table(
        name_pool,
        (sxml_name_table_memory){
            .entries = table->entries,
            .entry_byte_count =
                table->entry_capacity * sizeof(sxml_name_text_entry),
            .index = table->index,
        });
}

/* --------------------------------------------------------- Name table */

/* Writes the position of the entry holding key's name, if any. */
static bool sxml_name_table_find(const sxml_name_table *table,
                                 const sxml_name_entry *key,
                                 size_t *entry_position) {
    const sxml_name_hash_index *index = &table->index;
    if (index->slot_count == 0) {
        return false;
    }
    size_t slot_index = sxml_name_hash_index_first_slot(index, key->hash);
    while (index->slots[slot_index].entry_number != 0) {
        sxml_name_slot slot = index->slots[slot_index];
        size_t position = (size_t)slot.entry_number - 1;
        const sxml_name_entry *entry = &table->entries[position];
        if (slot.hash == key->hash &&
            entry->namespace_uri_index == key->namespace_uri_index &&
            sxml_byte_view_equals(entry->local_name, key->local_name)) {
            *entry_position = position;
            return true;
        }
        slot_index = sxml_name_hash_index_next_slot(index, slot_index);
    }
    return false;
}

static sxml_name_entry sxml_name_key_of(const sxml_name_request *request) {
    return (sxml_name_entry){
        .namespace_uri_index = request->namespace_uri.index,
        .hash = sxml_name_hash_name(request->namespace_uri.index,
                                    request->local_name),
        .local_name = request->local_name,
    };
}

/* ------------------------------------------------------------- Lifecycle */

static bool sxml_name_pool_configuration_is_valid(
    const sxml_name_pool_configuration *configuration) {
    return !sxml_allocator_is_null(configuration->allocator);
}

/*
 * The requirements of the pool and the offset of its text arena within
 * them: the arena is placed right after the pool, in the same memory.
 */
typedef struct sxml_name_pool_layout {
    sxml_memory_requirements requirements;
    size_t text_arena_offset;
} sxml_name_pool_layout;

static sxml_status sxml_name_pool_compute_layout(
    const sxml_name_pool_configuration *configuration,
    sxml_name_pool_layout *layout) {
    if (!sxml_name_pool_configuration_is_valid(configuration)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* The arena's requirements do not depend on the block allocator's
       state, which is the pool itself and not yet placed. */
    sxml_arena_configuration arena_configuration =
        sxml_name_pool_text_arena_configuration(NULL);
    sxml_memory_requirements arena_requirements = {.byte_count = 0,
                                                   .alignment = 1};
    sxml_status status = sxml_arena_memory_requirements(&arena_configuration,
                                                        &arena_requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_memory_requirements requirements = {
        .byte_count = sizeof(struct sxml_name_pool),
        .alignment = alignof(struct sxml_name_pool),
    };
    size_t text_arena_offset = 0;
    status = sxml_memory_requirements_append(&requirements, arena_requirements,
                                             &text_arena_offset);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *layout = (sxml_name_pool_layout){.requirements = requirements,
                                      .text_arena_offset = text_arena_offset};
    return SXML_STATUS_OK;
}

sxml_status sxml_name_pool_memory_requirements(
    const sxml_name_pool_configuration *configuration,
    sxml_memory_requirements *requirements) {
    sxml_name_pool_layout layout = {
        .requirements = {.byte_count = 0, .alignment = 1},
        .text_arena_offset = 0};
    sxml_status status = sxml_name_pool_compute_layout(configuration, &layout);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *requirements = layout.requirements;
    return SXML_STATUS_OK;
}

static const sxml_name_text_table sxml_name_empty_text_table = {
    .entries = NULL,
    .entry_count = 0,
    .entry_capacity = 0,
    .index = {.slots = NULL, .slot_count = 0},
};

sxml_status sxml_name_pool_initialize(
    sxml_memory_view memory, const sxml_name_pool_configuration *configuration,
    sxml_name_pool **name_pool) {
    sxml_name_pool_layout layout = {
        .requirements = {.byte_count = 0, .alignment = 1},
        .text_arena_offset = 0};
    sxml_status status = sxml_name_pool_compute_layout(configuration, &layout);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_memory_view_check_requirements(memory, layout.requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* memory_view_check_requirements has checked size and alignment. */
    void *placement = memory.bytes;
    sxml_name_pool *new_name_pool = placement;
    *new_name_pool = (sxml_name_pool){
        .configuration = *configuration,
        .owning_allocator = {.state = NULL, .operations = NULL},
        .held_byte_count = 0,
        .text_arena = NULL,
        .namespace_uris = sxml_name_empty_text_table,
        .namespace_prefixes = sxml_name_empty_text_table,
        .names = {.entries = NULL,
                  .entry_count = 0,
                  .entry_capacity = 0,
                  .index = {.slots = NULL, .slot_count = 0}},
    };
    sxml_arena_configuration arena_configuration =
        sxml_name_pool_text_arena_configuration(new_name_pool);
    /* The layout reserved the arena's bytes within memory; the arena does
       not allocate when it is initialized. */
    status = sxml_arena_initialize(
        (sxml_memory_view){
            .bytes = memory.bytes + layout.text_arena_offset,
            .byte_count =
                layout.requirements.byte_count - layout.text_arena_offset},
        &arena_configuration, &new_name_pool->text_arena);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *name_pool = new_name_pool;
    return SXML_STATUS_OK;
}

void sxml_name_pool_finalize(sxml_name_pool *name_pool) {
    if (name_pool == NULL) {
        return;
    }
    sxml_name_pool_release_text_table(name_pool, &name_pool->namespace_uris);
    sxml_name_pool_release_text_table(name_pool,
                                      &name_pool->namespace_prefixes);
    sxml_name_pool_release_table(
        name_pool,
        (sxml_name_table_memory){
            .entries = name_pool->names.entries,
            .entry_byte_count =
                name_pool->names.entry_capacity * sizeof(sxml_name_entry),
            .index = name_pool->names.index,
        });
    sxml_arena_finalize(name_pool->text_arena);
    name_pool->namespace_uris = sxml_name_empty_text_table;
    name_pool->namespace_prefixes = sxml_name_empty_text_table;
    name_pool->names = (sxml_name_table){
        .entries = NULL,
        .entry_count = 0,
        .entry_capacity = 0,
        .index = {.slots = NULL, .slot_count = 0}};
}

sxml_status sxml_name_pool_create(
    sxml_allocator allocator, const sxml_name_pool_configuration *configuration,
    sxml_name_pool **name_pool) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_name_pool_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(allocator,
                                     (sxml_allocation_request){
                                         .byte_count = requirements.byte_count,
                                         .alignment = requirements.alignment,
                                     },
                                     &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_name_pool *new_name_pool = NULL;
    status = sxml_name_pool_initialize(
        (sxml_memory_view){
            .bytes = memory,
            .byte_count = requirements.byte_count,
        },
        configuration, &new_name_pool);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_name_pool->owning_allocator = allocator;
    *name_pool = new_name_pool;
    return SXML_STATUS_OK;
}

void sxml_name_pool_destroy(sxml_name_pool *name_pool) {
    if (name_pool == NULL) {
        return;
    }
    /* Finalizing may not touch the pool afterwards, so keep the allocator. */
    sxml_allocator owning_allocator = name_pool->owning_allocator;
    sxml_name_pool_finalize(name_pool);
    sxml_allocator_release(owning_allocator, name_pool);
}

/* ------------------------------------------------------------- Interning */

/* Writes the well-known namespace whose URI is namespace_uri, if any. */
static bool sxml_name_find_well_known_namespace(
    sxml_byte_view namespace_uri, sxml_namespace_uri_identifier *identifier) {
    for (uint32_t namespace_index = 0;
         namespace_index < SXML_NAMESPACE_WELL_KNOWN_COUNT;
         namespace_index += 1) {
        if (sxml_byte_view_equals(
                sxml_well_known_namespace_uris[namespace_index],
                namespace_uri)) {
            *identifier =
                (sxml_namespace_uri_identifier){.index = namespace_index};
            return true;
        }
    }
    return false;
}

static sxml_namespace_uri_identifier sxml_name_namespace_uri_at(
    size_t entry_position) {
    return (sxml_namespace_uri_identifier){
        .index = (uint32_t)(entry_position + SXML_NAMESPACE_WELL_KNOWN_COUNT)};
}

sxml_status sxml_name_pool_intern_namespace_uri(
    sxml_name_pool *name_pool, sxml_byte_view namespace_uri,
    sxml_namespace_uri_identifier *identifier) {
    /* The empty text is in the well-known table as SXML_NAMESPACE_NONE. */
    if (sxml_name_find_well_known_namespace(namespace_uri, identifier)) {
        return SXML_STATUS_OK;
    }
    size_t entry_position = 0;
    sxml_status status = sxml_name_pool_intern_text(
        name_pool,
        (sxml_name_text_insertion){.table = &name_pool->namespace_uris,
                                   .text = namespace_uri},
        &entry_position);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *identifier = sxml_name_namespace_uri_at(entry_position);
    return SXML_STATUS_OK;
}

sxml_status sxml_name_pool_intern_namespace_prefix(
    sxml_name_pool *name_pool, sxml_byte_view namespace_prefix,
    sxml_namespace_prefix_identifier *identifier) {
    if (namespace_prefix.byte_count == 0) {
        *identifier = (sxml_namespace_prefix_identifier){.index = 0};
        return SXML_STATUS_OK;
    }
    size_t entry_position = 0;
    sxml_status status = sxml_name_pool_intern_text(
        name_pool,
        (sxml_name_text_insertion){.table = &name_pool->namespace_prefixes,
                                   .text = namespace_prefix},
        &entry_position);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *identifier = (sxml_namespace_prefix_identifier){
        .index = (uint32_t)(entry_position + 1)};
    return SXML_STATUS_OK;
}

static bool sxml_name_pool_issued_namespace_uri(
    const sxml_name_pool *name_pool,
    sxml_namespace_uri_identifier identifier) {
    return identifier.index < sxml_name_pool_namespace_uri_limit(name_pool);
}

sxml_status sxml_name_pool_intern_name(sxml_name_pool *name_pool,
                                       const sxml_name_request *request,
                                       sxml_name_identifier *identifier) {
    if (request->local_name.byte_count == 0 ||
        !sxml_name_pool_issued_namespace_uri(name_pool,
                                             request->namespace_uri)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_name_table *table = &name_pool->names;
    sxml_name_entry key = sxml_name_key_of(request);
    size_t entry_position = 0;
    if (!sxml_name_table_find(table, &key, &entry_position)) {
        void *entries = table->entries;
        sxml_status status = sxml_name_pool_prepare_insertion(
            name_pool, &(sxml_name_table_growth){
                           .entries = &entries,
                           .entry_capacity = &table->entry_capacity,
                           .entry_count = table->entry_count,
                           .entry_byte_count = sizeof(sxml_name_entry),
                           .entry_alignment = alignof(sxml_name_entry),
                           .index = &table->index,
                       });
        table->entries = entries;
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_copy_text(name_pool, request->local_name,
                                              &key.local_name);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        entry_position = table->entry_count;
        table->entries[entry_position] = key;
        table->entry_count += 1;
        sxml_name_hash_index_insert(
            &table->index,
            (sxml_name_slot){.entry_number = (uint32_t)(entry_position + 1),
                             .hash = key.hash});
    }
    *identifier =
        (sxml_name_identifier){.index = (uint32_t)(entry_position + 1)};
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------------- Lookup */

bool sxml_name_pool_find_name(const sxml_name_pool *name_pool,
                              const sxml_name_request *request,
                              sxml_name_identifier *identifier) {
    /* An unissued namespace URI or an empty local name names nothing the
       pool holds. */
    if (request->local_name.byte_count == 0 ||
        !sxml_name_pool_issued_namespace_uri(name_pool,
                                             request->namespace_uri)) {
        return false;
    }
    sxml_name_entry key = sxml_name_key_of(request);
    size_t entry_position = 0;
    if (!sxml_name_table_find(&name_pool->names, &key, &entry_position)) {
        return false;
    }
    *identifier =
        (sxml_name_identifier){.index = (uint32_t)(entry_position + 1)};
    return true;
}

bool sxml_name_pool_find_namespace_uri(
    const sxml_name_pool *name_pool, sxml_byte_view namespace_uri,
    sxml_namespace_uri_identifier *identifier) {
    if (sxml_name_find_well_known_namespace(namespace_uri, identifier)) {
        return true;
    }
    size_t entry_position = 0;
    if (!sxml_name_text_table_find(
            &name_pool->namespace_uris,
            (sxml_name_text_key){.text = namespace_uri,
                                 .hash = sxml_name_hash_text(namespace_uri)},
            &entry_position)) {
        return false;
    }
    *identifier = sxml_name_namespace_uri_at(entry_position);
    return true;
}

sxml_status sxml_name_pool_get_namespace_uri(
    const sxml_name_pool *name_pool, sxml_namespace_uri_identifier identifier,
    sxml_byte_view *namespace_uri) {
    if (identifier.index < SXML_NAMESPACE_WELL_KNOWN_COUNT) {
        *namespace_uri = sxml_well_known_namespace_uris[identifier.index];
        return SXML_STATUS_OK;
    }
    size_t entry_position =
        (size_t)identifier.index - SXML_NAMESPACE_WELL_KNOWN_COUNT;
    if (entry_position >= name_pool->namespace_uris.entry_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    *namespace_uri = name_pool->namespace_uris.entries[entry_position].text;
    return SXML_STATUS_OK;
}

sxml_status sxml_name_pool_get_namespace_prefix(
    const sxml_name_pool *name_pool,
    sxml_namespace_prefix_identifier identifier,
    sxml_byte_view *namespace_prefix) {
    if (identifier.index == 0) {
        *namespace_prefix = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
        return SXML_STATUS_OK;
    }
    size_t entry_position = (size_t)identifier.index - 1;
    if (entry_position >= name_pool->namespace_prefixes.entry_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    *namespace_prefix =
        name_pool->namespace_prefixes.entries[entry_position].text;
    return SXML_STATUS_OK;
}

sxml_status sxml_name_pool_get_name_parts(const sxml_name_pool *name_pool,
                                          sxml_name_identifier identifier,
                                          sxml_name_parts *parts) {
    if (identifier.index == 0 ||
        (size_t)identifier.index - 1 >= name_pool->names.entry_count) {
        return SXML_STATUS_INDEX_OUT_OF_RANGE;
    }
    const sxml_name_entry *entry =
        &name_pool->names.entries[identifier.index - 1];
    *parts = (sxml_name_parts){
        .namespace_uri = {.index = entry->namespace_uri_index},
        .local_name = entry->local_name,
    };
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------ Internal queries */

uint32_t sxml_name_pool_namespace_uri_limit(const sxml_name_pool *name_pool) {
    return (uint32_t)(name_pool->namespace_uris.entry_count +
                      SXML_NAMESPACE_WELL_KNOWN_COUNT);
}

uint32_t sxml_name_pool_namespace_prefix_limit(
    const sxml_name_pool *name_pool) {
    return (uint32_t)(name_pool->namespace_prefixes.entry_count + 1);
}

uint32_t sxml_name_pool_name_limit(const sxml_name_pool *name_pool) {
    return (uint32_t)(name_pool->names.entry_count + 1);
}
