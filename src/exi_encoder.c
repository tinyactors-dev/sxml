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
    /* The schema-informed grammar the element is in (8.5); SXML_EXI_NONE
       for a built-in element grammar (8.4.3). */
    uint32_t schema_grammar;
    uint32_t next_child;
    bool has_next_child;
    /* xml:space="preserve" is in scope; a child was written. */
    bool is_space_preserved;
    bool has_written_child;
} sxml_exi_open_element;

/* A value of the block, recorded for its channel (9.2.2): its text (in
   the tree, or at owned_offset in the encoder's own store) and its
   datatype (SXML_EXI_NONE for a String). */
typedef struct sxml_exi_recorded_value {
    sxml_byte_view text;
    size_t owned_offset;
    uint32_t channel;
    uint32_t datatype;
    bool is_owned;
} sxml_exi_recorded_value;

/* A value channel of the block: its qname and its number of values. */
typedef struct sxml_exi_block_channel {
    uint32_t uri;
    uint32_t local_name;
    uint32_t value_count;
} sxml_exi_block_channel;

/* What a self-contained element puts aside (8.4.3, SC): the string
   table, grammars and top level of the stream around it, and where its
   own elements start. */
typedef struct sxml_exi_saved_state {
    sxml_exi_string_table table;
    sxml_exi_grammars grammars;
    sxml_exi_nonterminal top_nonterminal;
    size_t element_base;
} sxml_exi_saved_state;

typedef struct sxml_exi_encoding {
    const sxml_exi_encode_request *request;
    sxml_allocator allocator;
    sxml_tree *tree;
    sxml_name_pool *name_pool;
    sxml_exi_options options;
    sxml_exi_bit_writer writer;
    sxml_exi_string_table table;
    sxml_exi_grammars grammars;
    /* Schema-informed (8.5): the schema's grammars, the top level's
       grammar among them, texts made for values (whitespace applied) and
       a scratch text. */
    sxml_exi_schema_grammars schema_grammars;
    sxml_exi_buffer owned_text;
    sxml_exi_buffer scratch_text;
    uint32_t top_schema_grammar;
    bool is_schema_informed;
    /* The text node being written is its element's first child, and the
       last or before a comment or processing instruction written. */
    bool is_first_child;
    bool is_before_end_or_markup;
    sxml_exi_open_element *elements;
    size_t element_count;
    size_t element_capacity;
    /* Inside self-contained elements: the elements below element_base
       are the enclosing streams'; saved_states, one per element. */
    size_t element_base;
    sxml_exi_saved_state *saved_states;
    size_t saved_state_count;
    size_t saved_state_capacity;
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
    return encoding->element_count <= encoding->element_base ? NULL : &encoding->elements[encoding->element_count - 1];
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

/* The characters of a String literal (7.1.10) after its length: each an
   Unsigned Integer, or with a restricted character set (7.1.10.1) an
   n-bit index into it, the index N escaping an Unsigned Integer. */
typedef struct sxml_exi_literal_write {
    sxml_byte_view text;
    sxml_exi_character_set character_set;
} sxml_exi_literal_write;

static sxml_status sxml_exi_write_literal_characters(sxml_exi_encoding *encoding, const sxml_exi_literal_write *write) {
    const sxml_exi_character_set set = write->character_set;
    const uint32_t width = set.character_count > 0 ? sxml_exi_bit_width(set.character_count + 1) : 0;
    size_t offset = 0;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && offset < write->text.byte_count) {
        const uint32_t codepoint = sxml_exi_utf8_next(write->text, &offset);
        if (set.character_count == 0) {
            status = sxml_exi_write_unsigned(&encoding->writer, codepoint);
            continue;
        }
        const uint32_t code = sxml_exi_character_index(set, codepoint);
        status = sxml_exi_write_n_bit(&encoding->writer, (sxml_exi_bits){.value = code, .bit_count = width});
        if (status == SXML_STATUS_OK && code == set.character_count) {
            status = sxml_exi_write_unsigned(&encoding->writer, codepoint);
        }
    }
    return status;
}

/* A value (7.3.3) of the qname in key, written now to encoding->writer; a
   literal's characters may use a restricted character set. */
typedef struct sxml_exi_string_value_write {
    const sxml_exi_string_key *key;
    sxml_exi_character_set character_set;
} sxml_exi_string_value_write;

static sxml_status sxml_exi_write_string_value(sxml_exi_encoding *encoding, const sxml_exi_string_value_write *write);

static sxml_status sxml_exi_write_string_value(sxml_exi_encoding *encoding, const sxml_exi_string_value_write *write) {
    const sxml_exi_string_key *key = write->key;
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
        status = sxml_exi_write_unsigned(&encoding->writer, sxml_exi_utf8_codepoint_count(key->text) + 2);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_literal_characters(
            encoding, &(sxml_exi_literal_write){.text = key->text, .character_set = write->character_set});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_add_value(&encoding->table, key);
    }
    return status;
}

/* ------------------------------------------------------- Typed values */

/* A value to write: its qname and text, and its datatype (SXML_EXI_NONE
   for a String, 7.1.10). */
typedef struct sxml_exi_value_write {
    sxml_exi_string_key key;
    uint32_t datatype;
} sxml_exi_value_write;

/* The restricted character set of a String of datatype: its own
   (7.1.10.1), or Table 7-2's with Preserve.lexicalValues. */
static sxml_exi_character_set sxml_exi_encoder_character_set(const sxml_exi_encoding *encoding, uint32_t datatype) {
    const sxml_exi_schema_grammars *grammars = &encoding->schema_grammars;
    if (datatype == SXML_EXI_NONE) {
        return (sxml_exi_character_set){.characters = NULL, .character_count = 0};
    }
    if (encoding->options.fidelity.preserves_lexical_values) {
        return sxml_exi_lexical_character_set(sxml_exi_datatype_lexical_set(grammars, datatype));
    }
    const sxml_exi_datatype *entry = &grammars->datatypes[datatype];
    if (entry->datatype_kind == SXML_EXI_DATATYPE_RESTRICTED_STRING) {
        return (sxml_exi_character_set){.characters = grammars->characters + entry->first_value,
                                        .character_count = entry->value_count};
    }
    return (sxml_exi_character_set){.characters = NULL, .character_count = 0};
}

/* Whether a value of datatype is a String in the stream. */
static bool sxml_exi_encoder_is_string(const sxml_exi_encoding *encoding, uint32_t datatype) {
    if (datatype == SXML_EXI_NONE || encoding->options.fidelity.preserves_lexical_values) {
        return true;
    }
    const sxml_exi_datatype_kind kind = encoding->schema_grammars.datatypes[datatype].datatype_kind;
    return kind == SXML_EXI_DATATYPE_STRING || kind == SXML_EXI_DATATYPE_RESTRICTED_STRING;
}

static bool sxml_exi_encoder_is_list(const sxml_exi_encoding *encoding, uint32_t datatype) {
    return datatype != SXML_EXI_NONE && !encoding->options.fidelity.preserves_lexical_values &&
           encoding->schema_grammars.datatypes[datatype].datatype_kind == SXML_EXI_DATATYPE_LIST;
}

/* A value that is not a list, written now. */
static sxml_status sxml_exi_write_scalar_value(sxml_exi_encoding *encoding, const sxml_exi_value_write *value) {
    if (sxml_exi_encoder_is_string(encoding, value->datatype)) {
        return sxml_exi_write_string_value(
            encoding, &(sxml_exi_string_value_write){.key = &value->key,
                                                     .character_set = sxml_exi_encoder_character_set(encoding, value->datatype)});
    }
    sxml_exi_validity validity = {0};
    const sxml_exi_canonicalization *canonicalization = &encoding->request->canonicalization;
    sxml_status status = sxml_exi_write_typed_value(
        &(sxml_exi_typed_write){.grammars = &encoding->schema_grammars,
                                .writer = &encoding->writer,
                                .uses_utc_time = canonicalization->is_canonical && canonicalization->uses_utc_time},
        (sxml_exi_value_text){.datatype = value->datatype, .text = value->key.text}, &validity);
    if (status == SXML_STATUS_OK && !validity.is_valid) {
        status = SXML_EXI_ENCODER_FAIL(encoding, SXML_STATUS_INVALID_ARGUMENT, "a value not of its datatype");
    }
    return status;
}

/* A value of an AT or CH event, written now (7): a String, a typed
   value, or a List (7.1.11: its length, then its items). */
static sxml_status sxml_exi_write_any_value(sxml_exi_encoding *encoding, const sxml_exi_value_write *value) {
    if (!sxml_exi_encoder_is_list(encoding, value->datatype)) {
        return sxml_exi_write_scalar_value(encoding, value);
    }
    const uint32_t item_datatype = encoding->schema_grammars.datatypes[value->datatype].base_datatype;
    uint64_t item_count = 0;
    size_t offset = 0;
    sxml_byte_view item = {0};
    while (sxml_exi_next_list_item(value->key.text, &offset, &item)) {
        item_count++;
    }
    sxml_status status = sxml_exi_write_unsigned(&encoding->writer, item_count);
    offset = 0;
    while (status == SXML_STATUS_OK && sxml_exi_next_list_item(value->key.text, &offset, &item)) {
        status = sxml_exi_write_scalar_value(
            encoding, &(sxml_exi_value_write){.key = {.text = item, .uri = value->key.uri, .local_name = value->key.local_name},
                                              .datatype = item_datatype});
    }
    return status;
}

