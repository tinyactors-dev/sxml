/*
 * The EXI encoder (include/sxml/exi.h; EXI 1.0, sections 5 to 8; D333): a
 * tree to an EXI stream. Golden: src/serialize.c's walk of a tree. The tree
 * is walked with an explicit stack of open elements (D71); events go
 * through the grammar machine and string table the decoder uses
 * (src/exi_grammar.c, src/exi_string_table.c).
 */
#include <stdalign.h>
#include <string.h>

#include "deflate_internal.h"
#include "exi_internal.h"
#include "memory_internal.h"
#include "sxml/serialize.h"
#include "sxml/tree.h"

#define SXML_EXI_ENCODER_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* An element being written: its node, qname, grammar and position, and
   the next child to write. */
typedef struct sxml_exi_open_element {
    uint32_t node_index;
    uint32_t uri;
    uint32_t local_name;
    uint32_t element_grammar;
    sxml_exi_nonterminal nonterminal;
    bool has_next_child;
    uint32_t next_child;
} sxml_exi_open_element;

/* A value of the block, recorded for its channel (9.2.2). */
typedef struct sxml_exi_recorded_value {
    sxml_byte_view text;
    uint32_t channel;
} sxml_exi_recorded_value;

/* A value channel of the block: its qname and its number of values. */
typedef struct sxml_exi_block_channel {
    uint32_t uri;
    uint32_t local_name;
    uint32_t value_count;
} sxml_exi_block_channel;

typedef struct sxml_exi_encoding {
    const sxml_exi_encode_request *request;
    sxml_allocator allocator;
    sxml_tree *tree;
    sxml_name_pool *name_pool;
    sxml_exi_options options;
    sxml_exi_bit_writer writer;
    sxml_exi_string_table table;
    sxml_exi_grammars grammars;
    sxml_exi_open_element *elements;
    size_t element_count;
    size_t element_capacity;
    sxml_exi_nonterminal top_nonterminal;
    sxml_byte_view failure_description;
    /* Pre-compression and compression (EXI 9): output holds the header and
       the blocks written; writer the structure channel of the block being
       written; values its values, in event order. */
    bool is_channelized;
    bool is_compressed;
    uint64_t block_size;
    uint64_t block_number;
    sxml_exi_bit_writer output;
    sxml_exi_recorded_value *values;
    size_t value_count;
    size_t value_capacity;
    sxml_exi_block_channel *channels;
    size_t channel_count;
    size_t channel_capacity;
} sxml_exi_encoding;

/* A failure: its status and what to tell. */
typedef struct sxml_exi_encoder_failure {
    sxml_status status;
    sxml_byte_view description;
} sxml_exi_encoder_failure;

static sxml_status sxml_exi_encoder_record(sxml_exi_encoding *encoding, sxml_exi_encoder_failure failure) {
    if (encoding->failure_description.byte_count == 0) {
        encoding->failure_description = failure.description;
    }
    return failure.status;
}

#define SXML_EXI_ENCODER_FAIL(state, failure_status, literal) \
    sxml_exi_encoder_record((state), (sxml_exi_encoder_failure){.status = (failure_status),        \
                                                                .description = SXML_EXI_ENCODER_TEXT(literal)})

/* ---------------------------------------------------------- Positions */

static sxml_exi_open_element *sxml_exi_innermost(sxml_exi_encoding *encoding) {
    return encoding->element_count == 0 ? NULL : &encoding->elements[encoding->element_count - 1];
}

static sxml_exi_position sxml_exi_encoder_position(sxml_exi_encoding *encoding) {
    const sxml_exi_open_element *element = sxml_exi_innermost(encoding);
    if (element == NULL) {
        return (sxml_exi_position){.nonterminal = encoding->top_nonterminal, .element_grammar = SXML_EXI_NONE};
    }
    return (sxml_exi_position){.nonterminal = element->nonterminal, .element_grammar = element->element_grammar};
}

static void sxml_exi_encoder_move_to(sxml_exi_encoding *encoding, sxml_exi_nonterminal nonterminal) {
    sxml_exi_open_element *element = sxml_exi_innermost(encoding);
    if (element != NULL) {
        element->nonterminal = nonterminal;
    } else {
        encoding->top_nonterminal = nonterminal;
    }
}

/* What to find in a position: a terminal, with the qname of SE (qname) and
   AT (qname). */
typedef struct sxml_exi_wanted {
    sxml_exi_event_kind event_kind;
    uint32_t uri;
    uint32_t local_name;
} sxml_exi_wanted;

/* The learned production of position that matches wanted (for SE and AT
   by qname, for EE and CH by kind); is_found false without one. */
static sxml_exi_lookup sxml_exi_find_learned(sxml_exi_encoding *encoding, sxml_exi_wanted wanted) {
    const sxml_exi_learned *learned = sxml_exi_grammars_learned(&encoding->grammars, sxml_exi_encoder_position(encoding));
    if (learned == NULL) {
        return (sxml_exi_lookup){.is_found = false, .index = 0};
    }
    const bool has_qname =
        wanted.event_kind == SXML_EXI_EVENT_START_ELEMENT || wanted.event_kind == SXML_EXI_EVENT_ATTRIBUTE;
    for (size_t production_index = 0; production_index < learned->production_count; production_index++) {
        const sxml_exi_production *production = &learned->productions[production_index];
        if (production->event_kind == wanted.event_kind &&
            (!has_qname || (production->uri == wanted.uri && production->local_name == wanted.local_name))) {
            return (sxml_exi_lookup){.is_found = true, .index = (uint32_t)production_index};
        }
    }
    return (sxml_exi_lookup){.is_found = false, .index = 0};
}

/* The tail production of event_kind in the current position. */
static sxml_exi_lookup sxml_exi_find_tail(sxml_exi_encoding *encoding, sxml_exi_event_kind event_kind) {
    const sxml_exi_tail *tail = &encoding->grammars.tails[sxml_exi_encoder_position(encoding).nonterminal];
    for (size_t production_index = 0; production_index < tail->production_count; production_index++) {
        if (tail->productions[production_index].event_kind == event_kind) {
            return (sxml_exi_lookup){.is_found = true, .index = (uint32_t)production_index};
        }
    }
    return (sxml_exi_lookup){.is_found = false, .index = 0};
}

static sxml_status sxml_exi_write_match(sxml_exi_encoding *encoding, sxml_exi_match match) {
    return sxml_exi_write_event_code(
        &encoding->grammars,
        &(sxml_exi_event_code_request){.position = sxml_exi_encoder_position(encoding), .match = match},
        &encoding->writer);
}

