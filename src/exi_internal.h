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
    /* Schema-informed (8.5): the grammar of the global element declared
       with this qname, the global type grammar of the type named by it
       (xsi:type, 8.5.4.4) and the datatype of the global attribute
       declared with it (AT (*), 8.5.4.1.3.2); SXML_EXI_NONE for none. */
    uint32_t schema_element_grammar;
    uint32_t type_grammar;
    uint32_t attribute_datatype;
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

/* The names a schema adds to the string table (7.3.1, Appendix D): per
   URI its local-names, as views borrowed for the table's initialization. */
typedef struct sxml_exi_schema_partition {
    sxml_byte_view uri;
    const sxml_byte_view *local_names;
    size_t local_name_count;
} sxml_exi_schema_partition;

typedef struct sxml_exi_schema_partition_view {
    const sxml_exi_schema_partition *partitions;
    size_t partition_count;
} sxml_exi_schema_partition_view;

/* What a table is made with. */
typedef struct sxml_exi_string_table_configuration {
    sxml_allocator allocator;
    const sxml_exi_options *options;
    /* Schema-informed: the XML Schema URI and its type names too
       (Appendix D.1, D.3). */
    bool is_schema_informed;
    /* Schema-informed: the schema's URIs and local-names, merged with the
       initial entries; the URIs that are not initial ones are appended in
       lexicographical order, every partition's local-names sorted (7.3.1,
       D.3). */
    sxml_exi_schema_partition_view schema_partitions;
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

/* ------------------------------------------------------------ Buffers */

/* Bytes that grow through allocator (typed values as text, 7). */
typedef struct sxml_exi_buffer {
    sxml_allocator allocator;
    uint8_t *bytes;
    size_t byte_count;
    size_t capacity;
} sxml_exi_buffer;

SXML_NODISCARD sxml_status sxml_exi_buffer_append(sxml_exi_buffer *buffer, sxml_byte_view bytes);
void sxml_exi_buffer_release(sxml_exi_buffer *buffer);

/* ---------------------------------------------------------- Datatypes */

/*
 * The EXI datatype representations a schema-informed grammar types values
 * with (EXI 7.1, 7.2, Table 7-1), as the oracle EXIficient 1.0.4 derives
 * them from XML Schema simple types (src/exi_schema_grammar.c).
 */
typedef enum sxml_exi_datatype_kind {
    /* 7.1.10, through the string table. */
    SXML_EXI_DATATYPE_STRING,
    /* 7.1.10.1: a string whose literals use a restricted character set. */
    SXML_EXI_DATATYPE_RESTRICTED_STRING,
    SXML_EXI_DATATYPE_BASE64_BINARY,
    SXML_EXI_DATATYPE_HEX_BINARY,
    /* 7.1.2: one bit, or two with pattern facets. */
    SXML_EXI_DATATYPE_BOOLEAN,
    SXML_EXI_DATATYPE_PATTERN_BOOLEAN,
    SXML_EXI_DATATYPE_DECIMAL,
    SXML_EXI_DATATYPE_FLOAT,
    /* 7.1.5: sign and magnitude, an Unsigned Integer, or an n-bit offset
       from the lower bound of a range of at most 4096 values. */
    SXML_EXI_DATATYPE_INTEGER,
    SXML_EXI_DATATYPE_UNSIGNED_INTEGER,
    SXML_EXI_DATATYPE_BOUNDED_INTEGER,
    SXML_EXI_DATATYPE_DATE_TIME,
    SXML_EXI_DATATYPE_LIST,
    SXML_EXI_DATATYPE_ENUMERATION,
} sxml_exi_datatype_kind;

/* The Date-Time variants (7.1.8, Table 7-4). */
typedef enum sxml_exi_date_time_kind {
    SXML_EXI_DATE_TIME_DATE_TIME,
    SXML_EXI_DATE_TIME_TIME,
    SXML_EXI_DATE_TIME_DATE,
    SXML_EXI_DATE_TIME_G_YEAR_MONTH,
    SXML_EXI_DATE_TIME_G_YEAR,
    SXML_EXI_DATE_TIME_G_MONTH_DAY,
    SXML_EXI_DATE_TIME_G_DAY,
    SXML_EXI_DATE_TIME_G_MONTH,
} sxml_exi_date_time_kind;

/* The whiteSpace facet (XSD 1.1 Part 2, 4.3.6) of a datatype. */
typedef enum sxml_exi_whitespace {
    SXML_EXI_WHITESPACE_PRESERVE,
    SXML_EXI_WHITESPACE_REPLACE,
    SXML_EXI_WHITESPACE_COLLAPSE,
} sxml_exi_whitespace;

/* The restricted character sets of Table 7-2 (Preserve.lexicalValues). */
typedef enum sxml_exi_lexical_set {
    SXML_EXI_LEXICAL_SET_NONE,
    SXML_EXI_LEXICAL_SET_BASE64_BINARY,
    SXML_EXI_LEXICAL_SET_HEX_BINARY,
    SXML_EXI_LEXICAL_SET_BOOLEAN,
    SXML_EXI_LEXICAL_SET_DATE_TIME,
    SXML_EXI_LEXICAL_SET_DECIMAL,
    SXML_EXI_LEXICAL_SET_DOUBLE,
    SXML_EXI_LEXICAL_SET_INTEGER,
} sxml_exi_lexical_set;

typedef struct sxml_exi_datatype {
    sxml_exi_datatype_kind datatype_kind;
    sxml_exi_date_time_kind date_time_kind;
    sxml_exi_whitespace whitespace;
    /* LIST: the datatype of the items; ENUMERATION: the datatype its
       values are of. */
    uint32_t base_datatype;
    /* BOUNDED_INTEGER: the bits of the offset, its lower bound (decimal
       text in the grammars' text store) and the number of values;
       ENUMERATION: the bits of the ordinal. */
    uint32_t bit_count;
    sxml_exi_text lower_bound;
    uint32_t value_count;
    /* ENUMERATION: value_count texts from first_value in the grammars'
       enumeration values (the canonical text of each value, as decoded);
       RESTRICTED_STRING: value_count code points from first_value in the
       grammars' characters, ascending. */
    size_t first_value;
} sxml_exi_datatype;

/* ---------------------------------------------- Schema-informed grammars */

/* The kinds of schema-informed grammars (8.5), named as the oracle names
   them: a start tag is a grammar Element_{i,j} with 0 <= j <= content
   (the first one is Element_{i,0}), an element grammar one with
   j > content or the copy Element_{i,content2} (8.5.4.4.1). */
typedef enum sxml_exi_grammar_kind {
    SXML_EXI_GRAMMAR_DOCUMENT_CONTENT,
    SXML_EXI_GRAMMAR_DOCUMENT_END,
    SXML_EXI_GRAMMAR_FRAGMENT_CONTENT,
    SXML_EXI_GRAMMAR_FIRST_START_TAG,
    SXML_EXI_GRAMMAR_START_TAG,
    SXML_EXI_GRAMMAR_ELEMENT,
} sxml_exi_grammar_kind;

/* A production with a one-part event code (8.5.4.3): its terminal, the
   qname of SE (qname) and AT (qname) or the URI of SE (uri : *) and
   AT (uri : *), the datatype of a typed AT or CH (SXML_EXI_NONE: an
   untyped CH of mixed content), the grammar it leads to (SXML_EXI_NONE
   after EE and ED) and, for SE (qname), the element declaration whose
   grammar the element is evaluated with. */
typedef struct sxml_exi_schema_production {
    sxml_exi_event_kind event_kind;
    uint32_t uri;
    uint32_t local_name;
    uint32_t datatype;
    uint32_t next;
    uint32_t element;
} sxml_exi_schema_production;

typedef struct sxml_exi_schema_grammar {
    sxml_exi_grammar_kind grammar_kind;
    uint32_t first_production;
    uint32_t production_count;
    /* Start tags: Element_{i,content2}, where undeclared SE, CH, ER, CM
       and PI lead; SXML_EXI_NONE for the others (they stay). */
    uint32_t element_content;
    /* Start tags: TypeEmpty (xsi:nil="true", 8.5.4.1.3); SXML_EXI_NONE
       when not made. */
    uint32_t type_empty;
    bool has_end_element;
    /* First start tags with strict: the AT (xsi:type) and AT (xsi:nil)
       productions of 8.5.4.4.2. */
    bool is_nillable;
    bool is_type_castable;
    /* The first AT (qname) production's event code and the number of
       them (AT (qname) [untyped value], 8.5.4.4.1). */
    uint32_t least_attribute;
    uint32_t attribute_count;
} sxml_exi_schema_grammar;

/* The productions with two- and three-part event codes that 8.5.4.4 and
   the fidelity options add to a schema-informed grammar, in order. */
typedef enum sxml_exi_undeclared {
    SXML_EXI_UNDECLARED_END_ELEMENT,
    SXML_EXI_UNDECLARED_TYPE,
    SXML_EXI_UNDECLARED_NIL,
    SXML_EXI_UNDECLARED_ATTRIBUTE_ANY,
    /* AT (qname) [untyped value] and AT (*) [untyped value], whose third
       part picks the attribute. */
    SXML_EXI_UNDECLARED_ATTRIBUTE_INVALID,
    SXML_EXI_UNDECLARED_NAMESPACE,
    SXML_EXI_UNDECLARED_SELF_CONTAINED,
    SXML_EXI_UNDECLARED_START_ELEMENT_ANY,
    SXML_EXI_UNDECLARED_CHARACTERS,
    SXML_EXI_UNDECLARED_ENTITY_REFERENCE,
    SXML_EXI_UNDECLARED_DOCTYPE,
} sxml_exi_undeclared;

enum {
    SXML_EXI_MAXIMUM_UNDECLARED = 12,
};

/* The second-level productions of a grammar, and whether a last
   second-level code leads to CM and PI (a third level). */
typedef struct sxml_exi_undeclared_list {
    sxml_exi_undeclared undeclared[SXML_EXI_MAXIMUM_UNDECLARED];
    uint32_t undeclared_count;
    bool has_third_level;
} sxml_exi_undeclared_list;

/* The schema-informed grammars of one stream. */
typedef struct sxml_exi_schema_grammars {
    sxml_allocator allocator;
    const sxml_exi_options *options;
    sxml_exi_schema_grammar *grammars;
    size_t grammar_count;
    size_t grammar_capacity;
    sxml_exi_schema_production *productions;
    size_t production_count;
    size_t production_capacity;
    sxml_exi_datatype *datatypes;
    size_t datatype_count;
    size_t datatype_capacity;
    /* Per element declaration of the schema: its grammar, a first start
       tag (SXML_EXI_NONE until made). */
    uint32_t *element_grammars;
    size_t element_declaration_count;
    sxml_exi_text *enumeration_values;
    size_t enumeration_value_count;
    size_t enumeration_value_capacity;
    uint32_t *characters;
    size_t character_count;
    size_t character_capacity;
    sxml_exi_buffer text;
    uint32_t document_content;
    uint32_t document_end;
    uint32_t fragment_content;
    /* The element grammar of only EE, the content of every TypeEmpty. */
    uint32_t empty_content;
    /* The datatypes of xsi:nil (Boolean) and of untyped values (String,
       for Preserve.lexicalValues). */
    uint32_t boolean_datatype;
    uint32_t string_datatype;
} sxml_exi_schema_grammars;

/* What the grammars are built from: the schema (NULL for the built-in
   XML Schema types only, 8.5.1), the options, and the string table the
   build initializes with the schema's names (7.3.1). */
typedef struct sxml_exi_schema_build {
    sxml_allocator allocator;
    const sxml_exi_options *options;
    const sxml_schema *schema;
} sxml_exi_schema_build;

/* Initializes table (schema-informed, with the schema's names) and builds
   the grammars; on failure both are finalized and *failure_description
   may name why. */
typedef struct sxml_exi_schema_setup {
    sxml_exi_string_table *table;
    sxml_exi_schema_grammars *grammars;
    sxml_byte_view *failure_description;
} sxml_exi_schema_setup;

SXML_NODISCARD sxml_status sxml_exi_schema_grammars_build(const sxml_exi_schema_build *build,
                                                          const sxml_exi_schema_setup *setup);
void sxml_exi_schema_grammars_finalize(sxml_exi_schema_grammars *grammars);

/* The second-level productions of grammar (8.5.4.4, pruned as 8.3 says),
   as the oracle numbers them. */
void sxml_exi_schema_undeclared_list(const sxml_exi_schema_grammars *grammars, uint32_t grammar,
                                     sxml_exi_undeclared_list *list);

/* Where an event is in a schema-informed grammar: a declared production
   (by its event code), an undeclared one, or CM or PI (third level). */
typedef enum sxml_exi_schema_match_kind {
    SXML_EXI_SCHEMA_MATCH_DECLARED,
    SXML_EXI_SCHEMA_MATCH_UNDECLARED,
    SXML_EXI_SCHEMA_MATCH_COMMENT,
    SXML_EXI_SCHEMA_MATCH_PROCESSING_INSTRUCTION,
} sxml_exi_schema_match_kind;

typedef struct sxml_exi_schema_match {
    sxml_exi_schema_match_kind match_kind;
    /* DECLARED: the production's event code; UNDECLARED: which. */
    uint32_t code;
    sxml_exi_undeclared undeclared;
    /* ATTRIBUTE_INVALID: the third part (an AT (qname) production's code
       less least_attribute, or attribute_count for AT (*)). */
    uint32_t attribute;
} sxml_exi_schema_match;

typedef struct sxml_exi_schema_code_write {
    uint32_t grammar;
    sxml_exi_schema_match match;
} sxml_exi_schema_code_write;

/* Writes the event code of match in grammar; DYNAMIC_ERROR when the
   grammar has no such production (strict, or pruned). */
SXML_NODISCARD sxml_status sxml_exi_schema_write_event_code(const sxml_exi_schema_grammars *grammars,
                                                            const sxml_exi_schema_code_write *write,
                                                            sxml_exi_bit_writer *writer);

typedef struct sxml_exi_schema_code_read {
    uint32_t grammar;
    sxml_exi_bit_reader *reader;
} sxml_exi_schema_code_read;

/* Reads an event code in grammar; MALFORMED_INPUT for one it lacks. */
SXML_NODISCARD sxml_status sxml_exi_schema_read_event_code(const sxml_exi_schema_grammars *grammars,
                                                           const sxml_exi_schema_code_read *read,
                                                           sxml_exi_schema_match *match);

/* Whether grammar has the undeclared production. */
typedef struct sxml_exi_undeclared_query {
    uint32_t grammar;
    sxml_exi_undeclared undeclared;
} sxml_exi_undeclared_query;

bool sxml_exi_schema_has_undeclared(const sxml_exi_schema_grammars *grammars, sxml_exi_undeclared_query query);

/* The production of grammar at event code (borrowed). */
static inline const sxml_exi_schema_production *sxml_exi_schema_production_at(const sxml_exi_schema_grammars *grammars,
                                                                             uint32_t grammar, uint32_t code) {
    return &grammars->productions[grammars->grammars[grammar].first_production + code];
}

/* --------------------------------------------------------- Typed values */

/* A typed value's text, checked and turned into its representation. */
typedef struct sxml_exi_value_text {
    uint32_t datatype;
    sxml_byte_view text;
} sxml_exi_value_text;

/*
 * Whether text is a value of a datatype of kind other than STRING,
 * RESTRICTED_STRING and LIST, as the oracle decides it (its lexical rules:
 * text trimmed, the representation's own range checks, no facets but the
 * bounds of a bounded integer and enumerations); writes it when writer is
 * not NULL.
 */
typedef struct sxml_exi_typed_write {
    const sxml_exi_schema_grammars *grammars;
    sxml_exi_bit_writer *writer;
} sxml_exi_typed_write;

/* Whether a value is one of its datatype. */
typedef struct sxml_exi_validity {
    bool is_valid;
} sxml_exi_validity;

SXML_NODISCARD sxml_status sxml_exi_write_typed_value(const sxml_exi_typed_write *write, sxml_exi_value_text value,
                                                      sxml_exi_validity *validity);

/* Reads a value of a datatype of kind other than STRING,
   RESTRICTED_STRING and LIST and appends its text (as the oracle writes
   it) to output. */
typedef struct sxml_exi_typed_read {
    const sxml_exi_schema_grammars *grammars;
    sxml_exi_bit_reader *reader;
    sxml_exi_buffer *output;
} sxml_exi_typed_read;

SXML_NODISCARD sxml_status sxml_exi_read_typed_value(const sxml_exi_typed_read *read, uint32_t datatype);

/* Appends to buffer the text the decoder writes for value (its
   canonical text in the oracle's form) when text is a value of the
   datatype (a kind other than STRING, RESTRICTED_STRING, LIST and
   ENUMERATION; is_valid false otherwise). For the values of
   enumerations (7.2). */
typedef struct sxml_exi_canonical_output {
    sxml_exi_buffer *buffer;
    bool is_valid;
} sxml_exi_canonical_output;

SXML_NODISCARD sxml_status sxml_exi_canonicalize_value(const sxml_exi_schema_grammars *grammars,
                                                       sxml_exi_value_text value, sxml_exi_canonical_output *output);

/* The bounds of an integer type (its effective minInclusive,
   minExclusive, maxInclusive and maxExclusive, empty when absent) and
   the representation they give (7.1.5): a range of at most 4096 values
   from lower_bound (its decimal text appended to the buffer), an
   Unsigned Integer for a lower bound of 0 or more, or sign and
   magnitude. */
typedef struct sxml_exi_integer_bounds {
    sxml_byte_view minimum_inclusive;
    sxml_byte_view minimum_exclusive;
    sxml_byte_view maximum_inclusive;
    sxml_byte_view maximum_exclusive;
} sxml_exi_integer_bounds;

typedef struct sxml_exi_integer_range {
    sxml_exi_datatype_kind datatype_kind;
    uint32_t value_count;
    sxml_exi_buffer *lower_bound;
} sxml_exi_integer_range;

SXML_NODISCARD sxml_status sxml_exi_integer_range_of(const sxml_exi_integer_bounds *bounds,
                                                     sxml_exi_integer_range *range);

/* The whiteSpace facet applied to text (XSD 1.1 Part 2, 4.3.6) into
   output; output empty for an empty result. */
typedef struct sxml_exi_whitespace_request {
    sxml_exi_whitespace whitespace;
    sxml_byte_view text;
} sxml_exi_whitespace_request;

SXML_NODISCARD sxml_status sxml_exi_apply_whitespace(sxml_exi_whitespace_request request, sxml_exi_buffer *output);

/* The restricted character set of a literal (7.1.10.1) or of
   Preserve.lexicalValues (Table 7-2): code points, ascending. */
typedef struct sxml_exi_character_set {
    const uint32_t *characters;
    size_t character_count;
} sxml_exi_character_set;

sxml_exi_character_set sxml_exi_lexical_character_set(sxml_exi_lexical_set lexical_set);
/* The Table 7-2 set of a datatype's values with Preserve.lexicalValues. */
sxml_exi_lexical_set sxml_exi_datatype_lexical_set(const sxml_exi_schema_grammars *grammars, uint32_t datatype);
/* The index of codepoint in set, or set.character_count when absent. */
uint32_t sxml_exi_character_index(sxml_exi_character_set set, uint32_t codepoint);

/* Splits text into list items at XML whitespace: *offset advances past
   the next item, written to *item; false when none is left. */
bool sxml_exi_next_list_item(sxml_byte_view text, size_t *offset, sxml_byte_view *item);

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

/* ------------------------------------------------------ Options document */

/* The options document of the header (EXI 5.4, Appendix C; exi_header.c):
   the options, written as EXIficient writes them (each that is not its
   default); schemaId when includes_schema_identifier too, which for an
   ABSENT schemaId says what the stream is: its text needs one, so a
   schema-informed stream without one is refused. */
typedef struct sxml_exi_options_document {
    const sxml_exi_options *options;
    bool includes_schema_identifier;
    bool is_schema_informed;
} sxml_exi_options_document;

SXML_NODISCARD sxml_status sxml_exi_write_options_document(sxml_exi_bit_writer *writer,
                                                           const sxml_exi_options_document *document);

/* Reads an options document into *options (from the defaults); the
   schemaId's text is appended to schema_identifier_text, for the caller
   to point options->schema_identifier.text at. */
typedef struct sxml_exi_options_document_read {
    sxml_exi_bit_reader *reader;
    sxml_exi_options *options;
    sxml_exi_buffer *schema_identifier_text;
    sxml_byte_view *failure_description;
} sxml_exi_options_document_read;

SXML_NODISCARD sxml_status sxml_exi_read_options_document(const sxml_exi_options_document_read *read);