/* Whether text is a value of datatype, as the oracle decides it: a
   String always, a List when each item is one of its item type, any
   value with Preserve.lexicalValues. */
static sxml_status sxml_exi_is_value_of(sxml_exi_encoding *encoding, const sxml_exi_value_write *value,
                                        sxml_exi_validity *validity) {
    validity->is_valid = true;
    if (sxml_exi_encoder_is_string(encoding, value->datatype)) {
        return SXML_STATUS_OK;
    }
    const bool is_list = sxml_exi_encoder_is_list(encoding, value->datatype);
    const uint32_t datatype = is_list ? encoding->schema_grammars.datatypes[value->datatype].base_datatype : value->datatype;
    if (sxml_exi_encoder_is_string(encoding, datatype)) {
        return SXML_STATUS_OK;
    }
    sxml_status status = SXML_STATUS_OK;
    size_t offset = 0;
    sxml_byte_view item = value->key.text;
    bool has_item = !is_list || sxml_exi_next_list_item(value->key.text, &offset, &item);
    while (status == SXML_STATUS_OK && has_item && validity->is_valid) {
        status = sxml_exi_write_typed_value(&(sxml_exi_typed_write){.grammars = &encoding->schema_grammars, .writer = NULL},
                                            (sxml_exi_value_text){.datatype = datatype, .text = item}, validity);
        has_item = is_list && sxml_exi_next_list_item(value->key.text, &offset, &item);
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
            const sxml_exi_recorded_value *recorded = &encoding->values[value_index];
            if (recorded->channel == channel_index) {
                const sxml_byte_view text =
                    recorded->is_owned
                        ? (sxml_byte_view){.bytes = encoding->owned_text.bytes + recorded->owned_offset, .byte_count = recorded->text.byte_count}
                        : recorded->text;
                status = sxml_exi_write_any_value(
                    encoding, &(sxml_exi_value_write){.key = {.text = text, .uri = values->uri, .local_name = values->local_name},
                                                      .datatype = recorded->datatype});
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
    encoding->owned_text.byte_count = 0;
    encoding->block_number++;
    return status;
}

/* A value of an AT or CH event: written now, or recorded for its channel
   and the block ended when it holds blockSize values (9.1). A text not in
   the tree (whitespace applied) is copied into the encoder's store. */
typedef struct sxml_exi_value_record {
    sxml_exi_value_write value;
    bool is_made;
} sxml_exi_value_record;

static sxml_status sxml_exi_encode_typed_value(sxml_exi_encoding *encoding, const sxml_exi_value_record *record) {
    const sxml_exi_string_key *key = &record->value.key;
    if (!encoding->is_channelized) {
        return sxml_exi_write_any_value(encoding, &record->value);
    }
    const size_t owned_offset = encoding->owned_text.byte_count;
    if (record->is_made) {
        sxml_status status = sxml_exi_buffer_append(&encoding->owned_text, key->text);
        if (status != SXML_STATUS_OK) {
            return status;
        }
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
    encoding->values[encoding->value_count++] = (sxml_exi_recorded_value){.text = key->text,
                                                                         .owned_offset = owned_offset,
                                                                         .channel = owner->channel_index,
                                                                         .datatype = record->value.datatype,
                                                                         .is_owned = record->is_made};
    if (encoding->value_count == encoding->block_size) {
        return sxml_exi_flush_block(encoding);
    }
    return SXML_STATUS_OK;
}

static sxml_status sxml_exi_encode_value(sxml_exi_encoding *encoding, const sxml_exi_string_key *key) {
    return sxml_exi_encode_typed_value(
        encoding, &(sxml_exi_value_record){.value = {.key = *key, .datatype = SXML_EXI_NONE}, .is_made = false});
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
    const sxml_exi_open_element *parent = sxml_exi_innermost(encoding);
    if (encoding->elements == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    encoding->elements[encoding->element_count++] = (sxml_exi_open_element){
        .node_index = event->node.node_index.index,
        .uri = qname.uri,
        .local_name = qname.local_name,
        .element_grammar = entry->element_grammar,
        .nonterminal = SXML_EXI_NONTERMINAL_START_TAG_CONTENT,
        /* 8.5.4.4.1: an element of a global element declaration has its
           grammar, wherever SE (*) matched it. */
        .schema_grammar = encoding->is_schema_informed ? entry->schema_element_grammar : SXML_EXI_NONE,
        .has_next_child = description->has_children,
        .next_child = description->first_child.index,
        .is_space_preserved = parent != NULL && parent->is_space_preserved,
    };
    return SXML_STATUS_OK;
}

/* ------------------------------------------------ Schema-informed events */

/* The schema-informed grammar of the innermost element or the top level;
   SXML_EXI_NONE in a built-in element grammar. */
static uint32_t sxml_exi_encoder_schema_grammar(sxml_exi_encoding *encoding) {
    if (!encoding->is_schema_informed) {
        return SXML_EXI_NONE;
    }
    const sxml_exi_open_element *element = sxml_exi_innermost(encoding);
    return element == NULL ? encoding->top_schema_grammar : element->schema_grammar;
}

static void sxml_exi_encoder_move_to_schema_grammar(sxml_exi_encoding *encoding, uint32_t grammar) {
    sxml_exi_open_element *element = sxml_exi_innermost(encoding);
    if (element == NULL) {
        encoding->top_schema_grammar = grammar;
    } else {
        element->schema_grammar = grammar;
    }
}

/* Where undeclared SE, CH, ER, CM and PI lead (8.5.4.4.1). */
static uint32_t sxml_exi_encoder_content_of(const sxml_exi_encoding *encoding, uint32_t grammar) {
    const uint32_t content = encoding->schema_grammars.grammars[grammar].element_content;
    return content != SXML_EXI_NONE ? content : grammar;
}

/* A declared production of a grammar: by terminal, with its qname for SE
   (qname) and AT (qname), its URI for SE (uri : *) and AT (uri : *),
   typed or not for CH. */
typedef struct sxml_exi_production_query {
    uint32_t grammar;
    sxml_exi_event_kind event_kind;
    uint32_t uri;
    uint32_t local_name;
    bool is_typed;
} sxml_exi_production_query;

static sxml_exi_lookup sxml_exi_find_schema_production(const sxml_exi_encoding *encoding,
                                                       const sxml_exi_production_query *query) {
    const sxml_exi_schema_grammars *grammars = &encoding->schema_grammars;
    const sxml_exi_schema_grammar *grammar = &grammars->grammars[query->grammar];
    for (uint32_t code_index = 0; code_index < grammar->production_count; code_index++) {
        const sxml_exi_schema_production *production = &grammars->productions[grammar->first_production + code_index];
        if (production->event_kind != query->event_kind) {
            continue;
        }
        bool is_match = true;
        switch (production->event_kind) {
        case SXML_EXI_EVENT_START_ELEMENT:
        case SXML_EXI_EVENT_ATTRIBUTE:
            is_match = production->uri == query->uri && production->local_name == query->local_name;
            break;
        case SXML_EXI_EVENT_START_ELEMENT_URI:
        case SXML_EXI_EVENT_ATTRIBUTE_URI:
            is_match = production->uri == query->uri;
            break;
        case SXML_EXI_EVENT_CHARACTERS:
            is_match = (production->datatype != SXML_EXI_NONE) == query->is_typed;
            break;
        case SXML_EXI_EVENT_START_DOCUMENT:
        case SXML_EXI_EVENT_END_DOCUMENT:
        case SXML_EXI_EVENT_START_ELEMENT_ANY:
        case SXML_EXI_EVENT_END_ELEMENT:
        case SXML_EXI_EVENT_ATTRIBUTE_ANY:
        case SXML_EXI_EVENT_NAMESPACE:
        case SXML_EXI_EVENT_COMMENT:
        case SXML_EXI_EVENT_PROCESSING_INSTRUCTION:
        case SXML_EXI_EVENT_DOCTYPE:
        case SXML_EXI_EVENT_ENTITY_REFERENCE:
        case SXML_EXI_EVENT_SELF_CONTAINED:
            break;
        }
        if (is_match) {
            return (sxml_exi_lookup){.is_found = true, .index = code_index};
        }
    }
    return (sxml_exi_lookup){.is_found = false, .index = 0};
}

/* Writes an event code of the current schema-informed grammar; strict
   (or pruned) grammars without the production refuse the content. */
static sxml_status sxml_exi_write_schema_code(sxml_exi_encoding *encoding, sxml_exi_schema_match match) {
    sxml_status status = sxml_exi_schema_write_event_code(
        &encoding->schema_grammars,
        &(sxml_exi_schema_code_write){.grammar = sxml_exi_encoder_schema_grammar(encoding), .match = match},
        &encoding->writer);
    if (status == SXML_STATUS_DYNAMIC_ERROR) {
        return SXML_EXI_ENCODER_FAIL(encoding, status, "content the schema-informed grammar has no production for");
    }
    return status;
}

static sxml_status sxml_exi_write_declared_code(sxml_exi_encoding *encoding, uint32_t code) {
    return sxml_exi_write_schema_code(encoding,
                                      (sxml_exi_schema_match){.match_kind = SXML_EXI_SCHEMA_MATCH_DECLARED, .code = code});
}

static sxml_status sxml_exi_write_undeclared_code(sxml_exi_encoding *encoding, sxml_exi_undeclared undeclared) {
    return sxml_exi_write_schema_code(
        encoding, (sxml_exi_schema_match){.match_kind = SXML_EXI_SCHEMA_MATCH_UNDECLARED, .undeclared = undeclared});
}

static bool sxml_exi_encoder_has_undeclared(const sxml_exi_encoding *encoding, sxml_exi_undeclared_query query) {
    return sxml_exi_schema_has_undeclared(&encoding->schema_grammars, query);
}

/* Opens an element in a schema-informed grammar: its grammar (a global
   element's for SE (*) and SE (uri : *); none declared: its built-in
   element grammar, 8.4.3). */
typedef struct sxml_exi_schema_open {
    const sxml_exi_node_event *event;
    sxml_exi_table_qname qname;
    uint32_t element_grammar;
} sxml_exi_schema_open;

static sxml_status sxml_exi_open_schema_element(sxml_exi_encoding *encoding, const sxml_exi_schema_open *open) {
    const sxml_exi_table_qname qname = open->qname;
    sxml_exi_local_name *entry = &encoding->table.uris[qname.uri].local_names[qname.local_name];
    uint32_t schema_grammar = open->element_grammar;
    uint32_t element_grammar = SXML_EXI_NONE;
    sxml_status status = SXML_STATUS_OK;
    if (schema_grammar == SXML_EXI_NONE) {
        schema_grammar = entry->schema_element_grammar;
    }
    if (schema_grammar == SXML_EXI_NONE) {
        if (entry->element_grammar == SXML_EXI_NONE) {
            uint32_t made = 0;
            status = sxml_exi_grammars_add_element(&encoding->grammars, &made);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            entry = &encoding->table.uris[qname.uri].local_names[qname.local_name];
            entry->element_grammar = made;
        }
        element_grammar = entry->element_grammar;
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
    const sxml_node_description *description = open->event->description;
    const sxml_exi_open_element *parent = sxml_exi_innermost(encoding);
    if (encoding->elements == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    encoding->elements[encoding->element_count++] = (sxml_exi_open_element){
        .node_index = open->event->node.node_index.index,
        .uri = qname.uri,
        .local_name = qname.local_name,
        .element_grammar = element_grammar,
        .nonterminal = SXML_EXI_NONTERMINAL_START_TAG_CONTENT,
        .schema_grammar = schema_grammar,
        .has_next_child = description->has_children,
        .next_child = description->first_child.index,
        .is_space_preserved = parent != NULL && parent->is_space_preserved,
    };
    return SXML_STATUS_OK;
}

/* SE in a schema-informed grammar: SE (qname), SE (uri : *), SE (*), or
   the undeclared SE (*) to Element_{i,content2} (8.5.4.3, 8.5.4.4). */
static sxml_status sxml_exi_encode_schema_start_element(sxml_exi_encoding *encoding, const sxml_exi_node_event *event) {
    const uint32_t grammar = sxml_exi_encoder_schema_grammar(encoding);
    sxml_exi_name_text name = {0};
    sxml_status status = sxml_exi_name_text_of(encoding, event->description, &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_exi_table_qname qname = {0};
    const bool is_known = sxml_exi_known_qname(encoding, &name, &qname).is_found;
    sxml_exi_lookup production = {.is_found = false};
    if (is_known) {
        production = sxml_exi_find_schema_production(encoding, &(sxml_exi_production_query){.grammar = grammar,
                                                                                            .event_kind = SXML_EXI_EVENT_START_ELEMENT,
                                                                                            .uri = qname.uri,
                                                                                            .local_name = qname.local_name});
    }
    if (production.is_found) {
        const sxml_exi_schema_production *declared =
            sxml_exi_schema_production_at(&encoding->schema_grammars, grammar, production.index);
        const uint32_t next = declared->next;
        const uint32_t child = encoding->schema_grammars.element_grammars[declared->element];
        status = sxml_exi_write_declared_code(encoding, production.index);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_qname(encoding, &(sxml_exi_qname_write){.name = &name, .writes_uri_and_local_name = false},
                                          &qname);
        }
        if (status == SXML_STATUS_OK) {
            sxml_exi_encoder_move_to_schema_grammar(encoding, next);
            status = sxml_exi_open_schema_element(
                encoding, &(sxml_exi_schema_open){.event = event, .qname = qname, .element_grammar = child});
        }
        return status;
    }
    sxml_exi_lookup uri = {0};
    status = sxml_exi_find_uri(&encoding->table, &(sxml_exi_string_key){.text = name.uri}, &uri);
    if (status == SXML_STATUS_OK && uri.is_found) {
        production = sxml_exi_find_schema_production(
            encoding, &(sxml_exi_production_query){.grammar = grammar, .event_kind = SXML_EXI_EVENT_START_ELEMENT_URI, .uri = uri.index});
    }
    uint32_t next = sxml_exi_encoder_content_of(encoding, grammar);
    if (status == SXML_STATUS_OK && production.is_found) {
        next = sxml_exi_schema_production_at(&encoding->schema_grammars, grammar, production.index)->next;
        qname.uri = uri.index;
        status = sxml_exi_write_declared_code(encoding, production.index);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_encode_local_name(encoding, &(sxml_exi_string_key){.text = name.local_name, .uri = uri.index},
                                                &qname.local_name);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_qname(encoding, &(sxml_exi_qname_write){.name = &name, .writes_uri_and_local_name = false},
                                          &qname);
        }
    } else if (status == SXML_STATUS_OK) {
        production = sxml_exi_find_schema_production(
            encoding, &(sxml_exi_production_query){.grammar = grammar, .event_kind = SXML_EXI_EVENT_START_ELEMENT_ANY});
        if (production.is_found) {
            next = sxml_exi_schema_production_at(&encoding->schema_grammars, grammar, production.index)->next;
            status = sxml_exi_write_declared_code(encoding, production.index);
        } else {
            status = sxml_exi_write_undeclared_code(encoding, SXML_EXI_UNDECLARED_START_ELEMENT_ANY);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_qname(encoding, &(sxml_exi_qname_write){.name = &name, .writes_uri_and_local_name = true},
                                          &qname);
        }
    }
    if (status == SXML_STATUS_OK) {
        sxml_exi_encoder_move_to_schema_grammar(encoding, next);
        status = sxml_exi_open_schema_element(
            encoding, &(sxml_exi_schema_open){.event = event, .qname = qname, .element_grammar = SXML_EXI_NONE});
    }
    return status;
}

/* EE in a schema-informed grammar; with strict and no EE production, an
   empty CH first when its type takes one (the oracle's). */
static sxml_status sxml_exi_encode_schema_end_element(sxml_exi_encoding *encoding) {
    uint32_t grammar = sxml_exi_encoder_schema_grammar(encoding);
    sxml_exi_lookup production = sxml_exi_find_schema_production(
        encoding, &(sxml_exi_production_query){.grammar = grammar, .event_kind = SXML_EXI_EVENT_END_ELEMENT});
    if (production.is_found) {
        return sxml_exi_write_declared_code(encoding, production.index);
    }
    if (sxml_exi_encoder_has_undeclared(encoding, (sxml_exi_undeclared_query){.grammar = grammar,
                                                                              .undeclared = SXML_EXI_UNDECLARED_END_ELEMENT})) {
        return sxml_exi_write_undeclared_code(encoding, SXML_EXI_UNDECLARED_END_ELEMENT);
    }
    const sxml_exi_lookup characters = sxml_exi_find_schema_production(
        encoding, &(sxml_exi_production_query){.grammar = grammar, .event_kind = SXML_EXI_EVENT_CHARACTERS, .is_typed = true});
    if (!characters.is_found) {
        return SXML_EXI_ENCODER_FAIL(encoding, SXML_STATUS_DYNAMIC_ERROR, "an end of element the grammar has no production for");
    }
    const sxml_exi_schema_production *typed = sxml_exi_schema_production_at(&encoding->schema_grammars, grammar, characters.index);
    const sxml_exi_open_element *element = sxml_exi_innermost(encoding);
    const sxml_exi_value_write empty = {.key = {.text = {.bytes = NULL, .byte_count = 0},
                                                .uri = element->uri,
                                                .local_name = element->local_name},
                                        .datatype = typed->datatype};
    sxml_exi_validity validity = {0};
    sxml_status status = sxml_exi_is_value_of(encoding, &empty, &validity);
    if (status == SXML_STATUS_OK && !validity.is_valid) {
        return SXML_EXI_ENCODER_FAIL(encoding, SXML_STATUS_DYNAMIC_ERROR, "an empty value its type does not take");
    }
    const uint32_t next = typed->next;
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_declared_code(encoding, characters.index);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_encode_typed_value(encoding, &(sxml_exi_value_record){.value = empty, .is_made = false});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_exi_encoder_move_to_schema_grammar(encoding, next);
    grammar = next;
    production = sxml_exi_find_schema_production(
        encoding, &(sxml_exi_production_query){.grammar = grammar, .event_kind = SXML_EXI_EVENT_END_ELEMENT});
    if (!production.is_found) {
        return SXML_EXI_ENCODER_FAIL(encoding, SXML_STATUS_DYNAMIC_ERROR, "an end of element the grammar has no production for");
    }
    return sxml_exi_write_declared_code(encoding, production.index);
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
    sxml_status status = SXML_STATUS_OK;
    if (sxml_exi_encoder_schema_grammar(encoding) != SXML_EXI_NONE) {
        status = sxml_exi_write_schema_code(
            encoding, (sxml_exi_schema_match){.match_kind = SXML_EXI_SCHEMA_MATCH_UNDECLARED, .undeclared = SXML_EXI_UNDECLARED_NAMESPACE});
    } else {
        status = sxml_exi_write_tail(encoding, SXML_EXI_EVENT_NAMESPACE);
    }
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
/* The namespace declarations of an element, written at once, or with
   Canonical EXI (its 4.4) sorted by prefix first: each inserted after the
   equal ones, as the oracle inserts them. */
typedef struct sxml_exi_namespace_list {
    sxml_exi_namespace_write *writes;
    size_t write_count;
    size_t write_capacity;
    bool sorts;
} sxml_exi_namespace_list;

static int sxml_exi_prefix_order(sxml_byte_view left, sxml_byte_view right) {
    const size_t common = left.byte_count < right.byte_count ? left.byte_count : right.byte_count;
    const int order = common == 0 ? 0 : memcmp(left.bytes, right.bytes, common);
    if (order != 0 || left.byte_count == right.byte_count) {
        return order;
    }
    return left.byte_count < right.byte_count ? -1 : 1;
}

static sxml_status sxml_exi_emit_namespace(sxml_exi_encoding *encoding, sxml_exi_namespace_list *list,
                                           const sxml_exi_namespace_write *write) {
    if (!list->sorts) {
        return sxml_exi_encode_namespace(encoding, write);
    }
    if (list->writes == NULL || list->write_count == list->write_capacity) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    size_t position = list->write_count;
    while (position > 0 && sxml_exi_prefix_order(list->writes[position - 1].binding.prefix, write->binding.prefix) > 0) {
        list->writes[position] = list->writes[position - 1];
        position--;
    }
    list->writes[position] = *write;
    list->write_count++;
    return SXML_STATUS_OK;
}

static sxml_status sxml_exi_encode_namespaces(sxml_exi_encoding *encoding, const sxml_exi_node_event *event) {
    sxml_namespace_binding_view own = {0};
    sxml_namespace_binding_view inherited = {0};
    sxml_status status = sxml_node_get_in_scope_namespaces(event->node, &own);
    sxml_exi_namespace_list list = {.sorts = encoding->request->canonicalization.is_canonical};
    if (status == SXML_STATUS_OK && list.sorts) {
        void *memory = NULL;
        list.write_capacity = own.namespace_binding_count + 1;
        status = sxml_allocator_allocate_array(
            encoding->allocator,
            (sxml_array_allocation_request){.element_count = list.write_capacity,
                                            .element_size = sizeof(sxml_exi_namespace_write),
                                            .element_alignment = alignof(sxml_exi_namespace_write)},
            &memory);
        list.writes = memory;
    }
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
            status = sxml_exi_emit_namespace(
                encoding, &list, &(sxml_exi_namespace_write){.binding = text, .is_local_element_namespace = is_local});
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
            status = sxml_exi_emit_namespace(
                encoding, &list,
                &(sxml_exi_namespace_write){.binding = {.uri = {.bytes = NULL, .byte_count = 0}, .local_name = {.bytes = NULL, .byte_count = 0}, .prefix = {.bytes = NULL, .byte_count = 0}},
                                            .is_local_element_namespace = !has_local_element_namespace &&
                                                                          element_name.uri.byte_count == 0 &&
                                                                          element_name.prefix.byte_count == 0});
        }
    }
    for (size_t write_index = 0; status == SXML_STATUS_OK && write_index < list.write_count; write_index++) {
        status = sxml_exi_encode_namespace(encoding, &list.writes[write_index]);
    }
    sxml_allocator_release(encoding->allocator, list.writes);
    return status;
}

/* xsi:type's value as a QName (7.1.7): its prefix resolved against the
   element's in-scope namespaces (8.4.3: an unbound prefix makes the whole
   lexical value the local-name in no namespace). */
static sxml_status sxml_exi_encode_type_name(sxml_exi_encoding *encoding, const sxml_exi_node_event *event,
                                             sxml_exi_table_qname *type) {
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
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_qname(encoding, &(sxml_exi_qname_write){.name = &name, .writes_uri_and_local_name = true},
                                      type);
    }
    return status;
}

/* 8.5.4.4: the type xsi:type names, when a grammar is found for it, is
   the element's grammar from here on. */
static void sxml_exi_encoder_switch_to_type(sxml_exi_encoding *encoding, sxml_exi_table_qname type) {
    sxml_exi_open_element *element = sxml_exi_innermost(encoding);
    if (!encoding->is_schema_informed || element == NULL) {
        return;
    }
    const uint32_t grammar = encoding->table.uris[type.uri].local_names[type.local_name].type_grammar;
    if (grammar != SXML_EXI_NONE) {
        element->schema_grammar = grammar;
    }
}

/* With Preserve.lexicalValues: xsi:type's text, its prefix resolved
   against the element's in-scope namespaces. */
static sxml_status sxml_exi_switch_to_lexical_type(sxml_exi_encoding *encoding, const sxml_exi_node_event *event) {
    sxml_byte_view value = {0};
    sxml_status status = sxml_node_get_content(event->node, &value);
    size_t colon = 0;
    while (status == SXML_STATUS_OK && colon < value.byte_count && value.bytes[colon] != ':') {
        colon++;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool has_prefix = colon < value.byte_count;
    const sxml_byte_view prefix = {.bytes = value.bytes, .byte_count = has_prefix ? colon : 0};
    sxml_exi_name_text name = {.local_name = has_prefix ? (sxml_byte_view){.bytes = value.bytes + colon + 1,
                                                                         .byte_count = value.byte_count - colon - 1}
                                                        : value};
    sxml_namespace_binding_view bindings = {0};
    const sxml_node_reference element = {.tree = event->node.tree, .node_index = event->description->parent};
    status = sxml_node_get_in_scope_namespaces(element, &bindings);
    bool is_bound = !has_prefix;
    for (size_t binding_index = 0; binding_index < bindings.namespace_binding_count && status == SXML_STATUS_OK && has_prefix;
         binding_index++) {
        sxml_exi_name_text text = {0};
        status = sxml_exi_binding_text(encoding, &bindings.namespace_bindings[binding_index], &text);
        if (status == SXML_STATUS_OK && sxml_exi_texts_equal(text.prefix, prefix)) {
            name.uri = text.uri;
            is_bound = true;
        }
    }
    sxml_exi_table_qname type = {0};
    if (status == SXML_STATUS_OK && is_bound && sxml_exi_known_qname(encoding, &name, &type).is_found) {
        sxml_exi_encoder_switch_to_type(encoding, type);
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
        sxml_exi_table_qname type = {0};
        status = sxml_exi_encode_type_name(encoding, event, &type);
        if (status == SXML_STATUS_OK) {
            sxml_exi_encoder_switch_to_type(encoding, type);
        }
        return status;
    }
    if (is_type && encoding->is_schema_informed) {
        /* Preserve.lexicalValues: a String, in the structure channel. */
        sxml_byte_view text = {0};
        status = sxml_node_get_content(event->node, &text);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_any_value(
                encoding, &(sxml_exi_value_write){.key = {.text = text, .uri = qname.uri, .local_name = qname.local_name},
                                                  .datatype = SXML_EXI_NONE});
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_switch_to_lexical_type(encoding, event);
        }
        return status;
    }
    sxml_byte_view value = {0};
    status = sxml_node_get_content(event->node, &value);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_encode_value(
            encoding, &(sxml_exi_string_key){.text = value, .uri = qname.uri, .local_name = qname.local_name});
    }
    return status;
}

/* ------------------------------------------- Schema-informed attributes */

/* The attribute's prefix, after its qname (7.1.7). */
static sxml_status sxml_exi_write_attribute_prefix(sxml_exi_encoding *encoding, const sxml_exi_name_text *name,
                                                   sxml_exi_table_qname *qname) {
    return sxml_exi_write_qname(encoding, &(sxml_exi_qname_write){.name = name, .writes_uri_and_local_name = false}, qname);
}

/* xsi:type in a schema-informed grammar (8.5.4.4): AT (xsi:type), else an
   AT (*); its value a QName in the structure channel (a String with
   Preserve.lexicalValues); then the type's grammar. */
static sxml_status sxml_exi_encode_schema_type(sxml_exi_encoding *encoding, const sxml_exi_node_event *event) {
    const uint32_t grammar = sxml_exi_encoder_schema_grammar(encoding);
    sxml_exi_name_text name = {0};
    sxml_status status = sxml_exi_name_text_of(encoding, event->description, &name);
    sxml_exi_table_qname qname = {.uri = 2, .local_name = 1};
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (sxml_exi_encoder_has_undeclared(encoding,
                                        (sxml_exi_undeclared_query){.grammar = grammar, .undeclared = SXML_EXI_UNDECLARED_TYPE})) {
        status = sxml_exi_write_undeclared_code(encoding, SXML_EXI_UNDECLARED_TYPE);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_attribute_prefix(encoding, &name, &qname);
        }
    } else {
        const sxml_exi_lookup any = sxml_exi_find_schema_production(
            encoding, &(sxml_exi_production_query){.grammar = grammar, .event_kind = SXML_EXI_EVENT_ATTRIBUTE_ANY});
        status = any.is_found ? sxml_exi_write_declared_code(encoding, any.index)
                              : sxml_exi_write_undeclared_code(encoding, SXML_EXI_UNDECLARED_ATTRIBUTE_ANY);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_qname(encoding, &(sxml_exi_qname_write){.name = &name, .writes_uri_and_local_name = true},
                                          &qname);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (encoding->options.fidelity.preserves_lexical_values) {
        sxml_byte_view text = {0};
        status = sxml_node_get_content(event->node, &text);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_any_value(
                encoding, &(sxml_exi_value_write){.key = {.text = text, .uri = 2, .local_name = 1}, .datatype = SXML_EXI_NONE});
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_switch_to_lexical_type(encoding, event);
        }
        return status;
    }
    sxml_exi_table_qname type = {0};
    status = sxml_exi_encode_type_name(encoding, event, &type);
    if (status == SXML_STATUS_OK) {
        sxml_exi_encoder_switch_to_type(encoding, type);
    }
    return status;
}

/* Java's String.trim, for xsi:nil's Boolean. */
static sxml_byte_view sxml_exi_encoder_trim(sxml_byte_view text) {
    while (text.byte_count > 0 && text.bytes[0] <= 0x20) {
        text.bytes++;
        text.byte_count--;
    }
    while (text.byte_count > 0 && text.bytes[text.byte_count - 1] <= 0x20) {
        text.byte_count--;
    }
    return text;
}

/* xsi:nil in a schema-informed grammar (8.5.4.4): AT (xsi:nil) with a
   Boolean in the structure channel, true leading to TypeEmpty; the
   oracle writes no xsi:nil="false" (an insignificant one), and an xsi:nil
   that is no Boolean as AT (*) [untyped value]. */
static sxml_status sxml_exi_encode_schema_nil(sxml_exi_encoding *encoding, const sxml_exi_node_event *event) {
    const uint32_t grammar = sxml_exi_encoder_schema_grammar(encoding);
    sxml_exi_name_text name = {0};
    sxml_byte_view value = {0};
    sxml_status status = sxml_exi_name_text_of(encoding, event->description, &name);
    if (status == SXML_STATUS_OK) {
        status = sxml_node_get_content(event->node, &value);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_exi_table_qname qname = {.uri = 2, .local_name = 0};
    const sxml_byte_view trimmed = sxml_exi_encoder_trim(value);
    const bool is_true = sxml_exi_texts_equal(trimmed, SXML_EXI_ENCODER_TEXT("true")) ||
                         sxml_exi_texts_equal(trimmed, SXML_EXI_ENCODER_TEXT("1"));
    const bool is_false = sxml_exi_texts_equal(trimmed, SXML_EXI_ENCODER_TEXT("false")) ||
                          sxml_exi_texts_equal(trimmed, SXML_EXI_ENCODER_TEXT("0"));
    const bool is_lexical = encoding->options.fidelity.preserves_lexical_values;
    if (!is_true && !is_false) {
        const uint32_t attribute_count = encoding->schema_grammars.grammars[grammar].attribute_count;
        status = sxml_exi_write_schema_code(encoding, (sxml_exi_schema_match){.match_kind = SXML_EXI_SCHEMA_MATCH_UNDECLARED,
                                                                              .undeclared = SXML_EXI_UNDECLARED_ATTRIBUTE_INVALID,
                                                                              .attribute = attribute_count});
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_qname(encoding, &(sxml_exi_qname_write){.name = &name, .writes_uri_and_local_name = true},
                                          &qname);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_encode_value(encoding, &(sxml_exi_string_key){.text = value, .uri = 2, .local_name = 1});
        }
        return status;
    }
    if (is_false && !is_lexical) {
        return SXML_STATUS_OK;
    }
    if (sxml_exi_encoder_has_undeclared(encoding,
                                        (sxml_exi_undeclared_query){.grammar = grammar, .undeclared = SXML_EXI_UNDECLARED_NIL})) {
        status = sxml_exi_write_undeclared_code(encoding, SXML_EXI_UNDECLARED_NIL);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_attribute_prefix(encoding, &name, &qname);
        }
    } else {
        const sxml_exi_lookup any = sxml_exi_find_schema_production(
            encoding, &(sxml_exi_production_query){.grammar = grammar, .event_kind = SXML_EXI_EVENT_ATTRIBUTE_ANY});
        if (!any.is_found) {
            return SXML_EXI_ENCODER_FAIL(encoding, SXML_STATUS_DYNAMIC_ERROR, "an xsi:nil the grammar has no production for");
        }
        status = sxml_exi_write_declared_code(encoding, any.index);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_qname(encoding, &(sxml_exi_qname_write){.name = &name, .writes_uri_and_local_name = true},
                                          &qname);
        }
    }
    if (status == SXML_STATUS_OK && is_lexical) {
        /* The oracle's: the value partitions of xsi:type. */
        status = sxml_exi_write_any_value(
            encoding, &(sxml_exi_value_write){.key = {.text = value, .uri = 2, .local_name = 1},
                                              .datatype = encoding->schema_grammars.boolean_datatype});
    } else if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_boolean(&encoding->writer, (sxml_exi_boolean){.value = is_true});
    }
    if (status == SXML_STATUS_OK && is_true) {
        const uint32_t type_empty = encoding->schema_grammars.grammars[grammar].type_empty;
        if (type_empty != SXML_EXI_NONE) {
            sxml_exi_encoder_move_to_schema_grammar(encoding, type_empty);
        }
    }
    return status;
}