/* Writes the tail production of event_kind; refuses when the grammar has
   none (the options pruned it). */
static sxml_status sxml_exi_write_tail(sxml_exi_encoding *encoding, sxml_exi_event_kind event_kind) {
    const sxml_exi_lookup found = sxml_exi_find_tail(encoding, event_kind);
    if (!found.is_found) {
        return SXML_EXI_ENCODER_FAIL(encoding, SXML_STATUS_DYNAMIC_ERROR, "content the grammar has no production for");
    }
    return sxml_exi_write_match(encoding, (sxml_exi_match){.is_learned = false, .index = found.index});
}

/* ------------------------------------------------------------ Strings */

/* A String (7.1.10) whose length is offset by length_offset (0 for a
   String, 1 for a local-name literal, 2 for a value literal). */
typedef struct sxml_exi_string_write {
    sxml_byte_view text;
    uint64_t length_offset;
} sxml_exi_string_write;

static sxml_status sxml_exi_write_string(sxml_exi_encoding *encoding, sxml_exi_string_write string) {
    const uint64_t codepoint_count = sxml_exi_utf8_codepoint_count(string.text);
    sxml_status status = sxml_exi_write_unsigned(&encoding->writer, codepoint_count + string.length_offset);
    size_t offset = 0;
    while (status == SXML_STATUS_OK && offset < string.text.byte_count) {
        status = sxml_exi_write_unsigned(&encoding->writer, sxml_exi_utf8_next(string.text, &offset));
    }
    return status;
}

/* A URI (compact-identifier partition, 7.3.2); its index to *uri. */
static sxml_status sxml_exi_encode_uri(sxml_exi_encoding *encoding, sxml_byte_view text, uint32_t *uri) {
    sxml_exi_lookup found = {0};
    sxml_status status = sxml_exi_find_uri(&encoding->table, &(sxml_exi_string_key){.text = text}, &found);
    const uint32_t width = sxml_exi_bit_width(encoding->table.uri_count + 1);
    if (status == SXML_STATUS_OK && found.is_found) {
        *uri = found.index;
        return sxml_exi_write_n_bit(&encoding->writer, (sxml_exi_bits){.value = found.index + 1, .bit_count = width});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_n_bit(&encoding->writer, (sxml_exi_bits){.value = 0, .bit_count = width});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_string(encoding, (sxml_exi_string_write){.text = text, .length_offset = 0});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_add_uri(&encoding->table, &(sxml_exi_string_key){.text = text}, uri);
    }
    return status;
}

/* A local-name (literal partition, 7.3.3) of uri. */
static sxml_status sxml_exi_encode_local_name(sxml_exi_encoding *encoding, const sxml_exi_string_key *key,
                                              uint32_t *local_name) {
    sxml_exi_lookup found = {0};
    sxml_status status = sxml_exi_find_local_name(&encoding->table, key, &found);
    if (status == SXML_STATUS_OK && found.is_found) {
        *local_name = found.index;
        status = sxml_exi_write_unsigned(&encoding->writer, 0);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_n_bit(
                &encoding->writer,
                (sxml_exi_bits){.value = found.index,
                                .bit_count = sxml_exi_bit_width(encoding->table.uris[key->uri].local_name_count)});
        }
        return status;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_string(encoding, (sxml_exi_string_write){.text = key->text, .length_offset = 1});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_add_local_name(&encoding->table, key, local_name);
    }
    return status;
}

/* A value (7.3.3) of the qname in key, written now to encoding->writer. */
static sxml_status sxml_exi_write_value(sxml_exi_encoding *encoding, const sxml_exi_string_key *key) {
    sxml_exi_lookup local = {0};
    sxml_status status = sxml_exi_find_local_value(&encoding->table, key, &local);
    if (status == SXML_STATUS_OK && local.is_found) {
        status = sxml_exi_write_unsigned(&encoding->writer, 0);
        if (status == SXML_STATUS_OK) {
            const sxml_exi_local_name *owner = &encoding->table.uris[key->uri].local_names[key->local_name];
            status = sxml_exi_write_n_bit(
                &encoding->writer,
                (sxml_exi_bits){.value = local.index, .bit_count = sxml_exi_bit_width(owner->local_value_count)});
        }
        return status;
    }
    sxml_exi_lookup global = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_find_global_value(&encoding->table, key, &global);
    }
    if (status == SXML_STATUS_OK && global.is_found) {
        status = sxml_exi_write_unsigned(&encoding->writer, 1);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_n_bit(
                &encoding->writer,
                (sxml_exi_bits){.value = global.index, .bit_count = sxml_exi_bit_width(encoding->table.global_value_count)});
        }
        return status;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_string(encoding, (sxml_exi_string_write){.text = key->text, .length_offset = 2});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_add_value(&encoding->table, key);
    }
    return status;
}

/* ------------------------------------------------------------ Blocks */

/* Writes one compressed stream (9.3) to the output: its bytes, deflated
   with compression; nothing for an empty one. */
