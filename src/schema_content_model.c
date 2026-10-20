/*
 * Content models (src/schema_internal.h, "Content models"; docs/DECISIONS.md
 * D167): the particle of a complex type compiled into a deterministic
 * automaton, its checks (cos-nonambig, cos-element-consistent,
 * cos-all-limited, cos-content-act-restrict), the matcher the validator
 * steps, schema wildcard tests and the text form.
 *
 * Golden file: src/regex.c (a compiled program in one block, explicit
 * stacks, small repetitions expanded and large ones counted, D147), and
 * src/types.c (a store of descriptions read through accessors).
 *
 * Compilation (XSD 1.1 Part 1, 3.8 and 3.9; the position automaton of
 * Glushkov, as in Brueggemann-Klein and Wood, "One-unambiguous regular
 * languages", 1998, with counters as in Kilpelaeinen and Tuhkanen,
 * "One-unambiguity of regular expressions with numeric occurrence
 * indicators", 2007):
 * 1. The particle tree is walked with an explicit stack of build tasks
 *    (D71) into a tree of content nodes: leaves (content model positions,
 *    one per element or wildcard particle occurrence), sequences, choices,
 *    repetitions (?, *, +) and counted repetitions. A particle whose
 *    maxOccurs (or, unbounded, minOccurs) is at most the expansion bound is
 *    expanded into copies (a{2,4} is a a (a a?)?); a larger one keeps one
 *    copy of its term under a counted node, which owns a content model
 *    counter.
 * 2. Each node gets nullable, first and last positions, children before
 *    parents (nodes are made parent first, so a reverse walk suffices).
 * 3. Each follow pair becomes an edge with the counter tests and counter
 *    operations of the counted nodes it leaves (at least minOccurs),
 *    repeats (below maxOccurs, add one) and enters (set to one).
 * 4. Subset construction: a state is a set of positions; from a state the
 *    edges into the positions of one particle with one set of tests and
 *    operations make one transition. Unique Particle Attribution
 *    (cos-nonambig, 3.8.6.4) then says that no two transitions of a state
 *    for different element particles (or for different wildcard
 *    particles) can match one name under counter values both allow; an
 *    element particle and a wildcard particle may compete, and the
 *    element wins (3.8.6.4, notes). Two transitions of one particle whose
 *    tests overlap (the counters of (a{1,20}){1,20} cannot tell which one
 *    repeats) are not an error but leave the matcher without a single
 *    choice: the counted particles involved are then expanded instead and
 *    the model compiled again, within maximum_content_model_state_count.
 * An all group (3.8.4.1.3) compiles to two states, before and after its
 *    first member, whose transitions count each member in a counter of its
 *    own. Open content (3.4.4.2) and substitution groups (3.3.6.3) are
 *    applied by the matcher: a transition labelled with an element
 *    declaration lists the declarations substitutable for it.
 */
#include "schema_internal.h"

#include "processor_internal.h"
#include "sort_internal.h"
#include "text_form_internal.h"

#include <stdalign.h>
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------- Scratch arrays */

#define SCRATCH_ARRAY_FIRST_CAPACITY 16U

