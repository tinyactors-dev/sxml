#pragma once

/*
 * EXI internals (include/sxml/exi.h; D333): the objects of the public API
 * and the parts of the codec both directions share. EXI 1.0 (Second
 * Edition) is cited as "EXI", by section.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/exi.h"
#include "sxml/memory.h"
#include "sxml/name.h"

/* The defaults of sxml_exi_limits fields that are 0. */
enum {
    SXML_EXI_DEFAULT_MAXIMUM_DEPTH = 4096,
    SXML_EXI_DEFAULT_MAXIMUM_LEARNED_PRODUCTION_COUNT = 1000000,
    /* EXI 5.4: blockSize defaults to 1,000,000. */
    SXML_EXI_DEFAULT_BLOCK_SIZE = 1000000,
};

/* No index: an unassigned compact identifier, an absent grammar. */
#define SXML_EXI_NONE UINT32_MAX

/* 1 GiB of decoded content, the default of maximum_content_byte_count. */
static const uint64_t sxml_exi_default_maximum_content_byte_count = UINT64_C(1073741824);

/* The limits with every 0 replaced by its default. */
sxml_exi_limits sxml_exi_limits_resolve(sxml_exi_limits limits);

/* ceil(log2(value)), the bits of an n-bit unsigned integer with value
   distinct values (EXI 6.2); 0 for 0 and 1. */
uint32_t sxml_exi_bit_width(uint64_t value);

/* --------------------------------------------------------- Bit streams */

/*
 * Reads an EXI stream (EXI 7.1): bits most significant first; with byte
 * alignment, n-bit unsigned integers take whole bytes, least significant
 * byte first (EXI 7.1.9). Never reads past input.
 */
typedef struct sxml_exi_bit_reader {
    sxml_byte_view input;
    size_t byte_position;
    /* Bits of input.bytes[byte_position] already read (0 to 7). */
    uint32_t bit_offset;
    bool is_byte_aligned;
} sxml_exi_bit_reader;

/* Reads bit_count (at most 32) raw bits, most significant first. */
typedef struct sxml_exi_bits_request {
    uint32_t bit_count;
} sxml_exi_bits_request;

SXML_NODISCARD sxml_status sxml_exi_read_bits(sxml_exi_bit_reader *reader, sxml_exi_bits_request request,
                                              uint32_t *value);
/* An n-bit unsigned integer (EXI 7.1.9), n at most 32. */
SXML_NODISCARD sxml_status sxml_exi_read_n_bit(sxml_exi_bit_reader *reader, sxml_exi_bits_request request,
                                               uint32_t *value);
/* An Unsigned Integer (EXI 7.1.6) that fits 64 bits; MALFORMED_INPUT for
   a larger one. */
SXML_NODISCARD sxml_status sxml_exi_read_unsigned(sxml_exi_bit_reader *reader, uint64_t *value);
/* A Boolean (EXI 7.1.2): a 1-bit unsigned integer. */
typedef struct sxml_exi_boolean {
    bool value;
} sxml_exi_boolean;

SXML_NODISCARD sxml_status sxml_exi_read_boolean(sxml_exi_bit_reader *reader, sxml_exi_boolean *boolean);
/* Skips to the next byte boundary (padding, EXI 5 and 8.4.3 SC). */
void sxml_exi_reader_align(sxml_exi_bit_reader *reader);

/* Writes an EXI stream into memory from allocator. */
typedef struct sxml_exi_bit_writer {
    sxml_allocator allocator;
    uint8_t *bytes;
    size_t byte_count;
    size_t capacity;
    /* Bits of the byte being filled (0 to 7), and those bits. */
    uint32_t pending_bit_count;
    uint32_t pending_bits;
    bool is_byte_aligned;
} sxml_exi_bit_writer;

/* bit_count bits of value (at most 32), most significant first. */
typedef struct sxml_exi_bits {
    uint32_t value;
    uint32_t bit_count;
} sxml_exi_bits;

