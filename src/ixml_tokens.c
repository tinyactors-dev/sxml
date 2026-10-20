/*
 * Regular tokens of a compiled Invisible XML grammar (src/ixml_internal.h,
 * glossary `regular_token`, `token_automaton`, D290): nonterminals whose
 * language is regular and whose XML is their text or nothing, compiled
 * once with the grammar into deterministic automata that the Earley
 * parser of src/ixml_earley.c runs over the input in one step.
 *
 * A nonterminal is a token when, through every nonterminal it uses:
 * - there is no recursion but the left recursion of a repetition (a
 *   production that starts with its own nonterminal), so the language is
 *   regular and the expansion below ends;
 * - every nonterminal used inside is hidden, and there are no insertions,
 *   so the XML is the text of the terminals read; and the terminals are
 *   all kept or all hidden, so that text is the whole input it matched or
 *   nothing (the Specification, "Constructing XML");
 * - the expansion stays small (SXML_IXML_TOKEN_MAXIMUM_SIZE states), it
 *   reads at most SXML_IXML_TOKEN_MAXIMUM_TERMINAL_COUNT distinct
 *   terminals, and its automaton has at most
 *   SXML_IXML_TOKEN_MAXIMUM_STATE_COUNT states.
 * Only such nonterminals used by a nonterminal that is not one get an
 * automaton: inside a token, the parser never predicts the others.
 *
 * The nondeterministic automaton is the grammar expanded in place
 * (Thompson's construction): a production is a chain of states, a
 * nonterminal a choice of its productions, a left recursion A: A, t a
 * loop. Each derivation of a text from the nonterminal is one path, and
 * each path one derivation, so the number of paths is the number of parse
 * trees (the Specification, "Parsing": ambiguity). The deterministic
 * automaton is made by the subset construction, keeping for each state of
 * the nondeterministic one the number of paths that reach it, one or
 * more: an end reached by more than one path is an ambiguous completion,
 * as the Earley parser would mark it. Paths are counted through empty
 * moves too, so an empty loop (a repetition of something that can be
 * empty) counts as more than one, as Earley's items do.
 *
 * A column of an automaton is the set of the token's terminals the next
 * character is in (bit i for the token's terminal i); the columns of ASCII
 * are computed here, others from the character's codepoint class.
 *
 * No recursion (D71): the analysis is a depth-first walk with an explicit
 * stack, the expansion a list of tasks.
 *
 * Golden file: src/ixml_tables.c (work lists over the compiled grammar,
 * arrays in allocator memory).
 */
#include "ixml_internal.h"

#include "memory_internal.h"

#include <stdlib.h>
#include <string.h>

/* The most states the nondeterministic automaton of a token may have. */
#define SXML_IXML_TOKEN_MAXIMUM_SIZE 512U

/* The bits of a 64-bit word. */
#define SXML_IXML_WORD_BIT_COUNT 64U

/* What the XML of a nonterminal's text can be, found through the
   nonterminals it uses. */
#define SXML_IXML_SHAPE_KEPT_TERMINAL 1U
#define SXML_IXML_SHAPE_HIDDEN_TERMINAL 2U
/* Recursion, an insertion, or a nonterminal shown as an element or an
   attribute inside. */
#define SXML_IXML_SHAPE_IRREGULAR 4U

/* The colors of the depth-first walk. */
#define SXML_IXML_COLOR_NEW 0U
#define SXML_IXML_COLOR_OPEN 1U
#define SXML_IXML_COLOR_DONE 2U

typedef struct token_finder {
    const sxml_ixml_grammar *grammar;
    sxml_ixml_parse_tables *tables;
    sxml_allocator allocator;
    const sxml_ixml_nonterminal *nonterminals;
    size_t nonterminal_count;
    const sxml_ixml_production *productions;
    const sxml_ixml_symbol *symbols;
    /* Per nonterminal: its shape, the size of its expansion (saturating
       above SXML_IXML_TOKEN_MAXIMUM_SIZE), whether it can be a token. */
    uint8_t *shapes;
    uint32_t *sizes;
    bool *is_candidate;
} token_finder;

static sxml_status allocate_zeroed(sxml_allocator allocator, sxml_array_allocation_request request,
                                   void **memory) {
    const sxml_status status = sxml_allocator_allocate_array(allocator, request, memory);
    if (status == SXML_STATUS_OK && *memory != NULL) {
        memset(*memory, 0, request.element_count * request.element_size);
    }
    return status;
}

static const sxml_ixml_symbol *symbol_of(const token_finder *finder, uint32_t production,
                                         uint32_t offset) {
    return &finder->symbols[finder->productions[production].first_symbol + offset];
}

/* The mark of a use of a nonterminal: the use's, else its rule's, else an
   element (the Specification, "Constructing XML"). */
static sxml_ixml_mark use_mark(const token_finder *finder, const sxml_ixml_symbol *symbol) {
    sxml_ixml_mark mark = (sxml_ixml_mark)symbol->mark;
    if (mark == SXML_IXML_MARK_NONE) {
        mark = (sxml_ixml_mark)finder->nonterminals[symbol->target].mark;
    }
    return mark == SXML_IXML_MARK_NONE ? SXML_IXML_MARK_ELEMENT : mark;
}

/* True when the symbol at offset of production is its nonterminal's own
   left recursion. */
typedef struct symbol_place {
    uint32_t production;
    uint32_t offset;
} symbol_place;

static bool is_left_recursion(const token_finder *finder, symbol_place place) {
    const sxml_ixml_symbol *symbol = symbol_of(finder, place.production, place.offset);
    return place.offset == 0 && symbol->symbol_kind == SXML_IXML_SYMBOL_KIND_NONTERMINAL &&
           symbol->target == finder->productions[place.production].nonterminal;
}

