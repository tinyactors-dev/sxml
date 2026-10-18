/*
 * JSON texts (RFC 8259; F&O 4.0, section 17.4; docs/DECISIONS.md, D82):
 * parsing with grammars/json into XDM values or the XML representation of
 * JSON, and writing that representation back as a JSON text. The
 * interface and its rules are in src/json_internal.h.
 *
 * Parsing has two steps. tree-sitter builds the concrete syntax tree
 * (D12); any ERROR or MISSING node is err:FOJS0001. Then one loop walks
 * the tree with a cursor and turns it into JSON events (start and end of
 * objects and arrays, keys, scalars), which a JSON sink receives: the
 * value sink makes maps, arrays and atomic items on explicit stacks, the
 * XML sink sends tree builder events. No function calls itself (D71): the
 * nesting of the text costs heap, never C stack.
 *
 * Golden file: src/xml.c (tree-sitter walk, error location) after
 * src/memory.c (checked sizes, allocator use).
 */
#include "json_internal.h"

#include "cast_internal.h"
#include "sxml/name.h"
#include "sxml/string.h"
#include "xdm_internal.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <string.h>
#include <tree_sitter/api.h>

/* Defined by the generated parser of grammars/json (D12). */
const TSLanguage *
tree_sitter_json(void); // NOLINT(readability-identifier-naming)

/* ------------------------------------------------------- Static errors */

static const sxml_static_error sxml_json_duplicate_key_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOJS0003"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A JSON object has two members with the same key and the duplicates "
        "option is \"reject\""),
};

static const sxml_static_error sxml_json_invalid_representation_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOJS0006"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The node is not the XML representation of JSON (F&O 4.0, section "
        "17.4.2)"),
};

static const sxml_static_error sxml_json_duplicate_representation_key_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOJS0006"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Two children of a map element have the same key"),
};

static const sxml_static_error sxml_json_invalid_escape_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOJS0007"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A string or key in escaped form holds an invalid JSON escape "
        "sequence"),
};

/* --------------------------------------------------------- Byte lists */

/* Bytes that grow through an allocator (there is no reallocate, D22). */
typedef struct sxml_json_bytes {
    sxml_allocator allocator;
    uint8_t *bytes;
    size_t byte_count;
    size_t byte_capacity;
} sxml_json_bytes;