/* An attribute in a schema-informed grammar (8.5.4.1.3.2, 8.5.4.1.4,
   8.5.4.4.1), as the oracle chooses: AT (qname) with a typed value, or
   AT (qname) [untyped value] for a value not of its type; else AT (uri :
   *) or AT (*), typed by the global attribute of the qname if any (a
   value not of it: AT (*) [untyped value]); else the undeclared AT (*). */
static sxml_status sxml_exi_encode_schema_attribute(sxml_exi_encoding *encoding, const sxml_exi_node_event *event) {
    const uint32_t grammar = sxml_exi_encoder_schema_grammar(encoding);
    const sxml_exi_schema_grammars *grammars = &encoding->schema_grammars;
    sxml_exi_name_text name = {0};
    sxml_byte_view value = {0};
    sxml_status status = sxml_exi_name_text_of(encoding, event->description, &name);
    if (status == SXML_STATUS_OK) {
        status = sxml_node_get_content(event->node, &value);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_exi_table_qname qname = {0};
    const bool is_known = sxml_exi_known_qname(encoding, &name, &qname).is_found;
    sxml_exi_lookup declared = {.is_found = false};
    if (is_known) {
        declared = sxml_exi_find_schema_production(encoding, &(sxml_exi_production_query){.grammar = grammar,
                                                                                          .event_kind = SXML_EXI_EVENT_ATTRIBUTE,
                                                                                          .uri = qname.uri,
                                                                                          .local_name = qname.local_name});
    }
    uint32_t datatype = SXML_EXI_NONE;
    uint32_t next = grammar;
    if (declared.is_found) {
        const sxml_exi_schema_production *production = sxml_exi_schema_production_at(grammars, grammar, declared.index);
        next = production->next;
        sxml_exi_validity validity = {0};
        status = sxml_exi_is_value_of(
            encoding,
            &(sxml_exi_value_write){.key = {.text = value, .uri = qname.uri, .local_name = qname.local_name},
                                    .datatype = production->datatype},
            &validity);
        if (status == SXML_STATUS_OK && validity.is_valid) {
            datatype = production->datatype;
            status = sxml_exi_write_declared_code(encoding, declared.index);
        } else if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_schema_code(
                encoding, (sxml_exi_schema_match){.match_kind = SXML_EXI_SCHEMA_MATCH_UNDECLARED,
                                                  .undeclared = SXML_EXI_UNDECLARED_ATTRIBUTE_INVALID,
                                                  .attribute = declared.index - grammars->grammars[grammar].least_attribute});
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_attribute_prefix(encoding, &name, &qname);
        }
    } else {
        sxml_exi_lookup uri = {0};
        status = sxml_exi_find_uri(&encoding->table, &(sxml_exi_string_key){.text = name.uri}, &uri);
        sxml_exi_lookup wildcard = {.is_found = false};
        bool is_uri_wildcard = false;
        if (status == SXML_STATUS_OK && uri.is_found) {
            wildcard = sxml_exi_find_schema_production(
                encoding, &(sxml_exi_production_query){.grammar = grammar, .event_kind = SXML_EXI_EVENT_ATTRIBUTE_URI, .uri = uri.index});
            is_uri_wildcard = wildcard.is_found;
        }
        if (!wildcard.is_found) {
            wildcard = sxml_exi_find_schema_production(
                encoding, &(sxml_exi_production_query){.grammar = grammar, .event_kind = SXML_EXI_EVENT_ATTRIBUTE_ANY});
        }
        const uint32_t global =
            is_known ? encoding->table.uris[qname.uri].local_names[qname.local_name].attribute_datatype : SXML_EXI_NONE;
        sxml_exi_validity validity = {.is_valid = true};
        if (status == SXML_STATUS_OK && global != SXML_EXI_NONE) {
            status = sxml_exi_is_value_of(
                encoding,
                &(sxml_exi_value_write){.key = {.text = value, .uri = qname.uri, .local_name = qname.local_name}, .datatype = global},
                &validity);
            datatype = validity.is_valid ? global : SXML_EXI_NONE;
        }
        if (status == SXML_STATUS_OK && !validity.is_valid) {
            status = sxml_exi_write_schema_code(
                encoding, (sxml_exi_schema_match){.match_kind = SXML_EXI_SCHEMA_MATCH_UNDECLARED,
                                                  .undeclared = SXML_EXI_UNDECLARED_ATTRIBUTE_INVALID,
                                                  .attribute = grammars->grammars[grammar].attribute_count});
        } else if (status == SXML_STATUS_OK && wildcard.is_found) {
            status = sxml_exi_write_declared_code(encoding, wildcard.index);
        } else if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_undeclared_code(encoding, SXML_EXI_UNDECLARED_ATTRIBUTE_ANY);
        }
        if (wildcard.is_found) {
            next = sxml_exi_schema_production_at(grammars, grammar, wildcard.index)->next;
        }
        if (status == SXML_STATUS_OK && is_uri_wildcard) {
            qname.uri = uri.index;
            status = sxml_exi_encode_local_name(encoding, &(sxml_exi_string_key){.text = name.local_name, .uri = uri.index},
                                                &qname.local_name);
            if (status == SXML_STATUS_OK) {
                status = sxml_exi_write_attribute_prefix(encoding, &name, &qname);
            }
        } else if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_qname(encoding, &(sxml_exi_qname_write){.name = &name, .writes_uri_and_local_name = true},
                                          &qname);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_exi_encoder_move_to_schema_grammar(encoding, next);
    if (qname.uri == 1 && sxml_exi_texts_equal(name.local_name, SXML_EXI_ENCODER_TEXT("space"))) {
        sxml_exi_open_element *element = sxml_exi_innermost(encoding);
        if (sxml_exi_texts_equal(value, SXML_EXI_ENCODER_TEXT("preserve"))) {
            element->is_space_preserved = true;
        } else if (sxml_exi_texts_equal(value, SXML_EXI_ENCODER_TEXT("default"))) {
            element->is_space_preserved = false;
        }
    }
    return sxml_exi_encode_typed_value(
        encoding, &(sxml_exi_value_record){
                      .value = {.key = {.text = value, .uri = qname.uri, .local_name = qname.local_name}, .datatype = datatype},
                      .is_made = false});
}

