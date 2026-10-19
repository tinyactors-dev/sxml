/*
 * The Earley parser of Invisible XML (src/ixml_internal.h; the Invisible
 * XML Specification, "Parsing", which names Earley's algorithm; Earley,
 * "An efficient context-free parsing algorithm", CACM 13(2), 1970).
 *
 * Items are (production, dot, origin) in the set of an input position.
 * Each item keeps the first way it was derived: its predecessor (the item
 * with the dot one symbol earlier) and, when the symbol is a nonterminal,
 * the completed item that derived it. These first derivations were made
 * from items that existed before, so following them always ends: they are
 * the parse tree read out. An item derived a second way, from another
 * predecessor or another child, is marked; the parse is ambiguous when a
 * marked item is in that tree or more than one item completes the root.
 * Together the items are the `parse_forest` (glossary).
 *
 * Nullable nonterminals: a nonterminal completed in the set it started in
 * is recorded, so an item that comes to wait for it later in the same set
 * is advanced over it too.
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

typedef struct earley_item {
    uint32_t production;
    uint32_t dot;
    uint32_t origin;
    /* The set the item is in. */
    uint32_t position;
    /* The first derivation; SXML_IXML_NONE for a predicted item, and for a
       child that is a terminal or an insertion. */
    uint32_t predecessor;
    uint32_t child;
    bool is_ambiguous;
} earley_item;

/* An item of a set waiting for nonterminal, or a nonterminal completed
   in the set it started in. */
typedef struct waiting_entry {
    uint32_t nonterminal;
    uint32_t item;
} waiting_entry;

typedef struct earley_parser {
    const sxml_ixml_grammar *grammar;
    sxml_ixml_codepoint_view input;
    sxml_allocator allocator;
    sxml_ixml_step_meter step_meter;
    sxml_xml_array items;       /* earley_item */
    uint32_t *set_starts;       /* input length + 2 */
    sxml_xml_array waiting;     /* waiting_entry, by set */
    uint32_t *waiting_starts;   /* input length + 2 */
    sxml_xml_array empty_completions; /* waiting_entry of the current set */
    /* The set each nonterminal was last predicted in, plus one. */
    uint32_t *predicted_positions;
    /* Open addressing over the items of the current set; a slot holding
       an item of another set is free. */
    uint32_t *hash_slots;
    size_t hash_capacity;
    size_t current_set_count;
    uint32_t position;
    uint64_t uncharged_steps;
} earley_parser;

/* The key and the first derivation of an item to add. */
typedef struct earley_addition {
    uint32_t production;
    uint32_t dot;
    uint32_t origin;
    uint32_t predecessor;
    uint32_t child;
} earley_addition;

static earley_item *item_at(const earley_parser *parser, uint32_t item_index) {
    return sxml_xml_array_at(&parser->items, item_index);
}

static const sxml_ixml_production *production_at(const earley_parser *parser,
                                                 uint32_t production) {
    return sxml_xml_array_at(&parser->grammar->productions, production);
}

static const sxml_ixml_symbol *symbol_at(const earley_parser *parser, uint32_t symbol) {
    return sxml_xml_array_at(&parser->grammar->symbols, symbol);
}

static const sxml_ixml_nonterminal *nonterminal_at(const earley_parser *parser,
                                                   uint32_t nonterminal) {
    return sxml_xml_array_at(&parser->grammar->nonterminals, nonterminal);
}

/* The symbol after the dot of item, or NULL when it is complete. */
static const sxml_ixml_symbol *next_symbol(const earley_parser *parser, const earley_item *item) {
    const sxml_ixml_production *production = production_at(parser, item->production);
    if (item->dot >= production->symbol_count) {
        return NULL;
    }
    return symbol_at(parser, production->first_symbol + item->dot);
}

static size_t hash_key(const earley_addition *addition) {
    uint64_t hash = addition->production;
    hash = hash * 0x9E3779B97F4A7C15ULL + addition->dot;
    hash = hash * 0x9E3779B97F4A7C15ULL + addition->origin;
    return (size_t)(hash ^ (hash >> 29U));
}