sxml_status sxml_schema_scratch_array_append(sxml_allocator allocator,
                                             sxml_schema_scratch_array *array, const void *entry) {
    if (array->entry_count == array->entry_capacity) {
        const size_t capacity =
            array->entry_capacity == 0 ? SCRATCH_ARRAY_FIRST_CAPACITY : array->entry_capacity * 2U;
        size_t byte_count = 0;
        if (capacity <= array->entry_capacity ||
            sxml_size_multiply(capacity, array->entry_byte_count, &byte_count) != SXML_STATUS_OK) {
            return SXML_STATUS_LIMIT_EXCEEDED;
        }
        void *memory = NULL;
        const sxml_status status = sxml_allocator_allocate(
            allocator,
            (sxml_allocation_request){.byte_count = byte_count, .alignment = alignof(max_align_t)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (array->entry_count != 0) {
            memcpy(memory, array->bytes, array->entry_count * array->entry_byte_count);
        }
        sxml_allocator_release(allocator, array->bytes);
        array->bytes = memory;
        array->entry_capacity = capacity;
    }
    memcpy(array->bytes + array->entry_count * array->entry_byte_count, entry,
           array->entry_byte_count);
    array->entry_count += 1;
    return SXML_STATUS_OK;
}

void sxml_schema_scratch_array_release(sxml_allocator allocator, sxml_schema_scratch_array *array) {
    sxml_allocator_release(allocator, array->bytes);
    array->bytes = NULL;
    array->entry_count = 0;
    array->entry_capacity = 0;
}

/* ------------------------------------------------------ Compiled form */

/* A run of entries of one of the model's arrays. */
typedef struct index_run {
    uint32_t first_index;
    uint32_t index_count;
} index_run;

/* A content model counter: the occurrences of the particle it counts. */
typedef struct content_model_counter {
    uint64_t minimum_occurrences;
    uint64_t maximum_occurrences;
    /* Where adding one stops: maxOccurs, or minOccurs when unbounded
       (larger values pass the same tests). */
    uint64_t saturation_value;
    /* The particle counted, for expanding it instead. */
    uint32_t particle;
} content_model_counter;

/* A counter test: lowest_value <= value <= highest_value. */
typedef struct counter_test {
    uint32_t counter;
    uint64_t lowest_value;
    uint64_t highest_value;
} counter_test;

typedef enum counter_operation_kind {
    SXML_COUNTER_OPERATION_SET_TO_ONE,
    SXML_COUNTER_OPERATION_INCREMENT,
} counter_operation_kind;

typedef struct counter_operation {
    uint32_t counter;
    counter_operation_kind operation_kind;
} counter_operation;

/* A content model transition. */
typedef struct content_model_transition {
    /* ELEMENT_DECLARATION or WILDCARD. */
    sxml_schema_term_kind term_kind;
    uint32_t term;
    /* The particle whose occurrence it moves to (for attribution). */
    uint32_t particle;
    uint32_t target_state;
    index_run tests;
    index_run operations;
    /* ELEMENT_DECLARATION: the declarations it matches, the term first,
       then the members substitutable for it (3.3.6.3). */
    index_run members;
} content_model_transition;

typedef struct content_model_state {
    index_run transitions;
    bool is_final;
    /* The tests a final state needs to end there; none: it may end. */
    index_run final_tests;
} content_model_state;

struct sxml_schema_content_model {
    sxml_schema_type_definition_index complex_type;
    bool is_all_group;
    /* States 1 to state_count; state 1 is the start. */
    uint32_t state_count;
    uint32_t counter_count;
    sxml_schema_open_content_mode open_content_mode;
    sxml_schema_wildcard_index open_content_wildcard;
    /* The suffix state of suffix open content, else 0. */
    uint32_t suffix_state;
    const content_model_state *states;
    const content_model_transition *transitions;
    const counter_test *tests;
    const counter_operation *operations;
    const uint32_t *members;
    const content_model_counter *counters;
    /* The names of the element particles, sorted (##definedSibling). */
    const uint32_t *sibling_names;
    uint32_t sibling_name_count;
};

/* ------------------------------------------------------------- Sorting */

static bool key_is_before(const void *state, sxml_sort_pair pair) {
    (void)state;
    return *(const uint64_t *)pair.first < *(const uint64_t *)pair.second;
}

/* Sorts keys ascending (src/sort_internal.h: no recursion, no memory). */
static void sort_keys(uint64_t *keys, size_t key_count) {
    sxml_sort_in_place(&(sxml_sort_request){.elements = keys,
                                            .element_count = key_count,
                                            .element_size = sizeof(uint64_t),
                                            .is_before = key_is_before,
                                            .state = NULL});
}

/* FNV-1a over 32-bit values. */
static uint64_t hash_values(const uint32_t *values, size_t value_count) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t value_index = 0; value_index < value_count; value_index += 1) {
        hash ^= values[value_index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

/* ------------------------------------------------------ Schema wildcards */

/* Whether a namespace constraint allows a namespace (3.10.4.3). */
static bool namespace_constraint_allows(const sxml_schema_components *components,
                                        const sxml_schema_wildcard *wildcard,
                                        uint32_t namespace_index) {
    const sxml_schema_index_view namespaces =
        sxml_schema_get_list(components, wildcard->namespaces);
    bool is_listed = false;
    for (size_t entry = 0; entry < namespaces.index_count; entry += 1) {
        if (namespaces.indexes[entry] == namespace_index) {
            is_listed = true;
        }
    }
    switch (wildcard->namespace_constraint_variety) {
    case SXML_SCHEMA_NAMESPACE_CONSTRAINT_ANY:
        return true;
    case SXML_SCHEMA_NAMESPACE_CONSTRAINT_ENUMERATION:
        return is_listed;
    case SXML_SCHEMA_NAMESPACE_CONSTRAINT_NOT:
        return !is_listed;
    }
    return false;
}

/* A name to test, with its namespace already looked up. */
typedef struct wildcard_name {
    sxml_schema_wildcard_index wildcard;
    sxml_name_identifier name;
    sxml_namespace_uri_identifier namespace_uri;
    sxml_schema_symbol_space symbol_space;
} wildcard_name;

/* Wildcard allows Expanded Name (3.10.4.2) without the keyword sibling. */
static bool wildcard_allows(const sxml_schema_components *components, const wildcard_name *test) {
    const sxml_schema_wildcard *wildcard = sxml_schema_get_wildcard(components, test->wildcard);
    if (!namespace_constraint_allows(components, wildcard, test->namespace_uri.index)) {
        return false;
    }
    const sxml_schema_index_view disallowed =
        sxml_schema_get_list(components, wildcard->disallowed_names);
    for (size_t entry = 0; entry < disallowed.index_count; entry += 1) {
        if (disallowed.indexes[entry] == test->name.index) {
            return false;
        }
    }
    uint32_t global_component = 0;
    if (wildcard->disallows_defined && test->symbol_space != SXML_SCHEMA_SYMBOL_SPACE_NONE &&
        sxml_schema_components_find_name(
            components,
            (sxml_schema_name_key){.symbol_space = test->symbol_space, .name = test->name},
            &global_component)) {
        return false;
    }
    return true;
}

bool sxml_schema_wildcard_allows_name(const sxml_schema_components *components,
                                      const sxml_schema_wildcard_test *test) {
    sxml_name_parts parts = {.namespace_uri = {.index = 0}};
    if (sxml_name_pool_get_name_parts(components->processor->name_pool, test->name, &parts) !=
        SXML_STATUS_OK) {
        return false;
    }
    const wildcard_name request = {.wildcard = test->wildcard,
                                   .name = test->name,
                                   .namespace_uri = parts.namespace_uri,
                                   .symbol_space = test->symbol_space};
    return wildcard_allows(components, &request);
}

/* Two wildcards to compare. */
typedef struct wildcard_pair {
    sxml_schema_wildcard_index first_wildcard;
    sxml_schema_wildcard_index second_wildcard;
} wildcard_pair;

/* Whether some namespace is allowed by both namespace constraints. */
static bool wildcards_overlap(const sxml_schema_components *components, wildcard_pair pair_index) {
    const sxml_schema_wildcard *first =
        sxml_schema_get_wildcard(components, pair_index.first_wildcard);
    const sxml_schema_wildcard *second =
        sxml_schema_get_wildcard(components, pair_index.second_wildcard);
    if (second->namespace_constraint_variety == SXML_SCHEMA_NAMESPACE_CONSTRAINT_ENUMERATION) {
        const sxml_schema_wildcard *swapped = first;
        first = second;
        second = swapped;
    }
    if (first->namespace_constraint_variety != SXML_SCHEMA_NAMESPACE_CONSTRAINT_ENUMERATION) {
        /* any and not(...) both allow infinitely many namespaces. */
        return true;
    }
    const sxml_schema_index_view namespaces = sxml_schema_get_list(components, first->namespaces);
    for (size_t entry = 0; entry < namespaces.index_count; entry += 1) {
        if (namespace_constraint_allows(components, second, namespaces.indexes[entry])) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------ Compiler */

/* Particles expanded when maxOccurs (or, unbounded, minOccurs) is at most
   this; larger ones are counted. A compilation that passes the state
   limit is tried again with every repetition above one counted. */
#define FIRST_EXPANSION_BOUND 16U

/* Most nodes, list entries, edges and transitions, as multiples of the
   state limit (the bound on work and memory, schema.h). */
#define NODES_PER_STATE 4U
#define LIST_ENTRIES_PER_STATE 64U
#define EDGES_PER_STATE 32U

/* sxml_schema_configuration.maximum_content_model_state_count's 0. */
#define DEFAULT_MAXIMUM_STATE_COUNT 65536U

typedef enum content_node_kind {
    SXML_CONTENT_NODE_LEAF,
    SXML_CONTENT_NODE_SEQUENCE,
    SXML_CONTENT_NODE_CHOICE,
    /* ? (0..1), * (0..unbounded) and + (1..unbounded). */
    SXML_CONTENT_NODE_REPEAT,
    SXML_CONTENT_NODE_COUNTED,
} content_node_kind;

/* A node of the expanded particle tree. Node 0 is unused, so 0 is "no
   node"; children have larger indexes than their parents. */
typedef struct content_node {
    content_node_kind node_kind;
    uint32_t parent;
    uint32_t first_child;
    uint32_t last_child;
    uint32_t next_sibling;
    uint64_t minimum_occurrences;
    uint64_t maximum_occurrences;
    /* LEAF: its position; COUNTED: its counter. */
    uint32_t position;
    uint32_t counter;
    bool is_nullable;
    index_run first_positions;
    index_run last_positions;
} content_node;

/* A content model position; position 0 is the start. */
typedef struct content_position {
    uint32_t leaf_node;
    uint32_t local_particle;
    bool is_final;
    index_run final_tests;
    /* Its edges, in the edge order. */
    index_run edges;
} content_position;

/* A particle that has positions in this model. */
typedef struct local_particle {
    uint32_t particle;
    sxml_schema_term_kind term_kind;
    uint32_t term;
    index_run members;
    /* While a state is made: its first transition group, plus one. */
    uint32_t group_head;
} local_particle;

typedef struct content_edge {
    uint32_t source_position;
    uint32_t target_position;
    index_run tests;
    index_run operations;
} content_edge;

typedef enum build_task_kind {
    SXML_BUILD_TASK_PARTICLE,
    SXML_BUILD_TASK_TERM,
    /* remaining_count copies of the term, one after the other. */
    SXML_BUILD_TASK_COPIES,
    /* The term under a + repetition. */
    SXML_BUILD_TASK_PLUS,
    /* remaining_count optional copies, nested: (t (t (t)?)?)?. */
    SXML_BUILD_TASK_OPTIONAL_CHAIN,
} build_task_kind;

typedef struct build_task {
    build_task_kind task_kind;
    uint32_t particle;
    uint32_t parent_node;
    uint64_t remaining_count;
} build_task;

/* The edges from a state into one particle with one set of tests and
   operations. */
typedef struct transition_group {
    uint32_t local_particle;
    index_run tests;
    index_run operations;
    uint32_t next_group;
    uint32_t smallest_target;
    uint32_t first_pair;
    uint32_t pair_count;
} transition_group;

/* A set of positions, a state of the subset construction. */
typedef struct position_set {
    index_run positions;
} position_set;

/* An open-addressing table of entry indexes plus one (0 is empty). */
typedef struct slot_table {
    uint32_t *slots;
    size_t slot_count;
    size_t used_count;
} slot_table;

typedef struct content_compiler {
    sxml_schema_compilation *compilation;
    sxml_schema_components *components;
    sxml_allocator allocator;
    sxml_schema_type_definition_index complex_type;
    uint32_t state_limit;
    uint64_t expansion_bound;
    bool is_all_group;
    /* Particles to expand rather than count (uint32_t); kept across
       attempts. */
    sxml_schema_scratch_array expanded_particles;
    sxml_schema_scratch_array tasks;
    sxml_schema_scratch_array nodes;
    sxml_schema_scratch_array positions;
    sxml_schema_scratch_array local_particles;
    sxml_schema_scratch_array position_lists;
    sxml_schema_scratch_array edges;
    sxml_schema_scratch_array edge_order;
    sxml_schema_scratch_array set_positions;
    sxml_schema_scratch_array sets;
    sxml_schema_scratch_array groups;
    sxml_schema_scratch_array keys;
    sxml_schema_scratch_array touched;
    sxml_schema_scratch_array set_buffer;
    /* The model being made. */
    sxml_schema_scratch_array states;
    sxml_schema_scratch_array transitions;
    sxml_schema_scratch_array tests;
    sxml_schema_scratch_array operations;
    sxml_schema_scratch_array members;
    sxml_schema_scratch_array counters;
    sxml_schema_scratch_array sibling_names;
    slot_table particle_slots;
    slot_table set_slots;
    uint32_t root_node;
    /* The values of one configuration of the restriction walk. */
    uint32_t configuration_width;
    /* Expanding what the counters cannot decide passed the limit: keep
       the transitions in priority order instead (see mark_counters). */
    bool allows_counter_ambiguity;
    /* Two transitions of one particle overlap: expand and try again. */
    bool has_conflict;
    bool has_new_expansion;
    /* An error was reported for this type (one is enough). */
    bool has_reported;
} content_compiler;

/* The scratch arrays of a compiler, for initializing and releasing. */
static sxml_schema_scratch_array *compiler_array(content_compiler *compiler, size_t array_index) {
    sxml_schema_scratch_array *const arrays[] = {
        &compiler->expanded_particles,
        &compiler->tasks,
        &compiler->nodes,
        &compiler->positions,
        &compiler->local_particles,
        &compiler->position_lists,
        &compiler->edges,
        &compiler->edge_order,
        &compiler->set_positions,
        &compiler->sets,
        &compiler->groups,
        &compiler->keys,
        &compiler->touched,
        &compiler->set_buffer,
        &compiler->states,
        &compiler->transitions,
        &compiler->tests,
        &compiler->operations,
        &compiler->members,
        &compiler->counters,
        &compiler->sibling_names,
    };
    return array_index < sizeof(arrays) / sizeof(arrays[0]) ? arrays[array_index] : NULL;
}

static void compiler_initialize_arrays(content_compiler *compiler) {
    const size_t entry_byte_counts[] = {
        sizeof(uint32_t),
        sizeof(build_task),
        sizeof(content_node),
        sizeof(content_position),
        sizeof(local_particle),
        sizeof(uint32_t),
        sizeof(content_edge),
        sizeof(uint32_t),
        sizeof(uint32_t),
        sizeof(position_set),
        sizeof(transition_group),
        sizeof(uint64_t),
        sizeof(uint32_t),
        sizeof(uint32_t),
        sizeof(content_model_state),
        sizeof(content_model_transition),
        sizeof(counter_test),
        sizeof(counter_operation),
        sizeof(uint32_t),
        sizeof(content_model_counter),
        sizeof(uint32_t),
    };
    for (size_t array_index = 0;
         array_index < sizeof(entry_byte_counts) / sizeof(entry_byte_counts[0]); array_index += 1) {
        *compiler_array(compiler, array_index) =
            (sxml_schema_scratch_array){.entry_byte_count = entry_byte_counts[array_index]};
    }
}

static void compiler_release(content_compiler *compiler) {
    for (size_t array_index = 0;; array_index += 1) {
        sxml_schema_scratch_array *array = compiler_array(compiler, array_index);
        if (array == NULL) {
            break;
        }
        sxml_schema_scratch_array_release(compiler->allocator, array);
    }
    sxml_allocator_release(compiler->allocator, compiler->particle_slots.slots);
    sxml_allocator_release(compiler->allocator, compiler->set_slots.slots);
    compiler->particle_slots = (slot_table){.slots = NULL};
    compiler->set_slots = (slot_table){.slots = NULL};
}

/* Empties everything of one attempt (not the expanded particles). */
static void compiler_reset(content_compiler *compiler) {
    for (size_t array_index = 1;; array_index += 1) {
        sxml_schema_scratch_array *array = compiler_array(compiler, array_index);
        if (array == NULL) {
            break;
        }
        array->entry_count = 0;
    }
    if (compiler->particle_slots.slots != NULL) {
        memset(compiler->particle_slots.slots, 0,
               compiler->particle_slots.slot_count * sizeof(uint32_t));
    }
    if (compiler->set_slots.slots != NULL) {
        memset(compiler->set_slots.slots, 0, compiler->set_slots.slot_count * sizeof(uint32_t));
    }
    compiler->particle_slots.used_count = 0;
    compiler->set_slots.used_count = 0;
    compiler->root_node = 0;
    compiler->has_conflict = false;
    compiler->has_new_expansion = false;
}

/* Typed entry access. */
static content_node *node_at(const content_compiler *compiler, uint32_t node_index) {
    return sxml_schema_scratch_array_at(&compiler->nodes, node_index);
}

static content_position *position_at(const content_compiler *compiler, uint32_t position_index) {
    return sxml_schema_scratch_array_at(&compiler->positions, position_index);
}

static local_particle *local_particle_at(const content_compiler *compiler, uint32_t local_index) {
    return sxml_schema_scratch_array_at(&compiler->local_particles, local_index);
}

static uint32_t *uint32_at(const sxml_schema_scratch_array *array, size_t entry_index) {
    return sxml_schema_scratch_array_at(array, entry_index);
}

static uint32_t entry_count_of(const sxml_schema_scratch_array *array) {
    return (uint32_t)array->entry_count;
}

static sxml_status append_uint32(content_compiler *compiler, sxml_schema_scratch_array *array,
                                 uint32_t value) {
    if (array->entry_count >= UINT32_MAX - 1U) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    return sxml_schema_scratch_array_append(compiler->allocator, array, &value);
}

/* ----------------------------------------------------------- Reporting */

/* An error of the complex type being compiled. */
typedef struct content_report {
    const char *constraint_name;
    const char *description;
    /* Optional: a name the detailed description ends with. */
    sxml_name_identifier name;
} content_report;

static sxml_status report_error(content_compiler *compiler, content_report report) {
    if (compiler->has_reported) {
        return SXML_STATUS_OK;
    }
    compiler->has_reported = true;
    const sxml_schema_type_definition *type_definition =
        sxml_schema_get_type_definition(compiler->components, compiler->complex_type);
    return sxml_schema_compilation_report(
        compiler->compilation,
        &(sxml_schema_report_request){
            .constraint_name = sxml_byte_view_from_c_string(report.constraint_name),
            .description = sxml_byte_view_from_c_string(report.description),
            .subject_name = report.name,
            .position = type_definition->position});
}

/* ---------------------------------------------------------- Slot tables */

typedef enum slot_table_kind {
    SXML_SLOT_TABLE_PARTICLES,
    SXML_SLOT_TABLE_SETS,
    /* Configurations of the restriction walk, rows of keys. */
    SXML_SLOT_TABLE_CONFIGURATIONS,
} slot_table_kind;

static slot_table *slot_table_of(content_compiler *compiler, slot_table_kind table_kind) {
    /* Configurations reuse the set slots once the states are made. */
    return table_kind == SXML_SLOT_TABLE_PARTICLES ? &compiler->particle_slots
                                                   : &compiler->set_slots;
}

static uint64_t set_hash(const content_compiler *compiler, index_run positions) {
    return hash_values(uint32_at(&compiler->set_positions, positions.first_index),
                       positions.index_count);
}

/* FNV-1a over 64-bit values. */
static uint64_t hash_row(const uint64_t *values, size_t value_count) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t value_index = 0; value_index < value_count; value_index += 1) {
        hash ^= values[value_index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t entry_hash(const content_compiler *compiler, slot_table_kind table_kind,
                           uint32_t entry_index) {
    if (table_kind == SXML_SLOT_TABLE_CONFIGURATIONS) {
        return hash_row(sxml_schema_scratch_array_at(
                            &compiler->keys, (size_t)entry_index * compiler->configuration_width),
                        compiler->configuration_width);
    }
    if (table_kind == SXML_SLOT_TABLE_PARTICLES) {
        const uint32_t particle = local_particle_at(compiler, entry_index)->particle;
        return hash_values(&particle, 1);
    }
    const position_set *set = sxml_schema_scratch_array_at(&compiler->sets, entry_index);
    return set_hash(compiler, set->positions);
}

/* Makes room for one more entry: at most half the slots in use. */
static sxml_status slot_table_reserve(content_compiler *compiler, slot_table_kind table_kind) {
    slot_table *table = slot_table_of(compiler, table_kind);
    if ((table->used_count + 1) * 2 <= table->slot_count) {
        return SXML_STATUS_OK;
    }
    const size_t slot_count = table->slot_count == 0 ? 64U : table->slot_count * 2U;
    if (slot_count > SIZE_MAX / sizeof(uint32_t)) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        compiler->allocator,
        (sxml_allocation_request){.byte_count = slot_count * sizeof(uint32_t),
                                  .alignment = alignof(uint32_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t *slots = memory;
    memset(slots, 0, slot_count * sizeof(uint32_t));
    for (size_t old_slot = 0; old_slot < table->slot_count; old_slot += 1) {
        const uint32_t stored = table->slots[old_slot];
        if (stored == 0) {
            continue;
        }
        size_t slot = (size_t)entry_hash(compiler, table_kind, stored - 1) & (slot_count - 1);
        while (slots[slot] != 0) {
            slot = (slot + 1) & (slot_count - 1);
        }
        slots[slot] = stored;
    }
    sxml_allocator_release(compiler->allocator, table->slots);
    table->slots = slots;
    table->slot_count = slot_count;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------- Substitution groups */

/* The derivation method of one step, for blocking: a list or union step
   counts as restriction (3.16.6.3). */
static sxml_schema_derivation_set step_method(const sxml_schema_type_definition *type_definition) {
    if (type_definition->variety == SXML_TYPE_VARIETY_COMPLEX) {
        return (sxml_schema_derivation_set)type_definition->derivation_method;
    }
    return SXML_SCHEMA_DERIVATION_RESTRICTION;
}

/* A member and head to check. */
typedef struct substitution_request {
    sxml_schema_element_declaration_index member;
    sxml_schema_element_declaration_index head;
} substitution_request;

/* Substitution Group OK (Transitive), 3.3.6.3, for a member of the head's
   substitution group (clause 2.2 holds: RESOLVE made the members). */
static bool is_substitutable(const sxml_schema_components *components,
                             substitution_request request) {
    const sxml_schema_element_declaration *head =
        sxml_schema_get_element_declaration(components, request.head);
    const sxml_schema_element_declaration *member =
        sxml_schema_get_element_declaration(components, request.member);
    if ((head->disallowed_substitutions & SXML_SCHEMA_DERIVATION_SUBSTITUTION) != 0) {
        return false;
    }
    const sxml_schema_type_definition *head_type =
        sxml_schema_get_type_definition(components, head->type_definition);
    sxml_schema_derivation_set blocked =
        head->disallowed_substitutions &
        (sxml_schema_derivation_set)(SXML_SCHEMA_DERIVATION_EXTENSION |
                                     SXML_SCHEMA_DERIVATION_RESTRICTION);
    if (head_type->variety == SXML_TYPE_VARIETY_COMPLEX) {
        blocked |= head_type->prohibited_substitutions;
    }
    sxml_schema_derivation_outcome outcome = {.is_derived = false};
    if (sxml_schema_check_derivation(
            components,
            &(sxml_schema_derivation_request){.derived_type = member->type_definition,
                                              .base_type = head->type_definition,
                                              .blocked_derivations = blocked},
            &outcome) != SXML_STATUS_OK ||
        !outcome.is_derived) {
        return false;
    }
    /* Clause 2.3 (3): the prohibited substitutions of the types between. */
    sxml_schema_derivation_set methods = 0;
    sxml_schema_derivation_set prohibited = 0;
    sxml_schema_type_definition_index current = member->type_definition;
    const uint32_t type_count = components->tables[SXML_SCHEMA_TABLE_TYPE_DEFINITION].entry_count;
    for (uint32_t step_count = 0; step_count < type_count && current.value != 0 &&
                                  current.value != head->type_definition.value &&
                                  current.value != (uint32_t)SXML_TYPE_XS_ANY_TYPE;
         step_count += 1) {
        const sxml_schema_type_definition *current_type =
            sxml_schema_get_type_definition(components, current);
        methods |= step_method(current_type);
        if (current.value != member->type_definition.value &&
            current_type->variety == SXML_TYPE_VARIETY_COMPLEX) {
            prohibited |= current_type->prohibited_substitutions;
        }
        current = current_type->base_type;
    }
    return (methods & prohibited) == 0;
}

/* ------------------------------------------------------ Building nodes */

/* Finds or adds the local particle of a particle (and, for an element
   particle, its substitutable members). */
static sxml_status find_local_particle(content_compiler *compiler, uint32_t particle_index,
                                       uint32_t *local_index) {
    sxml_status status = slot_table_reserve(compiler, SXML_SLOT_TABLE_PARTICLES);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    slot_table *table = &compiler->particle_slots;
    size_t slot = (size_t)hash_values(&particle_index, 1) & (table->slot_count - 1);
    while (table->slots[slot] != 0) {
        if (local_particle_at(compiler, table->slots[slot] - 1)->particle == particle_index) {
            *local_index = table->slots[slot] - 1;
            return SXML_STATUS_OK;
        }
        slot = (slot + 1) & (table->slot_count - 1);
    }
    const sxml_schema_components *components = compiler->components;
    const sxml_schema_particle *particle =
        sxml_schema_get_particle(components, (sxml_schema_particle_index){.value = particle_index});
    local_particle entry = {.particle = particle_index,
                            .term_kind = particle->term_kind,
                            .term = particle->term,
                            .members = {.first_index = entry_count_of(&compiler->members)}};
    if (particle->term_kind == SXML_SCHEMA_TERM_ELEMENT_DECLARATION) {
        status = append_uint32(compiler, &compiler->members, particle->term);
        const sxml_schema_element_declaration *head = sxml_schema_get_element_declaration(
            components, (sxml_schema_element_declaration_index){.value = particle->term});
        const sxml_schema_index_view group_members =
            sxml_schema_get_list(components, head->substitution_group_members);
        for (size_t member_index = 0;
             status == SXML_STATUS_OK && member_index < group_members.index_count;
             member_index += 1) {
            const substitution_request request = {
                .member = {.value = group_members.indexes[member_index]},
                .head = {.value = particle->term}};
            if (is_substitutable(components, request)) {
                status = append_uint32(compiler, &compiler->members, request.member.value);
            }
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    entry.members.index_count = entry_count_of(&compiler->members) - entry.members.first_index;
    const uint32_t new_index = entry_count_of(&compiler->local_particles);
    status =
        sxml_schema_scratch_array_append(compiler->allocator, &compiler->local_particles, &entry);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    table->slots[slot] = new_index + 1;
    table->used_count += 1;
    *local_index = new_index;
    return SXML_STATUS_OK;
}

static sxml_status add_node(content_compiler *compiler, const content_node *request,
                            uint32_t *node_index) {
    if (compiler->nodes.entry_count == 0) {
        const content_node unused = {.node_kind = SXML_CONTENT_NODE_SEQUENCE};
        const sxml_status status =
            sxml_schema_scratch_array_append(compiler->allocator, &compiler->nodes, &unused);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (compiler->nodes.entry_count > (size_t)compiler->state_limit * NODES_PER_STATE) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    const uint32_t new_index = entry_count_of(&compiler->nodes);
    content_node node = *request;
    node.first_child = 0;
    node.last_child = 0;
    node.next_sibling = 0;
    const sxml_status status =
        sxml_schema_scratch_array_append(compiler->allocator, &compiler->nodes, &node);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (node.parent != 0) {
        content_node *parent = node_at(compiler, node.parent);
        if (parent->last_child != 0) {
            node_at(compiler, parent->last_child)->next_sibling = new_index;
        } else {
            parent->first_child = new_index;
        }
        parent->last_child = new_index;
    } else if (compiler->root_node == 0) {
        compiler->root_node = new_index;
    }
    *node_index = new_index;
    return SXML_STATUS_OK;
}

/* A node of one kind under a parent. */
typedef struct node_request {
    content_node_kind node_kind;
    uint32_t parent_node;
    uint64_t minimum_occurrences;
    uint64_t maximum_occurrences;
} node_request;

static sxml_status add_inner_node(content_compiler *compiler, node_request request,
                                  uint32_t *node_index) {
    const content_node node = {.node_kind = request.node_kind,
                               .parent = request.parent_node,
                               .minimum_occurrences = request.minimum_occurrences,
                               .maximum_occurrences = request.maximum_occurrences};
    return add_node(compiler, &node, node_index);
}

/* A position for an element or wildcard particle under parent. */
static sxml_status add_leaf(content_compiler *compiler, uint32_t particle_index,
                            uint32_t parent_node) {
    if (compiler->positions.entry_count > compiler->state_limit) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    uint32_t local_index = 0;
    sxml_status status = find_local_particle(compiler, particle_index, &local_index);
    const uint32_t position_index = entry_count_of(&compiler->positions);
    const content_node node = {
        .node_kind = SXML_CONTENT_NODE_LEAF, .parent = parent_node, .position = position_index};
    uint32_t node_index = 0;
    if (status == SXML_STATUS_OK) {
        status = add_node(compiler, &node, &node_index);
    }
    const content_position position = {.leaf_node = node_index, .local_particle = local_index};
    if (status == SXML_STATUS_OK) {
        status =
            sxml_schema_scratch_array_append(compiler->allocator, &compiler->positions, &position);
    }
    return status;
}

static sxml_status push_task(content_compiler *compiler, build_task task) {
    return sxml_schema_scratch_array_append(compiler->allocator, &compiler->tasks, &task);
}

static bool is_expanded_particle(const content_compiler *compiler, uint32_t particle_index) {
    for (size_t entry = 0; entry < compiler->expanded_particles.entry_count; entry += 1) {
        if (*uint32_at(&compiler->expanded_particles, entry) == particle_index) {
            return true;
        }
    }
    return false;
}

static bool is_counted(const content_compiler *compiler, uint32_t particle_index) {
    const sxml_schema_particle *particle = sxml_schema_get_particle(
        compiler->components, (sxml_schema_particle_index){.value = particle_index});
    if (is_expanded_particle(compiler, particle_index)) {
        return false;
    }
    if (particle->maximum_occurrences == SXML_SCHEMA_UNBOUNDED) {
        return particle->minimum_occurrences > 1 &&
               particle->minimum_occurrences > compiler->expansion_bound;
    }
    return particle->maximum_occurrences > 1 &&
           particle->maximum_occurrences > compiler->expansion_bound;
}

/* A counted node and its counter. */
static sxml_status add_counted_node(content_compiler *compiler, build_task task,
                                    uint32_t *node_index) {
    const sxml_schema_particle *particle = sxml_schema_get_particle(
        compiler->components, (sxml_schema_particle_index){.value = task.particle});
    const content_model_counter counter = {.minimum_occurrences = particle->minimum_occurrences,
                                           .maximum_occurrences = particle->maximum_occurrences,
                                           .saturation_value = particle->maximum_occurrences ==
                                                                       SXML_SCHEMA_UNBOUNDED
                                                                   ? particle->minimum_occurrences
                                                                   : particle->maximum_occurrences,
                                           .particle = task.particle};
    content_node node = {.node_kind = SXML_CONTENT_NODE_COUNTED,
                         .parent = task.parent_node,
                         .minimum_occurrences = particle->minimum_occurrences,
                         .maximum_occurrences = particle->maximum_occurrences,
                         .counter = entry_count_of(&compiler->counters)};
    sxml_status status =
        sxml_schema_scratch_array_append(compiler->allocator, &compiler->counters, &counter);
    if (status == SXML_STATUS_OK) {
        status = add_node(compiler, &node, node_index);
    }
    return status;
}

/* A particle (3.9): its term once, counted, or expanded into copies. */
static sxml_status build_particle(content_compiler *compiler, build_task task) {
    const sxml_schema_particle *particle = sxml_schema_get_particle(
        compiler->components, (sxml_schema_particle_index){.value = task.particle});
    const uint64_t minimum = particle->minimum_occurrences;
    const uint64_t maximum = particle->maximum_occurrences;
    build_task term_task = {.task_kind = SXML_BUILD_TASK_TERM,
                            .particle = task.particle,
                            .parent_node = task.parent_node};
    uint32_t node_index = 0;
    sxml_status status = SXML_STATUS_OK;
    if (maximum == 0) {
        /* An absent particle (3.9.2): it adds nothing. */
        return SXML_STATUS_OK;
    }
    if (maximum != SXML_SCHEMA_UNBOUNDED && minimum > maximum) {
        /* p-props-correct.2.1, reported by COMPLEX_TYPES: nothing matches. */
        return add_inner_node(
            compiler,
            (node_request){.node_kind = SXML_CONTENT_NODE_CHOICE, .parent_node = task.parent_node},
            &node_index);
    }
    if (minimum == 1 && maximum == 1) {
        return push_task(compiler, term_task);
    }
    if (is_counted(compiler, task.particle)) {
        status = add_counted_node(compiler, task, &node_index);
        term_task.parent_node = node_index;
        return status == SXML_STATUS_OK ? push_task(compiler, term_task) : status;
    }
    if (maximum == SXML_SCHEMA_UNBOUNDED || (minimum == 0 && maximum == 1)) {
        if (minimum <= 1) {
            status = add_inner_node(compiler,
                                    (node_request){.node_kind = SXML_CONTENT_NODE_REPEAT,
                                                   .parent_node = task.parent_node,
                                                   .minimum_occurrences = minimum,
                                                   .maximum_occurrences = maximum},
                                    &node_index);
            term_task.parent_node = node_index;
            return status == SXML_STATUS_OK ? push_task(compiler, term_task) : status;
        }
    }
    /* t{m,n} is m copies, then n - m optional ones nested; t{m,} is m - 1
       copies and t+. Tasks run last pushed first. */
    status = add_inner_node(
        compiler,
        (node_request){.node_kind = SXML_CONTENT_NODE_SEQUENCE, .parent_node = task.parent_node},
        &node_index);
    if (status == SXML_STATUS_OK && maximum == SXML_SCHEMA_UNBOUNDED) {
        status = push_task(compiler, (build_task){.task_kind = SXML_BUILD_TASK_PLUS,
                                                  .particle = task.particle,
                                                  .parent_node = node_index});
    } else if (status == SXML_STATUS_OK && maximum > minimum) {
        status = push_task(compiler, (build_task){.task_kind = SXML_BUILD_TASK_OPTIONAL_CHAIN,
                                                  .particle = task.particle,
                                                  .parent_node = node_index,
                                                  .remaining_count = maximum - minimum});
    }
    const uint64_t copy_count = maximum == SXML_SCHEMA_UNBOUNDED ? minimum - 1 : minimum;
    if (status == SXML_STATUS_OK && copy_count > 0) {
        status = push_task(compiler, (build_task){.task_kind = SXML_BUILD_TASK_COPIES,
                                                  .particle = task.particle,
                                                  .parent_node = node_index,
                                                  .remaining_count = copy_count});
    }
    return status;
}

/* A term: a position, or a model group's node and its particles. */
static sxml_status build_term(content_compiler *compiler, build_task task) {
    const sxml_schema_components *components = compiler->components;
    const sxml_schema_particle *particle =
        sxml_schema_get_particle(components, (sxml_schema_particle_index){.value = task.particle});
    switch (particle->term_kind) {
    case SXML_SCHEMA_TERM_NONE:
        /* An unresolved reference, reported by RESOLVE. */
        return SXML_STATUS_OK;
    case SXML_SCHEMA_TERM_ELEMENT_DECLARATION:
    case SXML_SCHEMA_TERM_WILDCARD:
        return add_leaf(compiler, task.particle, task.parent_node);
    case SXML_SCHEMA_TERM_MODEL_GROUP:
        break;
    }
    const sxml_schema_model_group *model_group = sxml_schema_get_model_group(
        components, (sxml_schema_model_group_index){.value = particle->term});
    sxml_status status = SXML_STATUS_OK;
    if (model_group->compositor == SXML_SCHEMA_COMPOSITOR_ALL) {
        /* All Group Limited (3.8.6.2), clause 1: an all group only as a
           content type's particle or in an all group. */
        status =
            report_error(compiler, (content_report){.constraint_name = "cos-all-limited.1.2",
                                                    .description = "An all group appears inside a "
                                                                   "sequence or choice"});
    }
    uint32_t node_index = 0;
    if (status == SXML_STATUS_OK) {
        status = add_inner_node(
            compiler,
            (node_request){.node_kind = model_group->compositor == SXML_SCHEMA_COMPOSITOR_CHOICE
                                            ? SXML_CONTENT_NODE_CHOICE
                                            : SXML_CONTENT_NODE_SEQUENCE,
                           .parent_node = task.parent_node},
            &node_index);
    }
    const sxml_schema_index_view particles =
        sxml_schema_get_list(components, model_group->particles);
    for (size_t entry = particles.index_count; status == SXML_STATUS_OK && entry > 0; entry -= 1) {
        status = push_task(compiler, (build_task){.task_kind = SXML_BUILD_TASK_PARTICLE,
                                                  .particle = particles.indexes[entry - 1],
                                                  .parent_node = node_index});
    }
    return status;
}

/* One build task. */
static sxml_status run_build_task(content_compiler *compiler, build_task task) {
    build_task next_task = task;
    uint32_t node_index = 0;
    sxml_status status = SXML_STATUS_OK;
    switch (task.task_kind) {
    case SXML_BUILD_TASK_PARTICLE:
        return build_particle(compiler, task);
    case SXML_BUILD_TASK_TERM:
        return build_term(compiler, task);
    case SXML_BUILD_TASK_COPIES:
        if (task.remaining_count > 1) {
            next_task.remaining_count -= 1;
            status = push_task(compiler, next_task);
        }
        next_task.task_kind = SXML_BUILD_TASK_TERM;
        return status == SXML_STATUS_OK ? push_task(compiler, next_task) : status;
    case SXML_BUILD_TASK_PLUS:
        status = add_inner_node(compiler,
                                (node_request){.node_kind = SXML_CONTENT_NODE_REPEAT,
                                               .parent_node = task.parent_node,
                                               .minimum_occurrences = 1,
                                               .maximum_occurrences = SXML_SCHEMA_UNBOUNDED},
                                &node_index);
        next_task.task_kind = SXML_BUILD_TASK_TERM;
        next_task.parent_node = node_index;
        return status == SXML_STATUS_OK ? push_task(compiler, next_task) : status;
    case SXML_BUILD_TASK_OPTIONAL_CHAIN:
        status = add_inner_node(compiler,
                                (node_request){.node_kind = SXML_CONTENT_NODE_REPEAT,
                                               .parent_node = task.parent_node,
                                               .minimum_occurrences = 0,
                                               .maximum_occurrences = 1},
                                &node_index);
        if (status == SXML_STATUS_OK && task.remaining_count > 1) {
            status = add_inner_node(
                compiler,
                (node_request){.node_kind = SXML_CONTENT_NODE_SEQUENCE, .parent_node = node_index},
                &node_index);
            next_task.parent_node = node_index;
            next_task.remaining_count -= 1;
            if (status == SXML_STATUS_OK) {
                status = push_task(compiler, next_task);
            }
        }
        next_task.task_kind = SXML_BUILD_TASK_TERM;
        next_task.parent_node = node_index;
        next_task.remaining_count = 0;
        return status == SXML_STATUS_OK ? push_task(compiler, next_task) : status;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* Builds the node tree of the content type's particle. */
static sxml_status build_tree(content_compiler *compiler, uint32_t root_particle) {
    const content_position start = {.leaf_node = 0};
    sxml_status status =
        sxml_schema_scratch_array_append(compiler->allocator, &compiler->positions, &start);
    if (status == SXML_STATUS_OK) {
        status = push_task(compiler, (build_task){.task_kind = SXML_BUILD_TASK_PARTICLE,
                                                  .particle = root_particle,
                                                  .parent_node = 0});
    }
    while (status == SXML_STATUS_OK && compiler->tasks.entry_count > 0) {
        compiler->tasks.entry_count -= 1;
        const build_task task = *(const build_task *)sxml_schema_scratch_array_at(
            &compiler->tasks, compiler->tasks.entry_count);
        status = run_build_task(compiler, task);
    }
    return status;
}

/* -------------------------------------------- Nullable, first and last */

/* Appends a run of position lists to the end of the lists (a copy). */
static sxml_status append_positions(content_compiler *compiler, index_run run) {
    if (compiler->position_lists.entry_count + run.index_count >
        (size_t)compiler->state_limit * LIST_ENTRIES_PER_STATE) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t entry = 0; status == SXML_STATUS_OK && entry < run.index_count; entry += 1) {
        const uint32_t position = *uint32_at(&compiler->position_lists, run.first_index + entry);
        status = append_uint32(compiler, &compiler->position_lists, position);
    }
    return status;
}

/* Which list of a node. */
typedef enum list_choice {
    SXML_LIST_CHOICE_FIRST,
    SXML_LIST_CHOICE_LAST,
} list_choice;

static index_run node_list(const content_node *node, list_choice choice) {
    return choice == SXML_LIST_CHOICE_FIRST ? node->first_positions : node->last_positions;
}

/* The first or last positions of a sequence or choice from its children:
   a sequence's first positions are its children's up to the first one
   that is not nullable, its last ones the same from the end. */
static sxml_status compute_group_list(content_compiler *compiler, uint32_t node_index,
                                      list_choice choice) {
    const content_node *node = node_at(compiler, node_index);
    const bool is_sequence = node->node_kind == SXML_CONTENT_NODE_SEQUENCE;
    /* The children in order, from the key buffer (reused here). */
    compiler->touched.entry_count = 0;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t child = node->first_child; status == SXML_STATUS_OK && child != 0;
         child = node_at(compiler, child)->next_sibling) {
        status = append_uint32(compiler, &compiler->touched, child);
    }
    const uint32_t child_count = entry_count_of(&compiler->touched);
    /* One contributing child: share its run. */
    uint32_t contributor_count = 0;
    index_run shared = {.first_index = 0, .index_count = 0};
    for (uint32_t step_index = 0; status == SXML_STATUS_OK && step_index < child_count;
         step_index += 1) {
        const uint32_t child_position = choice == SXML_LIST_CHOICE_FIRST || !is_sequence
                                            ? step_index
                                            : child_count - 1 - step_index;
        const content_node *child =
            node_at(compiler, *uint32_at(&compiler->touched, child_position));
        if (node_list(child, choice).index_count > 0) {
            contributor_count += 1;
            shared = node_list(child, choice);
        }
        if (is_sequence && !child->is_nullable) {
            break;
        }
    }
    index_run result = shared;
    if (status == SXML_STATUS_OK && contributor_count > 1) {
        result.first_index = entry_count_of(&compiler->position_lists);
        for (uint32_t step_index = 0; status == SXML_STATUS_OK && step_index < child_count;
             step_index += 1) {
            const uint32_t child_position = choice == SXML_LIST_CHOICE_FIRST || !is_sequence
                                                ? step_index
                                                : child_count - 1 - step_index;
            const content_node *child =
                node_at(compiler, *uint32_at(&compiler->touched, child_position));
            const bool stops = is_sequence && !child->is_nullable;
            status = append_positions(compiler, node_list(child, choice));
            if (stops) {
                break;
            }
        }
        result.index_count = entry_count_of(&compiler->position_lists) - result.first_index;
    }
    if (status == SXML_STATUS_OK) {
        content_node *updated = node_at(compiler, node_index);
        if (choice == SXML_LIST_CHOICE_FIRST) {
            updated->first_positions = result;
        } else {
            updated->last_positions = result;
        }
    }
    return status;
}

/* Nullable, first and last of every node, children first. */
static sxml_status compute_node_lists(content_compiler *compiler) {
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t node_index = entry_count_of(&compiler->nodes);
         status == SXML_STATUS_OK && node_index > 1; node_index -= 1) {
        const uint32_t current = node_index - 1;
        content_node *node = node_at(compiler, current);
        switch (node->node_kind) {
        case SXML_CONTENT_NODE_LEAF: {
            const index_run run = {.first_index = entry_count_of(&compiler->position_lists),
                                   .index_count = 1};
            status = append_uint32(compiler, &compiler->position_lists, node->position);
            node = node_at(compiler, current);
            node->is_nullable = false;
            node->first_positions = run;
            node->last_positions = run;
            break;
        }
        case SXML_CONTENT_NODE_SEQUENCE:
        case SXML_CONTENT_NODE_CHOICE: {
            const bool is_sequence = node->node_kind == SXML_CONTENT_NODE_SEQUENCE;
            bool is_nullable = is_sequence;
            for (uint32_t child = node->first_child; child != 0;
                 child = node_at(compiler, child)->next_sibling) {
                const bool child_nullable = node_at(compiler, child)->is_nullable;
                is_nullable =
                    is_sequence ? is_nullable && child_nullable : is_nullable || child_nullable;
            }
            node->is_nullable = is_nullable;
            status = compute_group_list(compiler, current, SXML_LIST_CHOICE_FIRST);
            if (status == SXML_STATUS_OK) {
                status = compute_group_list(compiler, current, SXML_LIST_CHOICE_LAST);
            }
            break;
        }
        case SXML_CONTENT_NODE_REPEAT:
        case SXML_CONTENT_NODE_COUNTED: {
            const content_node *child = node_at(compiler, node->first_child);
            const bool has_child = node->first_child != 0;
            node->is_nullable = node->minimum_occurrences == 0 || !has_child || child->is_nullable;
            node->first_positions = has_child ? child->first_positions : (index_run){0, 0};
            node->last_positions = has_child ? child->last_positions : (index_run){0, 0};
            break;
        }
        }
    }
    return status;
}

/* --------------------------------------------------------------- Edges */

/* The effective minimum of a counted node: 0 when its term is nullable,
   since the remaining repetitions may then be empty. */
static uint64_t effective_minimum(const content_compiler *compiler, const content_node *node) {
    if (node->first_child == 0 || node_at(compiler, node->first_child)->is_nullable) {
        return 0;
    }
    return node->minimum_occurrences;
}

/* Adds the tests for leaving the counted nodes from position's leaf up to
   (not including) join_node. */
static sxml_status add_exit_tests(content_compiler *compiler, uint32_t position,
                                  uint32_t join_node) {
    sxml_status status = SXML_STATUS_OK;
    if (position == 0) {
        return status;
    }
    for (uint32_t node_index = position_at(compiler, position)->leaf_node;
         status == SXML_STATUS_OK && node_index != 0 && node_index != join_node;
         node_index = node_at(compiler, node_index)->parent) {
        const content_node *node = node_at(compiler, node_index);
        const uint64_t minimum = effective_minimum(compiler, node);
        if (node->node_kind == SXML_CONTENT_NODE_COUNTED && minimum > 0) {
            const counter_test test = {
                .counter = node->counter, .lowest_value = minimum, .highest_value = UINT64_MAX};
            status = sxml_schema_scratch_array_append(compiler->allocator, &compiler->tests, &test);
        }
    }
    return status;
}

/* Adds SET_TO_ONE for the counted nodes from position's leaf up to (not
   including) join_node: the edge enters them. */
static sxml_status add_entry_operations(content_compiler *compiler, uint32_t position,
                                        uint32_t join_node) {
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t node_index = position_at(compiler, position)->leaf_node;
         status == SXML_STATUS_OK && node_index != 0 && node_index != join_node;
         node_index = node_at(compiler, node_index)->parent) {
        const content_node *node = node_at(compiler, node_index);
        if (node->node_kind == SXML_CONTENT_NODE_COUNTED) {
            const counter_operation operation = {
                .counter = node->counter, .operation_kind = SXML_COUNTER_OPERATION_SET_TO_ONE};
            status = sxml_schema_scratch_array_append(compiler->allocator, &compiler->operations,
                                                      &operation);
        }
    }
    return status;
}

/* One follow pair, made at join_node (a sequence, a repetition that
   loops, or 0 for the start). */
typedef struct edge_request {
    uint32_t source_position;
    uint32_t target_position;
    uint32_t join_node;
    bool is_loop;
} edge_request;

static sxml_status add_edge(content_compiler *compiler, edge_request request) {
    if (compiler->edges.entry_count >= (size_t)compiler->state_limit * EDGES_PER_STATE) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    content_edge edge = {.source_position = request.source_position,
                         .target_position = request.target_position,
                         .tests = {.first_index = entry_count_of(&compiler->tests)}};
    sxml_status status = add_exit_tests(compiler, request.source_position, request.join_node);
    const content_node *join = request.join_node != 0 ? node_at(compiler, request.join_node) : NULL;
    const bool is_counted_loop =
        request.is_loop && join != NULL && join->node_kind == SXML_CONTENT_NODE_COUNTED;
    if (status == SXML_STATUS_OK && is_counted_loop &&
        join->maximum_occurrences != SXML_SCHEMA_UNBOUNDED) {
        const counter_test test = {.counter = join->counter,
                                   .lowest_value = 0,
                                   .highest_value = join->maximum_occurrences - 1};
        status = sxml_schema_scratch_array_append(compiler->allocator, &compiler->tests, &test);
    }
    edge.tests.index_count = entry_count_of(&compiler->tests) - edge.tests.first_index;
    edge.operations.first_index = entry_count_of(&compiler->operations);
    if (status == SXML_STATUS_OK && is_counted_loop) {
        const counter_operation operation = {.counter =
                                                 node_at(compiler, request.join_node)->counter,
                                             .operation_kind = SXML_COUNTER_OPERATION_INCREMENT};
        status = sxml_schema_scratch_array_append(compiler->allocator, &compiler->operations,
                                                  &operation);
    }
    if (status == SXML_STATUS_OK) {
        status = add_entry_operations(compiler, request.target_position, request.join_node);
    }
    edge.operations.index_count =
        entry_count_of(&compiler->operations) - edge.operations.first_index;
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_scratch_array_append(compiler->allocator, &compiler->edges, &edge);
    }
    return status;
}

/* Edges from every position of one list to every one of another. */
typedef struct edge_product {
    index_run source_positions;
    index_run target_positions;
    uint32_t join_node;
    bool is_loop;
} edge_product;

static sxml_status add_edge_product(content_compiler *compiler, edge_product product) {
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t source = 0;
         status == SXML_STATUS_OK && source < product.source_positions.index_count; source += 1) {
        for (uint32_t target = 0;
             status == SXML_STATUS_OK && target < product.target_positions.index_count;
             target += 1) {
            status = add_edge(
                compiler,
                (edge_request){
                    .source_position = *uint32_at(&compiler->position_lists,
                                                  product.source_positions.first_index + source),
                    .target_position = *uint32_at(&compiler->position_lists,
                                                  product.target_positions.first_index + target),
                    .join_node = product.join_node,
                    .is_loop = product.is_loop});
        }
    }
    return status;
}

/* The follow edges of one node. */
static sxml_status add_node_edges(content_compiler *compiler, uint32_t node_index) {
    const content_node *node = node_at(compiler, node_index);
    sxml_status status = SXML_STATUS_OK;
    if (node->node_kind == SXML_CONTENT_NODE_SEQUENCE) {
        for (uint32_t child = node->first_child; status == SXML_STATUS_OK && child != 0;
             child = node_at(compiler, child)->next_sibling) {
            for (uint32_t later = node_at(compiler, child)->next_sibling;
                 status == SXML_STATUS_OK && later != 0;
                 later = node_at(compiler, later)->next_sibling) {
                status = add_edge_product(
                    compiler,
                    (edge_product){.source_positions = node_at(compiler, child)->last_positions,
                                   .target_positions = node_at(compiler, later)->first_positions,
                                   .join_node = node_index});
                if (!node_at(compiler, later)->is_nullable) {
                    break;
                }
            }
        }
    } else if ((node->node_kind == SXML_CONTENT_NODE_REPEAT &&
                node->maximum_occurrences == SXML_SCHEMA_UNBOUNDED) ||
               node->node_kind == SXML_CONTENT_NODE_COUNTED) {
        status =
            add_edge_product(compiler, (edge_product){.source_positions = node->last_positions,
                                                      .target_positions = node->first_positions,
                                                      .join_node = node_index,
                                                      .is_loop = true});
    }
    return status;
}

/* Every edge, the start's and the final positions, then the edges of
   each position in order (a counting sort by source). */
static sxml_status add_edges(content_compiler *compiler) {
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t node_index = 1;
         status == SXML_STATUS_OK && node_index < compiler->nodes.entry_count; node_index += 1) {
        status = add_node_edges(compiler, node_index);
    }
    const content_node *root =
        compiler->root_node != 0 ? node_at(compiler, compiler->root_node) : NULL;
    /* The start (position 0) to the root's first positions. */
    const index_run root_first =
        root != NULL ? root->first_positions : (index_run){.first_index = 0, .index_count = 0};
    for (uint32_t target = 0; status == SXML_STATUS_OK && target < root_first.index_count;
         target += 1) {
        status = add_edge(
            compiler, (edge_request){.source_position = 0,
                                     .target_position = *uint32_at(&compiler->position_lists,
                                                                   root_first.first_index + target),
                                     .join_node = 0});
    }
    /* Final positions (and the start when the root is nullable). */
    position_at(compiler, 0)->is_final =
        compiler->root_node == 0 || node_at(compiler, compiler->root_node)->is_nullable;
    if (status == SXML_STATUS_OK && compiler->root_node != 0) {
        const index_run last = node_at(compiler, compiler->root_node)->last_positions;
        for (uint32_t entry = 0; status == SXML_STATUS_OK && entry < last.index_count; entry += 1) {
            const uint32_t position =
                *uint32_at(&compiler->position_lists, last.first_index + entry);
            const uint32_t first_test = entry_count_of(&compiler->tests);
            status = add_exit_tests(compiler, position, 0);
            content_position *final_position = position_at(compiler, position);
            final_position->is_final = true;
            final_position->final_tests =
                (index_run){.first_index = first_test,
                            .index_count = entry_count_of(&compiler->tests) - first_test};
        }
    }
    /* Counting sort of the edges by source position. */
    const uint32_t position_count = entry_count_of(&compiler->positions);
    for (uint32_t edge_index = 0;
         status == SXML_STATUS_OK && edge_index < compiler->edges.entry_count; edge_index += 1) {
        const content_edge *edge = sxml_schema_scratch_array_at(&compiler->edges, edge_index);
        position_at(compiler, edge->source_position)->edges.index_count += 1;
    }
    uint32_t next_index = 0;
    for (uint32_t position = 0; position < position_count; position += 1) {
        content_position *entry = position_at(compiler, position);
        entry->edges.first_index = next_index;
        next_index += entry->edges.index_count;
        entry->edges.index_count = 0;
    }
    for (uint32_t edge_index = 0;
         status == SXML_STATUS_OK && edge_index < compiler->edges.entry_count; edge_index += 1) {
        status = append_uint32(compiler, &compiler->edge_order, 0);
    }
    for (uint32_t edge_index = 0;
         status == SXML_STATUS_OK && edge_index < compiler->edges.entry_count; edge_index += 1) {
        const content_edge *edge = sxml_schema_scratch_array_at(&compiler->edges, edge_index);
        content_position *source = position_at(compiler, edge->source_position);
        *uint32_at(&compiler->edge_order, source->edges.first_index + source->edges.index_count) =
            edge_index;
        source->edges.index_count += 1;
    }
    return status;
}

/* -------------------------------------------------- Subset construction */

/* Whether two runs of tests (or of operations) are the same. */
typedef struct run_pair {
    index_run first_run;
    index_run second_run;
} run_pair;

static bool tests_equal(const content_compiler *compiler, run_pair pair_index) {
    if (pair_index.first_run.index_count != pair_index.second_run.index_count) {
        return false;
    }
    for (uint32_t entry = 0; entry < pair_index.first_run.index_count; entry += 1) {
        const counter_test *first = sxml_schema_scratch_array_at(
            &compiler->tests, pair_index.first_run.first_index + entry);
        const counter_test *second = sxml_schema_scratch_array_at(
            &compiler->tests, pair_index.second_run.first_index + entry);
        if (first->counter != second->counter || first->lowest_value != second->lowest_value ||
            first->highest_value != second->highest_value) {
            return false;
        }
    }
    return true;
}

static bool operations_equal(const content_compiler *compiler, run_pair pair_index) {
    if (pair_index.first_run.index_count != pair_index.second_run.index_count) {
        return false;
    }
    for (uint32_t entry = 0; entry < pair_index.first_run.index_count; entry += 1) {
        const counter_operation *first = sxml_schema_scratch_array_at(
            &compiler->operations, pair_index.first_run.first_index + entry);
        const counter_operation *second = sxml_schema_scratch_array_at(
            &compiler->operations, pair_index.second_run.first_index + entry);
        if (first->counter != second->counter || first->operation_kind != second->operation_kind) {
            return false;
        }
    }
    return true;
}

/* Whether some counter values pass both runs of tests. */
static bool tests_overlap(const content_compiler *compiler, run_pair pair_index) {
    for (uint32_t first_entry = 0; first_entry < pair_index.first_run.index_count;
         first_entry += 1) {
        const counter_test *first = sxml_schema_scratch_array_at(
            &compiler->tests, pair_index.first_run.first_index + first_entry);
        for (uint32_t second_entry = 0; second_entry < pair_index.second_run.index_count;
             second_entry += 1) {
            const counter_test *second = sxml_schema_scratch_array_at(
                &compiler->tests, pair_index.second_run.first_index + second_entry);
            if (first->counter == second->counter &&
                (first->highest_value < second->lowest_value ||
                 second->highest_value < first->lowest_value)) {
                return false;
            }
        }
    }
    return true;
}

/* Marks the particles of the counters a run of tests or operations names
   for expansion. */
typedef struct counter_marks {
    index_run tests;
    index_run operations;
} counter_marks;

static sxml_status mark_counter_particle(content_compiler *compiler, uint32_t counter) {
    const content_model_counter *entry = sxml_schema_scratch_array_at(&compiler->counters, counter);
    if (is_expanded_particle(compiler, entry->particle)) {
        return SXML_STATUS_OK;
    }
    compiler->has_new_expansion = true;
    return append_uint32(compiler, &compiler->expanded_particles, entry->particle);
}

static sxml_status mark_counters(content_compiler *compiler, counter_marks marks) {
    if (compiler->allows_counter_ambiguity) {
        /* The last resort when expansion passes the state limit: the
           matcher takes the first transition whose tests hold, an inner
           repetition before an outer one (fewer operations first). Exact
           unless an outer repetition's minimum needs the inner ones spread
           over more repetitions than taking the inner first gives. */
        return SXML_STATUS_OK;
    }
    compiler->has_conflict = true;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t entry = 0; status == SXML_STATUS_OK && entry < marks.tests.index_count;
         entry += 1) {
        const counter_test *test =
            sxml_schema_scratch_array_at(&compiler->tests, marks.tests.first_index + entry);
        status = mark_counter_particle(compiler, test->counter);
    }
    for (uint32_t entry = 0; status == SXML_STATUS_OK && entry < marks.operations.index_count;
         entry += 1) {
        const counter_operation *operation = sxml_schema_scratch_array_at(
            &compiler->operations, marks.operations.first_index + entry);
        status = mark_counter_particle(compiler, operation->counter);
    }
    return status;
}

/* Finds the state of the set in set_buffer, or adds it. */
static sxml_status find_or_add_state(content_compiler *compiler, uint32_t *state) {
    sxml_status status = slot_table_reserve(compiler, SXML_SLOT_TABLE_SETS);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint32_t *wanted = uint32_at(&compiler->set_buffer, 0);
    const size_t wanted_count = compiler->set_buffer.entry_count;
    slot_table *table = &compiler->set_slots;
    size_t slot = (size_t)hash_values(wanted, wanted_count) & (table->slot_count - 1);
    while (table->slots[slot] != 0) {
        const position_set *set =
            sxml_schema_scratch_array_at(&compiler->sets, table->slots[slot] - 1);
        if (set->positions.index_count == wanted_count &&
            memcmp(uint32_at(&compiler->set_positions, set->positions.first_index), wanted,
                   wanted_count * sizeof(uint32_t)) == 0) {
            *state = table->slots[slot] - 1;
            return SXML_STATUS_OK;
        }
        slot = (slot + 1) & (table->slot_count - 1);
    }
    if (compiler->sets.entry_count == 0) {
        /* Set 0 is unused, as state 0 is. */
        const position_set unused = {.positions = {0, 0}};
        const content_model_state unused_state = {.is_final = false};
        status = sxml_schema_scratch_array_append(compiler->allocator, &compiler->sets, &unused);
        if (status == SXML_STATUS_OK) {
            status = sxml_schema_scratch_array_append(compiler->allocator, &compiler->states,
                                                      &unused_state);
        }
    }
    if (status == SXML_STATUS_OK && compiler->sets.entry_count > compiler->state_limit) {
        status = SXML_STATUS_LIMIT_EXCEEDED;
    }
    const position_set set = {.positions = {.first_index = entry_count_of(&compiler->set_positions),
                                            .index_count = (uint32_t)wanted_count}};
    for (size_t entry = 0; status == SXML_STATUS_OK && entry < wanted_count; entry += 1) {
        status = append_uint32(compiler, &compiler->set_positions,
                               *uint32_at(&compiler->set_buffer, entry));
    }
    const uint32_t new_state = entry_count_of(&compiler->sets);
    const content_model_state new_entry = {.is_final = false};
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_scratch_array_append(compiler->allocator, &compiler->sets, &set);
    }
    if (status == SXML_STATUS_OK) {
        status =
            sxml_schema_scratch_array_append(compiler->allocator, &compiler->states, &new_entry);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* The table may have been probed before the arrays grew; the slot is
       still the free one found. */
    table->slots[slot] = new_state + 1;
    table->used_count += 1;
    *state = new_state;
    return SXML_STATUS_OK;
}

static content_model_state *state_at(const content_compiler *compiler, uint32_t state) {
    return sxml_schema_scratch_array_at(&compiler->states, state);
}

static transition_group *group_at(const content_compiler *compiler, uint32_t group) {
    return sxml_schema_scratch_array_at(&compiler->groups, group);
}

/* Groups the edges of a state's positions by particle, tests and
   operations; keys gets (group << 32 | target) for each edge. */
static sxml_status group_state_edges(content_compiler *compiler, uint32_t state) {
    compiler->groups.entry_count = 0;
    compiler->keys.entry_count = 0;
    compiler->touched.entry_count = 0;
    const position_set set =
        *(const position_set *)sxml_schema_scratch_array_at(&compiler->sets, state);
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t member = 0; status == SXML_STATUS_OK && member < set.positions.index_count;
         member += 1) {
        const uint32_t position =
            *uint32_at(&compiler->set_positions, set.positions.first_index + member);
        const index_run edges = position_at(compiler, position)->edges;
        for (uint32_t entry = 0; status == SXML_STATUS_OK && entry < edges.index_count;
             entry += 1) {
            const content_edge edge = *(const content_edge *)sxml_schema_scratch_array_at(
                &compiler->edges, *uint32_at(&compiler->edge_order, edges.first_index + entry));
            const uint32_t local_index =
                position_at(compiler, edge.target_position)->local_particle;
            uint32_t group = local_particle_at(compiler, local_index)->group_head;
            while (
                group != 0 &&
                !(tests_equal(compiler,
                              (run_pair){group_at(compiler, group - 1)->tests, edge.tests}) &&
                  operations_equal(compiler, (run_pair){group_at(compiler, group - 1)->operations,
                                                        edge.operations}))) {
                group = group_at(compiler, group - 1)->next_group;
            }
            if (group == 0) {
                local_particle *particle = local_particle_at(compiler, local_index);
                const transition_group new_group = {.local_particle = local_index,
                                                    .tests = edge.tests,
                                                    .operations = edge.operations,
                                                    .next_group = particle->group_head,
                                                    .smallest_target = edge.target_position};
                if (particle->group_head == 0) {
                    status = append_uint32(compiler, &compiler->touched, local_index);
                }
                if (status == SXML_STATUS_OK) {
                    status = sxml_schema_scratch_array_append(compiler->allocator,
                                                              &compiler->groups, &new_group);
                }
                group = entry_count_of(&compiler->groups);
                local_particle_at(compiler, local_index)->group_head = group;
            } else if (edge.target_position < group_at(compiler, group - 1)->smallest_target) {
                group_at(compiler, group - 1)->smallest_target = edge.target_position;
            }
            const uint64_t key = ((uint64_t)(group - 1) << 32U) | edge.target_position;
            if (status == SXML_STATUS_OK) {
                status =
                    sxml_schema_scratch_array_append(compiler->allocator, &compiler->keys, &key);
            }
        }
    }
    for (size_t entry = 0; entry < compiler->touched.entry_count; entry += 1) {
        local_particle_at(compiler, *uint32_at(&compiler->touched, entry))->group_head = 0;
    }
    return status;
}

/* Whether the state may end, and with which tests. */
static sxml_status make_state_final(content_compiler *compiler, uint32_t state) {
    const position_set set =
        *(const position_set *)sxml_schema_scratch_array_at(&compiler->sets, state);
    bool is_final = false;
    bool is_unconditional = false;
    index_run final_tests = {.first_index = 0, .index_count = 0};
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t member = 0; status == SXML_STATUS_OK && member < set.positions.index_count;
         member += 1) {
        const content_position *position = position_at(
            compiler, *uint32_at(&compiler->set_positions, set.positions.first_index + member));
        if (!position->is_final) {
            continue;
        }
        if (position->final_tests.index_count == 0) {
            is_unconditional = true;
        } else if (is_final &&
                   !tests_equal(compiler, (run_pair){final_tests, position->final_tests})) {
            /* Two ways to end that differ: count no more. */
            status = mark_counters(compiler, (counter_marks){.tests = final_tests});
            if (status == SXML_STATUS_OK) {
                status = mark_counters(compiler, (counter_marks){.tests = position->final_tests});
            }
        }
        if (!is_final) {
            final_tests = position->final_tests;
        }
        is_final = true;
    }
    content_model_state *entry = state_at(compiler, state);
    entry->is_final = is_final;
    entry->final_tests = is_unconditional ? (index_run){0, 0} : final_tests;
    return status;
}

/* The transitions of one state, in the order of their smallest target
   position (document order). */
static sxml_status make_state_transitions(content_compiler *compiler, uint32_t state) {
    sxml_status status = group_state_edges(compiler, state);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint64_t *keys = sxml_schema_scratch_array_at(&compiler->keys, 0);
    sort_keys(keys, compiler->keys.entry_count);
    for (uint32_t key_index = 0; key_index < compiler->keys.entry_count; key_index += 1) {
        const uint64_t key =
            *(const uint64_t *)sxml_schema_scratch_array_at(&compiler->keys, key_index);
        transition_group *group = group_at(compiler, (uint32_t)(key >> 32U));
        if (group->pair_count == 0) {
            group->first_pair = key_index;
        }
        group->pair_count += 1;
    }
    /* The order of the groups: by smallest target, then fewer
       operations, then creation, after the edge keys. */
    const size_t edge_key_count = compiler->keys.entry_count;
    for (uint32_t group = 0; status == SXML_STATUS_OK && group < compiler->groups.entry_count;
         group += 1) {
        const transition_group *entry = group_at(compiler, group);
        const uint64_t operation_count =
            entry->operations.index_count < 15U ? entry->operations.index_count : 15U;
        const uint64_t order_key =
            ((uint64_t)entry->smallest_target << 36U) | (operation_count << 32U) | group;
        status = sxml_schema_scratch_array_append(compiler->allocator, &compiler->keys, &order_key);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sort_keys(sxml_schema_scratch_array_at(&compiler->keys, edge_key_count),
              compiler->keys.entry_count - edge_key_count);
    const uint32_t first_transition = entry_count_of(&compiler->transitions);
    for (size_t order = edge_key_count;
         status == SXML_STATUS_OK && order < compiler->keys.entry_count; order += 1) {
        const uint32_t group_index =
            (uint32_t)(*(const uint64_t *)sxml_schema_scratch_array_at(&compiler->keys, order) &
                       UINT32_MAX);
        const transition_group group = *group_at(compiler, group_index);
        compiler->set_buffer.entry_count = 0;
        for (uint32_t pair_index = 0; status == SXML_STATUS_OK && pair_index < group.pair_count;
             pair_index += 1) {
            const uint32_t target = (uint32_t)(*(const uint64_t *)sxml_schema_scratch_array_at(
                                                   &compiler->keys, group.first_pair + pair_index) &
                                               UINT32_MAX);
            const size_t count = compiler->set_buffer.entry_count;
            if (count == 0 || *uint32_at(&compiler->set_buffer, count - 1) != target) {
                status = append_uint32(compiler, &compiler->set_buffer, target);
            }
        }
        uint32_t target_state = 0;
        if (status == SXML_STATUS_OK) {
            status = find_or_add_state(compiler, &target_state);
        }
        const local_particle *particle = local_particle_at(compiler, group.local_particle);
        const content_model_transition transition = {.term_kind = particle->term_kind,
                                                     .term = particle->term,
                                                     .particle = particle->particle,
                                                     .target_state = target_state,
                                                     .tests = group.tests,
                                                     .operations = group.operations,
                                                     .members = particle->members};
        if (status == SXML_STATUS_OK &&
            compiler->transitions.entry_count >= (size_t)compiler->state_limit * EDGES_PER_STATE) {
            status = SXML_STATUS_LIMIT_EXCEEDED;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_schema_scratch_array_append(compiler->allocator, &compiler->transitions,
                                                      &transition);
        }
    }
    if (status == SXML_STATUS_OK) {
        content_model_state *entry = state_at(compiler, state);
        entry->transitions =
            (index_run){.first_index = first_transition,
                        .index_count = entry_count_of(&compiler->transitions) - first_transition};
        status = make_state_final(compiler, state);
    }
    return status;
}

/* The deterministic automaton: states in the order they are found. */
static sxml_status make_states(content_compiler *compiler) {
    compiler->set_buffer.entry_count = 0;
    sxml_status status = append_uint32(compiler, &compiler->set_buffer, 0);
    uint32_t start_state = 0;
    if (status == SXML_STATUS_OK) {
        status = find_or_add_state(compiler, &start_state);
    }
    for (uint32_t state = 1; status == SXML_STATUS_OK && state < compiler->sets.entry_count;
         state += 1) {
        status = make_state_transitions(compiler, state);
    }
    return status;
}

/* ---------------------------------------------------------- All groups */

/* A frame of the walk over nested all groups. */
typedef struct all_group_frame {
    uint32_t model_group;
    uint32_t next_particle;
} all_group_frame;

/* The members of an all group and of the all groups in it (All Group
   Limited, 3.8.6.2, clause 2: no other model groups). */
static sxml_status collect_all_members(content_compiler *compiler, uint32_t model_group) {
    const sxml_schema_components *components = compiler->components;
    compiler->tasks.entry_count = 0;
    sxml_status status = push_task(compiler, (build_task){.particle = model_group});
    while (status == SXML_STATUS_OK && compiler->tasks.entry_count > 0) {
        build_task *frame =
            sxml_schema_scratch_array_at(&compiler->tasks, compiler->tasks.entry_count - 1);
        const sxml_schema_model_group *group = sxml_schema_get_model_group(
            components, (sxml_schema_model_group_index){.value = frame->particle});
        const sxml_schema_index_view particles = sxml_schema_get_list(components, group->particles);
        if (frame->remaining_count >= particles.index_count) {
            compiler->tasks.entry_count -= 1;
            continue;
        }
        const uint32_t particle_index = particles.indexes[frame->remaining_count];
        frame->remaining_count += 1;
        const sxml_schema_particle *particle = sxml_schema_get_particle(
            components, (sxml_schema_particle_index){.value = particle_index});
        if (particle->maximum_occurrences == 0 || particle->term_kind == SXML_SCHEMA_TERM_NONE) {
            continue;
        }
        if (particle->term_kind != SXML_SCHEMA_TERM_MODEL_GROUP) {
            uint32_t local_index = 0;
            status = find_local_particle(compiler, particle_index, &local_index);
            if (status == SXML_STATUS_OK &&
                compiler->counters.entry_count >= compiler->state_limit) {
                status = SXML_STATUS_LIMIT_EXCEEDED;
            }
            const content_model_counter counter = {
                .minimum_occurrences = particle->minimum_occurrences,
                .maximum_occurrences = particle->maximum_occurrences,
                .saturation_value =
                    particle->maximum_occurrences == SXML_SCHEMA_UNBOUNDED
                        ? (particle->minimum_occurrences > 0 ? particle->minimum_occurrences : 1)
                        : particle->maximum_occurrences,
                .particle = particle_index};
            if (status == SXML_STATUS_OK) {
                status = sxml_schema_scratch_array_append(compiler->allocator, &compiler->counters,
                                                          &counter);
            }
            continue;
        }
        const sxml_schema_model_group *inner = sxml_schema_get_model_group(
            components, (sxml_schema_model_group_index){.value = particle->term});
        if (inner->compositor != SXML_SCHEMA_COMPOSITOR_ALL) {
            status =
                report_error(compiler, (content_report){.constraint_name = "cos-all-limited.2",
                                                        .description = "An all group contains a "
                                                                       "sequence or choice"});
        } else if (particle->minimum_occurrences != 1 || particle->maximum_occurrences != 1) {
            status = report_error(compiler,
                                  (content_report){.constraint_name = "cos-all-limited.1.3",
                                                   .description = "An all group in an all group "
                                                                  "must occur exactly once"});
        } else if (compiler->tasks.entry_count > compiler->state_limit) {
            status = SXML_STATUS_LIMIT_EXCEEDED;
        } else {
            status = push_task(compiler, (build_task){.particle = particle->term});
        }
    }
    return status;
}

/* An all group (3.8.4.1.3): state 1 before any member, state 2 after one;
   member i is counted by counter i. */
static sxml_status make_all_group_states(content_compiler *compiler, uint32_t root_particle) {
    const sxml_schema_particle *root = sxml_schema_get_particle(
        compiler->components, (sxml_schema_particle_index){.value = root_particle});
    if (root->maximum_occurrences > 1 || root->minimum_occurrences > 1) {
        sxml_status status = report_error(
            compiler, (content_report){.constraint_name = "cos-all-limited.1.2",
                                       .description = "An all group's particle must have "
                                                      "maxOccurs 1 and minOccurs 0 or 1"});
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (root->maximum_occurrences == 0) {
        /* An absent all group: only the empty content (3.9.2). */
        const content_model_state states[] = {{.is_final = false}, {.is_final = true}};
        sxml_status status = SXML_STATUS_OK;
        for (size_t state = 0; status == SXML_STATUS_OK && state < 2; state += 1) {
            status = sxml_schema_scratch_array_append(compiler->allocator, &compiler->states,
                                                      &states[state]);
        }
        return status;
    }
    const bool is_optional = root->minimum_occurrences == 0;
    sxml_status status = collect_all_members(compiler, root->term);
    const content_model_state unused_state = {.is_final = false};
    for (uint32_t state = 0; status == SXML_STATUS_OK && state < 3; state += 1) {
        status =
            sxml_schema_scratch_array_append(compiler->allocator, &compiler->states, &unused_state);
    }
    const uint32_t member_count = entry_count_of(&compiler->counters);
    bool may_be_empty = is_optional;
    if (!may_be_empty) {
        may_be_empty = true;
        for (uint32_t member = 0; member < member_count; member += 1) {
            const content_model_counter *counter =
                sxml_schema_scratch_array_at(&compiler->counters, member);
            may_be_empty = may_be_empty && counter->minimum_occurrences == 0;
        }
    }
    /* The final tests of state 2: every member at least its minimum. */
    const uint32_t first_final_test = entry_count_of(&compiler->tests);
    for (uint32_t member = 0; status == SXML_STATUS_OK && member < member_count; member += 1) {
        const content_model_counter *counter =
            sxml_schema_scratch_array_at(&compiler->counters, member);
        if (counter->minimum_occurrences > 0) {
            const counter_test test = {.counter = member,
                                       .lowest_value = counter->minimum_occurrences,
                                       .highest_value = UINT64_MAX};
            status = sxml_schema_scratch_array_append(compiler->allocator, &compiler->tests, &test);
        }
    }
    const index_run final_tests = {.first_index = first_final_test,
                                   .index_count =
                                       entry_count_of(&compiler->tests) - first_final_test};
    for (uint32_t state = 1; status == SXML_STATUS_OK && state <= 2; state += 1) {
        const uint32_t first_transition = entry_count_of(&compiler->transitions);
        for (uint32_t member = 0; status == SXML_STATUS_OK && member < member_count; member += 1) {
            const content_model_counter counter =
                *(const content_model_counter *)sxml_schema_scratch_array_at(&compiler->counters,
                                                                             member);
            uint32_t local_index = 0;
            status = find_local_particle(compiler, counter.particle, &local_index);
            content_model_transition transition = {
                .tests = {.first_index = entry_count_of(&compiler->tests)},
                .operations = {.first_index = entry_count_of(&compiler->operations)},
                .target_state = 2};
            if (status == SXML_STATUS_OK && state == 2 &&
                counter.maximum_occurrences != SXML_SCHEMA_UNBOUNDED) {
                const counter_test test = {.counter = member,
                                           .lowest_value = 0,
                                           .highest_value = counter.maximum_occurrences - 1};
                status =
                    sxml_schema_scratch_array_append(compiler->allocator, &compiler->tests, &test);
                transition.tests.index_count = 1;
            }
            const counter_operation operation = {
                .counter = member, .operation_kind = SXML_COUNTER_OPERATION_INCREMENT};
            if (status == SXML_STATUS_OK) {
                status = sxml_schema_scratch_array_append(compiler->allocator,
                                                          &compiler->operations, &operation);
                transition.operations.index_count = 1;
            }
            if (status == SXML_STATUS_OK) {
                const local_particle *particle = local_particle_at(compiler, local_index);
                transition.term_kind = particle->term_kind;
                transition.term = particle->term;
                transition.particle = particle->particle;
                transition.members = particle->members;
                status = sxml_schema_scratch_array_append(compiler->allocator,
                                                          &compiler->transitions, &transition);
            }
        }
        if (status == SXML_STATUS_OK) {
            content_model_state *entry = state_at(compiler, state);
            entry->transitions = (index_run){.first_index = first_transition,
                                             .index_count = entry_count_of(&compiler->transitions) -
                                                            first_transition};
            entry->is_final = state == 1 ? may_be_empty : true;
            entry->final_tests = state == 1 ? (index_run){0, 0} : final_tests;
        }
    }
    /* states holds the unused state 0 and states 1 and 2. */
    if (status == SXML_STATUS_OK) {
        compiler->states.entry_count = 3;
    }
    return status;
}

/* --------------------------------------------------------------- Checks */

/* A transition of the state being checked. */
static const content_model_transition *transition_at(const content_compiler *compiler,
                                                     uint32_t transition) {
    return sxml_schema_scratch_array_at(&compiler->transitions, transition);
}

/* Two transitions of one state that can both match a name. */
static sxml_status check_transition_pair(content_compiler *compiler, run_pair pair_index) {
    const content_model_transition first =
        *transition_at(compiler, pair_index.first_run.first_index);
    const content_model_transition second =
        *transition_at(compiler, pair_index.second_run.first_index);
    if (!tests_overlap(compiler, (run_pair){first.tests, second.tests})) {
        return SXML_STATUS_OK;
    }
    if (first.particle == second.particle && !compiler->is_all_group) {
        sxml_status status = mark_counters(
            compiler, (counter_marks){.tests = first.tests, .operations = first.operations});
        if (status == SXML_STATUS_OK) {
            status = mark_counters(
                compiler, (counter_marks){.tests = second.tests, .operations = second.operations});
        }
        return status;
    }
    sxml_name_identifier name = {.index = 0};
    if (first.term_kind == SXML_SCHEMA_TERM_ELEMENT_DECLARATION) {
        name =
            sxml_schema_get_element_declaration(
                compiler->components, (sxml_schema_element_declaration_index){.value = first.term})
                ->element_name;
    }
    /* Unique Particle Attribution (3.8.6.4). */
    return report_error(compiler,
                        (content_report){.constraint_name = "cos-nonambig",
                                         .description = "Two particles of the content model "
                                                        "compete for one element",
                                         .name = name});
}

/* Unique Particle Attribution, and transitions of one particle that the
   counters cannot tell apart, for one state: element transitions that
   share a name, wildcard transitions whose namespaces overlap. */
static sxml_status check_state(content_compiler *compiler, uint32_t state) {
    const index_run transitions = state_at(compiler, state)->transitions;
    compiler->keys.entry_count = 0;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t entry = 0; status == SXML_STATUS_OK && entry < transitions.index_count;
         entry += 1) {
        const content_model_transition *transition =
            transition_at(compiler, transitions.first_index + entry);
        if (transition->term_kind != SXML_SCHEMA_TERM_ELEMENT_DECLARATION) {
            continue;
        }
        for (uint32_t member = 0;
             status == SXML_STATUS_OK && member < transition->members.index_count; member += 1) {
            const uint32_t declaration =
                *uint32_at(&compiler->members, transition->members.first_index + member);
            const sxml_name_identifier name =
                sxml_schema_get_element_declaration(
                    compiler->components,
                    (sxml_schema_element_declaration_index){.value = declaration})
                    ->element_name;
            const uint64_t key = ((uint64_t)name.index << 32U) | (transitions.first_index + entry);
            status = sxml_schema_scratch_array_append(compiler->allocator, &compiler->keys, &key);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint64_t *keys = sxml_schema_scratch_array_at(&compiler->keys, 0);
    sort_keys(sxml_schema_scratch_array_at(&compiler->keys, 0), compiler->keys.entry_count);
    for (size_t first = 0; status == SXML_STATUS_OK && first < compiler->keys.entry_count;
         first += 1) {
        for (size_t second = first + 1;
             status == SXML_STATUS_OK && second < compiler->keys.entry_count &&
             (keys[second] >> 32U) == (keys[first] >> 32U);
             second += 1) {
            if ((keys[second] & UINT32_MAX) != (keys[first] & UINT32_MAX)) {
                status = check_transition_pair(
                    compiler, (run_pair){.first_run = {(uint32_t)(keys[first] & UINT32_MAX), 1},
                                         .second_run = {(uint32_t)(keys[second] & UINT32_MAX), 1}});
            }
        }
    }
    for (uint32_t first = 0; status == SXML_STATUS_OK && first < transitions.index_count;
         first += 1) {
        const content_model_transition *first_transition =
            transition_at(compiler, transitions.first_index + first);
        if (first_transition->term_kind != SXML_SCHEMA_TERM_WILDCARD) {
            continue;
        }
        for (uint32_t second = first + 1;
             status == SXML_STATUS_OK && second < transitions.index_count; second += 1) {
            const content_model_transition *second_transition =
                transition_at(compiler, transitions.first_index + second);
            if (second_transition->term_kind == SXML_SCHEMA_TERM_WILDCARD &&
                wildcards_overlap(compiler->components,
                                  (wildcard_pair){.first_wildcard = {first_transition->term},
                                                  .second_wildcard = {second_transition->term}})) {
                status = check_transition_pair(
                    compiler, (run_pair){.first_run = {transitions.first_index + first, 1},
                                         .second_run = {transitions.first_index + second, 1}});
            }
        }
    }
    return status;
}

/* Whether a strict or lax wildcard of the model, or of the type's open
   content, matches the name of one of the model's element declarations
   (3.8.6.3, second paragraph, clause 2; 3.10.4.2: the name is a sibling,
   so a wildcard that disallows ##definedSibling does not match it). */
static bool wildcard_matches_sibling(const content_compiler *compiler, sxml_name_identifier name) {
    const sxml_schema_type_definition *type_definition =
        sxml_schema_get_type_definition(compiler->components, compiler->complex_type);
    const uint32_t particle_count = entry_count_of(&compiler->local_particles);
    for (uint32_t local_index = 0; local_index <= particle_count; local_index += 1) {
        sxml_schema_wildcard_index wildcard = {.value = 0};
        if (local_index < particle_count) {
            const local_particle *particle = local_particle_at(compiler, local_index);
            if (particle->term_kind == SXML_SCHEMA_TERM_WILDCARD) {
                wildcard.value = particle->term;
            }
        } else if (type_definition->open_content_mode != SXML_SCHEMA_OPEN_CONTENT_NONE) {
            wildcard = type_definition->open_content_wildcard;
        }
        if (wildcard.value == 0) {
            continue;
        }
        const sxml_schema_wildcard *definition =
            sxml_schema_get_wildcard(compiler->components, wildcard);
        if (definition->process_contents == SXML_SCHEMA_PROCESS_CONTENTS_SKIP ||
            definition->disallows_sibling) {
            continue;
        }
        if (sxml_schema_wildcard_allows_name(
                compiler->components,
                &(sxml_schema_wildcard_test){
                    .wildcard = wildcard,
                    .name = name,
                    .symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION})) {
            return true;
        }
    }
    return false;
}

/* Element Declarations Consistent (3.8.6.3): declarations of one name,
   also through substitution groups, have one named type; when a strict
   or lax wildcard matches their name and a top-level declaration of it
   exists, their type tables and its are equivalent. */
static sxml_status check_element_consistency(content_compiler *compiler) {
    compiler->keys.entry_count = 0;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t member = 0; status == SXML_STATUS_OK && member < compiler->members.entry_count;
         member += 1) {
        const uint32_t declaration = *uint32_at(&compiler->members, member);
        const sxml_name_identifier name =
            sxml_schema_get_element_declaration(
                compiler->components, (sxml_schema_element_declaration_index){.value = declaration})
                ->element_name;
        const uint64_t key = ((uint64_t)name.index << 32U) | declaration;
        status = sxml_schema_scratch_array_append(compiler->allocator, &compiler->keys, &key);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint64_t *keys = sxml_schema_scratch_array_at(&compiler->keys, 0);
    sort_keys(keys, compiler->keys.entry_count);
    for (size_t entry = 1; status == SXML_STATUS_OK && entry < compiler->keys.entry_count;
         entry += 1) {
        if ((keys[entry] >> 32U) != (keys[entry - 1] >> 32U) || keys[entry] == keys[entry - 1]) {
            continue;
        }
        const sxml_schema_element_declaration *first = sxml_schema_get_element_declaration(
            compiler->components, (sxml_schema_element_declaration_index){
                                      .value = (uint32_t)(keys[entry - 1] & UINT32_MAX)});
        const sxml_schema_element_declaration *second = sxml_schema_get_element_declaration(
            compiler->components,
            (sxml_schema_element_declaration_index){.value = (uint32_t)(keys[entry] & UINT32_MAX)});
        const bool is_named =
            sxml_schema_get_type_definition(compiler->components, first->type_definition)
                ->type_name.index != 0;
        /* 3.8.6.3 clauses 1 to 4: one named type, equivalent type
           tables. */
        if (first->type_definition.value != second->type_definition.value || !is_named ||
            !sxml_schema_type_tables_are_equivalent(
                compiler->components,
                &(sxml_schema_type_table_pair){
                    .first = {.value = (uint32_t)(keys[entry - 1] & UINT32_MAX)},
                    .second = {.value = (uint32_t)(keys[entry] & UINT32_MAX)}})) {
            status = report_error(
                compiler, (content_report){.constraint_name = "cos-element-consistent",
                                           .description = "Two element declarations of one name "
                                                          "in a content model have different types",
                                           .name = first->element_name});
        }
    }
    /* The second paragraph: declarations a wildcard competes with. */
    for (size_t entry = 0; status == SXML_STATUS_OK && !compiler->has_reported &&
                           entry < compiler->keys.entry_count;
         entry += 1) {
        const sxml_schema_element_declaration_index local = {
            .value = (uint32_t)(keys[entry] & UINT32_MAX)};
        const sxml_name_identifier name =
            sxml_schema_get_element_declaration(compiler->components, local)->element_name;
        uint32_t global = 0;
        if (!sxml_schema_components_find_name(
                compiler->components,
                (sxml_schema_name_key){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION,
                                       .name = name},
                &global) ||
            global == local.value || !wildcard_matches_sibling(compiler, name)) {
            continue;
        }
        if (!sxml_schema_type_tables_are_equivalent(
                compiler->components,
                &(sxml_schema_type_table_pair){.first = local, .second = {.value = global}})) {
            status = report_error(
                compiler,
                (content_report){.constraint_name = "cos-element-consistent",
                                 .description = "An element declaration and the top-level "
                                                "declaration of its name, which a wildcard of the "
                                                "content model matches, have different type tables",
                                 .name = name});
        }
    }
    return status;
}

/* The names of the model's element particles and of the members of their
   substitution groups, sorted and unique, for ##definedSibling (3.10.4.2:
   "the elements explicitly mentioned in the content model, and all
   elements substitutable for those elements"). */
static sxml_status collect_sibling_names(content_compiler *compiler) {
    compiler->keys.entry_count = 0;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t local_index = 0;
         status == SXML_STATUS_OK && local_index < compiler->local_particles.entry_count;
         local_index += 1) {
        const local_particle *particle = local_particle_at(compiler, local_index);
        if (particle->term_kind != SXML_SCHEMA_TERM_ELEMENT_DECLARATION) {
            continue;
        }
        const sxml_schema_element_declaration *declaration = sxml_schema_get_element_declaration(
            compiler->components, (sxml_schema_element_declaration_index){.value = particle->term});
        const uint64_t key = declaration->element_name.index;
        status = sxml_schema_scratch_array_append(compiler->allocator, &compiler->keys, &key);
        const sxml_schema_index_view members =
            sxml_schema_get_list(compiler->components, declaration->substitution_group_members);
        for (size_t member = 0; status == SXML_STATUS_OK && member < members.index_count;
             member += 1) {
            const uint64_t member_key =
                sxml_schema_get_element_declaration(
                    compiler->components,
                    (sxml_schema_element_declaration_index){.value = members.indexes[member]})
                    ->element_name.index;
            status =
                sxml_schema_scratch_array_append(compiler->allocator, &compiler->keys, &member_key);
        }
    }
    uint64_t *keys = sxml_schema_scratch_array_at(&compiler->keys, 0);
    sort_keys(keys, compiler->keys.entry_count);
    for (size_t entry = 0; status == SXML_STATUS_OK && entry < compiler->keys.entry_count;
         entry += 1) {
        if (entry == 0 || keys[entry] != keys[entry - 1]) {
            status = append_uint32(compiler, &compiler->sibling_names, (uint32_t)keys[entry]);
        }
    }
    return status;
}

/* ---------------------------------------------------------- The block */

/* One array of the block: where it goes and what is copied there. */
typedef struct block_part {
    const sxml_schema_scratch_array *source;
    size_t alignment;
    size_t offset;
} block_part;

/* Copies the model into a block of the components and writes it to the
   type's content_model. */
static sxml_status finish_model(content_compiler *compiler) {
    const sxml_schema_type_definition *type_definition =
        sxml_schema_get_type_definition(compiler->components, compiler->complex_type);
    sxml_schema_content_model model = {
        .complex_type = compiler->complex_type,
        .is_all_group = compiler->is_all_group,
        .counter_count = entry_count_of(&compiler->counters),
        .open_content_mode = type_definition->open_content_wildcard.value != 0
                                 ? type_definition->open_content_mode
                                 : SXML_SCHEMA_OPEN_CONTENT_NONE,
        .open_content_wildcard = type_definition->open_content_wildcard};
    sxml_status status = SXML_STATUS_OK;
    if (model.open_content_mode == SXML_SCHEMA_OPEN_CONTENT_SUFFIX) {
        model.suffix_state = entry_count_of(&compiler->states);
        const content_model_state suffix = {.is_final = true};
        status = sxml_schema_scratch_array_append(compiler->allocator, &compiler->states, &suffix);
    }
    model.state_count = entry_count_of(&compiler->states) - 1;
    block_part parts[] = {
        {&compiler->states, alignof(content_model_state), 0},
        {&compiler->transitions, alignof(content_model_transition), 0},
        {&compiler->tests, alignof(counter_test), 0},
        {&compiler->operations, alignof(counter_operation), 0},
        {&compiler->members, alignof(uint32_t), 0},
        {&compiler->counters, alignof(content_model_counter), 0},
        {&compiler->sibling_names, alignof(uint32_t), 0},
    };
    const size_t part_count = sizeof(parts) / sizeof(parts[0]);
    sxml_memory_requirements requirements = {.byte_count = sizeof(sxml_schema_content_model),
                                             .alignment = alignof(sxml_schema_content_model)};
    for (size_t part_index = 0; status == SXML_STATUS_OK && part_index < part_count;
         part_index += 1) {
        status = sxml_memory_requirements_append(
            &requirements,
            (sxml_memory_requirements){.byte_count = parts[part_index].source->entry_count *
                                                     parts[part_index].source->entry_byte_count,
                                       .alignment = parts[part_index].alignment},
            &parts[part_index].offset);
    }
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_components_allocate_block(
            compiler->components,
            (sxml_allocation_request){.byte_count = requirements.byte_count,
                                      .alignment = requirements.alignment},
            &memory);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *bytes = memory;
    for (size_t part_index = 0; part_index < part_count; part_index += 1) {
        const size_t byte_count =
            parts[part_index].source->entry_count * parts[part_index].source->entry_byte_count;
        if (byte_count != 0) {
            memcpy(bytes + parts[part_index].offset, parts[part_index].source->bytes, byte_count);
        }
    }
    model.states = (const void *)(bytes + parts[0].offset);
    model.transitions = (const void *)(bytes + parts[1].offset);
    model.tests = (const void *)(bytes + parts[2].offset);
    model.operations = (const void *)(bytes + parts[3].offset);
    model.members = (const void *)(bytes + parts[4].offset);
    model.counters = (const void *)(bytes + parts[5].offset);
    model.sibling_names = (const void *)(bytes + parts[6].offset);
    model.sibling_name_count = entry_count_of(&compiler->sibling_names);
    memcpy(bytes, &model, sizeof(model));
    sxml_schema_type_definition *edited = sxml_schema_components_edit_entry(
        compiler->components, (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_TYPE_DEFINITION,
                                                      .index = compiler->complex_type.value});
    if (edited == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    edited->content_model = memory;
    return SXML_STATUS_OK;
}

/* One attempt: the states, their checks; conflicts are left to the
   caller. */
static sxml_status compile_attempt(content_compiler *compiler, uint32_t root_particle) {
    sxml_status status = SXML_STATUS_OK;
    if (compiler->is_all_group) {
        status = make_all_group_states(compiler, root_particle);
    } else {
        status = build_tree(compiler, root_particle);
        if (status == SXML_STATUS_OK) {
            status = compute_node_lists(compiler);
        }
        if (status == SXML_STATUS_OK) {
            status = add_edges(compiler);
        }
        if (status == SXML_STATUS_OK) {
            status = make_states(compiler);
        }
    }
    for (uint32_t state = 1; status == SXML_STATUS_OK && state < compiler->states.entry_count;
         state += 1) {
        status = check_state(compiler, state);
    }
    return status;
}

static sxml_status check_content_restriction(content_compiler *compiler);

sxml_status sxml_schema_compile_content_model(sxml_schema_compilation *compilation,
                                              sxml_schema_type_definition_index complex_type) {
    const sxml_schema_type_definition *type_definition =
        sxml_schema_get_type_definition(compilation->components, complex_type);
    if ((type_definition->content_type_variety != SXML_CONTENT_TYPE_VARIETY_ELEMENT_ONLY &&
         type_definition->content_type_variety != SXML_CONTENT_TYPE_VARIETY_MIXED) ||
        type_definition->particle.value == 0) {
        return SXML_STATUS_OK;
    }
    const uint32_t configured_limit = compilation->configuration->maximum_content_model_state_count;
    content_compiler compiler = {.compilation = compilation,
                                 .components = compilation->components,
                                 .allocator = compilation->components->allocator,
                                 .complex_type = complex_type,
                                 .state_limit = configured_limit != 0 ? configured_limit
                                                                      : DEFAULT_MAXIMUM_STATE_COUNT,
                                 .expansion_bound = FIRST_EXPANSION_BOUND};
    compiler_initialize_arrays(&compiler);
    const uint32_t root_particle = type_definition->particle.value;
    const sxml_schema_particle *root =
        sxml_schema_get_particle(compilation->components, type_definition->particle);
    compiler.is_all_group =
        root->term_kind == SXML_SCHEMA_TERM_MODEL_GROUP &&
        sxml_schema_get_model_group(compilation->components,
                                    (sxml_schema_model_group_index){.value = root->term})
                ->compositor == SXML_SCHEMA_COMPOSITOR_ALL;
    sxml_status status = SXML_STATUS_OK;
    for (;;) {
        compiler_reset(&compiler);
        status = compile_attempt(&compiler, root_particle);
        if (status == SXML_STATUS_LIMIT_EXCEEDED && compiler.expansion_bound > 1) {
            /* Count every repetition and try again. */
            compiler.expansion_bound = 1;
            continue;
        }
        if (status == SXML_STATUS_LIMIT_EXCEEDED && compiler.expanded_particles.entry_count != 0 &&
            !compiler.allows_counter_ambiguity) {
            /* What expanding the undecided counters needs is too much. */
            compiler.allows_counter_ambiguity = true;
            compiler.expanded_particles.entry_count = 0;
            continue;
        }
        if (status != SXML_STATUS_OK || compiler.has_reported) {
            break;
        }
        if (compiler.has_conflict) {
            if (compiler.has_new_expansion) {
                continue;
            }
            status = SXML_STATUS_LIMIT_EXCEEDED;
            break;
        }
        status = check_element_consistency(&compiler);
        if (status == SXML_STATUS_OK) {
            status = collect_sibling_names(&compiler);
        }
        if (status == SXML_STATUS_OK && !compiler.has_reported) {
            status = finish_model(&compiler);
        }
        if (status == SXML_STATUS_OK && !compiler.has_reported) {
            status = check_content_restriction(&compiler);
        }
        break;
    }
    compiler_release(&compiler);
    return status;
}

/* -------------------------------------------------------------- Matcher */

uint32_t
sxml_schema_content_model_get_counter_count(const sxml_schema_content_model *content_model) {
    return content_model->counter_count;
}

void sxml_schema_content_model_start(sxml_schema_content_model_cursor *cursor) {
    cursor->state = 1;
    for (uint32_t counter = 0; counter < cursor->counter_count; counter += 1) {
        cursor->counter_values[counter] = 0;
    }
}

static bool cursor_tests_hold(const sxml_schema_content_model_cursor *cursor, index_run tests) {
    const sxml_schema_content_model *model = cursor->content_model;
    for (uint32_t entry = 0; entry < tests.index_count; entry += 1) {
        const counter_test *test = &model->tests[tests.first_index + entry];
        const uint64_t value =
            test->counter < cursor->counter_count ? cursor->counter_values[test->counter] : 0;
        if (value < test->lowest_value || value > test->highest_value) {
            return false;
        }
    }
    return true;
}

static void cursor_apply_operations(sxml_schema_content_model_cursor *cursor,
                                    index_run operations) {
    const sxml_schema_content_model *model = cursor->content_model;
    for (uint32_t entry = 0; entry < operations.index_count; entry += 1) {
        const counter_operation *operation = &model->operations[operations.first_index + entry];
        if (operation->counter >= cursor->counter_count) {
            continue;
        }
        uint64_t *value = &cursor->counter_values[operation->counter];
        if (operation->operation_kind == SXML_COUNTER_OPERATION_SET_TO_ONE) {
            *value = 1;
        } else if (*value < model->counters[operation->counter].saturation_value) {
            *value += 1;
        }
    }
}

bool sxml_schema_content_model_can_end(const sxml_schema_content_model_cursor *cursor) {
    const sxml_schema_content_model *model = cursor->content_model;
    if (cursor->state == 0 || cursor->state > model->state_count) {
        return false;
    }
    const content_model_state *state = &model->states[cursor->state];
    return state->is_final && cursor_tests_hold(cursor, state->final_tests);
}

/* Whether a wildcard of this model allows a name (with ##definedSibling). */
static bool model_wildcard_allows(const sxml_schema_components *components,
                                  const sxml_schema_content_model *model,
                                  const wildcard_name *test) {
    if (!wildcard_allows(components, test)) {
        return false;
    }
    if (!sxml_schema_get_wildcard(components, test->wildcard)->disallows_sibling) {
        return true;
    }
    for (uint32_t entry = 0; entry < model->sibling_name_count; entry += 1) {
        if (model->sibling_names[entry] == test->name.index) {
            return false;
        }
    }
    return true;
}

/* The transition a name takes from the cursor's state: an element
   transition before a wildcard one (3.8.6.4). */
typedef struct transition_choice {
    const content_model_transition *transition;
    uint32_t element_declaration;
} transition_choice;

static transition_choice choose_transition(const sxml_schema_components *components,
                                           const sxml_schema_content_model_cursor *cursor,
                                           const wildcard_name *name) {
    const sxml_schema_content_model *model = cursor->content_model;
    const content_model_state *state = &model->states[cursor->state];
    for (uint32_t entry = 0; entry < state->transitions.index_count; entry += 1) {
        const content_model_transition *transition =
            &model->transitions[state->transitions.first_index + entry];
        if (transition->term_kind != SXML_SCHEMA_TERM_ELEMENT_DECLARATION ||
            !cursor_tests_hold(cursor, transition->tests)) {
            continue;
        }
        for (uint32_t member = 0; member < transition->members.index_count; member += 1) {
            const uint32_t declaration = model->members[transition->members.first_index + member];
            if (sxml_schema_get_element_declaration(
                    components, (sxml_schema_element_declaration_index){.value = declaration})
                    ->element_name.index == name->name.index) {
                return (transition_choice){.transition = transition,
                                           .element_declaration = declaration};
            }
        }
    }
    for (uint32_t entry = 0; entry < state->transitions.index_count; entry += 1) {
        const content_model_transition *transition =
            &model->transitions[state->transitions.first_index + entry];
        wildcard_name test = *name;
        test.wildcard.value = transition->term;
        if (transition->term_kind == SXML_SCHEMA_TERM_WILDCARD &&
            cursor_tests_hold(cursor, transition->tests) &&
            model_wildcard_allows(components, model, &test)) {
            return (transition_choice){.transition = transition, .element_declaration = 0};
        }
    }
    return (transition_choice){.transition = NULL, .element_declaration = 0};
}

/* A cursor to step by a name whose namespace is known. */
typedef struct cursor_step {
    sxml_schema_content_model_cursor *cursor;
    const wildcard_name *name;
} cursor_step;

static void step_cursor(const sxml_schema_components *components, cursor_step step_request,
                        sxml_schema_content_model_match *match) {
    sxml_schema_content_model_cursor *cursor = step_request.cursor;
    const wildcard_name *name = step_request.name;
    const sxml_schema_content_model *model = cursor->content_model;
    *match = (sxml_schema_content_model_match){.match_kind = SXML_SCHEMA_MATCH_NONE};
    if (cursor->state == 0 || cursor->state > model->state_count) {
        return;
    }
    const transition_choice choice = choose_transition(components, cursor, name);
    if (choice.transition != NULL) {
        cursor_apply_operations(cursor, choice.transition->operations);
        cursor->state = choice.transition->target_state;
        if (choice.transition->term_kind == SXML_SCHEMA_TERM_ELEMENT_DECLARATION) {
            match->match_kind = SXML_SCHEMA_MATCH_ELEMENT_DECLARATION;
            match->element_declaration.value = choice.element_declaration;
        } else {
            match->match_kind = SXML_SCHEMA_MATCH_WILDCARD;
            match->wildcard.value = choice.transition->term;
        }
        return;
    }
    /* Open content (3.4.4.2, clause 1.2 of Element Locally Valid (Complex
       Type)): a name no particle takes, if the open content wildcard
       allows it, anywhere (interleave) or once the content could end
       (suffix). */
    wildcard_name test = *name;
    test.wildcard = model->open_content_wildcard;
    if (model->open_content_mode == SXML_SCHEMA_OPEN_CONTENT_NONE ||
        !model_wildcard_allows(components, model, &test)) {
        return;
    }
    if (model->open_content_mode == SXML_SCHEMA_OPEN_CONTENT_SUFFIX) {
        if (cursor->state != model->suffix_state && !sxml_schema_content_model_can_end(cursor)) {
            return;
        }
        cursor->state = model->suffix_state;
    }
    match->match_kind = SXML_SCHEMA_MATCH_WILDCARD;
    match->wildcard = model->open_content_wildcard;
}

sxml_status sxml_schema_content_model_step(const sxml_schema_components *components,
                                           const sxml_schema_content_model_step_request *request,
                                           sxml_schema_content_model_match *match) {
    sxml_name_parts parts = {.namespace_uri = {.index = 0}};
    const sxml_status status = sxml_name_pool_get_name_parts(components->processor->name_pool,
                                                             request->element_name, &parts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const wildcard_name name = {.name = request->element_name,
                                .namespace_uri = parts.namespace_uri,
                                .symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION};
    step_cursor(components, (cursor_step){.cursor = request->cursor, .name = &name}, match);
    return SXML_STATUS_OK;
}

static void add_expected_term(sxml_schema_expected_term_buffer *buffer,
                              sxml_schema_content_model_match term) {
    if (buffer->term_count < buffer->term_capacity) {
        buffer->terms[buffer->term_count] = term;
        buffer->term_count += 1;
    }
}

void sxml_schema_content_model_list_expected(const sxml_schema_components *components,
                                             const sxml_schema_content_model_cursor *cursor,
                                             sxml_schema_expected_term_buffer *buffer) {
    (void)components;
    const sxml_schema_content_model *model = cursor->content_model;
    buffer->term_count = 0;
    if (cursor->state == 0 || cursor->state > model->state_count) {
        return;
    }
    const content_model_state *state = &model->states[cursor->state];
    for (uint32_t entry = 0; entry < state->transitions.index_count; entry += 1) {
        const content_model_transition *transition =
            &model->transitions[state->transitions.first_index + entry];
        if (!cursor_tests_hold(cursor, transition->tests)) {
            continue;
        }
        if (transition->term_kind == SXML_SCHEMA_TERM_ELEMENT_DECLARATION) {
            add_expected_term(buffer, (sxml_schema_content_model_match){
                                          .match_kind = SXML_SCHEMA_MATCH_ELEMENT_DECLARATION,
                                          .element_declaration = {.value = transition->term}});
        } else {
            add_expected_term(
                buffer, (sxml_schema_content_model_match){.match_kind = SXML_SCHEMA_MATCH_WILDCARD,
                                                          .wildcard = {.value = transition->term}});
        }
    }
    const bool takes_open_content =
        model->open_content_mode == SXML_SCHEMA_OPEN_CONTENT_INTERLEAVE ||
        (model->open_content_mode == SXML_SCHEMA_OPEN_CONTENT_SUFFIX &&
         (cursor->state == model->suffix_state || sxml_schema_content_model_can_end(cursor)));
    if (takes_open_content) {
        add_expected_term(
            buffer, (sxml_schema_content_model_match){.match_kind = SXML_SCHEMA_MATCH_WILDCARD,
                                                      .wildcard = model->open_content_wildcard});
    }
}

/* ---------------------------------------------- Restriction of content */

/* Configurations the walk visits at most; past them the restriction is
   accepted, as 3.4.6.3 allows a processor to accept provisionally what it
   cannot decide from the schema alone. */
#define MAXIMUM_CONFIGURATION_COUNT 65536U

typedef enum binding_strength {
    SXML_BINDING_SKIP,
    SXML_BINDING_LAX,
    SXML_BINDING_STRICT,
} binding_strength;

static binding_strength wildcard_strength(const sxml_schema_components *components,
                                          sxml_schema_wildcard_index wildcard) {
    switch (sxml_schema_get_wildcard(components, wildcard)->process_contents) {
    case SXML_SCHEMA_PROCESS_CONTENTS_SKIP:
        return SXML_BINDING_SKIP;
    case SXML_SCHEMA_PROCESS_CONTENTS_LAX:
        return SXML_BINDING_LAX;
    case SXML_SCHEMA_PROCESS_CONTENTS_STRICT:
        return SXML_BINDING_STRICT;
    }
    return SXML_BINDING_STRICT;
}

/* Two element declarations: the general (base) and the specific one. */
typedef struct declaration_pair {
    sxml_schema_element_declaration_index general;
    sxml_schema_element_declaration_index specific;
} declaration_pair;

/* A declaration subsumes another (3.4.6.4, default bindings, clause 4). */
static bool declaration_subsumes(const sxml_schema_components *components,
                                 declaration_pair pair_index) {
    if (pair_index.general.value == pair_index.specific.value) {
        return true;
    }
    const sxml_schema_element_declaration *general =
        sxml_schema_get_element_declaration(components, pair_index.general);
    const sxml_schema_element_declaration *specific =
        sxml_schema_get_element_declaration(components, pair_index.specific);
    if (specific->is_nillable && !general->is_nillable) {
        return false;
    }
    /* A fixed value: the same, equal or identical, in the specific
       declaration's type. */
    if (general->value_constraint.variety == SXML_SCHEMA_VALUE_CONSTRAINT_FIXED &&
        (specific->value_constraint.variety != SXML_SCHEMA_VALUE_CONSTRAINT_FIXED ||
         !sxml_schema_value_constraints_are_equal(
             components,
             &(sxml_schema_value_comparison){.first = general->value_constraint,
                                             .second = specific->value_constraint,
                                             .type_definition = specific->type_definition}))) {
        return false;
    }
    if ((general->disallowed_substitutions & ~specific->disallowed_substitutions) != 0) {
        return false;
    }
    /* Clause 4.6: both type tables absent, or present and equivalent. */
    if (!sxml_schema_type_tables_are_equivalent(
            components, &(sxml_schema_type_table_pair){.first = pair_index.general,
                                                       .second = pair_index.specific})) {
        return false;
    }
    const sxml_schema_index_view general_constraints =
        sxml_schema_get_list(components, general->identity_constraints);
    const sxml_schema_index_view specific_constraints =
        sxml_schema_get_list(components, specific->identity_constraints);
    for (size_t general_entry = 0; general_entry < general_constraints.index_count;
         general_entry += 1) {
        bool is_present = false;
        for (size_t specific_entry = 0; specific_entry < specific_constraints.index_count;
             specific_entry += 1) {
            is_present = is_present || specific_constraints.indexes[specific_entry] ==
                                           general_constraints.indexes[general_entry];
        }
        if (!is_present) {
            return false;
        }
    }
    sxml_schema_derivation_outcome outcome = {.is_derived = false};
    const sxml_status status = sxml_schema_check_derivation(
        components,
        &(sxml_schema_derivation_request){
            .derived_type = specific->type_definition,
            .base_type = general->type_definition,
            .blocked_derivations = (sxml_schema_derivation_set)(SXML_SCHEMA_DERIVATION_EXTENSION |
                                                                SXML_SCHEMA_DERIVATION_LIST |
                                                                SXML_SCHEMA_DERIVATION_UNION)},
        &outcome);
    return status == SXML_STATUS_OK && outcome.is_derived;
}

/* The global element declaration of a name, or 0. */
static sxml_schema_element_declaration_index
global_declaration(const sxml_schema_components *components, sxml_name_identifier name) {
    uint32_t component = 0;
    if (!sxml_schema_components_find_name(
            components,
            (sxml_schema_name_key){.symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION,
                                   .name = name},
            &component)) {
        component = 0;
    }
    return (sxml_schema_element_declaration_index){.value = component};
}

bool sxml_schema_wildcard_is_subset(const sxml_schema_components *components,
                                    const sxml_schema_wildcard_subset_request *request) {
    const sxml_schema_wildcard *subset = sxml_schema_get_wildcard(components, request->subset);
    const sxml_schema_wildcard *superset = sxml_schema_get_wildcard(components, request->superset);
    bool is_namespace_subset = true;
    /* Clause 1: the namespace constraints (cos-ns-subset). */
    if (superset->namespace_constraint_variety != SXML_SCHEMA_NAMESPACE_CONSTRAINT_ANY) {
        const sxml_schema_index_view namespaces =
            sxml_schema_get_list(components, subset->namespaces);
        if (subset->namespace_constraint_variety == SXML_SCHEMA_NAMESPACE_CONSTRAINT_ENUMERATION) {
            for (size_t entry = 0; is_namespace_subset && entry < namespaces.index_count;
                 entry += 1) {
                is_namespace_subset =
                    namespace_constraint_allows(components, superset, namespaces.indexes[entry]);
            }
        } else if (subset->namespace_constraint_variety == SXML_SCHEMA_NAMESPACE_CONSTRAINT_ANY ||
                   superset->namespace_constraint_variety !=
                       SXML_SCHEMA_NAMESPACE_CONSTRAINT_NOT) {
            is_namespace_subset = false;
        } else {
            /* not(A) within not(B): B within A. */
            const sxml_schema_index_view excluded =
                sxml_schema_get_list(components, superset->namespaces);
            for (size_t entry = 0; is_namespace_subset && entry < excluded.index_count;
                 entry += 1) {
                is_namespace_subset =
                    !namespace_constraint_allows(components, subset, excluded.indexes[entry]);
            }
        }
    }
    if (!is_namespace_subset) {
        return false;
    }
    /* Clauses 2 to 4: what the superset disallows, the subset does not
       allow; its keywords defined and sibling the subset has too. */
    if ((superset->disallows_defined && !subset->disallows_defined) ||
        (superset->disallows_sibling && !subset->disallows_sibling)) {
        return false;
    }
    const sxml_schema_index_view disallowed =
        sxml_schema_get_list(components, superset->disallowed_names);
    for (size_t entry = 0; entry < disallowed.index_count; entry += 1) {
        if (sxml_schema_wildcard_allows_name(
                components,
                &(sxml_schema_wildcard_test){.wildcard = request->subset,
                                             .name = {.index = disallowed.indexes[entry]},
                                             .symbol_space = request->symbol_space})) {
            return false;
        }
    }
    return true;
}

/* The walk over the product of the two automata. */
typedef struct restriction_walk {
    content_compiler *compiler;
    const sxml_schema_content_model *restricted_model;
    const sxml_schema_content_model *base_model;
    /* Counter values: current and next, restricted and base. */
    uint64_t *values;
    sxml_schema_content_model_cursor restricted;
    sxml_schema_content_model_cursor base;
    sxml_schema_content_model_cursor restricted_next;
    sxml_schema_content_model_cursor base_next;
    bool is_violated;
} restriction_walk;

/* Adds the configuration (restricted_next, base_next) unless seen. */
static sxml_status add_configuration(restriction_walk *walk) {
    content_compiler *compiler = walk->compiler;
    const uint32_t width = compiler->configuration_width;
    const size_t row_count = compiler->keys.entry_count / width;
    if (row_count >= MAXIMUM_CONFIGURATION_COUNT) {
        return SXML_STATUS_OK;
    }
    const size_t first_value = compiler->keys.entry_count;
    sxml_status status = SXML_STATUS_OK;
    uint64_t value = walk->restricted_next.state;
    status = sxml_schema_scratch_array_append(compiler->allocator, &compiler->keys, &value);
    for (uint32_t counter = 0;
         status == SXML_STATUS_OK && counter < walk->restricted_next.counter_count; counter += 1) {
        status = sxml_schema_scratch_array_append(compiler->allocator, &compiler->keys,
                                                  &walk->restricted_next.counter_values[counter]);
    }
    value = walk->base_next.state;
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_scratch_array_append(compiler->allocator, &compiler->keys, &value);
    }
    for (uint32_t counter = 0; status == SXML_STATUS_OK && counter < walk->base_next.counter_count;
         counter += 1) {
        status = sxml_schema_scratch_array_append(compiler->allocator, &compiler->keys,
                                                  &walk->base_next.counter_values[counter]);
    }
    if (status == SXML_STATUS_OK) {
        status = slot_table_reserve(compiler, SXML_SLOT_TABLE_CONFIGURATIONS);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint64_t *row_values = sxml_schema_scratch_array_at(&compiler->keys, first_value);
    slot_table *table = &compiler->set_slots;
    size_t slot = (size_t)hash_row(row_values, width) & (table->slot_count - 1);
    while (table->slots[slot] != 0) {
        const uint64_t *seen =
            sxml_schema_scratch_array_at(&compiler->keys, (size_t)(table->slots[slot] - 1) * width);
        if (memcmp(seen, row_values, width * sizeof(uint64_t)) == 0) {
            compiler->keys.entry_count = first_value;
            return SXML_STATUS_OK;
        }
        slot = (slot + 1) & (table->slot_count - 1);
    }
    table->slots[slot] = (uint32_t)row_count + 1;
    table->used_count += 1;
    return SXML_STATUS_OK;
}

/* Loads configuration row into the current cursors. */
static void load_configuration(restriction_walk *walk, size_t row_index) {
    const content_compiler *compiler = walk->compiler;
    const uint64_t *values =
        sxml_schema_scratch_array_at(&compiler->keys, row_index * compiler->configuration_width);
    walk->restricted.state = (uint32_t)values[0];
    for (uint32_t counter = 0; counter < walk->restricted.counter_count; counter += 1) {
        walk->restricted.counter_values[counter] = values[1 + counter];
    }
    const size_t base_offset = 1 + (size_t)walk->restricted.counter_count;
    walk->base.state = (uint32_t)values[base_offset];
    for (uint32_t counter = 0; counter < walk->base.counter_count; counter += 1) {
        walk->base.counter_values[counter] = values[base_offset + 1 + counter];
    }
}

static void copy_cursor(sxml_schema_content_model_cursor *target,
                        const sxml_schema_content_model_cursor *source) {
    target->state = source->state;
    for (uint32_t counter = 0; counter < source->counter_count; counter += 1) {
        target->counter_values[counter] = source->counter_values[counter];
    }
}

/* The base's answer to one element of the restriction: base_next is
   moved from base. */
typedef struct element_move {
    sxml_name_identifier name;
    /* The restriction's default binding: a declaration, or 0 with the
       strength of its wildcard. */
    sxml_schema_element_declaration_index declaration;
    binding_strength strength;
} element_move;

static sxml_status check_element_move(restriction_walk *walk, element_move move) {
    const sxml_schema_components *components = walk->compiler->components;
    sxml_name_parts parts = {.namespace_uri = {.index = 0}};
    const sxml_status status =
        sxml_name_pool_get_name_parts(components->processor->name_pool, move.name, &parts);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const wildcard_name name = {.name = move.name,
                                .namespace_uri = parts.namespace_uri,
                                .symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION};
    copy_cursor(&walk->base_next, &walk->base);
    sxml_schema_content_model_match match = {.match_kind = SXML_SCHEMA_MATCH_NONE};
    step_cursor(components, (cursor_step){.cursor = &walk->base_next, .name = &name}, &match);
    if (match.match_kind == SXML_SCHEMA_MATCH_NONE) {
        walk->is_violated = true;
        return SXML_STATUS_OK;
    }
    /* B's default binding: its declaration, or through its wildcard the
       global declaration (unless skip), or the wildcard's keyword. */
    sxml_schema_element_declaration_index general = match.element_declaration;
    binding_strength general_strength = SXML_BINDING_STRICT;
    if (match.match_kind == SXML_SCHEMA_MATCH_WILDCARD) {
        general_strength = wildcard_strength(components, match.wildcard);
        general = general_strength == SXML_BINDING_SKIP ? (sxml_schema_element_declaration_index){0}
                                                        : global_declaration(components, move.name);
    }
    bool subsumes = false;
    if (general.value == 0) {
        /* A base wildcard without a global declaration of the name takes
           the restriction's declaration, as processors do (XSD 1.0's
           NSCompat, 3.9.6): read literally, 3.4.6.4 would refuse a
           strict wildcard restricted to a local element. */
        subsumes = general_strength == SXML_BINDING_SKIP || move.declaration.value != 0 ||
                   (general_strength == SXML_BINDING_LAX && move.strength != SXML_BINDING_SKIP) ||
                   move.strength == SXML_BINDING_STRICT;
    } else if (move.declaration.value != 0) {
        subsumes = declaration_subsumes(
            components, (declaration_pair){.general = general, .specific = move.declaration});
    }
    if (!subsumes) {
        walk->is_violated = true;
        return SXML_STATUS_OK;
    }
    return add_configuration(walk);
}

/* Whether the restriction takes a name by an element transition from
   its current configuration: its wildcard then never sees it (3.8.6.4). */
static bool restriction_takes_by_element(const restriction_walk *walk, sxml_name_identifier name) {
    const sxml_schema_components *components = walk->compiler->components;
    sxml_name_parts parts = {.namespace_uri = {.index = 0}};
    if (sxml_name_pool_get_name_parts(components->processor->name_pool, name, &parts) !=
        SXML_STATUS_OK) {
        return false;
    }
    const wildcard_name test = {.name = name,
                                .namespace_uri = parts.namespace_uri,
                                .symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION};
    const transition_choice choice = choose_transition(components, &walk->restricted, &test);
    return choice.transition != NULL &&
           choice.transition->term_kind == SXML_SCHEMA_TERM_ELEMENT_DECLARATION;
}

/* Whether a base wildcard's process contents bind at least as loosely
   as the restriction's (3.4.6.4, default bindings: skip subsumes all,
   lax subsumes lax and strict). */
static bool strength_subsumes(binding_strength general, binding_strength specific) {
    return general == SXML_BINDING_SKIP ||
           (general == SXML_BINDING_LAX && specific != SXML_BINDING_SKIP) ||
           (general == SXML_BINDING_STRICT && specific == SXML_BINDING_STRICT);
}

/* The wildcards of the base's transitions from its current
   configuration. */
typedef struct base_wildcards {
    const content_model_transition *transitions;
    uint32_t transition_count;
    const restriction_walk *walk;
} base_wildcards;

static bool is_live_wildcard(base_wildcards wildcards, uint32_t entry) {
    const content_model_transition *transition = &wildcards.transitions[entry];
    return transition->term_kind == SXML_SCHEMA_TERM_WILDCARD &&
           cursor_tests_hold(&wildcards.walk->base, transition->tests);
}

/* Whether some base wildcard allows a namespace. */
static bool some_wildcard_allows_namespace(base_wildcards wildcards, uint32_t namespace_index) {
    const sxml_schema_components *components = wildcards.walk->compiler->components;
    for (uint32_t entry = 0; entry < wildcards.transition_count; entry += 1) {
        if (!is_live_wildcard(wildcards, entry)) {
            continue;
        }
        const sxml_schema_wildcard_index wildcard = {wildcards.transitions[entry].term};
        if (namespace_constraint_allows(components, sxml_schema_get_wildcard(components, wildcard),
                                        namespace_index)) {
            return true;
        }
    }
    return false;
}

/* Whether the union of the base wildcards covers the restriction's
   wildcard where no one of them does (3.4.6.4: the base is
   deterministic, 3.8.6.4, so each name goes to the one wildcard that
   allows it). The namespaces: an enumeration's each by some wildcard;
   an infinite set (any, not) only within a union with a not, whose
   finitely many uncovered namespaces it does not allow. The names: a
   name one base wildcard disallows, another allows. A keyword
   (##defined, ##definedSibling) of a base wildcard the restriction
   lacks is not covered. */
static bool union_covers(base_wildcards wildcards, sxml_schema_wildcard_index restricted) {
    const sxml_schema_components *components = wildcards.walk->compiler->components;
    const sxml_schema_wildcard *specific = sxml_schema_get_wildcard(components, restricted);
    const sxml_schema_index_view specific_namespaces =
        sxml_schema_get_list(components, specific->namespaces);
    const sxml_schema_wildcard *first_not = NULL;
    uint32_t live_count = 0;
    for (uint32_t entry = 0; entry < wildcards.transition_count; entry += 1) {
        if (!is_live_wildcard(wildcards, entry)) {
            continue;
        }
        live_count += 1;
        const sxml_schema_wildcard *general = sxml_schema_get_wildcard(
            components, (sxml_schema_wildcard_index){wildcards.transitions[entry].term});
        if ((general->disallows_defined && !specific->disallows_defined) ||
            (general->disallows_sibling && !specific->disallows_sibling)) {
            return false;
        }
        if (general->namespace_constraint_variety == SXML_SCHEMA_NAMESPACE_CONSTRAINT_NOT &&
            first_not == NULL) {
            first_not = general;
        }
        const sxml_schema_index_view disallowed =
            sxml_schema_get_list(components, general->disallowed_names);
        for (size_t name_entry = 0; name_entry < disallowed.index_count; name_entry += 1) {
            const sxml_name_identifier name_identifier = {.index = disallowed.indexes[name_entry]};
            bool is_covered = !sxml_schema_wildcard_allows_name(
                components,
                &(sxml_schema_wildcard_test){
                    .wildcard = restricted,
                    .name = name_identifier,
                    .symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION});
            for (uint32_t other = 0; !is_covered && other < wildcards.transition_count;
                 other += 1) {
                is_covered = other != entry && is_live_wildcard(wildcards, other) &&
                             sxml_schema_wildcard_allows_name(
                                 components,
                                 &(sxml_schema_wildcard_test){
                                     .wildcard = {wildcards.transitions[other].term},
                                     .name = name_identifier,
                                     .symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION});
            }
            if (!is_covered) {
                return false;
            }
        }
    }
    if (live_count < 2U) {
        return false;
    }
    if (specific->namespace_constraint_variety == SXML_SCHEMA_NAMESPACE_CONSTRAINT_ENUMERATION) {
        for (size_t entry = 0; entry < specific_namespaces.index_count; entry += 1) {
            if (!some_wildcard_allows_namespace(wildcards, specific_namespaces.indexes[entry])) {
                return false;
            }
        }
        return true;
    }
    if (first_not == NULL) {
        return false;
    }
    /* The namespaces no base wildcard allows are among first_not's
       excluded ones. */
    const sxml_schema_index_view excluded = sxml_schema_get_list(components, first_not->namespaces);
    for (size_t entry = 0; entry < excluded.index_count; entry += 1) {
        if (!some_wildcard_allows_namespace(wildcards, excluded.indexes[entry]) &&
            namespace_constraint_allows(components, specific, excluded.indexes[entry])) {
            return false;
        }
    }
    return true;
}

/* A restricting wildcard the union of the base's wildcards covers: one
   configuration per base wildcard it shares a namespace with. */
static sxml_status add_union_configurations(restriction_walk *walk, base_wildcards wildcards,
                                            sxml_schema_wildcard_index restricted) {
    const sxml_schema_components *components = walk->compiler->components;
    const binding_strength strength = wildcard_strength(components, restricted);
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t entry = 0;
         status == SXML_STATUS_OK && !walk->is_violated && entry < wildcards.transition_count;
         entry += 1) {
        const content_model_transition *transition = &wildcards.transitions[entry];
        if (!is_live_wildcard(wildcards, entry) ||
            !wildcards_overlap(components,
                               (wildcard_pair){.first_wildcard = restricted,
                                               .second_wildcard = {transition->term}})) {
            continue;
        }
        if (!strength_subsumes(wildcard_strength(components, (sxml_schema_wildcard_index){
                                                                 transition->term}),
                               strength)) {
            walk->is_violated = true;
            break;
        }
        copy_cursor(&walk->base_next, &walk->base);
        cursor_apply_operations(&walk->base_next, transition->operations);
        walk->base_next.state = transition->target_state;
        status = add_configuration(walk);
    }
    return status;
}

/* A wildcard move of the restriction (restricted_next set): every name
   the base takes by an element transition is checked by name, the rest
   needs a base wildcard (or open content) that covers the wildcard, or
   base wildcards whose union covers it. */
static sxml_status check_wildcard_move(restriction_walk *walk,
                                       sxml_schema_wildcard_index wildcard) {
    const sxml_schema_components *components = walk->compiler->components;
    const sxml_schema_content_model *base = walk->base_model;
    const binding_strength strength = wildcard_strength(components, wildcard);
    const content_model_state *state = &base->states[walk->base.state];
    sxml_status status = SXML_STATUS_OK;
    bool is_covered = false;
    sxml_schema_wildcard_index covering = {.value = 0};
    for (uint32_t entry = 0;
         status == SXML_STATUS_OK && !walk->is_violated && entry < state->transitions.index_count;
         entry += 1) {
        const content_model_transition *transition =
            &base->transitions[state->transitions.first_index + entry];
        if (!cursor_tests_hold(&walk->base, transition->tests)) {
            continue;
        }
        if (transition->term_kind == SXML_SCHEMA_TERM_WILDCARD) {
            if (!is_covered &&
                sxml_schema_wildcard_is_subset(
                    components, &(sxml_schema_wildcard_subset_request){
                                    .subset = wildcard,
                                    .superset = {transition->term},
                                    .symbol_space =
                                        SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION})) {
                is_covered = true;
                covering.value = transition->term;
            }
            continue;
        }
        for (uint32_t member = 0; status == SXML_STATUS_OK && !walk->is_violated &&
                                  member < transition->members.index_count;
             member += 1) {
            const sxml_name_identifier name =
                sxml_schema_get_element_declaration(
                    components,
                    (sxml_schema_element_declaration_index){
                        .value = base->members[transition->members.first_index + member]})
                    ->element_name;
            if (!sxml_schema_wildcard_allows_name(
                    components,
                    &(sxml_schema_wildcard_test){
                        .wildcard = wildcard,
                        .name = name,
                        .symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION}) ||
                restriction_takes_by_element(walk, name)) {
                continue;
            }
            const sxml_schema_element_declaration_index specific =
                strength == SXML_BINDING_SKIP ? (sxml_schema_element_declaration_index){0}
                                              : global_declaration(components, name);
            if (specific.value == 0 && strength == SXML_BINDING_STRICT) {
                /* Not valid against the restriction: nothing to check. */
                continue;
            }
            status = check_element_move(
                walk, (element_move){.name = name, .declaration = specific, .strength = strength});
        }
    }
    if (status != SXML_STATUS_OK || walk->is_violated) {
        return status;
    }
    copy_cursor(&walk->base_next, &walk->base);
    if (is_covered) {
        const content_model_state *base_state = &base->states[walk->base.state];
        for (uint32_t entry = 0; entry < base_state->transitions.index_count; entry += 1) {
            const content_model_transition *transition =
                &base->transitions[base_state->transitions.first_index + entry];
            if (transition->term_kind == SXML_SCHEMA_TERM_WILDCARD &&
                transition->term == covering.value &&
                cursor_tests_hold(&walk->base, transition->tests)) {
                cursor_apply_operations(&walk->base_next, transition->operations);
                walk->base_next.state = transition->target_state;
                break;
            }
        }
    } else if (base->open_content_mode != SXML_SCHEMA_OPEN_CONTENT_NONE &&
               sxml_schema_wildcard_is_subset(
                   components,
                   &(sxml_schema_wildcard_subset_request){
                       .subset = wildcard,
                       .superset = base->open_content_wildcard,
                       .symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ELEMENT_DECLARATION}) &&
               (base->open_content_mode == SXML_SCHEMA_OPEN_CONTENT_INTERLEAVE ||
                walk->base.state == base->suffix_state ||
                sxml_schema_content_model_can_end(&walk->base))) {
        covering = base->open_content_wildcard;
        if (base->open_content_mode == SXML_SCHEMA_OPEN_CONTENT_SUFFIX) {
            walk->base_next.state = base->suffix_state;
        }
    } else {
        const content_model_state *base_state = &base->states[walk->base.state];
        const base_wildcards wildcards = {
            .transitions = &base->transitions[base_state->transitions.first_index],
            .transition_count = base_state->transitions.index_count,
            .walk = walk};
        if (union_covers(wildcards, wildcard)) {
            return add_union_configurations(walk, wildcards, wildcard);
        }
        walk->is_violated = true;
        return SXML_STATUS_OK;
    }
    if (!strength_subsumes(wildcard_strength(components, covering), strength)) {
        walk->is_violated = true;
        return SXML_STATUS_OK;
    }
    return add_configuration(walk);
}

/* Every move of the restriction from the current configuration. */
static sxml_status walk_configuration(restriction_walk *walk) {
    const sxml_schema_content_model *restricted = walk->restricted_model;
    if (sxml_schema_content_model_can_end(&walk->restricted) &&
        !sxml_schema_content_model_can_end(&walk->base)) {
        walk->is_violated = true;
        return SXML_STATUS_OK;
    }
    const content_model_state *state = &restricted->states[walk->restricted.state];
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t entry = 0;
         status == SXML_STATUS_OK && !walk->is_violated && entry < state->transitions.index_count;
         entry += 1) {
        const content_model_transition *transition =
            &restricted->transitions[state->transitions.first_index + entry];
        if (!cursor_tests_hold(&walk->restricted, transition->tests)) {
            continue;
        }
        copy_cursor(&walk->restricted_next, &walk->restricted);
        cursor_apply_operations(&walk->restricted_next, transition->operations);
        walk->restricted_next.state = transition->target_state;
        if (transition->term_kind == SXML_SCHEMA_TERM_WILDCARD) {
            status = check_wildcard_move(walk, (sxml_schema_wildcard_index){transition->term});
            continue;
        }
        for (uint32_t member = 0; status == SXML_STATUS_OK && !walk->is_violated &&
                                  member < transition->members.index_count;
             member += 1) {
            const uint32_t declaration =
                restricted->members[transition->members.first_index + member];
            status = check_element_move(
                walk,
                (element_move){.name = sxml_schema_get_element_declaration(
                                           walk->compiler->components,
                                           (sxml_schema_element_declaration_index){declaration})
                                           ->element_name,
                               .declaration = {declaration}});
        }
    }
    const bool takes_open_content =
        restricted->open_content_mode == SXML_SCHEMA_OPEN_CONTENT_INTERLEAVE ||
        (restricted->open_content_mode == SXML_SCHEMA_OPEN_CONTENT_SUFFIX &&
         (walk->restricted.state == restricted->suffix_state ||
          sxml_schema_content_model_can_end(&walk->restricted)));
    if (status == SXML_STATUS_OK && !walk->is_violated && takes_open_content) {
        copy_cursor(&walk->restricted_next, &walk->restricted);
        if (restricted->open_content_mode == SXML_SCHEMA_OPEN_CONTENT_SUFFIX) {
            walk->restricted_next.state = restricted->suffix_state;
        }
        status = check_wildcard_move(walk, restricted->open_content_wildcard);
    }
    return status;
}

/* Content type restricts (Complex Content), 3.4.6.4, clauses 1 and 2:
   every sequence the restriction accepts the base accepts, with default
   bindings the base's subsume, by a walk over pairs of configurations. */
static sxml_status check_content_restriction(content_compiler *compiler) {
    const sxml_schema_components *components = compiler->components;
    const sxml_schema_type_definition *type_definition =
        sxml_schema_get_type_definition(components, compiler->complex_type);
    const sxml_schema_type_definition *base_type =
        sxml_schema_get_type_definition(components, type_definition->base_type);
    if (type_definition->derivation_method != SXML_SCHEMA_DERIVATION_RESTRICTION ||
        type_definition->base_type.value == (uint32_t)SXML_TYPE_XS_ANY_TYPE ||
        base_type->variety != SXML_TYPE_VARIETY_COMPLEX || base_type->content_model == NULL ||
        type_definition->content_model == NULL) {
        return SXML_STATUS_OK;
    }
    restriction_walk walk = {.compiler = compiler,
                             .restricted_model = type_definition->content_model,
                             .base_model = base_type->content_model};
    const uint32_t restricted_counters = walk.restricted_model->counter_count;
    const uint32_t base_counters = walk.base_model->counter_count;
    compiler->configuration_width = 2 + restricted_counters + base_counters;
    compiler->keys.entry_count = 0;
    if (compiler->set_slots.slots != NULL) {
        memset(compiler->set_slots.slots, 0, compiler->set_slots.slot_count * sizeof(uint32_t));
    }
    compiler->set_slots.used_count = 0;
    void *memory = NULL;
    const size_t value_count = 2 * ((size_t)restricted_counters + base_counters) + 1;
    sxml_status status = sxml_allocator_allocate(
        compiler->allocator,
        (sxml_allocation_request){.byte_count = value_count * sizeof(uint64_t),
                                  .alignment = alignof(uint64_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    walk.values = memory;
    walk.restricted = (sxml_schema_content_model_cursor){.content_model = walk.restricted_model,
                                                         .counter_values = walk.values,
                                                         .counter_count = restricted_counters};
    walk.restricted_next = walk.restricted;
    walk.restricted_next.counter_values = walk.values + restricted_counters;
    walk.base = (sxml_schema_content_model_cursor){
        .content_model = walk.base_model,
        .counter_values = walk.values + 2 * (size_t)restricted_counters,
        .counter_count = base_counters};
    walk.base_next = walk.base;
    walk.base_next.counter_values = walk.base.counter_values + base_counters;
    sxml_schema_content_model_start(&walk.restricted_next);
    sxml_schema_content_model_start(&walk.base_next);
    status = add_configuration(&walk);
    for (size_t row_index = 0;
         status == SXML_STATUS_OK && !walk.is_violated &&
         row_index < compiler->keys.entry_count / compiler->configuration_width;
         row_index += 1) {
        load_configuration(&walk, row_index);
        status = walk_configuration(&walk);
    }
    sxml_allocator_release(compiler->allocator, memory);
    if (status == SXML_STATUS_OK && walk.is_violated) {
        status = report_error(compiler,
                              (content_report){.constraint_name = "cos-content-act-restrict",
                                               .description = "The content of a restriction "
                                                              "allows what its base's does not"});
    }
    return status;
}

/* ------------------------------------------------------------ Text form */

typedef sxml_text_form_writer text_writer;

static void write_bytes(text_writer *writer, sxml_byte_view bytes) {
    sxml_text_form_write_bytes(writer, bytes);
}

static void write_number(text_writer *writer, uint64_t number) {
    sxml_text_form_write_unsigned(writer, number);
}

/* A name with the first prefix the bindings give its namespace. */
static void write_name(const sxml_schema_components *components, text_writer *writer,
                       sxml_name_identifier name) {
    const sxml_name_pool *name_pool = components->processor->name_pool;
    sxml_name_parts parts = {.namespace_uri = {.index = 0}};
    if (sxml_name_pool_get_name_parts(name_pool, name, &parts) != SXML_STATUS_OK) {
        write_bytes(writer, sxml_byte_view_from_c_string("?"));
        return;
    }
    const sxml_schema_table_storage *bindings =
        &components->tables[SXML_SCHEMA_TABLE_NAMESPACE_BINDING];
    const sxml_namespace_binding *binding_entries = bindings->entries;
    bool is_bound = parts.namespace_uri.index == 0;
    for (uint32_t binding = 1; !is_bound && binding < bindings->entry_count; binding += 1) {
        if (binding_entries[binding].namespace_uri.index != parts.namespace_uri.index) {
            continue;
        }
        sxml_byte_view prefix = {.bytes = NULL, .byte_count = 0};
        if (binding_entries[binding].namespace_prefix.index != 0 &&
            sxml_name_pool_get_namespace_prefix(
                name_pool, binding_entries[binding].namespace_prefix, &prefix) == SXML_STATUS_OK) {
            write_bytes(writer, prefix);
            write_bytes(writer, sxml_byte_view_from_c_string(":"));
        }
        is_bound = true;
    }
    sxml_byte_view namespace_uri = {.bytes = NULL, .byte_count = 0};
    if (!is_bound && sxml_name_pool_get_namespace_uri(name_pool, parts.namespace_uri,
                                                      &namespace_uri) == SXML_STATUS_OK) {
        write_bytes(writer, sxml_byte_view_from_c_string("Q{"));
        write_bytes(writer, namespace_uri);
        write_bytes(writer, sxml_byte_view_from_c_string("}"));
    }
    write_bytes(writer, parts.local_name);
}

static void write_tests(text_writer *writer, const sxml_schema_content_model *model,
                        index_run tests) {
    for (uint32_t entry = 0; entry < tests.index_count; entry += 1) {
        const counter_test *test = &model->tests[tests.first_index + entry];
        write_bytes(writer, sxml_byte_view_from_c_string(entry == 0 ? "counter " : ", counter "));
        write_number(writer, (uint64_t)test->counter + 1);
        if (test->highest_value == UINT64_MAX) {
            write_bytes(writer, sxml_byte_view_from_c_string(" >= "));
            write_number(writer, test->lowest_value);
        } else if (test->lowest_value == 0) {
            write_bytes(writer, sxml_byte_view_from_c_string(" < "));
            write_number(writer, test->highest_value + 1);
        } else {
            write_bytes(writer, sxml_byte_view_from_c_string(" in "));
            write_number(writer, test->lowest_value);
            write_bytes(writer, sxml_byte_view_from_c_string(".."));
            write_number(writer, test->highest_value);
        }
    }
}

static void write_transition(const sxml_schema_components *components, text_writer *writer,
                             const content_model_transition *transition) {
    if (transition->term_kind == SXML_SCHEMA_TERM_ELEMENT_DECLARATION) {
        write_name(components, writer,
                   sxml_schema_get_element_declaration(
                       components, (sxml_schema_element_declaration_index){transition->term})
                       ->element_name);
        write_bytes(writer, sxml_byte_view_from_c_string(" element "));
    } else {
        write_bytes(writer, sxml_byte_view_from_c_string("wildcard "));
    }
    write_number(writer, transition->term);
    write_bytes(writer, sxml_byte_view_from_c_string(" -> "));
    write_number(writer, transition->target_state);
}

sxml_status
sxml_schema_content_model_write_text(const sxml_schema_components *components,
                                     const sxml_schema_content_model_text_request *request,
                                     size_t *byte_count) {
    const sxml_schema_content_model *model = request->content_model;
    text_writer writer = {.output = request->output, .byte_count = 0, .column = 0};
    write_bytes(&writer, sxml_byte_view_from_c_string("content-model of type "));
    write_number(&writer, model->complex_type.value);
    write_bytes(&writer,
                sxml_byte_view_from_c_string(model->is_all_group ? " all states " : " states "));
    write_number(&writer, model->state_count);
    write_bytes(&writer, sxml_byte_view_from_c_string(" counters "));
    write_number(&writer, model->counter_count);
    write_bytes(&writer, sxml_byte_view_from_c_string("\n"));
    for (uint32_t counter = 0; counter < model->counter_count; counter += 1) {
        write_bytes(&writer, sxml_byte_view_from_c_string("  counter "));
        write_number(&writer, (uint64_t)counter + 1);
        write_bytes(&writer, sxml_byte_view_from_c_string(" "));
        write_number(&writer, model->counters[counter].minimum_occurrences);
        write_bytes(&writer, sxml_byte_view_from_c_string(".."));
        if (model->counters[counter].maximum_occurrences == SXML_SCHEMA_UNBOUNDED) {
            write_bytes(&writer, sxml_byte_view_from_c_string("unbounded"));
        } else {
            write_number(&writer, model->counters[counter].maximum_occurrences);
        }
        write_bytes(&writer, sxml_byte_view_from_c_string("\n"));
    }
    for (uint32_t state_index = 1; state_index <= model->state_count; state_index += 1) {
        const content_model_state *state = &model->states[state_index];
        write_bytes(&writer, sxml_byte_view_from_c_string("  state "));
        write_number(&writer, state_index);
        if (state->is_final) {
            write_bytes(&writer, sxml_byte_view_from_c_string(" final"));
            if (state->final_tests.index_count != 0) {
                write_bytes(&writer, sxml_byte_view_from_c_string(" if "));
                write_tests(&writer, model, state->final_tests);
            }
        }
        if (state_index == model->suffix_state) {
            write_bytes(&writer, sxml_byte_view_from_c_string(" suffix"));
        }
        for (uint32_t entry = 0; entry < state->transitions.index_count; entry += 1) {
            const content_model_transition *transition =
                &model->transitions[state->transitions.first_index + entry];
            write_bytes(&writer, sxml_byte_view_from_c_string(entry == 0 ? ": " : ", "));
            write_transition(components, &writer, transition);
            if (transition->tests.index_count + transition->operations.index_count != 0) {
                write_bytes(&writer, sxml_byte_view_from_c_string(" ["));
                write_tests(&writer, model, transition->tests);
                for (uint32_t operation = 0; operation < transition->operations.index_count;
                     operation += 1) {
                    const counter_operation *entry_operation =
                        &model->operations[transition->operations.first_index + operation];
                    write_bytes(&writer, sxml_byte_view_from_c_string(
                                             operation == 0 && transition->tests.index_count == 0
                                                 ? "counter "
                                                 : ", counter "));
                    write_number(&writer, (uint64_t)entry_operation->counter + 1);
                    write_bytes(&writer, sxml_byte_view_from_c_string(
                                             entry_operation->operation_kind ==
                                                     SXML_COUNTER_OPERATION_SET_TO_ONE
                                                 ? " = 1"
                                                 : " + 1"));
                }
                write_bytes(&writer, sxml_byte_view_from_c_string("]"));
            }
        }
        write_bytes(&writer, sxml_byte_view_from_c_string("\n"));
    }
    if (model->open_content_mode != SXML_SCHEMA_OPEN_CONTENT_NONE) {
        write_bytes(&writer, sxml_byte_view_from_c_string(
                                 model->open_content_mode == SXML_SCHEMA_OPEN_CONTENT_SUFFIX
                                     ? "  open-content suffix wildcard "
                                     : "  open-content interleave wildcard "));
        write_number(&writer, model->open_content_wildcard.value);
        if (model->suffix_state != 0) {
            write_bytes(&writer, sxml_byte_view_from_c_string(" -> "));
            write_number(&writer, model->suffix_state);
        }
        write_bytes(&writer, sxml_byte_view_from_c_string("\n"));
    }
    *byte_count = writer.byte_count;
    return writer.byte_count <= request->output.byte_count ? SXML_STATUS_OK
                                                           : SXML_STATUS_CAPACITY_EXCEEDED;
}