static sxml_status sxml_exi_write_stream(sxml_exi_encoding *encoding, sxml_byte_view bytes) {
    if (bytes.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    if (!encoding->is_compressed) {
        sxml_status status = SXML_STATUS_OK;
        for (size_t byte_index = 0; byte_index < bytes.byte_count && status == SXML_STATUS_OK; byte_index++) {
            status = sxml_exi_write_bits(&encoding->output, (sxml_exi_bits){.value = bytes.bytes[byte_index], .bit_count = 8});
        }
        return status;
    }
    sxml_memory_view deflated = {0};
    sxml_status status = sxml_deflate(&(sxml_deflate_request){.input = bytes, .allocator = encoding->allocator}, &deflated);
    for (size_t byte_index = 0; byte_index < deflated.byte_count && status == SXML_STATUS_OK; byte_index++) {
        status = sxml_exi_write_bits(&encoding->output, (sxml_exi_bits){.value = deflated.bytes[byte_index], .bit_count = 8});
    }
    sxml_allocator_release(encoding->allocator, deflated.bytes);
    return status;
}

/* Which channels a stream holds: those with at most 100 values (small),
   more (large), or every one. */
typedef enum sxml_exi_channel_selection {
    SXML_EXI_CHANNELS_ALL,
    SXML_EXI_CHANNELS_SMALL,
    SXML_EXI_CHANNELS_ONE,
} sxml_exi_channel_selection;

typedef struct sxml_exi_channel_write {
    sxml_exi_channel_selection selection;
    uint32_t channel;
} sxml_exi_channel_write;

/* Writes the values of the selected channels into encoding->writer, in
   channel order, value order within each: the order a decoder reads, so
   the string table changes alike (9.3). */
static sxml_status sxml_exi_write_channels(sxml_exi_encoding *encoding, sxml_exi_channel_write write) {
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t channel_index = 0; channel_index < encoding->channel_count && status == SXML_STATUS_OK; channel_index++) {
        const sxml_exi_block_channel *values = &encoding->channels[channel_index];
        const bool is_selected = write.selection == SXML_EXI_CHANNELS_ALL ||
                                 (write.selection == SXML_EXI_CHANNELS_SMALL && values->value_count <= 100) ||
                                 (write.selection == SXML_EXI_CHANNELS_ONE && channel_index == write.channel);
        for (size_t value_index = 0; is_selected && value_index < encoding->value_count && status == SXML_STATUS_OK;
             value_index++) {
            if (encoding->values[value_index].channel == channel_index) {
                status = sxml_exi_write_value(encoding, &(sxml_exi_string_key){.text = encoding->values[value_index].text,
                                                                                .uri = values->uri,
                                                                                .local_name = values->local_name});
            }
        }
    }
    return status;
}

/* Writes a stream of channels: into a fresh writer, then to the output. */
static sxml_status sxml_exi_write_channel_stream(sxml_exi_encoding *encoding, sxml_exi_channel_write write) {
    const sxml_exi_bit_writer structure = encoding->writer;
    encoding->writer = (sxml_exi_bit_writer){.allocator = encoding->allocator, .is_byte_aligned = true};
    sxml_status status = sxml_exi_write_channels(encoding, write);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_stream(encoding, sxml_exi_writer_bytes(&encoding->writer));
    }
    sxml_exi_writer_release(&encoding->writer);
    encoding->writer = structure;
    return status;
}

/* Ends a block (9): its structure channel and its value channels as the
   compressed streams of 9.3. */
static sxml_status sxml_exi_flush_block(sxml_exi_encoding *encoding) {
    sxml_status status = SXML_STATUS_OK;
    if (encoding->value_count <= 100) {
        /* One stream: the structure, then every channel. */
        status = sxml_exi_write_channels(encoding, (sxml_exi_channel_write){.selection = SXML_EXI_CHANNELS_ALL});
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_stream(encoding, sxml_exi_writer_bytes(&encoding->writer));
        }
    } else {
        status = sxml_exi_write_stream(encoding, sxml_exi_writer_bytes(&encoding->writer));
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_channel_stream(encoding, (sxml_exi_channel_write){.selection = SXML_EXI_CHANNELS_SMALL});
        }
        for (uint32_t channel_index = 0; channel_index < encoding->channel_count && status == SXML_STATUS_OK; channel_index++) {
            if (encoding->channels[channel_index].value_count > 100) {
                status = sxml_exi_write_channel_stream(
                    encoding, (sxml_exi_channel_write){.selection = SXML_EXI_CHANNELS_ONE, .channel = channel_index});
            }
        }
    }
    sxml_exi_writer_release(&encoding->writer);
    encoding->writer = (sxml_exi_bit_writer){.allocator = encoding->allocator, .is_byte_aligned = true};
    encoding->value_count = 0;
    encoding->channel_count = 0;
    encoding->block_number++;
    return status;
}

/* A value of an AT or CH event: written now, or recorded for its channel
   and the block ended when it holds blockSize values (9.1). */
static sxml_status sxml_exi_encode_value(sxml_exi_encoding *encoding, const sxml_exi_string_key *key) {
    if (!encoding->is_channelized) {
        return sxml_exi_write_value(encoding, key);
    }
    sxml_exi_local_name *owner = &encoding->table.uris[key->uri].local_names[key->local_name];
    if (owner->channel_block != encoding->block_number + 1 || owner->channel_index >= encoding->channel_count) {
        void *elements = encoding->channels;
        sxml_status status = sxml_allocator_reserve_array(
            encoding->allocator, &(sxml_array_reservation_request){.elements = &elements,
                                                                   .capacity = &encoding->channel_capacity,
                                                                   .element_count = encoding->channel_count,
                                                                   .required_count = encoding->channel_count + 1,
                                                                   .element_size = sizeof(sxml_exi_block_channel),
                                                                   .element_alignment = alignof(sxml_exi_block_channel),
                                                                   .minimum_capacity = 16});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        encoding->channels = elements;
        owner->channel_block = encoding->block_number + 1;
        owner->channel_index = (uint32_t)encoding->channel_count;
        encoding->channels[encoding->channel_count++] =
            (sxml_exi_block_channel){.uri = key->uri, .local_name = key->local_name, .value_count = 0};
    }
    void *elements = encoding->values;
    sxml_status status = sxml_allocator_reserve_array(
        encoding->allocator, &(sxml_array_reservation_request){.elements = &elements,
                                                               .capacity = &encoding->value_capacity,
                                                               .element_count = encoding->value_count,
                                                               .required_count = encoding->value_count + 1,
                                                               .element_size = sizeof(sxml_exi_recorded_value),
                                                               .element_alignment = alignof(sxml_exi_recorded_value),
                                                               .minimum_capacity = 64});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    encoding->values = elements;
    encoding->channels[owner->channel_index].value_count++;
    encoding->values[encoding->value_count++] =
        (sxml_exi_recorded_value){.text = key->text, .channel = owner->channel_index};
    if (encoding->value_count == encoding->block_size) {
        return sxml_exi_flush_block(encoding);
    }
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------- Names */

/* A node's name as text: namespace URI, local name and prefix. */
typedef struct sxml_exi_name_text {
    sxml_byte_view uri;
    sxml_byte_view local_name;
    sxml_byte_view prefix;
} sxml_exi_name_text;

static sxml_status sxml_exi_name_text_of(sxml_exi_encoding *encoding, const sxml_node_description *description,
                                         sxml_exi_name_text *name) {
    sxml_name_parts parts = {0};
    sxml_status status = sxml_name_pool_get_name_parts(encoding->name_pool, description->node_name, &parts);
    *name = (sxml_exi_name_text){.local_name = parts.local_name};
    if (status == SXML_STATUS_OK && parts.namespace_uri.index != 0) {
        status = sxml_name_pool_get_namespace_uri(encoding->name_pool, parts.namespace_uri, &name->uri);
    }
    if (status == SXML_STATUS_OK && description->namespace_prefix.index != 0) {
        status = sxml_name_pool_get_namespace_prefix(encoding->name_pool, description->namespace_prefix, &name->prefix);
    }
    return status;
}

/* The qname of a name, in the string table, writing its URI and local-name
   unless known_qname has them (a learned production). */
typedef struct sxml_exi_qname_write {
    const sxml_exi_name_text *name;
    bool writes_uri_and_local_name;
} sxml_exi_qname_write;

typedef struct sxml_exi_table_qname {
    uint32_t uri;
    uint32_t local_name;
} sxml_exi_table_qname;

static sxml_status sxml_exi_write_qname(sxml_exi_encoding *encoding, const sxml_exi_qname_write *write,
                                        sxml_exi_table_qname *qname) {
    sxml_status status = SXML_STATUS_OK;
    if (write->writes_uri_and_local_name) {
        status = sxml_exi_encode_uri(encoding, write->name->uri, &qname->uri);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_encode_local_name(
                encoding, &(sxml_exi_string_key){.text = write->name->local_name, .uri = qname->uri}, &qname->local_name);
        }
    }
    if (status == SXML_STATUS_OK && encoding->options.fidelity.preserves_prefixes) {
        /* 7.1.7: the prefix's index in its URI's partition; a prefix not
           there yet is resolved by an NS event, and 0 stands for it. */
        sxml_exi_lookup prefix = {0};
        status = sxml_exi_find_prefix(&encoding->table,
                                      &(sxml_exi_string_key){.text = write->name->prefix, .uri = qname->uri}, &prefix);
        const size_t prefix_count = encoding->table.uris[qname->uri].prefix_count;
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_n_bit(
                &encoding->writer, (sxml_exi_bits){.value = prefix.is_found ? prefix.index : 0,
                                                   .bit_count = sxml_exi_bit_width(prefix_count == 0 ? 1 : prefix_count)});
        }
    }
    return status;
}

