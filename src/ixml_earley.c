/*
 * The Earley parser of Invisible XML (src/ixml_internal.h; the Invisible
 * XML Specification, "Parsing", which names Earley's algorithm; Earley,
 * "An efficient context-free parsing algorithm", CACM 13(2), 1970), with
 * the refinements of D283 and D290 that keep its work and its memory in
 * proportion to the input for the grammars people write.
 *
 * Items are (dotted rule, origin) in an item set, 16 bytes in one array;
 * the sets are consecutive runs of it, numbered in input order (glossary
 * `item_set_number`), and an item's origin is the number of the set it
 * started in. A set is made only for a position some item reaches, so the
 * inside of a regular token (below) has none. Positions are byte offsets
 * in the UTF-8 input. Each item keeps the first way it was derived: its
 * predecessor (the item with the dot one symbol earlier) and, when the
 * symbol is a nonterminal, the completed item that derived it. These
 * first derivations were made from items that existed before, so
 * following them always ends: they are the parse tree read out. An item
 * derived a second way, from another predecessor or another child, is
 * marked; the parse is ambiguous when a marked item is in that tree or
 * more than one item completes the root. Together the items are the
 * `parse_forest` (glossary).
 *
 * Lookahead (glossary `lookahead_set`, `codepoint_class`): an item is made
 * only when its lookahead set meets the class of the next character (or
 * the end of the input). An item it leaves out could not be part of any
 * parse of the whole input, nor derive one that is: so the items that
 * read characters, the place a parse fails, the parse tree and its marks
 * are those of the parser without lookahead. Only the terminals a failed
 * parse expected depend on the items left out at the failure, so that one
 * set is made again without lookahead.
 *
 * Leo's items (glossary `leo_item`; Leo, "A general context-free parsing
 * algorithm running in linear time on every LR(k) grammar without using
 * lookahead", TCS 82, 1991): when a nonterminal B completes from origin
 * k, and exactly one item of set k waits for B as its last symbol, the
 * completion climbs the chain of such items up the right recursion. The
 * Leo item of (k, B) remembers the topmost item of that chain, so the
 * completion makes only that item; it is marked as made through the Leo
 * item, and the items between are made when the parse tree reaches it.
 * A chain item whose origin is its own set is not followed (it would be a
 * cycle of empty derivations), so chains go strictly back in the input.
 *
 * Regular tokens (glossary `regular_token`, `token_automaton`, D290):
 * predicting a nonterminal that is a token runs its automaton from the
 * current position instead of predicting its productions. Every end the
 * automaton reaches is a completed item of the nonterminal there, as
 * Earley's items would complete it (all ends, not the longest: iXML has
 * no longest match), unless the lookahead rules it out; an end reached
 * more than one way is marked ambiguous, as the items of the token's
 * rules would be. A completion at a later position waits in a heap until
 * the parser reaches that position. The parse, its tree, its XML and its
 * ambiguity are those without tokens; only a failure would report other
 * expected terminals (the token's insides have no items), so a parse that
 * fails with tokens is made again without them.
 *
 * Nullable nonterminals: a nonterminal completed in the set it started in
 * is recorded, so an item that comes to wait for it later in the same set
 * is advanced over it too.
 *
 * The parse tree (D290): hidden nonterminals other than the root, and
 * hidden terminals, have no nodes, and kept characters next to each other
 * are one `text_run`, so the tree holds what the XML shows.
 *
 * Every item made, every Leo item, every symbol of the parse tree, and
 * every run of a token's automaton, with one more for every
 * SXML_IXML_TOKEN_CHARACTERS_PER_STEP characters it reads, is one step of
 * the caller's step meter.
 *
 * Golden file: src/regex.c (a compiled program run by a loop with
 * explicit state in allocator memory, no recursion).
 */
#include "ixml_internal.h"

#include "memory_internal.h"

#include <stdlib.h>
#include <string.h>

/* How many steps are charged at once. */
#define SXML_IXML_STEP_BATCH 1024U

/* The characters a token's automaton reads for one step (D290): a
   character costs it a few instructions, an item some hundred. */
#define SXML_IXML_TOKEN_CHARACTERS_PER_STEP 16U

/* The flags in the top bits of an item's dotted rule. */
#define SXML_IXML_ITEM_AMBIGUOUS 0x80000000U
#define SXML_IXML_ITEM_THROUGH_LEO 0x40000000U
#define SXML_IXML_ITEM_RULE_MASK 0x3FFFFFFFU

/* The child of an item that a token's automaton completed (D290): not an
   item index, which stays below it. */
#define SXML_IXML_TOKEN_CHILD (SXML_IXML_NONE - 1U)

/* The codepoint classes kept for codepoints from 128 up, by codepoint
   modulo this count. */
#define SXML_IXML_CLASS_CACHE_COUNT 1024U

/* The bits of a 64-bit word. */
#define SXML_IXML_WORD_BIT_COUNT 64U

/* Items are kept in blocks of 2^14, which never move: an input of
   megabytes needs no copies of its item array, and memory grows by one
   block at a time. */
#define SXML_IXML_ITEM_BLOCK_BITS 14U
#define SXML_IXML_ITEM_BLOCK_SIZE ((size_t)1 << SXML_IXML_ITEM_BLOCK_BITS)

typedef struct earley_item {
    /* The dotted rule, with SXML_IXML_ITEM_AMBIGUOUS and
       SXML_IXML_ITEM_THROUGH_LEO. */
    uint32_t rule;
    /* The number of the set it started in. */
    uint32_t origin;
    /* The first derivation; SXML_IXML_NONE for a predicted item, and for a
       child that is a terminal or an insertion. Made through a Leo item:
       predecessor is the Leo item and child the completed item at the
       bottom of its chain. Completed by a token: no predecessor, child
       SXML_IXML_TOKEN_CHILD. */
    uint32_t predecessor;
    uint32_t child;
} earley_item;

/* An item set: where its items and waiting entries start. Its position
   is kept apart (set_positions), for the parse tree. */
typedef struct item_set {
    uint32_t first_item;
    uint32_t first_waiting;
} item_set;

/* An item of a set waiting for nonterminal, or a nonterminal completed
   in the set it started in. */
typedef struct waiting_entry {
    uint32_t nonterminal;
    uint32_t item;
} waiting_entry;

/* The Leo item of (set, nonterminal). */
typedef struct leo_entry {
    uint32_t set_number;
    uint32_t nonterminal;
    /* The one item of the set that waits for the nonterminal. */
    uint32_t waiting;
    /* The Leo item of that item's origin and nonterminal, or
       SXML_IXML_NONE at the top of the chain. */
    uint32_t next;
    /* The waiting item at the top of the chain. */
    uint32_t top;
} leo_entry;

/* A slot of the hash table of the current set: the key of its item,
   kept beside it so a probe reads no item, with SXML_IXML_ITEM_AMBIGUOUS
   once the item is marked. */
typedef struct hash_slot {
    uint32_t rule;
    uint32_t origin;
    uint32_t item;
} hash_slot;

/* A token's completion at a later position, waiting in the heap. */
typedef struct token_completion {
    uint32_t position;
    uint32_t origin;
    uint32_t rule;
    /* SXML_IXML_ITEM_AMBIGUOUS when the automaton reached the end more
       than one way. */
    uint32_t flags;
} token_completion;

/* A growable array of fixed-size elements, by its own fields. */
typedef struct growable {
    void *elements;
    size_t element_count;
    size_t capacity;
} growable;

typedef struct earley_parser {
    const sxml_ixml_grammar *grammar;
    const sxml_ixml_parse_tables *tables;
    const sxml_ixml_dotted_rule *rules;
    const sxml_ixml_production *productions;
    const sxml_ixml_nonterminal *nonterminals;
    sxml_byte_view input;
    sxml_allocator allocator;
    sxml_ixml_step_meter step_meter;
    /* The items: item_count of them in blocks of SXML_IXML_ITEM_BLOCK_SIZE
       (block pointers in item_blocks). */
    growable item_blocks; /* earley_item * */
    size_t item_count;
    growable sets;              /* item_set */
    growable set_positions;     /* uint32_t, by set number */
    growable waiting;           /* waiting_entry, by set */
    growable empty_completions; /* waiting_entry of the current set */
    /* The items of the current set whose next symbol is a terminal. */
    growable scanners; /* uint32_t */
    /* The set each nonterminal was last predicted in, plus one. */
    uint32_t *predicted_sets;
    /* The set each nonterminal last completed empty in, plus one: an item
       that comes to wait for it looks at the empty completions only
       then. */
    uint32_t *empty_sets;
    /* Open addressing over the items of the current set; a slot holding
       an item of an earlier set is free. */
    hash_slot *hash_slots;
    size_t hash_capacity;
    size_t current_set_count;
    /* The current set, its first item and its position. */
    uint32_t set_number;
    uint32_t set_first_item;
    uint32_t position;
    /* The Leo items found, and open addressing over them by set and
       nonterminal. */
    growable leo_entries; /* leo_entry */
    uint32_t *leo_slots;
    size_t leo_capacity;
    /* Tokens' completions at later positions: a heap by position. */
    growable completions; /* token_completion */
    /* Off while a set is made again for a failure's expected terminals. */
    bool uses_lookahead;
    /* Off when a parse that failed with tokens is made again (D290). */
    bool uses_tokens;
    /* Two class buffers of word_count words: the class of the character at
       the current set's position (or of the end), and of the one before
       while scanning it. */
    uint64_t *class_buffers;
    const uint64_t *set_class;
    /* The class of the end of the input. */
    uint64_t *end_class;
    /* Classes of codepoints from 128 up, by codepoint modulo
       SXML_IXML_CLASS_CACHE_COUNT. */
    uint32_t *cached_codepoints;
    uint64_t *cached_classes;
    uint64_t uncharged_steps;
} earley_parser;

