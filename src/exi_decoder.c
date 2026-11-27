/*
 * The EXI decoder (include/sxml/exi.h; EXI 1.0, sections 5 to 8; D333): an
 * EXI stream to tree builder events. Golden: src/xml.c's parser, which
 * sends the same events.
 *
 * The body is read with an explicit stack of open elements (D71). An
 * element's start tag (its SE, NS and AT events) is held until its first
 * content event or its end, then sent: start_element, its namespace
 * bindings, its attributes, as builders expect.
 */
#include <stdalign.h>
#include <string.h>

#include "deflate_internal.h"
#include "exi_internal.h"
#include "memory_internal.h"
#include "sxml/tree.h"

/* A namespace binding in scope while decoding: prefix and URI as text in
   the decoder's own store. */
typedef struct sxml_exi_scoped_binding {
    sxml_exi_text prefix;
    sxml_exi_text uri;
    uint32_t depth;
} sxml_exi_scoped_binding;

/* A builder event of the body, sent at once or, with pre-compression and
   compression, recorded until its block's values are read (EXI 9). */
typedef enum sxml_exi_emitted_kind {
    SXML_EXI_EMITTED_START_ELEMENT,
    SXML_EXI_EMITTED_BINDING,
    SXML_EXI_EMITTED_ATTRIBUTE,
    SXML_EXI_EMITTED_END_ELEMENT,
    SXML_EXI_EMITTED_TEXT,
    SXML_EXI_EMITTED_COMMENT,
    SXML_EXI_EMITTED_PROCESSING_INSTRUCTION,
} sxml_exi_emitted_kind;

/* A value of the body: text in the decoder's store, or a slot of the
   block's value channels not read yet. */
typedef struct sxml_exi_value_handle {
    bool is_slot;
    uint32_t slot;
    sxml_exi_text text;
} sxml_exi_value_handle;

typedef struct sxml_exi_emitted {
    sxml_exi_emitted_kind emitted_kind;
    sxml_name_identifier name;
    sxml_namespace_prefix_identifier prefix;
    sxml_namespace_uri_identifier uri;
    sxml_exi_value_handle text;
    sxml_exi_text data;
} sxml_exi_emitted;

/* A value channel of the current block (9.2.2): its qname, its values in
   order and the slots they fill. */
typedef struct sxml_exi_channel {
    uint32_t uri;
    uint32_t local_name;
    uint32_t value_count;
    /* The slots of its values, in order: an index into channel_slots. */
    size_t first_slot_entry;
} sxml_exi_channel;

/* A value slot: its channel, its datatype (SXML_EXI_NONE for a String,
   7.1.10) and its text once read. */
typedef struct sxml_exi_slot {
    uint32_t channel;
    uint32_t datatype;
    sxml_exi_text text;
} sxml_exi_slot;

/* An attribute of the start tag being read. */
typedef struct sxml_exi_pending_attribute {
    sxml_name_identifier name;
    sxml_namespace_prefix_identifier prefix;
    sxml_exi_value_handle value;
} sxml_exi_pending_attribute;

/* A namespace declaration of the start tag being read. */
typedef struct sxml_exi_pending_binding {
    sxml_exi_text prefix;
    sxml_exi_text uri;
} sxml_exi_pending_binding;

typedef struct sxml_exi_frame {
    uint32_t uri;
    uint32_t local_name;
    uint32_t element_grammar;
    sxml_exi_nonterminal nonterminal;
    /* The schema-informed grammar the element is in (8.5); SXML_EXI_NONE
       for a built-in element grammar (8.4.3). */
    uint32_t schema_grammar;
    /* The bindings in scope when the element started. */
    size_t binding_mark;
} sxml_exi_frame;

/* The production an event code named, and whether it was learned. */
typedef struct sxml_exi_decoded_event {
    sxml_exi_production production;
    bool is_learned;
} sxml_exi_decoded_event;

typedef struct sxml_exi_decoding {
    sxml_exi_decoder *decoder;
    const sxml_exi_decode_request *request;
    sxml_tree_builder builder;
    sxml_allocator allocator;
    sxml_name_pool *name_pool;
    sxml_exi_limits limits;
    sxml_exi_options options;
    sxml_exi_bit_reader reader;
    sxml_exi_string_table table;
    sxml_exi_grammars grammars;
    /* Schema-informed (8.5): the schema's grammars (with the top level's
       grammar among them, top_schema_grammar, below). */
    sxml_exi_schema_grammars schema_grammars;
    /* A typed value as text, before it is stored. */
    sxml_exi_buffer typed_text;
    /* Decoded text the events refer to (values, prefixes, comments). */
    uint8_t *text_bytes;
    size_t text_byte_count;
    size_t text_capacity;
    uint64_t content_byte_count;
    sxml_exi_frame *frames;
    size_t frame_count;
    size_t frame_capacity;
    sxml_exi_scoped_binding *bindings;
    size_t binding_count;
    size_t binding_capacity;
    /* The start tag being read. */
    bool has_pending_start;
    sxml_name_identifier pending_name;
    uint32_t pending_uri;
    uint32_t pending_prefix_index;
    /* Schema-informed: the top level's grammar (DocContent, DocEnd or
       FragmentContent). */
    uint32_t top_schema_grammar;
    bool has_pending_prefix_index;
    sxml_exi_text pending_local_prefix;
    bool has_pending_local_prefix;
    sxml_exi_pending_attribute *attributes;
    size_t attribute_count;
    size_t attribute_capacity;
    sxml_exi_pending_binding *declarations;
    size_t declaration_count;
    size_t declaration_capacity;
    /* Synthesized prefixes without Preserve.prefixes: ns0, ns1, ... */
    uint32_t next_synthesized_prefix;
    /* The top level: document or fragment, and where it is. */
    sxml_exi_nonterminal top_nonterminal;
    sxml_byte_view failure_description;
    /* The header had options; the body reached ED; schema-informed
       grammars are in use (8.5). */
    bool has_header_options;
    bool is_finished;
    bool is_schema_informed;
    /* Pre-compression or compression (EXI 9): events recorded per block,
       values read per channel after the block's structure. */
    bool is_channelized;
    bool is_compressed;
    uint64_t block_size;
    uint64_t block_value_count;
    /* The block being read, from 0. */
    uint64_t block_number;
    sxml_exi_emitted *emitted;
    size_t emitted_count;
    size_t emitted_capacity;
    sxml_exi_slot *slots;
    size_t slot_count;
    size_t slot_capacity;
    sxml_exi_channel *channels;
    size_t channel_count;
    size_t channel_capacity;
    /* Per channel, its slots in order (filled when the block ends). */
    uint32_t *channel_slots;
    size_t channel_slot_capacity;
    /* With compression: the raw stream position of the next compressed
       stream, and the inflated stream being read. */
    sxml_byte_view raw_input;
    size_t raw_position;
    uint8_t *inflated;
    size_t inflated_byte_count;
} sxml_exi_decoding;

#define SXML_EXI_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* A failure: its status and what to tell. */
typedef struct sxml_exi_failure {
    sxml_status status;
    sxml_byte_view description;
} sxml_exi_failure;

static sxml_status sxml_exi_record_failure(sxml_exi_decoding *decoding, sxml_exi_failure failure) {
    if (decoding->failure_description.byte_count == 0) {
        decoding->failure_description = failure.description;
    }
    return failure.status;
}

#define SXML_EXI_FAIL(state, failure_status, literal)                                                      \
    sxml_exi_record_failure((state),                                                                   \
                            (sxml_exi_failure){.status = (failure_status), .description = SXML_EXI_TEXT(literal)})

/* ------------------------------------------------------------- Memory */

typedef struct sxml_exi_array {
    void **elements;
    size_t *capacity;
    size_t element_count;
    size_t element_size;
} sxml_exi_array;

static sxml_status sxml_exi_grow(sxml_allocator allocator, const sxml_exi_array *array) {
    if (array->element_count < *array->capacity) {
        return SXML_STATUS_OK;
    }
    return sxml_allocator_reserve_array(allocator, &(sxml_array_reservation_request){
                                                       .elements = array->elements,
                                                       .capacity = array->capacity,
                                                       .element_count = array->element_count,
                                                       .required_count = array->element_count + 1,
                                                       .element_size = array->element_size,
                                                       .element_alignment = alignof(max_align_t),
                                                       .minimum_capacity = 16});
}

static sxml_byte_view sxml_exi_decoded_view(const sxml_exi_decoding *decoding, sxml_exi_text text) {
    if (decoding->text_bytes == NULL) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return (sxml_byte_view){.bytes = decoding->text_bytes + text.offset, .byte_count = text.byte_count};
}

/* Appends UTF-8 to the decoder's text store. */
static sxml_status sxml_exi_append_text(sxml_exi_decoding *decoding, sxml_byte_view bytes) {
    decoding->content_byte_count += bytes.byte_count;
    if (decoding->content_byte_count > decoding->limits.maximum_content_byte_count) {
        return SXML_EXI_FAIL(decoding, SXML_STATUS_LIMIT_EXCEEDED, "the stream decodes to more content than the limit");
    }
    if (decoding->text_byte_count + bytes.byte_count > decoding->text_capacity) {
        void *elements = decoding->text_bytes;
        sxml_status status = sxml_allocator_reserve_array(
            decoding->allocator, &(sxml_array_reservation_request){.elements = &elements,
                                                                   .capacity = &decoding->text_capacity,
                                                                   .element_count = decoding->text_byte_count,
                                                                   .required_count = decoding->text_byte_count + bytes.byte_count,
                                                                   .element_size = 1,
                                                                   .element_alignment = 1,
                                                                   .minimum_capacity = 4096});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        decoding->text_bytes = elements;
    }
    if (bytes.byte_count > 0) {
        memcpy(decoding->text_bytes + decoding->text_byte_count, bytes.bytes, bytes.byte_count);
    }
    decoding->text_byte_count += bytes.byte_count;
    return SXML_STATUS_OK;
}

/* Appends codepoint as UTF-8; MALFORMED_INPUT for one XML does not allow
   (XML 1.0, production [2] Char). */
static sxml_status sxml_exi_append_codepoint(sxml_exi_decoding *decoding, uint64_t codepoint) {
    const bool is_char = codepoint == 0x9 || codepoint == 0xA || codepoint == 0xD ||
                         (codepoint >= 0x20 && codepoint <= 0xD7FF) || (codepoint >= 0xE000 && codepoint <= 0xFFFD) ||
                         (codepoint >= 0x10000 && codepoint <= 0x10FFFF);
    if (!is_char) {
        return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "a character XML does not allow");
    }
    uint8_t bytes[4];
    size_t byte_count = 0;
    const uint32_t value = (uint32_t)codepoint;
    if (value < 0x80) {
        bytes[byte_count++] = (uint8_t)value;
    } else if (value < 0x800) {
        bytes[byte_count++] = (uint8_t)(0xC0U | (value >> 6));
        bytes[byte_count++] = (uint8_t)(0x80U | (value & 0x3FU));
    } else if (value < 0x10000) {
        bytes[byte_count++] = (uint8_t)(0xE0U | (value >> 12));
        bytes[byte_count++] = (uint8_t)(0x80U | ((value >> 6) & 0x3FU));
        bytes[byte_count++] = (uint8_t)(0x80U | (value & 0x3FU));
    } else {
        bytes[byte_count++] = (uint8_t)(0xF0U | (value >> 18));
        bytes[byte_count++] = (uint8_t)(0x80U | ((value >> 12) & 0x3FU));
        bytes[byte_count++] = (uint8_t)(0x80U | ((value >> 6) & 0x3FU));
        bytes[byte_count++] = (uint8_t)(0x80U | (value & 0x3FU));
    }
    return sxml_exi_append_text(decoding, (sxml_byte_view){.bytes = bytes, .byte_count = byte_count});
}

/* Reads codepoint_count characters (EXI 7.1.10) into the text store: each
   an Unsigned Integer, or with a restricted character set (7.1.10.1) an
   n-bit index into it, the index N escaping an Unsigned Integer. */
typedef struct sxml_exi_characters_read {
    uint64_t codepoint_count;
    sxml_exi_character_set character_set;
} sxml_exi_characters_read;