/* Finds a qname already in the table, without writing anything. */
static sxml_exi_lookup sxml_exi_known_qname(sxml_exi_encoding *encoding, const sxml_exi_name_text *name,
                                            sxml_exi_table_qname *qname) {
    sxml_exi_lookup uri = {0};
    sxml_exi_lookup local_name = {0};
    if (sxml_exi_find_uri(&encoding->table, &(sxml_exi_string_key){.text = name->uri}, &uri) != SXML_STATUS_OK ||
        !uri.is_found ||
        sxml_exi_find_local_name(&encoding->table, &(sxml_exi_string_key){.text = name->local_name, .uri = uri.index},
                                 &local_name) != SXML_STATUS_OK ||
        !local_name.is_found) {
        return (sxml_exi_lookup){.is_found = false, .index = 0};
    }
    *qname = (sxml_exi_table_qname){.uri = uri.index, .local_name = local_name.index};
    return (sxml_exi_lookup){.is_found = true, .index = 0};
}

/* ------------------------------------------------------------- Events */

/* An event of a node: the node and its description. */
typedef struct sxml_exi_node_event {
    sxml_node_reference node;
    const sxml_node_description *description;
} sxml_exi_node_event;

/* Writes an SE (qname) or SE (*), learns, and opens the element. */
static sxml_status sxml_exi_encode_start_element(sxml_exi_encoding *encoding, const sxml_exi_node_event *event) {
    sxml_exi_name_text name = {0};
    sxml_status status = sxml_exi_name_text_of(encoding, event->description, &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_exi_position position = sxml_exi_encoder_position(encoding);
    sxml_exi_table_qname qname = {0};
    sxml_exi_lookup learned = {.is_found = false, .index = 0};
    if (sxml_exi_known_qname(encoding, &name, &qname).is_found) {
        learned = sxml_exi_find_learned(encoding, (sxml_exi_wanted){.event_kind = SXML_EXI_EVENT_START_ELEMENT,
                                                                    .uri = qname.uri,
                                                                    .local_name = qname.local_name});
    }
    if (learned.is_found) {
        status = sxml_exi_write_match(encoding, (sxml_exi_match){.is_learned = true, .index = learned.index});
    } else {
        status = sxml_exi_write_tail(encoding, SXML_EXI_EVENT_START_ELEMENT_ANY);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_qname(
            encoding, &(sxml_exi_qname_write){.name = &name, .writes_uri_and_local_name = !learned.is_found}, &qname);
    }
    if (status == SXML_STATUS_OK && !learned.is_found && position.nonterminal != SXML_EXI_NONTERMINAL_DOCUMENT_CONTENT) {
        status = sxml_exi_grammars_learn(
            &encoding->grammars,
            &(sxml_exi_learning){.position = position,
                                 .production = {.event_kind = SXML_EXI_EVENT_START_ELEMENT,
                                                .uri = qname.uri,
                                                .local_name = qname.local_name}});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    switch (position.nonterminal) {
    case SXML_EXI_NONTERMINAL_DOCUMENT_CONTENT:
        sxml_exi_encoder_move_to(encoding, SXML_EXI_NONTERMINAL_DOCUMENT_END);
        break;
    case SXML_EXI_NONTERMINAL_START_TAG_CONTENT:
        sxml_exi_encoder_move_to(encoding, SXML_EXI_NONTERMINAL_ELEMENT_CONTENT);
        break;
    case SXML_EXI_NONTERMINAL_FRAGMENT_CONTENT:
    case SXML_EXI_NONTERMINAL_ELEMENT_CONTENT:
    case SXML_EXI_NONTERMINAL_DOCUMENT_END:
    case SXML_EXI_NONTERMINAL_COUNT:
        break;
    }
    sxml_exi_local_name *entry = &encoding->table.uris[qname.uri].local_names[qname.local_name];
    if (entry->element_grammar == SXML_EXI_NONE) {
        uint32_t element_grammar = 0;
        status = sxml_exi_grammars_add_element(&encoding->grammars, &element_grammar);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        entry = &encoding->table.uris[qname.uri].local_names[qname.local_name];
        entry->element_grammar = element_grammar;
    }
    if (encoding->element_count == encoding->element_capacity) {
        void *elements = encoding->elements;
        status = sxml_allocator_reserve_array(
            encoding->allocator, &(sxml_array_reservation_request){.elements = &elements,
                                                                   .capacity = &encoding->element_capacity,
                                                                   .element_count = encoding->element_count,
                                                                   .required_count = encoding->element_count + 1,
                                                                   .element_size = sizeof(sxml_exi_open_element),
                                                                   .element_alignment = alignof(sxml_exi_open_element),
                                                                   .minimum_capacity = 16});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        encoding->elements = elements;
    }
    const sxml_node_description *description = event->description;
    encoding->elements[encoding->element_count++] = (sxml_exi_open_element){
        .node_index = event->node.node_index.index,
        .uri = qname.uri,
        .local_name = qname.local_name,
        .element_grammar = entry->element_grammar,
        .nonterminal = SXML_EXI_NONTERMINAL_START_TAG_CONTENT,
        .has_next_child = description->has_children,
        .next_child = description->first_child.index,
    };
    return SXML_STATUS_OK;
}

/* The prefix's text of a binding. */
static sxml_status sxml_exi_binding_text(sxml_exi_encoding *encoding, const sxml_namespace_binding *binding,
                                         sxml_exi_name_text *text) {
    *text = (sxml_exi_name_text){0};
    sxml_status status = SXML_STATUS_OK;
    if (binding->namespace_prefix.index != 0) {
        status = sxml_name_pool_get_namespace_prefix(encoding->name_pool, binding->namespace_prefix, &text->prefix);
    }
    if (status == SXML_STATUS_OK && binding->namespace_uri.index != 0) {
        status = sxml_name_pool_get_namespace_uri(encoding->name_pool, binding->namespace_uri, &text->uri);
    }
    return status;
}

/* Whether bindings has prefix bound to uri. */
typedef struct sxml_exi_binding_query {
    sxml_namespace_binding_view bindings;
    sxml_namespace_binding binding;
} sxml_exi_binding_query;

static bool sxml_exi_has_binding(const sxml_exi_binding_query *query) {
    for (size_t binding_index = 0; binding_index < query->bindings.namespace_binding_count; binding_index++) {
        const sxml_namespace_binding *candidate = &query->bindings.namespace_bindings[binding_index];
        if (candidate->namespace_prefix.index == query->binding.namespace_prefix.index &&
            candidate->namespace_uri.index == query->binding.namespace_uri.index) {
            return true;
        }
    }
    return false;
}

/* One NS event (4, 7.1.7): uri, prefix, local-element-ns. */
typedef struct sxml_exi_namespace_write {
    sxml_exi_name_text binding;
    bool is_local_element_namespace;
} sxml_exi_namespace_write;

static sxml_status sxml_exi_encode_namespace(sxml_exi_encoding *encoding, const sxml_exi_namespace_write *write) {
    sxml_status status = sxml_exi_write_tail(encoding, SXML_EXI_EVENT_NAMESPACE);
    uint32_t uri = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_encode_uri(encoding, write->binding.uri, &uri);
    }
    sxml_exi_lookup prefix = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_find_prefix(&encoding->table,
                                      &(sxml_exi_string_key){.text = write->binding.prefix, .uri = uri}, &prefix);
    }
    if (status == SXML_STATUS_OK) {
        const uint32_t width = sxml_exi_bit_width(encoding->table.uris[uri].prefix_count + 1);
        status = sxml_exi_write_n_bit(&encoding->writer,
                                      (sxml_exi_bits){.value = prefix.is_found ? prefix.index + 1 : 0, .bit_count = width});
        if (status == SXML_STATUS_OK && !prefix.is_found) {
            status = sxml_exi_write_string(encoding, (sxml_exi_string_write){.text = write->binding.prefix});
            uint32_t added = 0;
            if (status == SXML_STATUS_OK) {
                status = sxml_exi_add_prefix(&encoding->table,
                                             &(sxml_exi_string_key){.text = write->binding.prefix, .uri = uri}, &added);
            }
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_boolean(&encoding->writer, (sxml_exi_boolean){.value = write->is_local_element_namespace});
    }
    return status;
}

static bool sxml_exi_texts_equal(sxml_byte_view first, sxml_byte_view second) {
    return first.byte_count == second.byte_count &&
           (first.byte_count == 0 || memcmp(first.bytes, second.bytes, first.byte_count) == 0);
}

/* The NS events of an element (Preserve.prefixes): its in-scope
   namespaces its parent does not have, and the default namespace's
   undeclaration when the parent has one and it has none. */
static sxml_status sxml_exi_encode_namespaces(sxml_exi_encoding *encoding, const sxml_exi_node_event *event) {
    sxml_namespace_binding_view own = {0};
    sxml_namespace_binding_view inherited = {0};
    sxml_status status = sxml_node_get_in_scope_namespaces(event->node, &own);
    const sxml_node_description *description = event->description;
    sxml_node_description parent_description = {0};
    bool has_element_parent = false;
    if (status == SXML_STATUS_OK && description->has_parent) {
        const sxml_node_reference parent = {.tree = event->node.tree, .node_index = description->parent};
        status = sxml_node_describe(parent, &parent_description);
        if (status == SXML_STATUS_OK && parent_description.node_kind == SXML_NODE_KIND_ELEMENT) {
            has_element_parent = true;
            status = sxml_node_get_in_scope_namespaces(parent, &inherited);
        }
    }
    sxml_exi_name_text element_name = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_name_text_of(encoding, description, &element_name);
    }
    bool has_local_element_namespace = false;
    for (size_t binding_index = 0; binding_index < own.namespace_binding_count && status == SXML_STATUS_OK;
         binding_index++) {
        const sxml_namespace_binding *binding = &own.namespace_bindings[binding_index];
        if (binding->namespace_uri.index == SXML_NAMESPACE_XML) {
            continue;
        }
        if (has_element_parent &&
            sxml_exi_has_binding(&(sxml_exi_binding_query){.bindings = inherited, .binding = *binding})) {
            continue;
        }
        sxml_exi_name_text text = {0};
        status = sxml_exi_binding_text(encoding, binding, &text);
        const bool is_local = !has_local_element_namespace && sxml_exi_texts_equal(text.prefix, element_name.prefix) &&
                              sxml_exi_texts_equal(text.uri, element_name.uri);
        has_local_element_namespace = has_local_element_namespace || is_local;
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_encode_namespace(
                encoding, &(sxml_exi_namespace_write){.binding = text, .is_local_element_namespace = is_local});
        }
    }
    /* An undeclared default namespace: the parent has one, this has none. */
    if (status == SXML_STATUS_OK && has_element_parent) {
        bool parent_has_default = false;
        bool has_default = false;
        for (size_t binding_index = 0; binding_index < inherited.namespace_binding_count; binding_index++) {
            parent_has_default = parent_has_default || inherited.namespace_bindings[binding_index].namespace_prefix.index == 0;
        }
        for (size_t binding_index = 0; binding_index < own.namespace_binding_count; binding_index++) {
            has_default = has_default || own.namespace_bindings[binding_index].namespace_prefix.index == 0;
        }
        if (parent_has_default && !has_default) {
            status = sxml_exi_encode_namespace(
                encoding,
                &(sxml_exi_namespace_write){.binding = {0},
                                            .is_local_element_namespace = !has_local_element_namespace &&
                                                                          element_name.uri.byte_count == 0 &&
                                                                          element_name.prefix.byte_count == 0});
        }
    }
    return status;
}