static uint32_t add_size(uint32_t size, uint32_t added) {
    const uint32_t sum = size + added;
    return sum > SXML_IXML_TOKEN_MAXIMUM_SIZE || sum < size ? SXML_IXML_TOKEN_MAXIMUM_SIZE + 1U : sum;
}

/* The shape and size of nonterminal, whose uses are done (or open, in a
   cycle that has marked it irregular already). */
static void finish_nonterminal(token_finder *finder, uint32_t nonterminal) {
    const sxml_ixml_nonterminal *definition = &finder->nonterminals[nonterminal];
    uint8_t shape = finder->shapes[nonterminal];
    uint32_t size = 1;
    for (uint32_t production_offset = 0; production_offset < definition->production_count;
         production_offset += 1) {
        const uint32_t production = definition->first_production + production_offset;
        size = add_size(size, finder->productions[production].symbol_count + 1U);
        for (uint32_t offset = 0; offset < finder->productions[production].symbol_count;
             offset += 1) {
            const sxml_ixml_symbol *symbol = symbol_of(finder, production, offset);
            switch ((sxml_ixml_symbol_kind)symbol->symbol_kind) {
            case SXML_IXML_SYMBOL_KIND_TERMINAL:
                shape |= symbol->mark == SXML_IXML_MARK_HIDDEN ? SXML_IXML_SHAPE_HIDDEN_TERMINAL
                                                               : SXML_IXML_SHAPE_KEPT_TERMINAL;
                break;
            case SXML_IXML_SYMBOL_KIND_INSERTION:
                shape |= SXML_IXML_SHAPE_IRREGULAR;
                break;
            case SXML_IXML_SYMBOL_KIND_NONTERMINAL:
                if (use_mark(finder, symbol) != SXML_IXML_MARK_HIDDEN) {
                    shape |= SXML_IXML_SHAPE_IRREGULAR;
                }
                if (!is_left_recursion(finder, (symbol_place){.production = production,
                                                              .offset = offset})) {
                    shape |= finder->shapes[symbol->target];
                    size = add_size(size, finder->sizes[symbol->target]);
                }
                break;
            }
        }
    }
    finder->shapes[nonterminal] = shape;
    finder->sizes[nonterminal] = size;
}

/* A nonterminal on the walk's stack and where its uses are read. */
typedef struct walk_frame {
    uint32_t nonterminal;
    uint32_t production_offset;
    uint32_t symbol_offset;
} walk_frame;

/* Finds the shape and size of every nonterminal, children first, by a
   depth-first walk over the uses (not the left recursions): a use of an
   open nonterminal closes a cycle, and every nonterminal on it is
   irregular. */
static sxml_status find_shapes(token_finder *finder) {
    const size_t count = finder->nonterminal_count;
    void *memory = NULL;
    sxml_status status = allocate_zeroed(
        finder->allocator,
        (sxml_array_allocation_request){
            .element_count = count, .element_size = sizeof(uint8_t), .element_alignment = 1},
        &memory);
    uint8_t *colors = memory;
    walk_frame *frames = NULL;
    uint32_t *frame_of = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(finder->allocator,
                                 (sxml_array_allocation_request){.element_count = count,
                                                                 .element_size = sizeof(walk_frame),
                                                                 .element_alignment = _Alignof(walk_frame)},
                                 &memory);
        frames = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(finder->allocator,
                                 (sxml_array_allocation_request){.element_count = count,
                                                                 .element_size = sizeof(uint32_t),
                                                                 .element_alignment = _Alignof(uint32_t)},
                                 &memory);
        frame_of = memory;
    }
    for (uint32_t start = 0; status == SXML_STATUS_OK && start < count; start += 1) {
        if (colors[start] != SXML_IXML_COLOR_NEW) {
            continue;
        }
        size_t frame_count = 1;
        frames[0] = (walk_frame){.nonterminal = start, .production_offset = 0, .symbol_offset = 0};
        frame_of[start] = 0;
        colors[start] = SXML_IXML_COLOR_OPEN;
        while (frame_count > 0) {
            walk_frame *frame = &frames[frame_count - 1];
            const sxml_ixml_nonterminal *definition = &finder->nonterminals[frame->nonterminal];
            if (frame->production_offset == definition->production_count) {
                finish_nonterminal(finder, frame->nonterminal);
                colors[frame->nonterminal] = SXML_IXML_COLOR_DONE;
                frame_count -= 1;
                continue;
            }
            const uint32_t production = definition->first_production + frame->production_offset;
            if (frame->symbol_offset == finder->productions[production].symbol_count) {
                frame->production_offset += 1;
                frame->symbol_offset = 0;
                continue;
            }
            const symbol_place place = {.production = production, .offset = frame->symbol_offset};
            frame->symbol_offset += 1;
            const sxml_ixml_symbol *symbol = symbol_of(finder, place.production, place.offset);
            if (symbol->symbol_kind != SXML_IXML_SYMBOL_KIND_NONTERMINAL ||
                is_left_recursion(finder, place)) {
                continue;
            }
            const uint32_t target = symbol->target;
            if (colors[target] == SXML_IXML_COLOR_OPEN) {
                for (size_t cycle_index = frame_of[target]; cycle_index < frame_count;
                     cycle_index += 1) {
                    finder->shapes[frames[cycle_index].nonterminal] |= SXML_IXML_SHAPE_IRREGULAR;
                }
            } else if (colors[target] == SXML_IXML_COLOR_NEW) {
                colors[target] = SXML_IXML_COLOR_OPEN;
                frame_of[target] = (uint32_t)frame_count;
                frames[frame_count] =
                    (walk_frame){.nonterminal = target, .production_offset = 0, .symbol_offset = 0};
                frame_count += 1;
            }
        }
    }
    for (uint32_t nonterminal = 0; status == SXML_STATUS_OK && nonterminal < count;
         nonterminal += 1) {
        const uint8_t shape = finder->shapes[nonterminal];
        finder->is_candidate[nonterminal] =
            nonterminal != 0 && (shape & SXML_IXML_SHAPE_IRREGULAR) == 0 &&
            (shape & (SXML_IXML_SHAPE_KEPT_TERMINAL | SXML_IXML_SHAPE_HIDDEN_TERMINAL)) !=
                (SXML_IXML_SHAPE_KEPT_TERMINAL | SXML_IXML_SHAPE_HIDDEN_TERMINAL) &&
            finder->sizes[nonterminal] <= SXML_IXML_TOKEN_MAXIMUM_SIZE;
    }
    sxml_allocator_release(finder->allocator, frame_of);
    sxml_allocator_release(finder->allocator, frames);
    sxml_allocator_release(finder->allocator, colors);
    return status;
}