static sxml_status sxml_exi_read_characters(sxml_exi_decoding *decoding, const sxml_exi_characters_read *read,
                                            sxml_exi_text *text) {
    const size_t start = decoding->text_byte_count;
    const uint64_t codepoint_count = read->codepoint_count;
    const sxml_exi_character_set set = read->character_set;
    const uint32_t width = set.character_count > 0 ? sxml_exi_bit_width(set.character_count + 1) : 0;
    /* Each character takes at least one bit of the stream: a count past
       what is left cannot be right. */
    if (codepoint_count / 8 > decoding->reader.input.byte_count - decoding->reader.byte_position) {
        return SXML_EXI_FAIL(decoding, SXML_STATUS_UNEXPECTED_END_OF_INPUT, "a string longer than the stream");
    }
    for (uint64_t character_index = 0; character_index < codepoint_count; character_index++) {
        uint64_t codepoint = 0;
        sxml_status status = SXML_STATUS_OK;
        if (set.character_count > 0) {
            uint32_t code = 0;
            status = sxml_exi_read_n_bit(&decoding->reader, (sxml_exi_bits_request){.bit_count = width}, &code);
            if (status == SXML_STATUS_OK && code < set.character_count) {
                codepoint = set.characters[code];
            } else if (status == SXML_STATUS_OK && code == set.character_count) {
                status = sxml_exi_read_unsigned(&decoding->reader, &codepoint);
            } else if (status == SXML_STATUS_OK) {
                status = SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "a restricted character past its set");
            }
        } else {
            status = sxml_exi_read_unsigned(&decoding->reader, &codepoint);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_append_codepoint(decoding, codepoint);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    *text = (sxml_exi_text){.offset = start,
                            .byte_count = (uint32_t)(decoding->text_byte_count - start),
                            .codepoint_count = (uint32_t)codepoint_count};
    return SXML_STATUS_OK;
}

/* A String (EXI 7.1.10): its length, then its characters. */
static sxml_status sxml_exi_read_string(sxml_exi_decoding *decoding, sxml_exi_text *text) {
    uint64_t length = 0;
    sxml_status status = sxml_exi_read_unsigned(&decoding->reader, &length);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_exi_read_characters(decoding, &(sxml_exi_characters_read){.codepoint_count = length}, text);
}

/* ---------------------------------------------------------- String table */

static sxml_status sxml_exi_read_uri(sxml_exi_decoding *decoding, uint32_t *uri) {
    uint32_t value = 0;
    sxml_status status = sxml_exi_read_n_bit(
        &decoding->reader, (sxml_exi_bits_request){.bit_count = sxml_exi_bit_width(decoding->table.uri_count + 1)},
        &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (value != 0) {
        if (value - 1 >= decoding->table.uri_count) {
            return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "a URI compact identifier past the partition");
        }
        *uri = value - 1;
        return SXML_STATUS_OK;
    }
    sxml_exi_text text = {0};
    status = sxml_exi_read_string(decoding, &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_exi_add_uri(&decoding->table, &(sxml_exi_string_key){.text = sxml_exi_decoded_view(decoding, text)}, uri);
}

static sxml_status sxml_exi_read_local_name(sxml_exi_decoding *decoding, uint32_t uri, uint32_t *local_name) {
    uint64_t length = 0;
    sxml_status status = sxml_exi_read_unsigned(&decoding->reader, &length);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_exi_uri *partition = &decoding->table.uris[uri];
    if (length == 0) {
        uint32_t identifier = 0;
        status = sxml_exi_read_n_bit(
            &decoding->reader, (sxml_exi_bits_request){.bit_count = sxml_exi_bit_width(partition->local_name_count)},
            &identifier);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (identifier >= partition->local_name_count) {
            return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "a local-name compact identifier past the partition");
        }
        *local_name = identifier;
        return SXML_STATUS_OK;
    }
    sxml_exi_text text = {0};
    status = sxml_exi_read_characters(decoding, &(sxml_exi_characters_read){.codepoint_count = length - 1}, &text);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (text.byte_count == 0) {
        return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "an empty local-name");
    }
    return sxml_exi_add_local_name(
        &decoding->table, &(sxml_exi_string_key){.text = sxml_exi_decoded_view(decoding, text), .uri = uri}, local_name);
}

/* A value (EXI 7.3.3) of the qname (uri, local_name). */
typedef struct sxml_exi_qname {
    uint32_t uri;
    uint32_t local_name;
} sxml_exi_qname;

/* Reads a value (7.3.3) of the qname (uri, local_name) into the decoder's
   store; a literal's characters may use a restricted character set
   (7.1.10.1, Table 7-2). */
typedef struct sxml_exi_value_read {
    sxml_exi_qname qname;
    sxml_exi_character_set character_set;
} sxml_exi_value_read;

static sxml_status sxml_exi_read_value_literal(sxml_exi_decoding *decoding, const sxml_exi_value_read *read,
                                               sxml_exi_text *value) {
    const sxml_exi_qname qname = read->qname;
    uint64_t length = 0;
    sxml_status status = sxml_exi_read_unsigned(&decoding->reader, &length);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (length == 0 || length == 1) {
        uint32_t entry = SXML_EXI_NONE;
        uint32_t identifier = 0;
        if (length == 0) {
            const sxml_exi_local_name *owner = &decoding->table.uris[qname.uri].local_names[qname.local_name];
            status = sxml_exi_read_n_bit(
                &decoding->reader, (sxml_exi_bits_request){.bit_count = sxml_exi_bit_width(owner->local_value_count)},
                &identifier);
            entry = sxml_exi_local_value_entry(&decoding->table, (sxml_exi_value_reference){
                                                                     .uri = qname.uri,
                                                                     .local_name = qname.local_name,
                                                                     .compact_identifier = identifier});
        } else {
            status = sxml_exi_read_n_bit(
                &decoding->reader,
                (sxml_exi_bits_request){.bit_count = sxml_exi_bit_width(decoding->table.global_value_count)}, &identifier);
            entry = sxml_exi_global_value_entry(&decoding->table, identifier);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (entry == SXML_EXI_NONE) {
            return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "a value compact identifier with no value");
        }
        const sxml_byte_view hit = sxml_exi_string_table_view(&decoding->table, decoding->table.values[entry].text);
        const size_t start = decoding->text_byte_count;
        status = sxml_exi_append_text(decoding, hit);
        *value = (sxml_exi_text){.offset = start, .byte_count = (uint32_t)hit.byte_count};
        return status;
    }
    status = sxml_exi_read_characters(
        decoding, &(sxml_exi_characters_read){.codepoint_count = length - 2, .character_set = read->character_set}, value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_exi_add_value(&decoding->table,
                              &(sxml_exi_string_key){.text = sxml_exi_decoded_view(decoding, *value),
                                                     .uri = qname.uri,
                                                     .local_name = qname.local_name});
}

/* A value to read: its qname (whose value partitions a String uses) and
   its datatype, SXML_EXI_NONE for a String (7.1.10). */
typedef struct sxml_exi_typed_value {
    sxml_exi_qname qname;
    uint32_t datatype;
} sxml_exi_typed_value;

/* The restricted character set a String of datatype uses: the
   datatype's own (7.1.10.1), or Table 7-2's with Preserve.lexicalValues. */
static sxml_exi_character_set sxml_exi_value_character_set(const sxml_exi_decoding *decoding, uint32_t datatype) {
    const sxml_exi_schema_grammars *grammars = &decoding->schema_grammars;
    if (datatype == SXML_EXI_NONE) {
        return (sxml_exi_character_set){.characters = NULL, .character_count = 0};
    }
    if (decoding->options.fidelity.preserves_lexical_values) {
        return sxml_exi_lexical_character_set(sxml_exi_datatype_lexical_set(grammars, datatype));
    }
    const sxml_exi_datatype *entry = &grammars->datatypes[datatype];
    if (entry->datatype_kind == SXML_EXI_DATATYPE_RESTRICTED_STRING) {
        return (sxml_exi_character_set){.characters = grammars->characters + entry->first_value,
                                        .character_count = entry->value_count};
    }
    return (sxml_exi_character_set){.characters = NULL, .character_count = 0};
}

/* Whether a value of datatype is a String in the stream (the string
   table's, 7.3.3): untyped, a string, or any with Preserve.lexicalValues. */
static bool sxml_exi_is_string_value(const sxml_exi_decoding *decoding, uint32_t datatype) {
    if (datatype == SXML_EXI_NONE || decoding->options.fidelity.preserves_lexical_values) {
        return true;
    }
    const sxml_exi_datatype_kind kind = decoding->schema_grammars.datatypes[datatype].datatype_kind;
    return kind == SXML_EXI_DATATYPE_STRING || kind == SXML_EXI_DATATYPE_RESTRICTED_STRING;
}

/* A value that is not a list: a String, or a typed value appended to the
   store as the oracle writes it. */
static sxml_status sxml_exi_read_scalar_text(sxml_exi_decoding *decoding, sxml_exi_typed_value value,
                                             sxml_exi_text *text) {
    if (sxml_exi_is_string_value(decoding, value.datatype)) {
        return sxml_exi_read_value_literal(
            decoding,
            &(sxml_exi_value_read){.qname = value.qname, .character_set = sxml_exi_value_character_set(decoding, value.datatype)},
            text);
    }
    decoding->typed_text.byte_count = 0;
    sxml_status status = sxml_exi_read_typed_value(&(sxml_exi_typed_read){.grammars = &decoding->schema_grammars,
                                                                          .reader = &decoding->reader,
                                                                          .output = &decoding->typed_text},
                                                   value.datatype);
    if (status == SXML_STATUS_MALFORMED_INPUT) {
        return SXML_EXI_FAIL(decoding, status, "a typed value its datatype does not allow");
    }
    const size_t start = decoding->text_byte_count;
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_append_text(
            decoding, (sxml_byte_view){.bytes = decoding->typed_text.bytes, .byte_count = decoding->typed_text.byte_count});
    }
    *text = (sxml_exi_text){.offset = start, .byte_count = (uint32_t)(decoding->text_byte_count - start)};
    return status;
}

/* A value of an AT or CH event (7): a String, a typed value, or a List
   (7.1.11: its length, then its items, written separated by spaces). */
static sxml_status sxml_exi_read_value_text(sxml_exi_decoding *decoding, sxml_exi_typed_value value,
                                            sxml_exi_text *text) {
    const bool is_list = value.datatype != SXML_EXI_NONE && !decoding->options.fidelity.preserves_lexical_values &&
                         decoding->schema_grammars.datatypes[value.datatype].datatype_kind == SXML_EXI_DATATYPE_LIST;
    if (!is_list) {
        return sxml_exi_read_scalar_text(decoding, value, text);
    }
    uint64_t item_count = 0;
    sxml_status status = sxml_exi_read_unsigned(&decoding->reader, &item_count);
    if (status == SXML_STATUS_OK && item_count > decoding->reader.input.byte_count * 8) {
        return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "a list longer than the stream");
    }
    const uint32_t item_datatype = decoding->schema_grammars.datatypes[value.datatype].base_datatype;
    const size_t start = decoding->text_byte_count;
    for (uint64_t item_index = 0; item_index < item_count && status == SXML_STATUS_OK; item_index++) {
        if (item_index > 0) {
            status = sxml_exi_append_text(decoding, SXML_EXI_TEXT(" "));
        }
        sxml_exi_text item = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_read_scalar_text(decoding, (sxml_exi_typed_value){.qname = value.qname, .datatype = item_datatype},
                                               &item);
        }
    }
    *text = (sxml_exi_text){.offset = start, .byte_count = (uint32_t)(decoding->text_byte_count - start)};
    return status;
}

/* A value of an AT or CH event: read now, or, with pre-compression and
   compression, a slot of its qname's channel, read when the block ends
   (9.2.2). */