/* xsi:type's value as a QName (7.1.7): its prefix resolved against the
   element's in-scope namespaces (8.4.3: an unbound prefix makes the whole
   lexical value the local-name in no namespace). */
static sxml_status sxml_exi_encode_type_name(sxml_exi_encoding *encoding, const sxml_exi_node_event *event) {
    sxml_byte_view value = {0};
    sxml_status status = sxml_node_get_content(event->node, &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_exi_name_text name = {.local_name = value};
    size_t colon = 0;
    while (colon < value.byte_count && value.bytes[colon] != ':') {
        colon++;
    }
    const sxml_byte_view prefix = {.bytes = value.bytes, .byte_count = colon < value.byte_count ? colon : 0};
    if (colon < value.byte_count) {
        name.local_name = (sxml_byte_view){.bytes = value.bytes + colon + 1, .byte_count = value.byte_count - colon - 1};
    }
    sxml_namespace_binding_view bindings = {0};
    const sxml_node_reference element = {.tree = event->node.tree, .node_index = event->description->parent};
    status = sxml_node_get_in_scope_namespaces(element, &bindings);
    bool is_bound = false;
    for (size_t binding_index = 0; binding_index < bindings.namespace_binding_count && status == SXML_STATUS_OK;
         binding_index++) {
        sxml_exi_name_text text = {0};
        status = sxml_exi_binding_text(encoding, &bindings.namespace_bindings[binding_index], &text);
        if (status == SXML_STATUS_OK && sxml_exi_texts_equal(text.prefix, prefix)) {
            name.uri = text.uri;
            name.prefix = prefix;
            is_bound = true;
        }
    }
    if (status == SXML_STATUS_OK && !is_bound && prefix.byte_count > 0) {
        name = (sxml_exi_name_text){.local_name = value};
    }
    sxml_exi_table_qname qname = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_qname(encoding, &(sxml_exi_qname_write){.name = &name, .writes_uri_and_local_name = true},
                                      &qname);
    }
    return status;
}

