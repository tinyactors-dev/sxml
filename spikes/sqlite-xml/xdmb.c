/*
 * XDMB, a spike (README.md). Encoder over the public tree API, and an
 * sxml_tree implementation reading the bytes in place.
 *
 * Layout, little-endian, varints are unsigned LEB128:
 *
 *   "XDMB" version(1) widths(1)      widths: bits 0-1 node, 2-3 name,
 *                                    4-5 payload; code c is 1 << c bytes
 *   varint node_count string_count name_count set_count text_heap_size
 *   varint document_uri_string base_uri_string
 *   strings:  string_count x (varint length, bytes)   string 0 is ""
 *   names:    name_count x (varint uri, local, prefix)   name 0 is none
 *   sets:     set_count x (varint binding_count, pairs of varint prefix,
 *             uri), each set sorted by prefix string, in-scope bindings
 *   nodes:    node_count x record, fixed size per document:
 *               kind_flags(1) name(wn) parent(wi) last_descendant(wi)
 *               payload(wp)
 *             payload: element = set number, content nodes = offset of
 *             their text in the heap, document = 0
 *   text heap: entries of varint length, bytes; equal texts shared
 *
 * Nodes are in sxml's document order (preorder, attributes first), so
 * the descendants of n are n+1 .. last_descendant(n), as the arena tree
 * and the XPath accelerator's pre/size encoding have it.
 */
#include "xdmb.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "sxml/name.h"
#include "sxml/types.h"

enum {
    XDMB_VERSION = 1,
    XDMB_KIND_MASK = 0x07,
    XDMB_FLAG_ID = 0x08,
    XDMB_FLAG_IDREFS = 0x10,
    XDMB_FLAG_NILLED = 0x20,
    XDMB_FLAG_CDATA = 0x40,
    XDMB_FLAG_NO_ESCAPING = 0x80,
};

static const uint8_t xdmb_magic[4] = {'X', 'D', 'M', 'B'};

/* ------------------------------------------------------------ Buffers */

typedef struct xdmb_buffer {
    uint8_t *bytes;
    size_t byte_count;
    size_t capacity;
    bool failed;
} xdmb_buffer;

static void xdmb_buffer_append(xdmb_buffer *buffer, const void *bytes, size_t byte_count) {
    if (buffer->failed) {
        return;
    }
    if (buffer->byte_count + byte_count > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity * 2 : 256;
        while (capacity < buffer->byte_count + byte_count) {
            capacity *= 2;
        }
        uint8_t *grown = realloc(buffer->bytes, capacity);
        if (grown == NULL) {
            buffer->failed = true;
            return;
        }
        buffer->bytes = grown;
        buffer->capacity = capacity;
    }
    if (byte_count > 0) {
        memcpy(buffer->bytes + buffer->byte_count, bytes, byte_count);
    }
    buffer->byte_count += byte_count;
}

static void xdmb_buffer_varint(xdmb_buffer *buffer, uint64_t value) {
    uint8_t bytes[10];
    size_t count = 0;
    do {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        bytes[count++] = byte | (value ? 0x80 : 0);
    } while (value);
    xdmb_buffer_append(buffer, bytes, count);
}

static void xdmb_buffer_fixed(xdmb_buffer *buffer, uint32_t value, unsigned width) {
    uint8_t bytes[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16),
                        (uint8_t)(value >> 24)};
    xdmb_buffer_append(buffer, bytes, width);
}

/* -------------------------------------------- Interner: bytes -> number */

typedef struct xdmb_interner {
    uint32_t *slots; /* 0 empty, else entry + 1 */
    size_t slot_count;
    xdmb_buffer keys; /* the keys, back to back */
    size_t *key_offsets;
    size_t *key_lengths;
    uint32_t entry_count;
    size_t entry_capacity;
    bool failed;
} xdmb_interner;

static uint64_t xdmb_hash(const uint8_t *bytes, size_t byte_count) {
    uint64_t hash = 1469598103934665603ULL;
    for (size_t index = 0; index < byte_count; index++) {
        hash = (hash ^ bytes[index]) * 1099511628211ULL;
    }
    return hash;
}

static bool xdmb_interner_grow(xdmb_interner *interner) {
    size_t slot_count = interner->slot_count ? interner->slot_count * 2 : 64;
    uint32_t *slots = calloc(slot_count, sizeof *slots);
    if (slots == NULL) {
        return false;
    }
    for (uint32_t entry = 0; entry < interner->entry_count; entry++) {
        const uint8_t *key = interner->keys.bytes + interner->key_offsets[entry];
        size_t slot = xdmb_hash(key, interner->key_lengths[entry]) & (slot_count - 1);
        while (slots[slot]) {
            slot = (slot + 1) & (slot_count - 1);
        }
        slots[slot] = entry + 1;
    }
    free(interner->slots);
    interner->slots = slots;
    interner->slot_count = slot_count;
    return true;
}