/* The key and the first derivation of an item to add. */
typedef struct earley_addition {
    uint32_t rule;
    uint32_t origin;
    uint32_t predecessor;
    uint32_t child;
} earley_addition;

static earley_item *item_at(const earley_parser *parser, uint32_t item_index) {
    earley_item *const *blocks = (earley_item *const *)parser->item_blocks.elements;
    return &blocks[item_index >> SXML_IXML_ITEM_BLOCK_BITS]
                  [item_index & (SXML_IXML_ITEM_BLOCK_SIZE - 1)];
}

static uint32_t rule_of(const earley_item *item) {
    return item->rule & SXML_IXML_ITEM_RULE_MASK;
}

static const item_set *set_at(const earley_parser *parser, uint32_t set_number) {
    return &((const item_set *)parser->sets.elements)[set_number];
}

static uint32_t set_position(const earley_parser *parser, uint32_t set_number) {
    return ((const uint32_t *)parser->set_positions.elements)[set_number];
}

/* The end of the waiting entries of a closed set. */
static size_t waiting_end(const earley_parser *parser, uint32_t set_number) {
    return set_number + 1U < parser->sets.element_count ? set_at(parser, set_number + 1U)->first_waiting
                                                        : parser->waiting.element_count;
}

/* Makes room for one more element in array. */
typedef struct growth_request {
    growable *array;
    size_t element_size;
    size_t element_alignment;
} growth_request;

static sxml_status grow(const earley_parser *parser, growth_request request) {
    if (request.array->element_count < request.array->capacity) {
        return SXML_STATUS_OK;
    }
    return sxml_allocator_reserve_array(
        parser->allocator,
        &(sxml_array_reservation_request){.elements = &request.array->elements,
                                          .capacity = &request.array->capacity,
                                          .element_count = request.array->element_count,
                                          .required_count = request.array->element_count + 1,
                                          .element_size = request.element_size,
                                          .element_alignment = request.element_alignment,
                                          .minimum_capacity = 64});
}

static sxml_status push_index(earley_parser *parser, growable *array, uint32_t value) {
    const sxml_status status = grow(parser, (growth_request){.array = array,
                                                            .element_size = sizeof(uint32_t),
                                                            .element_alignment = _Alignof(uint32_t)});
    if (status == SXML_STATUS_OK) {
        ((uint32_t *)array->elements)[array->element_count] = value;
        array->element_count += 1;
    }
    return status;
}

static sxml_status push_entry(earley_parser *parser, growable *array, waiting_entry entry) {
    const sxml_status status =
        grow(parser, (growth_request){.array = array,
                                      .element_size = sizeof(waiting_entry),
                                      .element_alignment = _Alignof(waiting_entry)});
    if (status == SXML_STATUS_OK) {
        ((waiting_entry *)array->elements)[array->element_count] = entry;
        array->element_count += 1;
    }
    return status;
}

/* Makes room for one more item: a new block when the last is full.
   SXML_STATUS_LIMIT_EXCEEDED when the item indices are used up. */
static sxml_status make_item_room(earley_parser *parser) {
    if (parser->item_count >= SXML_IXML_TOKEN_CHILD - 1) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (parser->item_count < parser->item_blocks.element_count * SXML_IXML_ITEM_BLOCK_SIZE) {
        return SXML_STATUS_OK;
    }
    sxml_status status = grow(parser, (growth_request){.array = &parser->item_blocks,
                                                       .element_size = sizeof(earley_item *),
                                                       .element_alignment = _Alignof(earley_item *)});
    void *block = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            parser->allocator,
            (sxml_allocation_request){.byte_count = SXML_IXML_ITEM_BLOCK_SIZE * sizeof(earley_item),
                                      .alignment = _Alignof(earley_item)},
            &block);
    }
    if (status == SXML_STATUS_OK && parser->item_blocks.elements == NULL) {
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        ((earley_item **)parser->item_blocks.elements)[parser->item_blocks.element_count] = block;
        parser->item_blocks.element_count += 1;
    }
    return status;
}

static const waiting_entry *waiting_at(const earley_parser *parser, size_t entry_index) {
    return &((const waiting_entry *)parser->waiting.elements)[entry_index];
}

static sxml_status charge_steps(earley_parser *parser, uint64_t step_count) {
    parser->uncharged_steps += step_count;
    if (parser->uncharged_steps < SXML_IXML_STEP_BATCH || parser->step_meter.operations == NULL) {
        return SXML_STATUS_OK;
    }
    const uint64_t charged = parser->uncharged_steps;
    parser->uncharged_steps = 0;
    return parser->step_meter.operations->charge(parser->step_meter.state, charged);
}

/* ------------------------------------------------------ The input */

/* The character at position, which must be below the input's end. */
static sxml_xml_decoded_codepoint character_at(const earley_parser *parser, uint32_t position) {
    const uint8_t byte = parser->input.bytes[position];
    if (byte < 0x80U) {
        return (sxml_xml_decoded_codepoint){.codepoint = byte, .encoded_byte_count = 1};
    }
    return sxml_xml_decode_codepoint(parser->input, position);
}

/* The position of the character before position, which must be above
   0: UTF-8 continuation bytes are 10xxxxxx. */
static uint32_t previous_position(const earley_parser *parser, uint32_t position) {
    uint32_t previous = position - 1U;
    while (previous > 0 && (parser->input.bytes[previous] & 0xC0U) == 0x80U) {
        previous -= 1;
    }
    return previous;
}

/* ------------------------------------------------- Codepoint classes */

/* The class of codepoint: a table's or the cache's words, valid until the
   cache is next used. */
static const uint64_t *codepoint_class(earley_parser *parser, uint32_t codepoint) {
    const size_t word_count = parser->tables->word_count;
    if (codepoint < 128U) {
        return &parser->tables->ascii_classes[codepoint * word_count];
    }
    const size_t slot = codepoint % SXML_IXML_CLASS_CACHE_COUNT;
    uint64_t *cached = &parser->cached_classes[slot * word_count];
    if (parser->cached_codepoints[slot] != codepoint) {
        sxml_ixml_compute_codepoint_class(
            (sxml_ixml_class_request){.grammar = parser->grammar, .codepoint = codepoint}, cached);
        parser->cached_codepoints[slot] = codepoint;
    }
    return cached;
}

/* The class of the character at position, or of the end of the input. */
static const uint64_t *position_class(earley_parser *parser, uint32_t position) {
    if (position >= parser->input.byte_count) {
        return parser->end_class;
    }
    return codepoint_class(parser, character_at(parser, position).codepoint);
}

/* True when the lookahead set of rule meets class_words. */
typedef struct lookahead_test {
    uint32_t rule;
    const uint64_t *class_words;
} lookahead_test;

static bool lookahead_meets(const earley_parser *parser, lookahead_test test) {
    const size_t word_count = parser->tables->word_count;
    const uint64_t *lookahead =
        &parser->tables->lookahead_words[(size_t)parser->rules[test.rule].lookahead_set * word_count];
    for (size_t word_index = 0; word_index < word_count; word_index += 1) {
        if ((lookahead[word_index] & test.class_words[word_index]) != 0) {
            return true;
        }
    }
    return false;
}

/* True when the lookahead set of rule meets the class of the current
   set's position, or when lookahead is off. */
static bool lookahead_allows(const earley_parser *parser, uint32_t rule) {
    return !parser->uses_lookahead ||
           lookahead_meets(parser, (lookahead_test){.rule = rule, .class_words = parser->set_class});
}

/* ---------------------------------------------------------- Item sets */

/* Consecutive origins of one rule land in nearby slots, which keeps
   the probes of an ambiguous parse, with many origins, in cache. */
static size_t hash_key(uint32_t rule, uint32_t origin) {
    uint64_t hash = rule;
    hash = hash * 0x9E3779B97F4A7C15ULL + origin;
    return (size_t)(hash ^ (hash >> 29U));
}

static bool slot_is_free(uint32_t slot_item, uint32_t set_start) {
    return slot_item == SXML_IXML_NONE || slot_item < set_start;
}

