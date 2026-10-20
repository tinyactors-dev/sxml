/*
 * The parse tables of a compiled Invisible XML grammar (src/ixml_internal.h,
 * D283): what the Earley parser of src/ixml_earley.c reads besides the
 * rules, computed once when a grammar is compiled.
 *
 * - Dotted rules (glossary `dotted_rule`): one per dot position of each
 *   production, so an item names its production and dot with one number,
 *   and advancing it adds one.
 * - Distinct terminals (glossary `distinct_terminal`): a string is one
 *   terminal per character, so `"a"` used twice is two terminals of one
 *   character set; the parser works with the distinct sets.
 * - Codepoint classes (glossary `codepoint_class`): the distinct terminals
 *   a codepoint matches, as a bit set. Those of ASCII are computed here;
 *   others when the parser meets them (sxml_ixml_compute_codepoint_class).
 * - Lookahead sets (glossary `lookahead_set`): for each dotted rule
 *   A: alpha . beta, FIRST(beta), with FOLLOW(A) when beta can be empty,
 *   as bit sets of distinct terminals plus one bit for the end of the
 *   input. An item is useful only when the next character's class meets
 *   its lookahead set (Earley 1970 used lookahead the same way; the global
 *   FOLLOW sets of SLR(1) need no lookahead in the items themselves).
 * - Regular tokens (glossary `regular_token`, D290): src/ixml_tokens.c,
 *   the last step.
 *
 * Nullable nonterminals, FIRST and FOLLOW are computed with work lists,
 * not by sweeping all rules until nothing changes, so a grammar of
 * thousands of chained rules takes time in proportion to its size; and
 * without recursion (D71).
 *
 * Golden file: src/ixml.c (the compiler's work list of pending
 * nonterminals, arrays in allocator memory).
 */
#include "ixml_internal.h"

#include "memory_internal.h"
#include "regex_internal.h"

#include <string.h>

/* The bits of a 64-bit word. */
#define SXML_IXML_WORD_BIT_COUNT 64U

/* The codepoints whose classes are computed with the grammar. */
#define SXML_IXML_ASCII_CODEPOINT_COUNT 128U

typedef struct table_builder {
    const sxml_ixml_grammar *grammar;
    sxml_ixml_parse_tables *tables;
    sxml_allocator allocator;
    const sxml_ixml_nonterminal *nonterminals;
    size_t nonterminal_count;
    const sxml_ixml_production *productions;
    size_t production_count;
    const sxml_ixml_symbol *symbols;
    const sxml_ixml_terminal *terminals;
    size_t terminal_count;
    const sxml_ixml_codepoint_range *ranges;
    bool *is_nullable;
    /* nonterminal_count sets of word_count words each. */
    uint64_t *first_words;
    uint64_t *follow_words;
    /* Open addressing over the lookahead sets, by their words. */
    uint32_t *set_slots;
    size_t set_slot_capacity;
    size_t lookahead_capacity;
} table_builder;

/* A dependency between two nonterminals' sets: the set of target takes in
   the set of source. Kept by source, as a compressed list. */
typedef struct set_edges {
    uint32_t *edge_starts; /* nonterminal_count + 1 */
    uint32_t *edge_targets;
} set_edges;

static sxml_status allocate_elements(sxml_allocator allocator, sxml_array_allocation_request request,
                                     void **memory) {
    return sxml_allocator_allocate_array(allocator, request, memory);
}

static sxml_status allocate_indices(sxml_allocator allocator, size_t count, uint32_t **indices) {
    void *memory = NULL;
    const sxml_status status = allocate_elements(
        allocator,
        (sxml_array_allocation_request){
            .element_count = count, .element_size = sizeof(uint32_t), .element_alignment = _Alignof(uint32_t)},
        &memory);
    *indices = memory;
    return status;
}

static sxml_status allocate_words(sxml_allocator allocator, size_t count, uint64_t **words) {
    void *memory = NULL;
    const sxml_status status = allocate_elements(
        allocator,
        (sxml_array_allocation_request){
            .element_count = count, .element_size = sizeof(uint64_t), .element_alignment = _Alignof(uint64_t)},
        &memory);
    *words = memory;
    return status;
}

static void set_bit(uint64_t *words, size_t bit) {
    words[bit / SXML_IXML_WORD_BIT_COUNT] |= (uint64_t)1 << (bit % SXML_IXML_WORD_BIT_COUNT);
}

/* Adds the words of source to target; true when target changed. */
typedef struct word_union {
    uint64_t *target;
    const uint64_t *source;
} word_union;

static bool unite_words(word_union union_request, size_t word_count) {
    bool has_changed = false;
    for (size_t word_index = 0; word_index < word_count; word_index += 1) {
        const uint64_t united = union_request.target[word_index] | union_request.source[word_index];
        has_changed = has_changed || united != union_request.target[word_index];
        union_request.target[word_index] = united;
    }
    return has_changed;
}

/* ------------------------------------------------- Distinct terminals */

/* True when the terminal matches codepoint, whose general category is
   category (the Specification, "Character sets"). */
typedef struct category_match {
    const sxml_ixml_terminal *terminal;
    const sxml_ixml_codepoint_range *ranges;
    uint32_t category;
} category_match;