static sxml_status sxml_exi_encode_attribute(sxml_exi_encoding *encoding, const sxml_exi_node_event *event) {
    sxml_exi_name_text name = {0};
    sxml_status status = sxml_exi_name_text_of(encoding, event->description, &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_exi_position position = sxml_exi_encoder_position(encoding);
    sxml_exi_table_qname qname = {0};
    sxml_exi_lookup learned = {.is_found = false, .index = 0};
    if (sxml_exi_known_qname(encoding, &name, &qname).is_found) {
        learned = sxml_exi_find_learned(
            encoding, (sxml_exi_wanted){.event_kind = SXML_EXI_EVENT_ATTRIBUTE, .uri = qname.uri, .local_name = qname.local_name});
    }
    if (learned.is_found) {
        status = sxml_exi_write_match(encoding, (sxml_exi_match){.is_learned = true, .index = learned.index});
    } else {
        status = sxml_exi_write_tail(encoding, SXML_EXI_EVENT_ATTRIBUTE_ANY);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_qname(
            encoding, &(sxml_exi_qname_write){.name = &name, .writes_uri_and_local_name = !learned.is_found}, &qname);
    }
    const bool is_type = qname.uri == 2 && qname.local_name == 1;
    if (status == SXML_STATUS_OK && !learned.is_found) {
        /* 8.4.3: AT (*) teaches AT (qname); xsi:type once only, and a
           learned xsi:type would have matched above. */
        status = sxml_exi_grammars_learn(
            &encoding->grammars,
            &(sxml_exi_learning){.position = position,
                                 .production = {.event_kind = SXML_EXI_EVENT_ATTRIBUTE,
                                                .uri = qname.uri,
                                                .local_name = qname.local_name}});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (is_type && !encoding->options.fidelity.preserves_lexical_values) {
        return sxml_exi_encode_type_name(encoding, event);
    }
    sxml_byte_view value = {0};
    status = sxml_node_get_content(event->node, &value);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_encode_value(
            encoding, &(sxml_exi_string_key){.text = value, .uri = qname.uri, .local_name = qname.local_name});
    }
    return status;
}

/* The attributes of an element, xsi:type first, then xsi:nil (6). */
static sxml_status sxml_exi_encode_attributes(sxml_exi_encoding *encoding, const sxml_exi_node_event *event) {
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t attribute_order = 0; attribute_order < 3 && status == SXML_STATUS_OK; attribute_order++) {
        for (uint32_t attribute_index = 0; attribute_index < event->description->attribute_count && status == SXML_STATUS_OK;
             attribute_index++) {
            const sxml_node_reference attribute = {
                .tree = event->node.tree, .node_index = {event->node.node_index.index + 1 + attribute_index}};
            sxml_node_description description = {0};
            status = sxml_node_describe(attribute, &description);
            sxml_name_parts parts = {0};
            if (status == SXML_STATUS_OK) {
                status = sxml_name_pool_get_name_parts(encoding->name_pool, description.node_name, &parts);
            }
            if (status != SXML_STATUS_OK) {
                break;
            }
            const bool is_instance = parts.namespace_uri.index == SXML_NAMESPACE_XML_SCHEMA_INSTANCE;
            const bool is_type = is_instance && sxml_exi_texts_equal(parts.local_name, SXML_EXI_ENCODER_TEXT("type"));
            const bool is_nil = is_instance && sxml_exi_texts_equal(parts.local_name, SXML_EXI_ENCODER_TEXT("nil"));
            const uint32_t attribute_pass = is_type ? 0U : is_nil ? 1U : 2U;
            if (attribute_pass == attribute_order) {
                status = sxml_exi_encode_attribute(encoding,
                                                   &(sxml_exi_node_event){.node = attribute, .description = &description});
            }
        }
    }
    return status;
}