static sxml_status sxml_exi_take_value(sxml_exi_decoding *decoding, sxml_exi_typed_value typed, sxml_exi_value_handle *value) {
    const sxml_exi_qname qname = typed.qname;
    if (!decoding->is_channelized) {
        *value = (sxml_exi_value_handle){.is_slot = false};
        return sxml_exi_read_value_text(decoding, typed, &value->text);
    }
    sxml_exi_local_name *owner = &decoding->table.uris[qname.uri].local_names[qname.local_name];
    uint32_t channel = owner->channel_index;
    if (owner->channel_block != decoding->block_number + 1 || channel >= decoding->channel_count ||
        decoding->channels[channel].uri != qname.uri || decoding->channels[channel].local_name != qname.local_name) {
        void *elements = decoding->channels;
        sxml_status status = sxml_exi_grow(decoding->allocator, &(sxml_exi_array){.elements = &elements,
                                                                                   .capacity = &decoding->channel_capacity,
                                                                                   .element_count = decoding->channel_count,
                                                                                   .element_size = sizeof(sxml_exi_channel)});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        decoding->channels = elements;
        channel = (uint32_t)decoding->channel_count;
        decoding->channels[decoding->channel_count++] =
            (sxml_exi_channel){.uri = qname.uri, .local_name = qname.local_name, .value_count = 0};
        owner = &decoding->table.uris[qname.uri].local_names[qname.local_name];
        owner->channel_block = decoding->block_number + 1;
        owner->channel_index = channel;
    }
    void *elements = decoding->slots;
    sxml_status status = sxml_exi_grow(decoding->allocator, &(sxml_exi_array){.elements = &elements,
                                                                               .capacity = &decoding->slot_capacity,
                                                                               .element_count = decoding->slot_count,
                                                                               .element_size = sizeof(sxml_exi_slot)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    decoding->slots = elements;
    decoding->channels[channel].value_count++;
    *value = (sxml_exi_value_handle){.is_slot = true, .slot = (uint32_t)decoding->slot_count};
    decoding->slots[decoding->slot_count++] = (sxml_exi_slot){.channel = channel, .datatype = typed.datatype};
    decoding->block_value_count++;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Names */

static sxml_status sxml_exi_namespace_identifier(sxml_exi_decoding *decoding, uint32_t uri,
                                                 sxml_namespace_uri_identifier *identifier) {
    sxml_exi_uri *partition = &decoding->table.uris[uri];
    if (!partition->has_namespace_uri) {
        sxml_status status = sxml_name_pool_intern_namespace_uri(
            decoding->name_pool, sxml_exi_string_table_view(&decoding->table, partition->text), &partition->namespace_uri);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        partition->has_namespace_uri = true;
    }
    *identifier = partition->namespace_uri;
    return SXML_STATUS_OK;
}

static sxml_status sxml_exi_name_identifier(sxml_exi_decoding *decoding, sxml_exi_qname qname,
                                            sxml_name_identifier *name) {
    sxml_exi_local_name *entry = &decoding->table.uris[qname.uri].local_names[qname.local_name];
    if (entry->name.index == 0) {
        sxml_namespace_uri_identifier uri = {0};
        sxml_status status = sxml_exi_namespace_identifier(decoding, qname.uri, &uri);
        entry = &decoding->table.uris[qname.uri].local_names[qname.local_name];
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_intern_name(
                decoding->name_pool,
                &(sxml_name_request){.namespace_uri = uri,
                                     .local_name = sxml_exi_string_table_view(&decoding->table, entry->text)},
                &entry->name);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    *name = entry->name;
    return SXML_STATUS_OK;
}

/* The prefix of a qname with Preserve.prefixes (7.1.7): an n-bit index
   into its URI's prefix partition. */
static sxml_status sxml_exi_read_qname_prefix(sxml_exi_decoding *decoding, uint32_t uri, uint32_t *prefix) {
    const size_t prefix_count = decoding->table.uris[uri].prefix_count;
    return sxml_exi_read_n_bit(
        &decoding->reader,
        (sxml_exi_bits_request){.bit_count = sxml_exi_bit_width(prefix_count == 0 ? 1 : prefix_count)}, prefix);
}

/* The binding in scope for uri, the most recent first; NONE without. */
static size_t sxml_exi_find_binding_for_uri(const sxml_exi_decoding *decoding, sxml_byte_view uri) {
    for (size_t binding_index = decoding->binding_count; binding_index > 0; binding_index--) {
        const sxml_exi_scoped_binding *binding = &decoding->bindings[binding_index - 1];
        const sxml_byte_view bound = sxml_exi_decoded_view(decoding, binding->uri);
        if (bound.byte_count == uri.byte_count && (uri.byte_count == 0 || memcmp(bound.bytes, uri.bytes, uri.byte_count) == 0)) {
            /* The prefix must not be bound again to something else later. */
            const sxml_byte_view prefix = sxml_exi_decoded_view(decoding, binding->prefix);
            bool is_shadowed = false;
            for (size_t later = binding_index; later < decoding->binding_count; later++) {
                const sxml_byte_view other = sxml_exi_decoded_view(decoding, decoding->bindings[later].prefix);
                if (other.byte_count == prefix.byte_count &&
                    (prefix.byte_count == 0 || memcmp(other.bytes, prefix.bytes, prefix.byte_count) == 0)) {
                    is_shadowed = true;
                }
            }
            if (!is_shadowed) {
                return binding_index - 1;
            }
        }
    }
    return SIZE_MAX;
}

static sxml_status sxml_exi_push_binding(sxml_exi_decoding *decoding, const sxml_exi_pending_binding *binding) {
    void *elements = decoding->bindings;
    sxml_status status = sxml_exi_grow(decoding->allocator, &(sxml_exi_array){.elements = &elements,
                                                                               .capacity = &decoding->binding_capacity,
                                                                               .element_count = decoding->binding_count,
                                                                               .element_size = sizeof(sxml_exi_scoped_binding)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    decoding->bindings = elements;
    decoding->bindings[decoding->binding_count++] =
        (sxml_exi_scoped_binding){.prefix = binding->prefix, .uri = binding->uri, .depth = (uint32_t)decoding->frame_count};
    return SXML_STATUS_OK;
}

static sxml_status sxml_exi_add_declaration(sxml_exi_decoding *decoding, const sxml_exi_pending_binding *binding) {
    void *elements = decoding->declarations;
    sxml_status status = sxml_exi_grow(decoding->allocator, &(sxml_exi_array){.elements = &elements,
                                                                               .capacity = &decoding->declaration_capacity,
                                                                               .element_count = decoding->declaration_count,
                                                                               .element_size = sizeof(sxml_exi_pending_binding)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    decoding->declarations = elements;
    decoding->declarations[decoding->declaration_count++] = *binding;
    return sxml_exi_push_binding(decoding, binding);
}

/* Without Preserve.prefixes: a prefix for uri, declared on the element
   being started when no binding in scope has it (prefixes ns0, ns1, ...,
   as EXI 6.1 leaves to the decoder). */
static sxml_status sxml_exi_synthesize_prefix(sxml_exi_decoding *decoding, uint32_t uri, sxml_exi_text *prefix) {
    const sxml_byte_view uri_text = sxml_exi_string_table_view(&decoding->table, decoding->table.uris[uri].text);
    if (uri_text.byte_count == 0) {
        *prefix = (sxml_exi_text){0};
        return SXML_STATUS_OK;
    }
    if (uri == 1) {
        /* The XML namespace is always bound to "xml". */
        const size_t start = decoding->text_byte_count;
        sxml_status status = sxml_exi_append_text(decoding, SXML_EXI_TEXT("xml"));
        *prefix = (sxml_exi_text){.offset = start, .byte_count = 3, .codepoint_count = 3};
        return status;
    }
    const size_t found = sxml_exi_find_binding_for_uri(decoding, uri_text);
    if (found != SIZE_MAX && decoding->bindings[found].prefix.byte_count > 0) {
        *prefix = decoding->bindings[found].prefix;
        return SXML_STATUS_OK;
    }
    char name[16];
    size_t length = 0;
    name[length++] = 'n';
    name[length++] = 's';
    char digits[12];
    size_t digit_count = 0;
    uint32_t number = decoding->next_synthesized_prefix++;
    do {
        digits[digit_count++] = (char)('0' + number % 10);
        number /= 10;
    } while (number > 0);
    while (digit_count > 0) {
        name[length++] = digits[--digit_count];
    }
    const size_t prefix_start = decoding->text_byte_count;
    sxml_status status =
        sxml_exi_append_text(decoding, (sxml_byte_view){.bytes = (const uint8_t *)name, .byte_count = length});
    const size_t uri_start = decoding->text_byte_count;
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_append_text(decoding, uri_text);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *prefix = (sxml_exi_text){.offset = prefix_start, .byte_count = (uint32_t)length, .codepoint_count = (uint32_t)length};
    return sxml_exi_add_declaration(
        decoding, &(sxml_exi_pending_binding){
                      .prefix = *prefix,
                      .uri = {.offset = uri_start, .byte_count = (uint32_t)uri_text.byte_count, .codepoint_count = 0}});
}

/* The prefix text of a prefix partition entry, copied into the decoder's
   store. */
static sxml_status sxml_exi_prefix_text(sxml_exi_decoding *decoding, sxml_exi_qname prefix_reference,
                                        sxml_exi_text *prefix) {
    const sxml_exi_uri *partition = &decoding->table.uris[prefix_reference.uri];
    if (prefix_reference.local_name >= partition->prefix_count) {
        return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "a prefix the stream does not declare");
    }
    const sxml_byte_view text = sxml_exi_string_table_view(&decoding->table, partition->prefixes[prefix_reference.local_name]);
    const size_t start = decoding->text_byte_count;
    sxml_status status = sxml_exi_append_text(decoding, text);
    *prefix = (sxml_exi_text){.offset = start, .byte_count = (uint32_t)text.byte_count, .codepoint_count = 0};
    return status;
}

static sxml_status sxml_exi_prefix_identifier(sxml_exi_decoding *decoding, sxml_exi_text prefix,
                                              sxml_namespace_prefix_identifier *identifier) {
    return sxml_name_pool_intern_namespace_prefix(decoding->name_pool, sxml_exi_decoded_view(decoding, prefix),
                                                  identifier);
}

/* --------------------------------------------------------------- Emitting */

static sxml_byte_view sxml_exi_handle_view(const sxml_exi_decoding *decoding, sxml_exi_value_handle handle) {
    return sxml_exi_decoded_view(decoding, handle.is_slot ? decoding->slots[handle.slot].text : handle.text);
}

/* Sends an emitted event to the builder. */
static sxml_status sxml_exi_deliver(sxml_exi_decoding *decoding, const sxml_exi_emitted *event) {
    const sxml_tree_builder builder = decoding->builder;
    switch (event->emitted_kind) {
    case SXML_EXI_EMITTED_START_ELEMENT:
        return builder.operations->start_element(
            builder.state, &(sxml_element_start_event){.element_name = event->name, .namespace_prefix = event->prefix});
    case SXML_EXI_EMITTED_BINDING:
        return builder.operations->add_namespace_binding(
            builder.state, &(sxml_namespace_binding){.namespace_prefix = event->prefix, .namespace_uri = event->uri});
    case SXML_EXI_EMITTED_ATTRIBUTE:
        return builder.operations->add_attribute(
            builder.state, &(sxml_attribute_event){.attribute_name = event->name,
                                                   .namespace_prefix = event->prefix,
                                                   .attribute_value = sxml_exi_handle_view(decoding, event->text)});
    case SXML_EXI_EMITTED_END_ELEMENT:
        return builder.operations->end_element(builder.state);
    case SXML_EXI_EMITTED_TEXT: {
        const sxml_byte_view text = sxml_exi_handle_view(decoding, event->text);
        if (text.byte_count == 0) {
            return SXML_STATUS_OK;
        }
        return builder.operations->add_text(builder.state, &(sxml_text_event){.text = text});
    }
    case SXML_EXI_EMITTED_COMMENT:
        return builder.operations->add_comment(
            builder.state, &(sxml_comment_event){.comment = sxml_exi_handle_view(decoding, event->text)});
    case SXML_EXI_EMITTED_PROCESSING_INSTRUCTION:
        return builder.operations->add_processing_instruction(
            builder.state, &(sxml_processing_instruction_event){.target = sxml_exi_handle_view(decoding, event->text),
                                                               .data = sxml_exi_decoded_view(decoding, event->data)});
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* Sends an event now, or records it until its block's values are read. */
static sxml_status sxml_exi_emit(sxml_exi_decoding *decoding, const sxml_exi_emitted *event) {
    if (!decoding->is_channelized) {
        return sxml_exi_deliver(decoding, event);
    }
    void *elements = decoding->emitted;
    sxml_status status = sxml_exi_grow(decoding->allocator, &(sxml_exi_array){.elements = &elements,
                                                                               .capacity = &decoding->emitted_capacity,
                                                                               .element_count = decoding->emitted_count,
                                                                               .element_size = sizeof(sxml_exi_emitted)});
    if (status == SXML_STATUS_OK) {
        decoding->emitted = elements;
        decoding->emitted[decoding->emitted_count++] = *event;
    }
    return status;
}

/* ----------------------------------------------------------- Start tags */

/* Sends the held start tag: the element, its bindings, its attributes. */
static sxml_status sxml_exi_flush_start(sxml_exi_decoding *decoding) {
    if (!decoding->has_pending_start) {
        return SXML_STATUS_OK;
    }
    decoding->has_pending_start = false;
    sxml_exi_text element_prefix = {0};
    sxml_status status = SXML_STATUS_OK;
    if (decoding->options.fidelity.preserves_prefixes) {
        if (decoding->has_pending_local_prefix) {
            element_prefix = decoding->pending_local_prefix;
        } else if (decoding->has_pending_prefix_index) {
            status = sxml_exi_prefix_text(decoding,
                                          (sxml_exi_qname){.uri = decoding->pending_uri,
                                                           .local_name = decoding->pending_prefix_index},
                                          &element_prefix);
        }
    } else {
        status = sxml_exi_synthesize_prefix(decoding, decoding->pending_uri, &element_prefix);
    }
    sxml_namespace_prefix_identifier prefix = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_prefix_identifier(decoding, element_prefix, &prefix);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_emit(decoding, &(sxml_exi_emitted){.emitted_kind = SXML_EXI_EMITTED_START_ELEMENT,
                                                             .name = decoding->pending_name,
                                                             .prefix = prefix});
    }
    for (size_t declaration_index = 0; declaration_index < decoding->declaration_count && status == SXML_STATUS_OK;
         declaration_index++) {
        const sxml_exi_pending_binding *declaration = &decoding->declarations[declaration_index];
        sxml_namespace_prefix_identifier bound_prefix = {0};
        sxml_namespace_uri_identifier bound_uri = {0};
        status = sxml_exi_prefix_identifier(decoding, declaration->prefix, &bound_prefix);
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_intern_namespace_uri(decoding->name_pool,
                                                         sxml_exi_decoded_view(decoding, declaration->uri), &bound_uri);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_emit(decoding, &(sxml_exi_emitted){.emitted_kind = SXML_EXI_EMITTED_BINDING,
                                                                 .prefix = bound_prefix,
                                                                 .uri = bound_uri});
        }
    }
    for (size_t attribute_index = 0; attribute_index < decoding->attribute_count && status == SXML_STATUS_OK;
         attribute_index++) {
        const sxml_exi_pending_attribute *attribute = &decoding->attributes[attribute_index];
        status = sxml_exi_emit(decoding, &(sxml_exi_emitted){.emitted_kind = SXML_EXI_EMITTED_ATTRIBUTE,
                                                             .name = attribute->name,
                                                             .prefix = attribute->prefix,
                                                             .text = attribute->value});
    }
    decoding->attribute_count = 0;
    decoding->declaration_count = 0;
    return status;
}

/* --------------------------------------------------------------- Events */

static sxml_exi_frame *sxml_exi_top_frame(sxml_exi_decoding *decoding) {
    return decoding->frame_count == 0 ? NULL : &decoding->frames[decoding->frame_count - 1];
}

/* The position of the innermost open element, or of the top level. */
static sxml_exi_position sxml_exi_current_position(sxml_exi_decoding *decoding) {
    const sxml_exi_frame *frame = sxml_exi_top_frame(decoding);
    if (frame == NULL) {
        return (sxml_exi_position){.nonterminal = decoding->top_nonterminal, .element_grammar = SXML_EXI_NONE};
    }
    return (sxml_exi_position){.nonterminal = frame->nonterminal, .element_grammar = frame->element_grammar};
}

static void sxml_exi_move_to(sxml_exi_decoding *decoding, sxml_exi_nonterminal nonterminal) {
    sxml_exi_frame *frame = sxml_exi_top_frame(decoding);
    if (frame != NULL) {
        frame->nonterminal = nonterminal;
    } else {
        decoding->top_nonterminal = nonterminal;
    }
}

/* Whether a learned production of kind (and, for EE and CH, any of them)
   is in position's learned productions. */
static bool sxml_exi_has_learned(sxml_exi_decoding *decoding, sxml_exi_position position, sxml_exi_event_kind kind) {
    const sxml_exi_learned *learned = sxml_exi_grammars_learned(&decoding->grammars, position);
    if (learned == NULL) {
        return true;
    }
    for (size_t production_index = 0; production_index < learned->production_count; production_index++) {
        if (learned->productions[production_index].event_kind == kind) {
            return true;
        }
    }
    return false;
}

/* An SE event: the element's qname, then its grammar and the new frame. */
static sxml_status sxml_exi_start_element(sxml_exi_decoding *decoding, const sxml_exi_decoded_event *event) {
    const sxml_exi_production *production = &event->production;
    const bool is_learned = event->is_learned;
    const sxml_exi_position position = sxml_exi_current_position(decoding);
    sxml_exi_qname qname = {.uri = production->uri, .local_name = production->local_name};
    sxml_status status = SXML_STATUS_OK;
    if (!is_learned) {
        status = sxml_exi_read_uri(decoding, &qname.uri);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_read_local_name(decoding, qname.uri, &qname.local_name);
        }
    }
    uint32_t prefix_index = 0;
    if (status == SXML_STATUS_OK && decoding->options.fidelity.preserves_prefixes) {
        status = sxml_exi_read_qname_prefix(decoding, qname.uri, &prefix_index);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* The parent's start tag ends here: send it, and move the parent on. */
    status = sxml_exi_flush_start(decoding);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!is_learned && position.nonterminal != SXML_EXI_NONTERMINAL_DOCUMENT_CONTENT) {
        /* 8.4.2, 8.4.3: SE (*) teaches the grammar SE (qname). */
        status = sxml_exi_grammars_learn(
            &decoding->grammars,
            &(sxml_exi_learning){.position = position,
                                 .production = {.event_kind = SXML_EXI_EVENT_START_ELEMENT,
                                                .uri = qname.uri,
                                                .local_name = qname.local_name}});
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    switch (position.nonterminal) {
    case SXML_EXI_NONTERMINAL_DOCUMENT_CONTENT:
        sxml_exi_move_to(decoding, SXML_EXI_NONTERMINAL_DOCUMENT_END);
        break;
    case SXML_EXI_NONTERMINAL_START_TAG_CONTENT:
        sxml_exi_move_to(decoding, SXML_EXI_NONTERMINAL_ELEMENT_CONTENT);
        break;
    case SXML_EXI_NONTERMINAL_FRAGMENT_CONTENT:
    case SXML_EXI_NONTERMINAL_ELEMENT_CONTENT:
        break;
    case SXML_EXI_NONTERMINAL_DOCUMENT_END:
    case SXML_EXI_NONTERMINAL_COUNT:
        return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "an element where none may start");
    }
    if (decoding->frame_count >= decoding->limits.maximum_depth) {
        return SXML_EXI_FAIL(decoding, SXML_STATUS_LIMIT_EXCEEDED, "elements nested deeper than the limit");
    }
    sxml_exi_local_name *entry = &decoding->table.uris[qname.uri].local_names[qname.local_name];
    if (entry->element_grammar == SXML_EXI_NONE) {
        uint32_t element_grammar = 0;
        status = sxml_exi_grammars_add_element(&decoding->grammars, &element_grammar);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        entry = &decoding->table.uris[qname.uri].local_names[qname.local_name];
        entry->element_grammar = element_grammar;
    }
    void *elements = decoding->frames;
    status = sxml_exi_grow(decoding->allocator, &(sxml_exi_array){.elements = &elements,
                                                                   .capacity = &decoding->frame_capacity,
                                                                   .element_count = decoding->frame_count,
                                                                   .element_size = sizeof(sxml_exi_frame)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    decoding->frames = elements;
    decoding->frames[decoding->frame_count++] = (sxml_exi_frame){
        .uri = qname.uri,
        .local_name = qname.local_name,
        .element_grammar = entry->element_grammar,
        .nonterminal = SXML_EXI_NONTERMINAL_START_TAG_CONTENT,
        /* 8.5.2, 8.5.4.4.1: an element of a global element declaration
           has its grammar, wherever SE (*) matched it. */
        .schema_grammar = decoding->is_schema_informed ? entry->schema_element_grammar : SXML_EXI_NONE,
        .binding_mark = decoding->binding_count,
    };
    status = sxml_exi_name_identifier(decoding, qname, &decoding->pending_name);
    decoding->has_pending_start = true;
    decoding->pending_uri = qname.uri;
    decoding->pending_prefix_index = prefix_index;
    decoding->has_pending_prefix_index = decoding->options.fidelity.preserves_prefixes;
    decoding->has_pending_local_prefix = false;
    return status;
}

static sxml_status sxml_exi_end_element(sxml_exi_decoding *decoding, const sxml_exi_decoded_event *event) {
    const bool is_learned = event->is_learned;
    const sxml_exi_position position = sxml_exi_current_position(decoding);
    sxml_status status = SXML_STATUS_OK;
    if (!is_learned && position.nonterminal == SXML_EXI_NONTERMINAL_START_TAG_CONTENT &&
        !sxml_exi_has_learned(decoding, position, SXML_EXI_EVENT_END_ELEMENT)) {
        status = sxml_exi_grammars_learn(
            &decoding->grammars,
            &(sxml_exi_learning){.position = position, .production = {.event_kind = SXML_EXI_EVENT_END_ELEMENT}});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_flush_start(decoding);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_emit(decoding, &(sxml_exi_emitted){.emitted_kind = SXML_EXI_EMITTED_END_ELEMENT});
    }
    if (status == SXML_STATUS_OK) {
        decoding->binding_count = decoding->frames[decoding->frame_count - 1].binding_mark;
        decoding->frame_count--;
    }
    return status;
}

/* xsi:type's value without Preserve.lexicalValues: a QName (7.1.7), as
   prefix:local text with a prefix bound to its URI, and the qname. */
typedef struct sxml_exi_type_name {
    sxml_byte_view value;
    sxml_exi_qname qname;
} sxml_exi_type_name;

static sxml_status sxml_exi_read_type_name(sxml_exi_decoding *decoding, sxml_exi_type_name *type_name) {
    sxml_exi_qname type = {0};
    sxml_status status = sxml_exi_read_uri(decoding, &type.uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_read_local_name(decoding, type.uri, &type.local_name);
    }
    sxml_exi_text prefix = {0};
    if (status == SXML_STATUS_OK && decoding->options.fidelity.preserves_prefixes) {
        uint32_t prefix_index = 0;
        status = sxml_exi_read_qname_prefix(decoding, type.uri, &prefix_index);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_prefix_text(decoding, (sxml_exi_qname){.uri = type.uri, .local_name = prefix_index}, &prefix);
        }
    } else if (status == SXML_STATUS_OK) {
        status = sxml_exi_synthesize_prefix(decoding, type.uri, &prefix);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_byte_view local = sxml_exi_string_table_view(
        &decoding->table, decoding->table.uris[type.uri].local_names[type.local_name].text);
    const sxml_byte_view prefix_text = sxml_exi_decoded_view(decoding, prefix);
    /* Copy the prefix first: appending may move the store. */
    uint8_t prefix_copy[256];
    if (prefix_text.byte_count > sizeof prefix_copy) {
        return SXML_EXI_FAIL(decoding, SXML_STATUS_LIMIT_EXCEEDED, "an xsi:type prefix longer than 256 bytes");
    }
    if (prefix_text.byte_count > 0) {
        memcpy(prefix_copy, prefix_text.bytes, prefix_text.byte_count);
    }
    const size_t start = decoding->text_byte_count;
    if (prefix_text.byte_count > 0) {
        status = sxml_exi_append_text(decoding, (sxml_byte_view){.bytes = prefix_copy, .byte_count = prefix_text.byte_count});
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_append_text(decoding, SXML_EXI_TEXT(":"));
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_append_text(decoding, local);
    }
    if (status == SXML_STATUS_OK) {
        type_name->value =
            (sxml_byte_view){.bytes = decoding->text_bytes + start, .byte_count = decoding->text_byte_count - start};
        type_name->qname = type;
    }
    return status;
}

/* 8.5.4.4: xsi:type's type, when a grammar is found for it, is the
   element's grammar from here on (a global type grammar). */
static void sxml_exi_switch_to_type(sxml_exi_decoding *decoding, sxml_exi_qname type) {
    sxml_exi_frame *frame = decoding->frame_count == 0 ? NULL : &decoding->frames[decoding->frame_count - 1];
    if (!decoding->is_schema_informed || frame == NULL) {
        return;
    }
    const uint32_t grammar = decoding->table.uris[type.uri].local_names[type.local_name].type_grammar;
    if (grammar != SXML_EXI_NONE) {
        frame->schema_grammar = grammar;
    }
}

/* With Preserve.lexicalValues: xsi:type's text, its prefix resolved by
   the bindings in scope (none: no namespace). */
static sxml_status sxml_exi_switch_to_lexical_type(sxml_exi_decoding *decoding, sxml_byte_view text) {
    size_t colon = 0;
    while (colon < text.byte_count && text.bytes[colon] != ':') {
        colon++;
    }
    const bool has_prefix = colon < text.byte_count;
    const sxml_byte_view prefix = {.bytes = text.bytes, .byte_count = has_prefix ? colon : 0};
    const sxml_byte_view local = has_prefix ? (sxml_byte_view){.bytes = text.bytes + colon + 1, .byte_count = text.byte_count - colon - 1}
                                            : text;
    sxml_byte_view uri = {.bytes = NULL, .byte_count = 0};
    bool is_bound = !has_prefix;
    for (size_t binding_index = decoding->binding_count; binding_index > 0; binding_index--) {
        const sxml_exi_scoped_binding *binding = &decoding->bindings[binding_index - 1];
        const sxml_byte_view bound = sxml_exi_decoded_view(decoding, binding->prefix);
        if (bound.byte_count == prefix.byte_count &&
            (prefix.byte_count == 0 || memcmp(bound.bytes, prefix.bytes, prefix.byte_count) == 0)) {
            uri = sxml_exi_decoded_view(decoding, binding->uri);
            is_bound = true;
            break;
        }
    }
    if (!is_bound) {
        return SXML_STATUS_OK;
    }
    sxml_exi_lookup found_uri = {0};
    sxml_exi_lookup found_local = {0};
    sxml_status status = sxml_exi_find_uri(&decoding->table, &(sxml_exi_string_key){.text = uri}, &found_uri);
    if (status == SXML_STATUS_OK && found_uri.is_found) {
        status = sxml_exi_find_local_name(&decoding->table,
                                          &(sxml_exi_string_key){.text = local, .uri = found_uri.index}, &found_local);
    }
    if (status == SXML_STATUS_OK && found_uri.is_found && found_local.is_found) {
        sxml_exi_switch_to_type(decoding, (sxml_exi_qname){.uri = found_uri.index, .local_name = found_local.index});
    }
    return status;
}

static sxml_status sxml_exi_attribute(sxml_exi_decoding *decoding, const sxml_exi_decoded_event *event) {
    const sxml_exi_production *production = &event->production;
    const bool is_learned = event->is_learned;
    const sxml_exi_position position = sxml_exi_current_position(decoding);
    sxml_exi_qname qname = {.uri = production->uri, .local_name = production->local_name};
    sxml_status status = SXML_STATUS_OK;
    if (!is_learned) {
        status = sxml_exi_read_uri(decoding, &qname.uri);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_read_local_name(decoding, qname.uri, &qname.local_name);
        }
    }
    sxml_exi_text prefix = {0};
    if (status == SXML_STATUS_OK && decoding->options.fidelity.preserves_prefixes) {
        uint32_t prefix_index = 0;
        status = sxml_exi_read_qname_prefix(decoding, qname.uri, &prefix_index);
        if (status == SXML_STATUS_OK && decoding->table.uris[qname.uri].prefix_count > 0) {
            status = sxml_exi_prefix_text(decoding, (sxml_exi_qname){.uri = qname.uri, .local_name = prefix_index}, &prefix);
        }
    } else if (status == SXML_STATUS_OK) {
        status = sxml_exi_synthesize_prefix(decoding, qname.uri, &prefix);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* 8.4.3: AT (*) teaches AT (qname), but a second AT (xsi:type). */
    const bool is_type = qname.uri == 2 && qname.local_name == 1;
    if (!is_learned) {
        bool is_known = false;
        if (is_type) {
            const sxml_exi_learned *learned = sxml_exi_grammars_learned(&decoding->grammars, position);
            for (size_t production_index = 0; learned != NULL && production_index < learned->production_count;
                 production_index++) {
                const sxml_exi_production *known = &learned->productions[production_index];
                is_known = is_known || (known->event_kind == SXML_EXI_EVENT_ATTRIBUTE && known->uri == 2 && known->local_name == 1);
            }
        }
        if (!is_known) {
            status = sxml_exi_grammars_learn(
                &decoding->grammars,
                &(sxml_exi_learning){.position = position,
                                     .production = {.event_kind = SXML_EXI_EVENT_ATTRIBUTE,
                                                    .uri = qname.uri,
                                                    .local_name = qname.local_name}});
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
    }
    sxml_exi_value_handle value = {.is_slot = false};
    if (is_type && !decoding->options.fidelity.preserves_lexical_values) {
        /* xsi:type's value stays in the structure channel (9.2.1). */
        sxml_exi_type_name type_name = {0};
        status = sxml_exi_read_type_name(decoding, &type_name);
        value.text = (sxml_exi_text){.offset = (size_t)(type_name.value.bytes - decoding->text_bytes),
                                     .byte_count = (uint32_t)type_name.value.byte_count};
        if (status == SXML_STATUS_OK) {
            sxml_exi_switch_to_type(decoding, type_name.qname);
        }
    } else if (is_type && decoding->is_schema_informed) {
        /* Preserve.lexicalValues: a String, in the structure channel. */
        status = sxml_exi_read_value_text(decoding, (sxml_exi_typed_value){.qname = qname, .datatype = SXML_EXI_NONE},
                                          &value.text);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_switch_to_lexical_type(decoding, sxml_exi_decoded_view(decoding, value.text));
        }
    } else {
        status = sxml_exi_take_value(decoding, (sxml_exi_typed_value){.qname = qname, .datatype = SXML_EXI_NONE}, &value);
    }
    sxml_exi_pending_attribute attribute = {.value = value};
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_name_identifier(decoding, qname, &attribute.name);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_prefix_identifier(decoding, prefix, &attribute.prefix);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *elements = decoding->attributes;
    status = sxml_exi_grow(decoding->allocator, &(sxml_exi_array){.elements = &elements,
                                                                   .capacity = &decoding->attribute_capacity,
                                                                   .element_count = decoding->attribute_count,
                                                                   .element_size = sizeof(sxml_exi_pending_attribute)});
    if (status == SXML_STATUS_OK) {
        decoding->attributes = elements;
        decoding->attributes[decoding->attribute_count++] = attribute;
    }
    return status;
}

/* Whether a namespace declaration is one Namespaces in XML 1.0, section 3,
   allows: xml only for its namespace and that namespace only for xml,
   xmlns and its namespace never declared, and no prefix undeclared (that is
   XML 1.1's). A stream may carry any; the tree must not get them. */
static bool sxml_exi_is_allowed_binding(sxml_byte_view prefix, sxml_byte_view uri) {
    static const uint8_t xml_prefix[] = {'x', 'm', 'l'};
    static const uint8_t xmlns_prefix[] = {'x', 'm', 'l', 'n', 's'};
    static const uint8_t xml_uri[] = "http://www.w3.org/XML/1998/namespace";
    static const uint8_t xmlns_uri[] = "http://www.w3.org/2000/xmlns/";
    const sxml_byte_view xml = {.bytes = xml_prefix, .byte_count = sizeof(xml_prefix)};
    const sxml_byte_view xmlns = {.bytes = xmlns_prefix, .byte_count = sizeof(xmlns_prefix)};
    const sxml_byte_view xml_namespace = {.bytes = xml_uri, .byte_count = sizeof(xml_uri) - 1};
    const sxml_byte_view xmlns_namespace = {.bytes = xmlns_uri, .byte_count = sizeof(xmlns_uri) - 1};
    const bool is_xml_prefix = sxml_byte_view_equals(prefix, xml);
    const bool is_xml_namespace = sxml_byte_view_equals(uri, xml_namespace);
    if (is_xml_prefix != is_xml_namespace) {
        return false;
    }
    if (sxml_byte_view_equals(prefix, xmlns) || sxml_byte_view_equals(uri, xmlns_namespace)) {
        return false;
    }
    return prefix.byte_count == 0 || uri.byte_count > 0;
}

static sxml_status sxml_exi_namespace(sxml_exi_decoding *decoding) {
    uint32_t uri = 0;
    sxml_status status = sxml_exi_read_uri(decoding, &uri);
    uint32_t value = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_read_n_bit(
            &decoding->reader,
            (sxml_exi_bits_request){.bit_count = sxml_exi_bit_width(decoding->table.uris[uri].prefix_count + 1)}, &value);
    }
    uint32_t prefix_index = 0;
    if (status == SXML_STATUS_OK) {
        if (value != 0) {
            if (value - 1 >= decoding->table.uris[uri].prefix_count) {
                return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "a prefix compact identifier past the partition");
            }
            prefix_index = value - 1;
        } else {
            sxml_exi_text literal = {0};
            status = sxml_exi_read_string(decoding, &literal);
            if (status == SXML_STATUS_OK) {
                status = sxml_exi_add_prefix(
                    &decoding->table, &(sxml_exi_string_key){.text = sxml_exi_decoded_view(decoding, literal), .uri = uri},
                    &prefix_index);
            }
        }
    }
    sxml_exi_boolean is_local_element_namespace = {.value = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_read_boolean(&decoding->reader, &is_local_element_namespace);
    }
    sxml_exi_text prefix = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_prefix_text(decoding, (sxml_exi_qname){.uri = uri, .local_name = prefix_index}, &prefix);
    }
    const size_t uri_start = decoding->text_byte_count;
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_append_text(decoding, sxml_exi_string_table_view(&decoding->table, decoding->table.uris[uri].text));
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_exi_text uri_text = {.offset = uri_start, .byte_count = (uint32_t)(decoding->text_byte_count - uri_start)};
    if (!sxml_exi_is_allowed_binding(sxml_exi_decoded_view(decoding, prefix), sxml_exi_decoded_view(decoding, uri_text))) {
        return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT,
                             "a namespace declaration Namespaces in XML 1.0 does not allow (section 3)");
    }
    if (is_local_element_namespace.value && (!decoding->has_pending_start || uri != decoding->pending_uri)) {
        /* EXI 1.0, 4: local-element-ns marks the declaration of the element's
           own prefix, so its URI is the element's. */
        return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT,
                             "a local-element-ns declaration of another namespace than its element's");
    }
    if (is_local_element_namespace.value) {
        decoding->pending_local_prefix = prefix;
        decoding->has_pending_local_prefix = true;
    }
    return sxml_exi_add_declaration(
        decoding, &(sxml_exi_pending_binding){
                      .prefix = prefix,
                      .uri = uri_text});
}

static sxml_status sxml_exi_characters(sxml_exi_decoding *decoding, const sxml_exi_decoded_event *event) {
    const bool is_learned = event->is_learned;
    const sxml_exi_position position = sxml_exi_current_position(decoding);
    const sxml_exi_frame *frame = sxml_exi_top_frame(decoding);
    if (frame == NULL) {
        return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "characters outside the document element");
    }
    sxml_status status = SXML_STATUS_OK;
    if (!is_learned && !sxml_exi_has_learned(decoding, position, SXML_EXI_EVENT_CHARACTERS)) {
        status = sxml_exi_grammars_learn(
            &decoding->grammars,
            &(sxml_exi_learning){.position = position, .production = {.event_kind = SXML_EXI_EVENT_CHARACTERS}});
    }
    const sxml_exi_qname qname = {.uri = frame->uri, .local_name = frame->local_name};
    sxml_exi_move_to(decoding, SXML_EXI_NONTERMINAL_ELEMENT_CONTENT);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_flush_start(decoding);
    }
    sxml_exi_value_handle value = {.is_slot = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_take_value(decoding, (sxml_exi_typed_value){.qname = qname, .datatype = SXML_EXI_NONE}, &value);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_emit(decoding, &(sxml_exi_emitted){.emitted_kind = SXML_EXI_EMITTED_TEXT, .text = value});
    }
    return status;
}

static sxml_status sxml_exi_comment(sxml_exi_decoding *decoding) {
    sxml_exi_text text = {0};
    sxml_status status = sxml_exi_read_string(decoding, &text);
    if (status == SXML_STATUS_OK && sxml_exi_top_frame(decoding) != NULL) {
        sxml_exi_move_to(decoding, SXML_EXI_NONTERMINAL_ELEMENT_CONTENT);
        status = sxml_exi_flush_start(decoding);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_emit(decoding, &(sxml_exi_emitted){.emitted_kind = SXML_EXI_EMITTED_COMMENT,
                                                             .text = {.is_slot = false, .text = text}});
    }
    return status;
}

static sxml_status sxml_exi_processing_instruction(sxml_exi_decoding *decoding) {
    sxml_exi_text target = {0};
    sxml_exi_text data = {0};
    sxml_status status = sxml_exi_read_string(decoding, &target);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_read_string(decoding, &data);
    }
    if (status == SXML_STATUS_OK && target.byte_count == 0) {
        return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "a processing instruction without a target");
    }
    if (status == SXML_STATUS_OK && sxml_exi_top_frame(decoding) != NULL) {
        sxml_exi_move_to(decoding, SXML_EXI_NONTERMINAL_ELEMENT_CONTENT);
        status = sxml_exi_flush_start(decoding);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_emit(decoding, &(sxml_exi_emitted){.emitted_kind = SXML_EXI_EMITTED_PROCESSING_INSTRUCTION,
                                                             .text = {.is_slot = false, .text = target},
                                                             .data = data});
    }
    return status;
}