/* ------------------------------------------- The nondeterministic automaton */

/* A move of the nondeterministic automaton: on the token's terminal
   terminal, or empty (SXML_IXML_NONE). */
typedef struct nfa_edge {
    uint32_t source;
    uint32_t terminal;
    uint32_t target;
} nfa_edge;

typedef enum nfa_task_kind {
    /* A nonterminal from source to target. */
    SXML_IXML_NFA_TASK_KIND_NONTERMINAL,
    /* A production's symbols from symbol_offset on, from source to
       target. */
    SXML_IXML_NFA_TASK_KIND_SEQUENCE,
} nfa_task_kind;

typedef struct nfa_task {
    uint32_t kind; /* nfa_task_kind */
    /* The nonterminal or the production. */
    uint32_t subject;
    uint32_t symbol_offset;
    uint32_t source;
    uint32_t target;
} nfa_task;

/* What compiling one token keeps: its automata in the making. State 0 is
   the start of the nondeterministic automaton, state 1 its end. */
typedef struct token_builder {
    token_finder *finder;
    uint32_t nonterminal;
    sxml_xml_array edges; /* nfa_edge */
    sxml_xml_array tasks; /* nfa_task */
    uint32_t state_count;
    /* The token's distinct terminals, in column-bit order. */
    uint32_t terminals[SXML_IXML_TOKEN_MAXIMUM_TERMINAL_COUNT];
    uint32_t terminal_count;
    /* The expansion broke a limit: not a token. */
    bool is_too_large;
    /* Edges by source: for state s, edges edge_starts[s] to
       edge_starts[s + 1] of sorted_edges. */
    uint32_t *edge_starts;
    nfa_edge *sorted_edges;
} token_builder;

static sxml_status push_task(token_builder *builder, nfa_task task) {
    void *element = NULL;
    const sxml_status status = sxml_xml_array_push(&builder->tasks, &element);
    if (status == SXML_STATUS_OK) {
        *(nfa_task *)element = task;
    }
    return status;
}

static sxml_status push_edge(token_builder *builder, nfa_edge edge) {
    void *element = NULL;
    const sxml_status status = sxml_xml_array_push(&builder->edges, &element);
    if (status == SXML_STATUS_OK) {
        *(nfa_edge *)element = edge;
    }
    return status;
}

static uint32_t new_state(token_builder *builder) {
    if (builder->state_count >= SXML_IXML_TOKEN_MAXIMUM_SIZE) {
        builder->is_too_large = true;
        return 0;
    }
    builder->state_count += 1;
    return builder->state_count - 1;
}

/* The column bit of a distinct terminal of the grammar in the token. */
static uint32_t token_terminal(token_builder *builder, uint32_t distinct) {
    for (uint32_t terminal_index = 0; terminal_index < builder->terminal_count; terminal_index += 1) {
        if (builder->terminals[terminal_index] == distinct) {
            return terminal_index;
        }
    }
    if (builder->terminal_count == SXML_IXML_TOKEN_MAXIMUM_TERMINAL_COUNT) {
        builder->is_too_large = true;
        return 0;
    }
    builder->terminals[builder->terminal_count] = distinct;
    builder->terminal_count += 1;
    return builder->terminal_count - 1;
}

/* Expands a nonterminal between two states: one path per production, a
   loop for its left recursions. */
static sxml_status expand_nonterminal(token_builder *builder, const nfa_task *task) {
    const token_finder *finder = builder->finder;
    const sxml_ixml_nonterminal *definition = &finder->nonterminals[task->subject];
    bool has_loop = false;
    for (uint32_t production_offset = 0; production_offset < definition->production_count;
         production_offset += 1) {
        const uint32_t production = definition->first_production + production_offset;
        has_loop = has_loop ||
                   (finder->productions[production].symbol_count > 0 &&
                    is_left_recursion(finder, (symbol_place){.production = production, .offset = 0}));
    }
    uint32_t loop = task->target;
    sxml_status status = SXML_STATUS_OK;
    if (has_loop) {
        loop = new_state(builder);
        status = push_edge(builder, (nfa_edge){.source = loop,
                                               .terminal = SXML_IXML_NONE,
                                               .target = task->target});
    }
    for (uint32_t production_offset = 0;
         status == SXML_STATUS_OK && production_offset < definition->production_count;
         production_offset += 1) {
        const uint32_t production = definition->first_production + production_offset;
        const bool is_recursive =
            finder->productions[production].symbol_count > 0 &&
            is_left_recursion(finder, (symbol_place){.production = production, .offset = 0});
        status = push_task(builder, (nfa_task){.kind = SXML_IXML_NFA_TASK_KIND_SEQUENCE,
                                               .subject = production,
                                               .symbol_offset = is_recursive ? 1U : 0U,
                                               .source = is_recursive ? loop : task->source,
                                               .target = loop});
    }
    return status;
}