/* An attribute node and its name, to sort (the oracle sorts attributes
   by local-name then URI for schema-informed grammars). */
typedef struct sxml_exi_sorted_attribute {
    uint32_t node_index;
    sxml_byte_view uri;
    sxml_byte_view local_name;
} sxml_exi_sorted_attribute;

static int sxml_exi_attribute_order(const sxml_exi_sorted_attribute *first, const sxml_exi_sorted_attribute *second) {
    const sxml_byte_view views[2][2] = {{first->local_name, first->uri}, {second->local_name, second->uri}};
    for (size_t part_index = 0; part_index < 2; part_index++) {
        const sxml_byte_view left = views[0][part_index];
        const sxml_byte_view right = views[1][part_index];
        const size_t common = left.byte_count < right.byte_count ? left.byte_count : right.byte_count;
        const int order = common == 0 ? 0 : memcmp(left.bytes, right.bytes, common);
        if (order != 0) {
            return order;
        }
        if (left.byte_count != right.byte_count) {
            return left.byte_count < right.byte_count ? -1 : 1;
        }
    }
    return 0;
}

/* One attribute of a schema-informed stream: by the grammar the element
   is in at that point. */
typedef struct sxml_exi_attribute_kind {
    bool is_type;
    bool is_nil;
} sxml_exi_attribute_kind;