/* A DT event: its four strings, which a tree has no place for (D333). */
static sxml_status sxml_exi_doctype(sxml_exi_decoding *decoding) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t string_index = 0; string_index < 4 && status == SXML_STATUS_OK; string_index++) {
        sxml_exi_text ignored = {0};
        status = sxml_exi_read_string(decoding, &ignored);
    }
    return status;
}

/* ------------------------------------------------ Schema-informed events */

/* The schema-informed grammar of the innermost element or the top level;
   SXML_EXI_NONE in a built-in element grammar. */
static uint32_t sxml_exi_current_schema_grammar(const sxml_exi_decoding *decoding) {
    if (!decoding->is_schema_informed) {
        return SXML_EXI_NONE;
    }
    if (decoding->frame_count == 0) {
        return decoding->top_schema_grammar;
    }
    return decoding->frames[decoding->frame_count - 1].schema_grammar;
}

static void sxml_exi_move_to_schema_grammar(sxml_exi_decoding *decoding, uint32_t grammar) {
    if (decoding->frame_count == 0) {
        decoding->top_schema_grammar = grammar;
    } else {
        decoding->frames[decoding->frame_count - 1].schema_grammar = grammar;
    }
}

/* Where undeclared SE, CH, ER, CM and PI lead (8.5.4.4.1): a start tag's
   Element_{i,content2}; an element grammar stays. */