static bool slot_is_free(const earley_parser *parser, uint32_t slot) {
    return slot == SXML_IXML_NONE || item_at(parser, slot)->position != parser->position;
}

/* Rebuilds the hash table for the current set at twice the capacity. */
static sxml_status grow_hash(earley_parser *parser) {
    const size_t capacity = parser->hash_capacity == 0 ? 64 : parser->hash_capacity * 2;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
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
    const uint32_t set_start = parser->set_starts[parser->position];
    for (size_t item_index = set_start; item_index < parser->items.element_count;
         item_index += 1) {
        const earley_item *item = item_at(parser, (uint32_t)item_index);
        const earley_addition key = {
            .production = item->production, .dot = item->dot, .origin = item->origin};
        size_t slot = hash_key(&key) & mask;
        while (slots[slot] != SXML_IXML_NONE) {
            slot = (slot + 1) & mask;
        }
        slots[slot] = (uint32_t)item_index;
    }
    sxml_allocator_release(parser->allocator, parser->hash_slots);
    parser->hash_slots = slots;
    parser->hash_capacity = capacity;
    return SXML_STATUS_OK;
}

static sxml_status charge_step(earley_parser *parser) {
    parser->uncharged_steps += 1;
    if (parser->uncharged_steps < SXML_IXML_STEP_BATCH || parser->step_meter.operations == NULL) {
        return SXML_STATUS_OK;
    }
    const uint64_t step_count = parser->uncharged_steps;
    parser->uncharged_steps = 0;
    return parser->step_meter.operations->charge(parser->step_meter.state, step_count);
}

/* Adds an item to the current set, or marks the item there ambiguous when
   addition derives it another way. */
static sxml_status add_item(earley_parser *parser, const earley_addition *addition) {
    if ((parser->current_set_count + 1) * 2 > parser->hash_capacity) {
        const sxml_status status = grow_hash(parser);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    const size_t mask = parser->hash_capacity - 1;
    size_t slot = hash_key(addition) & mask;
    while (!slot_is_free(parser, parser->hash_slots[slot])) {
        earley_item *existing = item_at(parser, parser->hash_slots[slot]);
        if (existing->production == addition->production && existing->dot == addition->dot &&
            existing->origin == addition->origin) {
            if (existing->predecessor != addition->predecessor ||
                existing->child != addition->child) {
                existing->is_ambiguous = true;
            }
            return SXML_STATUS_OK;
        }
        slot = (slot + 1) & mask;
    }
    if (parser->items.element_count >= SXML_IXML_NONE - 1) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    void *element = NULL;
    sxml_status status = sxml_xml_array_push(&parser->items, &element);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    earley_item *item = element;
    *item = (earley_item){.production = addition->production,
                          .dot = addition->dot,
                          .origin = addition->origin,
                          .position = parser->position,
                          .predecessor = addition->predecessor,
                          .child = addition->child,
                          .is_ambiguous = false};
    parser->hash_slots[slot] = (uint32_t)(parser->items.element_count - 1);
    parser->current_set_count += 1;
    return charge_step(parser);
}

/* Starts the set of position: empty, with a fresh hash table. */
static void start_set(earley_parser *parser, uint32_t position) {
    parser->position = position;
    parser->set_starts[position] = (uint32_t)parser->items.element_count;
    parser->current_set_count = 0;
}

static sxml_status push_entry(sxml_xml_array *array, waiting_entry entry) {
    void *element = NULL;
    const sxml_status status = sxml_xml_array_push(array, &element);
    if (status == SXML_STATUS_OK) {
        *(waiting_entry *)element = entry;
    }
    return status;
}

/* Predicts nonterminal in the current set, once per set. */
static sxml_status predict(earley_parser *parser, uint32_t nonterminal) {
    if (parser->predicted_positions[nonterminal] == parser->position + 1) {
        return SXML_STATUS_OK;
    }
    parser->predicted_positions[nonterminal] = parser->position + 1;
    const sxml_ixml_nonterminal *definition = nonterminal_at(parser, nonterminal);
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t production_index = 0;
         status == SXML_STATUS_OK && production_index < definition->production_count;
         production_index += 1) {
        status = add_item(parser,
                          &(earley_addition){.production = definition->first_production +
                                                           production_index,
                                             .dot = 0,
                                             .origin = parser->position,
                                             .predecessor = SXML_IXML_NONE,
                                             .child = SXML_IXML_NONE});
    }
    return status;
}