static sxml_status sxml_exi_encode_schema_stream_attribute(sxml_exi_encoding *encoding, const sxml_exi_node_event *event,
                                                           sxml_exi_attribute_kind kind) {
    if (sxml_exi_encoder_schema_grammar(encoding) == SXML_EXI_NONE) {
        return sxml_exi_encode_attribute(encoding, event);
    }
    if (kind.is_type) {
        return sxml_exi_encode_schema_type(encoding, event);
    }
    if (kind.is_nil) {
        return sxml_exi_encode_schema_nil(encoding, event);
    }
    return sxml_exi_encode_schema_attribute(encoding, event);
}

/* The attributes of an element of a schema-informed stream: xsi:type,
   xsi:nil, then the others by local-name and URI. */
static sxml_status sxml_exi_encode_schema_attributes(sxml_exi_encoding *encoding, const sxml_exi_node_event *event) {
    const uint32_t attribute_count = event->description->attribute_count;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(encoding->allocator,
                                                       (sxml_array_allocation_request){.element_count = attribute_count,
                                                                                       .element_size = sizeof(sxml_exi_sorted_attribute),
                                                                                       .element_alignment = alignof(sxml_exi_sorted_attribute)},
                                                       &memory);
    sxml_exi_sorted_attribute *sorted = memory;
    size_t sorted_count = 0;
    for (uint32_t pass_index = 0; pass_index < 3 && status == SXML_STATUS_OK; pass_index++) {
        for (uint32_t attribute_index = 0; attribute_index < attribute_count && status == SXML_STATUS_OK; attribute_index++) {
            const sxml_node_reference attribute = {.tree = event->node.tree,
                                                   .node_index = {event->node.node_index.index + 1 + attribute_index}};
            sxml_node_description description = {0};
            status = sxml_node_describe(attribute, &description);
            sxml_exi_name_text name = {0};
            if (status == SXML_STATUS_OK) {
                status = sxml_exi_name_text_of(encoding, &description, &name);
            }
            if (status != SXML_STATUS_OK) {
                break;
            }
            const bool is_instance = sxml_exi_texts_equal(name.uri, SXML_EXI_ENCODER_TEXT("http://www.w3.org/2001/XMLSchema-instance"));
            const sxml_exi_attribute_kind kind = {
                .is_type = is_instance && sxml_exi_texts_equal(name.local_name, SXML_EXI_ENCODER_TEXT("type")),
                .is_nil = is_instance && sxml_exi_texts_equal(name.local_name, SXML_EXI_ENCODER_TEXT("nil"))};
            const uint32_t attribute_pass = kind.is_type ? 0U : kind.is_nil ? 1U : 2U;
            if (attribute_pass != pass_index) {
                continue;
            }
            if (pass_index < 2) {
                status = sxml_exi_encode_schema_stream_attribute(
                    encoding, &(sxml_exi_node_event){.node = attribute, .description = &description}, kind);
                continue;
            }
            /* Inserted after the equal ones: a stable order. */
            const sxml_exi_sorted_attribute entry = {.node_index = attribute.node_index.index,
                                                     .uri = name.uri,
                                                     .local_name = name.local_name};
            size_t position = sorted_count;
            while (position > 0 && sxml_exi_attribute_order(&sorted[position - 1], &entry) > 0) {
                sorted[position] = sorted[position - 1];
                position--;
            }
            sorted[position] = entry;
            sorted_count++;
        }
    }
    for (size_t sorted_index = 0; sorted_index < sorted_count && status == SXML_STATUS_OK; sorted_index++) {
        const sxml_node_reference attribute = {.tree = event->node.tree, .node_index = {sorted[sorted_index].node_index}};
        sxml_node_description description = {0};
        status = sxml_node_describe(attribute, &description);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_encode_schema_stream_attribute(
                encoding, &(sxml_exi_node_event){.node = attribute, .description = &description},
                (sxml_exi_attribute_kind){.is_type = false, .is_nil = false});
        }
    }
    sxml_allocator_release(encoding->allocator, sorted);
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