/* Expands the symbols of a production from symbol_offset on. */
static sxml_status expand_sequence(token_builder *builder, const nfa_task *task) {
    const token_finder *finder = builder->finder;
    const uint32_t symbol_count = finder->productions[task->subject].symbol_count;
    if (task->symbol_offset == symbol_count) {
        return push_edge(builder, (nfa_edge){.source = task->source,
                                             .terminal = SXML_IXML_NONE,
                                             .target = task->target});
    }
    const sxml_ixml_symbol *symbol = symbol_of(finder, task->subject, task->symbol_offset);
    const bool is_last = task->symbol_offset + 1 == symbol_count;
    const uint32_t middle = is_last ? task->target : new_state(builder);
    sxml_status status = SXML_STATUS_OK;
    if (symbol->symbol_kind == SXML_IXML_SYMBOL_KIND_TERMINAL) {
        const uint32_t distinct = finder->tables->distinct_terminals[symbol->target];
        status = push_edge(builder, (nfa_edge){.source = task->source,
                                               .terminal = token_terminal(builder, distinct),
                                               .target = middle});
    } else {
        status = push_task(builder, (nfa_task){.kind = SXML_IXML_NFA_TASK_KIND_NONTERMINAL,
                                               .subject = symbol->target,
                                               .symbol_offset = 0,
                                               .source = task->source,
                                               .target = middle});
    }
    if (status == SXML_STATUS_OK && !is_last) {
        status = push_task(builder, (nfa_task){.kind = SXML_IXML_NFA_TASK_KIND_SEQUENCE,
                                               .subject = task->subject,
                                               .symbol_offset = task->symbol_offset + 1,
                                               .source = middle,
                                               .target = task->target});
    }
    return status;
}

/* Expands the token's nonterminal into the nondeterministic automaton,
   then sorts its edges by source. */
static sxml_status build_nfa(token_builder *builder) {
    builder->state_count = 2;
    sxml_status status = push_task(builder, (nfa_task){.kind = SXML_IXML_NFA_TASK_KIND_NONTERMINAL,
                                                       .subject = builder->nonterminal,
                                                       .symbol_offset = 0,
                                                       .source = 0,
                                                       .target = 1});
    while (status == SXML_STATUS_OK && !builder->is_too_large && builder->tasks.element_count > 0) {
        builder->tasks.element_count -= 1;
        const nfa_task task =
            *(const nfa_task *)sxml_xml_array_at(&builder->tasks, builder->tasks.element_count);
        status = task.kind == SXML_IXML_NFA_TASK_KIND_NONTERMINAL ? expand_nonterminal(builder, &task)
                                                        : expand_sequence(builder, &task);
    }
    if (status != SXML_STATUS_OK || builder->is_too_large) {
        return status;
    }
    const sxml_allocator allocator = builder->finder->allocator;
    void *memory = NULL;
    status = allocate_zeroed(allocator,
                             (sxml_array_allocation_request){.element_count = builder->state_count + 1U,
                                                             .element_size = sizeof(uint32_t),
                                                             .element_alignment = _Alignof(uint32_t)},
                             &memory);
    builder->edge_starts = memory;
    const size_t edge_count = builder->edges.element_count;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(
            allocator,
            (sxml_array_allocation_request){.element_count = edge_count + 1U,
                                            .element_size = sizeof(nfa_edge),
                                            .element_alignment = _Alignof(nfa_edge)},
            &memory);
        builder->sorted_edges = memory;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const nfa_edge *edges = (const nfa_edge *)builder->edges.elements;
    for (size_t edge_index = 0; edge_index < edge_count; edge_index += 1) {
        builder->edge_starts[edges[edge_index].source + 1] += 1;
    }
    for (uint32_t state = 0; state < builder->state_count; state += 1) {
        builder->edge_starts[state + 1] += builder->edge_starts[state];
    }
    /* Filled in order through cursors, kept in the starts and shifted
       back after. */
    for (size_t edge_index = 0; edge_index < edge_count; edge_index += 1) {
        builder->sorted_edges[builder->edge_starts[edges[edge_index].source]] = edges[edge_index];
        builder->edge_starts[edges[edge_index].source] += 1;
    }
    for (uint32_t state = builder->state_count; state > 0; state -= 1) {
        builder->edge_starts[state] = builder->edge_starts[state - 1];
    }
    builder->edge_starts[0] = 0;
    return SXML_STATUS_OK;
}

/* --------------------------------------------- The deterministic automaton */

/* A state of the nondeterministic automaton in a deterministic state,
   with the number of paths that reach it (1, or 2 for more). */
typedef struct counted_state {
    uint32_t state;
    uint32_t path_count;
} counted_state;

/* A deterministic state: entry_count counted states from first_entry of
   the entries, sorted by state. */
typedef struct dfa_state {
    uint32_t first_entry;
    uint32_t entry_count;
} dfa_state;

typedef struct subset_builder {
    token_builder *token;
    sxml_xml_array entries; /* counted_state */
    sxml_xml_array states;  /* dfa_state */
    /* Open addressing over the states, by their entries. */
    uint32_t *slots;
    size_t slot_capacity;
    /* Per state of the nondeterministic automaton: its path count in the
       closure being made, and the work list of the closure. */
    uint8_t *path_counts;
    sxml_xml_array changed; /* counted_state: a state and the paths added */
    sxml_xml_array touched; /* uint32_t */
} subset_builder;

/* Adds path_count paths to state, at most 2, and queues what changed. */
typedef struct path_addition {
    uint32_t state;
    uint32_t path_count;
} path_addition;