SXML_NODISCARD sxml_status sxml_exi_write_bits(sxml_exi_bit_writer *writer, sxml_exi_bits bits);
SXML_NODISCARD sxml_status sxml_exi_write_n_bit(sxml_exi_bit_writer *writer, sxml_exi_bits bits);
SXML_NODISCARD sxml_status sxml_exi_write_unsigned(sxml_exi_bit_writer *writer, uint64_t value);
SXML_NODISCARD sxml_status sxml_exi_write_boolean(sxml_exi_bit_writer *writer, sxml_exi_boolean boolean);
/* Pads with zero bits to the next byte boundary. */
SXML_NODISCARD sxml_status sxml_exi_writer_align(sxml_exi_bit_writer *writer);
/* The whole bytes written (pending bits not included). */
sxml_byte_view sxml_exi_writer_bytes(const sxml_exi_bit_writer *writer);
void sxml_exi_writer_release(sxml_exi_bit_writer *writer);

/* ----------------------------------------------------------- Characters */

/* Reads one codepoint of well-formed UTF-8 at *offset and advances it. */
uint32_t sxml_exi_utf8_next(sxml_byte_view text, size_t *offset);
/* The codepoints of well-formed UTF-8. */
size_t sxml_exi_utf8_codepoint_count(sxml_byte_view text);

/* ----------------------------------------------------------- String table */

/* A string of the table: bytes in the table's text store, and its length
   in characters (EXI counts characters, 7.1.10). */
typedef struct sxml_exi_text {
    size_t offset;
    uint32_t byte_count;
    uint32_t codepoint_count;
} sxml_exi_text;

/* A local-name of a URI partition: the name, and what EXI keys by its
   qname (uri, local-name): its value partition and global element
   grammar (EXI 7.3.1, 8.4). */
typedef struct sxml_exi_local_name {
    sxml_exi_text text;
    /* The local value partition: compact identifier -> value entry, or
       SXML_EXI_NONE once unassigned (7.3.3). An array in the table's
       memory. */
    uint32_t *local_values;
    size_t local_value_count;
    size_t local_value_capacity;
    /* The global element grammar of this qname, SXML_EXI_NONE until made. */
    uint32_t element_grammar;
    /* The name pool identifier of the qname, 0 until interned. */
    sxml_name_identifier name;
    /* With pre-compression and compression: the value channel of this
       qname in the block numbered channel_block + 1 (0: none yet). */
    uint64_t channel_block;
    uint32_t channel_index;
} sxml_exi_local_name;

typedef struct sxml_exi_uri {
    sxml_exi_text text;
    /* Prefixes (compact-identifier partition, 7.3.2), as text. */
    sxml_exi_text *prefixes;
    size_t prefix_count;
    size_t prefix_capacity;
    sxml_exi_local_name *local_names;
    size_t local_name_count;
    size_t local_name_capacity;
    /* The name pool identifier of the URI, set once interned. */
    sxml_namespace_uri_identifier namespace_uri;
    bool has_namespace_uri;
} sxml_exi_uri;

/* A value of the value partitions (7.3.3). */
typedef struct sxml_exi_value {
    sxml_exi_text text;
    /* Its qname, whose local partition holds it, and its identifier
       there. */
    uint32_t uri;
    uint32_t local_name;
    uint32_t local_identifier;
} sxml_exi_value;

/* A slot of the table's hash index. */
typedef struct sxml_exi_hash_slot {
    uint64_t hash;
    /* Which kind of key, and its scope (a URI index, or URI and local-name
       for local values). */
    uint32_t key_kind;
    uint32_t scope_uri;
    uint32_t scope_local_name;
    sxml_exi_text text;
    /* What the key maps to; SXML_EXI_NONE for a value no longer held. */
    uint32_t target;
    bool is_used;
} sxml_exi_hash_slot;