static bool sxml_exi_is_whitespace_only(sxml_byte_view text) {
    for (size_t byte_index = 0; byte_index < text.byte_count; byte_index++) {
        const uint8_t byte = text.bytes[byte_index];
        if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r') {
            return false;
        }
    }
    return true;
}

/* CH in a schema-informed grammar, as the oracle writes character data:
   the whiteSpace facet of the grammar's typed CH, if its first production
   is one, applied (unless xml:space="preserve" or Preserve.lexicalValues);
   without one, whitespace-only text dropped but between a start tag and
   its end (or a comment or processing instruction); then CH with a typed
   value when the text is one of the type, an untyped CH (declared, or
   undeclared to Element_{i,content2}) otherwise (8.5.4.1.3.1, 8.5.4.4.1). */
static sxml_status sxml_exi_encode_schema_characters(sxml_exi_encoding *encoding, sxml_byte_view text) {
    const uint32_t grammar = sxml_exi_encoder_schema_grammar(encoding);
    const sxml_exi_schema_grammars *grammars = &encoding->schema_grammars;
    sxml_exi_open_element *element = sxml_exi_innermost(encoding);
    const sxml_exi_schema_grammar *current = &grammars->grammars[grammar];
    const sxml_exi_schema_production *first =
        current->production_count > 0 ? &grammars->productions[current->first_production] : NULL;
    const bool has_typed_first =
        first != NULL && first->event_kind == SXML_EXI_EVENT_CHARACTERS && first->datatype != SXML_EXI_NONE;
    sxml_byte_view value = text;
    bool is_made = false;
    sxml_status status = SXML_STATUS_OK;
    const bool keeps_text = encoding->options.fidelity.preserves_lexical_values || element->is_space_preserved ||
                            (has_typed_first && grammars->datatypes[first->datatype].whitespace == SXML_EXI_WHITESPACE_PRESERVE);
    if (!keeps_text) {
        if (has_typed_first) {
            encoding->scratch_text.byte_count = 0;
            status = sxml_exi_apply_whitespace(
                (sxml_exi_whitespace_request){.whitespace = grammars->datatypes[first->datatype].whitespace, .text = text},
                &encoding->scratch_text);
            value = (sxml_byte_view){.bytes = encoding->scratch_text.bytes, .byte_count = encoding->scratch_text.byte_count};
            is_made = true;
        } else if (sxml_exi_is_whitespace_only(text) && !(encoding->is_first_child && encoding->is_before_end_or_markup)) {
            value.byte_count = 0;
        }
        if (status != SXML_STATUS_OK || value.byte_count == 0) {
            return status;
        }
    }
    const sxml_exi_lookup typed = sxml_exi_find_schema_production(
        encoding, &(sxml_exi_production_query){.grammar = grammar, .event_kind = SXML_EXI_EVENT_CHARACTERS, .is_typed = true});
    sxml_exi_value_record record = {
        .value = {.key = {.text = value, .uri = element->uri, .local_name = element->local_name}, .datatype = SXML_EXI_NONE},
        .is_made = is_made};
    sxml_exi_validity validity = {.is_valid = false};
    if (typed.is_found) {
        record.value.datatype = sxml_exi_schema_production_at(grammars, grammar, typed.index)->datatype;
        status = sxml_exi_is_value_of(encoding, &record.value, &validity);
    }
    uint32_t next = sxml_exi_encoder_content_of(encoding, grammar);
    if (status == SXML_STATUS_OK && typed.is_found && validity.is_valid) {
        next = sxml_exi_schema_production_at(grammars, grammar, typed.index)->next;
        status = sxml_exi_write_declared_code(encoding, typed.index);
    } else if (status == SXML_STATUS_OK) {
        record.value.datatype = SXML_EXI_NONE;
        const sxml_exi_lookup untyped = sxml_exi_find_schema_production(
            encoding, &(sxml_exi_production_query){.grammar = grammar, .event_kind = SXML_EXI_EVENT_CHARACTERS, .is_typed = false});
        if (untyped.is_found) {
            next = sxml_exi_schema_production_at(grammars, grammar, untyped.index)->next;
            status = sxml_exi_write_declared_code(encoding, untyped.index);
        } else if (sxml_exi_encoder_has_undeclared(
                       encoding, (sxml_exi_undeclared_query){.grammar = grammar, .undeclared = SXML_EXI_UNDECLARED_CHARACTERS})) {
            status = sxml_exi_write_undeclared_code(encoding, SXML_EXI_UNDECLARED_CHARACTERS);
        } else if (!element->is_space_preserved && encoding->options.is_strict &&
                   sxml_exi_is_whitespace_only(value)) {
            /* The oracle skips whitespace strict grammars cannot hold. */
            return SXML_STATUS_OK;
        } else {
            return SXML_EXI_ENCODER_FAIL(encoding, SXML_STATUS_DYNAMIC_ERROR, "characters the grammar has no production for");
        }
    }
    if (status == SXML_STATUS_OK) {
        sxml_exi_encoder_move_to_schema_grammar(encoding, next);
        status = sxml_exi_encode_typed_value(encoding, &record);
    }
    return status;
}