/* Writes the event code of a CH, EE or (in StartTagContent) of what is
   learned, learning CH and EE as 8.4.3 says. */
static sxml_status sxml_exi_encode_learnable(sxml_exi_encoding *encoding, sxml_exi_event_kind event_kind) {
    const sxml_exi_position position = sxml_exi_encoder_position(encoding);
    const sxml_exi_lookup learned = sxml_exi_find_learned(encoding, (sxml_exi_wanted){.event_kind = event_kind});
    if (learned.is_found) {
        return sxml_exi_write_match(encoding, (sxml_exi_match){.is_learned = true, .index = learned.index});
    }
    sxml_status status = sxml_exi_write_tail(encoding, event_kind);
    /* EE is learned in StartTagContent only: ElementContent's EE has a
       one-part code already. */
    const bool learns = event_kind == SXML_EXI_EVENT_CHARACTERS ||
                        position.nonterminal == SXML_EXI_NONTERMINAL_START_TAG_CONTENT;
    if (status == SXML_STATUS_OK && learns && sxml_exi_grammars_learned(&encoding->grammars, position) != NULL) {
        status = sxml_exi_grammars_learn(&encoding->grammars,
                                         &(sxml_exi_learning){.position = position, .production = {.event_kind = event_kind}});
    }
    return status;
}

static sxml_status sxml_exi_encode_characters(sxml_exi_encoding *encoding, const sxml_exi_node_event *event) {
    sxml_exi_open_element *element = sxml_exi_innermost(encoding);
    if (element == NULL) {
        /* Text at the top of a fragment: no grammar has a CH there. */
        return SXML_EXI_ENCODER_FAIL(encoding, SXML_STATUS_DYNAMIC_ERROR, "text outside any element");
    }
    sxml_byte_view text = {0};
    sxml_status status = sxml_node_get_content(event->node, &text);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_encode_learnable(encoding, SXML_EXI_EVENT_CHARACTERS);
    }
    element = sxml_exi_innermost(encoding);
    const sxml_exi_string_key key = {.text = text, .uri = element->uri, .local_name = element->local_name};
    sxml_exi_encoder_move_to(encoding, SXML_EXI_NONTERMINAL_ELEMENT_CONTENT);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_encode_value(encoding, &key);
    }
    return status;
}

/* A comment or processing instruction, when the options keep it. */
static sxml_status sxml_exi_encode_other(sxml_exi_encoding *encoding, const sxml_exi_node_event *event) {
    const bool is_comment = event->description->node_kind == SXML_NODE_KIND_COMMENT;
    if (is_comment ? !encoding->options.fidelity.preserves_comments
                   : !encoding->options.fidelity.preserves_processing_instructions) {
        return SXML_STATUS_OK;
    }
    sxml_byte_view content = {0};
    sxml_status status = sxml_node_get_content(event->node, &content);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_tail(encoding, is_comment ? SXML_EXI_EVENT_COMMENT : SXML_EXI_EVENT_PROCESSING_INSTRUCTION);
    }
    if (status == SXML_STATUS_OK && sxml_exi_innermost(encoding) != NULL) {
        sxml_exi_encoder_move_to(encoding, SXML_EXI_NONTERMINAL_ELEMENT_CONTENT);
    }
    if (status == SXML_STATUS_OK && !is_comment) {
        sxml_name_parts target = {0};
        status = sxml_name_pool_get_name_parts(encoding->name_pool, event->description->node_name, &target);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_string(encoding, (sxml_exi_string_write){.text = target.local_name});
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_string(encoding, (sxml_exi_string_write){.text = content});
    }
    return status;
}

/* ------------------------------------------------------------- The walk */