/* The waiting item advanced over its nonterminal by completed. */
static sxml_status advance(earley_parser *parser, uint32_t waiting_item, uint32_t completed) {
    const earley_item waiting = *item_at(parser, waiting_item);
    return add_item(parser, &(earley_addition){.production = waiting.production,
                                               .dot = waiting.dot + 1,
                                               .origin = waiting.origin,
                                               .predecessor = waiting_item,
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

/* Advances the items of set origin (closed, its waiting entries sorted)
   that wait for the nonterminal completed derives. */
static sxml_status complete_from(earley_parser *parser, uint32_t origin, uint32_t completed) {
    const uint32_t nonterminal = production_at(parser, item_at(parser, completed)->production)
                                     ->nonterminal;
    size_t low = parser->waiting_starts[origin];
    size_t high = parser->waiting_starts[origin + 1];
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const waiting_entry *entry = sxml_xml_array_at(&parser->waiting, middle);
        if (entry->nonterminal < nonterminal) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    sxml_status status = SXML_STATUS_OK;
    for (size_t entry_index = low;
         status == SXML_STATUS_OK && entry_index < parser->waiting_starts[origin + 1];
         entry_index += 1) {
        const waiting_entry entry = *(const waiting_entry *)sxml_xml_array_at(&parser->waiting,
                                                                              entry_index);
        if (entry.nonterminal != nonterminal) {
            break;
        }
        status = advance(parser, entry.item, completed);
    }
    return status;
}

/* Processes the items of the current set until none is left: prediction,
   insertions and completion. Then sorts its waiting entries. */
static sxml_status close_set(earley_parser *parser) {
    const uint32_t position = parser->position;
    parser->waiting_starts[position] = (uint32_t)parser->waiting.element_count;
    parser->empty_completions.element_count = 0;
    sxml_status status = SXML_STATUS_OK;
    for (size_t cursor = parser->set_starts[position];
         status == SXML_STATUS_OK && cursor < parser->items.element_count; cursor += 1) {
        const uint32_t item_index = (uint32_t)cursor;
        const earley_item item = *item_at(parser, item_index);
        const sxml_ixml_symbol *symbol = next_symbol(parser, &item);
        if (symbol == NULL) {
            const uint32_t nonterminal = production_at(parser, item.production)->nonterminal;
            if (item.origin != position) {
                status = complete_from(parser, item.origin, item_index);
                continue;
            }
            /* Completed in the set it started in: advance the items that
               already wait for it here; later ones see the record. */
            status = push_entry(&parser->empty_completions,
                                (waiting_entry){.nonterminal = nonterminal, .item = item_index});
            const size_t waiting_end = parser->waiting.element_count;
            for (size_t entry_index = parser->waiting_starts[position];
                 status == SXML_STATUS_OK && entry_index < waiting_end; entry_index += 1) {
                const waiting_entry entry =
                    *(const waiting_entry *)sxml_xml_array_at(&parser->waiting, entry_index);
                if (entry.nonterminal == nonterminal) {
                    status = advance(parser, entry.item, item_index);
                }
            }
            continue;
        }
        switch ((sxml_ixml_symbol_kind)symbol->symbol_kind) {
        case SXML_IXML_SYMBOL_KIND_NONTERMINAL: {
            const uint32_t nonterminal = symbol->target;
            status = push_entry(&parser->waiting,
                                (waiting_entry){.nonterminal = nonterminal, .item = item_index});
            if (status == SXML_STATUS_OK) {
                status = predict(parser, nonterminal);
            }
            const size_t completion_count = parser->empty_completions.element_count;
            for (size_t completion_index = 0;
                 status == SXML_STATUS_OK && completion_index < completion_count;
                 completion_index += 1) {
                const waiting_entry completion = *(const waiting_entry *)sxml_xml_array_at(
                    &parser->empty_completions, completion_index);
                if (completion.nonterminal == nonterminal) {
                    status = advance(parser, item_index, completion.item);
                }
            }
            break;
        }
        case SXML_IXML_SYMBOL_KIND_INSERTION:
            /* An insertion matches no characters (the Specification,
               "Insertions"). */
            status = add_item(parser, &(earley_addition){.production = item.production,
                                                         .dot = item.dot + 1,
                                                         .origin = item.origin,
                                                         .predecessor = item_index,
                                                         .child = SXML_IXML_NONE});
            break;
        case SXML_IXML_SYMBOL_KIND_TERMINAL:
            break;
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    parser->waiting_starts[position + 1] = (uint32_t)parser->waiting.element_count;
    const size_t waiting_count =
        parser->waiting.element_count - parser->waiting_starts[position];
    if (waiting_count > 1) {
        qsort(sxml_xml_array_at(&parser->waiting, parser->waiting_starts[position]),
              waiting_count, sizeof(waiting_entry), compare_waiting_entries);
    }
    return SXML_STATUS_OK;
}

/* Starts the next set with the items of the current one that read the
   input character at the current position. */
static sxml_status scan(earley_parser *parser) {
    const uint32_t position = parser->position;
    const uint32_t codepoint = parser->input.codepoints[position];
    const uint32_t set_start = parser->set_starts[position];
    const uint32_t set_end = (uint32_t)parser->items.element_count;
    start_set(parser, position + 1);
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t item_index = set_start; status == SXML_STATUS_OK && item_index < set_end;
         item_index += 1) {
        const earley_item item = *item_at(parser, item_index);
        const sxml_ixml_symbol *symbol = next_symbol(parser, &item);
        if (symbol == NULL || symbol->symbol_kind != SXML_IXML_SYMBOL_KIND_TERMINAL ||
            !sxml_ixml_terminal_matches(
                (sxml_ixml_terminal_match){.grammar = parser->grammar,
                                           .terminal = symbol->target},
                codepoint)) {
            continue;
        }
        status = add_item(parser, &(earley_addition){.production = item.production,
                                                     .dot = item.dot + 1,
                                                     .origin = item.origin,
                                                     .predecessor = item_index,
                                                     .child = SXML_IXML_NONE});
    }
    return status;
}

/* Records where the parse failed and the terminals the set of position
   could have read there. */
static sxml_status record_failure(earley_parser *parser, uint32_t position,
                                  sxml_ixml_parse_result *result) {
    result->failure_position = position;
    const uint32_t set_end = parser->set_starts[position + 1];
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t item_index = parser->set_starts[position];
         status == SXML_STATUS_OK && item_index < set_end; item_index += 1) {
        const sxml_ixml_symbol *symbol = next_symbol(parser, item_at(parser, item_index));
        if (symbol == NULL || symbol->symbol_kind != SXML_IXML_SYMBOL_KIND_TERMINAL) {
            continue;
        }
        bool is_known = false;
        for (size_t known_index = 0;
             !is_known && known_index < result->expected_terminals.element_count;
             known_index += 1) {
            is_known = *(const uint32_t *)sxml_xml_array_at(&result->expected_terminals,
                                                            known_index) == symbol->target;
        }
        if (!is_known) {
            void *element = NULL;
            status = sxml_xml_array_push(&result->expected_terminals, &element);
            if (status == SXML_STATUS_OK) {
                *(uint32_t *)element = symbol->target;
            }
        }
    }
    return status;
}

/* A nonterminal node of the tree still to expand, and its item. */
typedef struct expansion {
    uint32_t node;
    uint32_t item;
} expansion;

static sxml_status push_node(sxml_ixml_parse_result *result, sxml_ixml_tree_node node,
                             uint32_t *node_index) {
    if (result->nodes.element_count >= SXML_IXML_NONE - 1) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    void *element = NULL;
    const sxml_status status = sxml_xml_array_push(&result->nodes, &element);
    if (status == SXML_STATUS_OK) {
        *(sxml_ixml_tree_node *)element = node;
        *node_index = (uint32_t)(result->nodes.element_count - 1);
    }
    return status;
}

/* Reads the parse tree out of the first derivations, from the completed
   root item root_item. */
static sxml_status build_tree(earley_parser *parser, uint32_t root_item,
                              sxml_ixml_parse_result *result) {
    sxml_xml_array pending = sxml_xml_array_empty((sxml_xml_array_configuration){
        .allocator = parser->allocator, .element_byte_count = sizeof(expansion)});
    uint32_t root_node = 0;
    sxml_status status =
        push_node(result,
                  (sxml_ixml_tree_node){.node_kind = SXML_IXML_NODE_KIND_NONTERMINAL,
                                        .symbol = SXML_IXML_NONE,
                                        .detail = 0,
                                        .parent = SXML_IXML_NONE,
                                        .first_child = SXML_IXML_NONE,
                                        .next_sibling = SXML_IXML_NONE},
                  &root_node);
    void *element = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_array_push(&pending, &element);
    }
    if (status == SXML_STATUS_OK) {
        *(expansion *)element = (expansion){.node = root_node, .item = root_item};
    }
    while (status == SXML_STATUS_OK && pending.element_count > 0) {
        const expansion next =
            *(const expansion *)sxml_xml_array_at(&pending, pending.element_count - 1);
        pending.element_count -= 1;
        uint32_t current = next.item;
        while (status == SXML_STATUS_OK) {
            const earley_item item = *item_at(parser, current);
            if (item.is_ambiguous) {
                result->is_ambiguous = true;
            }
            if (item.dot == 0) {
                break;
            }
            const uint32_t symbol_index = production_at(parser, item.production)->first_symbol +
                                          item.dot - 1;
            const sxml_ixml_symbol *symbol = symbol_at(parser, symbol_index);
            sxml_ixml_tree_node *parent = sxml_xml_array_at(&result->nodes, next.node);
            sxml_ixml_tree_node child = {.node_kind = SXML_IXML_NODE_KIND_INSERTION,
                                         .symbol = symbol_index,
                                         .detail = 0,
                                         .parent = next.node,
                                         .first_child = SXML_IXML_NONE,
                                         .next_sibling = parent->first_child};
            if (symbol->symbol_kind == SXML_IXML_SYMBOL_KIND_NONTERMINAL) {
                child.node_kind = SXML_IXML_NODE_KIND_NONTERMINAL;
                child.detail = symbol->target;
            } else if (symbol->symbol_kind == SXML_IXML_SYMBOL_KIND_TERMINAL) {
                child.node_kind = SXML_IXML_NODE_KIND_TERMINAL;
                child.detail = item_at(parser, item.predecessor)->position;
            }
            uint32_t child_index = 0;
            status = push_node(result, child, &child_index);
            if (status == SXML_STATUS_OK) {
                ((sxml_ixml_tree_node *)sxml_xml_array_at(&result->nodes, next.node))
                    ->first_child = child_index;
                status = charge_step(parser);
            }
            if (status == SXML_STATUS_OK &&
                child.node_kind == SXML_IXML_NODE_KIND_NONTERMINAL) {
                status = sxml_xml_array_push(&pending, &element);
                if (status == SXML_STATUS_OK) {
                    *(expansion *)element = (expansion){.node = child_index, .item = item.child};
                }
            }
            current = item.predecessor;
        }
    }
    sxml_xml_array_release(&pending);
    return status;
}

/* Runs the sets over the whole input and reads out the result. */
static sxml_status run_parser(earley_parser *parser, sxml_ixml_parse_result *result) {
    const uint32_t input_length = (uint32_t)parser->input.codepoint_count;
    start_set(parser, 0);
    sxml_status status = predict(parser, 0);
    for (uint32_t position = 0; status == SXML_STATUS_OK; position += 1) {
        status = close_set(parser);
        if (status != SXML_STATUS_OK || position == input_length) {
            break;
        }
        status = scan(parser);
        if (status == SXML_STATUS_OK && parser->current_set_count == 0) {
            /* No item read the character. */
            return record_failure(parser, position, result);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    parser->set_starts[input_length + 1] = (uint32_t)parser->items.element_count;
    uint32_t root_item = SXML_IXML_NONE;
    for (uint32_t item_index = parser->set_starts[input_length];
         item_index < parser->set_starts[input_length + 1]; item_index += 1) {
        const earley_item *item = item_at(parser, item_index);
        if (item->origin != 0 || next_symbol(parser, item) != NULL ||
            production_at(parser, item->production)->nonterminal != 0) {
            continue;
        }
        if (root_item == SXML_IXML_NONE) {
            root_item = item_index;
        } else {
            /* Two productions of the root derive the whole input. */
            result->is_ambiguous = true;
        }
    }
    if (root_item == SXML_IXML_NONE) {
        return record_failure(parser, input_length, result);
    }
    result->is_sentence = true;
    return build_tree(parser, root_item, result);
}

sxml_ixml_parse_result sxml_ixml_parse_result_empty(sxml_allocator allocator) {
    return (sxml_ixml_parse_result){
        .allocator = allocator,
        .is_sentence = false,
        .is_ambiguous = false,
        .nodes = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = allocator, .element_byte_count = sizeof(sxml_ixml_tree_node)}),
        .failure_position = 0,
        .expected_terminals = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = allocator, .element_byte_count = sizeof(uint32_t)})};
}