static sxml_status sxml_exi_encode_characters(sxml_exi_encoding *encoding, const sxml_exi_node_event *event) {
    sxml_exi_open_element *element = sxml_exi_innermost(encoding);
    if (element != NULL && sxml_exi_encoder_schema_grammar(encoding) != SXML_EXI_NONE) {
        sxml_byte_view text = {0};
        sxml_status status = sxml_node_get_content(event->node, &text);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_encode_schema_characters(encoding, text);
        }
        return status;
    }
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
    const uint32_t schema_grammar = sxml_exi_encoder_schema_grammar(encoding);
    if (status == SXML_STATUS_OK && schema_grammar != SXML_EXI_NONE) {
        /* 8.5.4.4.1: the third level; then Element_{i,content2}. */
        status = sxml_exi_write_schema_code(
            encoding, (sxml_exi_schema_match){.match_kind = is_comment ? SXML_EXI_SCHEMA_MATCH_COMMENT
                                                                       : SXML_EXI_SCHEMA_MATCH_PROCESSING_INSTRUCTION});
        if (status == SXML_STATUS_OK && sxml_exi_innermost(encoding) != NULL) {
            sxml_exi_encoder_move_to_schema_grammar(encoding, sxml_exi_encoder_content_of(encoding, schema_grammar));
        }
    } else if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_tail(encoding, is_comment ? SXML_EXI_EVENT_COMMENT : SXML_EXI_EVENT_PROCESSING_INSTRUCTION);
        if (status == SXML_STATUS_OK && sxml_exi_innermost(encoding) != NULL) {
            sxml_exi_encoder_move_to(encoding, SXML_EXI_NONTERMINAL_ELEMENT_CONTENT);
        }
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

/* ------------------------------------------------- Self-contained elements */

/* Whether the element is one the request writes self-contained. */
static bool sxml_exi_is_self_contained(const sxml_exi_encoding *encoding, const sxml_node_description *description) {
    if (!encoding->options.allows_self_contained) {
        return false;
    }
    const sxml_element_name_view names = encoding->request->self_contained_elements;
    for (size_t name_index = 0; name_index < names.element_name_count; name_index++) {
        if (names.element_names[name_index].index == description->node_name.index) {
            return true;
        }
    }
    return false;
}

/* The string table and grammars a stream starts with (Appendix D, 8.4). */
static sxml_status sxml_exi_encoder_start_state(sxml_exi_encoding *encoding) {
    const sxml_status status = sxml_exi_string_table_initialize(
        &encoding->table, &(sxml_exi_string_table_configuration){.allocator = encoding->allocator, .options = &encoding->options});
    if (status == SXML_STATUS_OK) {
        sxml_exi_grammars_initialize(&encoding->grammars,
                                     &(sxml_exi_grammars_configuration){.allocator = encoding->allocator,
                                                                        .options = &encoding->options,
                                                                        .maximum_learned_production_count = UINT32_MAX});
    }
    return status;
}

/* 8.4.3, SC Fragment: after the element's SE, SC (a second-level code of
   StartTagContent) and padding to a byte; the string table and grammars
   put aside for new ones; then, as EXIficient's EXIBodyEncoderInOrderSC
   writes it, the fragment SD, SE (qname) and, with Preserve.prefixes, a
   declaration of the element's own prefix. The element's own
   declarations, attributes and content follow in the fragment. */
static sxml_status sxml_exi_begin_self_contained(sxml_exi_encoding *encoding, const sxml_exi_node_event *event) {
    if (encoding->is_schema_informed) {
        return SXML_EXI_ENCODER_FAIL(encoding, SXML_STATUS_UNSUPPORTED_FEATURE,
                                     "self-contained elements with schema-informed grammars are not written");
    }
    sxml_status status = sxml_exi_write_tail(encoding, SXML_EXI_EVENT_SELF_CONTAINED);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_writer_align(&encoding->writer);
    }
    if (status == SXML_STATUS_OK && encoding->saved_state_count == encoding->saved_state_capacity) {
        void *saved_states = encoding->saved_states;
        status = sxml_allocator_reserve_array(
            encoding->allocator, &(sxml_array_reservation_request){.elements = &saved_states,
                                                                   .capacity = &encoding->saved_state_capacity,
                                                                   .element_count = encoding->saved_state_count,
                                                                   .required_count = encoding->saved_state_count + 1,
                                                                   .element_size = sizeof(sxml_exi_saved_state),
                                                                   .element_alignment = alignof(sxml_exi_saved_state),
                                                                   .minimum_capacity = 4});
        encoding->saved_states = saved_states;
    }
    if (status != SXML_STATUS_OK || encoding->saved_states == NULL) {
        return status == SXML_STATUS_OK ? SXML_STATUS_INVALID_ARGUMENT : status;
    }
    encoding->saved_states[encoding->saved_state_count++] = (sxml_exi_saved_state){
        .table = encoding->table,
        .grammars = encoding->grammars,
        .top_nonterminal = encoding->top_nonterminal,
        .element_base = encoding->element_base,
    };
    encoding->element_base = encoding->element_count;
    encoding->top_nonterminal = SXML_EXI_NONTERMINAL_FRAGMENT_CONTENT;
    status = sxml_exi_encoder_start_state(encoding);
    if (status != SXML_STATUS_OK) {
        /* Nothing new to release: the saved state is the current one. */
        encoding->saved_state_count--;
        encoding->table = encoding->saved_states[encoding->saved_state_count].table;
        encoding->element_base = encoding->saved_states[encoding->saved_state_count].element_base;
        return status;
    }
    status = sxml_exi_encode_start_element(encoding, event);
    if (status == SXML_STATUS_OK && encoding->options.fidelity.preserves_prefixes) {
        sxml_exi_name_text name = {0};
        status = sxml_exi_name_text_of(encoding, event->description, &name);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_encode_namespace(
                encoding, &(sxml_exi_namespace_write){.binding = {.uri = name.uri, .prefix = name.prefix},
                                                      .is_local_element_namespace = true});
        }
    }
    return status;
}

/* After the EE of a self-contained element: the fragment's ED, padding
   to a byte, and the stream around it back, whose element ends there
   without an EE of its own. */
static sxml_status sxml_exi_end_self_contained(sxml_exi_encoding *encoding) {
    sxml_status status = sxml_exi_write_tail(encoding, SXML_EXI_EVENT_END_DOCUMENT);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_writer_align(&encoding->writer);
    }
    sxml_exi_string_table_finalize(&encoding->table);
    sxml_exi_grammars_finalize(&encoding->grammars);
    const sxml_exi_saved_state *saved = &encoding->saved_states[--encoding->saved_state_count];
    encoding->table = saved->table;
    encoding->grammars = saved->grammars;
    encoding->top_nonterminal = saved->top_nonterminal;
    encoding->element_base = saved->element_base;
    encoding->element_count--;
    return status;
}