static bool terminal_matches_codepoint(category_match match, uint32_t codepoint) {
    bool is_member = false;
    for (uint32_t range_index = 0; !is_member && range_index < match.terminal->range_count;
         range_index += 1) {
        const sxml_ixml_codepoint_range *range =
            &match.ranges[match.terminal->first_range + range_index];
        is_member = codepoint >= range->first_codepoint && codepoint <= range->last_codepoint;
    }
    if (!is_member && match.terminal->category_mask != 0) {
        is_member = (match.terminal->category_mask & (1U << match.category)) != 0;
    }
    return is_member != match.terminal->is_exclusion;
}

static uint64_t hash_terminal(const table_builder *builder, uint32_t terminal_index) {
    const sxml_ixml_terminal *terminal = &builder->terminals[terminal_index];
    uint64_t hash = ((uint64_t)terminal->category_mask << 1U) | (terminal->is_exclusion ? 1U : 0U);
    for (uint32_t range_index = 0; range_index < terminal->range_count; range_index += 1) {
        const sxml_ixml_codepoint_range *range = &builder->ranges[terminal->first_range + range_index];
        hash = (hash ^ range->first_codepoint) * 0x9E3779B97F4A7C15ULL;
        hash = (hash ^ range->last_codepoint) * 0x9E3779B97F4A7C15ULL;
    }
    return hash ^ (hash >> 31U);
}

typedef struct terminal_pair {
    uint32_t left;
    uint32_t right;
} terminal_pair;

static bool terminals_are_equal(const table_builder *builder, terminal_pair pair) {
    const sxml_ixml_terminal *left = &builder->terminals[pair.left];
    const sxml_ixml_terminal *right = &builder->terminals[pair.right];
    if (left->category_mask != right->category_mask || left->is_exclusion != right->is_exclusion ||
        left->range_count != right->range_count) {
        return false;
    }
    for (uint32_t range_index = 0; range_index < left->range_count; range_index += 1) {
        const sxml_ixml_codepoint_range *left_range = &builder->ranges[left->first_range + range_index];
        const sxml_ixml_codepoint_range *right_range =
            &builder->ranges[right->first_range + range_index];
        if (left_range->first_codepoint != right_range->first_codepoint ||
            left_range->last_codepoint != right_range->last_codepoint) {
            return false;
        }
    }
    return true;
}

/* Numbers the distinct terminals, in the order they first appear. */
static sxml_status find_distinct_terminals(table_builder *builder) {
    sxml_ixml_parse_tables *tables = builder->tables;
    const size_t terminal_count = builder->terminal_count;
    size_t slot_capacity = 16;
    while (slot_capacity < terminal_count * 2) {
        slot_capacity *= 2;
    }
    uint32_t *slots = NULL;
    sxml_status status = allocate_indices(builder->allocator, slot_capacity, &slots);
    if (status == SXML_STATUS_OK) {
        status = allocate_indices(builder->allocator, terminal_count, &tables->distinct_terminals);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_indices(builder->allocator, terminal_count, &tables->representative_terminals);
    }
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(builder->allocator, slots);
        return status;
    }
    memset(slots, 0xFF, slot_capacity * sizeof(uint32_t));
    const size_t mask = slot_capacity - 1;
    for (uint32_t terminal_index = 0; terminal_index < terminal_count; terminal_index += 1) {
        size_t slot = (size_t)hash_terminal(builder, terminal_index) & mask;
        uint32_t distinct = SXML_IXML_NONE;
        while (slots[slot] != SXML_IXML_NONE) {
            const uint32_t candidate = slots[slot];
            if (terminals_are_equal(builder,
                                    (terminal_pair){.left = tables->representative_terminals[candidate],
                                                    .right = terminal_index})) {
                distinct = candidate;
                break;
            }
            slot = (slot + 1) & mask;
        }
        if (distinct == SXML_IXML_NONE) {
            distinct = (uint32_t)tables->distinct_terminal_count;
            tables->representative_terminals[distinct] = terminal_index;
            tables->distinct_terminal_count += 1;
            slots[slot] = distinct;
        }
        tables->distinct_terminals[terminal_index] = distinct;
    }
    sxml_allocator_release(builder->allocator, slots);
    /* One bit more than the distinct terminals: the end of the input. */
    tables->word_count =
        (tables->distinct_terminal_count + 1 + SXML_IXML_WORD_BIT_COUNT - 1) / SXML_IXML_WORD_BIT_COUNT;
    return SXML_STATUS_OK;
}

/* -------------------------------------------------------- Dotted rules */