typedef struct sxml_exi_string_table {
    sxml_allocator allocator;
    /* The text of every string, back to back (UTF-8). */
    uint8_t *text_bytes;
    size_t text_byte_count;
    size_t text_capacity;
    sxml_exi_uri *uris;
    size_t uri_count;
    size_t uri_capacity;
    sxml_exi_value *values;
    size_t value_count;
    size_t value_capacity;
    /* The global value partition: compact identifier -> value entry. */
    uint32_t *global_values;
    size_t global_value_count;
    size_t global_value_capacity;
    /* globalID (7.3.1): the identifier of the next global value. */
    uint32_t global_identifier;
    /* valuePartitionCapacity, valueMaxLength; UINT64_MAX for unbounded. */
    uint64_t value_partition_capacity;
    uint64_t value_maximum_length;
    sxml_exi_hash_slot *slots;
    size_t slot_count;
    size_t used_slot_count;
} sxml_exi_string_table;

/* What a table is made with. */
typedef struct sxml_exi_string_table_configuration {
    sxml_allocator allocator;
    const sxml_exi_options *options;
    /* Schema-informed: the XML Schema URI and its type names too
       (Appendix D.1, D.3). */
    bool is_schema_informed;
} sxml_exi_string_table_configuration;

/* Makes the table with its initial entries (Appendix D). */
SXML_NODISCARD sxml_status sxml_exi_string_table_initialize(sxml_exi_string_table *table,
                                                            const sxml_exi_string_table_configuration *configuration);
void sxml_exi_string_table_finalize(sxml_exi_string_table *table);

/* The bytes of a string of the table, valid until the table grows. */
sxml_byte_view sxml_exi_string_table_view(const sxml_exi_string_table *table, sxml_exi_text text);

/* What a lookup finds: is_found and, when found, the index. */
typedef struct sxml_exi_lookup {
    bool is_found;
    uint32_t index;
} sxml_exi_lookup;

/* A string to find or add within a scope. */
typedef struct sxml_exi_string_key {
    sxml_byte_view text;
    /* The URI partition of a prefix or local-name; the qname (uri,
       local_name) of a local value. */
    uint32_t uri;
    uint32_t local_name;
} sxml_exi_string_key;

SXML_NODISCARD sxml_status sxml_exi_find_uri(sxml_exi_string_table *table, const sxml_exi_string_key *key,
                                             sxml_exi_lookup *lookup);
SXML_NODISCARD sxml_status sxml_exi_add_uri(sxml_exi_string_table *table, const sxml_exi_string_key *key,
                                            uint32_t *uri);
SXML_NODISCARD sxml_status sxml_exi_find_prefix(sxml_exi_string_table *table, const sxml_exi_string_key *key,
                                                sxml_exi_lookup *lookup);
SXML_NODISCARD sxml_status sxml_exi_add_prefix(sxml_exi_string_table *table, const sxml_exi_string_key *key,
                                               uint32_t *prefix);
SXML_NODISCARD sxml_status sxml_exi_find_local_name(sxml_exi_string_table *table, const sxml_exi_string_key *key,
                                                    sxml_exi_lookup *lookup);
SXML_NODISCARD sxml_status sxml_exi_add_local_name(sxml_exi_string_table *table, const sxml_exi_string_key *key,
                                                   uint32_t *local_name);
/* A value in the local partition of key's qname (its local identifier). */
SXML_NODISCARD sxml_status sxml_exi_find_local_value(sxml_exi_string_table *table, const sxml_exi_string_key *key,
                                                     sxml_exi_lookup *lookup);
/* A value in the global partition (its global identifier). */
SXML_NODISCARD sxml_status sxml_exi_find_global_value(sxml_exi_string_table *table, const sxml_exi_string_key *key,
                                                      sxml_exi_lookup *lookup);
/* Adds a value missed in both partitions, if valuePartitionCapacity and
   valueMaxLength allow (7.3.3); key's qname is its local partition. */
SXML_NODISCARD sxml_status sxml_exi_add_value(sxml_exi_string_table *table, const sxml_exi_string_key *key);
/* The value entry a local or global compact identifier names;
   SXML_EXI_NONE when unassigned or out of range. */