static uint32_t sxml_exi_content_grammar_of(const sxml_exi_decoding *decoding, uint32_t grammar) {
    const uint32_t content = decoding->schema_grammars.grammars[grammar].element_content;
    return content != SXML_EXI_NONE ? content : grammar;
}

/* An element started from a schema-informed grammar: its qname, the
   grammar it is evaluated with (SXML_EXI_NONE: its built-in element
   grammar, 8.4.3) and where its parent goes on. */
typedef struct sxml_exi_schema_start {
    sxml_exi_qname qname;
    uint32_t element_grammar;
    uint32_t parent_grammar;
} sxml_exi_schema_start;

/* The grammar of an element matched by SE (*) or SE (uri : *): the global
   element's, else a built-in one (8.5.4.1.7). */
static uint32_t sxml_exi_global_element_grammar(const sxml_exi_decoding *decoding, sxml_exi_qname qname) {
    return decoding->table.uris[qname.uri].local_names[qname.local_name].schema_element_grammar;
}

static sxml_status sxml_exi_schema_start_element(sxml_exi_decoding *decoding, const sxml_exi_schema_start *start) {
    const sxml_exi_qname qname = start->qname;
    uint32_t prefix_index = 0;
    sxml_status status = SXML_STATUS_OK;
    if (decoding->options.fidelity.preserves_prefixes) {
        status = sxml_exi_read_qname_prefix(decoding, qname.uri, &prefix_index);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_flush_start(decoding);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_exi_move_to_schema_grammar(decoding, start->parent_grammar);
    if (decoding->frame_count >= decoding->limits.maximum_depth) {
        return SXML_EXI_FAIL(decoding, SXML_STATUS_LIMIT_EXCEEDED, "elements nested deeper than the limit");
    }
    uint32_t element_grammar = SXML_EXI_NONE;
    if (start->element_grammar == SXML_EXI_NONE) {
        sxml_exi_local_name *entry = &decoding->table.uris[qname.uri].local_names[qname.local_name];
        if (entry->element_grammar == SXML_EXI_NONE) {
            uint32_t made = 0;
            status = sxml_exi_grammars_add_element(&decoding->grammars, &made);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            entry = &decoding->table.uris[qname.uri].local_names[qname.local_name];
            entry->element_grammar = made;
        }
        element_grammar = entry->element_grammar;
    }
    void *elements = decoding->frames;
    status = sxml_exi_grow(decoding->allocator, &(sxml_exi_array){.elements = &elements,
                                                                   .capacity = &decoding->frame_capacity,
                                                                   .element_count = decoding->frame_count,
                                                                   .element_size = sizeof(sxml_exi_frame)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    decoding->frames = elements;
    decoding->frames[decoding->frame_count++] = (sxml_exi_frame){
        .uri = qname.uri,
        .local_name = qname.local_name,
        .element_grammar = element_grammar,
        .nonterminal = SXML_EXI_NONTERMINAL_START_TAG_CONTENT,
        .schema_grammar = start->element_grammar,
        .binding_mark = decoding->binding_count,
    };
    status = sxml_exi_name_identifier(decoding, qname, &decoding->pending_name);
    decoding->has_pending_start = true;
    decoding->pending_uri = qname.uri;
    decoding->pending_prefix_index = prefix_index;
    decoding->has_pending_prefix_index = decoding->options.fidelity.preserves_prefixes;
    decoding->has_pending_local_prefix = false;
    return status;
}

static sxml_status sxml_exi_schema_end_element(sxml_exi_decoding *decoding) {
    if (decoding->frame_count == 0) {
        return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "an end of element outside any element");
    }
    sxml_status status = sxml_exi_flush_start(decoding);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_emit(decoding, &(sxml_exi_emitted){.emitted_kind = SXML_EXI_EMITTED_END_ELEMENT});
    }
    if (status == SXML_STATUS_OK) {
        decoding->binding_count = decoding->frames[decoding->frame_count - 1].binding_mark;
        decoding->frame_count--;
    }
    return status;
}

/* The prefix of an attribute's qname: read with Preserve.prefixes (7.1.7),
   else one in scope or made. */
static sxml_status sxml_exi_attribute_prefix(sxml_exi_decoding *decoding, uint32_t uri, sxml_exi_text *prefix) {
    *prefix = (sxml_exi_text){0};
    if (!decoding->options.fidelity.preserves_prefixes) {
        return sxml_exi_synthesize_prefix(decoding, uri, prefix);
    }
    uint32_t prefix_index = 0;
    sxml_status status = sxml_exi_read_qname_prefix(decoding, uri, &prefix_index);
    if (status == SXML_STATUS_OK && decoding->table.uris[uri].prefix_count > 0) {
        status = sxml_exi_prefix_text(decoding, (sxml_exi_qname){.uri = uri, .local_name = prefix_index}, prefix);
    }
    return status;
}

/* Holds an attribute of the start tag being read. */
typedef struct sxml_exi_held_attribute {
    sxml_exi_qname qname;
    sxml_exi_text prefix;
    sxml_exi_value_handle value;
} sxml_exi_held_attribute;

static sxml_status sxml_exi_hold_attribute(sxml_exi_decoding *decoding, const sxml_exi_held_attribute *held) {
    if (!decoding->has_pending_start) {
        return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "an attribute after the start tag");
    }
    sxml_exi_pending_attribute attribute = {.value = held->value};
    sxml_status status = sxml_exi_name_identifier(decoding, held->qname, &attribute.name);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_prefix_identifier(decoding, held->prefix, &attribute.prefix);
    }
    void *elements = decoding->attributes;
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_grow(decoding->allocator, &(sxml_exi_array){.elements = &elements,
                                                                       .capacity = &decoding->attribute_capacity,
                                                                       .element_count = decoding->attribute_count,
                                                                       .element_size = sizeof(sxml_exi_pending_attribute)});
    }
    if (status == SXML_STATUS_OK) {
        decoding->attributes = elements;
        decoding->attributes[decoding->attribute_count++] = attribute;
    }
    return status;
}