static sxml_status add_paths(subset_builder *builder, path_addition addition) {
    const uint32_t old_count = builder->path_counts[addition.state];
    uint32_t new_count = old_count + addition.path_count;
    new_count = new_count > 2U ? 2U : new_count;
    if (new_count == old_count) {
        return SXML_STATUS_OK;
    }
    void *element = NULL;
    sxml_status status = SXML_STATUS_OK;
    if (old_count == 0) {
        status = sxml_xml_array_push(&builder->touched, &element);
        if (status == SXML_STATUS_OK) {
            *(uint32_t *)element = addition.state;
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_xml_array_push(&builder->changed, &element);
    }
    if (status == SXML_STATUS_OK) {
        builder->path_counts[addition.state] = (uint8_t)new_count;
        *(counted_state *)element =
            (counted_state){.state = addition.state, .path_count = new_count - old_count};
    }
    return status;
}

/* Follows the empty moves from the states added so far, adding to each
   the paths that reach it (capped at 2, which is all that matters:
   min(2, a + b) = min(2, min(2, a) + b), so the order does not). */
static sxml_status close_paths(subset_builder *builder) {
    const token_builder *token = builder->token;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && builder->changed.element_count > 0) {
        builder->changed.element_count -= 1;
        const counted_state changed =
            *(const counted_state *)sxml_xml_array_at(&builder->changed, builder->changed.element_count);
        for (uint32_t edge_index = token->edge_starts[changed.state];
             status == SXML_STATUS_OK && edge_index < token->edge_starts[changed.state + 1];
             edge_index += 1) {
            const nfa_edge *edge = &token->sorted_edges[edge_index];
            if (edge->terminal == SXML_IXML_NONE) {
                status = add_paths(builder, (path_addition){.state = edge->target,
                                                            .path_count = changed.path_count});
            }
        }
    }
    return status;
}

static int compare_states(const void *left, const void *right) {
    const uint32_t left_state = *(const uint32_t *)left;
    const uint32_t right_state = *(const uint32_t *)right;
    return left_state < right_state ? -1 : left_state > right_state ? 1 : 0;
}

static uint64_t hash_entries(const counted_state *entries, size_t entry_count) {
    uint64_t hash = entry_count;
    for (size_t entry_index = 0; entry_index < entry_count; entry_index += 1) {
        hash = (hash ^ entries[entry_index].state) * 0x9E3779B97F4A7C15ULL;
        hash = (hash ^ entries[entry_index].path_count) * 0x9E3779B97F4A7C15ULL;
    }
    return hash ^ (hash >> 29U);
}

static bool has_terminal_edge(const token_builder *token, uint32_t state) {
    for (uint32_t edge_index = token->edge_starts[state]; edge_index < token->edge_starts[state + 1];
         edge_index += 1) {
        if (token->sorted_edges[edge_index].terminal != SXML_IXML_NONE) {
            return true;
        }
    }
    return false;
}