typedef struct sxml_exi_value_reference {
    uint32_t uri;
    uint32_t local_name;
    uint32_t compact_identifier;
} sxml_exi_value_reference;
uint32_t sxml_exi_local_value_entry(const sxml_exi_string_table *table, sxml_exi_value_reference reference);
uint32_t sxml_exi_global_value_entry(const sxml_exi_string_table *table, uint32_t compact_identifier);

/* --------------------------------------------------------------- Grammars */

/* The terminal symbols of EXI grammars (EXI 4, 8.1). */
typedef enum sxml_exi_event_kind {
    SXML_EXI_EVENT_START_DOCUMENT,
    SXML_EXI_EVENT_END_DOCUMENT,
    /* SE (qname), SE (uri : *), SE (*). */
    SXML_EXI_EVENT_START_ELEMENT,
    SXML_EXI_EVENT_START_ELEMENT_URI,
    SXML_EXI_EVENT_START_ELEMENT_ANY,
    SXML_EXI_EVENT_END_ELEMENT,
    /* AT (qname), AT (uri : *), AT (*). */
    SXML_EXI_EVENT_ATTRIBUTE,
    SXML_EXI_EVENT_ATTRIBUTE_URI,
    SXML_EXI_EVENT_ATTRIBUTE_ANY,
    SXML_EXI_EVENT_CHARACTERS,
    SXML_EXI_EVENT_NAMESPACE,
    SXML_EXI_EVENT_COMMENT,
    SXML_EXI_EVENT_PROCESSING_INSTRUCTION,
    SXML_EXI_EVENT_DOCTYPE,
    SXML_EXI_EVENT_ENTITY_REFERENCE,
    SXML_EXI_EVENT_SELF_CONTAINED,
} sxml_exi_event_kind;

/* Where a production leads: the nonterminals of the built-in grammars
   (EXI 8.4), and the end of an element. */
typedef enum sxml_exi_nonterminal {
    SXML_EXI_NONTERMINAL_DOCUMENT_CONTENT,
    SXML_EXI_NONTERMINAL_DOCUMENT_END,
    SXML_EXI_NONTERMINAL_FRAGMENT_CONTENT,
    SXML_EXI_NONTERMINAL_START_TAG_CONTENT,
    SXML_EXI_NONTERMINAL_ELEMENT_CONTENT,
    SXML_EXI_NONTERMINAL_COUNT,
} sxml_exi_nonterminal;

/* A production with a learned or declared one-part event code: its
   terminal, and the qname of SE (qname) and AT (qname). */
typedef struct sxml_exi_production {
    sxml_exi_event_kind event_kind;
    uint32_t uri;
    uint32_t local_name;
} sxml_exi_production;

/* A production of the fixed tail of a nonterminal (8.4.1 to 8.4.3), with
   its event code after pruning (8.3): the first part relative to the
   nonterminal's learned productions, then up to two more parts. */
typedef struct sxml_exi_tail_production {
    sxml_exi_event_kind event_kind;
    uint32_t first_part;
    uint32_t second_part;
    uint32_t third_part;
    uint32_t part_count;
} sxml_exi_tail_production;

enum {
    SXML_EXI_MAXIMUM_TAIL_PRODUCTIONS = 12,
};

/* The tail of one nonterminal for given fidelity options, with the counts
   that size its event code parts. */
typedef struct sxml_exi_tail {
    sxml_exi_tail_production productions[SXML_EXI_MAXIMUM_TAIL_PRODUCTIONS];
    size_t production_count;
    /* Distinct first parts of the tail. */
    uint32_t first_part_count;
} sxml_exi_tail;

/* The learned productions of one nonterminal, event code 0 first. */
typedef struct sxml_exi_learned {
    sxml_exi_production *productions;
    size_t production_count;
    size_t production_capacity;
} sxml_exi_learned;

/* A built-in element grammar (8.4.3): its two nonterminals' learned
   productions. */
typedef struct sxml_exi_element_grammar {
    sxml_exi_learned start_tag_content;
    sxml_exi_learned element_content;
} sxml_exi_element_grammar;

/* The built-in grammars of one stream: the tails for its fidelity options,
   the fragment content's learned productions, and every element grammar. */