static bool sxml_exi_is_type_attribute(sxml_exi_qname qname) {
    return qname.uri == 2 && qname.local_name == 1;
}

/* An attribute of a schema-informed grammar: its prefix, then its value,
   typed by datatype, or by the global attribute of its qname when
   is_wildcard (AT (*) and AT (uri : *), 8.5.4.1.3.2, 8.5.4.4.1); an
   xsi:type an AT (*) matched is a QName that switches the grammar. */
typedef struct sxml_exi_schema_attribute {
    sxml_exi_qname qname;
    uint32_t datatype;
    bool is_wildcard;
} sxml_exi_schema_attribute;

static sxml_status sxml_exi_read_schema_attribute(sxml_exi_decoding *decoding, const sxml_exi_schema_attribute *request) {
    const sxml_exi_qname qname = request->qname;
    sxml_exi_held_attribute held = {.qname = qname};
    sxml_status status = sxml_exi_attribute_prefix(decoding, qname.uri, &held.prefix);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (request->is_wildcard && sxml_exi_is_type_attribute(qname)) {
        if (decoding->options.fidelity.preserves_lexical_values) {
            status = sxml_exi_read_value_text(decoding, (sxml_exi_typed_value){.qname = qname, .datatype = SXML_EXI_NONE},
                                              &held.value.text);
            if (status == SXML_STATUS_OK) {
                status = sxml_exi_switch_to_lexical_type(decoding, sxml_exi_decoded_view(decoding, held.value.text));
            }
        } else {
            sxml_exi_type_name type_name = {0};
            status = sxml_exi_read_type_name(decoding, &type_name);
            held.value.text = (sxml_exi_text){.offset = (size_t)(type_name.value.bytes - decoding->text_bytes),
                                              .byte_count = (uint32_t)type_name.value.byte_count};
            if (status == SXML_STATUS_OK) {
                sxml_exi_switch_to_type(decoding, type_name.qname);
            }
        }
    } else {
        const uint32_t datatype =
            request->is_wildcard ? decoding->table.uris[qname.uri].local_names[qname.local_name].attribute_datatype
                                 : request->datatype;
        status = sxml_exi_take_value(decoding, (sxml_exi_typed_value){.qname = qname, .datatype = datatype}, &held.value);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_hold_attribute(decoding, &held);
    }
    return status;
}

/* AT (xsi:type) (8.5.4.4): a QName (or with Preserve.lexicalValues a
   String), in the structure channel (9.2.1); the type's grammar from
   here on. */
static sxml_status sxml_exi_read_schema_type(sxml_exi_decoding *decoding) {
    const sxml_exi_qname qname = {.uri = 2, .local_name = 1};
    sxml_exi_held_attribute held = {.qname = qname};
    sxml_status status = sxml_exi_attribute_prefix(decoding, qname.uri, &held.prefix);
    if (status == SXML_STATUS_OK && decoding->options.fidelity.preserves_lexical_values) {
        status = sxml_exi_read_value_text(decoding, (sxml_exi_typed_value){.qname = qname, .datatype = SXML_EXI_NONE},
                                          &held.value.text);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_switch_to_lexical_type(decoding, sxml_exi_decoded_view(decoding, held.value.text));
        }
    } else if (status == SXML_STATUS_OK) {
        sxml_exi_type_name type_name = {0};
        status = sxml_exi_read_type_name(decoding, &type_name);
        held.value.text = (sxml_exi_text){.offset = (size_t)(type_name.value.bytes - decoding->text_bytes),
                                          .byte_count = (uint32_t)type_name.value.byte_count};
        if (status == SXML_STATUS_OK) {
            sxml_exi_switch_to_type(decoding, type_name.qname);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_hold_attribute(decoding, &held);
    }
    return status;
}

/* AT (xsi:nil) (8.5.4.4): a Boolean (or a String of Table 7-2's boolean
   set), in the structure channel; true makes TypeEmpty the grammar. */
static sxml_status sxml_exi_read_schema_nil(sxml_exi_decoding *decoding, uint32_t grammar) {
    const sxml_exi_qname qname = {.uri = 2, .local_name = 0};
    sxml_exi_held_attribute held = {.qname = qname};
    sxml_status status = sxml_exi_attribute_prefix(decoding, qname.uri, &held.prefix);
    bool is_nil = false;
    if (status == SXML_STATUS_OK && decoding->options.fidelity.preserves_lexical_values) {
        status = sxml_exi_read_value_text(
            decoding, (sxml_exi_typed_value){.qname = qname, .datatype = decoding->schema_grammars.boolean_datatype},
            &held.value.text);
        const sxml_byte_view text = sxml_exi_decoded_view(decoding, held.value.text);
        is_nil = (text.byte_count == 4 && memcmp(text.bytes, "true", 4) == 0) ||
                 (text.byte_count == 1 && text.bytes[0] == '1');
    } else if (status == SXML_STATUS_OK) {
        sxml_exi_boolean boolean = {0};
        status = sxml_exi_read_boolean(&decoding->reader, &boolean);
        is_nil = boolean.value;
        const size_t start = decoding->text_byte_count;
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_append_text(decoding, is_nil ? SXML_EXI_TEXT("true") : SXML_EXI_TEXT("false"));
        }
        held.value.text = (sxml_exi_text){.offset = start, .byte_count = (uint32_t)(decoding->text_byte_count - start)};
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_hold_attribute(decoding, &held);
    }
    if (status == SXML_STATUS_OK && is_nil) {
        const uint32_t type_empty = decoding->schema_grammars.grammars[grammar].type_empty;
        if (type_empty != SXML_EXI_NONE) {
            sxml_exi_move_to_schema_grammar(decoding, type_empty);
        }
    }
    return status;
}

/* A CH of a schema-informed grammar: a typed value of datatype, or a
   String (SXML_EXI_NONE); then grammar next. */
typedef struct sxml_exi_schema_characters {
    uint32_t datatype;
    uint32_t next;
} sxml_exi_schema_characters;

static sxml_status sxml_exi_read_schema_characters(sxml_exi_decoding *decoding,
                                                   const sxml_exi_schema_characters *characters) {
    const sxml_exi_frame *frame = sxml_exi_top_frame(decoding);
    if (frame == NULL) {
        return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "characters outside the document element");
    }
    const sxml_exi_qname qname = {.uri = frame->uri, .local_name = frame->local_name};
    sxml_exi_move_to_schema_grammar(decoding, characters->next);
    sxml_status status = sxml_exi_flush_start(decoding);
    sxml_exi_value_handle value = {.is_slot = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_take_value(decoding, (sxml_exi_typed_value){.qname = qname, .datatype = characters->datatype},
                                     &value);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_emit(decoding, &(sxml_exi_emitted){.emitted_kind = SXML_EXI_EMITTED_TEXT, .text = value});
    }
    return status;
}

/* A qname read in full (SE (*), AT (*)): its URI, then its local-name. */
static sxml_status sxml_exi_read_qname(sxml_exi_decoding *decoding, sxml_exi_qname *qname) {
    sxml_status status = sxml_exi_read_uri(decoding, &qname->uri);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_read_local_name(decoding, qname->uri, &qname->local_name);
    }
    return status;
}