static sxml_status grow_slots(subset_builder *builder) {
    const size_t capacity = builder->slot_capacity == 0 ? 64 : builder->slot_capacity * 2;
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate_array(
        builder->token->finder->allocator,
        (sxml_array_allocation_request){
            .element_count = capacity, .element_size = sizeof(uint32_t), .element_alignment = _Alignof(uint32_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t *slots = memory;
    memset(slots, 0xFF, capacity * sizeof(uint32_t));
    const counted_state *entries = (const counted_state *)builder->entries.elements;
    for (uint32_t state_index = 0; state_index < builder->states.element_count; state_index += 1) {
        const dfa_state *state = sxml_xml_array_at(&builder->states, state_index);
        size_t slot = hash_entries(&entries[state->first_entry], state->entry_count) & (capacity - 1);
        while (slots[slot] != SXML_IXML_NONE) {
            slot = (slot + 1) & (capacity - 1);
        }
        slots[slot] = state_index;
    }
    sxml_allocator_release(builder->token->finder->allocator, builder->slots);
    builder->slots = slots;
    builder->slot_capacity = capacity;
    return SXML_STATUS_OK;
}

/* Turns the closure just made into a deterministic state: its states
   that read a terminal, and the end, with their path counts; the state
   found or added goes to *state_index (SXML_IXML_TOKEN_DEAD for none).
   Resets the closure. */
static sxml_status intern_closure(subset_builder *builder, uint32_t *state_index) {
    const token_builder *token = builder->token;
    uint32_t *touched = (uint32_t *)builder->touched.elements;
    const size_t touched_count = builder->touched.element_count;
    if (touched_count > 1) {
        qsort(touched, touched_count, sizeof(uint32_t), compare_states);
    }
    const size_t first_entry = builder->entries.element_count;
    sxml_status status = SXML_STATUS_OK;
    for (size_t touched_index = 0; status == SXML_STATUS_OK && touched_index < touched_count;
         touched_index += 1) {
        const uint32_t state = touched[touched_index];
        if (state == 1U || has_terminal_edge(token, state)) {
            void *element = NULL;
            status = sxml_xml_array_push(&builder->entries, &element);
            if (status == SXML_STATUS_OK) {
                *(counted_state *)element =
                    (counted_state){.state = state, .path_count = builder->path_counts[state]};
            }
        }
        builder->path_counts[state] = 0;
    }
    builder->touched.element_count = 0;
    const size_t entry_count = builder->entries.element_count - first_entry;
    *state_index = SXML_IXML_TOKEN_DEAD;
    if (status != SXML_STATUS_OK || entry_count == 0) {
        builder->entries.element_count = first_entry;
        return status;
    }
    if ((builder->states.element_count + 1) * 2 > builder->slot_capacity) {
        status = grow_slots(builder);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    const counted_state *entries = (const counted_state *)builder->entries.elements;
    const size_t mask = builder->slot_capacity - 1;
    size_t slot = hash_entries(&entries[first_entry], entry_count) & mask;
    while (builder->slots[slot] != SXML_IXML_NONE) {
        const dfa_state *known = sxml_xml_array_at(&builder->states, builder->slots[slot]);
        if (known->entry_count == entry_count &&
            memcmp(&entries[known->first_entry], &entries[first_entry],
                   entry_count * sizeof(counted_state)) == 0) {
            builder->entries.element_count = first_entry;
            *state_index = builder->slots[slot];
            return SXML_STATUS_OK;
        }
        slot = (slot + 1) & mask;
    }
    if (builder->states.element_count >= SXML_IXML_TOKEN_MAXIMUM_STATE_COUNT) {
        builder->token->is_too_large = true;
        return SXML_STATUS_OK;
    }
    void *element = NULL;
    status = sxml_xml_array_push(&builder->states, &element);
    if (status == SXML_STATUS_OK) {
        *(dfa_state *)element =
            (dfa_state){.first_entry = (uint32_t)first_entry, .entry_count = (uint32_t)entry_count};
        *state_index = (uint32_t)builder->states.element_count - 1U;
        builder->slots[slot] = *state_index;
    }
    return status;
}

/* The state after state_index reads a character of column column. */
typedef struct transition_request {
    uint32_t state_index;
    uint32_t column;
} transition_request;

static sxml_status find_transition(subset_builder *builder, transition_request request,
                                   uint32_t *next_state) {
    const token_builder *token = builder->token;
    const dfa_state state = *(const dfa_state *)sxml_xml_array_at(&builder->states, request.state_index);
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t entry_index = 0; status == SXML_STATUS_OK && entry_index < state.entry_count;
         entry_index += 1) {
        const counted_state entry =
            *(const counted_state *)sxml_xml_array_at(&builder->entries, state.first_entry + entry_index);
        for (uint32_t edge_index = token->edge_starts[entry.state];
             status == SXML_STATUS_OK && edge_index < token->edge_starts[entry.state + 1];
             edge_index += 1) {
            const nfa_edge *edge = &token->sorted_edges[edge_index];
            if (edge->terminal != SXML_IXML_NONE && ((request.column >> edge->terminal) & 1U) != 0) {
                status = add_paths(builder, (path_addition){.state = edge->target,
                                                            .path_count = entry.path_count});
            }
        }
    }
    if (status == SXML_STATUS_OK) {
        status = close_paths(builder);
    }
    if (status == SXML_STATUS_OK) {
        status = intern_closure(builder, next_state);
    }
    return status;
}

/* Appends the token's automaton to the tables: transitions, ends, its
   terminals and the columns of ASCII. */
static sxml_status store_token(subset_builder *builder, const sxml_xml_array *transitions) {
    token_builder *token = builder->token;
    sxml_ixml_parse_tables *tables = token->finder->tables;
    const sxml_ixml_nonterminal *definition = &token->finder->nonterminals[token->nonterminal];
    const sxml_ixml_production *first = &token->finder->productions[definition->first_production];
    const sxml_ixml_token stored = {
        .nonterminal = token->nonterminal,
        .completed_rule = tables->first_dotted_rules[definition->first_production] + first->symbol_count,
        .first_transition = (uint32_t)tables->token_transitions.element_count,
        .first_state = (uint32_t)tables->token_accepts.element_count,
        .state_count = (uint32_t)builder->states.element_count,
        .first_terminal = (uint32_t)tables->token_terminals.element_count,
        .terminal_count = token->terminal_count,
        .keeps_text = (token->finder->shapes[token->nonterminal] & SXML_IXML_SHAPE_KEPT_TERMINAL) != 0};
    void *element = NULL;
    sxml_status status = sxml_xml_array_push(&tables->tokens, &element);
    if (status == SXML_STATUS_OK) {
        *(sxml_ixml_token *)element = stored;
    }
    for (size_t transition_index = 0;
         status == SXML_STATUS_OK && transition_index < transitions->element_count;
         transition_index += 1) {
        status = sxml_xml_array_push(&tables->token_transitions, &element);
        if (status == SXML_STATUS_OK) {
            *(uint16_t *)element = *(const uint16_t *)sxml_xml_array_at(transitions, transition_index);
        }
    }
    for (uint32_t state_index = 0;
         status == SXML_STATUS_OK && state_index < builder->states.element_count; state_index += 1) {
        const dfa_state *state = sxml_xml_array_at(&builder->states, state_index);
        uint8_t path_count = 0;
        for (uint32_t entry_index = 0; entry_index < state->entry_count; entry_index += 1) {
            const counted_state *entry =
                sxml_xml_array_at(&builder->entries, state->first_entry + entry_index);
            if (entry->state == 1U) {
                path_count = (uint8_t)entry->path_count;
            }
        }
        status = sxml_xml_array_push(&tables->token_accepts, &element);
        if (status == SXML_STATUS_OK) {
            *(uint8_t *)element = path_count;
        }
    }
    for (uint32_t terminal_index = 0;
         status == SXML_STATUS_OK && terminal_index < token->terminal_count; terminal_index += 1) {
        status = sxml_xml_array_push(&tables->token_terminals, &element);
        if (status == SXML_STATUS_OK) {
            *(uint32_t *)element = token->terminals[terminal_index];
        }
    }
    for (uint32_t codepoint = 0; status == SXML_STATUS_OK && codepoint < 128U; codepoint += 1) {
        const uint64_t *class_words = &tables->ascii_classes[codepoint * tables->word_count];
        uint32_t column = 0;
        for (uint32_t terminal_index = 0; terminal_index < token->terminal_count; terminal_index += 1) {
            const uint32_t distinct = token->terminals[terminal_index];
            if (((class_words[distinct / SXML_IXML_WORD_BIT_COUNT] >> (distinct % SXML_IXML_WORD_BIT_COUNT)) &
                 1U) != 0) {
                column |= 1U << terminal_index;
            }
        }
        status = sxml_xml_array_push(&tables->token_ascii_columns, &element);
        if (status == SXML_STATUS_OK) {
            *(uint8_t *)element = (uint8_t)column;
        }
    }
    return status;
}

/* Makes the deterministic automaton of a token whose nondeterministic one
   is built, and stores it unless it is too large. */
static sxml_status build_dfa(token_builder *token) {
    const sxml_allocator allocator = token->finder->allocator;
    subset_builder builder = {
        .token = token,
        .entries = sxml_xml_array_empty(
            (sxml_xml_array_configuration){.allocator = allocator, .element_byte_count = sizeof(counted_state)}),
        .states = sxml_xml_array_empty(
            (sxml_xml_array_configuration){.allocator = allocator, .element_byte_count = sizeof(dfa_state)}),
        .slots = NULL,
        .slot_capacity = 0,
        .path_counts = NULL,
        .changed = sxml_xml_array_empty(
            (sxml_xml_array_configuration){.allocator = allocator, .element_byte_count = sizeof(counted_state)}),
        .touched = sxml_xml_array_empty(
            (sxml_xml_array_configuration){.allocator = allocator, .element_byte_count = sizeof(uint32_t)})};
    sxml_xml_array transitions = sxml_xml_array_empty(
        (sxml_xml_array_configuration){.allocator = allocator, .element_byte_count = sizeof(uint16_t)});
    void *memory = NULL;
    sxml_status status = allocate_zeroed(
        allocator,
        (sxml_array_allocation_request){
            .element_count = token->state_count, .element_size = sizeof(uint8_t), .element_alignment = 1},
        &memory);
    builder.path_counts = memory;
    uint32_t start = SXML_IXML_TOKEN_DEAD;
    if (status == SXML_STATUS_OK) {
        status = add_paths(&builder, (path_addition){.state = 0, .path_count = 1});
    }
    if (status == SXML_STATUS_OK) {
        status = close_paths(&builder);
    }
    if (status == SXML_STATUS_OK) {
        status = intern_closure(&builder, &start);
    }
    if (status == SXML_STATUS_OK && start == SXML_IXML_TOKEN_DEAD) {
        /* A language with no text at all, not even the empty one: no
           state to start in. */
        token->is_too_large = true;
    }
    const uint32_t column_count = 1U << token->terminal_count;
    for (uint32_t state_index = 0; status == SXML_STATUS_OK && !token->is_too_large &&
                                   state_index < builder.states.element_count;
         state_index += 1) {
        for (uint32_t column = 0; status == SXML_STATUS_OK && !token->is_too_large && column < column_count;
             column += 1) {
            uint32_t next = SXML_IXML_TOKEN_DEAD;
            status = find_transition(&builder, (transition_request){.state_index = state_index, .column = column},
                                     &next);
            void *element = NULL;
            if (status == SXML_STATUS_OK) {
                status = sxml_xml_array_push(&transitions, &element);
            }
            if (status == SXML_STATUS_OK) {
                *(uint16_t *)element = (uint16_t)next;
            }
        }
    }
    if (status == SXML_STATUS_OK && !token->is_too_large) {
        status = store_token(&builder, &transitions);
    }
    sxml_xml_array_release(&transitions);
    sxml_xml_array_release(&builder.entries);
    sxml_xml_array_release(&builder.states);
    sxml_xml_array_release(&builder.changed);
    sxml_xml_array_release(&builder.touched);
    sxml_allocator_release(allocator, builder.slots);
    sxml_allocator_release(allocator, builder.path_counts);
    return status;
}

/* Compiles nonterminal as a token; *is_token says whether it became
   one. */
static sxml_status compile_token(token_finder *finder, uint32_t nonterminal, bool *is_token) {
    const sxml_allocator allocator = finder->allocator;
    token_builder builder = {
        .finder = finder,
        .nonterminal = nonterminal,
        .edges = sxml_xml_array_empty(
            (sxml_xml_array_configuration){.allocator = allocator, .element_byte_count = sizeof(nfa_edge)}),
        .tasks = sxml_xml_array_empty(
            (sxml_xml_array_configuration){.allocator = allocator, .element_byte_count = sizeof(nfa_task)}),
        .state_count = 0,
        .terminal_count = 0,
        .is_too_large = false,
        .edge_starts = NULL,
        .sorted_edges = NULL};
    sxml_status status = build_nfa(&builder);
    if (status == SXML_STATUS_OK && !builder.is_too_large) {
        status = build_dfa(&builder);
    }
    *is_token = status == SXML_STATUS_OK && !builder.is_too_large;
    sxml_xml_array_release(&builder.edges);
    sxml_xml_array_release(&builder.tasks);
    sxml_allocator_release(allocator, builder.edge_starts);
    sxml_allocator_release(allocator, builder.sorted_edges);
    return status;
}

/* Compiles the tokens that nonterminals which are not tokens use, and
   when one cannot be compiled, the tokens it uses in turn (a work list
   of nonterminals that are not tokens, from all the others). */
static sxml_status compile_tokens(token_finder *finder) {
    const size_t count = finder->nonterminal_count;
    sxml_ixml_parse_tables *tables = finder->tables;
    void *memory = NULL;
    sxml_status status =
        sxml_allocator_allocate_array(finder->allocator,
                                      (sxml_array_allocation_request){.element_count = count,
                                                                      .element_size = sizeof(uint32_t),
                                                                      .element_alignment = _Alignof(uint32_t)},
                                      &memory);
    uint32_t *work = memory;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(tables->tokens_by_nonterminal, 0xFF, count * sizeof(uint32_t));
    size_t work_count = 0;
    for (uint32_t nonterminal = 0; nonterminal < count; nonterminal += 1) {
        if (!finder->is_candidate[nonterminal]) {
            work[work_count] = nonterminal;
            work_count += 1;
        }
    }
    /* Whether a candidate was tried, kept in the candidate flag: tried
       ones that compiled have a token. */
    while (status == SXML_STATUS_OK && work_count > 0) {
        work_count -= 1;
        const sxml_ixml_nonterminal *definition = &finder->nonterminals[work[work_count]];
        for (uint32_t production_offset = 0;
             status == SXML_STATUS_OK && production_offset < definition->production_count;
             production_offset += 1) {
            const uint32_t production = definition->first_production + production_offset;
            for (uint32_t offset = 0;
                 status == SXML_STATUS_OK && offset < finder->productions[production].symbol_count;
                 offset += 1) {
                const sxml_ixml_symbol *symbol = symbol_of(finder, production, offset);
                if (symbol->symbol_kind != SXML_IXML_SYMBOL_KIND_NONTERMINAL ||
                    !finder->is_candidate[symbol->target] ||
                    tables->tokens_by_nonterminal[symbol->target] != SXML_IXML_NONE) {
                    continue;
                }
                const uint32_t token_index = (uint32_t)tables->tokens.element_count;
                bool is_token = false;
                status = compile_token(finder, symbol->target, &is_token);
                if (status == SXML_STATUS_OK && is_token) {
                    tables->tokens_by_nonterminal[symbol->target] = token_index;
                } else if (status == SXML_STATUS_OK) {
                    finder->is_candidate[symbol->target] = false;
                    work[work_count] = symbol->target;
                    work_count += 1;
                }
            }
        }
    }
    sxml_allocator_release(finder->allocator, work);
    return status;
}

/* Finds what can follow each token where it is used: the lookahead sets
   of the dotted rules just after its uses, outside its own productions
   (inside them, its left recursion is the automaton's). One pass over the
   grammar's productions. */
static sxml_status find_follows(token_finder *finder) {
    sxml_ixml_parse_tables *tables = finder->tables;
    const size_t word_count = tables->word_count;
    const size_t follow_count = tables->tokens.element_count * word_count;
    sxml_status status = SXML_STATUS_OK;
    for (size_t word_index = 0; status == SXML_STATUS_OK && word_index < follow_count; word_index += 1) {
        void *element = NULL;
        status = sxml_xml_array_push(&tables->token_follows, &element);
    }
    uint64_t *follows = (uint64_t *)tables->token_follows.elements;
    for (uint32_t nonterminal = 0; status == SXML_STATUS_OK && nonterminal < finder->nonterminal_count;
         nonterminal += 1) {
        const sxml_ixml_nonterminal *definition = &finder->nonterminals[nonterminal];
        for (uint32_t production_offset = 0; production_offset < definition->production_count;
             production_offset += 1) {
            const uint32_t production = definition->first_production + production_offset;
            for (uint32_t offset = 0; offset < finder->productions[production].symbol_count; offset += 1) {
                const sxml_ixml_symbol *symbol = symbol_of(finder, production, offset);
                if (symbol->symbol_kind != SXML_IXML_SYMBOL_KIND_NONTERMINAL ||
                    symbol->target == nonterminal ||
                    tables->tokens_by_nonterminal[symbol->target] == SXML_IXML_NONE) {
                    continue;
                }
                const uint32_t after = tables->first_dotted_rules[production] + offset + 1;
                const uint64_t *lookahead =
                    &tables->lookahead_words[(size_t)tables->dotted_rules[after].lookahead_set * word_count];
                uint64_t *follow = &follows[(size_t)tables->tokens_by_nonterminal[symbol->target] * word_count];
                for (size_t word_index = 0; word_index < word_count; word_index += 1) {
                    follow[word_index] |= lookahead[word_index];
                }
            }
        }
    }
    return status;
}

sxml_status sxml_ixml_prepare_tokens(sxml_ixml_grammar *grammar) {
    sxml_ixml_parse_tables *tables = &grammar->tables;
    const sxml_allocator allocator = tables->allocator;
    token_finder finder = {
        .grammar = grammar,
        .tables = tables,
        .allocator = allocator,
        .nonterminals = (const sxml_ixml_nonterminal *)grammar->nonterminals.elements,
        .nonterminal_count = grammar->nonterminals.element_count,
        .productions = (const sxml_ixml_production *)grammar->productions.elements,
        .symbols = (const sxml_ixml_symbol *)grammar->symbols.elements,
        .shapes = NULL,
        .sizes = NULL,
        .is_candidate = NULL};
    const size_t count = finder.nonterminal_count;
    void *memory = NULL;
    sxml_status status = allocate_zeroed(
        allocator,
        (sxml_array_allocation_request){
            .element_count = count, .element_size = sizeof(uint8_t), .element_alignment = 1},
        &memory);
    finder.shapes = memory;
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(allocator,
                                 (sxml_array_allocation_request){.element_count = count,
                                                                 .element_size = sizeof(uint32_t),
                                                                 .element_alignment = _Alignof(uint32_t)},
                                 &memory);
        finder.sizes = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_zeroed(allocator,
                                 (sxml_array_allocation_request){.element_count = count,
                                                                 .element_size = sizeof(bool),
                                                                 .element_alignment = _Alignof(bool)},
                                 &memory);
        finder.is_candidate = memory;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(allocator,
                                               (sxml_array_allocation_request){.element_count = count,
                                                                               .element_size = sizeof(uint32_t),
                                                                               .element_alignment = _Alignof(uint32_t)},
                                               &memory);
        tables->tokens_by_nonterminal = memory;
    }
    if (status == SXML_STATUS_OK && count > 0) {
        status = find_shapes(&finder);
    }
    if (status == SXML_STATUS_OK && count > 0) {
        status = compile_tokens(&finder);
    }
    if (status == SXML_STATUS_OK && count > 0) {
        status = find_follows(&finder);
    }
    sxml_allocator_release(allocator, finder.is_candidate);
    sxml_allocator_release(allocator, finder.sizes);
    sxml_allocator_release(allocator, finder.shapes);
    return status;
}