typedef struct sxml_exi_grammars {
    sxml_allocator allocator;
    sxml_exi_tail tails[SXML_EXI_NONTERMINAL_COUNT];
    sxml_exi_learned fragment_content;
    sxml_exi_element_grammar *element_grammars;
    size_t element_grammar_count;
    size_t element_grammar_capacity;
    size_t learned_production_count;
    size_t maximum_learned_production_count;
} sxml_exi_grammars;

typedef struct sxml_exi_grammars_configuration {
    sxml_allocator allocator;
    const sxml_exi_options *options;
    uint32_t maximum_learned_production_count;
} sxml_exi_grammars_configuration;

void sxml_exi_grammars_initialize(sxml_exi_grammars *grammars, const sxml_exi_grammars_configuration *configuration);
void sxml_exi_grammars_finalize(sxml_exi_grammars *grammars);

/* A new element grammar; its index to *element_grammar. */
SXML_NODISCARD sxml_status sxml_exi_grammars_add_element(sxml_exi_grammars *grammars, uint32_t *element_grammar);

/* Where an event is in a nonterminal: a learned production (by event
   code) or a tail production (by index in the tail). */
typedef struct sxml_exi_match {
    bool is_learned;
    uint32_t index;
} sxml_exi_match;

/* A nonterminal of a grammar: which one, and for the element
   nonterminals, of which element grammar. */
typedef struct sxml_exi_position {
    sxml_exi_nonterminal nonterminal;
    uint32_t element_grammar;
} sxml_exi_position;

/* The learned productions of position (NULL for the document
   nonterminals, which learn nothing). */
sxml_exi_learned *sxml_exi_grammars_learned(sxml_exi_grammars *grammars, sxml_exi_position position);

/* A production to learn at event code 0 of a position (8.4.2, 8.4.3). */
typedef struct sxml_exi_learning {
    sxml_exi_position position;
    sxml_exi_production production;
} sxml_exi_learning;

SXML_NODISCARD sxml_status sxml_exi_grammars_learn(sxml_exi_grammars *grammars, const sxml_exi_learning *learning);

/* Writes the event code of match in position. */
typedef struct sxml_exi_event_code_request {
    sxml_exi_position position;
    sxml_exi_match match;
} sxml_exi_event_code_request;

SXML_NODISCARD sxml_status sxml_exi_write_event_code(sxml_exi_grammars *grammars,
                                                     const sxml_exi_event_code_request *request,
                                                     sxml_exi_bit_writer *writer);

/* Reads an event code in position and writes the production it names. */
typedef struct sxml_exi_event_code_read {
    sxml_exi_position position;
    sxml_exi_bit_reader *reader;
} sxml_exi_event_code_read;

SXML_NODISCARD sxml_status sxml_exi_read_event_code(sxml_exi_grammars *grammars,
                                                    const sxml_exi_event_code_read *read, sxml_exi_match *match);

/* The terminal of a match, and the qname of a learned production. */
sxml_exi_production sxml_exi_match_production(sxml_exi_grammars *grammars, sxml_exi_position position,
                                              sxml_exi_match match);

/* ---------------------------------------------------------- The objects */

struct sxml_exi_decoder {
    sxml_exi_decoder_configuration configuration;
    /* The allocator of sxml_exi_decoder_create; null after initialize. */
    sxml_allocator owning_allocator;
};

struct sxml_exi_encoder {
    sxml_exi_encoder_configuration configuration;
    sxml_allocator owning_allocator;
};

struct sxml_exi_options_reader {
    sxml_exi_options_reader_configuration configuration;
    sxml_allocator owning_allocator;
};

/* The body codecs of exi_decoder.c and exi_encoder.c, called by exi.c. */
SXML_NODISCARD sxml_status sxml_exi_decode_stream(sxml_exi_decoder *decoder, const sxml_exi_decode_request *request,
                                                  sxml_tree_builder builder);
SXML_NODISCARD sxml_status sxml_exi_encode_stream(sxml_exi_encoder *encoder, const sxml_exi_encode_request *request,
                                                  sxml_output_stream output_stream);