/* The number of key, from 0 in order of first insertion; *is_new tells. */
static uint32_t xdmb_intern(xdmb_interner *interner, sxml_byte_view key, bool *is_new) {
    *is_new = false;
    if (interner->failed) {
        return 0;
    }
    if ((interner->entry_count + 1) * 2 > interner->slot_count && !xdmb_interner_grow(interner)) {
        interner->failed = true;
        return 0;
    }
    size_t slot = xdmb_hash(key.bytes, key.byte_count) & (interner->slot_count - 1);
    while (interner->slots[slot]) {
        uint32_t entry = interner->slots[slot] - 1;
        if (interner->key_lengths[entry] == key.byte_count &&
            memcmp(interner->keys.bytes + interner->key_offsets[entry], key.bytes, key.byte_count) == 0) {
            return entry;
        }
        slot = (slot + 1) & (interner->slot_count - 1);
    }
    if (interner->entry_count == interner->entry_capacity) {
        size_t capacity = interner->entry_capacity ? interner->entry_capacity * 2 : 64;
        size_t *offsets = realloc(interner->key_offsets, capacity * sizeof *offsets);
        if (offsets) interner->key_offsets = offsets;
        size_t *lengths = realloc(interner->key_lengths, capacity * sizeof *lengths);
        if (lengths) interner->key_lengths = lengths;
        if (!offsets || !lengths) {
            interner->failed = true;
            return 0;
        }
        interner->entry_capacity = capacity;
    }
    uint32_t entry = interner->entry_count++;
    interner->key_offsets[entry] = interner->keys.byte_count;
    interner->key_lengths[entry] = key.byte_count;
    xdmb_buffer_append(&interner->keys, key.bytes, key.byte_count);
    interner->failed |= interner->keys.failed;
    interner->slots[slot] = entry + 1;
    *is_new = true;
    return entry;
}

static void xdmb_interner_free(xdmb_interner *interner) {
    free(interner->slots);
    free(interner->keys.bytes);
    free(interner->key_offsets);
    free(interner->key_lengths);
}

/* --------------------------------------------------------------- Encoder */

typedef struct xdmb_record {
    uint8_t kind_flags;
    uint32_t name;
    uint32_t parent;
    uint32_t last_descendant;
    uint32_t payload;
} xdmb_record;

typedef struct xdmb_encoder {
    sxml_name_pool *name_pool;
    xdmb_interner strings; /* "" is string 0 */
    xdmb_buffer string_table;
    xdmb_interner names;
    xdmb_buffer name_table;
    xdmb_interner sets;
    xdmb_buffer set_table;
    xdmb_interner texts;
    xdmb_buffer text_heap;
} xdmb_encoder;

static uint32_t xdmb_encoder_string(xdmb_encoder *encoder, sxml_byte_view text) {
    bool is_new = false;
    uint32_t number = xdmb_intern(&encoder->strings, text, &is_new);
    if (is_new && number > 0) {
        xdmb_buffer_varint(&encoder->string_table, text.byte_count);
        xdmb_buffer_append(&encoder->string_table, text.bytes, text.byte_count);
    }
    return number;
}

static sxml_byte_view xdmb_uri_text(xdmb_encoder *encoder, sxml_namespace_uri_identifier uri) {
    sxml_byte_view text = {0};
    if (uri.index != 0 &&
        sxml_name_pool_get_namespace_uri(encoder->name_pool, uri, &text) != SXML_STATUS_OK) {
        text = (sxml_byte_view){0};
    }
    return text;
}

static sxml_byte_view xdmb_prefix_text(xdmb_encoder *encoder, sxml_namespace_prefix_identifier prefix) {
    sxml_byte_view text = {0};
    if (prefix.index != 0 &&
        sxml_name_pool_get_namespace_prefix(encoder->name_pool, prefix, &text) != SXML_STATUS_OK) {
        text = (sxml_byte_view){0};
    }
    return text;
}