static sxml_status number_dotted_rules(table_builder *builder) {
    sxml_ixml_parse_tables *tables = builder->tables;
    size_t dotted_rule_count = 0;
    for (size_t production_index = 0; production_index < builder->production_count;
         production_index += 1) {
        dotted_rule_count += (size_t)builder->productions[production_index].symbol_count + 1;
        if (dotted_rule_count > SXML_IXML_MAXIMUM_DOTTED_RULE_COUNT) {
            return SXML_STATUS_LIMIT_EXCEEDED;
        }
    }
    void *memory = NULL;
    sxml_status status = allocate_elements(
        builder->allocator,
        (sxml_array_allocation_request){.element_count = dotted_rule_count,
                                        .element_size = sizeof(sxml_ixml_dotted_rule),
                                        .element_alignment = _Alignof(sxml_ixml_dotted_rule)},
        &memory);
    if (status == SXML_STATUS_OK) {
        tables->dotted_rules = memory;
        tables->dotted_rule_count = dotted_rule_count;
        status = allocate_indices(builder->allocator, builder->production_count,
                                  &tables->first_dotted_rules);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t next_rule = 0;
    for (uint32_t production_index = 0; production_index < builder->production_count;
         production_index += 1) {
        const sxml_ixml_production *production = &builder->productions[production_index];
        tables->first_dotted_rules[production_index] = next_rule;
        for (uint32_t dot_position = 0; dot_position < production->symbol_count; dot_position += 1) {
            const sxml_ixml_symbol *symbol = &builder->symbols[production->first_symbol + dot_position];
            sxml_ixml_dotted_rule rule = {.production = production_index,
                                          .target = 0,
                                          .lookahead_set = 0,
                                          .kind = SXML_IXML_DOTTED_RULE_KIND_INSERTION};
            switch ((sxml_ixml_symbol_kind)symbol->symbol_kind) {
            case SXML_IXML_SYMBOL_KIND_NONTERMINAL:
                rule.kind = SXML_IXML_DOTTED_RULE_KIND_NONTERMINAL;
                rule.target = symbol->target;
                break;
            case SXML_IXML_SYMBOL_KIND_TERMINAL:
                rule.kind = SXML_IXML_DOTTED_RULE_KIND_TERMINAL;
                rule.target = tables->distinct_terminals[symbol->target];
                break;
            case SXML_IXML_SYMBOL_KIND_INSERTION:
                break;
            }
            tables->dotted_rules[next_rule] = rule;
            next_rule += 1;
        }
        tables->dotted_rules[next_rule] =
            (sxml_ixml_dotted_rule){.production = production_index,
                                    .target = production->nonterminal,
                                    .lookahead_set = 0,
                                    .kind = SXML_IXML_DOTTED_RULE_KIND_COMPLETE};
        next_rule += 1;
    }
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------- Nullable rules */

/* Finds the nonterminals that derive the empty string: a production all
   of whose symbols are insertions or nullable nonterminals makes its
   nonterminal nullable. Each production counts its symbols not yet known
   to be nullable; a nonterminal that becomes nullable lowers the count of
   every production it occurs in. */
static sxml_status find_nullable(table_builder *builder) {
    const size_t nonterminal_count = builder->nonterminal_count;
    uint32_t *remaining = NULL;
    uint32_t *occurrence_starts = NULL;
    uint32_t *occurrences = NULL;
    uint32_t *work = NULL;
    size_t occurrence_count = 0;
    for (size_t production_index = 0; production_index < builder->production_count;
         production_index += 1) {
        occurrence_count += builder->productions[production_index].symbol_count;
    }
    sxml_status status = allocate_indices(builder->allocator, builder->production_count, &remaining);
    if (status == SXML_STATUS_OK) {
        status = allocate_indices(builder->allocator, nonterminal_count + 1, &occurrence_starts);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_indices(builder->allocator, occurrence_count, &occurrences);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_indices(builder->allocator, nonterminal_count, &work);
    }
    if (status == SXML_STATUS_OK) {
        for (uint32_t production_index = 0; production_index < builder->production_count;
             production_index += 1) {
            const sxml_ixml_production *production = &builder->productions[production_index];
            for (uint32_t dot_position = 0; dot_position < production->symbol_count; dot_position += 1) {
                const sxml_ixml_symbol *symbol = &builder->symbols[production->first_symbol + dot_position];
                if (symbol->symbol_kind == SXML_IXML_SYMBOL_KIND_INSERTION) {
                    continue;
                }
                remaining[production_index] += 1;
                if (symbol->symbol_kind == SXML_IXML_SYMBOL_KIND_NONTERMINAL) {
                    occurrence_starts[symbol->target + 1] += 1;
                }
            }
        }
        for (size_t nonterminal_index = 0; nonterminal_index < nonterminal_count;
             nonterminal_index += 1) {
            occurrence_starts[nonterminal_index + 1] += occurrence_starts[nonterminal_index];
        }
        /* Filled from the starts, which are then the ends; shifted back
           below. */
        for (uint32_t production_index = 0; production_index < builder->production_count;
             production_index += 1) {
            const sxml_ixml_production *production = &builder->productions[production_index];
            for (uint32_t dot_position = 0; dot_position < production->symbol_count; dot_position += 1) {
                const sxml_ixml_symbol *symbol = &builder->symbols[production->first_symbol + dot_position];
                if (symbol->symbol_kind == SXML_IXML_SYMBOL_KIND_NONTERMINAL) {
                    occurrences[occurrence_starts[symbol->target]] = production_index;
                    occurrence_starts[symbol->target] += 1;
                }
            }
        }
        for (size_t nonterminal_index = nonterminal_count; nonterminal_index > 0;
             nonterminal_index -= 1) {
            occurrence_starts[nonterminal_index] = occurrence_starts[nonterminal_index - 1];
        }
        occurrence_starts[0] = 0;
        size_t work_count = 0;
        for (uint32_t production_index = 0; production_index < builder->production_count;
             production_index += 1) {
            const uint32_t nonterminal = builder->productions[production_index].nonterminal;
            if (remaining[production_index] == 0 && !builder->is_nullable[nonterminal]) {
                builder->is_nullable[nonterminal] = true;
                work[work_count] = nonterminal;
                work_count += 1;
            }
        }
        while (work_count > 0) {
            work_count -= 1;
            const uint32_t nullable = work[work_count];
            for (uint32_t occurrence = occurrence_starts[nullable];
                 occurrence < occurrence_starts[nullable + 1]; occurrence += 1) {
                const uint32_t production_index = occurrences[occurrence];
                remaining[production_index] -= 1;
                const uint32_t nonterminal = builder->productions[production_index].nonterminal;
                if (remaining[production_index] == 0 && !builder->is_nullable[nonterminal]) {
                    builder->is_nullable[nonterminal] = true;
                    work[work_count] = nonterminal;
                    work_count += 1;
                }
            }
        }
    }
    sxml_allocator_release(builder->allocator, work);
    sxml_allocator_release(builder->allocator, occurrences);
    sxml_allocator_release(builder->allocator, occurrence_starts);
    sxml_allocator_release(builder->allocator, remaining);
    return status;
}

/* ------------------------------------------------- FIRST and FOLLOW */

/* What a pass over the productions records: a dependency of the set of
   target on the set of source. */
typedef struct edge_request {
    set_edges *edges;
    /* NULL while counting. */
    uint32_t *fill_positions;
} edge_request;

typedef struct edge {
    uint32_t source;
    uint32_t target;
} edge;

static void record_edge(edge_request request, edge dependency) {
    if (request.fill_positions == NULL) {
        request.edges->edge_starts[dependency.source + 1] += 1;
        return;
    }
    request.edges->edge_targets[request.fill_positions[dependency.source]] = dependency.target;
    request.fill_positions[dependency.source] += 1;
}

/* FIRST: a production's symbols up to the first that is not nullable.
   A terminal goes into the production's FIRST set; a nonterminal makes
   it take in that nonterminal's. */
static void scan_first(table_builder *builder, edge_request request) {
    const size_t word_count = builder->tables->word_count;
    for (uint32_t production_index = 0; production_index < builder->production_count;
         production_index += 1) {
        const sxml_ixml_production *production = &builder->productions[production_index];
        for (uint32_t dot_position = 0; dot_position < production->symbol_count; dot_position += 1) {
            const sxml_ixml_symbol *symbol = &builder->symbols[production->first_symbol + dot_position];
            if (symbol->symbol_kind == SXML_IXML_SYMBOL_KIND_INSERTION) {
                continue;
            }
            if (symbol->symbol_kind == SXML_IXML_SYMBOL_KIND_TERMINAL) {
                if (request.fill_positions == NULL) {
                    set_bit(&builder->first_words[(size_t)production->nonterminal * word_count],
                            builder->tables->distinct_terminals[symbol->target]);
                }
                break;
            }
            record_edge(request, (edge){.source = symbol->target, .target = production->nonterminal});
            if (!builder->is_nullable[symbol->target]) {
                break;
            }
        }
    }
}

/* FOLLOW: for each nonterminal of a production, FIRST of what follows it
   there goes into its FOLLOW set, and when all that can be empty, it
   takes in the FOLLOW set of the production's nonterminal. Read from the
   end, with the FIRST set of the rest in rest_words. */
typedef struct follow_scan {
    edge_request request;
    uint64_t *rest_words;
} follow_scan;

static void scan_follow(table_builder *builder, follow_scan scan) {
    const size_t word_count = builder->tables->word_count;
    for (uint32_t production_index = 0; production_index < builder->production_count;
         production_index += 1) {
        const sxml_ixml_production *production = &builder->productions[production_index];
        memset(scan.rest_words, 0, word_count * sizeof(uint64_t));
        bool rest_is_nullable = true;
        for (uint32_t dot_position = production->symbol_count; dot_position > 0; dot_position -= 1) {
            const sxml_ixml_symbol *symbol = &builder->symbols[production->first_symbol + dot_position - 1];
            if (symbol->symbol_kind == SXML_IXML_SYMBOL_KIND_INSERTION) {
                continue;
            }
            if (symbol->symbol_kind == SXML_IXML_SYMBOL_KIND_TERMINAL) {
                memset(scan.rest_words, 0, word_count * sizeof(uint64_t));
                set_bit(scan.rest_words, builder->tables->distinct_terminals[symbol->target]);
                rest_is_nullable = false;
                continue;
            }
            uint64_t *follow = &builder->follow_words[(size_t)symbol->target * word_count];
            if (scan.request.fill_positions == NULL) {
                (void)unite_words((word_union){.target = follow, .source = scan.rest_words},
                                  word_count);
            }
            if (rest_is_nullable) {
                record_edge(scan.request,
                            (edge){.source = production->nonterminal, .target = symbol->target});
            }
            const uint64_t *first = &builder->first_words[(size_t)symbol->target * word_count];
            if (builder->is_nullable[symbol->target]) {
                (void)unite_words((word_union){.target = scan.rest_words, .source = first},
                                  word_count);
            } else {
                memcpy(scan.rest_words, first, word_count * sizeof(uint64_t));
                rest_is_nullable = false;
            }
        }
    }
}

/* Which sets a propagation works on. */
typedef enum set_family {
    SXML_IXML_SET_FAMILY_FIRST,
    SXML_IXML_SET_FAMILY_FOLLOW,
} set_family;

/* Collects the edges of one family (a counting pass, then a filling
   pass), then passes sets along them until none changes. */
static sxml_status propagate_sets(table_builder *builder, set_family family) {
    const size_t nonterminal_count = builder->nonterminal_count;
    const size_t word_count = builder->tables->word_count;
    set_edges edges = {.edge_starts = NULL, .edge_targets = NULL};
    uint32_t *fill_positions = NULL;
    uint32_t *work = NULL;
    bool *is_waiting = NULL;
    uint64_t *rest_words = NULL;
    uint64_t *sets = family == SXML_IXML_SET_FAMILY_FIRST ? builder->first_words : builder->follow_words;
    sxml_status status = allocate_indices(builder->allocator, nonterminal_count + 1, &edges.edge_starts);
    if (status == SXML_STATUS_OK) {
        status = allocate_words(builder->allocator, word_count, &rest_words);
    }
    if (status == SXML_STATUS_OK) {
        if (family == SXML_IXML_SET_FAMILY_FIRST) {
            scan_first(builder, (edge_request){.edges = &edges, .fill_positions = NULL});
        } else {
            scan_follow(builder,
                        (follow_scan){.request = {.edges = &edges, .fill_positions = NULL},
                                      .rest_words = rest_words});
        }
        for (size_t nonterminal_index = 0; nonterminal_index < nonterminal_count;
             nonterminal_index += 1) {
            edges.edge_starts[nonterminal_index + 1] += edges.edge_starts[nonterminal_index];
        }
        status = allocate_indices(builder->allocator, edges.edge_starts[nonterminal_count],
                                  &edges.edge_targets);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_indices(builder->allocator, nonterminal_count, &fill_positions);
    }
    if (status == SXML_STATUS_OK) {
        memcpy(fill_positions, edges.edge_starts, nonterminal_count * sizeof(uint32_t));
        if (family == SXML_IXML_SET_FAMILY_FIRST) {
            scan_first(builder, (edge_request){.edges = &edges, .fill_positions = fill_positions});
        } else {
            scan_follow(builder, (follow_scan){.request = {.edges = &edges,
                                                           .fill_positions = fill_positions},
                                               .rest_words = rest_words});
        }
        status = allocate_indices(builder->allocator, nonterminal_count, &work);
    }
    if (status == SXML_STATUS_OK) {
        void *memory = NULL;
        status = allocate_elements(
            builder->allocator,
            (sxml_array_allocation_request){.element_count = nonterminal_count,
                                            .element_size = sizeof(bool),
                                            .element_alignment = _Alignof(bool)},
            &memory);
        is_waiting = memory;
    }
    if (status == SXML_STATUS_OK) {
        size_t work_count = 0;
        for (uint32_t nonterminal_index = 0; nonterminal_index < nonterminal_count;
             nonterminal_index += 1) {
            work[work_count] = nonterminal_index;
            work_count += 1;
            is_waiting[nonterminal_index] = true;
        }
        while (work_count > 0) {
            work_count -= 1;
            const uint32_t source = work[work_count];
            is_waiting[source] = false;
            for (uint32_t edge_index = edges.edge_starts[source];
                 edge_index < edges.edge_starts[source + 1]; edge_index += 1) {
                const uint32_t target = edges.edge_targets[edge_index];
                if (unite_words((word_union){.target = &sets[(size_t)target * word_count],
                                             .source = &sets[(size_t)source * word_count]},
                                word_count) &&
                    !is_waiting[target]) {
                    is_waiting[target] = true;
                    work[work_count] = target;
                    work_count += 1;
                }
            }
        }
    }
    sxml_allocator_release(builder->allocator, is_waiting);
    sxml_allocator_release(builder->allocator, work);
    sxml_allocator_release(builder->allocator, fill_positions);
    sxml_allocator_release(builder->allocator, edges.edge_targets);
    sxml_allocator_release(builder->allocator, edges.edge_starts);
    sxml_allocator_release(builder->allocator, rest_words);
    return status;
}

/* ---------------------------------------------------- Lookahead sets */

static uint64_t hash_words(const uint64_t *words, size_t word_count) {
    uint64_t hash = 0x2545F4914F6CDD1DULL;
    for (size_t word_index = 0; word_index < word_count; word_index += 1) {
        hash = (hash ^ words[word_index]) * 0x9E3779B97F4A7C15ULL;
    }
    return hash ^ (hash >> 29U);
}

static sxml_status grow_set_slots(table_builder *builder) {
    const size_t capacity = builder->set_slot_capacity == 0 ? 64 : builder->set_slot_capacity * 2;
    uint32_t *slots = NULL;
    const sxml_status status = allocate_indices(builder->allocator, capacity, &slots);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(slots, 0xFF, capacity * sizeof(uint32_t));
    const size_t word_count = builder->tables->word_count;
    const size_t mask = capacity - 1;
    for (uint32_t set_index = 0; set_index < builder->tables->lookahead_set_count; set_index += 1) {
        size_t slot = (size_t)hash_words(&builder->tables->lookahead_words[set_index * word_count],
                                         word_count) &
                      mask;
        while (slots[slot] != SXML_IXML_NONE) {
            slot = (slot + 1) & mask;
        }
        slots[slot] = set_index;
    }
    sxml_allocator_release(builder->allocator, builder->set_slots);
    builder->set_slots = slots;
    builder->set_slot_capacity = capacity;
    return SXML_STATUS_OK;
}

/* The index of the lookahead set words, added once. */
static sxml_status intern_set(table_builder *builder, const uint64_t *words, uint32_t *set_index) {
    sxml_ixml_parse_tables *tables = builder->tables;
    const size_t word_count = tables->word_count;
    if ((tables->lookahead_set_count + 1) * 2 > builder->set_slot_capacity) {
        const sxml_status status = grow_set_slots(builder);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    const size_t mask = builder->set_slot_capacity - 1;
    size_t slot = (size_t)hash_words(words, word_count) & mask;
    while (builder->set_slots[slot] != SXML_IXML_NONE) {
        const uint32_t candidate = builder->set_slots[slot];
        if (memcmp(&tables->lookahead_words[candidate * word_count], words,
                   word_count * sizeof(uint64_t)) == 0) {
            *set_index = candidate;
            return SXML_STATUS_OK;
        }
        slot = (slot + 1) & mask;
    }
    void *elements = tables->lookahead_words;
    const sxml_status status = sxml_allocator_reserve_array(
        builder->allocator,
        &(sxml_array_reservation_request){.elements = &elements,
                                          .capacity = &builder->lookahead_capacity,
                                          .element_count = tables->lookahead_set_count * word_count,
                                          .required_count = (tables->lookahead_set_count + 1) * word_count,
                                          .element_size = sizeof(uint64_t),
                                          .element_alignment = _Alignof(uint64_t),
                                          .minimum_capacity = 64});
    tables->lookahead_words = elements;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memcpy(&tables->lookahead_words[tables->lookahead_set_count * word_count], words,
           word_count * sizeof(uint64_t));
    *set_index = (uint32_t)tables->lookahead_set_count;
    builder->set_slots[slot] = *set_index;
    tables->lookahead_set_count += 1;
    return SXML_STATUS_OK;
}

/* Gives every dotted rule its lookahead set, reading each production
   from its end: the complete rule's is the FOLLOW set of the
   production's nonterminal, and the rule before a symbol has the
   symbol's FIRST set, with the next rule's when the symbol can be
   empty. */
static sxml_status assign_lookahead_sets(table_builder *builder) {
    sxml_ixml_parse_tables *tables = builder->tables;
    const size_t word_count = tables->word_count;
    uint64_t *rest_words = NULL;
    sxml_status status = allocate_words(builder->allocator, word_count, &rest_words);
    for (uint32_t production_index = 0;
         status == SXML_STATUS_OK && production_index < builder->production_count;
         production_index += 1) {
        const sxml_ixml_production *production = &builder->productions[production_index];
        const uint32_t first_rule = tables->first_dotted_rules[production_index];
        memcpy(rest_words, &builder->follow_words[(size_t)production->nonterminal * word_count],
               word_count * sizeof(uint64_t));
        status = intern_set(builder, rest_words,
                            &tables->dotted_rules[first_rule + production->symbol_count].lookahead_set);
        for (uint32_t dot_position = production->symbol_count; status == SXML_STATUS_OK && dot_position > 0;
             dot_position -= 1) {
            const sxml_ixml_symbol *symbol = &builder->symbols[production->first_symbol + dot_position - 1];
            if (symbol->symbol_kind == SXML_IXML_SYMBOL_KIND_TERMINAL) {
                memset(rest_words, 0, word_count * sizeof(uint64_t));
                set_bit(rest_words, tables->distinct_terminals[symbol->target]);
            } else if (symbol->symbol_kind == SXML_IXML_SYMBOL_KIND_NONTERMINAL) {
                const uint64_t *first = &builder->first_words[(size_t)symbol->target * word_count];
                if (builder->is_nullable[symbol->target]) {
                    (void)unite_words((word_union){.target = rest_words, .source = first},
                                      word_count);
                } else {
                    memcpy(rest_words, first, word_count * sizeof(uint64_t));
                }
            }
            status = intern_set(builder, rest_words,
                                &tables->dotted_rules[first_rule + dot_position - 1].lookahead_set);
        }
    }
    sxml_allocator_release(builder->allocator, rest_words);
    return status;
}

/* ------------------------------------------------- Codepoint classes */

void sxml_ixml_compute_codepoint_class(sxml_ixml_class_request request, uint64_t *class_words) {
    const sxml_ixml_grammar *grammar = request.grammar;
    const sxml_ixml_parse_tables *tables = &grammar->tables;
    memset(class_words, 0, tables->word_count * sizeof(uint64_t));
    const uint32_t category = (uint32_t)sxml_regex_get_general_category(request.codepoint);
    const sxml_ixml_terminal *terminals = (const sxml_ixml_terminal *)grammar->terminals.elements;
    const sxml_ixml_codepoint_range *ranges =
        (const sxml_ixml_codepoint_range *)grammar->ranges.elements;
    for (size_t distinct = 0; distinct < tables->distinct_terminal_count; distinct += 1) {
        if (terminal_matches_codepoint(
                (category_match){.terminal = &terminals[tables->representative_terminals[distinct]],
                                 .ranges = ranges,
                                 .category = category},
                request.codepoint)) {
            set_bit(class_words, distinct);
        }
    }
}

static sxml_status compute_ascii_classes(table_builder *builder) {
    sxml_ixml_parse_tables *tables = builder->tables;
    const sxml_status status = allocate_words(
        builder->allocator, SXML_IXML_ASCII_CODEPOINT_COUNT * tables->word_count, &tables->ascii_classes);
    for (uint32_t codepoint = 0; status == SXML_STATUS_OK && codepoint < SXML_IXML_ASCII_CODEPOINT_COUNT;
         codepoint += 1) {
        sxml_ixml_compute_codepoint_class(
            (sxml_ixml_class_request){.grammar = builder->grammar, .codepoint = codepoint},
            &tables->ascii_classes[codepoint * tables->word_count]);
    }
    return status;
}

/* ------------------------------------------------ Exposed attributes */

/* The mark of a use of a nonterminal: the use's, else its rule's, else
   an element (the Specification, "Constructing XML"). */
static sxml_ixml_mark use_mark(const table_builder *builder, const sxml_ixml_symbol *symbol) {
    sxml_ixml_mark mark = (sxml_ixml_mark)symbol->mark;
    if (mark == SXML_IXML_MARK_NONE) {
        mark = (sxml_ixml_mark)builder->nonterminals[symbol->target].mark;
    }
    return mark == SXML_IXML_MARK_NONE ? SXML_IXML_MARK_ELEMENT : mark;
}

/* Finds the nonterminals whose nodes can expose an attribute: one of
   their productions uses a nonterminal as an attribute, or uses hidden
   a nonterminal that can. Passed back along the hidden uses with a work
   list. */
static sxml_status find_attribute_exposure(table_builder *builder) {
    const size_t nonterminal_count = builder->nonterminal_count;
    bool *exposes = NULL;
    uint32_t *use_starts = NULL;
    uint32_t *users = NULL;
    uint32_t *work = NULL;
    void *memory = NULL;
    sxml_status status = allocate_elements(
        builder->allocator,
        (sxml_array_allocation_request){
            .element_count = nonterminal_count, .element_size = sizeof(bool), .element_alignment = _Alignof(bool)},
        &memory);
    exposes = memory;
    if (status == SXML_STATUS_OK) {
        status = allocate_indices(builder->allocator, nonterminal_count + 1, &use_starts);
    }
    size_t use_count = 0;
    for (size_t production_index = 0; status == SXML_STATUS_OK && production_index < builder->production_count;
         production_index += 1) {
        const sxml_ixml_production *production = &builder->productions[production_index];
        for (uint32_t dot_position = 0; dot_position < production->symbol_count; dot_position += 1) {
            const sxml_ixml_symbol *symbol = &builder->symbols[production->first_symbol + dot_position];
            if (symbol->symbol_kind != SXML_IXML_SYMBOL_KIND_NONTERMINAL) {
                continue;
            }
            const sxml_ixml_mark mark = use_mark(builder, symbol);
            if (mark == SXML_IXML_MARK_ATTRIBUTE) {
                exposes[production->nonterminal] = true;
            } else if (mark == SXML_IXML_MARK_HIDDEN) {
                use_starts[symbol->target + 1] += 1;
                use_count += 1;
            }
        }
    }
    if (status == SXML_STATUS_OK) {
        for (size_t nonterminal_index = 0; nonterminal_index < nonterminal_count; nonterminal_index += 1) {
            use_starts[nonterminal_index + 1] += use_starts[nonterminal_index];
        }
        status = allocate_indices(builder->allocator, use_count, &users);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_indices(builder->allocator, nonterminal_count, &work);
    }
    if (status == SXML_STATUS_OK) {
        /* The starts are cursors while filling, then the ends; shifted
           back below. */
        for (size_t production_index = 0; production_index < builder->production_count;
             production_index += 1) {
            const sxml_ixml_production *production = &builder->productions[production_index];
            for (uint32_t dot_position = 0; dot_position < production->symbol_count; dot_position += 1) {
                const sxml_ixml_symbol *symbol = &builder->symbols[production->first_symbol + dot_position];
                if (symbol->symbol_kind == SXML_IXML_SYMBOL_KIND_NONTERMINAL &&
                    use_mark(builder, symbol) == SXML_IXML_MARK_HIDDEN) {
                    users[use_starts[symbol->target]] = production->nonterminal;
                    use_starts[symbol->target] += 1;
                }
            }
        }
        for (size_t nonterminal_index = nonterminal_count; nonterminal_index > 0; nonterminal_index -= 1) {
            use_starts[nonterminal_index] = use_starts[nonterminal_index - 1];
        }
        use_starts[0] = 0;
        size_t work_count = 0;
        for (uint32_t nonterminal_index = 0; nonterminal_index < nonterminal_count; nonterminal_index += 1) {
            if (exposes[nonterminal_index]) {
                work[work_count] = nonterminal_index;
                work_count += 1;
            }
        }
        while (work_count > 0) {
            work_count -= 1;
            const uint32_t exposing = work[work_count];
            for (uint32_t use_index = use_starts[exposing]; use_index < use_starts[exposing + 1];
                 use_index += 1) {
                if (!exposes[users[use_index]]) {
                    exposes[users[use_index]] = true;
                    work[work_count] = users[use_index];
                    work_count += 1;
                }
            }
        }
        builder->tables->exposes_attributes = exposes;
        exposes = NULL;
    }
    sxml_allocator_release(builder->allocator, exposes);
    sxml_allocator_release(builder->allocator, work);
    sxml_allocator_release(builder->allocator, users);
    sxml_allocator_release(builder->allocator, use_starts);
    return status;
}

/* --------------------------------------------------------- The tables */

void sxml_ixml_parse_tables_release(sxml_ixml_parse_tables *tables) {
    const sxml_allocator allocator = tables->allocator;
    sxml_allocator_release(allocator, tables->dotted_rules);
    sxml_allocator_release(allocator, tables->first_dotted_rules);
    sxml_allocator_release(allocator, tables->distinct_terminals);
    sxml_allocator_release(allocator, tables->representative_terminals);
    sxml_allocator_release(allocator, tables->lookahead_words);
    sxml_allocator_release(allocator, tables->ascii_classes);
    sxml_allocator_release(allocator, tables->exposes_attributes);
    sxml_allocator_release(allocator, tables->tokens_by_nonterminal);
    sxml_xml_array_release(&tables->tokens);
    sxml_xml_array_release(&tables->token_transitions);
    sxml_xml_array_release(&tables->token_accepts);
    sxml_xml_array_release(&tables->token_terminals);
    sxml_xml_array_release(&tables->token_ascii_columns);
    sxml_xml_array_release(&tables->token_follows);
    memset(tables, 0, sizeof(*tables));
}

static sxml_xml_array empty_array(sxml_allocator allocator, size_t element_byte_count) {
    return sxml_xml_array_empty((sxml_xml_array_configuration){
        .allocator = allocator, .element_byte_count = element_byte_count});
}

sxml_status sxml_ixml_grammar_prepare_tables(sxml_ixml_grammar *grammar, sxml_allocator allocator) {
    sxml_ixml_parse_tables *tables = &grammar->tables;
    memset(tables, 0, sizeof(*tables));
    tables->allocator = allocator;
    tables->tokens = empty_array(allocator, sizeof(sxml_ixml_token));
    tables->token_transitions = empty_array(allocator, sizeof(uint16_t));
    tables->token_accepts = empty_array(allocator, sizeof(uint8_t));
    tables->token_terminals = empty_array(allocator, sizeof(uint32_t));
    tables->token_ascii_columns = empty_array(allocator, sizeof(uint8_t));
    tables->token_follows = empty_array(allocator, sizeof(uint64_t));
    table_builder builder = {
        .grammar = grammar,
        .tables = tables,
        .allocator = allocator,
        .nonterminals = (const sxml_ixml_nonterminal *)grammar->nonterminals.elements,
        .nonterminal_count = grammar->nonterminals.element_count,
        .productions = (const sxml_ixml_production *)grammar->productions.elements,
        .production_count = grammar->productions.element_count,
        .symbols = (const sxml_ixml_symbol *)grammar->symbols.elements,
        .terminals = (const sxml_ixml_terminal *)grammar->terminals.elements,
        .terminal_count = grammar->terminals.element_count,
        .ranges = (const sxml_ixml_codepoint_range *)grammar->ranges.elements,
        .is_nullable = NULL,
        .first_words = NULL,
        .follow_words = NULL,
        .set_slots = NULL,
        .set_slot_capacity = 0,
        .lookahead_capacity = 0};
    sxml_status status = find_distinct_terminals(&builder);
    if (status == SXML_STATUS_OK) {
        status = number_dotted_rules(&builder);
    }
    if (status == SXML_STATUS_OK) {
        void *memory = NULL;
        status = allocate_elements(allocator,
                                   (sxml_array_allocation_request){.element_count =
                                                                       builder.nonterminal_count,
                                                                   .element_size = sizeof(bool),
                                                                   .element_alignment = _Alignof(bool)},
                                   &memory);
        builder.is_nullable = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_words(allocator, builder.nonterminal_count * tables->word_count,
                                &builder.first_words);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_words(allocator, builder.nonterminal_count * tables->word_count,
                                &builder.follow_words);
    }
    if (status == SXML_STATUS_OK) {
        status = find_nullable(&builder);
    }
    if (status == SXML_STATUS_OK) {
        status = propagate_sets(&builder, SXML_IXML_SET_FAMILY_FIRST);
    }
    if (status == SXML_STATUS_OK && builder.nonterminal_count > 0) {
        /* The root is followed by the end of the input (the
           Specification, "Parsing": the whole input is one root). */
        set_bit(builder.follow_words, tables->distinct_terminal_count);
        status = propagate_sets(&builder, SXML_IXML_SET_FAMILY_FOLLOW);
    }
    if (status == SXML_STATUS_OK) {
        status = assign_lookahead_sets(&builder);
    }
    if (status == SXML_STATUS_OK) {
        status = compute_ascii_classes(&builder);
    }
    if (status == SXML_STATUS_OK) {
        status = find_attribute_exposure(&builder);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_ixml_prepare_tokens(grammar);
    }
    sxml_allocator_release(allocator, builder.set_slots);
    sxml_allocator_release(allocator, builder.follow_words);
    sxml_allocator_release(allocator, builder.first_words);
    sxml_allocator_release(allocator, builder.is_nullable);
    if (status != SXML_STATUS_OK) {
        sxml_ixml_parse_tables_release(tables);
    }
    return status;
}