/* What a failure leaves inside self-contained elements: the saved tables
   and grammars (the current ones are the caller's to finalize). */
static void sxml_exi_release_saved_states(sxml_exi_encoding *encoding) {
    for (size_t saved_index = 0; saved_index < encoding->saved_state_count; saved_index++) {
        sxml_exi_string_table_finalize(&encoding->saved_states[saved_index].table);
        sxml_exi_grammars_finalize(&encoding->saved_states[saved_index].grammars);
    }
    encoding->saved_state_count = 0;
    sxml_allocator_release(encoding->allocator, encoding->saved_states);
    encoding->saved_states = NULL;
}

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
        status = sxml_exi_encoder_schema_grammar(encoding) != SXML_EXI_NONE
                     ? sxml_exi_encode_schema_start_element(encoding, &event)
                     : sxml_exi_encode_start_element(encoding, &event);
        if (status == SXML_STATUS_OK && sxml_exi_is_self_contained(encoding, &description)) {
            status = sxml_exi_begin_self_contained(encoding, &event);
        }
        if (status == SXML_STATUS_OK && encoding->options.fidelity.preserves_prefixes) {
            status = sxml_exi_encode_namespaces(encoding, &event);
        }
        if (status == SXML_STATUS_OK) {
            /* Canonical EXI 4.4: sorted with every grammar. */
            const bool sorts = encoding->is_schema_informed || encoding->request->canonicalization.is_canonical;
            status = sorts ? sxml_exi_encode_schema_attributes(encoding, &event)
                           : sxml_exi_encode_attributes(encoding, &event);
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
    if (encoding->is_schema_informed) {
        encoding->top_schema_grammar = encoding->options.is_fragment ? encoding->schema_grammars.fragment_content
                                                                     : encoding->schema_grammars.document_content;
    }
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
            encoding->is_first_child = !element->has_written_child;
            element->has_written_child = true;
            encoding->is_before_end_or_markup = !child_description.has_next_sibling;
            if (status == SXML_STATUS_OK && child_description.has_next_sibling) {
                sxml_node_description sibling = {0};
                status = sxml_node_describe((sxml_node_reference){.tree = root.tree, .node_index = child_description.next_sibling},
                                            &sibling);
                encoding->is_before_end_or_markup =
                    (sibling.node_kind == SXML_NODE_KIND_COMMENT && encoding->options.fidelity.preserves_comments) ||
                    (sibling.node_kind == SXML_NODE_KIND_PROCESSING_INSTRUCTION &&
                     encoding->options.fidelity.preserves_processing_instructions);
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_exi_encode_node(encoding, child);
            }
            continue;
        }
        status = sxml_exi_encoder_schema_grammar(encoding) != SXML_EXI_NONE
                     ? sxml_exi_encode_schema_end_element(encoding)
                     : sxml_exi_encode_learnable(encoding, SXML_EXI_EVENT_END_ELEMENT);
        encoding->element_count--;
        if (status == SXML_STATUS_OK && encoding->saved_state_count > 0 &&
            encoding->element_count == encoding->element_base) {
            status = sxml_exi_end_self_contained(encoding);
        }
    }
    if (status == SXML_STATUS_OK && encoding->is_schema_informed) {
        const sxml_exi_lookup end = sxml_exi_find_schema_production(
            encoding, &(sxml_exi_production_query){.grammar = encoding->top_schema_grammar, .event_kind = SXML_EXI_EVENT_END_DOCUMENT});
        status = end.is_found ? sxml_exi_write_declared_code(encoding, end.index)
                              : SXML_EXI_ENCODER_FAIL(encoding, SXML_STATUS_DYNAMIC_ERROR, "an end of document out of place");
    } else if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_tail(encoding, SXML_EXI_EVENT_END_DOCUMENT);
    }
    return status;
}

/* EXI 5: [cookie] 1 0, presence bit, final version 1 (0 0000), [options
   document (5.4)]. */
static sxml_status sxml_exi_write_header(sxml_exi_encoding *encoding) {
    sxml_status status = SXML_STATUS_OK;
    /* Canonical EXI 3: no cookie, and the options document unless
       omitOptionsDocument. */
    const sxml_exi_canonicalization *canonicalization = &encoding->request->canonicalization;
    const sxml_exi_header_options header_options_value =
        canonicalization->is_canonical
            ? (sxml_exi_header_options){.includes_options = !canonicalization->omits_options_document}
            : encoding->request->header_options;
    const sxml_exi_header_options *header_options = &header_options_value;
    if (header_options->includes_cookie) {
        for (size_t byte_index = 0; byte_index < 4 && status == SXML_STATUS_OK; byte_index++) {
            status = sxml_exi_write_bits(&encoding->writer,
                                         (sxml_exi_bits){.value = (uint8_t)"$EXI"[byte_index], .bit_count = 8});
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_bits(&encoding->writer, (sxml_exi_bits){.value = 0x2, .bit_count = 2});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_bits(&encoding->writer,
                                     (sxml_exi_bits){.value = header_options->includes_options ? 1U : 0U, .bit_count = 1});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_bits(&encoding->writer, (sxml_exi_bits){.value = 0, .bit_count = 5});
    }
    if (status == SXML_STATUS_OK && header_options->includes_options) {
        status = sxml_exi_write_options_document(
            &encoding->writer, &(sxml_exi_options_document){.options = &encoding->options,
                                                            .includes_schema_identifier =
                                                                header_options->includes_schema_identifier,
                                                            .is_schema_informed = encoding->is_schema_informed});
        if (status == SXML_STATUS_UNSUPPORTED_FEATURE) {
            status = SXML_EXI_ENCODER_FAIL(encoding, status,
                                           "a datatypeRepresentationMap is not written in the options document");
        } else if (status == SXML_STATUS_INVALID_ARGUMENT) {
            status = SXML_EXI_ENCODER_FAIL(encoding, status, "a schemaId to write for a schema the options do not name");
        }
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
    /* Canonical EXI 4.1: compression is computed as pre-compression. */
    if (request->canonicalization.is_canonical && encoding.options.alignment == SXML_EXI_ALIGNMENT_COMPRESSION) {
        encoding.options.alignment = SXML_EXI_ALIGNMENT_PRE_COMPRESSION;
    }
    encoding.is_channelized =
        options->alignment == SXML_EXI_ALIGNMENT_PRE_COMPRESSION || options->alignment == SXML_EXI_ALIGNMENT_COMPRESSION;
    encoding.is_compressed = options->alignment == SXML_EXI_ALIGNMENT_COMPRESSION;
    encoding.block_size = options->has_block_size ? options->block_size : SXML_EXI_DEFAULT_BLOCK_SIZE;
    if (status == SXML_STATUS_OK && encoding.block_size == 0) {
        status = SXML_EXI_ENCODER_FAIL(&encoding, SXML_STATUS_INVALID_ARGUMENT, "a blockSize of 0");
    }
    /* 8.5: the schema, or the built-in types only. */
    const sxml_exi_schema_identifier_kind identifier_kind = options->schema_identifier.schema_identifier_kind;
    if (status == SXML_STATUS_OK && request->schema == NULL && identifier_kind == SXML_EXI_SCHEMA_IDENTIFIER_TEXT) {
        status = SXML_EXI_ENCODER_FAIL(&encoding, SXML_STATUS_INVALID_ARGUMENT, "a schemaId without its schema");
    }
    encoding.is_schema_informed = identifier_kind != SXML_EXI_SCHEMA_IDENTIFIER_NONE &&
                                  (request->schema != NULL || identifier_kind == SXML_EXI_SCHEMA_IDENTIFIER_BUILT_IN_TYPES);
    const sxml_schema *schema = identifier_kind == SXML_EXI_SCHEMA_IDENTIFIER_BUILT_IN_TYPES ? NULL : request->schema;
    encoding.owned_text = (sxml_exi_buffer){.allocator = encoding.allocator};
    encoding.scratch_text = (sxml_exi_buffer){.allocator = encoding.allocator};
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
    bool has_schema_grammars = false;
    if (status == SXML_STATUS_OK && encoding.is_schema_informed) {
        status = sxml_exi_schema_grammars_build(
            &(sxml_exi_schema_build){.allocator = encoding.allocator, .options = options, .schema = schema},
            &(sxml_exi_schema_setup){.table = &encoding.table,
                                     .grammars = &encoding.schema_grammars,
                                     .failure_description = &encoding.failure_description});
        has_table = status == SXML_STATUS_OK;
        has_schema_grammars = has_table;
    } else if (status == SXML_STATUS_OK) {
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
        sxml_exi_release_saved_states(&encoding);
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
    if (has_schema_grammars) {
        sxml_exi_schema_grammars_finalize(&encoding.schema_grammars);
    }
    sxml_exi_buffer_release(&encoding.owned_text);
    sxml_exi_buffer_release(&encoding.scratch_text);
    sxml_allocator_release(encoding.allocator, encoding.elements);
    sxml_allocator_release(encoding.allocator, encoding.values);
    sxml_allocator_release(encoding.allocator, encoding.channels);
    sxml_exi_writer_release(&encoding.writer);
    sxml_exi_writer_release(&encoding.output);
    return status;
}