/* A declared production of a schema-informed grammar (8.5.4.3). */
static sxml_status sxml_exi_decode_declared(sxml_exi_decoding *decoding, const sxml_exi_schema_production *production) {
    sxml_exi_qname qname = {.uri = production->uri, .local_name = production->local_name};
    sxml_status status = SXML_STATUS_OK;
    switch (production->event_kind) {
    case SXML_EXI_EVENT_START_ELEMENT:
        return sxml_exi_schema_start_element(
            decoding, &(sxml_exi_schema_start){.qname = qname,
                                               .element_grammar = decoding->schema_grammars.element_grammars[production->element],
                                               .parent_grammar = production->next});
    case SXML_EXI_EVENT_START_ELEMENT_URI:
    case SXML_EXI_EVENT_START_ELEMENT_ANY:
        if (production->event_kind == SXML_EXI_EVENT_START_ELEMENT_URI) {
            status = sxml_exi_read_local_name(decoding, qname.uri, &qname.local_name);
        } else {
            status = sxml_exi_read_qname(decoding, &qname);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_exi_schema_start_element(
            decoding, &(sxml_exi_schema_start){.qname = qname,
                                               .element_grammar = sxml_exi_global_element_grammar(decoding, qname),
                                               .parent_grammar = production->next});
    case SXML_EXI_EVENT_END_ELEMENT:
        return sxml_exi_schema_end_element(decoding);
    case SXML_EXI_EVENT_ATTRIBUTE:
        sxml_exi_move_to_schema_grammar(decoding, production->next);
        return sxml_exi_read_schema_attribute(
            decoding, &(sxml_exi_schema_attribute){.qname = qname, .datatype = production->datatype, .is_wildcard = false});
    case SXML_EXI_EVENT_ATTRIBUTE_URI:
    case SXML_EXI_EVENT_ATTRIBUTE_ANY:
        if (production->event_kind == SXML_EXI_EVENT_ATTRIBUTE_URI) {
            status = sxml_exi_read_local_name(decoding, qname.uri, &qname.local_name);
        } else {
            status = sxml_exi_read_qname(decoding, &qname);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_exi_move_to_schema_grammar(decoding, production->next);
        return sxml_exi_read_schema_attribute(
            decoding, &(sxml_exi_schema_attribute){.qname = qname, .datatype = SXML_EXI_NONE, .is_wildcard = true});
    case SXML_EXI_EVENT_CHARACTERS:
        return sxml_exi_read_schema_characters(
            decoding, &(sxml_exi_schema_characters){.datatype = production->datatype, .next = production->next});
    case SXML_EXI_EVENT_END_DOCUMENT:
        decoding->is_finished = true;
        return SXML_STATUS_OK;
    case SXML_EXI_EVENT_START_DOCUMENT:
    case SXML_EXI_EVENT_NAMESPACE:
    case SXML_EXI_EVENT_COMMENT:
    case SXML_EXI_EVENT_PROCESSING_INSTRUCTION:
    case SXML_EXI_EVENT_DOCTYPE:
    case SXML_EXI_EVENT_ENTITY_REFERENCE:
    case SXML_EXI_EVENT_SELF_CONTAINED:
        break;
    }
    return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "a production schema-informed grammars do not have");
}

/* An undeclared production (8.5.4.4) of grammar. */
typedef struct sxml_exi_undeclared_event {
    uint32_t grammar;
    const sxml_exi_schema_match *match;
} sxml_exi_undeclared_event;

static sxml_status sxml_exi_decode_undeclared(sxml_exi_decoding *decoding, const sxml_exi_undeclared_event *event) {
    const uint32_t grammar = event->grammar;
    const sxml_exi_schema_grammars *grammars = &decoding->schema_grammars;
    sxml_exi_qname qname = {0};
    sxml_status status = SXML_STATUS_OK;
    switch (event->match->undeclared) {
    case SXML_EXI_UNDECLARED_END_ELEMENT:
        return sxml_exi_schema_end_element(decoding);
    case SXML_EXI_UNDECLARED_TYPE:
        return sxml_exi_read_schema_type(decoding);
    case SXML_EXI_UNDECLARED_NIL:
        return sxml_exi_read_schema_nil(decoding, grammar);
    case SXML_EXI_UNDECLARED_ATTRIBUTE_ANY:
        status = sxml_exi_read_qname(decoding, &qname);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_exi_read_schema_attribute(
            decoding, &(sxml_exi_schema_attribute){.qname = qname, .datatype = SXML_EXI_NONE, .is_wildcard = true});
    case SXML_EXI_UNDECLARED_ATTRIBUTE_INVALID: {
        const sxml_exi_schema_grammar *current = &grammars->grammars[grammar];
        if (event->match->attribute < current->attribute_count) {
            const sxml_exi_schema_production *production =
                sxml_exi_schema_production_at(grammars, grammar, current->least_attribute + event->match->attribute);
            qname = (sxml_exi_qname){.uri = production->uri, .local_name = production->local_name};
            sxml_exi_move_to_schema_grammar(decoding, production->next);
        } else {
            status = sxml_exi_read_qname(decoding, &qname);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        return sxml_exi_read_schema_attribute(
            decoding, &(sxml_exi_schema_attribute){.qname = qname, .datatype = SXML_EXI_NONE, .is_wildcard = false});
    }
    case SXML_EXI_UNDECLARED_NAMESPACE:
        return sxml_exi_namespace(decoding);
    case SXML_EXI_UNDECLARED_SELF_CONTAINED:
        return SXML_EXI_FAIL(decoding, SXML_STATUS_UNSUPPORTED_FEATURE, "self-contained elements are not read yet");
    case SXML_EXI_UNDECLARED_START_ELEMENT_ANY:
        status = sxml_exi_read_qname(decoding, &qname);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return sxml_exi_schema_start_element(
            decoding, &(sxml_exi_schema_start){.qname = qname,
                                               .element_grammar = sxml_exi_global_element_grammar(decoding, qname),
                                               .parent_grammar = sxml_exi_content_grammar_of(decoding, grammar)});
    case SXML_EXI_UNDECLARED_CHARACTERS:
        return sxml_exi_read_schema_characters(
            decoding, &(sxml_exi_schema_characters){.datatype = SXML_EXI_NONE,
                                                    .next = sxml_exi_content_grammar_of(decoding, grammar)});
    case SXML_EXI_UNDECLARED_ENTITY_REFERENCE:
        return SXML_EXI_FAIL(decoding, SXML_STATUS_UNSUPPORTED_FEATURE,
                             "an entity reference, which a tree cannot hold unexpanded");
    case SXML_EXI_UNDECLARED_DOCTYPE:
        return sxml_exi_doctype(decoding);
    }
    return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "an undeclared production unknown here");
}

/* One event of a schema-informed grammar. */
static sxml_status sxml_exi_decode_schema_event(sxml_exi_decoding *decoding, uint32_t grammar) {
    sxml_exi_schema_match match = {0};
    sxml_status status = sxml_exi_schema_read_event_code(
        &decoding->schema_grammars, &(sxml_exi_schema_code_read){.grammar = grammar, .reader = &decoding->reader}, &match);
    if (status != SXML_STATUS_OK) {
        return status == SXML_STATUS_MALFORMED_INPUT
                   ? SXML_EXI_FAIL(decoding, status, "an event code no production of the grammar has")
                   : status;
    }
    switch (match.match_kind) {
    case SXML_EXI_SCHEMA_MATCH_DECLARED:
        return sxml_exi_decode_declared(decoding, sxml_exi_schema_production_at(&decoding->schema_grammars, grammar, match.code));
    case SXML_EXI_SCHEMA_MATCH_UNDECLARED:
        return sxml_exi_decode_undeclared(decoding, &(sxml_exi_undeclared_event){.grammar = grammar, .match = &match});
    case SXML_EXI_SCHEMA_MATCH_COMMENT:
    case SXML_EXI_SCHEMA_MATCH_PROCESSING_INSTRUCTION:
        status = match.match_kind == SXML_EXI_SCHEMA_MATCH_COMMENT ? sxml_exi_comment(decoding)
                                                                   : sxml_exi_processing_instruction(decoding);
        if (status == SXML_STATUS_OK && decoding->frame_count > 0) {
            sxml_exi_move_to_schema_grammar(decoding, sxml_exi_content_grammar_of(decoding, grammar));
        }
        return status;
    }
    return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "an event code no production of the grammar has");
}

/* One event at the current position; is_finished at ED. */
static sxml_status sxml_exi_decode_event(sxml_exi_decoding *decoding) {
    const uint32_t schema_grammar = sxml_exi_current_schema_grammar(decoding);
    if (schema_grammar != SXML_EXI_NONE) {
        return sxml_exi_decode_schema_event(decoding, schema_grammar);
    }
    const sxml_exi_position position = sxml_exi_current_position(decoding);
    sxml_exi_match match = {0};
    sxml_status status = sxml_exi_read_event_code(
        &decoding->grammars, &(sxml_exi_event_code_read){.position = position, .reader = &decoding->reader}, &match);
    if (status != SXML_STATUS_OK) {
        return status == SXML_STATUS_MALFORMED_INPUT
                   ? SXML_EXI_FAIL(decoding, status, "an event code no production of the grammar has")
                   : status;
    }
    const sxml_exi_decoded_event event = {
        .production = sxml_exi_match_production(&decoding->grammars, position, match), .is_learned = match.is_learned};
    switch (event.production.event_kind) {
    case SXML_EXI_EVENT_START_ELEMENT:
    case SXML_EXI_EVENT_START_ELEMENT_ANY:
        return sxml_exi_start_element(decoding, &event);
    case SXML_EXI_EVENT_END_ELEMENT:
        return sxml_exi_end_element(decoding, &event);
    case SXML_EXI_EVENT_ATTRIBUTE:
    case SXML_EXI_EVENT_ATTRIBUTE_ANY:
        return sxml_exi_attribute(decoding, &event);
    case SXML_EXI_EVENT_NAMESPACE:
        return sxml_exi_namespace(decoding);
    case SXML_EXI_EVENT_CHARACTERS:
        return sxml_exi_characters(decoding, &event);
    case SXML_EXI_EVENT_COMMENT:
        return sxml_exi_comment(decoding);
    case SXML_EXI_EVENT_PROCESSING_INSTRUCTION:
        return sxml_exi_processing_instruction(decoding);
    case SXML_EXI_EVENT_DOCTYPE:
        return sxml_exi_doctype(decoding);
    case SXML_EXI_EVENT_END_DOCUMENT:
        decoding->is_finished = true;
        return SXML_STATUS_OK;
    case SXML_EXI_EVENT_ENTITY_REFERENCE:
        return SXML_EXI_FAIL(decoding, SXML_STATUS_UNSUPPORTED_FEATURE,
                             "an entity reference, which a tree cannot hold unexpanded");
    case SXML_EXI_EVENT_SELF_CONTAINED:
        return SXML_EXI_FAIL(decoding, SXML_STATUS_UNSUPPORTED_FEATURE, "self-contained elements are not read yet");
    case SXML_EXI_EVENT_START_DOCUMENT:
    case SXML_EXI_EVENT_START_ELEMENT_URI:
    case SXML_EXI_EVENT_ATTRIBUTE_URI:
        break;
    }
    return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "an event the built-in grammars do not have");
}

/* --------------------------------------------------------------- Header */

/* EXI 5: [cookie] 1 0, presence bit, version; padding later. */
static sxml_status sxml_exi_read_header(sxml_exi_decoding *decoding) {
    sxml_exi_bit_reader *reader = &decoding->reader;
    if (reader->input.byte_count >= 4 && memcmp(reader->input.bytes, "$EXI", 4) == 0) {
        reader->byte_position = 4;
    }
    uint32_t bits = 0;
    sxml_status status = sxml_exi_read_bits(reader, (sxml_exi_bits_request){.bit_count = 2}, &bits);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (bits != 2) {
        return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "no EXI distinguishing bits");
    }
    uint32_t presence = 0;
    uint32_t preview = 0;
    status = sxml_exi_read_bits(reader, (sxml_exi_bits_request){.bit_count = 1}, &presence);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_read_bits(reader, (sxml_exi_bits_request){.bit_count = 1}, &preview);
    }
    uint32_t version = 1;
    for (;;) {
        uint32_t group = 0;
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_read_bits(reader, (sxml_exi_bits_request){.bit_count = 4}, &group);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        version += group;
        if (group != 15) {
            break;
        }
        if (version > 1000) {
            return SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "an EXI version field that does not end");
        }
    }
    if (preview != 0 || version != 1) {
        return SXML_EXI_FAIL(decoding, SXML_STATUS_UNSUPPORTED_FEATURE, "an EXI format version other than final version 1");
    }
    decoding->has_header_options = presence != 0;
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------------------- Blocks */

/* Opens the next compressed stream (9.3): with compression, inflates it
   from the raw stream; with pre-compression the streams are back to back
   and reading goes on. No bytes left is an empty stream (a final block
   whose events take no bytes has none). */