/* The name number (from 1) of a node's name and prefix. */
static uint32_t xdmb_encoder_name(xdmb_encoder *encoder, const sxml_node_description *description) {
    uint32_t key[2] = {description->node_name.index, description->namespace_prefix.index};
    bool is_new = false;
    uint32_t number = xdmb_intern(&encoder->names,
                                  (sxml_byte_view){(const uint8_t *)key, sizeof key}, &is_new);
    if (is_new) {
        sxml_name_parts parts = {0};
        if (sxml_name_pool_get_name_parts(encoder->name_pool, description->node_name, &parts) != SXML_STATUS_OK) {
            encoder->names.failed = true;
        }
        xdmb_buffer_varint(&encoder->name_table, xdmb_encoder_string(encoder, xdmb_uri_text(encoder, parts.namespace_uri)));
        xdmb_buffer_varint(&encoder->name_table, xdmb_encoder_string(encoder, parts.local_name));
        xdmb_buffer_varint(&encoder->name_table,
                           xdmb_encoder_string(encoder, xdmb_prefix_text(encoder, description->namespace_prefix)));
    }
    return number + 1;
}

typedef struct xdmb_binding_text {
    sxml_byte_view prefix;
    sxml_byte_view uri;
} xdmb_binding_text;

static int xdmb_compare_bindings(const void *first, const void *second) {
    const xdmb_binding_text *a = first;
    const xdmb_binding_text *b = second;
    size_t common = a->prefix.byte_count < b->prefix.byte_count ? a->prefix.byte_count : b->prefix.byte_count;
    int order = common ? memcmp(a->prefix.bytes, b->prefix.bytes, common) : 0;
    if (order != 0) {
        return order;
    }
    return (a->prefix.byte_count > b->prefix.byte_count) - (a->prefix.byte_count < b->prefix.byte_count);
}

/* The set number of an element's in-scope namespaces, sorted by prefix. */
static uint32_t xdmb_encoder_set(xdmb_encoder *encoder, sxml_namespace_binding_view bindings) {
    xdmb_binding_text stack_texts[16];
    xdmb_binding_text *texts = stack_texts;
    if (bindings.namespace_binding_count > 16) {
        texts = malloc(bindings.namespace_binding_count * sizeof *texts);
        if (texts == NULL) {
            encoder->sets.failed = true;
            return 0;
        }
    }
    for (size_t index = 0; index < bindings.namespace_binding_count; index++) {
        texts[index].prefix = xdmb_prefix_text(encoder, bindings.namespace_bindings[index].namespace_prefix);
        texts[index].uri = xdmb_uri_text(encoder, bindings.namespace_bindings[index].namespace_uri);
    }
    qsort(texts, bindings.namespace_binding_count, sizeof *texts, xdmb_compare_bindings);
    /* The key: the string numbers of the sorted pairs. */
    xdmb_buffer key = {0};
    for (size_t index = 0; index < bindings.namespace_binding_count; index++) {
        uint32_t pair[2] = {xdmb_encoder_string(encoder, texts[index].prefix),
                            xdmb_encoder_string(encoder, texts[index].uri)};
        xdmb_buffer_append(&key, pair, sizeof pair);
    }
    bool is_new = false;
    uint32_t number = xdmb_intern(&encoder->sets, (sxml_byte_view){key.bytes, key.byte_count}, &is_new);
    if (is_new) {
        xdmb_buffer_varint(&encoder->set_table, bindings.namespace_binding_count);
        const uint32_t *pairs = (const uint32_t *)key.bytes;
        for (size_t index = 0; index < bindings.namespace_binding_count * 2; index++) {
            xdmb_buffer_varint(&encoder->set_table, pairs[index]);
        }
    }
    encoder->sets.failed |= key.failed;
    free(key.bytes);
    if (texts != stack_texts) {
        free(texts);
    }
    return number;
}

static unsigned xdmb_width_code(uint32_t maximum) {
    return maximum <= 0xFF ? 0 : maximum <= 0xFFFF ? 1 : 2;
}