/* Rebuilds the hash table for the current set at twice the capacity. */
static sxml_status grow_hash(earley_parser *parser) {
    const size_t capacity = parser->hash_capacity == 0 ? 64 : parser->hash_capacity * 2;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        parser->allocator,
        (sxml_array_allocation_request){.element_count = capacity,
                                        .element_size = sizeof(hash_slot),
                                        .element_alignment = _Alignof(hash_slot)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    hash_slot *slots = memory;
    memset(slots, 0xFF, capacity * sizeof(hash_slot));
    const size_t mask = capacity - 1;
    for (size_t item_index = parser->set_first_item; item_index < parser->item_count;
         item_index += 1) {
        const earley_item *item = item_at(parser, (uint32_t)item_index);
        size_t slot = hash_key(rule_of(item), item->origin) & mask;
        while (slots[slot].item != SXML_IXML_NONE) {
            slot = (slot + 1) & mask;
        }
        slots[slot] = (hash_slot){.rule = item->rule & (SXML_IXML_ITEM_RULE_MASK | SXML_IXML_ITEM_AMBIGUOUS),
                                  .origin = item->origin,
                                  .item = (uint32_t)item_index};
    }
    sxml_allocator_release(parser->allocator, parser->hash_slots);
    parser->hash_slots = slots;
    parser->hash_capacity = capacity;
    return SXML_STATUS_OK;
}

/* Adds an item to the current set unless the next character cannot
   continue it, or marks the item there ambiguous when addition derives it
   another way. flags are SXML_IXML_ITEM_THROUGH_LEO when addition's
   predecessor is a Leo item, and SXML_IXML_ITEM_AMBIGUOUS when addition
   is itself more than one derivation (a token's end). */
typedef struct item_request {
    earley_addition addition;
    uint32_t flags;
} item_request;

/* The slot of the key in the current set's table: its item's, or the
   free slot where it goes. */
static size_t find_slot(const earley_parser *parser, const earley_addition *addition) {
    const size_t mask = parser->hash_capacity - 1;
    const uint32_t set_start = parser->set_first_item;
    const hash_slot *slots = parser->hash_slots;
    size_t slot = hash_key(addition->rule, addition->origin) & mask;
    while (!slot_is_free(slots[slot].item, set_start) &&
           ((slots[slot].rule & SXML_IXML_ITEM_RULE_MASK) != addition->rule ||
            slots[slot].origin != addition->origin)) {
        slot = (slot + 1) & mask;
    }
    return slot;
}

static sxml_status add_item(earley_parser *parser, item_request request) {
    const earley_addition *addition = &request.addition;
    size_t slot = 0;
    if (parser->hash_capacity > 0) {
        slot = find_slot(parser, addition);
        hash_slot *found = &parser->hash_slots[slot];
        if (!slot_is_free(found->item, parser->set_first_item)) {
            /* Made before, so the lookahead allowed it. Once marked, the
               slot says so, and the item is not read again: an ambiguous
               grammar derives its items many times. */
            if ((found->rule & SXML_IXML_ITEM_AMBIGUOUS) != 0) {
                return SXML_STATUS_OK;
            }
            earley_item *existing = item_at(parser, found->item);
            if ((existing->rule & SXML_IXML_ITEM_THROUGH_LEO) !=
                    (request.flags & SXML_IXML_ITEM_THROUGH_LEO) ||
                existing->predecessor != addition->predecessor ||
                existing->child != addition->child ||
                (request.flags & SXML_IXML_ITEM_AMBIGUOUS) != 0) {
                existing->rule |= SXML_IXML_ITEM_AMBIGUOUS;
                found->rule |= SXML_IXML_ITEM_AMBIGUOUS;
            }
            return SXML_STATUS_OK;
        }
    }
    if (!lookahead_allows(parser, addition->rule)) {
        return SXML_STATUS_OK;
    }
    if ((parser->current_set_count + 1) * 2 > parser->hash_capacity) {
        const sxml_status status = grow_hash(parser);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        slot = find_slot(parser, addition);
    }
    sxml_status status = make_item_room(parser);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint32_t item_index = (uint32_t)parser->item_count;
    *item_at(parser, item_index) = (earley_item){.rule = addition->rule | request.flags,
                                                 .origin = addition->origin,
                                                 .predecessor = addition->predecessor,
                                                 .child = addition->child};
    parser->item_count += 1;
    parser->hash_slots[slot] =
        (hash_slot){.rule = addition->rule | (request.flags & SXML_IXML_ITEM_AMBIGUOUS),
                    .origin = addition->origin,
                    .item = item_index};
    parser->current_set_count += 1;
    return charge_steps(parser, 1);
}

static sxml_status add_plain_item(earley_parser *parser, earley_addition addition) {
    return add_item(parser, (item_request){.addition = addition, .flags = 0});
}

/* Starts a set at position: empty, with the class of its character. */
static sxml_status start_set(earley_parser *parser, uint32_t position) {
    if (parser->sets.element_count >= SXML_IXML_NONE - 1) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    parser->set_positions.element_count = parser->sets.element_count;
    sxml_status status = grow(parser, (growth_request){.array = &parser->sets,
                                                      .element_size = sizeof(item_set),
                                                      .element_alignment = _Alignof(item_set)});
    if (status == SXML_STATUS_OK) {
        status = push_index(parser, &parser->set_positions, position);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    ((item_set *)parser->sets.elements)[parser->sets.element_count] =
        (item_set){.first_item = (uint32_t)parser->item_count,
                   .first_waiting = (uint32_t)parser->waiting.element_count};
    parser->set_number = (uint32_t)parser->sets.element_count;
    parser->sets.element_count += 1;
    parser->set_first_item = (uint32_t)parser->item_count;
    parser->position = position;
    parser->current_set_count = 0;
    const size_t word_count = parser->tables->word_count;
    uint64_t *class_words = parser->set_class == parser->class_buffers
                                ? parser->class_buffers + word_count
                                : parser->class_buffers;
    memcpy(class_words, position_class(parser, position), word_count * sizeof(uint64_t));
    parser->set_class = class_words;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Tokens */

/* Restores the heap of completions upward from its last element, or
   downward from its first. */
static bool completion_precedes(const token_completion *left, const token_completion *right) {
    return left->position < right->position ||
           (left->position == right->position && left->origin < right->origin);
}

static sxml_status schedule_completion(earley_parser *parser, token_completion completion) {
    const sxml_status status =
        grow(parser, (growth_request){.array = &parser->completions,
                                      .element_size = sizeof(token_completion),
                                      .element_alignment = _Alignof(token_completion)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    token_completion *heap = parser->completions.elements;
    size_t place = parser->completions.element_count;
    parser->completions.element_count += 1;
    while (place > 0 && completion_precedes(&completion, &heap[(place - 1) / 2])) {
        heap[place] = heap[(place - 1) / 2];
        place = (place - 1) / 2;
    }
    heap[place] = completion;
    return SXML_STATUS_OK;
}

static token_completion take_completion(earley_parser *parser) {
    token_completion *heap = parser->completions.elements;
    const token_completion first = heap[0];
    parser->completions.element_count -= 1;
    const size_t count = parser->completions.element_count;
    if (count == 0) {
        return first;
    }
    const token_completion moving = heap[count];
    size_t place = 0;
    while (place * 2 + 1 < count) {
        size_t child = place * 2 + 1;
        if (child + 1 < count && completion_precedes(&heap[child + 1], &heap[child])) {
            child += 1;
        }
        if (!completion_precedes(&heap[child], &moving)) {
            break;
        }
        heap[place] = heap[child];
        place = child;
    }
    heap[place] = moving;
    return first;
}

static const sxml_ixml_token *token_at(const earley_parser *parser, uint32_t token_index) {
    return &((const sxml_ixml_token *)parser->tables->tokens.elements)[token_index];
}

/* The column of a character from 128 up in a token's automaton: the
   token's terminals that its class has. */
typedef struct column_request {
    const sxml_ixml_token *token;
    uint32_t codepoint;
} column_request;

static uint32_t token_column(earley_parser *parser, column_request request) {
    const uint64_t *class_words = codepoint_class(parser, request.codepoint);
    const uint32_t *terminals =
        (const uint32_t *)parser->tables->token_terminals.elements + request.token->first_terminal;
    uint32_t column = 0;
    for (uint32_t terminal_index = 0; terminal_index < request.token->terminal_count;
         terminal_index += 1) {
        const uint32_t distinct = terminals[terminal_index];
        if (((class_words[distinct / SXML_IXML_WORD_BIT_COUNT] >> (distinct % SXML_IXML_WORD_BIT_COUNT)) &
             1U) != 0) {
            column |= 1U << terminal_index;
        }
    }
    return column;
}

/* A token ends at position, reached way_count ways (1, or 2 for more). */
typedef struct token_end {
    uint32_t token_index;
    uint32_t position;
    uint32_t way_count;
} token_end;

static sxml_status complete_token(earley_parser *parser, token_end end) {
    const sxml_ixml_token *token = token_at(parser, end.token_index);
    const uint32_t flags = end.way_count > 1 ? SXML_IXML_ITEM_AMBIGUOUS : 0U;
    const size_t word_count = parser->tables->word_count;
    const uint64_t *follow =
        (const uint64_t *)parser->tables->token_follows.elements + (size_t)end.token_index * word_count;
    const uint64_t *next_class = position_class(parser, end.position);
    bool can_follow = false;
    for (size_t word_index = 0; !can_follow && word_index < word_count; word_index += 1) {
        can_follow = (follow[word_index] & next_class[word_index]) != 0;
    }
    if (!can_follow) {
        /* No use of the token goes on with the next character. */
        return SXML_STATUS_OK;
    }
    if (end.position == parser->position) {
        /* An empty match completes in the current set. */
        return add_item(parser, (item_request){.addition = {.rule = token->completed_rule,
                                                            .origin = parser->set_number,
                                                            .predecessor = SXML_IXML_NONE,
                                                            .child = SXML_IXML_TOKEN_CHILD},
                                               .flags = flags});
    }
    return schedule_completion(parser, (token_completion){.position = end.position,
                                                          .origin = parser->set_number,
                                                          .rule = token->completed_rule,
                                                          .flags = flags});
}

/* Runs the automaton of token token_index from the current position: a
   completion for every end (D290). */
static sxml_status run_token(earley_parser *parser, uint32_t token_index) {
    const sxml_ixml_parse_tables *tables = parser->tables;
    const sxml_ixml_token *token = token_at(parser, token_index);
    const uint16_t *transitions =
        (const uint16_t *)tables->token_transitions.elements + token->first_transition;
    const uint8_t *accepts = (const uint8_t *)tables->token_accepts.elements + token->first_state;
    const uint8_t *ascii_columns =
        (const uint8_t *)tables->token_ascii_columns.elements + (size_t)token_index * 128U;
    const sxml_byte_view input = parser->input;
    uint32_t state = 0;
    uint32_t position = parser->position;
    uint64_t read_count = 0;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK) {
        if (accepts[state] != 0) {
            status = complete_token(parser, (token_end){.token_index = token_index,
                                                        .position = position,
                                                        .way_count = accepts[state]});
        }
        if (status != SXML_STATUS_OK || position >= input.byte_count) {
            break;
        }
        const uint8_t byte = input.bytes[position];
        uint32_t column = 0;
        uint32_t length = 1;
        if (byte < 0x80U) {
            column = ascii_columns[byte];
        } else {
            const sxml_xml_decoded_codepoint decoded = sxml_xml_decode_codepoint(input, position);
            length = (uint32_t)decoded.encoded_byte_count;
            column = token_column(parser, (column_request){.token = token, .codepoint = decoded.codepoint});
        }
        const uint16_t next = transitions[((size_t)state << token->terminal_count) | column];
        if (next == SXML_IXML_TOKEN_DEAD) {
            break;
        }
        state = next;
        position += length;
        read_count += 1;
    }
    if (status == SXML_STATUS_OK) {
        status = charge_steps(parser, 1U + read_count / SXML_IXML_TOKEN_CHARACTERS_PER_STEP);
    }
    return status;
}

/* Adds the completions waiting for the current position. */
static sxml_status add_completions(earley_parser *parser) {
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && parser->completions.element_count > 0 &&
           ((const token_completion *)parser->completions.elements)[0].position == parser->position) {
        const token_completion completion = take_completion(parser);
        status = add_item(parser, (item_request){.addition = {.rule = completion.rule,
                                                              .origin = completion.origin,
                                                              .predecessor = SXML_IXML_NONE,
                                                              .child = SXML_IXML_TOKEN_CHILD},
                                                 .flags = completion.flags});
    }
    return status;
}

/* ------------------------------------------------------- Prediction */

/* Predicts nonterminal in the current set, once per set: its productions,
   or the ends of its token. */
static sxml_status predict(earley_parser *parser, uint32_t nonterminal) {
    if (parser->predicted_sets[nonterminal] == parser->set_number + 1) {
        return SXML_STATUS_OK;
    }
    parser->predicted_sets[nonterminal] = parser->set_number + 1;
    const uint32_t token_index = parser->tables->tokens_by_nonterminal[nonterminal];
    if (parser->uses_tokens && token_index != SXML_IXML_NONE) {
        return run_token(parser, token_index);
    }
    const sxml_ixml_nonterminal *definition = &parser->nonterminals[nonterminal];
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t production_index = 0;
         status == SXML_STATUS_OK && production_index < definition->production_count;
         production_index += 1) {
        status = add_plain_item(
            parser,
            (earley_addition){
                .rule = parser->tables->first_dotted_rules[definition->first_production + production_index],
                .origin = parser->set_number,
                .predecessor = SXML_IXML_NONE,
                .child = SXML_IXML_NONE});
    }
    return status;
}

/* The waiting item advanced over its nonterminal by completed. */
static sxml_status advance(earley_parser *parser, waiting_entry entry, uint32_t completed) {
    const earley_item waiting = *item_at(parser, entry.item);
    return add_plain_item(parser, (earley_addition){.rule = rule_of(&waiting) + 1,
                                                    .origin = waiting.origin,
                                                    .predecessor = entry.item,
                                                    .child = completed});
}

static int compare_waiting_entries(const void *left, const void *right) {
    const waiting_entry *left_entry = left;
    const waiting_entry *right_entry = right;
    if (left_entry->nonterminal != right_entry->nonterminal) {
        return left_entry->nonterminal < right_entry->nonterminal ? -1 : 1;
    }
    if (left_entry->item != right_entry->item) {
        return left_entry->item < right_entry->item ? -1 : 1;
    }
    return 0;
}

/* The first waiting entry of a closed set (its entries sorted) for
   nonterminal, or the set's end. */
typedef struct waiting_query {
    uint32_t set_number;
    uint32_t nonterminal;
} waiting_query;

static size_t first_waiting(const earley_parser *parser, waiting_query query) {
    size_t low = set_at(parser, query.set_number)->first_waiting;
    size_t high = waiting_end(parser, query.set_number);
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (waiting_at(parser, middle)->nonterminal < query.nonterminal) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

/* ---------------------------------------------------------- Leo items */

static size_t leo_hash(uint32_t set_number, uint32_t nonterminal) {
    return hash_key(set_number, nonterminal);
}

static const leo_entry *leo_at(const earley_parser *parser, uint32_t entry_index) {
    return &((const leo_entry *)parser->leo_entries.elements)[entry_index];
}

static sxml_status grow_leo_slots(earley_parser *parser) {
    const size_t capacity = parser->leo_capacity == 0 ? 64 : parser->leo_capacity * 2;
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate_array(
        parser->allocator,
        (sxml_array_allocation_request){.element_count = capacity,
                                        .element_size = sizeof(uint32_t),
                                        .element_alignment = _Alignof(uint32_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t *slots = memory;
    memset(slots, 0xFF, capacity * sizeof(uint32_t));
    const size_t mask = capacity - 1;
    for (uint32_t entry_index = 0; entry_index < parser->leo_entries.element_count;
         entry_index += 1) {
        const leo_entry *entry = leo_at(parser, entry_index);
        size_t slot = leo_hash(entry->set_number, entry->nonterminal) & mask;
        while (slots[slot] != SXML_IXML_NONE) {
            slot = (slot + 1) & mask;
        }
        slots[slot] = entry_index;
    }
    sxml_allocator_release(parser->allocator, parser->leo_slots);
    parser->leo_slots = slots;
    parser->leo_capacity = capacity;
    return SXML_STATUS_OK;
}

/* The Leo item of query already found, or SXML_IXML_NONE. */
static uint32_t find_leo(const earley_parser *parser, waiting_query query) {
    if (parser->leo_capacity == 0) {
        return SXML_IXML_NONE;
    }
    const size_t mask = parser->leo_capacity - 1;
    size_t slot = leo_hash(query.set_number, query.nonterminal) & mask;
    while (parser->leo_slots[slot] != SXML_IXML_NONE) {
        const leo_entry *entry = leo_at(parser, parser->leo_slots[slot]);
        if (entry->set_number == query.set_number && entry->nonterminal == query.nonterminal) {
            return parser->leo_slots[slot];
        }
        slot = (slot + 1) & mask;
    }
    return SXML_IXML_NONE;
}

/* Records entry and writes its index to *entry_index. */
static sxml_status record_leo(earley_parser *parser, leo_entry entry, uint32_t *entry_index) {
    if ((parser->leo_entries.element_count + 1) * 2 > parser->leo_capacity) {
        const sxml_status status = grow_leo_slots(parser);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    sxml_status status = grow(parser, (growth_request){.array = &parser->leo_entries,
                                                       .element_size = sizeof(leo_entry),
                                                       .element_alignment = _Alignof(leo_entry)});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *entry_index = (uint32_t)parser->leo_entries.element_count;
    ((leo_entry *)parser->leo_entries.elements)[*entry_index] = entry;
    parser->leo_entries.element_count += 1;
    const size_t mask = parser->leo_capacity - 1;
    size_t slot = leo_hash(entry.set_number, entry.nonterminal) & mask;
    while (parser->leo_slots[slot] != SXML_IXML_NONE) {
        slot = (slot + 1) & mask;
    }
    parser->leo_slots[slot] = *entry_index;
    return charge_steps(parser, 1);
}

/* The one item of set query.set_number that waits for query.nonterminal
   as its last symbol, starting earlier in the input; SXML_IXML_NONE when
   there is none or more than one waits for it (Leo's condition). first
   is the set's first waiting entry for the nonterminal (first_waiting). */
static uint32_t leo_candidate(const earley_parser *parser, waiting_query query, size_t first) {
    const size_t end = waiting_end(parser, query.set_number);
    if (first >= end || waiting_at(parser, first)->nonterminal != query.nonterminal ||
        (first + 1 < end && waiting_at(parser, first + 1)->nonterminal == query.nonterminal)) {
        return SXML_IXML_NONE;
    }
    const uint32_t waiting = waiting_at(parser, first)->item;
    const earley_item *item = item_at(parser, waiting);
    if (parser->rules[rule_of(item) + 1].kind != SXML_IXML_DOTTED_RULE_KIND_COMPLETE ||
        item->origin >= query.set_number) {
        return SXML_IXML_NONE;
    }
    return waiting;
}

/* One step down a Leo chain: the set and nonterminal that the waiting
   item of query's set waits in. */
static waiting_query leo_step(const earley_parser *parser, uint32_t waiting) {
    const earley_item *item = item_at(parser, waiting);
    return (waiting_query){
        .set_number = item->origin,
        .nonterminal = parser->productions[parser->rules[rule_of(item)].production].nonterminal};
}

/* The Leo item of query, which must have a candidate (leo_candidate),
   found once: down the chain to a set and nonterminal already known or
   without a candidate, then down again recording the Leo items of the
   steps in order, without recursion (D71). Each one's next is the one
   recorded after it, the last one's the item found at the bottom, and
   all share the top. Only Leo items are recorded; a set without one is
   found again by its waiting entries. */
static sxml_status leo_item(earley_parser *parser, waiting_query query, uint32_t *entry_index) {
    waiting_query current = query;
    uint32_t bottom = SXML_IXML_NONE;
    uint32_t last_waiting = SXML_IXML_NONE;
    size_t step_count = 0;
    for (;;) {
        bottom = find_leo(parser, current);
        if (bottom != SXML_IXML_NONE) {
            break;
        }
        const uint32_t waiting = leo_candidate(parser, current, first_waiting(parser, current));
        if (waiting == SXML_IXML_NONE) {
            break;
        }
        last_waiting = waiting;
        step_count += 1;
        current = leo_step(parser, waiting);
    }
    const uint32_t top = bottom != SXML_IXML_NONE ? leo_at(parser, bottom)->top : last_waiting;
    const uint32_t first_index = (uint32_t)parser->leo_entries.element_count;
    *entry_index = step_count == 0 ? bottom : first_index;
    current = query;
    sxml_status status = SXML_STATUS_OK;
    for (size_t step_index = 0; status == SXML_STATUS_OK && step_index < step_count; step_index += 1) {
        const uint32_t waiting = leo_candidate(parser, current, first_waiting(parser, current));
        uint32_t recorded = SXML_IXML_NONE;
        status = record_leo(parser,
                            (leo_entry){.set_number = current.set_number,
                                        .nonterminal = current.nonterminal,
                                        .waiting = waiting,
                                        .next = step_index + 1 < step_count
                                                    ? first_index + (uint32_t)step_index + 1U
                                                    : bottom,
                                        .top = top},
                            &recorded);
        current = leo_step(parser, waiting);
    }
    return status;
}

/* Advances the items of set origin (closed, its waiting entries sorted)
   that wait for the nonterminal completed derives, or the top of its Leo
   chain. */
typedef struct completion {
    uint32_t origin;
    uint32_t nonterminal;
    uint32_t completed;
} completion;

static sxml_status complete_from(earley_parser *parser, completion done) {
    const waiting_query query = {.set_number = done.origin, .nonterminal = done.nonterminal};
    const size_t first = first_waiting(parser, query);
    if (leo_candidate(parser, query, first) != SXML_IXML_NONE) {
        uint32_t leo = SXML_IXML_NONE;
        const sxml_status status = leo_item(parser, query, &leo);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const earley_item top = *item_at(parser, leo_at(parser, leo)->top);
        return add_item(parser, (item_request){.addition = {.rule = rule_of(&top) + 1,
                                                            .origin = top.origin,
                                                            .predecessor = leo,
                                                            .child = done.completed},
                                               .flags = SXML_IXML_ITEM_THROUGH_LEO});
    }
    sxml_status status = SXML_STATUS_OK;
    const size_t end = waiting_end(parser, done.origin);
    for (size_t entry_index = first; status == SXML_STATUS_OK && entry_index < end;
         entry_index += 1) {
        const waiting_entry waiting = *waiting_at(parser, entry_index);
        if (waiting.nonterminal != done.nonterminal) {
            break;
        }
        status = advance(parser, waiting, done.completed);
    }
    return status;
}

/* An item completed in the set it started in: advances the items that
   already wait for its nonterminal here; later ones see the record. */
static sxml_status complete_empty(earley_parser *parser, waiting_entry completed) {
    parser->empty_sets[completed.nonterminal] = parser->set_number + 1;
    sxml_status status = push_entry(parser, &parser->empty_completions, completed);
    const size_t entry_end = parser->waiting.element_count;
    for (size_t entry_index = set_at(parser, parser->set_number)->first_waiting;
         status == SXML_STATUS_OK && entry_index < entry_end; entry_index += 1) {
        const waiting_entry entry = *waiting_at(parser, entry_index);
        if (entry.nonterminal == completed.nonterminal) {
            status = advance(parser, entry, completed.item);
        }
    }
    return status;
}

/* An item waiting for nonterminal: records it, predicts the nonterminal,
   and advances it over the nonterminal's empty completions here. */
static sxml_status wait_for(earley_parser *parser, waiting_entry entry) {
    sxml_status status = push_entry(parser, &parser->waiting, entry);
    if (status == SXML_STATUS_OK) {
        status = predict(parser, entry.nonterminal);
    }
    if (parser->empty_sets[entry.nonterminal] != parser->set_number + 1) {
        return status;
    }
    const size_t completion_count = parser->empty_completions.element_count;
    for (size_t completion_index = 0; status == SXML_STATUS_OK && completion_index < completion_count;
         completion_index += 1) {
        const waiting_entry completion_entry =
            ((const waiting_entry *)parser->empty_completions.elements)[completion_index];
        if (completion_entry.nonterminal == entry.nonterminal) {
            status = advance(parser, entry, completion_entry.item);
        }
    }
    return status;
}

/* Processes the items of the current set until none is left: prediction,
   insertions and completion. Then sorts its waiting entries. */
static sxml_status close_set(earley_parser *parser) {
    const uint32_t set_number = parser->set_number;
    const size_t first_waiting_entry = set_at(parser, set_number)->first_waiting;
    parser->empty_completions.element_count = 0;
    parser->scanners.element_count = 0;
    sxml_status status = SXML_STATUS_OK;
    for (size_t cursor = parser->set_first_item; status == SXML_STATUS_OK && cursor < parser->item_count;
         cursor += 1) {
        const uint32_t item_index = (uint32_t)cursor;
        const earley_item item = *item_at(parser, item_index);
        const sxml_ixml_dotted_rule *rule = &parser->rules[rule_of(&item)];
        switch ((sxml_ixml_dotted_rule_kind)rule->kind) {
        case SXML_IXML_DOTTED_RULE_KIND_COMPLETE:
            if (item.origin != set_number) {
                status = complete_from(parser, (completion){.origin = item.origin,
                                                            .nonterminal = rule->target,
                                                            .completed = item_index});
            } else {
                status = complete_empty(
                    parser, (waiting_entry){.nonterminal = rule->target, .item = item_index});
            }
            break;
        case SXML_IXML_DOTTED_RULE_KIND_NONTERMINAL:
            status = wait_for(parser, (waiting_entry){.nonterminal = rule->target, .item = item_index});
            break;
        case SXML_IXML_DOTTED_RULE_KIND_INSERTION:
            /* An insertion matches no characters (the Specification,
               "Insertions"). */
            status = add_plain_item(parser, (earley_addition){.rule = rule_of(&item) + 1,
                                                              .origin = item.origin,
                                                              .predecessor = item_index,
                                                              .child = SXML_IXML_NONE});
            break;
        case SXML_IXML_DOTTED_RULE_KIND_TERMINAL:
            status = push_index(parser, &parser->scanners, item_index);
            break;
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const size_t waiting_count = parser->waiting.element_count - first_waiting_entry;
    if (waiting_count < 2) {
        return SXML_STATUS_OK;
    }
    waiting_entry *entries = &((waiting_entry *)parser->waiting.elements)[first_waiting_entry];
    if (waiting_count > 16) {
        qsort(entries, waiting_count, sizeof(waiting_entry), compare_waiting_entries);
        return SXML_STATUS_OK;
    }
    /* Most sets have a few waiting entries: an insertion sort. */
    for (size_t sorted_count = 1; sorted_count < waiting_count; sorted_count += 1) {
        const waiting_entry moving = entries[sorted_count];
        size_t place = sorted_count;
        while (place > 0 && compare_waiting_entries(&entries[place - 1], &moving) > 0) {
            entries[place] = entries[place - 1];
            place -= 1;
        }
        entries[place] = moving;
    }
    return SXML_STATUS_OK;
}

static bool class_has(const uint64_t *class_words, uint32_t distinct_terminal) {
    return ((class_words[distinct_terminal / SXML_IXML_WORD_BIT_COUNT] >>
             (distinct_terminal % SXML_IXML_WORD_BIT_COUNT)) &
            1U) != 0;
}

/* The scanners of the current set that read its character. */
static size_t count_matches(const earley_parser *parser) {
    size_t matched_count = 0;
    const uint32_t *scanners = parser->scanners.elements;
    for (size_t scanner_index = 0; scanner_index < parser->scanners.element_count;
         scanner_index += 1) {
        const earley_item *item = item_at(parser, scanners[scanner_index]);
        if (class_has(parser->set_class, parser->rules[rule_of(item)].target)) {
            matched_count += 1;
        }
    }
    return matched_count;
}

/* Starts the next set at position: the items of the current one that
   read its character, when scans says position is the next character's,
   and the tokens' completions there. */
typedef struct next_set {
    uint32_t position;
    bool scans;
} next_set;

static sxml_status move_to(earley_parser *parser, next_set next) {
    const uint64_t *scanned_class = parser->set_class;
    sxml_status status = start_set(parser, next.position);
    if (status == SXML_STATUS_OK) {
        status = add_completions(parser);
    }
    const uint32_t *scanners = parser->scanners.elements;
    for (size_t scanner_index = 0;
         status == SXML_STATUS_OK && next.scans && scanner_index < parser->scanners.element_count;
         scanner_index += 1) {
        const uint32_t item_index = scanners[scanner_index];
        const earley_item item = *item_at(parser, item_index);
        if (!class_has(scanned_class, parser->rules[rule_of(&item)].target)) {
            continue;
        }
        status = add_plain_item(parser, (earley_addition){.rule = rule_of(&item) + 1,
                                                          .origin = item.origin,
                                                          .predecessor = item_index,
                                                          .child = SXML_IXML_NONE});
    }
    return status;
}

/* ---------------------------------------------------------- Failures */

/* Makes set set_number, the last, again without lookahead, from the set
   before it (or from the root at 0), so that it holds every item that
   could read the next character. The sets before it are kept: their
   items that read a character are those without lookahead. Only without
   tokens, so the set before is the previous character's. */
static sxml_status remake_set(earley_parser *parser, uint32_t set_number) {
    parser->uses_lookahead = false;
    const item_set remade = *set_at(parser, set_number);
    const uint32_t remade_position = set_position(parser, set_number);
    parser->item_count = remade.first_item;
    parser->waiting.element_count = remade.first_waiting;
    parser->sets.element_count = set_number;
    /* The slots may name items made again below. */
    if (parser->hash_slots != NULL) {
        memset(parser->hash_slots, 0xFF, parser->hash_capacity * sizeof(hash_slot));
    }
    for (size_t nonterminal_index = 0; nonterminal_index < parser->grammar->nonterminals.element_count;
         nonterminal_index += 1) {
        if (parser->predicted_sets[nonterminal_index] == set_number + 1) {
            parser->predicted_sets[nonterminal_index] = 0;
        }
        if (parser->empty_sets[nonterminal_index] == set_number + 1) {
            parser->empty_sets[nonterminal_index] = 0;
        }
    }
    sxml_status status = SXML_STATUS_OK;
    if (set_number == 0) {
        status = start_set(parser, 0);
        if (status == SXML_STATUS_OK) {
            status = predict(parser, 0);
        }
    } else {
        /* The class of the character before, which the items of its set
           read. */
        const size_t word_count = parser->tables->word_count;
        uint64_t *class_words = parser->set_class == parser->class_buffers
                                    ? parser->class_buffers + word_count
                                    : parser->class_buffers;
        memcpy(class_words, position_class(parser, set_position(parser, set_number - 1)),
               word_count * sizeof(uint64_t));
        const uint64_t *scanned_class = class_words;
        parser->set_class = class_words;
        status = start_set(parser, remade_position);
        for (uint32_t item_index = set_at(parser, set_number - 1)->first_item;
             status == SXML_STATUS_OK && item_index < remade.first_item; item_index += 1) {
            const earley_item item = *item_at(parser, item_index);
            const sxml_ixml_dotted_rule *rule = &parser->rules[rule_of(&item)];
            if (rule->kind != SXML_IXML_DOTTED_RULE_KIND_TERMINAL ||
                !class_has(scanned_class, rule->target)) {
                continue;
            }
            status = add_plain_item(parser, (earley_addition){.rule = rule_of(&item) + 1,
                                                              .origin = item.origin,
                                                              .predecessor = item_index,
                                                              .child = SXML_IXML_NONE});
        }
    }
    if (status == SXML_STATUS_OK) {
        status = close_set(parser);
    }
    return status;
}

/* Records where the parse failed and the terminals the current set,
   made again without lookahead, could have read there. */
static sxml_status record_failure(earley_parser *parser, sxml_ixml_parse_result *result) {
    result->failure_position = parser->position;
    sxml_status status = remake_set(parser, parser->set_number);
    const uint32_t *scanners = parser->scanners.elements;
    for (size_t scanner_index = 0;
         status == SXML_STATUS_OK && scanner_index < parser->scanners.element_count;
         scanner_index += 1) {
        const uint32_t distinct =
            parser->rules[rule_of(item_at(parser, scanners[scanner_index]))].target;
        const uint32_t terminal = parser->tables->representative_terminals[distinct];
        bool is_known = false;
        for (size_t known_index = 0;
             !is_known && known_index < result->expected_terminals.element_count;
             known_index += 1) {
            is_known = *(const uint32_t *)sxml_xml_array_at(&result->expected_terminals,
                                                            known_index) == terminal;
        }
        if (!is_known) {
            void *element = NULL;
            status = sxml_xml_array_push(&result->expected_terminals, &element);
            if (status == SXML_STATUS_OK) {
                *(uint32_t *)element = terminal;
            }
        }
    }
    return status;
}

/* --------------------------------------------------------- The tree */

/* Part of the tree still to read out: the items from item back to the
   start of its production, whose symbols become children of node; item
   ends at end_position. */
typedef struct expansion {
    uint32_t node;
    uint32_t item;
    uint32_t end_position;
} expansion;

/* The node node_index of result, to change it. */
static sxml_ixml_tree_node *node_to_change(sxml_ixml_parse_result *result, uint32_t node_index) {
    return &result->node_blocks[node_index >> SXML_IXML_NODE_BLOCK_BITS]
                               [node_index & (SXML_IXML_NODE_BLOCK_SIZE - 1)];
}

/* Appends node to the result's nodes, a new block when the last is
   full. */
static sxml_status push_node(sxml_ixml_parse_result *result, sxml_ixml_tree_node node,
                             uint32_t *node_index) {
    if (result->node_count >= SXML_IXML_NONE - 1) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    if (result->node_count == result->node_block_count * SXML_IXML_NODE_BLOCK_SIZE) {
        void *blocks = (void *)result->node_blocks;
        sxml_status status = sxml_allocator_reserve_array(
            result->allocator,
            &(sxml_array_reservation_request){.elements = &blocks,
                                              .capacity = &result->node_block_capacity,
                                              .element_count = result->node_block_count,
                                              .required_count = result->node_block_count + 1,
                                              .element_size = sizeof(sxml_ixml_tree_node *),
                                              .element_alignment = _Alignof(sxml_ixml_tree_node *),
                                              .minimum_capacity = 16});
        result->node_blocks = (sxml_ixml_tree_node **)blocks;
        void *block = NULL;
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                result->allocator,
                (sxml_allocation_request){
                    .byte_count = SXML_IXML_NODE_BLOCK_SIZE * sizeof(sxml_ixml_tree_node),
                    .alignment = _Alignof(sxml_ixml_tree_node)},
                &block);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        result->node_blocks[result->node_block_count] = block;
        result->node_block_count += 1;
    }
    *node_index = (uint32_t)result->node_count;
    *node_to_change(result, *node_index) = node;
    result->node_count += 1;
    return SXML_STATUS_OK;
}

/* Puts node first among the children of its parent. */
static sxml_status prepend_child(sxml_ixml_parse_result *result, sxml_ixml_tree_node node,
                                 uint32_t *node_index) {
    node.next_sibling = sxml_ixml_parse_result_node(result, node.parent)->first_child;
    const sxml_status status = push_node(result, node, node_index);
    if (status == SXML_STATUS_OK) {
        node_to_change(result, node.parent)->first_child = *node_index;
    }
    return status;
}

/* The characters from start_position to end_position, kept, first among
   the children of parent: joined to the text run there when it starts at
   end_position. */
typedef struct text_request {
    uint32_t parent;
    uint32_t start_position;
    uint32_t end_position;
} text_request;

static sxml_status prepend_text(sxml_ixml_parse_result *result, text_request request) {
    if (request.start_position == request.end_position) {
        return SXML_STATUS_OK;
    }
    const uint32_t first = sxml_ixml_parse_result_node(result, request.parent)->first_child;
    if (first != SXML_IXML_NONE) {
        sxml_ixml_tree_node *run = node_to_change(result, first);
        if (run->node_kind == SXML_IXML_NODE_KIND_TERMINAL && run->detail == request.end_position) {
            run->detail = request.start_position;
            run->byte_count += request.end_position - request.start_position;
            return SXML_STATUS_OK;
        }
    }
    uint32_t node_index = 0;
    return prepend_child(result,
                         (sxml_ixml_tree_node){.node_kind = SXML_IXML_NODE_KIND_TERMINAL,
                                               .symbol = SXML_IXML_NONE,
                                               .detail = request.start_position,
                                               .byte_count = request.end_position - request.start_position,
                                               .parent = request.parent,
                                               .first_child = SXML_IXML_NONE,
                                               .next_sibling = SXML_IXML_NONE},
                         &node_index);
}

/* Makes the items between an item made through a Leo item and the bottom
   of its chain, each derived from the one waiting item of its set and the
   item below, and gives the item its own first derivation (D283). */
static sxml_status expand_leo(earley_parser *parser, uint32_t item_index) {
    const earley_item item = *item_at(parser, item_index);
    uint32_t below = item.child;
    uint32_t entry_index = item.predecessor;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && leo_at(parser, entry_index)->next != SXML_IXML_NONE) {
        const leo_entry entry = *leo_at(parser, entry_index);
        const earley_item waiting = *item_at(parser, entry.waiting);
        status = make_item_room(parser);
        if (status != SXML_STATUS_OK) {
            break;
        }
        const uint32_t made = (uint32_t)parser->item_count;
        *item_at(parser, made) = (earley_item){.rule = rule_of(&waiting) + 1,
                                               .origin = waiting.origin,
                                               .predecessor = entry.waiting,
                                               .child = below};
        parser->item_count += 1;
        below = made;
        entry_index = entry.next;
        status = charge_steps(parser, 1);
    }
    if (status == SXML_STATUS_OK) {
        earley_item *top = item_at(parser, item_index);
        top->predecessor = leo_at(parser, entry_index)->waiting;
        top->child = below;
        top->rule &= ~SXML_IXML_ITEM_THROUGH_LEO;
    }
    return status;
}

/* The mark of a use of a nonterminal: the use's, else its rule's, else
   an element (the Specification, "Constructing XML"). */
static sxml_ixml_mark use_mark(const earley_parser *parser, const sxml_ixml_symbol *symbol) {
    sxml_ixml_mark mark = (sxml_ixml_mark)symbol->mark;
    if (mark == SXML_IXML_MARK_NONE) {
        mark = (sxml_ixml_mark)parser->nonterminals[symbol->target].mark;
    }
    return mark == SXML_IXML_MARK_NONE ? SXML_IXML_MARK_ELEMENT : mark;
}

/* The dot of item's dotted rule: how many symbols it has read. */
static uint32_t dot_of(const earley_parser *parser, uint32_t rule) {
    return rule - parser->tables->first_dotted_rules[parser->rules[rule].production];
}

/* The tree reader: the pending parts of the tree. */
typedef struct tree_reader {
    earley_parser *parser;
    sxml_ixml_parse_result *result;
    growable pending; /* expansion */
} tree_reader;

static sxml_status push_expansion(tree_reader *reader, expansion part) {
    const sxml_status status =
        grow(reader->parser, (growth_request){.array = &reader->pending,
                                              .element_size = sizeof(expansion),
                                              .element_alignment = _Alignof(expansion)});
    if (status == SXML_STATUS_OK) {
        ((expansion *)reader->pending.elements)[reader->pending.element_count] = part;
        reader->pending.element_count += 1;
    }
    return status;
}

/* Reads out one part: the symbols of its item's production, from the
   last read back, as children of its node. A hidden nonterminal's
   symbols go to the same node, before the symbols left of it, which wait
   as a part of their own. */
static sxml_status read_part(tree_reader *reader, expansion part) {
    earley_parser *parser = reader->parser;
    sxml_ixml_parse_result *result = reader->result;
    const sxml_ixml_symbol *symbols = (const sxml_ixml_symbol *)parser->grammar->symbols.elements;
    uint32_t current = part.item;
    uint32_t position = part.end_position;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK) {
        if ((item_at(parser, current)->rule & SXML_IXML_ITEM_THROUGH_LEO) != 0) {
            status = expand_leo(parser, current);
            if (status != SXML_STATUS_OK) {
                break;
            }
        }
        const earley_item item = *item_at(parser, current);
        if ((item.rule & SXML_IXML_ITEM_AMBIGUOUS) != 0) {
            result->is_ambiguous = true;
        }
        const uint32_t rule = rule_of(&item);
        if (item.child == SXML_IXML_TOKEN_CHILD) {
            /* A token's text, all of it (D290). */
            const uint32_t token_index =
                parser->tables->tokens_by_nonterminal[parser->rules[rule].target];
            status = charge_steps(parser, 1);
            if (status == SXML_STATUS_OK && token_at(parser, token_index)->keeps_text) {
                status = prepend_text(result, (text_request){.parent = part.node,
                                                             .start_position = set_position(parser, item.origin),
                                                             .end_position = position});
            }
            break;
        }
        const uint32_t dot = dot_of(parser, rule);
        if (dot == 0) {
            break;
        }
        const uint32_t production = parser->rules[rule].production;
        const uint32_t symbol_index = parser->productions[production].first_symbol + dot - 1;
        const sxml_ixml_symbol *symbol = &symbols[symbol_index];
        status = charge_steps(parser, 1);
        if (status != SXML_STATUS_OK) {
            break;
        }
        uint32_t node_index = 0;
        switch ((sxml_ixml_symbol_kind)symbol->symbol_kind) {
        case SXML_IXML_SYMBOL_KIND_TERMINAL: {
            const uint32_t start = previous_position(parser, position);
            if (symbol->mark != SXML_IXML_MARK_HIDDEN) {
                status = prepend_text(result, (text_request){
                                                  .parent = part.node, .start_position = start, .end_position = position});
            }
            position = start;
            break;
        }
        case SXML_IXML_SYMBOL_KIND_INSERTION:
            status = prepend_child(result,
                                   (sxml_ixml_tree_node){.node_kind = SXML_IXML_NODE_KIND_INSERTION,
                                                         .symbol = symbol_index,
                                                         .detail = 0,
                                                         .byte_count = 0,
                                                         .parent = part.node,
                                                         .first_child = SXML_IXML_NONE,
                                                         .next_sibling = SXML_IXML_NONE},
                                   &node_index);
            break;
        case SXML_IXML_SYMBOL_KIND_NONTERMINAL: {
            const uint32_t child_end = position;
            position = set_position(parser, item_at(parser, item.child)->origin);
            if (use_mark(parser, symbol) == SXML_IXML_MARK_HIDDEN) {
                /* Its children are this node's: read them first, then
                   what is left of this production. */
                if (dot_of(parser, rule_of(item_at(parser, item.predecessor))) > 0) {
                    status = push_expansion(reader, (expansion){.node = part.node,
                                                                .item = item.predecessor,
                                                                .end_position = position});
                }
                if (status == SXML_STATUS_OK) {
                    status = push_expansion(reader, (expansion){.node = part.node,
                                                                .item = item.child,
                                                                .end_position = child_end});
                }
                return status;
            }
            status = prepend_child(result,
                                   (sxml_ixml_tree_node){.node_kind = SXML_IXML_NODE_KIND_NONTERMINAL,
                                                         .symbol = symbol_index,
                                                         .detail = symbol->target,
                                                         .byte_count = 0,
                                                         .parent = part.node,
                                                         .first_child = SXML_IXML_NONE,
                                                         .next_sibling = SXML_IXML_NONE},
                                   &node_index);
            if (status == SXML_STATUS_OK) {
                status = push_expansion(reader, (expansion){.node = node_index,
                                                            .item = item.child,
                                                            .end_position = child_end});
            }
            break;
        }
        }
        current = item.predecessor;
    }
    return status;
}

/* Reads the parse tree out of the first derivations, from the completed
   root item root_item. */
static sxml_status build_tree(earley_parser *parser, uint32_t root_item,
                              sxml_ixml_parse_result *result) {
    tree_reader reader = {.parser = parser,
                          .result = result,
                          .pending = {.elements = NULL, .element_count = 0, .capacity = 0}};
    uint32_t root_node = 0;
    sxml_status status =
        push_node(result,
                  (sxml_ixml_tree_node){.node_kind = SXML_IXML_NODE_KIND_NONTERMINAL,
                                        .symbol = SXML_IXML_NONE,
                                        .detail = 0,
                                        .byte_count = 0,
                                        .parent = SXML_IXML_NONE,
                                        .first_child = SXML_IXML_NONE,
                                        .next_sibling = SXML_IXML_NONE},
                  &root_node);
    if (status == SXML_STATUS_OK) {
        status = push_expansion(&reader, (expansion){.node = root_node,
                                                     .item = root_item,
                                                     .end_position = (uint32_t)parser->input.byte_count});
    }
    while (status == SXML_STATUS_OK && reader.pending.element_count > 0) {
        reader.pending.element_count -= 1;
        status = read_part(&reader, ((const expansion *)reader.pending.elements)[reader.pending.element_count]);
    }
    sxml_allocator_release(parser->allocator, reader.pending.elements);
    return status;
}

/* ------------------------------------------------------ The parse */

/* Forgets every item, set and Leo item, to parse again. */
static void reset_parser(earley_parser *parser) {
    parser->item_count = 0;
    parser->sets.element_count = 0;
    parser->set_positions.element_count = 0;
    parser->waiting.element_count = 0;
    parser->leo_entries.element_count = 0;
    parser->completions.element_count = 0;
    if (parser->hash_slots != NULL) {
        memset(parser->hash_slots, 0xFF, parser->hash_capacity * sizeof(hash_slot));
    }
    if (parser->leo_slots != NULL) {
        memset(parser->leo_slots, 0xFF, parser->leo_capacity * sizeof(uint32_t));
    }
    const size_t nonterminal_count = parser->grammar->nonterminals.element_count;
    memset(parser->predicted_sets, 0, nonterminal_count * sizeof(uint32_t));
    memset(parser->empty_sets, 0, nonterminal_count * sizeof(uint32_t));
}

/* Releases what only making the sets needs, before the tree is read out
   of the items: the sets but their positions, the waiting entries, the
   hash tables, the heap. */
static void release_set_memory(earley_parser *parser) {
    const sxml_allocator allocator = parser->allocator;
    sxml_allocator_release(allocator, parser->waiting.elements);
    parser->waiting = (growable){.elements = NULL, .element_count = 0, .capacity = 0};
    sxml_allocator_release(allocator, parser->hash_slots);
    parser->hash_slots = NULL;
    parser->hash_capacity = 0;
    sxml_allocator_release(allocator, parser->leo_slots);
    parser->leo_slots = NULL;
    parser->leo_capacity = 0;
    sxml_allocator_release(allocator, parser->completions.elements);
    parser->completions = (growable){.elements = NULL, .element_count = 0, .capacity = 0};
    sxml_allocator_release(allocator, parser->scanners.elements);
    parser->scanners = (growable){.elements = NULL, .element_count = 0, .capacity = 0};
    sxml_allocator_release(allocator, parser->sets.elements);
    parser->sets = (growable){.elements = NULL, .element_count = 0, .capacity = 0};
}

/* Runs the sets over the whole input; *has_failed when a set has no item
   that reads its character and no token ends later, or the input ends
   with no root completed. */
static sxml_status run_sets(earley_parser *parser, uint32_t *root_item, bool *has_failed) {
    const uint32_t input_end = (uint32_t)parser->input.byte_count;
    *root_item = SXML_IXML_NONE;
    *has_failed = false;
    sxml_status status = start_set(parser, 0);
    if (status == SXML_STATUS_OK) {
        status = predict(parser, 0);
    }
    while (status == SXML_STATUS_OK) {
        status = close_set(parser);
        if (status != SXML_STATUS_OK || parser->position == input_end) {
            break;
        }
        const bool scans = count_matches(parser) > 0;
        if (!scans && parser->completions.element_count == 0) {
            /* No item read the character, and no token ends later. */
            *has_failed = true;
            return SXML_STATUS_OK;
        }
        const uint32_t next_position =
            scans ? parser->position + (uint32_t)character_at(parser, parser->position).encoded_byte_count
                  : ((const token_completion *)parser->completions.elements)[0].position;
        status = move_to(parser, (next_set){.position = next_position, .scans = scans});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    for (size_t item_index = parser->set_first_item; item_index < parser->item_count; item_index += 1) {
        const earley_item *item = item_at(parser, (uint32_t)item_index);
        const sxml_ixml_dotted_rule *rule = &parser->rules[rule_of(item)];
        if (item->origin != 0 || rule->kind != SXML_IXML_DOTTED_RULE_KIND_COMPLETE ||
            rule->target != 0) {
            continue;
        }
        if (*root_item == SXML_IXML_NONE) {
            *root_item = (uint32_t)item_index;
        } else {
            /* Two productions of the root derive the whole input:
               ambiguous, marked on the first. */
            item_at(parser, *root_item)->rule |= SXML_IXML_ITEM_AMBIGUOUS;
        }
    }
    *has_failed = *root_item == SXML_IXML_NONE;
    return SXML_STATUS_OK;
}

/* Parses the whole input and reads out the result: again without tokens
   when it fails with them (D290). */
static sxml_status run_parser(earley_parser *parser, sxml_ixml_parse_result *result) {
    uint32_t root_item = SXML_IXML_NONE;
    bool has_failed = false;
    sxml_status status = run_sets(parser, &root_item, &has_failed);
    if (status == SXML_STATUS_OK && has_failed && parser->tables->tokens.element_count > 0) {
        reset_parser(parser);
        parser->uses_tokens = false;
        status = run_sets(parser, &root_item, &has_failed);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (has_failed) {
        return record_failure(parser, result);
    }
    result->is_sentence = true;
    release_set_memory(parser);
    return build_tree(parser, root_item, result);
}

sxml_ixml_parse_result sxml_ixml_parse_result_empty(sxml_allocator allocator) {
    return (sxml_ixml_parse_result){
        .allocator = allocator,
        .is_sentence = false,
        .is_ambiguous = false,
        .node_blocks = NULL,
        .node_block_count = 0,
        .node_block_capacity = 0,
        .node_count = 0,
        .failure_position = 0,
        .expected_terminals = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = allocator, .element_byte_count = sizeof(uint32_t)})};
}

void sxml_ixml_parse_result_finalize(sxml_ixml_parse_result *result) {
    for (size_t block_index = 0; block_index < result->node_block_count; block_index += 1) {
        sxml_allocator_release(result->allocator, result->node_blocks[block_index]);
    }
    sxml_allocator_release(result->allocator, (void *)result->node_blocks);
    result->node_blocks = NULL;
    result->node_block_count = 0;
    result->node_block_capacity = 0;
    result->node_count = 0;
    sxml_xml_array_release(&result->expected_terminals);
}

/* Allocates count zeroed elements of size bytes. */
typedef struct zeroed_request {
    size_t count;
    size_t element_size;
} zeroed_request;

static sxml_status allocate_zeroed(sxml_allocator allocator, zeroed_request request, void **memory) {
    const sxml_status status = sxml_allocator_allocate_array(
        allocator,
        (sxml_array_allocation_request){.element_count = request.count,
                                        .element_size = request.element_size,
                                        .element_alignment = _Alignof(uint64_t)},
        memory);
    if (status == SXML_STATUS_OK && *memory != NULL) {
        memset(*memory, 0, request.count * request.element_size);
    }
    return status;
}

/* Allocates the parser's fixed arrays. */
static sxml_status allocate_parser(earley_parser *parser) {
    const size_t word_count = parser->tables->word_count;
    const size_t nonterminal_count = parser->grammar->nonterminals.element_count;
    void *memory = NULL;
    sxml_status status = allocate_zeroed(
        parser->allocator, (zeroed_request){.count = nonterminal_count, .element_size = sizeof(uint32_t)},
        &memory);
    parser->predicted_sets = memory;
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(parser->allocator,
                                 (zeroed_request){.count = nonterminal_count, .element_size = sizeof(uint32_t)},
                                 &memory);
        parser->empty_sets = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(
            parser->allocator, (zeroed_request){.count = 3 * word_count, .element_size = sizeof(uint64_t)},
            &memory);
        parser->class_buffers = memory;
        parser->end_class = parser->class_buffers == NULL ? NULL : parser->class_buffers + 2 * word_count;
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(
            parser->allocator,
            (zeroed_request){.count = SXML_IXML_CLASS_CACHE_COUNT, .element_size = sizeof(uint32_t)},
            &memory);
        parser->cached_codepoints = memory;
    }
    if (status == SXML_STATUS_OK) {
        /* Slot 0 holds no class yet, and no codepoint that falls in it is
           0 (those are ASCII). */
        status = allocate_zeroed(parser->allocator,
                                 (zeroed_request){.count = SXML_IXML_CLASS_CACHE_COUNT * word_count,
                                                  .element_size = sizeof(uint64_t)},
                                 &memory);
        parser->cached_classes = memory;
    }
    if (status == SXML_STATUS_OK && (parser->class_buffers == NULL || parser->cached_classes == NULL ||
                                     parser->predicted_sets == NULL || parser->empty_sets == NULL)) {
        /* Compiled tables have at least one word: the end of the input;
           compiled grammars at least one nonterminal. */
        status = SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status == SXML_STATUS_OK) {
        const size_t end_bit = parser->tables->distinct_terminal_count;
        parser->end_class[end_bit / SXML_IXML_WORD_BIT_COUNT] |= (uint64_t)1
                                                                 << (end_bit % SXML_IXML_WORD_BIT_COUNT);
    }
    return status;
}

static void release_parser(earley_parser *parser) {
    const sxml_allocator allocator = parser->allocator;
    sxml_allocator_release(allocator, parser->cached_classes);
    sxml_allocator_release(allocator, parser->cached_codepoints);
    sxml_allocator_release(allocator, parser->class_buffers);
    sxml_allocator_release(allocator, parser->completions.elements);
    sxml_allocator_release(allocator, parser->leo_slots);
    sxml_allocator_release(allocator, parser->leo_entries.elements);
    sxml_allocator_release(allocator, parser->hash_slots);
    sxml_allocator_release(allocator, parser->empty_sets);
    sxml_allocator_release(allocator, parser->predicted_sets);
    sxml_allocator_release(allocator, parser->sets.elements);
    sxml_allocator_release(allocator, parser->set_positions.elements);
    sxml_allocator_release(allocator, parser->scanners.elements);
    sxml_allocator_release(allocator, parser->empty_completions.elements);
    sxml_allocator_release(allocator, parser->waiting.elements);
    earley_item **blocks = (earley_item **)parser->item_blocks.elements;
    for (size_t block_index = 0; block_index < parser->item_blocks.element_count; block_index += 1) {
        sxml_allocator_release(allocator, blocks[block_index]);
    }
    sxml_allocator_release(allocator, parser->item_blocks.elements);
}

sxml_status sxml_ixml_parse(const sxml_ixml_parse_request *request,
                            sxml_ixml_parse_result *result) {
    const sxml_ixml_grammar *grammar = request->grammar;
    if (grammar->nonterminals.element_count == 0 || grammar->tables.dotted_rules == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (request->input.text.byte_count >= SXML_IXML_NONE - 2) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    earley_parser parser;
    memset(&parser, 0, sizeof(parser));
    parser.grammar = grammar;
    parser.tables = &grammar->tables;
    parser.rules = grammar->tables.dotted_rules;
    parser.productions = (const sxml_ixml_production *)grammar->productions.elements;
    parser.nonterminals = (const sxml_ixml_nonterminal *)grammar->nonterminals.elements;
    parser.input = request->input.text;
    parser.allocator = result->allocator;
    parser.step_meter = request->step_meter;
    parser.uses_lookahead = true;
    parser.uses_tokens = true;
    sxml_status status = allocate_parser(&parser);
    if (status == SXML_STATUS_OK) {
        status = run_parser(&parser, result);
    }
    release_parser(&parser);
    return status;
}