/* Writes one child node of the innermost element (or of the top). */
static sxml_status sxml_exi_encode_node(sxml_exi_encoding *encoding, sxml_node_reference node) {
    sxml_node_description description = {0};
    sxml_status status = sxml_node_describe(node, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_exi_node_event event = {.node = node, .description = &description};
    switch (description.node_kind) {
    case SXML_NODE_KIND_ELEMENT:
        status = sxml_exi_encode_start_element(encoding, &event);
        if (status == SXML_STATUS_OK && encoding->options.fidelity.preserves_prefixes) {
            status = sxml_exi_encode_namespaces(encoding, &event);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_encode_attributes(encoding, &event);
        }
        return status;
    case SXML_NODE_KIND_TEXT:
        return sxml_exi_encode_characters(encoding, &event);
    case SXML_NODE_KIND_COMMENT:
    case SXML_NODE_KIND_PROCESSING_INSTRUCTION:
        return sxml_exi_encode_other(encoding, &event);
    case SXML_NODE_KIND_DOCUMENT:
    case SXML_NODE_KIND_ATTRIBUTE:
    case SXML_NODE_KIND_NAMESPACE:
    case SXML_NODE_KIND_ANY:
        break;
    }
    return SXML_EXI_ENCODER_FAIL(encoding, SXML_STATUS_DYNAMIC_ERROR, "a node EXI has no event for here");
}

/* Writes the children of the document (or the root element), then each
   element's content and EE, over the stack of open elements. */
static sxml_status sxml_exi_encode_body(sxml_exi_encoding *encoding, sxml_node_reference root) {
    sxml_node_description root_description = {0};
    sxml_status status = sxml_node_describe(root, &root_description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    encoding->top_nonterminal =
        encoding->options.is_fragment ? SXML_EXI_NONTERMINAL_FRAGMENT_CONTENT : SXML_EXI_NONTERMINAL_DOCUMENT_CONTENT;
    /* The top level's children: a document's, or the root element alone. */
    bool has_top_child = root_description.node_kind == SXML_NODE_KIND_DOCUMENT ? root_description.has_children : true;
    uint32_t top_child = root_description.node_kind == SXML_NODE_KIND_DOCUMENT ? root_description.first_child.index
                                                                                : root.node_index.index;
    const bool is_document_root = root_description.node_kind == SXML_NODE_KIND_DOCUMENT;
    while (status == SXML_STATUS_OK) {
        sxml_exi_open_element *element = sxml_exi_innermost(encoding);
        if (element == NULL) {
            if (!has_top_child) {
                break;
            }
            const sxml_node_reference child = {.tree = root.tree, .node_index = {top_child}};
            sxml_node_description child_description = {0};
            status = sxml_node_describe(child, &child_description);
            has_top_child = is_document_root && child_description.has_next_sibling;
            top_child = child_description.next_sibling.index;
            if (status == SXML_STATUS_OK) {
                status = sxml_exi_encode_node(encoding, child);
            }
            continue;
        }
        if (element->has_next_child) {
            const sxml_node_reference child = {.tree = root.tree, .node_index = {element->next_child}};
            sxml_node_description child_description = {0};
            status = sxml_node_describe(child, &child_description);
            element->has_next_child = child_description.has_next_sibling;
            element->next_child = child_description.next_sibling.index;
            if (status == SXML_STATUS_OK) {
                status = sxml_exi_encode_node(encoding, child);
            }
            continue;
        }
        status = sxml_exi_encode_learnable(encoding, SXML_EXI_EVENT_END_ELEMENT);
        encoding->element_count--;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_tail(encoding, SXML_EXI_EVENT_END_DOCUMENT);
    }
    return status;
}

/* EXI 5: [cookie] 1 0, presence bit, final version 1 (0 0000). */
static sxml_status sxml_exi_write_header(sxml_exi_encoding *encoding) {
    sxml_status status = SXML_STATUS_OK;
    if (encoding->request->header_options.includes_cookie) {
        for (size_t byte_index = 0; byte_index < 4 && status == SXML_STATUS_OK; byte_index++) {
            status = sxml_exi_write_bits(&encoding->writer,
                                         (sxml_exi_bits){.value = (uint8_t)"$EXI"[byte_index], .bit_count = 8});
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_bits(&encoding->writer, (sxml_exi_bits){.value = 0x2, .bit_count = 2});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_bits(&encoding->writer, (sxml_exi_bits){.value = 0, .bit_count = 1});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_bits(&encoding->writer, (sxml_exi_bits){.value = 0, .bit_count = 5});
    }
    return status;
}

sxml_status sxml_exi_encode_stream(sxml_exi_encoder *encoder, const sxml_exi_encode_request *request,
                                   sxml_output_stream output_stream) {
    static const sxml_exi_options default_options = {0};
    sxml_exi_encoding encoding = {
        .request = request,
        .allocator = encoder->configuration.allocator,
        .tree = request->root.tree,
        .options = request->options != NULL ? *request->options : default_options,
        .writer = {.allocator = encoder->configuration.allocator},
    };
    sxml_tree_description tree_description = {0};
    sxml_status status = request->root.tree == NULL ? SXML_STATUS_INVALID_ARGUMENT
                                                    : sxml_tree_describe(request->root.tree, &tree_description);
    encoding.name_pool = tree_description.name_pool;
    const sxml_exi_options *options = &encoding.options;
    if (status == SXML_STATUS_OK &&
        (request->header_options.includes_options || request->canonicalization.is_canonical)) {
        status = SXML_EXI_ENCODER_FAIL(&encoding, SXML_STATUS_UNSUPPORTED_FEATURE,
                                       "options documents and Canonical EXI are not written yet (packets 8.10, 8.11)");
    }
    encoding.is_channelized =
        options->alignment == SXML_EXI_ALIGNMENT_PRE_COMPRESSION || options->alignment == SXML_EXI_ALIGNMENT_COMPRESSION;
    encoding.is_compressed = options->alignment == SXML_EXI_ALIGNMENT_COMPRESSION;
    encoding.block_size = options->has_block_size ? options->block_size : SXML_EXI_DEFAULT_BLOCK_SIZE;
    if (status == SXML_STATUS_OK && encoding.block_size == 0) {
        status = SXML_EXI_ENCODER_FAIL(&encoding, SXML_STATUS_INVALID_ARGUMENT, "a blockSize of 0");
    }
    if (status == SXML_STATUS_OK &&
        (request->schema != NULL ||
         options->schema_identifier.schema_identifier_kind == SXML_EXI_SCHEMA_IDENTIFIER_BUILT_IN_TYPES ||
         options->schema_identifier.schema_identifier_kind == SXML_EXI_SCHEMA_IDENTIFIER_TEXT)) {
        status = SXML_EXI_ENCODER_FAIL(&encoding, SXML_STATUS_UNSUPPORTED_FEATURE,
                                       "schema-informed grammars are not written yet (packet 8.9)");
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_header(&encoding);
    }
    if (status == SXML_STATUS_OK && (options->alignment == SXML_EXI_ALIGNMENT_BYTE_ALIGNMENT || encoding.is_channelized)) {
        status = sxml_exi_writer_align(&encoding.writer);
        encoding.writer.is_byte_aligned = true;
    }
    if (status == SXML_STATUS_OK && encoding.is_channelized) {
        /* The header is the output's start; the writer is the first
           block's structure channel. */
        encoding.output = encoding.writer;
        encoding.writer = (sxml_exi_bit_writer){.allocator = encoding.allocator, .is_byte_aligned = true};
    }
    bool has_table = false;
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_string_table_initialize(
            &encoding.table, &(sxml_exi_string_table_configuration){.allocator = encoding.allocator, .options = options});
        has_table = status == SXML_STATUS_OK;
    }
    if (status == SXML_STATUS_OK) {
        sxml_exi_grammars_initialize(
            &encoding.grammars,
            &(sxml_exi_grammars_configuration){.allocator = encoding.allocator,
                                               .options = options,
                                               .maximum_learned_production_count = UINT32_MAX});
        status = sxml_exi_encode_body(&encoding, request->root);
        sxml_exi_grammars_finalize(&encoding.grammars);
    }
    if (status == SXML_STATUS_OK && encoding.is_channelized) {
        status = sxml_exi_flush_block(&encoding);
    }
    sxml_exi_bit_writer *final = encoding.is_channelized ? &encoding.output : &encoding.writer;
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_writer_align(final);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_output_stream_write(output_stream, sxml_exi_writer_bytes(final));
    }
    if (request->diagnostic != NULL && status != SXML_STATUS_OK) {
        request->diagnostic->description = encoding.failure_description;
    }
    if (has_table) {
        sxml_exi_string_table_finalize(&encoding.table);
    }
    sxml_allocator_release(encoding.allocator, encoding.elements);
    sxml_allocator_release(encoding.allocator, encoding.values);
    sxml_allocator_release(encoding.allocator, encoding.channels);
    sxml_exi_writer_release(&encoding.writer);
    sxml_exi_writer_release(&encoding.output);
    return status;
}