void sxml_ixml_parse_result_finalize(sxml_ixml_parse_result *result) {
    sxml_xml_array_release(&result->nodes);
    sxml_xml_array_release(&result->expected_terminals);
}

static sxml_status allocate_positions(sxml_allocator allocator, size_t count, uint32_t **positions) {
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate_array(
        allocator,
        (sxml_array_allocation_request){.element_count = count,
                                        .element_size = sizeof(uint32_t),
                                        .element_alignment = _Alignof(uint32_t)},
        &memory);
    *positions = memory;
    return status;
}

sxml_status sxml_ixml_parse(const sxml_ixml_parse_request *request,
                            sxml_ixml_parse_result *result) {
    const sxml_ixml_grammar *grammar = request->grammar;
    if (grammar->nonterminals.element_count == 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (request->input.codepoint_count >= SXML_IXML_NONE - 2) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    const sxml_allocator allocator = result->allocator;
    earley_parser parser = {
        .grammar = grammar,
        .input = request->input,
        .allocator = allocator,
        .step_meter = request->step_meter,
        .items = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = allocator, .element_byte_count = sizeof(earley_item)}),
        .waiting = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = allocator, .element_byte_count = sizeof(waiting_entry)}),
        .empty_completions = sxml_xml_array_empty((sxml_xml_array_configuration){
            .allocator = allocator, .element_byte_count = sizeof(waiting_entry)}),
        .uncharged_steps = 0};
    const size_t set_count = request->input.codepoint_count + 2;
    sxml_status status = allocate_positions(allocator, set_count, &parser.set_starts);
    if (status == SXML_STATUS_OK) {
        status = allocate_positions(allocator, set_count, &parser.waiting_starts);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_positions(allocator, grammar->nonterminals.element_count,
                                    &parser.predicted_positions);
    }
    if (status == SXML_STATUS_OK) {
        status = run_parser(&parser, result);
    }
    sxml_allocator_release(allocator, parser.hash_slots);
    sxml_allocator_release(allocator, parser.predicted_positions);
    sxml_allocator_release(allocator, parser.waiting_starts);
    sxml_allocator_release(allocator, parser.set_starts);
    sxml_xml_array_release(&parser.empty_completions);
    sxml_xml_array_release(&parser.waiting);
    sxml_xml_array_release(&parser.items);
    return status;
}