sxml_status xdmb_encode(sxml_tree *tree, xdmb_bytes *encoded) {
    sxml_tree_description tree_description = {0};
    sxml_status status = sxml_tree_describe(tree, &tree_description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t node_count = tree_description.node_count;
    xdmb_record *records = calloc(node_count ? node_count : 1, sizeof *records);
    uint32_t *text_offsets = NULL; /* by text number */
    size_t text_offset_capacity = 0;
    xdmb_encoder encoder = {.name_pool = tree_description.name_pool};
    bool is_new = false;
    xdmb_intern(&encoder.strings, (sxml_byte_view){(const uint8_t *)"", 0}, &is_new);
    uint32_t document_uri = xdmb_encoder_string(&encoder, tree_description.document_uri);
    uint32_t base_uri = xdmb_encoder_string(&encoder, tree_description.base_uri);
    if (records == NULL) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    uint32_t maximum_payload = 0;
    for (uint32_t index = 0; index < node_count && status == SXML_STATUS_OK; index++) {
        sxml_node_reference node = {.tree = tree, .node_index = {index}, .namespace_number = 0};
        sxml_node_description description = {0};
        status = sxml_node_describe(node, &description);
        if (status != SXML_STATUS_OK) {
            break;
        }
        xdmb_record *record = &records[index];
        record->kind_flags = (uint8_t)description.node_kind |
                             (description.is_id ? XDMB_FLAG_ID : 0) |
                             (description.is_idrefs ? XDMB_FLAG_IDREFS : 0) |
                             (description.is_nilled ? XDMB_FLAG_NILLED : 0) |
                             (description.is_cdata_section ? XDMB_FLAG_CDATA : 0) |
                             (description.disables_output_escaping ? XDMB_FLAG_NO_ESCAPING : 0);
        record->parent = description.has_parent ? description.parent.index : 0;
        record->last_descendant = description.last_descendant.index;
        switch (description.node_kind) {
        case SXML_NODE_KIND_ELEMENT: {
            record->name = xdmb_encoder_name(&encoder, &description);
            sxml_namespace_binding_view bindings = {0};
            status = sxml_node_get_in_scope_namespaces(node, &bindings);
            record->payload = xdmb_encoder_set(&encoder, bindings);
            break;
        }
        case SXML_NODE_KIND_ATTRIBUTE:
        case SXML_NODE_KIND_PROCESSING_INSTRUCTION:
            record->name = xdmb_encoder_name(&encoder, &description);
            /* fall through */
        case SXML_NODE_KIND_TEXT:
        case SXML_NODE_KIND_COMMENT:
        case SXML_NODE_KIND_NAMESPACE: {
            if (description.node_kind == SXML_NODE_KIND_NAMESPACE) {
                record->name = xdmb_encoder_name(&encoder, &description);
            }
            sxml_byte_view content = {0};
            status = sxml_node_get_content(node, &content);
            uint32_t number = xdmb_intern(&encoder.texts, content, &is_new);
            if (is_new) {
                if (number >= text_offset_capacity) {
                    text_offset_capacity = text_offset_capacity ? text_offset_capacity * 2 : 64;
                    uint32_t *grown = realloc(text_offsets, text_offset_capacity * sizeof *grown);
                    if (grown == NULL) {
                        status = SXML_STATUS_OUT_OF_MEMORY;
                        break;
                    }
                    text_offsets = grown;
                }
                text_offsets[number] = (uint32_t)encoder.text_heap.byte_count;
                xdmb_buffer_varint(&encoder.text_heap, content.byte_count);
                xdmb_buffer_append(&encoder.text_heap, content.bytes, content.byte_count);
            }
            if (text_offsets != NULL) {
                record->payload = text_offsets[number];
            }
            break;
        }
        default:
            break;
        }
        if (record->payload > maximum_payload) {
            maximum_payload = record->payload;
        }
    }
    bool failed = encoder.strings.failed || encoder.names.failed || encoder.sets.failed ||
                  encoder.texts.failed || encoder.string_table.failed || encoder.name_table.failed ||
                  encoder.set_table.failed || encoder.text_heap.failed;
    if (status == SXML_STATUS_OK && failed) {
        status = SXML_STATUS_OUT_OF_MEMORY;
    }
    xdmb_buffer output = {0};
    if (status == SXML_STATUS_OK) {
        unsigned node_code = xdmb_width_code(node_count);
        unsigned name_code = xdmb_width_code(encoder.names.entry_count);
        unsigned payload_code = xdmb_width_code(maximum_payload);
        uint8_t header[6] = {xdmb_magic[0], xdmb_magic[1], xdmb_magic[2], xdmb_magic[3], XDMB_VERSION,
                             (uint8_t)(node_code | name_code << 2 | payload_code << 4)};
        xdmb_buffer_append(&output, header, sizeof header);
        xdmb_buffer_varint(&output, node_count);
        xdmb_buffer_varint(&output, encoder.strings.entry_count);
        xdmb_buffer_varint(&output, encoder.names.entry_count);
        xdmb_buffer_varint(&output, encoder.sets.entry_count);
        xdmb_buffer_varint(&output, encoder.text_heap.byte_count);
        xdmb_buffer_varint(&output, document_uri);
        xdmb_buffer_varint(&output, base_uri);
        xdmb_buffer_append(&output, encoder.string_table.bytes, encoder.string_table.byte_count);
        xdmb_buffer_append(&output, encoder.name_table.bytes, encoder.name_table.byte_count);
        xdmb_buffer_append(&output, encoder.set_table.bytes, encoder.set_table.byte_count);
        for (uint32_t index = 0; index < node_count; index++) {
            const xdmb_record *record = &records[index];
            xdmb_buffer_append(&output, &record->kind_flags, 1);
            xdmb_buffer_fixed(&output, record->name, 1u << name_code);
            xdmb_buffer_fixed(&output, record->parent, 1u << node_code);
            xdmb_buffer_fixed(&output, record->last_descendant, 1u << node_code);
            xdmb_buffer_fixed(&output, record->payload, 1u << payload_code);
        }
        xdmb_buffer_append(&output, encoder.text_heap.bytes, encoder.text_heap.byte_count);
        if (output.failed) {
            status = SXML_STATUS_OUT_OF_MEMORY;
        }
    }
    free(records);
    free(text_offsets);
    xdmb_interner_free(&encoder.strings);
    xdmb_interner_free(&encoder.names);
    xdmb_interner_free(&encoder.sets);
    xdmb_interner_free(&encoder.texts);
    free(encoder.string_table.bytes);
    free(encoder.name_table.bytes);
    free(encoder.set_table.bytes);
    free(encoder.text_heap.bytes);
    if (status != SXML_STATUS_OK) {
        free(output.bytes);
        return status;
    }
    encoded->bytes = output.bytes;
    encoded->byte_count = output.byte_count;
    return SXML_STATUS_OK;
}

void xdmb_free(xdmb_bytes *encoded) {
    free(encoded->bytes);
    encoded->bytes = NULL;
    encoded->byte_count = 0;
}

int xdmb_is_encoded(sxml_byte_view bytes) {
    return bytes.byte_count >= 6 && memcmp(bytes.bytes, xdmb_magic, 4) == 0 && bytes.bytes[4] == XDMB_VERSION;
}

/* ---------------------------------------------------------------- Reader */

typedef struct xdmb_reader {
    sxml_byte_view bytes;
    sxml_name_pool *name_pool;
    sxml_allocator allocator;
    sxml_tree *tree;
    uint32_t node_count;
    unsigned node_width;
    unsigned name_width;
    unsigned payload_width;
    unsigned record_size;
    const uint8_t *node_table;
    const uint8_t *text_heap;
    size_t text_heap_size;
    sxml_byte_view document_uri;
    sxml_byte_view base_uri;
    /* By name number - 1. */
    sxml_name_identifier *names;
    sxml_namespace_prefix_identifier *prefixes;
    /* Set s is bindings[set_starts[s] .. set_starts[s + 1]). */
    sxml_namespace_binding *bindings;
    uint32_t *set_starts;
} xdmb_reader;

typedef struct xdmb_cursor {
    const uint8_t *at;
    const uint8_t *end;
    bool failed;
} xdmb_cursor;

static uint64_t xdmb_read_varint(xdmb_cursor *cursor) {
    uint64_t value = 0;
    unsigned shift = 0;
    while (cursor->at < cursor->end && shift < 64) {
        uint8_t byte = *cursor->at++;
        value |= (uint64_t)(byte & 0x7F) << shift;
        if (!(byte & 0x80)) {
            return value;
        }
        shift += 7;
    }
    cursor->failed = true;
    return 0;
}

static inline uint32_t xdmb_read_fixed(const uint8_t *at, unsigned width) {
    switch (width) {
    case 1:
        return at[0];
    case 2:
        return (uint32_t)at[0] | (uint32_t)at[1] << 8;
    default:
        return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
    }
}

static inline const uint8_t *xdmb_record_at(const xdmb_reader *reader, uint32_t index) {
    return reader->node_table + (size_t)index * reader->record_size;
}

static inline uint8_t xdmb_kind_flags(const xdmb_reader *reader, uint32_t index) {
    return xdmb_record_at(reader, index)[0];
}

static inline uint32_t xdmb_name(const xdmb_reader *reader, uint32_t index) {
    return xdmb_read_fixed(xdmb_record_at(reader, index) + 1, reader->name_width);
}

static inline uint32_t xdmb_parent(const xdmb_reader *reader, uint32_t index) {
    return xdmb_read_fixed(xdmb_record_at(reader, index) + 1 + reader->name_width, reader->node_width);
}

static inline uint32_t xdmb_last_descendant(const xdmb_reader *reader, uint32_t index) {
    return xdmb_read_fixed(xdmb_record_at(reader, index) + 1 + reader->name_width + reader->node_width,
                           reader->node_width);
}

static inline uint32_t xdmb_payload(const xdmb_reader *reader, uint32_t index) {
    return xdmb_read_fixed(xdmb_record_at(reader, index) + 1 + reader->name_width + 2 * reader->node_width,
                           reader->payload_width);
}

static sxml_status xdmb_describe_tree(void *state, sxml_tree_description *description) {
    const xdmb_reader *reader = state;
    *description = (sxml_tree_description){.node_count = reader->node_count,
                                           .document_uri = reader->document_uri,
                                           .base_uri = reader->base_uri,
                                           .name_pool = reader->name_pool};
    return SXML_STATUS_OK;
}

static uint32_t xdmb_attribute_count(const xdmb_reader *reader, uint32_t index) {
    uint32_t count = 0;
    while (index + 1 + count < reader->node_count &&
           (xdmb_kind_flags(reader, index + 1 + count) & XDMB_KIND_MASK) == SXML_NODE_KIND_ATTRIBUTE &&
           xdmb_parent(reader, index + 1 + count) == index) {
        count++;
    }
    return count;
}

static sxml_status xdmb_describe_node(void *state, sxml_node_index node_index,
                                      sxml_node_description *description) {
    const xdmb_reader *reader = state;
    uint32_t index = node_index.index;
    uint8_t kind_flags = xdmb_kind_flags(reader, index);
    sxml_node_kind kind = (sxml_node_kind)(kind_flags & XDMB_KIND_MASK);
    uint32_t name = xdmb_name(reader, index);
    uint32_t last = xdmb_last_descendant(reader, index);
    *description = (sxml_node_description){
        .node_kind = kind,
        .node_name = name ? reader->names[name - 1] : (sxml_name_identifier){0},
        .namespace_prefix = name ? reader->prefixes[name - 1] : (sxml_namespace_prefix_identifier){0},
        .has_parent = index != 0,
        .parent = {index != 0 ? xdmb_parent(reader, index) : 0},
        .last_descendant = {last},
        .is_id = kind_flags & XDMB_FLAG_ID,
        .is_idrefs = kind_flags & XDMB_FLAG_IDREFS,
        .is_nilled = kind_flags & XDMB_FLAG_NILLED,
        .is_cdata_section = kind_flags & XDMB_FLAG_CDATA,
        .disables_output_escaping = kind_flags & XDMB_FLAG_NO_ESCAPING,
        .type_annotation = sxml_builtin_type_identifier(
            kind == SXML_NODE_KIND_ELEMENT     ? SXML_TYPE_XS_UNTYPED
            : kind == SXML_NODE_KIND_ATTRIBUTE ? SXML_TYPE_XS_UNTYPED_ATOMIC
                                               : SXML_TYPE_NONE),
    };
    if (kind == SXML_NODE_KIND_ELEMENT || kind == SXML_NODE_KIND_DOCUMENT) {
        uint32_t attributes = kind == SXML_NODE_KIND_ELEMENT ? xdmb_attribute_count(reader, index) : 0;
        description->attribute_count = attributes;
        if (last > index + attributes) {
            description->has_children = true;
            description->first_child.index = index + 1 + attributes;
            /* The last child: climb from the last descendant. */
            uint32_t child = last;
            while (xdmb_parent(reader, child) != index) {
                child = xdmb_parent(reader, child);
            }
            description->last_child.index = child;
        }
    }
    if (index != 0 && kind != SXML_NODE_KIND_ATTRIBUTE) {
        uint32_t parent = xdmb_parent(reader, index);
        uint32_t next = last + 1;
        if (next < reader->node_count && xdmb_parent(reader, next) == parent) {
            description->has_next_sibling = true;
            description->next_sibling.index = next;
        }
        /* The previous sibling: climb from the node before this one. */
        uint32_t before = index - 1;
        if (before != parent) {
            while (xdmb_parent(reader, before) != parent) {
                before = xdmb_parent(reader, before);
            }
            if ((xdmb_kind_flags(reader, before) & XDMB_KIND_MASK) != SXML_NODE_KIND_ATTRIBUTE) {
                description->has_previous_sibling = true;
                description->previous_sibling.index = before;
            }
        }
    }
    return SXML_STATUS_OK;
}

static sxml_status xdmb_get_node_content(void *state, sxml_node_index node_index, sxml_byte_view *content) {
    const xdmb_reader *reader = state;
    sxml_node_kind kind = (sxml_node_kind)(xdmb_kind_flags(reader, node_index.index) & XDMB_KIND_MASK);
    if (kind == SXML_NODE_KIND_DOCUMENT || kind == SXML_NODE_KIND_ELEMENT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    uint32_t offset = xdmb_payload(reader, node_index.index);
    if (offset >= reader->text_heap_size) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    xdmb_cursor cursor = {reader->text_heap + offset, reader->text_heap + reader->text_heap_size, false};
    uint64_t length = xdmb_read_varint(&cursor);
    if (cursor.failed || length > (uint64_t)(cursor.end - cursor.at)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *content = (sxml_byte_view){cursor.at, (size_t)length};
    return SXML_STATUS_OK;
}

static sxml_status xdmb_get_in_scope_namespaces(void *state, sxml_node_index element,
                                                sxml_namespace_binding_view *bindings) {
    const xdmb_reader *reader = state;
    if ((xdmb_kind_flags(reader, element.index) & XDMB_KIND_MASK) != SXML_NODE_KIND_ELEMENT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    uint32_t set = xdmb_payload(reader, element.index);
    *bindings = (sxml_namespace_binding_view){reader->bindings + reader->set_starts[set],
                                              reader->set_starts[set + 1] - reader->set_starts[set]};
    return SXML_STATUS_OK;
}

static const sxml_tree_operations xdmb_tree_operations = {
    .describe_tree = xdmb_describe_tree,
    .describe_node = xdmb_describe_node,
    .get_node_content = xdmb_get_node_content,
    .get_in_scope_namespaces = xdmb_get_in_scope_namespaces,
};

static void xdmb_reader_free(xdmb_reader *reader) {
    free(reader->names);
    free(reader->prefixes);
    free(reader->bindings);
    free(reader->set_starts);
    free(reader);
}

static void xdmb_release(void *state, sxml_tree *tree) {
    xdmb_reader *reader = state;
    sxml_tree_destroy(tree);
    xdmb_reader_free(reader);
}

static const sxml_tree_owner_operations xdmb_owner_operations = {.release = xdmb_release};

sxml_status xdmb_open(const xdmb_open_request *request, sxml_tree **tree) {
    if (!xdmb_is_encoded(request->bytes)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    xdmb_reader *reader = calloc(1, sizeof *reader);
    if (reader == NULL) {
        return SXML_STATUS_OUT_OF_MEMORY;
    }
    reader->bytes = request->bytes;
    reader->name_pool = sxml_item_heap_get_name_pool(request->item_heap);
    reader->allocator = request->allocator;
    uint8_t widths = request->bytes.bytes[5];
    reader->node_width = 1u << (widths & 3);
    reader->name_width = 1u << ((widths >> 2) & 3);
    reader->payload_width = 1u << ((widths >> 4) & 3);
    reader->record_size = 1 + reader->name_width + 2 * reader->node_width + reader->payload_width;
    xdmb_cursor cursor = {request->bytes.bytes + 6, request->bytes.bytes + request->bytes.byte_count, false};
    uint64_t node_count = xdmb_read_varint(&cursor);
    uint64_t string_count = xdmb_read_varint(&cursor);
    uint64_t name_count = xdmb_read_varint(&cursor);
    uint64_t set_count = xdmb_read_varint(&cursor);
    uint64_t text_heap_size = xdmb_read_varint(&cursor);
    uint64_t document_uri = xdmb_read_varint(&cursor);
    uint64_t base_uri = xdmb_read_varint(&cursor);
    sxml_status status = SXML_STATUS_OK;
    sxml_byte_view *strings = NULL;
    if (cursor.failed || node_count == 0 || node_count >= UINT32_MAX || string_count == 0 ||
        string_count > request->bytes.byte_count || name_count > request->bytes.byte_count ||
        set_count > request->bytes.byte_count) {
        status = SXML_STATUS_INVALID_ARGUMENT;
        goto done;
    }
    reader->node_count = (uint32_t)node_count;
    strings = calloc(string_count, sizeof *strings);
    reader->names = calloc(name_count ? name_count : 1, sizeof *reader->names);
    reader->prefixes = calloc(name_count ? name_count : 1, sizeof *reader->prefixes);
    reader->set_starts = calloc(set_count + 1, sizeof *reader->set_starts);
    if (!strings || !reader->names || !reader->prefixes || !reader->set_starts) {
        status = SXML_STATUS_OUT_OF_MEMORY;
        goto done;
    }
    for (uint64_t index = 1; index < string_count; index++) {
        uint64_t length = xdmb_read_varint(&cursor);
        if (cursor.failed || length > (uint64_t)(cursor.end - cursor.at)) {
            status = SXML_STATUS_INVALID_ARGUMENT;
            goto done;
        }
        strings[index] = (sxml_byte_view){cursor.at, (size_t)length};
        cursor.at += length;
    }
    if (document_uri >= string_count || base_uri >= string_count) {
        status = SXML_STATUS_INVALID_ARGUMENT;
        goto done;
    }
    reader->document_uri = strings[document_uri];
    reader->base_uri = strings[base_uri];
    for (uint64_t index = 0; index < name_count && status == SXML_STATUS_OK; index++) {
        uint64_t uri = xdmb_read_varint(&cursor);
        uint64_t local = xdmb_read_varint(&cursor);
        uint64_t prefix = xdmb_read_varint(&cursor);
        if (cursor.failed || uri >= string_count || local == 0 || local >= string_count || prefix >= string_count) {
            status = SXML_STATUS_INVALID_ARGUMENT;
            break;
        }
        sxml_namespace_uri_identifier uri_identifier = {0};
        if (uri != 0) {
            status = sxml_name_pool_intern_namespace_uri(reader->name_pool, strings[uri], &uri_identifier);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_intern_name(
                reader->name_pool,
                &(sxml_name_request){.namespace_uri = uri_identifier, .local_name = strings[local]},
                &reader->names[index]);
        }
        if (status == SXML_STATUS_OK && prefix != 0) {
            status = sxml_name_pool_intern_namespace_prefix(reader->name_pool, strings[prefix],
                                                            &reader->prefixes[index]);
        }
    }
    if (status != SXML_STATUS_OK) {
        goto done;
    }
    /* Sets: count bindings first, then intern. */
    const uint8_t *sets_start = cursor.at;
    uint64_t binding_total = 0;
    for (uint64_t set = 0; set < set_count; set++) {
        uint64_t count = xdmb_read_varint(&cursor);
        for (uint64_t pair = 0; pair < count * 2 && !cursor.failed; pair++) {
            xdmb_read_varint(&cursor);
        }
        binding_total += count;
        if (cursor.failed || binding_total > request->bytes.byte_count) {
            status = SXML_STATUS_INVALID_ARGUMENT;
            goto done;
        }
    }
    reader->bindings = calloc(binding_total ? binding_total : 1, sizeof *reader->bindings);
    if (reader->bindings == NULL) {
        status = SXML_STATUS_OUT_OF_MEMORY;
        goto done;
    }
    cursor.at = sets_start;
    uint32_t binding_index = 0;
    for (uint64_t set = 0; set < set_count && status == SXML_STATUS_OK; set++) {
        reader->set_starts[set] = binding_index;
        uint64_t count = xdmb_read_varint(&cursor);
        for (uint64_t pair = 0; pair < count && status == SXML_STATUS_OK; pair++) {
            uint64_t prefix = xdmb_read_varint(&cursor);
            uint64_t uri = xdmb_read_varint(&cursor);
            if (prefix >= string_count || uri >= string_count) {
                status = SXML_STATUS_INVALID_ARGUMENT;
                break;
            }
            sxml_namespace_binding *binding = &reader->bindings[binding_index++];
            if (prefix != 0) {
                status = sxml_name_pool_intern_namespace_prefix(reader->name_pool, strings[prefix],
                                                                &binding->namespace_prefix);
            }
            if (status == SXML_STATUS_OK && uri != 0) {
                status = sxml_name_pool_intern_namespace_uri(reader->name_pool, strings[uri], &binding->namespace_uri);
            }
        }
    }
    reader->set_starts[set_count] = binding_index;
    if (status != SXML_STATUS_OK) {
        goto done;
    }
    size_t table_size = (size_t)node_count * reader->record_size;
    if (table_size > (size_t)(cursor.end - cursor.at) ||
        text_heap_size != (uint64_t)(cursor.end - cursor.at) - table_size) {
        status = SXML_STATUS_INVALID_ARGUMENT;
        goto done;
    }
    reader->node_table = cursor.at;
    reader->text_heap = cursor.at + table_size;
    reader->text_heap_size = (size_t)text_heap_size;
    /* Validate the structural fields once, so the operations may trust
       them: names and sets in range, parents before their children, the
       descendant ranges nested. O(nodes). */
    for (uint32_t index = 0; index < reader->node_count; index++) {
        uint8_t kind = xdmb_kind_flags(reader, index) & XDMB_KIND_MASK;
        uint32_t name = xdmb_name(reader, index);
        uint32_t parent = xdmb_parent(reader, index);
        uint32_t last = xdmb_last_descendant(reader, index);
        bool valid = kind <= SXML_NODE_KIND_NAMESPACE && name <= name_count && last >= index &&
                     last < reader->node_count && (index == 0 || (parent < index && xdmb_last_descendant(reader, parent) >= last));
        if (kind == SXML_NODE_KIND_ELEMENT) {
            valid = valid && xdmb_payload(reader, index) < set_count;
        }
        if (!valid) {
            status = SXML_STATUS_INVALID_ARGUMENT;
            goto done;
        }
    }
    status = sxml_tree_create(request->allocator,
                              &(sxml_tree_configuration){
                                  .state = reader,
                                  .operations = &xdmb_tree_operations,
                                  .item_heap = request->item_heap,
                                  .owner = {.state = reader, .operations = &xdmb_owner_operations},
                              },
                              tree);
done:
    free(strings);
    if (status != SXML_STATUS_OK) {
        xdmb_reader_free(reader);
        return status;
    }
    reader->tree = *tree;
    return SXML_STATUS_OK;
}