static sxml_status sxml_exi_open_stream(sxml_exi_decoding *decoding) {
    if (!decoding->is_compressed) {
        return SXML_STATUS_OK;
    }
    sxml_allocator_release(decoding->allocator, decoding->inflated);
    decoding->inflated = NULL;
    decoding->inflated_byte_count = 0;
    decoding->reader = (sxml_exi_bit_reader){.input = {.bytes = NULL, .byte_count = 0}, .is_byte_aligned = true};
    if (decoding->raw_position >= decoding->raw_input.byte_count) {
        return SXML_STATUS_OK;
    }
    const uint64_t left = decoding->limits.maximum_content_byte_count > decoding->content_byte_count
                              ? decoding->limits.maximum_content_byte_count - decoding->content_byte_count
                              : 0;
    sxml_inflate_result result = {0};
    sxml_status status = sxml_inflate(
        &(sxml_inflate_request){.input = {.bytes = decoding->raw_input.bytes + decoding->raw_position,
                                          .byte_count = decoding->raw_input.byte_count - decoding->raw_position},
                                .allocator = decoding->allocator,
                                .maximum_output_byte_count = left > SIZE_MAX ? SIZE_MAX : (size_t)left},
        &result);
    if (status != SXML_STATUS_OK) {
        return status == SXML_STATUS_LIMIT_EXCEEDED
                   ? SXML_EXI_FAIL(decoding, status, "a compressed stream inflates past the content limit")
                   : SXML_EXI_FAIL(decoding, status, "a compressed stream that DEFLATE cannot inflate");
    }
    decoding->raw_position += result.consumed_byte_count;
    decoding->inflated = result.output.bytes;
    decoding->inflated_byte_count = result.output.byte_count;
    decoding->reader.input = (sxml_byte_view){.bytes = result.output.bytes, .byte_count = result.output.byte_count};
    return SXML_STATUS_OK;
}

/* Reads the values of one channel of the block into their slots. */
static sxml_status sxml_exi_read_channel(sxml_exi_decoding *decoding, uint32_t channel) {
    const sxml_exi_channel *values = &decoding->channels[channel];
    const sxml_exi_qname qname = {.uri = values->uri, .local_name = values->local_name};
    for (uint32_t value_index = 0; value_index < values->value_count; value_index++) {
        const uint32_t slot = decoding->channel_slots[values->first_slot_entry + value_index];
        sxml_exi_text text = {0};
        sxml_status status = sxml_exi_read_value_text(
            decoding, (sxml_exi_typed_value){.qname = qname, .datatype = decoding->slots[slot].datatype}, &text);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        decoding->slots[slot].text = text;
    }
    return SXML_STATUS_OK;
}

/* Ends a block (9): its values, read channel by channel in the order of
   9.3, then its events sent; then the next block's stream opened. */
static sxml_status sxml_exi_end_block(sxml_exi_decoding *decoding) {
    sxml_status status = SXML_STATUS_OK;
    if (decoding->slot_count > decoding->channel_slot_capacity) {
        void *memory = NULL;
        status = sxml_allocator_allocate_array(decoding->allocator,
                                               (sxml_array_allocation_request){.element_count = decoding->slot_count,
                                                                               .element_size = sizeof(uint32_t),
                                                                               .element_alignment = alignof(uint32_t)},
                                               &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_allocator_release(decoding->allocator, decoding->channel_slots);
        decoding->channel_slots = memory;
        decoding->channel_slot_capacity = decoding->slot_count;
    }
    /* Each channel's slots, in order: offsets by count, then filled. */
    size_t offset = 0;
    for (size_t channel_index = 0; channel_index < decoding->channel_count; channel_index++) {
        decoding->channels[channel_index].first_slot_entry = offset;
        offset += decoding->channels[channel_index].value_count;
        decoding->channels[channel_index].value_count = 0;
    }
    for (size_t slot_index = 0; slot_index < decoding->slot_count; slot_index++) {
        sxml_exi_channel *channel = &decoding->channels[decoding->slots[slot_index].channel];
        decoding->channel_slots[channel->first_slot_entry + channel->value_count++] = (uint32_t)slot_index;
    }
    if (decoding->block_value_count <= 100) {
        for (uint32_t channel_index = 0; channel_index < decoding->channel_count && status == SXML_STATUS_OK; channel_index++) {
            status = sxml_exi_read_channel(decoding, channel_index);
        }
    } else {
        bool has_small = false;
        for (size_t channel_index = 0; channel_index < decoding->channel_count; channel_index++) {
            has_small = has_small || decoding->channels[channel_index].value_count <= 100;
        }
        if (has_small) {
            status = sxml_exi_open_stream(decoding);
        }
        for (uint32_t channel_index = 0; channel_index < decoding->channel_count && status == SXML_STATUS_OK; channel_index++) {
            if (decoding->channels[channel_index].value_count <= 100) {
                status = sxml_exi_read_channel(decoding, channel_index);
            }
        }
        for (uint32_t channel_index = 0; channel_index < decoding->channel_count && status == SXML_STATUS_OK; channel_index++) {
            if (decoding->channels[channel_index].value_count > 100) {
                status = sxml_exi_open_stream(decoding);
                if (status == SXML_STATUS_OK) {
                    status = sxml_exi_read_channel(decoding, channel_index);
                }
            }
        }
    }
    for (size_t event_index = 0; event_index < decoding->emitted_count && status == SXML_STATUS_OK; event_index++) {
        status = sxml_exi_deliver(decoding, &decoding->emitted[event_index]);
    }
    /* A start tag still held spans the boundary: its values are read now,
       and its slots are reused by the next block. */
    for (size_t attribute_index = 0; attribute_index < decoding->attribute_count; attribute_index++) {
        sxml_exi_value_handle *value = &decoding->attributes[attribute_index].value;
        if (value->is_slot) {
            *value = (sxml_exi_value_handle){.is_slot = false, .text = decoding->slots[value->slot].text};
        }
    }
    decoding->emitted_count = 0;
    decoding->slot_count = 0;
    decoding->channel_count = 0;
    decoding->block_value_count = 0;
    decoding->block_number++;
    if (status == SXML_STATUS_OK && !decoding->is_finished) {
        status = sxml_exi_open_stream(decoding);
    }
    return status;
}

/* ---------------------------------------------------------------- Body */

static void sxml_exi_release_decoding(sxml_exi_decoding *decoding) {
    sxml_exi_string_table_finalize(&decoding->table);
    sxml_exi_grammars_finalize(&decoding->grammars);
    if (decoding->is_schema_informed) {
        sxml_exi_schema_grammars_finalize(&decoding->schema_grammars);
    }
    sxml_exi_buffer_release(&decoding->typed_text);
    sxml_allocator_release(decoding->allocator, decoding->text_bytes);
    sxml_allocator_release(decoding->allocator, decoding->frames);
    sxml_allocator_release(decoding->allocator, decoding->bindings);
    sxml_allocator_release(decoding->allocator, decoding->attributes);
    sxml_allocator_release(decoding->allocator, decoding->declarations);
    sxml_allocator_release(decoding->allocator, decoding->emitted);
    sxml_allocator_release(decoding->allocator, decoding->slots);
    sxml_allocator_release(decoding->allocator, decoding->channels);
    sxml_allocator_release(decoding->allocator, decoding->channel_slots);
    sxml_allocator_release(decoding->allocator, decoding->inflated);
}

static sxml_status sxml_exi_decode_body(sxml_exi_decoding *decoding) {
    sxml_status status = decoding->builder.operations->start_document(
        decoding->builder.state, &(sxml_document_start_event){.document_uri = decoding->request->document_uri,
                                                              .base_uri = decoding->request->base_uri});
    /* SD: the one production of Document and Fragment takes no bits. */
    decoding->top_nonterminal =
        decoding->options.is_fragment ? SXML_EXI_NONTERMINAL_FRAGMENT_CONTENT : SXML_EXI_NONTERMINAL_DOCUMENT_CONTENT;
    if (decoding->is_schema_informed) {
        decoding->top_schema_grammar = decoding->options.is_fragment ? decoding->schema_grammars.fragment_content
                                                                     : decoding->schema_grammars.document_content;
    }
    while (status == SXML_STATUS_OK && !decoding->is_finished) {
        status = sxml_exi_decode_event(decoding);
        if (status == SXML_STATUS_OK && decoding->is_channelized &&
            (decoding->is_finished || decoding->block_value_count == decoding->block_size)) {
            status = sxml_exi_end_block(decoding);
        }
    }
    if (status == SXML_STATUS_OK && decoding->frame_count != 0) {
        status = SXML_EXI_FAIL(decoding, SXML_STATUS_MALFORMED_INPUT, "the document ends inside an element");
    }
    if (status == SXML_STATUS_OK) {
        status = decoding->builder.operations->end_document(decoding->builder.state);
    }
    return status;
}

sxml_status sxml_exi_decode_stream(sxml_exi_decoder *decoder, const sxml_exi_decode_request *request,
                                   sxml_tree_builder builder) {
    static const sxml_exi_options default_options = {0};
    sxml_exi_decoding decoding = {
        .decoder = decoder,
        .request = request,
        .builder = builder,
        .allocator = decoder->configuration.allocator,
        .name_pool = decoder->configuration.name_pool,
        .limits = decoder->configuration.limits,
        .options = request->options != NULL ? *request->options : default_options,
        .reader = {.input = request->input},
    };
    sxml_status status = sxml_exi_read_header(&decoding);
    if (status == SXML_STATUS_OK && decoding.has_header_options) {
        status = SXML_EXI_FAIL(&decoding, SXML_STATUS_UNSUPPORTED_FEATURE,
                               "an options document in the header is not read yet (packet 8.10)");
    }
    const sxml_exi_options *options = &decoding.options;
    decoding.is_channelized =
        options->alignment == SXML_EXI_ALIGNMENT_PRE_COMPRESSION || options->alignment == SXML_EXI_ALIGNMENT_COMPRESSION;
    decoding.is_compressed = options->alignment == SXML_EXI_ALIGNMENT_COMPRESSION;
    decoding.block_size = options->has_block_size ? options->block_size : SXML_EXI_DEFAULT_BLOCK_SIZE;
    if (status == SXML_STATUS_OK && decoding.block_size == 0) {
        status = SXML_EXI_FAIL(&decoding, SXML_STATUS_MALFORMED_INPUT, "a blockSize of 0");
    }
    /* 8.5: a schema known out of band, one the schemaId names, or the
       built-in types only. */
    const sxml_schema *schema = request->schema;
    const sxml_exi_schema_identifier_kind identifier_kind = options->schema_identifier.schema_identifier_kind;
    if (status == SXML_STATUS_OK && schema == NULL && identifier_kind == SXML_EXI_SCHEMA_IDENTIFIER_TEXT) {
        if (request->schema_resolver.operations != NULL && request->schema_resolver.operations->resolve != NULL) {
            status = request->schema_resolver.operations->resolve(request->schema_resolver.state,
                                                                  &options->schema_identifier, &schema);
        }
        if (status == SXML_STATUS_OK && schema == NULL) {
            status = SXML_EXI_FAIL(&decoding, SXML_STATUS_UNSUPPORTED_FEATURE, "a schemaId no schema is found for");
        }
    }
    decoding.is_schema_informed = identifier_kind != SXML_EXI_SCHEMA_IDENTIFIER_NONE &&
                                  (schema != NULL || identifier_kind == SXML_EXI_SCHEMA_IDENTIFIER_BUILT_IN_TYPES);
    if (identifier_kind == SXML_EXI_SCHEMA_IDENTIFIER_BUILT_IN_TYPES) {
        schema = NULL;
    }
    decoding.typed_text = (sxml_exi_buffer){.allocator = decoding.allocator};
    if (status == SXML_STATUS_OK && (options->alignment == SXML_EXI_ALIGNMENT_BYTE_ALIGNMENT || decoding.is_channelized)) {
        /* 5: the header is padded to a byte; 9: compressed values are
           byte-aligned. */
        sxml_exi_reader_align(&decoding.reader);
        decoding.reader.is_byte_aligned = true;
    }
    if (status == SXML_STATUS_OK && decoding.is_compressed) {
        decoding.raw_input = request->input;
        decoding.raw_position = decoding.reader.byte_position;
        status = sxml_exi_open_stream(&decoding);
    }
    if (status == SXML_STATUS_OK && decoding.is_schema_informed) {
        status = sxml_exi_schema_grammars_build(
            &(sxml_exi_schema_build){.allocator = decoding.allocator, .options = options, .schema = schema},
            &(sxml_exi_schema_setup){.table = &decoding.table,
                                     .grammars = &decoding.schema_grammars,
                                     .failure_description = &decoding.failure_description});
        if (status != SXML_STATUS_OK) {
            decoding.is_schema_informed = false;
            decoding.table = (sxml_exi_string_table){.allocator = decoding.allocator};
        }
    } else if (status == SXML_STATUS_OK) {
        status = sxml_exi_string_table_initialize(
            &decoding.table, &(sxml_exi_string_table_configuration){.allocator = decoding.allocator, .options = options});
    }
    if (status == SXML_STATUS_OK) {
        sxml_exi_grammars_initialize(
            &decoding.grammars,
            &(sxml_exi_grammars_configuration){.allocator = decoding.allocator,
                                               .options = options,
                                               .maximum_learned_production_count =
                                                   decoding.limits.maximum_learned_production_count});
        status = sxml_exi_decode_body(&decoding);
    }
    if (request->diagnostic != NULL && status != SXML_STATUS_OK) {
        request->diagnostic->description = decoding.failure_description;
        request->diagnostic->location = (sxml_source_location){.byte_offset = decoding.reader.byte_position};
    }
    sxml_exi_release_decoding(&decoding);
    return status;
}