static sxml_status sxml_json_bytes_reserve(sxml_json_bytes *list,
                                           size_t additional_byte_count) {
    size_t required = 0;
    sxml_status status =
        sxml_size_add(list->byte_count, additional_byte_count, &required);
    if (status != SXML_STATUS_OK || required <= list->byte_capacity) {
        return status;
    }
    size_t capacity = list->byte_capacity < 64 ? 64 : list->byte_capacity;
    while (capacity < required) {
        status = sxml_size_multiply(capacity, 2, &capacity);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(
        list->allocator,
        (sxml_allocation_request){.byte_count = capacity, .alignment = 1},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (list->byte_count > 0) {
        memcpy(memory, list->bytes, list->byte_count);
    }
    sxml_allocator_release(list->allocator, list->bytes);
    list->bytes = memory;
    list->byte_capacity = capacity;
    return SXML_STATUS_OK;
}

static sxml_status sxml_json_bytes_append(sxml_json_bytes *list,
                                          sxml_byte_view piece) {
    if (piece.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    const sxml_status status = sxml_json_bytes_reserve(list, piece.byte_count);
    if (status == SXML_STATUS_OK) {
        memcpy(list->bytes + list->byte_count, piece.bytes, piece.byte_count);
        list->byte_count += piece.byte_count;
    }
    return status;
}

static sxml_byte_view sxml_json_bytes_view(const sxml_json_bytes *list) {
    return (sxml_byte_view){.bytes = list->bytes, .byte_count = list->byte_count};
}

static void sxml_json_bytes_release(sxml_json_bytes *list) {
    sxml_allocator_release(list->allocator, list->bytes);
    list->bytes = NULL;
    list->byte_count = 0;
    list->byte_capacity = 0;
}

/* Elements of one size that grow through an allocator: the stacks. */
typedef struct sxml_json_list {
    sxml_allocator allocator;
    void *elements;
    size_t element_count;
    size_t element_capacity;
    size_t element_byte_count;
    size_t element_alignment;
} sxml_json_list;

/* Makes room for one more element and writes its address to *element;
   element_count is not changed. */
static sxml_status sxml_json_list_reserve(sxml_json_list *list, void **element) {
    if (list->element_count == list->element_capacity) {
        size_t capacity = list->element_capacity < 16 ? 16 : list->element_capacity;
        sxml_status status = SXML_STATUS_OK;
        if (list->element_capacity >= 16) {
            status = sxml_size_multiply(capacity, 2, &capacity);
        }
        size_t byte_count = 0;
        if (status == SXML_STATUS_OK) {
            status = sxml_size_multiply(capacity, list->element_byte_count, &byte_count);
        }
        void *memory = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                list->allocator,
                (sxml_allocation_request){.byte_count = byte_count,
                                          .alignment = list->element_alignment},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (list->element_count > 0) {
            memcpy(memory, list->elements, list->element_count * list->element_byte_count);
        }
        sxml_allocator_release(list->allocator, list->elements);
        list->elements = memory;
        list->element_capacity = capacity;
    }
    *element = (uint8_t *)list->elements + list->element_count * list->element_byte_count;
    return SXML_STATUS_OK;
}

static void *sxml_json_list_at(const sxml_json_list *list, size_t element_index) {
    if (list->elements == NULL) {
        return NULL;
    }
    return (uint8_t *)list->elements + element_index * list->element_byte_count;
}

static void sxml_json_list_release(sxml_json_list *list) {
    sxml_allocator_release(list->allocator, list->elements);
    list->elements = NULL;
    list->element_count = 0;
    list->element_capacity = 0;
}

#define SXML_JSON_LIST(allocator_value, element_type)                         \
    ((sxml_json_list){.allocator = (allocator_value),                         \
                      .elements = NULL,                                       \
                      .element_count = 0,                                     \
                      .element_capacity = 0,                                  \
                      .element_byte_count = sizeof(element_type),             \
                      .element_alignment = alignof(element_type)})

/* ------------------------------------------------------------ Codepoints */

/* F&O 4.0, section 17.4.3: the characters xs:string holds (XML 1.1). */
static bool sxml_json_is_permitted_codepoint(uint32_t codepoint) {
    return (codepoint >= 0x1U && codepoint <= 0xD7FFU) ||
           (codepoint >= 0xE000U && codepoint <= 0xFFFDU) ||
           (codepoint >= 0x10000U && codepoint <= 0x10FFFFU);
}

/* F&O 4.0, section 17.4.4, option escape: the characters written as JSON
   escape sequences in escaped form. */
static bool sxml_json_is_special_codepoint(uint32_t codepoint) {
    return codepoint <= 0x1FU || (codepoint >= 0x7FU && codepoint <= 0x9FU) ||
           codepoint == '\\' || !sxml_json_is_permitted_codepoint(codepoint);
}

/* UTF-8 of codepoint (surrogates included, as generalized UTF-8, for keys
   compared in xml-to-json) into output; writes the byte count. */
static size_t sxml_json_encode_utf8(uint32_t codepoint, uint8_t output[4]) {
    if (codepoint < 0x80U) {
        output[0] = (uint8_t)codepoint;
        return 1;
    }
    if (codepoint < 0x800U) {
        output[0] = (uint8_t)(0xC0U | (codepoint >> 6U));
        output[1] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        return 2;
    }
    if (codepoint < 0x10000U) {
        output[0] = (uint8_t)(0xE0U | (codepoint >> 12U));
        output[1] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
        output[2] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        return 3;
    }
    output[0] = (uint8_t)(0xF0U | (codepoint >> 18U));
    output[1] = (uint8_t)(0x80U | ((codepoint >> 12U) & 0x3FU));
    output[2] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
    output[3] = (uint8_t)(0x80U | (codepoint & 0x3FU));
    return 4;
}

/* A codepoint read from well-formed UTF-8. */
typedef struct sxml_json_decoded_codepoint {
    uint32_t codepoint;
    size_t byte_count;
} sxml_json_decoded_codepoint;

/* Reads the codepoint at text.bytes[0]; text is well-formed UTF-8 and not
   empty. */
static sxml_json_decoded_codepoint sxml_json_decode_utf8(sxml_byte_view text) {
    const uint8_t lead = text.bytes[0];
    if (lead < 0x80U || text.byte_count < 2) {
        return (sxml_json_decoded_codepoint){.codepoint = lead, .byte_count = 1};
    }
    if (lead < 0xE0U) {
        return (sxml_json_decoded_codepoint){
            .codepoint = ((uint32_t)(lead & 0x1FU) << 6U) | (text.bytes[1] & 0x3FU),
            .byte_count = 2};
    }
    if (lead < 0xF0U || text.byte_count < 4) {
        return (sxml_json_decoded_codepoint){
            .codepoint = ((uint32_t)(lead & 0x0FU) << 12U) |
                         ((uint32_t)(text.bytes[1] & 0x3FU) << 6U) |
                         (text.bytes[2] & 0x3FU),
            .byte_count = 3};
    }
    return (sxml_json_decoded_codepoint){
        .codepoint = ((uint32_t)(lead & 0x07U) << 18U) |
                     ((uint32_t)(text.bytes[1] & 0x3FU) << 12U) |
                     ((uint32_t)(text.bytes[2] & 0x3FU) << 6U) | (text.bytes[3] & 0x3FU),
        .byte_count = 4};
}

static int sxml_json_hex_digit_value(uint8_t character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

/* The value of four hex digits at text; -1 when they are not four hex
   digits. */
static int32_t sxml_json_read_hex4(sxml_byte_view text) {
    if (text.byte_count < 4) {
        return -1;
    }
    int32_t value = 0;
    for (size_t digit_index = 0; digit_index < 4; digit_index += 1) {
        const int digit = sxml_json_hex_digit_value(text.bytes[digit_index]);
        if (digit < 0) {
            return -1;
        }
        value = value * 16 + digit;
    }
    return value;
}

/* Appends \uHHHH, upper-case (F&O 4.0, sections 17.4.4 and 17.4.7). */
static sxml_status sxml_json_append_unicode_escape(sxml_json_bytes *output,
                                                   uint32_t codepoint) {
    static const char hex_digits[] = "0123456789ABCDEF";
    const uint8_t escape[6] = {
        '\\',
        'u',
        (uint8_t)hex_digits[(codepoint >> 12U) & 0xFU],
        (uint8_t)hex_digits[(codepoint >> 8U) & 0xFU],
        (uint8_t)hex_digits[(codepoint >> 4U) & 0xFU],
        (uint8_t)hex_digits[codepoint & 0xFU],
    };
    return sxml_json_bytes_append(output,
                                  (sxml_byte_view){.bytes = escape, .byte_count = 6});
}

static sxml_status sxml_json_append_codepoint(sxml_json_bytes *output,
                                              uint32_t codepoint) {
    uint8_t encoded[4];
    const size_t byte_count = sxml_json_encode_utf8(codepoint, encoded);
    return sxml_json_bytes_append(
        output, (sxml_byte_view){.bytes = encoded, .byte_count = byte_count});
}

/* The two-character escape of codepoint ("n" for U+000A), 0 when it has
   none (RFC 8259, section 7). The quote and the solidus are left out:
   they are not special characters. */
static uint8_t sxml_json_short_escape(uint32_t codepoint) {
    switch (codepoint) {
    case 0x08U:
        return 'b';
    case 0x0CU:
        return 'f';
    case 0x0AU:
        return 'n';
    case 0x0DU:
        return 'r';
    case 0x09U:
        return 't';
    case '\\':
        return '\\';
    default:
        return 0;
    }
}

/* ------------------------------------------------------------ Key index */

/*
 * The keys of the objects (or map elements) open at once, to find
 * duplicates in O(1) per key: an open-addressing table with linear
 * probing. Keys of an object are removed when it closes, in the reverse
 * order of insertion, which leaves every probe sequence of older keys
 * intact (an older key never probes through a slot filled later).
 */
typedef struct sxml_json_key_slot {
    uint64_t hash;
    uint64_t object_number;
    size_t key_offset;
    size_t key_byte_count;
    bool is_used;
} sxml_json_key_slot;

typedef struct sxml_json_key_index {
    sxml_json_key_slot *slots;
    size_t slot_count;
    /* Slots filled, in insertion order (size_t). */
    sxml_json_list inserted_slots;
    /* The bytes of the keys. */
    sxml_json_bytes key_bytes;
} sxml_json_key_index;

/* Where an object's keys start, to remove them when it closes. */
typedef struct sxml_json_key_mark {
    size_t inserted_count;
    size_t key_byte_count;
} sxml_json_key_mark;

/* A key of one object to look up or insert. */
typedef struct sxml_json_key_query {
    uint64_t object_number;
    sxml_byte_view key;
} sxml_json_key_query;

typedef struct sxml_json_key_outcome {
    bool is_duplicate;
} sxml_json_key_outcome;

static sxml_json_key_index sxml_json_key_index_make(sxml_allocator allocator) {
    return (sxml_json_key_index){
        .slots = NULL,
        .slot_count = 0,
        .inserted_slots = SXML_JSON_LIST(allocator, size_t),
        .key_bytes = {.allocator = allocator, .bytes = NULL, .byte_count = 0,
                      .byte_capacity = 0},
    };
}

static void sxml_json_key_index_release(sxml_json_key_index *index) {
    sxml_allocator_release(index->key_bytes.allocator, index->slots);
    index->slots = NULL;
    index->slot_count = 0;
    sxml_json_list_release(&index->inserted_slots);
    sxml_json_bytes_release(&index->key_bytes);
}

/* FNV-1a over the object number and the key. */
static uint64_t sxml_json_key_hash(const sxml_json_key_query *query) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t shift = 0; shift < 64; shift += 8) {
        hash = (hash ^ ((query->object_number >> shift) & 0xFFU)) * 1099511628211ULL;
    }
    for (size_t byte_index = 0; byte_index < query->key.byte_count; byte_index += 1) {
        hash = (hash ^ query->key.bytes[byte_index]) * 1099511628211ULL;
    }
    return hash;
}

static bool sxml_json_key_slot_matches(const sxml_json_key_index *index,
                                       const sxml_json_key_slot *slot,
                                       const sxml_json_key_query *query) {
    return slot->object_number == query->object_number &&
           slot->key_byte_count == query->key.byte_count &&
           (query->key.byte_count == 0 ||
            memcmp(index->key_bytes.bytes + slot->key_offset, query->key.bytes,
                   query->key.byte_count) == 0);
}

/* The slot where hash is found or would go. */
static size_t sxml_json_key_probe(const sxml_json_key_index *index,
                                  const sxml_json_key_query *query, uint64_t hash) {
    size_t slot_index = (size_t)(hash & (index->slot_count - 1));
    while (index->slots[slot_index].is_used &&
           !(index->slots[slot_index].hash == hash &&
             sxml_json_key_slot_matches(index, &index->slots[slot_index], query))) {
        slot_index = (slot_index + 1) & (index->slot_count - 1);
    }
    return slot_index;
}

/* Doubles the table, inserting the keys again in their insertion order so
   removal in reverse order stays valid. */
static sxml_status sxml_json_key_index_grow(sxml_json_key_index *index) {
    size_t slot_count = index->slot_count == 0 ? 64 : 0;
    sxml_status status = SXML_STATUS_OK;
    if (index->slot_count != 0) {
        status = sxml_size_multiply(index->slot_count, 2, &slot_count);
    }
    size_t byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(slot_count, sizeof(sxml_json_key_slot), &byte_count);
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            index->key_bytes.allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(sxml_json_key_slot)},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(memory, 0, byte_count);
    sxml_json_key_slot *old_slots = index->slots;
    index->slots = memory;
    index->slot_count = slot_count;
    for (size_t insertion_index = 0;
         insertion_index < index->inserted_slots.element_count; insertion_index += 1) {
        size_t *slot_number = sxml_json_list_at(&index->inserted_slots, insertion_index);
        const sxml_json_key_slot old_slot = old_slots[*slot_number];
        size_t slot_index = (size_t)(old_slot.hash & (slot_count - 1));
        while (index->slots[slot_index].is_used) {
            slot_index = (slot_index + 1) & (slot_count - 1);
        }
        index->slots[slot_index] = old_slot;
        *slot_number = slot_index;
    }
    sxml_allocator_release(index->key_bytes.allocator, old_slots);
    return SXML_STATUS_OK;
}

/* Inserts query's key unless the object has it already, and says which. */
static sxml_status sxml_json_key_index_insert(sxml_json_key_index *index,
                                              const sxml_json_key_query *query,
                                              sxml_json_key_outcome *outcome) {
    sxml_status status = SXML_STATUS_OK;
    if (index->slot_count == 0 ||
        index->inserted_slots.element_count + 1 > index->slot_count / 2) {
        status = sxml_json_key_index_grow(index);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint64_t hash = sxml_json_key_hash(query);
    const size_t slot_index = sxml_json_key_probe(index, query, hash);
    if (index->slots[slot_index].is_used) {
        outcome->is_duplicate = true;
        return SXML_STATUS_OK;
    }
    void *inserted = NULL;
    const size_t key_offset = index->key_bytes.byte_count;
    status = sxml_json_list_reserve(&index->inserted_slots, &inserted);
    if (status == SXML_STATUS_OK) {
        status = sxml_json_bytes_append(&index->key_bytes, query->key);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *(size_t *)inserted = slot_index;
    index->inserted_slots.element_count += 1;
    index->slots[slot_index] = (sxml_json_key_slot){.hash = hash,
                                                    .object_number = query->object_number,
                                                    .key_offset = key_offset,
                                                    .key_byte_count = query->key.byte_count,
                                                    .is_used = true};
    outcome->is_duplicate = false;
    return SXML_STATUS_OK;
}

static sxml_json_key_mark sxml_json_key_index_mark(const sxml_json_key_index *index) {
    return (sxml_json_key_mark){.inserted_count = index->inserted_slots.element_count,
                                .key_byte_count = index->key_bytes.byte_count};
}

/* Removes the keys inserted since mark, newest first. */
static void sxml_json_key_index_restore(sxml_json_key_index *index,
                                        sxml_json_key_mark mark) {
    while (index->inserted_slots.element_count > mark.inserted_count) {
        index->inserted_slots.element_count -= 1;
        const size_t *slot_number = sxml_json_list_at(&index->inserted_slots,
                                                      index->inserted_slots.element_count);
        index->slots[*slot_number].is_used = false;
    }
    index->key_bytes.byte_count = mark.key_byte_count;
}

/* ------------------------------------------------------------ JSON events */

typedef enum sxml_json_event_kind {
    SXML_JSON_EVENT_KIND_START_OBJECT,
    SXML_JSON_EVENT_KIND_END_OBJECT,
    SXML_JSON_EVENT_KIND_START_ARRAY,
    SXML_JSON_EVENT_KIND_END_ARRAY,
    /* The key of the member whose value comes next. */
    SXML_JSON_EVENT_KIND_KEY,
    SXML_JSON_EVENT_KIND_STRING,
    SXML_JSON_EVENT_KIND_NUMBER,
    SXML_JSON_EVENT_KIND_TRUE,
    SXML_JSON_EVENT_KIND_FALSE,
    SXML_JSON_EVENT_KIND_NULL,
} sxml_json_event_kind;

/* One JSON event. text is borrowed for the receive call. */
typedef struct sxml_json_event {
    sxml_json_event_kind event_kind;
    /* KEY and STRING: the decoded (or escaped-form) text; NUMBER: the
       lexical form. */
    sxml_byte_view text;
    /* KEY and STRING in escaped form: text holds a backslash. */
    bool has_backslash;
} sxml_json_event;

/* What a sink answers to an event. */
typedef struct sxml_json_reply {
    /* After KEY: leave out the member's value (a duplicate dropped by
       "use-first"); the walk sends no events for it. */
    bool skips_value;
} sxml_json_reply;

typedef struct sxml_json_sink_operations {
    sxml_status (*receive)(void *state, const sxml_json_event *event,
                           sxml_json_reply *reply);
} sxml_json_sink_operations;

/* The receiver of the walk's events. Its receive never starts a walk, so
   the walk and the sinks do not recurse through each other (D71). */
typedef struct sxml_json_sink {
    void *state;
    const sxml_json_sink_operations *operations;
} sxml_json_sink;

/* ------------------------------------------------------------- The walk */

/* Symbols and fields of grammars/json, looked up once per parse. */
typedef struct sxml_json_grammar {
    TSSymbol object_symbol;
    TSSymbol array_symbol;
    TSSymbol member_symbol;
    TSSymbol string_symbol;
    TSSymbol number_symbol;
    TSSymbol true_symbol;
    TSSymbol false_symbol;
    TSSymbol null_symbol;
    TSSymbol escape_symbol;
    TSSymbol unescaped_symbol;
    TSFieldId key_field;
} sxml_json_grammar;

/* The tree-sitter API takes the name as a C string. */
// ast-grep-ignore: pointer-parameter-without-length
static TSSymbol sxml_json_symbol(const TSLanguage *language, const char *name) {
    return ts_language_symbol_for_name(language, name, (uint32_t)strlen(name), true);
}

static sxml_json_grammar sxml_json_load_grammar(const TSLanguage *language) {
    return (sxml_json_grammar){
        .object_symbol = sxml_json_symbol(language, "object"),
        .array_symbol = sxml_json_symbol(language, "array"),
        .member_symbol = sxml_json_symbol(language, "member"),
        .string_symbol = sxml_json_symbol(language, "string"),
        .number_symbol = sxml_json_symbol(language, "number"),
        .true_symbol = sxml_json_symbol(language, "true"),
        .false_symbol = sxml_json_symbol(language, "false"),
        .null_symbol = sxml_json_symbol(language, "null"),
        .escape_symbol = sxml_json_symbol(language, "escape_sequence"),
        .unescaped_symbol = sxml_json_symbol(language, "unescaped_text"),
        .key_field = ts_language_field_id_for_name(language, "key", 3),
    };
}

/* The state of one walk over a concrete syntax tree. */
typedef struct sxml_json_walk {
    sxml_item_heap *item_heap;
    const sxml_json_text_request *request;
    sxml_byte_view json_text;
    sxml_json_grammar grammar;
    sxml_json_sink sink;
    /* The text of the string being decoded, reused for every string. */
    sxml_json_bytes decoded;
    uint32_t depth;
    /* The string being decoded holds a backslash in escaped form. */
    bool has_backslash;
    /* The next value node is a dropped duplicate's. */
    bool skips_next_value;
} sxml_json_walk;

/* The text of a node. */
static sxml_byte_view sxml_json_node_text(const sxml_json_walk *walk, TSNode node) {
    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    return (sxml_byte_view){.bytes = walk->json_text.bytes + start,
                            .byte_count = end - start};
}

/* Appends the fallback for a codepoint that is not permitted: the
   replacement given for it, or U+FFFD (F&O 4.0, section 17.4.4, option
   fallback). */
static sxml_status sxml_json_append_fallback(sxml_json_walk *walk, uint32_t codepoint) {
    const sxml_json_replacement_view replacements = walk->request->options.replacements;
    for (size_t replacement_index = 0; replacement_index < replacements.replacement_count;
         replacement_index += 1) {
        const sxml_json_replacement *replacement =
            &replacements.replacements[replacement_index];
        if (replacement->codepoint == codepoint) {
            return sxml_json_bytes_append(&walk->decoded, replacement->replacement_text);
        }
    }
    return sxml_json_append_codepoint(&walk->decoded, 0xFFFDU);
}

/* Appends one decoded codepoint of an escape sequence (F&O 4.0, section
   17.4.4, options escape and fallback). */
static sxml_status sxml_json_append_escaped_codepoint(sxml_json_walk *walk,
                                                      uint32_t codepoint) {
    if (!walk->request->options.keeps_escaped_form) {
        if (!sxml_json_is_permitted_codepoint(codepoint)) {
            return sxml_json_append_fallback(walk, codepoint);
        }
        return sxml_json_append_codepoint(&walk->decoded, codepoint);
    }
    if (!sxml_json_is_special_codepoint(codepoint)) {
        return sxml_json_append_codepoint(&walk->decoded, codepoint);
    }
    walk->has_backslash = true;
    const uint8_t short_escape = sxml_json_short_escape(codepoint);
    if (short_escape != 0) {
        const uint8_t escape[2] = {'\\', short_escape};
        return sxml_json_bytes_append(&walk->decoded,
                                      (sxml_byte_view){.bytes = escape, .byte_count = 2});
    }
    return sxml_json_append_unicode_escape(&walk->decoded, codepoint);
}

/* Appends unescaped text; in escaped form the controls U+007F to U+009F
   become escape sequences too. */
static sxml_status sxml_json_append_unescaped_text(sxml_json_walk *walk,
                                                   sxml_byte_view text) {
    if (!walk->request->options.keeps_escaped_form) {
        return sxml_json_bytes_append(&walk->decoded, text);
    }
    size_t run_start = 0;
    size_t offset = 0;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && offset < text.byte_count) {
        const sxml_json_decoded_codepoint decoded = sxml_json_decode_utf8(
            (sxml_byte_view){.bytes = text.bytes + offset,
                             .byte_count = text.byte_count - offset});
        if (sxml_json_is_special_codepoint(decoded.codepoint)) {
            status = sxml_json_bytes_append(
                &walk->decoded, (sxml_byte_view){.bytes = text.bytes + run_start,
                                                 .byte_count = offset - run_start});
            if (status == SXML_STATUS_OK) {
                status = sxml_json_append_escaped_codepoint(walk, decoded.codepoint);
            }
            run_start = offset + decoded.byte_count;
        }
        offset += decoded.byte_count;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_json_bytes_append(
            &walk->decoded, (sxml_byte_view){.bytes = text.bytes + run_start,
                                             .byte_count = text.byte_count - run_start});
    }
    return status;
}

/* The codepoint of an escape_sequence node (RFC 8259, section 7). */
static uint32_t sxml_json_escape_codepoint(sxml_byte_view escape) {
    switch (escape.bytes[1]) {
    case 'b':
        return 0x08U;
    case 'f':
        return 0x0CU;
    case 'n':
        return 0x0AU;
    case 'r':
        return 0x0DU;
    case 't':
        return 0x09U;
    case 'u':
        return (uint32_t)sxml_json_read_hex4((sxml_byte_view){
            .bytes = escape.bytes + 2, .byte_count = escape.byte_count - 2});
    default:
        /* '"', '\\' and '/' stand for themselves. */
        return escape.bytes[1];
    }
}

static bool sxml_json_is_high_surrogate(uint32_t codepoint) {
    return codepoint >= 0xD800U && codepoint <= 0xDBFFU;
}

static bool sxml_json_is_low_surrogate(uint32_t codepoint) {
    return codepoint >= 0xDC00U && codepoint <= 0xDFFFU;
}

/* The codepoint of the escape "\\uXXXX" at offset of text, or -1 when
   there is none there. */
static int32_t sxml_json_unicode_escape_at(sxml_byte_view text, size_t offset) {
    if (offset + 6 > text.byte_count || text.bytes[offset] != '\\' ||
        text.bytes[offset + 1] != 'u') {
        return -1;
    }
    return sxml_json_read_hex4(
        (sxml_byte_view){.bytes = text.bytes + offset + 2, .byte_count = 4});
}

sxml_json_unpermitted_escape sxml_json_find_unpermitted_escape(sxml_json_escape_search search) {
    const sxml_byte_view text = search.json_text;
    size_t offset = search.start_offset;
    while (offset < text.byte_count) {
        if (text.bytes[offset] != '\\') {
            offset += 1;
            continue;
        }
        const int32_t escaped = sxml_json_unicode_escape_at(text, offset);
        if (escaped < 0) {
            /* A two-character escape (all permitted), or invalid JSON. */
            offset += 2;
            continue;
        }
        const uint32_t codepoint = (uint32_t)escaped;
        const size_t end_offset = offset + 6;
        if (sxml_json_is_high_surrogate(codepoint)) {
            const int32_t next = sxml_json_unicode_escape_at(text, end_offset);
            if (next >= 0 && sxml_json_is_low_surrogate((uint32_t)next)) {
                offset = end_offset + 6;
                continue;
            }
        }
        if (!sxml_json_is_permitted_codepoint(codepoint)) {
            return (sxml_json_unpermitted_escape){
                .is_found = true,
                .codepoint = codepoint,
                .escape_sequence = {.bytes = text.bytes + offset, .byte_count = 6},
                .next_offset = end_offset};
        }
        offset = end_offset;
    }
    return (sxml_json_unpermitted_escape){.is_found = false,
                                          .codepoint = 0,
                                          .escape_sequence = {.bytes = NULL, .byte_count = 0},
                                          .next_offset = text.byte_count};
}

/* Decodes a string node into walk->decoded and describes it in *event:
   surrogate escapes are paired here (RFC 8259, section 7); an unpaired
   one is not a permitted character (F&O 4.0, section 17.4.3). */
static sxml_status sxml_json_decode_string(sxml_json_walk *walk, TSNode string_node,
                                           sxml_json_event *event) {
    walk->decoded.byte_count = 0;
    walk->has_backslash = false;
    bool has_pending_high_surrogate = false;
    uint32_t pending_high_surrogate = 0;
    sxml_status status = SXML_STATUS_OK;
    TSTreeCursor cursor = ts_tree_cursor_new(string_node);
    for (bool has_child = ts_tree_cursor_goto_first_child(&cursor);
         has_child && status == SXML_STATUS_OK;
         has_child = ts_tree_cursor_goto_next_sibling(&cursor)) {
        const TSNode child = ts_tree_cursor_current_node(&cursor);
        const TSSymbol symbol = ts_node_symbol(child);
        const bool is_escape = symbol == walk->grammar.escape_symbol;
        const uint32_t codepoint =
            is_escape ? sxml_json_escape_codepoint(sxml_json_node_text(walk, child)) : 0;
        if (has_pending_high_surrogate) {
            has_pending_high_surrogate = false;
            if (is_escape && sxml_json_is_low_surrogate(codepoint)) {
                status = sxml_json_append_escaped_codepoint(
                    walk,
                    0x10000U + ((pending_high_surrogate - 0xD800U) << 10U) +
                        (codepoint - 0xDC00U));
                continue;
            }
            status = sxml_json_append_escaped_codepoint(walk, pending_high_surrogate);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (is_escape && sxml_json_is_high_surrogate(codepoint)) {
            has_pending_high_surrogate = true;
            pending_high_surrogate = codepoint;
        } else if (is_escape) {
            status = sxml_json_append_escaped_codepoint(walk, codepoint);
        } else if (symbol == walk->grammar.unescaped_symbol) {
            status = sxml_json_append_unescaped_text(walk, sxml_json_node_text(walk, child));
        }
    }
    ts_tree_cursor_delete(&cursor);
    if (status == SXML_STATUS_OK && has_pending_high_surrogate) {
        status = sxml_json_append_escaped_codepoint(walk, pending_high_surrogate);
    }
    if (status == SXML_STATUS_OK) {
        event->text = sxml_json_bytes_view(&walk->decoded);
        event->has_backslash = walk->has_backslash;
    }
    return status;
}

static sxml_status sxml_json_send(sxml_json_walk *walk, const sxml_json_event *event,
                                  sxml_json_reply *reply) {
    *reply = (sxml_json_reply){.skips_value = false};
    return walk->sink.operations->receive(walk->sink.state, event, reply);
}

/* A node the walk reaches, and whether it holds a member's key. */
typedef struct sxml_json_visit {
    TSNode node;
    bool is_key;
} sxml_json_visit;

/* Whether the walk goes into a node's children (arrays, objects and
   members). */
typedef struct sxml_json_descent {
    bool descends;
} sxml_json_descent;

/* Sends the events that start a node and says whether its children are
   walked. */
static sxml_status sxml_json_enter(sxml_json_walk *walk, sxml_json_visit visit,
                                   sxml_json_descent *descent) {
    descent->descends = false;
    const TSNode node = visit.node;
    if (!ts_node_is_named(node)) {
        return SXML_STATUS_OK;
    }
    const TSSymbol symbol = ts_node_symbol(node);
    if (symbol == walk->grammar.member_symbol) {
        descent->descends = true;
        return SXML_STATUS_OK;
    }
    if (walk->skips_next_value && !visit.is_key) {
        walk->skips_next_value = false;
        return SXML_STATUS_OK;
    }
    sxml_json_event event = {.event_kind = SXML_JSON_EVENT_KIND_NULL,
                             .text = {.bytes = NULL, .byte_count = 0},
                             .has_backslash = false};
    if (symbol == walk->grammar.object_symbol || symbol == walk->grammar.array_symbol) {
        const uint32_t maximum_depth = walk->request->maximum_depth;
        if (maximum_depth != 0 && walk->depth >= maximum_depth) {
            return SXML_STATUS_LIMIT_EXCEEDED;
        }
        walk->depth += 1;
        descent->descends = true;
        event.event_kind = symbol == walk->grammar.object_symbol
                               ? SXML_JSON_EVENT_KIND_START_OBJECT
                               : SXML_JSON_EVENT_KIND_START_ARRAY;
    } else if (symbol == walk->grammar.string_symbol) {
        event.event_kind =
            visit.is_key ? SXML_JSON_EVENT_KIND_KEY : SXML_JSON_EVENT_KIND_STRING;
        const sxml_status status = sxml_json_decode_string(walk, node, &event);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    } else if (symbol == walk->grammar.number_symbol) {
        event.event_kind = SXML_JSON_EVENT_KIND_NUMBER;
        event.text = sxml_json_node_text(walk, node);
    } else if (symbol == walk->grammar.true_symbol) {
        event.event_kind = SXML_JSON_EVENT_KIND_TRUE;
    } else if (symbol == walk->grammar.false_symbol) {
        event.event_kind = SXML_JSON_EVENT_KIND_FALSE;
    } else if (symbol != walk->grammar.null_symbol) {
        /* Children of strings are read by sxml_json_decode_string. */
        return SXML_STATUS_OK;
    }
    sxml_json_reply reply;
    const sxml_status status = sxml_json_send(walk, &event, &reply);
    if (status == SXML_STATUS_OK && event.event_kind == SXML_JSON_EVENT_KIND_KEY) {
        walk->skips_next_value = reply.skips_value;
    }
    return status;
}

/* Sends the events that end a node the walk descended into. */
static sxml_status sxml_json_leave(sxml_json_walk *walk, TSNode node) {
    const TSSymbol symbol = ts_node_symbol(node);
    if (symbol != walk->grammar.object_symbol && symbol != walk->grammar.array_symbol) {
        return SXML_STATUS_OK;
    }
    walk->depth -= 1;
    const sxml_json_event event = {
        .event_kind = symbol == walk->grammar.object_symbol ? SXML_JSON_EVENT_KIND_END_OBJECT
                                                            : SXML_JSON_EVENT_KIND_END_ARRAY,
        .text = {.bytes = NULL, .byte_count = 0},
        .has_backslash = false};
    sxml_json_reply reply;
    return sxml_json_send(walk, &event, &reply);
}

/* Walks the value of the document node in document order, entering each
   node and leaving those it descended into, with one cursor and no
   recursion (D71). */
static sxml_status sxml_json_walk_document(sxml_json_walk *walk, TSNode document) {
    TSTreeCursor cursor = ts_tree_cursor_new(document);
    sxml_status status = SXML_STATUS_OK;
    bool has_node = ts_tree_cursor_goto_first_child(&cursor);
    while (has_node && status == SXML_STATUS_OK) {
        const sxml_json_visit visit = {
            .node = ts_tree_cursor_current_node(&cursor),
            .is_key = ts_tree_cursor_current_field_id(&cursor) == walk->grammar.key_field};
        sxml_json_descent descent = {.descends = false};
        status = sxml_json_enter(walk, visit, &descent);
        const bool descends = descent.descends;
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (descends && ts_tree_cursor_goto_first_child(&cursor)) {
            continue;
        }
        if (descends) {
            status = sxml_json_leave(walk, visit.node);
        }
        has_node = false;
        while (status == SXML_STATUS_OK) {
            if (ts_tree_cursor_goto_next_sibling(&cursor)) {
                has_node = true;
                break;
            }
            /* ts_tree_cursor_current_depth is linear in the depth; the
               document node is recognized instead. */
            if (!ts_tree_cursor_goto_parent(&cursor) ||
                ts_node_eq(ts_tree_cursor_current_node(&cursor), document)) {
                break;
            }
            status = sxml_json_leave(walk, ts_tree_cursor_current_node(&cursor));
        }
    }
    ts_tree_cursor_delete(&cursor);
    return status;
}

/* Finds the first ERROR or MISSING node under root (as src/xml.c does),
   root itself when none is found, and returns its last leaf. */
static TSNode sxml_json_find_error(TSNode root) {
    TSNode found = root;
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    for (;;) {
        const TSNode node = ts_tree_cursor_current_node(&cursor);
        if (ts_node_is_error(node) || ts_node_is_missing(node)) {
            found = node;
            break;
        }
        if (ts_node_has_error(node) && ts_tree_cursor_goto_first_child(&cursor)) {
            continue;
        }
        bool has_next = false;
        while (!has_next) {
            has_next = ts_tree_cursor_goto_next_sibling(&cursor);
            if (!has_next && !ts_tree_cursor_goto_parent(&cursor)) {
                break;
            }
        }
        if (!has_next) {
            break;
        }
    }
    ts_tree_cursor_delete(&cursor);
    /* An ERROR node spans the tokens tree-sitter could not place; the
       last of them is where the text stopped being JSON. */
    while (ts_node_child_count(found) > 0) {
        found = ts_node_child(found, ts_node_child_count(found) - 1);
    }
    return found;
}

/* Writes the decimal digits of value. */
static size_t sxml_json_write_decimal(uint64_t value, uint8_t output[24]) {
    uint8_t reversed[24];
    size_t digit_count = 0;
    do {
        reversed[digit_count] = (uint8_t)('0' + value % 10U);
        value /= 10U;
        digit_count += 1;
    } while (value != 0);
    for (size_t digit_index = 0; digit_index < digit_count; digit_index += 1) {
        output[digit_index] = reversed[digit_count - 1 - digit_index];
    }
    return digit_count;
}

/* Raises FOJS0001 for the text at byte_offset, with its line and column
   (in codepoints) in the description. */
static sxml_status sxml_json_raise_syntax_error(sxml_item_heap *item_heap,
                                                const sxml_json_text_request *request,
                                                size_t byte_offset) {
    const sxml_byte_view text = request->json_text;
    uint64_t line_number = 1;
    uint64_t column_number = 1;
    for (size_t offset = 0; offset < byte_offset && offset < text.byte_count; offset += 1) {
        if (text.bytes[offset] == '\n') {
            line_number += 1;
            column_number = 1;
        } else if ((text.bytes[offset] & 0xC0U) != 0x80U) {
            column_number += 1;
        }
    }
    static const char prefix[] = "The input is not a JSON text (RFC 8259): error at line ";
    static const char middle[] = ", column ";
    uint8_t description[sizeof(prefix) + sizeof(middle) + 48];
    size_t byte_count = sizeof(prefix) - 1;
    memcpy(description, prefix, byte_count);
    byte_count += sxml_json_write_decimal(line_number, description + byte_count);
    memcpy(description + byte_count, middle, sizeof(middle) - 1);
    byte_count += sizeof(middle) - 1;
    byte_count += sxml_json_write_decimal(column_number, description + byte_count);
    return sxml_item_heap_raise_error(
        item_heap,
        &(sxml_error_raise_request){
            .error_status = SXML_STATUS_DYNAMIC_ERROR,
            .code = {.namespace_uri = SXML_BYTE_VIEW_INITIALIZER(SXML_ERROR_NAMESPACE_URI),
                     .local_name = SXML_BYTE_VIEW_INITIALIZER("FOJS0001"),
                     .namespace_prefix = SXML_BYTE_VIEW_INITIALIZER("err")},
            .description = {.bytes = description, .byte_count = byte_count},
            .has_value = false,
            .value = {.content = {.representation = 0}},
            .location = {.source_name = {.bytes = NULL, .byte_count = 0},
                         .line_number = 0,
                         .column_number = 0,
                         .byte_offset = 0}},
        request->error_record);
}

/* Parses request's text with grammars/json and walks it into sink. */
static sxml_status sxml_json_parse(sxml_item_heap *item_heap,
                                   const sxml_json_text_request *request,
                                   sxml_json_sink sink) {
    if (item_heap == NULL || sxml_allocator_is_null(request->allocator) ||
        (request->json_text.bytes == NULL && request->json_text.byte_count != 0)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_byte_view text = request->json_text;
    /* F&O 4.0, section 17.4.4: a leading byte order mark is skipped. */
    if (text.byte_count >= 3 && text.bytes[0] == 0xEFU && text.bytes[1] == 0xBBU &&
        text.bytes[2] == 0xBFU) {
        text.bytes += 3;
        text.byte_count -= 3;
    }
    if (text.byte_count >= UINT32_MAX) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    /* U+0000 cannot appear unescaped in JSON (RFC 8259, section 7), and
       tree-sitter would read it as the end of the input. */
    const uint8_t *nul = text.byte_count == 0 ? NULL : memchr(text.bytes, 0, text.byte_count);
    if (nul != NULL) {
        return sxml_json_raise_syntax_error(item_heap, request,
                                            (size_t)(nul - request->json_text.bytes));
    }
    const TSLanguage *language = tree_sitter_json();
    TSParser *parser = ts_parser_new();
    if (parser == NULL) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    if (!ts_parser_set_language(parser, language)) {
        ts_parser_delete(parser);
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    TSTree *tree = ts_parser_parse_string(parser, NULL,
                                          text.byte_count == 0 ? "" : (const char *)text.bytes,
                                          (uint32_t)text.byte_count);
    ts_parser_delete(parser);
    if (tree == NULL) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    const TSNode root = ts_tree_root_node(tree);
    sxml_status status = SXML_STATUS_OK;
    if (ts_node_has_error(root)) {
        const TSNode error_node = sxml_json_find_error(root);
        status = sxml_json_raise_syntax_error(
            item_heap, request,
            (size_t)(text.bytes - request->json_text.bytes) + ts_node_start_byte(error_node));
    } else {
        sxml_json_walk walk = {
            .item_heap = item_heap,
            .request = request,
            .json_text = text,
            .grammar = sxml_json_load_grammar(language),
            .sink = sink,
            .decoded = {.allocator = request->allocator, .bytes = NULL, .byte_count = 0,
                        .byte_capacity = 0},
            .depth = 0,
            .has_backslash = false,
            .skips_next_value = false,
        };
        status = sxml_json_walk_document(&walk, root);
        sxml_json_bytes_release(&walk.decoded);
    }
    ts_tree_delete(tree);
    return status;
}

/* ------------------------------------------------------------ Value sink */

/* An array or object being read: where its members or entries start. */
typedef struct sxml_json_value_frame {
    bool is_object;
    size_t first_index;
} sxml_json_value_frame;

/* Makes XDM values from events (fn:parse-json). Every value on its stacks
   is owned. */
/* Keys an object of a parse reuses: the items of the first distinct keys
   (retained), so the keys that every object of an array repeats are one
   item each, not one per object (D163). Bounded, so a lookup stays cheap. */
#define SXML_JSON_KEY_CACHE_SIZE 32U

typedef struct sxml_json_value_sink {
    sxml_item_heap *item_heap;
    const sxml_json_text_request *request;
    sxml_item cached_keys[SXML_JSON_KEY_CACHE_SIZE];
    sxml_byte_view cached_key_texts[SXML_JSON_KEY_CACHE_SIZE];
    size_t cached_key_count;
    sxml_json_list frames;
    /* The entries of the open objects (sxml_map_entry). */
    sxml_json_list entries;
    /* The members of the open arrays (sxml_sequence). */
    sxml_json_list members;
    bool has_value;
    sxml_sequence value;
} sxml_json_value_sink;

/* Gives value (owned: taken over, also on failure) to the innermost open
   array or object, or makes it the result. */
static sxml_status sxml_json_value_sink_place(sxml_json_value_sink *sink,
                                              sxml_sequence value) {
    if (sink->frames.element_count == 0) {
        sink->value = value;
        sink->has_value = true;
        return SXML_STATUS_OK;
    }
    const sxml_json_value_frame *frame =
        sxml_json_list_at(&sink->frames, sink->frames.element_count - 1);
    if (frame->is_object) {
        sxml_map_entry *entry =
            sxml_json_list_at(&sink->entries, sink->entries.element_count - 1);
        entry->value = value;
        return SXML_STATUS_OK;
    }
    void *member = NULL;
    const sxml_status status = sxml_json_list_reserve(&sink->members, &member);
    if (status != SXML_STATUS_OK) {
        sxml_sequence_release(value);
        return status;
    }
    *(sxml_sequence *)member = value;
    sink->members.element_count += 1;
    return SXML_STATUS_OK;
}

static sxml_status sxml_json_make_string_item(sxml_item_heap *item_heap,
                                              sxml_byte_view text, sxml_item *item) {
    sxml_string string = {.utf8 = {.bytes = NULL, .byte_count = 0},
                          .codepoint_count = 0,
                          .is_ascii = true};
    sxml_status status = sxml_string_from_utf8(text, &string);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = string}}},
        item);
}

/* True when the lexical form of a JSON number has an exponent. */
static bool sxml_json_number_has_exponent(sxml_byte_view text) {
    return memchr(text.bytes, 'e', text.byte_count) != NULL ||
           memchr(text.bytes, 'E', text.byte_count) != NULL;
}

/* A JSON number by the number format (F&O 4.0, section 17.4.4,
   number-format). */
static sxml_status sxml_json_make_number(sxml_json_value_sink *sink, sxml_byte_view text,
                                         sxml_sequence *number) {
    const sxml_json_number_format number_format = sink->request->options.number_format;
    const bool has_exponent = sxml_json_number_has_exponent(text);
    sxml_text_cast_request cast = {
        .text = text,
        .target_type = sxml_builtin_type_identifier(SXML_TYPE_XS_DOUBLE),
        .namespace_bindings = {.namespace_bindings = NULL, .namespace_binding_count = 0},
        .error_record = sink->request->error_record};
    if (number_format == SXML_JSON_NUMBER_FORMAT_DOUBLE ||
        (number_format == SXML_JSON_NUMBER_FORMAT_ADAPTIVE && has_exponent)) {
        return sxml_cast_make_value_from_text(sink->item_heap, &cast, number);
    }
    if (has_exponent) {
        /* "first cast to xs:double and the resulting xs:double is then cast
           to xs:decimal" (FOCA0002 for an infinity). */
        sxml_sequence double_value = sxml_sequence_empty();
        sxml_status status =
            sxml_cast_make_value_from_text(sink->item_heap, &cast, &double_value);
        if (status == SXML_STATUS_OK) {
            status = sxml_cast_make_value(
                sink->item_heap,
                &(sxml_cast_request){
                    .value = double_value,
                    .target_type = sxml_builtin_type_identifier(SXML_TYPE_XS_DECIMAL),
                    .allows_empty = false,
                    .namespace_bindings = cast.namespace_bindings,
                    .error_record = sink->request->error_record},
                number);
        }
        sxml_sequence_release(double_value);
        return status;
    }
    /* A value in the value space of xs:integer becomes xs:integer: the
       lexical form without a point, or with only zeros after it. */
    const uint8_t *point = memchr(text.bytes, '.', text.byte_count);
    bool is_integral = point == NULL;
    size_t integer_byte_count = text.byte_count;
    if (point != NULL) {
        integer_byte_count = (size_t)(point - text.bytes);
        is_integral = true;
        for (size_t offset = integer_byte_count + 1; offset < text.byte_count; offset += 1) {
            is_integral = is_integral && text.bytes[offset] == '0';
        }
    }
    cast.text = (sxml_byte_view){.bytes = text.bytes,
                                 .byte_count = is_integral ? integer_byte_count
                                                           : text.byte_count};
    cast.target_type = sxml_builtin_type_identifier(is_integral ? SXML_TYPE_XS_INTEGER
                                                                : SXML_TYPE_XS_DECIMAL);
    return sxml_cast_make_value_from_text(sink->item_heap, &cast, number);
}

/* Ends the innermost object: its entries become a map. */
static sxml_status sxml_json_value_sink_end_object(sxml_json_value_sink *sink,
                                                   size_t first_index) {
    const size_t entry_count = sink->entries.element_count - first_index;
    const sxml_json_duplicates_policy policy = sink->request->options.duplicates_policy;
    sxml_error_record duplicate_error = {.value = NULL};
    sxml_item map = {.representation = 0};
    sxml_status status = sxml_map_make(
        sink->item_heap,
        &(sxml_map_request){
            .map_entries = {.map_entries = sxml_json_list_at(&sink->entries, first_index),
                            .map_entry_count = entry_count},
            .duplicate_key_policy = policy == SXML_JSON_DUPLICATES_POLICY_REJECT
                                        ? SXML_DUPLICATE_KEY_POLICY_REJECT
                                    : policy == SXML_JSON_DUPLICATES_POLICY_USE_LAST
                                        ? SXML_DUPLICATE_KEY_POLICY_USE_LAST
                                        : SXML_DUPLICATE_KEY_POLICY_USE_FIRST,
            .error_record = &duplicate_error},
        &map);
    for (size_t entry_index = first_index; entry_index < sink->entries.element_count;
         entry_index += 1) {
        const sxml_map_entry *entry = sxml_json_list_at(&sink->entries, entry_index);
        sxml_item_release(entry->key);
        sxml_sequence_release(entry->value);
    }
    sink->entries.element_count = first_index;
    sink->frames.element_count -= 1;
    if (status == SXML_STATUS_DYNAMIC_ERROR) {
        /* XQDY0137 of the map constructor is FOJS0003 here. */
        return sxml_error_record_report(sink->request->error_record,
                                        &sxml_json_duplicate_key_error);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_json_value_sink_place(sink, sxml_sequence_from_item(map));
}

/* Ends the innermost array: its members become an array. */
static sxml_status sxml_json_value_sink_end_array(sxml_json_value_sink *sink,
                                                  size_t first_index) {
    sxml_item array = {.representation = 0};
    const sxml_status status = sxml_array_make(
        sink->item_heap,
        (sxml_sequence_view){.sequences = sxml_json_list_at(&sink->members, first_index),
                             .sequence_count = sink->members.element_count - first_index},
        &array);
    for (size_t member_index = first_index; member_index < sink->members.element_count;
         member_index += 1) {
        sxml_sequence_release(*(const sxml_sequence *)sxml_json_list_at(&sink->members,
                                                                         member_index));
    }
    sink->members.element_count = first_index;
    sink->frames.element_count -= 1;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_json_value_sink_place(sink, sxml_sequence_from_item(array));
}

/* Starts an array or object (event_kind START_ARRAY or START_OBJECT). */
static sxml_status sxml_json_value_sink_start(sxml_json_value_sink *sink,
                                              sxml_json_event_kind event_kind) {
    const bool is_object = event_kind == SXML_JSON_EVENT_KIND_START_OBJECT;
    void *frame = NULL;
    const sxml_status status = sxml_json_list_reserve(&sink->frames, &frame);
    if (status == SXML_STATUS_OK) {
        *(sxml_json_value_frame *)frame = (sxml_json_value_frame){
            .is_object = is_object,
            .first_index = is_object ? sink->entries.element_count
                                     : sink->members.element_count};
        sink->frames.element_count += 1;
    }
    return status;
}

static sxml_status sxml_json_value_sink_add_key(sxml_json_value_sink *sink,
                                                sxml_byte_view key_text) {
    void *entry = NULL;
    sxml_status status = sxml_json_list_reserve(&sink->entries, &entry);
    sxml_item key = {.representation = 0};
    bool is_cached = false;
    for (size_t key_index = 0; status == SXML_STATUS_OK && key_index < sink->cached_key_count;
         key_index += 1) {
        if (sxml_byte_view_equals(sink->cached_key_texts[key_index], key_text)) {
            key = sink->cached_keys[key_index];
            sxml_item_retain(key);
            is_cached = true;
            break;
        }
    }
    if (status == SXML_STATUS_OK && !is_cached) {
        status = sxml_json_make_string_item(sink->item_heap, key_text, &key);
        sxml_atomic_datum datum;
        memset(&datum, 0, sizeof(datum));
        if (status == SXML_STATUS_OK && sink->cached_key_count < SXML_JSON_KEY_CACHE_SIZE &&
            sxml_atomic_item_get_datum(key, &datum) == SXML_STATUS_OK) {
            /* The text is the item's own, valid while the item lives. */
            sxml_item_retain(key);
            sink->cached_keys[sink->cached_key_count] = key;
            sink->cached_key_texts[sink->cached_key_count] = datum.datum_details.string.utf8;
            sink->cached_key_count += 1;
        }
    }
    if (status == SXML_STATUS_OK) {
        *(sxml_map_entry *)entry =
            (sxml_map_entry){.key = key, .value = sxml_sequence_empty()};
        sink->entries.element_count += 1;
    }
    return status;
}

static sxml_status sxml_json_value_sink_receive(void *state, const sxml_json_event *event,
                                                sxml_json_reply *reply) {
    (void)reply;
    sxml_json_value_sink *sink = state;
    sxml_status status = SXML_STATUS_OK;
    sxml_item item = {.representation = 0};
    sxml_sequence number = sxml_sequence_empty();
    switch (event->event_kind) {
    case SXML_JSON_EVENT_KIND_START_OBJECT:
    case SXML_JSON_EVENT_KIND_START_ARRAY:
        return sxml_json_value_sink_start(sink, event->event_kind);
    case SXML_JSON_EVENT_KIND_END_OBJECT:
    case SXML_JSON_EVENT_KIND_END_ARRAY: {
        const sxml_json_value_frame *frame =
            sxml_json_list_at(&sink->frames, sink->frames.element_count - 1);
        return frame->is_object ? sxml_json_value_sink_end_object(sink, frame->first_index)
                                : sxml_json_value_sink_end_array(sink, frame->first_index);
    }
    case SXML_JSON_EVENT_KIND_KEY:
        return sxml_json_value_sink_add_key(sink, event->text);
    case SXML_JSON_EVENT_KIND_STRING:
        status = sxml_json_make_string_item(sink->item_heap, event->text, &item);
        return status == SXML_STATUS_OK
                   ? sxml_json_value_sink_place(sink, sxml_sequence_from_item(item))
                   : status;
    case SXML_JSON_EVENT_KIND_NUMBER:
        status = sxml_json_make_number(sink, event->text, &number);
        return status == SXML_STATUS_OK ? sxml_json_value_sink_place(sink, number) : status;
    case SXML_JSON_EVENT_KIND_TRUE:
        return sxml_json_value_sink_place(sink,
                                          sxml_sequence_from_item(sxml_atomic_item_true()));
    case SXML_JSON_EVENT_KIND_FALSE:
        return sxml_json_value_sink_place(sink,
                                          sxml_sequence_from_item(sxml_atomic_item_false()));
    case SXML_JSON_EVENT_KIND_NULL:
        sxml_sequence_retain(sink->request->options.null_value);
        return sxml_json_value_sink_place(sink, sink->request->options.null_value);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

static const sxml_json_sink_operations sxml_json_value_sink_operations = {
    .receive = sxml_json_value_sink_receive,
};

sxml_status sxml_json_make_value(sxml_item_heap *item_heap,
                                 const sxml_json_text_request *request,
                                 sxml_sequence *value) {
    if (request == NULL || value == NULL ||
        request->options.duplicates_policy == SXML_JSON_DUPLICATES_POLICY_RETAIN) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_json_value_sink sink = {
        .item_heap = item_heap,
        .request = request,
        .frames = SXML_JSON_LIST(request->allocator, sxml_json_value_frame),
        .entries = SXML_JSON_LIST(request->allocator, sxml_map_entry),
        .members = SXML_JSON_LIST(request->allocator, sxml_sequence),
        .has_value = false,
        .value = sxml_sequence_empty(),
        .cached_key_count = 0,
    };
    const sxml_status status = sxml_json_parse(
        item_heap, request,
        (sxml_json_sink){.state = &sink, .operations = &sxml_json_value_sink_operations});
    for (size_t entry_index = 0; entry_index < sink.entries.element_count; entry_index += 1) {
        const sxml_map_entry *entry = sxml_json_list_at(&sink.entries, entry_index);
        sxml_item_release(entry->key);
        sxml_sequence_release(entry->value);
    }
    for (size_t member_index = 0; member_index < sink.members.element_count;
         member_index += 1) {
        sxml_sequence_release(
            *(const sxml_sequence *)sxml_json_list_at(&sink.members, member_index));
    }
    for (size_t key_index = 0; key_index < sink.cached_key_count; key_index += 1) {
        sxml_item_release(sink.cached_keys[key_index]);
    }
    sxml_json_list_release(&sink.frames);
    sxml_json_list_release(&sink.entries);
    sxml_json_list_release(&sink.members);
    if (status != SXML_STATUS_OK) {
        sxml_sequence_release(sink.value);
        return status;
    }
    *value = sink.value;
    return SXML_STATUS_OK;
}

/* -------------------------------------------------------------- XML sink */

/* The names of the XML representation of JSON (F&O 4.0, section 17.4.2). */
typedef struct sxml_json_names {
    sxml_name_identifier map_name;
    sxml_name_identifier array_name;
    sxml_name_identifier string_name;
    sxml_name_identifier number_name;
    sxml_name_identifier boolean_name;
    sxml_name_identifier null_name;
    sxml_name_identifier key_name;
    sxml_name_identifier escaped_name;
    sxml_name_identifier escaped_key_name;
} sxml_json_names;

/* A name in namespace with local name text. */
typedef struct sxml_json_name_text {
    sxml_well_known_namespace name_namespace;
    const char *local_name;
} sxml_json_name_text;

static sxml_status sxml_json_intern_names(sxml_name_pool *name_pool,
                                          sxml_json_names *names) {
    const sxml_json_name_text texts[] = {
        {SXML_NAMESPACE_FUNCTIONS, "map"},     {SXML_NAMESPACE_FUNCTIONS, "array"},
        {SXML_NAMESPACE_FUNCTIONS, "string"},  {SXML_NAMESPACE_FUNCTIONS, "number"},
        {SXML_NAMESPACE_FUNCTIONS, "boolean"}, {SXML_NAMESPACE_FUNCTIONS, "null"},
        {SXML_NAMESPACE_NONE, "key"},          {SXML_NAMESPACE_NONE, "escaped"},
        {SXML_NAMESPACE_NONE, "escaped-key"},
    };
    sxml_name_identifier *const outputs[] = {
        &names->map_name,     &names->array_name,   &names->string_name,
        &names->number_name,  &names->boolean_name, &names->null_name,
        &names->key_name,     &names->escaped_name, &names->escaped_key_name,
    };
    for (size_t name_index = 0; name_index < sizeof(texts) / sizeof(texts[0]);
         name_index += 1) {
        const sxml_status status = sxml_name_pool_intern_name(
            name_pool,
            &(sxml_name_request){
                .namespace_uri = sxml_well_known_namespace_identifier(texts[name_index].name_namespace),
                .local_name = sxml_byte_view_from_c_string(texts[name_index].local_name)},
            outputs[name_index]);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

/* An object being converted: its number in the key index, and where its
   keys start. */
typedef struct sxml_json_xml_frame {
    uint64_t object_number;
    sxml_json_key_mark key_mark;
} sxml_json_xml_frame;

/* Sends the XML representation of JSON to a tree builder
   (fn:json-to-xml). */
typedef struct sxml_json_xml_sink {
    const sxml_json_text_request *request;
    sxml_tree_builder tree_builder;
    sxml_json_names names;
    /* The open arrays and objects (sxml_json_xml_frame); an array has
       object number 0. */
    sxml_json_list frames;
    uint64_t next_object_number;
    sxml_json_key_index key_index;
    /* The key of the next element, copied from its KEY event. */
    bool has_key;
    bool key_has_backslash;
    sxml_json_bytes key;
    bool has_started_element;
} sxml_json_xml_sink;

static const sxml_source_location sxml_json_no_location = {
    .source_name = {.bytes = NULL, .byte_count = 0},
    .line_number = 0,
    .column_number = 0,
    .byte_offset = 0};

static sxml_status sxml_json_xml_sink_attribute(sxml_json_xml_sink *sink,
                                                sxml_name_identifier name,
                                                sxml_byte_view value) {
    return sink->tree_builder.operations->add_attribute(
        sink->tree_builder.state,
        &(sxml_attribute_event){.attribute_name = name,
                                .namespace_prefix = {.index = 0},
                                .attribute_value = value,
                                .type_annotation = {.index = SXML_TYPE_NONE},
                                .is_id = false,
                                .is_idrefs = false,
                                .location = sxml_json_no_location});
}

/* An element with text content (string, number, boolean) or none (null);
   for maps and arrays only the name is used. */
typedef struct sxml_json_leaf {
    sxml_name_identifier name;
    sxml_byte_view text;
    bool is_escaped_string;
} sxml_json_leaf;

/* Starts the element for a value, with its key and escaped attributes. */
static sxml_status sxml_json_xml_sink_start_element(sxml_json_xml_sink *sink,
                                                    const sxml_json_leaf *leaf) {
    const sxml_name_identifier name = leaf->name;
    const bool is_escaped_string = leaf->is_escaped_string;
    const sxml_tree_builder builder = sink->tree_builder;
    sxml_status status = builder.operations->start_element(
        builder.state, &(sxml_element_start_event){.element_name = name,
                                                   .namespace_prefix = {.index = 0},
                                                   .type_annotation = {.index = SXML_TYPE_NONE},
                                                   .is_id = false,
                                                   .is_idrefs = false,
                                                   .is_nilled = false,
                                                   .ignores_parent_namespaces = false,
                                                   .location = sxml_json_no_location});
    if (status == SXML_STATUS_OK && !sink->has_started_element) {
        sink->has_started_element = true;
        status = builder.operations->add_namespace_binding(
            builder.state,
            &(sxml_namespace_binding){
                .namespace_prefix = {.index = 0},
                .namespace_uri = sxml_well_known_namespace_identifier(SXML_NAMESPACE_FUNCTIONS)});
    }
    const sxml_byte_view true_text = SXML_BYTE_VIEW_FROM_LITERAL("true");
    if (status == SXML_STATUS_OK && sink->has_key) {
        status = sxml_json_xml_sink_attribute(sink, sink->names.key_name,
                                              sxml_json_bytes_view(&sink->key));
        if (status == SXML_STATUS_OK && sink->key_has_backslash) {
            status = sxml_json_xml_sink_attribute(sink, sink->names.escaped_key_name,
                                                  true_text);
        }
        sink->has_key = false;
    }
    if (status == SXML_STATUS_OK && is_escaped_string) {
        status = sxml_json_xml_sink_attribute(sink, sink->names.escaped_name, true_text);
    }
    return status;
}

static sxml_status sxml_json_xml_sink_leaf(sxml_json_xml_sink *sink,
                                           const sxml_json_leaf *leaf) {
    sxml_status status = sxml_json_xml_sink_start_element(sink, leaf);
    if (status == SXML_STATUS_OK && leaf->text.byte_count > 0) {
        status = sink->tree_builder.operations->add_text(
            sink->tree_builder.state,
            &(sxml_text_event){.text = leaf->text, .location = sxml_json_no_location});
    }
    if (status == SXML_STATUS_OK) {
        status = sink->tree_builder.operations->end_element(sink->tree_builder.state);
    }
    return status;
}

/* Starts a map or array element (event_kind START_OBJECT or
   START_ARRAY). */
static sxml_status sxml_json_xml_sink_start_container(sxml_json_xml_sink *sink,
                                                      sxml_json_event_kind event_kind) {
    const bool is_object = event_kind == SXML_JSON_EVENT_KIND_START_OBJECT;
    void *frame = NULL;
    sxml_status status = sxml_json_list_reserve(&sink->frames, &frame);
    if (status == SXML_STATUS_OK) {
        status = sxml_json_xml_sink_start_element(
            sink, &(sxml_json_leaf){.name = is_object ? sink->names.map_name
                                                      : sink->names.array_name,
                                    .text = {.bytes = NULL, .byte_count = 0},
                                    .is_escaped_string = false});
    }
    if (status == SXML_STATUS_OK) {
        uint64_t object_number = 0;
        if (is_object) {
            sink->next_object_number += 1;
            object_number = sink->next_object_number;
        }
        *(sxml_json_xml_frame *)frame = (sxml_json_xml_frame){
            .object_number = object_number,
            .key_mark = sxml_json_key_index_mark(&sink->key_index)};
        sink->frames.element_count += 1;
    }
    return status;
}

static sxml_status sxml_json_xml_sink_end_container(sxml_json_xml_sink *sink) {
    sink->frames.element_count -= 1;
    const sxml_json_xml_frame *frame =
        sxml_json_list_at(&sink->frames, sink->frames.element_count);
    sxml_json_key_index_restore(&sink->key_index, frame->key_mark);
    return sink->tree_builder.operations->end_element(sink->tree_builder.state);
}

/* A key: duplicates by the policy (F&O 4.0, section 17.4.6, duplicates),
   then kept for the value's element. */
static sxml_status sxml_json_xml_sink_key(sxml_json_xml_sink *sink,
                                          const sxml_json_event *event,
                                          sxml_json_reply *reply) {
    const sxml_json_duplicates_policy policy = sink->request->options.duplicates_policy;
    if (policy != SXML_JSON_DUPLICATES_POLICY_RETAIN) {
        const sxml_json_xml_frame *frame =
            sxml_json_list_at(&sink->frames, sink->frames.element_count - 1);
        sxml_json_key_outcome outcome = {.is_duplicate = false};
        const sxml_status status = sxml_json_key_index_insert(
            &sink->key_index,
            &(sxml_json_key_query){.object_number = frame->object_number, .key = event->text},
            &outcome);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (outcome.is_duplicate && policy == SXML_JSON_DUPLICATES_POLICY_REJECT) {
            return sxml_error_record_report(sink->request->error_record,
                                            &sxml_json_duplicate_key_error);
        }
        if (outcome.is_duplicate) {
            reply->skips_value = true;
            return SXML_STATUS_OK;
        }
    }
    sink->key.byte_count = 0;
    const sxml_status status = sxml_json_bytes_append(&sink->key, event->text);
    if (status == SXML_STATUS_OK) {
        sink->has_key = true;
        sink->key_has_backslash = event->has_backslash;
    }
    return status;
}

static sxml_status sxml_json_xml_sink_receive(void *state, const sxml_json_event *event,
                                              sxml_json_reply *reply) {
    sxml_json_xml_sink *sink = state;
    sxml_json_leaf leaf = {.name = sink->names.null_name,
                           .text = event->text,
                           .is_escaped_string = false};
    switch (event->event_kind) {
    case SXML_JSON_EVENT_KIND_START_OBJECT:
    case SXML_JSON_EVENT_KIND_START_ARRAY:
        return sxml_json_xml_sink_start_container(sink, event->event_kind);
    case SXML_JSON_EVENT_KIND_END_OBJECT:
    case SXML_JSON_EVENT_KIND_END_ARRAY:
        return sxml_json_xml_sink_end_container(sink);
    case SXML_JSON_EVENT_KIND_KEY:
        return sxml_json_xml_sink_key(sink, event, reply);
    case SXML_JSON_EVENT_KIND_STRING:
        leaf.name = sink->names.string_name;
        leaf.is_escaped_string = event->has_backslash;
        return sxml_json_xml_sink_leaf(sink, &leaf);
    case SXML_JSON_EVENT_KIND_NUMBER:
        leaf.name = sink->names.number_name;
        return sxml_json_xml_sink_leaf(sink, &leaf);
    case SXML_JSON_EVENT_KIND_TRUE:
        leaf.name = sink->names.boolean_name;
        leaf.text = SXML_BYTE_VIEW_FROM_LITERAL("true");
        return sxml_json_xml_sink_leaf(sink, &leaf);
    case SXML_JSON_EVENT_KIND_FALSE:
        leaf.name = sink->names.boolean_name;
        leaf.text = SXML_BYTE_VIEW_FROM_LITERAL("false");
        return sxml_json_xml_sink_leaf(sink, &leaf);
    case SXML_JSON_EVENT_KIND_NULL:
        leaf.text = (sxml_byte_view){.bytes = NULL, .byte_count = 0};
        return sxml_json_xml_sink_leaf(sink, &leaf);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

static const sxml_json_sink_operations sxml_json_xml_sink_operations = {
    .receive = sxml_json_xml_sink_receive,
};

sxml_status sxml_json_send_xml_events(sxml_item_heap *item_heap,
                                      const sxml_json_text_request *request,
                                      sxml_tree_builder tree_builder) {
    if (item_heap == NULL || request == NULL || tree_builder.operations == NULL ||
        request->options.duplicates_policy == SXML_JSON_DUPLICATES_POLICY_USE_LAST) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_json_xml_sink sink = {
        .request = request,
        .tree_builder = tree_builder,
        .frames = SXML_JSON_LIST(request->allocator, sxml_json_xml_frame),
        .next_object_number = 0,
        .key_index = sxml_json_key_index_make(request->allocator),
        .has_key = false,
        .key_has_backslash = false,
        .key = {.allocator = request->allocator, .bytes = NULL, .byte_count = 0,
                .byte_capacity = 0},
        .has_started_element = false,
    };
    sxml_status status =
        sxml_json_intern_names(sxml_item_heap_get_name_pool(item_heap), &sink.names);
    if (status == SXML_STATUS_OK) {
        status = sxml_json_parse(
            item_heap, request,
            (sxml_json_sink){.state = &sink, .operations = &sxml_json_xml_sink_operations});
    }
    sxml_json_list_release(&sink.frames);
    sxml_json_key_index_release(&sink.key_index);
    sxml_json_bytes_release(&sink.key);
    return status;
}

/* ----------------------------------------------------------- xml-to-json */

/* The elements of the XML representation of JSON. */
typedef enum sxml_json_element_kind {
    SXML_JSON_ELEMENT_KIND_MAP,
    SXML_JSON_ELEMENT_KIND_ARRAY,
    SXML_JSON_ELEMENT_KIND_STRING,
    SXML_JSON_ELEMENT_KIND_NUMBER,
    SXML_JSON_ELEMENT_KIND_BOOLEAN,
    SXML_JSON_ELEMENT_KIND_NULL,
} sxml_json_element_kind;

/* Where an element sits: what attributes it may have. */
typedef enum sxml_json_element_place {
    /* The node given, or the child of the document given: key and
       escaped-key are allowed and ignored. */
    SXML_JSON_ELEMENT_PLACE_TOP,
    /* A child of map: key is required. */
    SXML_JSON_ELEMENT_PLACE_IN_MAP,
    /* A child of array: no key. */
    SXML_JSON_ELEMENT_PLACE_IN_ARRAY,
} sxml_json_element_place;

/* A map or array element being written. */
typedef struct sxml_json_writer_frame {
    sxml_node_reference element;
    bool is_map;
    bool has_next_child;
    sxml_node_index next_child;
    bool has_written_member;
    uint64_t object_number;
    sxml_json_key_mark key_mark;
} sxml_json_writer_frame;

typedef struct sxml_json_writer {
    sxml_item_heap *item_heap;
    const sxml_json_writing_request *request;
    sxml_output_stream output_stream;
    sxml_name_pool *name_pool;
    sxml_json_names names;
    sxml_json_list frames;
    uint64_t next_object_number;
    sxml_json_key_index key_index;
    /* The text content of a leaf element, then its JSON form. */
    sxml_json_bytes content;
    sxml_json_bytes output;
} sxml_json_writer;

/* What the attributes of an element said. */
typedef struct sxml_json_attributes {
    bool has_key;
    sxml_byte_view key;
    bool is_escaped_key;
    bool is_escaped;
} sxml_json_attributes;

static sxml_status sxml_json_report_invalid(const sxml_json_writer *writer) {
    return sxml_error_record_report(writer->request->error_record,
                                    &sxml_json_invalid_representation_error);
}

/* The xs:boolean lexical form after whitespace collapsing (XSD 1.1 Part
   2, section 3.3.2): writes the value, or returns false when text is not
   one. */
typedef struct sxml_json_boolean_reading {
    bool is_boolean;
    bool value;
} sxml_json_boolean_reading;

static sxml_json_boolean_reading sxml_json_read_boolean(sxml_byte_view text) {
    text = sxml_xml_text_trim_whitespace(text);
    const bool is_true = sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("true")) ||
                         sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("1"));
    const bool is_false = sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("false")) ||
                          sxml_byte_view_equals(text, SXML_BYTE_VIEW_FROM_LITERAL("0"));
    return (sxml_json_boolean_reading){.is_boolean = is_true || is_false, .value = is_true};
}

static bool sxml_json_name_is(sxml_name_identifier name, sxml_name_identifier expected) {
    return name.index == expected.index;
}

/* An element and its description. */
typedef struct sxml_json_described_element {
    sxml_node_reference element;
    const sxml_node_description *description;
    /* For leaf content: text is allowed (false for null). */
    bool allows_text;
} sxml_json_described_element;

/* Reads and checks the attributes of element (F&O 4.0, section 17.4.7:
   attributes in other namespaces are ignored). */
static sxml_status sxml_json_read_attributes(const sxml_json_writer *writer,
                                             sxml_json_described_element described,
                                             sxml_json_attributes *attributes) {
    const sxml_node_reference element = described.element;
    const sxml_node_description *description = described.description;
    *attributes = (sxml_json_attributes){.has_key = false,
                                         .key = {.bytes = NULL, .byte_count = 0},
                                         .is_escaped_key = false,
                                         .is_escaped = false};
    bool has_escaped_key = false;
    for (uint32_t attribute_number = 1; attribute_number <= description->attribute_count;
         attribute_number += 1) {
        const sxml_node_reference attribute = {
            .tree = element.tree,
            .node_index = {.index = element.node_index.index + attribute_number},
            .namespace_number = 0};
        sxml_node_description attribute_description;
        sxml_byte_view value = {.bytes = NULL, .byte_count = 0};
        sxml_name_parts parts = {.namespace_uri = {.index = 0},
                                 .local_name = {.bytes = NULL, .byte_count = 0}};
        sxml_status status = sxml_node_describe(attribute, &attribute_description);
        if (status == SXML_STATUS_OK) {
            status = sxml_node_get_content(attribute, &value);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_get_name_parts(writer->name_pool,
                                                   attribute_description.node_name, &parts);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const sxml_name_identifier name = attribute_description.node_name;
        const sxml_json_boolean_reading flag = sxml_json_read_boolean(value);
        if (parts.namespace_uri.index != SXML_NAMESPACE_NONE &&
            parts.namespace_uri.index != SXML_NAMESPACE_FUNCTIONS) {
            continue;
        }
        if (sxml_json_name_is(name, writer->names.key_name)) {
            attributes->has_key = true;
            attributes->key = value;
        } else if (sxml_json_name_is(name, writer->names.escaped_key_name) &&
                   flag.is_boolean) {
            has_escaped_key = true;
            attributes->is_escaped_key = flag.value;
        } else if (sxml_json_name_is(name, writer->names.escaped_name) && flag.is_boolean) {
            attributes->is_escaped = flag.value;
            if (!sxml_json_name_is(description->node_name, writer->names.string_name)) {
                return sxml_json_report_invalid(writer);
            }
        } else {
            return sxml_json_report_invalid(writer);
        }
    }
    if (has_escaped_key && !attributes->has_key) {
        return sxml_json_report_invalid(writer);
    }
    return SXML_STATUS_OK;
}

/* The kind of a JSON element; false when element's name is none of them. */
static bool sxml_json_element_kind_of(const sxml_json_writer *writer,
                                      sxml_name_identifier name,
                                      sxml_json_element_kind *kind) {
    const sxml_json_names *names = &writer->names;
    const sxml_name_identifier candidates[] = {
        names->map_name,    names->array_name,   names->string_name,
        names->number_name, names->boolean_name, names->null_name,
    };
    const sxml_json_element_kind kinds[] = {
        SXML_JSON_ELEMENT_KIND_MAP,    SXML_JSON_ELEMENT_KIND_ARRAY,
        SXML_JSON_ELEMENT_KIND_STRING, SXML_JSON_ELEMENT_KIND_NUMBER,
        SXML_JSON_ELEMENT_KIND_BOOLEAN, SXML_JSON_ELEMENT_KIND_NULL,
    };
    for (size_t candidate_index = 0; candidate_index < 6; candidate_index += 1) {
        if (sxml_json_name_is(name, candidates[candidate_index])) {
            *kind = kinds[candidate_index];
            return true;
        }
    }
    return false;
}

static sxml_status sxml_json_write(sxml_json_writer *writer, sxml_byte_view bytes) {
    return sxml_output_stream_write(writer->output_stream, bytes);
}

/* Collects the text children of a leaf element into writer->content;
   element children, or any text in null, are invalid (comments and
   processing instructions are ignored). */
static sxml_status sxml_json_read_leaf_content(sxml_json_writer *writer,
                                               sxml_json_described_element described) {
    const sxml_node_reference element = described.element;
    const sxml_node_description *description = described.description;
    const bool allows_text = described.allows_text;
    writer->content.byte_count = 0;
    bool has_child = description->has_children;
    sxml_node_index child_index = description->first_child;
    while (has_child) {
        const sxml_node_reference child = {
            .tree = element.tree, .node_index = child_index, .namespace_number = 0};
        sxml_node_description child_description;
        sxml_status status = sxml_node_describe(child, &child_description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (child_description.node_kind == SXML_NODE_KIND_ELEMENT) {
            return sxml_json_report_invalid(writer);
        }
        if (child_description.node_kind == SXML_NODE_KIND_TEXT) {
            sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
            status = sxml_node_get_content(child, &text);
            if (status == SXML_STATUS_OK) {
                status = sxml_json_bytes_append(&writer->content, text);
            }
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        has_child = child_description.has_next_sibling;
        child_index = child_description.next_sibling;
    }
    if (!allows_text && sxml_xml_text_trim_whitespace(sxml_json_bytes_view(&writer->content)).byte_count > 0) {
        return sxml_json_report_invalid(writer);
    }
    return SXML_STATUS_OK;
}

static bool sxml_json_is_digit(uint8_t byte) {
    return byte >= '0' && byte <= '9';
}

/*
 * Writes a number element's text in JSON syntax (F&O 4.0, section 17.4.7):
 * the text must match the xs:double pattern
 * (\+|-)?([0-9]+(\.[0-9]*)?|\.[0-9]+)([Ee](\+|-)?[0-9]+)? after trimming;
 * a leading plus and leading zeros go, and a zero is added on a side of a
 * point that has no digit.
 */
static sxml_status sxml_json_write_number(sxml_json_writer *writer, sxml_byte_view text) {
    text = sxml_xml_text_trim_whitespace(text);
    size_t offset = 0;
    bool is_negative = false;
    if (offset < text.byte_count && (text.bytes[offset] == '+' || text.bytes[offset] == '-')) {
        is_negative = text.bytes[offset] == '-';
        offset += 1;
    }
    const size_t integer_start = offset;
    while (offset < text.byte_count && sxml_json_is_digit(text.bytes[offset])) {
        offset += 1;
    }
    const size_t integer_end = offset;
    bool has_point = false;
    size_t fraction_start = offset;
    size_t fraction_end = offset;
    if (offset < text.byte_count && text.bytes[offset] == '.') {
        has_point = true;
        offset += 1;
        fraction_start = offset;
        while (offset < text.byte_count && sxml_json_is_digit(text.bytes[offset])) {
            offset += 1;
        }
        fraction_end = offset;
    }
    if (integer_end == integer_start && fraction_end == fraction_start) {
        return sxml_json_report_invalid(writer);
    }
    const size_t exponent_start = offset;
    if (offset < text.byte_count && (text.bytes[offset] == 'e' || text.bytes[offset] == 'E')) {
        offset += 1;
        if (offset < text.byte_count && (text.bytes[offset] == '+' || text.bytes[offset] == '-')) {
            offset += 1;
        }
        const size_t exponent_digits = offset;
        while (offset < text.byte_count && sxml_json_is_digit(text.bytes[offset])) {
            offset += 1;
        }
        if (offset == exponent_digits) {
            return sxml_json_report_invalid(writer);
        }
    }
    if (offset != text.byte_count) {
        return sxml_json_report_invalid(writer);
    }
    size_t first_digit = integer_start;
    while (first_digit + 1 < integer_end && text.bytes[first_digit] == '0') {
        first_digit += 1;
    }
    writer->output.byte_count = 0;
    sxml_status status = SXML_STATUS_OK;
    if (is_negative) {
        status = sxml_json_bytes_append(&writer->output, SXML_BYTE_VIEW_FROM_LITERAL("-"));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_json_bytes_append(
            &writer->output, integer_end == integer_start
                                 ? SXML_BYTE_VIEW_FROM_LITERAL("0")
                                 : (sxml_byte_view){.bytes = text.bytes + first_digit,
                                                    .byte_count = integer_end - first_digit});
    }
    if (status == SXML_STATUS_OK && has_point) {
        status = sxml_json_bytes_append(
            &writer->output, fraction_end == fraction_start
                                 ? SXML_BYTE_VIEW_FROM_LITERAL(".0")
                                 : (sxml_byte_view){.bytes = text.bytes + fraction_start - 1,
                                                    .byte_count =
                                                        fraction_end - fraction_start + 1});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_json_bytes_append(
            &writer->output, (sxml_byte_view){.bytes = text.bytes + exponent_start,
                                              .byte_count = text.byte_count - exponent_start});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_json_write(writer, sxml_json_bytes_view(&writer->output));
    }
    return status;
}

/* Checks the escape sequence at text (which starts with a backslash) and
   writes its length, 0 when it is invalid (F&O 4.0, section 17.4.7,
   FOJS0007). */
static size_t sxml_json_escape_length(sxml_byte_view text) {
    if (text.byte_count < 2) {
        return 0;
    }
    switch (text.bytes[1]) {
    case '"':
    case '\\':
    case '/':
    case 'b':
    case 'f':
    case 'n':
    case 'r':
    case 't':
        return 2;
    case 'u':
        return sxml_json_read_hex4((sxml_byte_view){.bytes = text.bytes + 2,
                                                    .byte_count = text.byte_count - 2}) >= 0
                   ? 6
                   : 0;
    default:
        return 0;
    }
}

/* What to quote and how (F&O 4.0, section 17.4.7, "Strings are escaped as
   follows"). */
typedef struct sxml_json_quoting {
    sxml_byte_view text;
    /* escaped="true" or escaped-key="true": escape sequences in text are
       copied. */
    bool is_escaped_form;
} sxml_json_quoting;

/* Writes text as a JSON string into writer->output, quoted. */
static sxml_status sxml_json_quote(sxml_json_writer *writer, sxml_json_quoting quoting) {
    const sxml_byte_view text = quoting.text;
    sxml_json_bytes *output = &writer->output;
    output->byte_count = 0;
    sxml_status status = sxml_json_bytes_append(output, SXML_BYTE_VIEW_FROM_LITERAL("\""));
    size_t offset = 0;
    while (status == SXML_STATUS_OK && offset < text.byte_count) {
        const sxml_byte_view rest = {.bytes = text.bytes + offset,
                                     .byte_count = text.byte_count - offset};
        const sxml_json_decoded_codepoint decoded = sxml_json_decode_utf8(rest);
        const uint32_t codepoint = decoded.codepoint;
        if (codepoint == '\\' && quoting.is_escaped_form) {
            const size_t escape_length = sxml_json_escape_length(rest);
            if (escape_length == 0) {
                return sxml_error_record_report(writer->request->error_record,
                                                &sxml_json_invalid_escape_error);
            }
            status = sxml_json_bytes_append(
                output, (sxml_byte_view){.bytes = rest.bytes, .byte_count = escape_length});
            offset += escape_length;
            continue;
        }
        const uint8_t short_escape = codepoint == '"' ? '"'
                                     : codepoint == '/' && writer->request->escapes_solidus
                                         ? '/'
                                         : sxml_json_short_escape(codepoint);
        if (short_escape != 0) {
            const uint8_t escape[2] = {'\\', short_escape};
            status = sxml_json_bytes_append(output,
                                            (sxml_byte_view){.bytes = escape, .byte_count = 2});
        } else if ((codepoint >= 1 && codepoint <= 31) ||
                   (codepoint >= 127 && codepoint <= 159)) {
            status = sxml_json_append_unicode_escape(output, codepoint);
        } else {
            status = sxml_json_bytes_append(
                output, (sxml_byte_view){.bytes = rest.bytes, .byte_count = decoded.byte_count});
        }
        offset += decoded.byte_count;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_json_bytes_append(output, SXML_BYTE_VIEW_FROM_LITERAL("\""));
    }
    return status;
}

/* The normalized key (F&O 4.0, section 17.4.7): escape sequences of an
   escaped key decoded, into writer->content; surrogates as generalized
   UTF-8 so that \uD800 and \ud800 still compare equal. */
static sxml_status sxml_json_normalize_key(sxml_json_writer *writer,
                                           const sxml_json_attributes *attributes) {
    sxml_json_bytes *content = &writer->content;
    content->byte_count = 0;
    if (!attributes->is_escaped_key) {
        return sxml_json_bytes_append(content, attributes->key);
    }
    const sxml_byte_view key = attributes->key;
    size_t offset = 0;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && offset < key.byte_count) {
        const sxml_byte_view rest = {.bytes = key.bytes + offset,
                                     .byte_count = key.byte_count - offset};
        const size_t escape_length = rest.bytes[0] == '\\' ? sxml_json_escape_length(rest) : 0;
        if (rest.bytes[0] == '\\' && escape_length == 0) {
            return sxml_error_record_report(writer->request->error_record,
                                            &sxml_json_invalid_escape_error);
        }
        if (escape_length == 0) {
            status = sxml_json_bytes_append(content,
                                            (sxml_byte_view){.bytes = rest.bytes, .byte_count = 1});
            offset += 1;
            continue;
        }
        uint32_t codepoint = sxml_json_escape_codepoint(rest);
        offset += escape_length;
        if (sxml_json_is_high_surrogate(codepoint) && offset + 6 <= key.byte_count &&
            key.bytes[offset] == '\\' && key.bytes[offset + 1] == 'u') {
            const sxml_byte_view next = {.bytes = key.bytes + offset,
                                         .byte_count = key.byte_count - offset};
            const uint32_t low = sxml_json_escape_codepoint(next);
            if (sxml_json_escape_length(next) == 6 && sxml_json_is_low_surrogate(low)) {
                codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) + (low - 0xDC00U);
                offset += 6;
            }
        }
        status = sxml_json_append_codepoint(content, codepoint);
    }
    return status;
}

/* Writes the key of a map's child, after checking it is not a duplicate
   among the map's children. */
static sxml_status sxml_json_write_key(sxml_json_writer *writer,
                                       const sxml_json_writer_frame *frame,
                                       const sxml_json_attributes *attributes) {
    sxml_status status = sxml_json_normalize_key(writer, attributes);
    sxml_json_key_outcome outcome = {.is_duplicate = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_json_key_index_insert(
            &writer->key_index,
            &(sxml_json_key_query){.object_number = frame->object_number,
                                   .key = sxml_json_bytes_view(&writer->content)},
            &outcome);
    }
    if (status == SXML_STATUS_OK && outcome.is_duplicate) {
        return sxml_error_record_report(writer->request->error_record,
                                        &sxml_json_duplicate_representation_key_error);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_json_quote(writer, (sxml_json_quoting){
                                             .text = attributes->key,
                                             .is_escaped_form = attributes->is_escaped_key});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_json_bytes_append(&writer->output, SXML_BYTE_VIEW_FROM_LITERAL(":"));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_json_write(writer, sxml_json_bytes_view(&writer->output));
    }
    return status;
}

/* An element to write: where it is, and the frame of its map, if any. */
typedef struct sxml_json_element_visit {
    sxml_node_reference element;
    sxml_json_element_place place;
} sxml_json_element_visit;

/* Writes one element: leaves completely, maps and arrays up to their
   opening bracket, pushing a frame whose children the loop writes. */
static sxml_status sxml_json_write_element(sxml_json_writer *writer,
                                           sxml_json_element_visit visit) {
    sxml_node_description description;
    sxml_status status = sxml_node_describe(visit.element, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_json_element_kind kind = SXML_JSON_ELEMENT_KIND_NULL;
    if (description.node_kind != SXML_NODE_KIND_ELEMENT ||
        !sxml_json_element_kind_of(writer, description.node_name, &kind)) {
        return sxml_json_report_invalid(writer);
    }
    sxml_json_attributes attributes;
    status = sxml_json_read_attributes(
        writer,
        (sxml_json_described_element){
            .element = visit.element, .description = &description, .allows_text = true},
        &attributes);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if ((visit.place == SXML_JSON_ELEMENT_PLACE_IN_MAP && !attributes.has_key) ||
        (visit.place == SXML_JSON_ELEMENT_PLACE_IN_ARRAY && attributes.has_key)) {
        return sxml_json_report_invalid(writer);
    }
    if (visit.place == SXML_JSON_ELEMENT_PLACE_IN_MAP) {
        const sxml_json_writer_frame *frame =
            sxml_json_list_at(&writer->frames, writer->frames.element_count - 1);
        status = sxml_json_write_key(writer, frame, &attributes);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (kind == SXML_JSON_ELEMENT_KIND_MAP || kind == SXML_JSON_ELEMENT_KIND_ARRAY) {
        void *frame = NULL;
        status = sxml_json_list_reserve(&writer->frames, &frame);
        if (status == SXML_STATUS_OK) {
            status = sxml_json_write(writer, kind == SXML_JSON_ELEMENT_KIND_MAP
                                                 ? SXML_BYTE_VIEW_FROM_LITERAL("{")
                                                 : SXML_BYTE_VIEW_FROM_LITERAL("["));
        }
        if (status == SXML_STATUS_OK) {
            writer->next_object_number += 1;
            *(sxml_json_writer_frame *)frame = (sxml_json_writer_frame){
                .element = visit.element,
                .is_map = kind == SXML_JSON_ELEMENT_KIND_MAP,
                .has_next_child = description.has_children,
                .next_child = description.first_child,
                .has_written_member = false,
                .object_number = writer->next_object_number,
                .key_mark = sxml_json_key_index_mark(&writer->key_index)};
            writer->frames.element_count += 1;
        }
        return status;
    }
    status = sxml_json_read_leaf_content(
        writer, (sxml_json_described_element){.element = visit.element,
                                              .description = &description,
                                              .allows_text = kind != SXML_JSON_ELEMENT_KIND_NULL});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_json_boolean_reading boolean_value =
        sxml_json_read_boolean(sxml_json_bytes_view(&writer->content));
    switch (kind) {
    case SXML_JSON_ELEMENT_KIND_NULL:
        return sxml_json_write(writer, SXML_BYTE_VIEW_FROM_LITERAL("null"));
    case SXML_JSON_ELEMENT_KIND_BOOLEAN:
        if (!boolean_value.is_boolean) {
            return sxml_json_report_invalid(writer);
        }
        return sxml_json_write(writer, boolean_value.value ? SXML_BYTE_VIEW_FROM_LITERAL("true")
                                                     : SXML_BYTE_VIEW_FROM_LITERAL("false"));
    case SXML_JSON_ELEMENT_KIND_NUMBER:
        return sxml_json_write_number(writer, sxml_json_bytes_view(&writer->content));
    case SXML_JSON_ELEMENT_KIND_STRING:
        status = sxml_json_quote(writer,
                                 (sxml_json_quoting){.text = sxml_json_bytes_view(&writer->content),
                                                     .is_escaped_form = attributes.is_escaped});
        return status == SXML_STATUS_OK
                   ? sxml_json_write(writer, sxml_json_bytes_view(&writer->output))
                   : status;
    case SXML_JSON_ELEMENT_KIND_MAP:
    case SXML_JSON_ELEMENT_KIND_ARRAY:
        break;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* Moves the innermost map or array to its next child: elements are
   written, whitespace text, comments and processing instructions are
   skipped, other text is invalid; at the end the bracket closes. */
static sxml_status sxml_json_write_next_child(sxml_json_writer *writer) {
    sxml_json_writer_frame *frame =
        sxml_json_list_at(&writer->frames, writer->frames.element_count - 1);
    while (frame->has_next_child) {
        const sxml_node_reference child = {.tree = frame->element.tree,
                                           .node_index = frame->next_child,
                                           .namespace_number = 0};
        sxml_node_description description;
        sxml_status status = sxml_node_describe(child, &description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        frame->has_next_child = description.has_next_sibling;
        frame->next_child = description.next_sibling;
        if (description.node_kind == SXML_NODE_KIND_TEXT) {
            sxml_byte_view text = {.bytes = NULL, .byte_count = 0};
            status = sxml_node_get_content(child, &text);
            if (status == SXML_STATUS_OK && sxml_xml_text_trim_whitespace(text).byte_count > 0) {
                status = sxml_json_report_invalid(writer);
            }
            if (status != SXML_STATUS_OK) {
                return status;
            }
            continue;
        }
        if (description.node_kind != SXML_NODE_KIND_ELEMENT) {
            continue;
        }
        if (frame->has_written_member) {
            status = sxml_json_write(writer, SXML_BYTE_VIEW_FROM_LITERAL(","));
        }
        frame->has_written_member = true;
        if (status != SXML_STATUS_OK) {
            return status;
        }
        /* frame may move when the element pushes a frame. */
        return sxml_json_write_element(
            writer, (sxml_json_element_visit){.element = child,
                                              .place = frame->is_map
                                                           ? SXML_JSON_ELEMENT_PLACE_IN_MAP
                                                           : SXML_JSON_ELEMENT_PLACE_IN_ARRAY});
    }
    const bool is_map = frame->is_map;
    sxml_json_key_index_restore(&writer->key_index, frame->key_mark);
    writer->frames.element_count -= 1;
    return sxml_json_write(writer, is_map ? SXML_BYTE_VIEW_FROM_LITERAL("}")
                                          : SXML_BYTE_VIEW_FROM_LITERAL("]"));
}

/* The element a document node stands for: its one element child, with no
   text children (F&O 4.0, section 17.4.7). */
static sxml_status sxml_json_find_document_element(const sxml_json_writer *writer,
                                                   sxml_node_reference document,
                                                   sxml_node_reference *element) {
    sxml_node_description description;
    sxml_status status = sxml_node_describe(document, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (description.node_kind == SXML_NODE_KIND_ELEMENT) {
        *element = document;
        return SXML_STATUS_OK;
    }
    if (description.node_kind != SXML_NODE_KIND_DOCUMENT) {
        return sxml_json_report_invalid(writer);
    }
    bool has_element = false;
    bool has_child = description.has_children;
    sxml_node_index child_index = description.first_child;
    while (has_child) {
        const sxml_node_reference child = {
            .tree = document.tree, .node_index = child_index, .namespace_number = 0};
        sxml_node_description child_description;
        status = sxml_node_describe(child, &child_description);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (child_description.node_kind == SXML_NODE_KIND_TEXT ||
            (child_description.node_kind == SXML_NODE_KIND_ELEMENT && has_element)) {
            return sxml_json_report_invalid(writer);
        }
        if (child_description.node_kind == SXML_NODE_KIND_ELEMENT) {
            has_element = true;
            *element = child;
        }
        has_child = child_description.has_next_sibling;
        child_index = child_description.next_sibling;
    }
    return has_element ? SXML_STATUS_OK : sxml_json_report_invalid(writer);
}

sxml_status sxml_json_write_xml_representation(sxml_item_heap *item_heap,
                                               const sxml_json_writing_request *request,
                                               sxml_output_stream output_stream) {
    if (item_heap == NULL || request == NULL || request->node.tree == NULL ||
        sxml_allocator_is_null(request->allocator) || output_stream.operations == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_allocator allocator = request->allocator;
    sxml_json_writer writer = {
        .item_heap = item_heap,
        .request = request,
        .output_stream = output_stream,
        .name_pool = sxml_item_heap_get_name_pool(item_heap),
        .frames = SXML_JSON_LIST(allocator, sxml_json_writer_frame),
        .next_object_number = 0,
        .key_index = sxml_json_key_index_make(allocator),
        .content = {.allocator = allocator, .bytes = NULL, .byte_count = 0, .byte_capacity = 0},
        .output = {.allocator = allocator, .bytes = NULL, .byte_count = 0, .byte_capacity = 0},
    };
    sxml_status status = sxml_json_intern_names(writer.name_pool, &writer.names);
    sxml_node_reference element = request->node;
    if (status == SXML_STATUS_OK) {
        status = sxml_json_find_document_element(&writer, request->node, &element);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_json_write_element(
            &writer, (sxml_json_element_visit){.element = element,
                                               .place = SXML_JSON_ELEMENT_PLACE_TOP});
    }
    while (status == SXML_STATUS_OK && writer.frames.element_count > 0) {
        status = sxml_json_write_next_child(&writer);
    }
    sxml_json_list_release(&writer.frames);
    sxml_json_key_index_release(&writer.key_index);
    sxml_json_bytes_release(&writer.content);
    sxml_json_bytes_release(&writer.output);
    return status;
}
