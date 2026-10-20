/*
 * Compile stage RESOLVE: schema references to components, and the
 * properties that need only that (src/schema_internal.h, RESOLVE;
 * docs/DECISIONS.md D167, D170).
 *
 * Golden file: src/xquery_module.c (names resolved after every module is
 * read; work lists instead of recursion).
 *
 * Specification: XSD 1.1 Part 1, 3.17.6.2 "QName resolution (Schema
 * Document)" (src-resolve), 3.3.2.1 (the type definition of an element
 * declaration without one: its first substitution group head's, else
 * xs:anyType), 3.2.2.1 (an attribute declaration's: xs:anySimpleType),
 * 3.3.6.2 (substitution groups), 3.16.2 (the base, item and member types
 * of a simple type), 3.8.6.1 mg-props-correct.2, 3.6.3
 * src-attribute_group.3, 3.4.6.1 ct-props-correct.3, 3.16.6.1
 * st-props-correct.2 and 3.3.6.1 e-props-correct.6 (no circular
 * definitions).
 *
 * Every walk over the components' graph (base chains, substitution
 * groups, nested groups) is a loop with an explicit stack and a mark per
 * component (D71).
 */
#include "schema_internal.h"

#include "evaluator_internal.h"
#include "processor_internal.h"
#include "xml_internal.h"

#include <stdalign.h>
#include <string.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* The first type definition of a schema's own (the built-in ones and the
   type of xsi:schemaLocation come first, src/schema_internal.h). */
#define FIRST_OWN_TYPE_DEFINITION 56U

typedef struct resolver {
    sxml_schema_compilation *compilation;
    sxml_schema_components *components;
    sxml_name_pool *name_pool;
    sxml_allocator allocator;
    /* uint32_t work: a stack, marks, pairs. */
    uint32_t *stack;
    size_t stack_count;
    size_t stack_capacity;
    uint32_t *marks;
    size_t mark_capacity;
    uint32_t *pairs;
    size_t pair_count;
    size_t pair_capacity;
} resolver;

static uint32_t table_count(const resolver *state, sxml_schema_table table) {
    return state->components->tables[table].entry_count;
}

static void *edit(resolver *state, sxml_schema_table table, uint32_t index) {
    return sxml_schema_components_edit_entry(
        state->components, (sxml_schema_entry_key){.table = table, .index = index});
}

static uint32_t component_of(const resolver *state, sxml_schema_reference_index reference) {
    return sxml_schema_get_reference(state->components, reference)->component;
}

/* An array of uint32_t grown to hold required_count. */
typedef struct index_array {
    uint32_t **indexes;
    size_t *capacity;
    size_t count;
} index_array;

static sxml_status reserve_indexes(resolver *state, index_array array, size_t required_count) {
    void *elements = *array.indexes;
    sxml_status status = sxml_evaluator_grow(
        state->allocator, &(sxml_evaluator_growth){.elements = &elements,
                                                   .element_capacity = array.capacity,
                                                   .element_count = array.count,
                                                   .required_count = required_count,
                                                   .element_byte_count = sizeof(uint32_t),
                                                   .alignment = alignof(uint32_t)});
    *array.indexes = elements;
    return status;
}

static sxml_status push(resolver *state, uint32_t index) {
    sxml_status status = reserve_indexes(state,
                                         (index_array){.indexes = &state->stack,
                                                       .capacity = &state->stack_capacity,
                                                       .count = state->stack_count},
                                         state->stack_count + 1U);
    if (status == SXML_STATUS_OK) {
        state->stack[state->stack_count] = index;
        state->stack_count += 1;
    }
    return status;
}

/* Clears the marks for a table of entry_count entries. */
static sxml_status clear_marks(resolver *state, uint32_t entry_count) {
    sxml_status status = reserve_indexes(
        state,
        (index_array){.indexes = &state->marks, .capacity = &state->mark_capacity, .count = 0},
        entry_count == 0 ? 1U : entry_count);
    if (status == SXML_STATUS_OK && entry_count != 0) {
        memset(state->marks, 0, (size_t)entry_count * sizeof(uint32_t));
    }
    return status;
}

/* ------------------------------------------------------------ Reports */

typedef struct resolve_error {
    const char *constraint_name;
    const char *description;
    /* Shown after the description: a component's name. */
    sxml_name_identifier name;
    sxml_schema_position position;
} resolve_error;

static sxml_status report(resolver *state, resolve_error error) {
    return sxml_schema_compilation_report(
        state->compilation,
        &(sxml_schema_report_request){
            .constraint_name = sxml_byte_view_from_c_string(error.constraint_name),
            .description = sxml_byte_view_from_c_string(error.description),
            .subject_name = error.name,
            .position = error.position});
}

/* ---------------------------------------------------------- References */

/* src-resolve: every reference names a component of its symbol space
   (Part 1, 3.17.6.2, clause 3). A redefinition's reference to the
   original is resolved already. */
static sxml_status resolve_all_references(resolver *state) {
    const uint32_t count = table_count(state, SXML_SCHEMA_TABLE_REFERENCE);
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t index = 1; status == SXML_STATUS_OK && index < count; index += 1) {
        sxml_schema_reference *reference = edit(state, SXML_SCHEMA_TABLE_REFERENCE, index);
        if (reference == NULL || reference->component != 0) {
            continue;
        }
        uint32_t component = 0;
        if (sxml_schema_components_find_name(
                state->components,
                (sxml_schema_name_key){.symbol_space = reference->symbol_space,
                                       .name = reference->name},
                &component)) {
            reference->component = component;
            continue;
        }
        const resolve_error error = {.constraint_name = "src-resolve",
                                     .description = "A QName value names no component of its "
                                                    "kind",
                                     .name = reference->name,
                                     .position = reference->position};
        status = report(state, error);
    }
    return status;
}

/* ---------------------------------------------------- Type definitions */

/* Writes base, item and member types (Part 1, 3.4.2, 3.16.2). */
static sxml_status resolve_type_definitions(resolver *state) {
    const uint32_t count = table_count(state, SXML_SCHEMA_TABLE_TYPE_DEFINITION);
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t index = FIRST_OWN_TYPE_DEFINITION; status == SXML_STATUS_OK && index < count;
         index += 1) {
        sxml_schema_type_definition *definition =
            edit(state, SXML_SCHEMA_TABLE_TYPE_DEFINITION, index);
        if (definition == NULL || definition->is_builtin) {
            continue;
        }
        definition->default_attribute_group.value =
            component_of(state, definition->default_attribute_group_reference);
        if (definition->variety == SXML_TYPE_VARIETY_COMPLEX) {
            /* 3.4.2.3.3: without simpleContent or complexContent, a
               restriction of xs:anyType. */
            definition->base_type.value = definition->base_type_reference.value != 0
                                              ? component_of(state, definition->base_type_reference)
                                              : (uint32_t)SXML_TYPE_XS_ANY_TYPE;
            continue;
        }
        if (definition->derivation_method == SXML_SCHEMA_DERIVATION_RESTRICTION) {
            definition->base_type.value = definition->base_type_reference.value != 0
                                              ? component_of(state, definition->base_type_reference)
                                              : definition->anonymous_base_type.value;
            continue;
        }
        definition->base_type.value = (uint32_t)SXML_TYPE_XS_ANY_SIMPLE_TYPE;
        if (definition->derivation_method == SXML_SCHEMA_DERIVATION_LIST) {
            definition->item_type.value = definition->item_type_reference.value != 0
                                              ? component_of(state, definition->item_type_reference)
                                              : definition->anonymous_item_type.value;
            continue;
        }
        /* A union's member types: the referenced ones, then the anonymous
           ones (3.16.2.3). */
        const sxml_schema_index_view references =
            sxml_schema_get_list(state->components, definition->member_type_references);
        const sxml_schema_index_view anonymous =
            sxml_schema_get_list(state->components, definition->anonymous_member_types);
        state->pair_count = 0;
        status = reserve_indexes(
            state,
            (index_array){.indexes = &state->pairs, .capacity = &state->pair_capacity, .count = 0},
            references.index_count + anonymous.index_count + 1U);
        for (size_t member = 0; status == SXML_STATUS_OK && member < references.index_count;
             member += 1) {
            const uint32_t type = component_of(
                state, (sxml_schema_reference_index){.value = references.indexes[member]});
            if (type != 0) {
                state->pairs[state->pair_count] = type;
                state->pair_count += 1;
            }
        }
        for (size_t member = 0; status == SXML_STATUS_OK && member < anonymous.index_count;
             member += 1) {
            state->pairs[state->pair_count] = anonymous.indexes[member];
            state->pair_count += 1;
        }
        sxml_schema_list members = {.first_entry_index = 0, .entry_count = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_schema_components_add_list(
                state->components,
                (sxml_schema_index_view){.indexes = state->pairs, .index_count = state->pair_count},
                &members);
        }
        definition = edit(state, SXML_SCHEMA_TABLE_TYPE_DEFINITION, index);
        if (status == SXML_STATUS_OK && definition != NULL) {
            definition->member_types = members;
        }
    }
    return status;
}

/* The marks of a walk over components: not seen, on the stack, done. */
enum { SXML_MARK_NEW = 0, SXML_MARK_ACTIVE = 1, SXML_MARK_DONE = 2 };

/* The types a type definition derives from directly: its base (not for
   xs:anyType, its own base) and, for simple types, item and member types.
   Writes at most capacity of them. */
typedef struct dependency_list {
    uint32_t types[8];
    size_t type_count;
    /* A union's members beyond the first eight: the list. */
    sxml_schema_index_view more_members;
} dependency_list;

static dependency_list dependencies_of(const resolver *state, uint32_t type) {
    dependency_list list = {.type_count = 0, .more_members = {.indexes = NULL, .index_count = 0}};
    const sxml_schema_type_definition *definition = sxml_schema_get_type_definition(
        state->components, (sxml_schema_type_definition_index){.value = type});
    if (definition->is_builtin) {
        return list;
    }
    if (definition->base_type.value != 0) {
        list.types[list.type_count] = definition->base_type.value;
        list.type_count += 1;
    }
    if (definition->item_type.value != 0) {
        list.types[list.type_count] = definition->item_type.value;
        list.type_count += 1;
    }
    list.more_members = sxml_schema_get_list(state->components, definition->member_types);
    return list;
}

/* ct-props-correct.3 and st-props-correct.2: no type definition derives
   from itself, through bases, item types or member types. A depth-first
   walk: on the stack, a type and the position of its next dependency. */
static sxml_status check_type_cycles(resolver *state) {
    const uint32_t count = table_count(state, SXML_SCHEMA_TABLE_TYPE_DEFINITION);
    sxml_status status = clear_marks(state, count);
    for (uint32_t root_type = FIRST_OWN_TYPE_DEFINITION;
         status == SXML_STATUS_OK && root_type < count; root_type += 1) {
        if (state->marks[root_type] != SXML_MARK_NEW) {
            continue;
        }
        state->stack_count = 0;
        status = push(state, root_type);
        if (status == SXML_STATUS_OK) {
            status = push(state, 0);
        }
        state->marks[root_type] = SXML_MARK_ACTIVE;
        while (status == SXML_STATUS_OK && state->stack_count > 0) {
            const uint32_t type = state->stack[state->stack_count - 2U];
            const uint32_t next = state->stack[state->stack_count - 1U];
            const dependency_list dependencies = dependencies_of(state, type);
            const size_t total = dependencies.type_count + dependencies.more_members.index_count;
            if (next >= total) {
                state->marks[type] = SXML_MARK_DONE;
                state->stack_count -= 2U;
                continue;
            }
            state->stack[state->stack_count - 1U] = next + 1U;
            const uint32_t dependency =
                next < dependencies.type_count
                    ? dependencies.types[next]
                    : dependencies.more_members.indexes[next - dependencies.type_count];
            if (dependency >= count || dependency < FIRST_OWN_TYPE_DEFINITION) {
                continue;
            }
            if (state->marks[dependency] == SXML_MARK_ACTIVE) {
                const sxml_schema_type_definition *definition = sxml_schema_get_type_definition(
                    state->components, (sxml_schema_type_definition_index){.value = dependency});
                status = report(
                    state, (resolve_error){.constraint_name =
                                               definition->variety == SXML_TYPE_VARIETY_COMPLEX
                                                   ? "ct-props-correct.3"
                                                   : "st-props-correct.2",
                                           .description = "A type definition derives from itself",
                                           .name = definition->type_name,
                                           .position = definition->position});
                continue;
            }
            if (state->marks[dependency] == SXML_MARK_NEW) {
                state->marks[dependency] = SXML_MARK_ACTIVE;
                status = push(state, dependency);
                if (status == SXML_STATUS_OK) {
                    status = push(state, 0);
                }
            }
        }
    }
    return status;
}

/* The {variety} of a simple type restriction is its base's (Part 1,
   3.16.2.1): follows each base chain to a list, a union or an atomic
   type. Cycles were reported; they keep the variety read. */
static sxml_status resolve_varieties(resolver *state) {
    const uint32_t count = table_count(state, SXML_SCHEMA_TABLE_TYPE_DEFINITION);
    sxml_status status = clear_marks(state, count);
    for (uint32_t root_type = FIRST_OWN_TYPE_DEFINITION;
         status == SXML_STATUS_OK && root_type < count; root_type += 1) {
        state->stack_count = 0;
        uint32_t type = root_type;
        /* Up the chain of undecided restrictions. */
        while (status == SXML_STATUS_OK && type >= FIRST_OWN_TYPE_DEFINITION && type < count &&
               state->marks[type] == SXML_MARK_NEW) {
            const sxml_schema_type_definition *definition = sxml_schema_get_type_definition(
                state->components, (sxml_schema_type_definition_index){.value = type});
            state->marks[type] = SXML_MARK_ACTIVE;
            if (definition->variety == SXML_TYPE_VARIETY_COMPLEX ||
                definition->derivation_method != SXML_SCHEMA_DERIVATION_RESTRICTION) {
                state->marks[type] = SXML_MARK_DONE;
                break;
            }
            status = push(state, type);
            type = definition->base_type.value;
        }
        const sxml_type_variety base_variety =
            sxml_schema_get_type_definition(state->components,
                                            (sxml_schema_type_definition_index){.value = type})
                ->variety;
        const bool is_decided =
            type != 0 && type < count &&
            (state->marks[type] == SXML_MARK_DONE || type < FIRST_OWN_TYPE_DEFINITION) &&
            (base_variety == SXML_TYPE_VARIETY_ATOMIC || base_variety == SXML_TYPE_VARIETY_LIST ||
             base_variety == SXML_TYPE_VARIETY_UNION);
        while (state->stack_count > 0) {
            const uint32_t restriction = state->stack[state->stack_count - 1U];
            state->stack_count -= 1;
            sxml_schema_type_definition *definition =
                edit(state, SXML_SCHEMA_TABLE_TYPE_DEFINITION, restriction);
            if (definition != NULL && is_decided) {
                definition->variety = base_variety;
            }
            state->marks[restriction] = SXML_MARK_DONE;
        }
    }
    return status;
}

/* ------------------------------------------------------- Declarations */

/* Substitution group affiliations (3.3.2.1), as element declaration
   indexes. */
static sxml_status resolve_affiliations(resolver *state) {
    const uint32_t count = table_count(state, SXML_SCHEMA_TABLE_ELEMENT_DECLARATION);
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t index = 1; status == SXML_STATUS_OK && index < count; index += 1) {
        const sxml_schema_element_declaration *declaration = sxml_schema_get_element_declaration(
            state->components, (sxml_schema_element_declaration_index){.value = index});
        const sxml_schema_index_view references =
            sxml_schema_get_list(state->components, declaration->substitution_group_references);
        if (references.index_count == 0) {
            continue;
        }
        state->pair_count = 0;
        status = reserve_indexes(
            state,
            (index_array){.indexes = &state->pairs, .capacity = &state->pair_capacity, .count = 0},
            references.index_count);
        for (size_t head_position = 0;
             status == SXML_STATUS_OK && head_position < references.index_count;
             head_position += 1) {
            const uint32_t component = component_of(
                state, (sxml_schema_reference_index){.value = references.indexes[head_position]});
            if (component != 0) {
                state->pairs[state->pair_count] = component;
                state->pair_count += 1;
            }
        }
        sxml_schema_list affiliations = {.first_entry_index = 0, .entry_count = 0};
        if (status == SXML_STATUS_OK) {
            status = sxml_schema_components_add_list(
                state->components,
                (sxml_schema_index_view){.indexes = state->pairs, .index_count = state->pair_count},
                &affiliations);
        }
        sxml_schema_element_declaration *entry =
            edit(state, SXML_SCHEMA_TABLE_ELEMENT_DECLARATION, index);
        if (status == SXML_STATUS_OK && entry != NULL) {
            entry->substitution_group_affiliations = affiliations;
        }
    }
    return status;
}

/* The first head of a declaration, or 0. */
static uint32_t first_head(const resolver *state, uint32_t declaration) {
    const sxml_schema_index_view heads = sxml_schema_get_list(
        state->components,
        sxml_schema_get_element_declaration(
            state->components, (sxml_schema_element_declaration_index){.value = declaration})
            ->substitution_group_affiliations);
    return heads.index_count == 0 ? 0 : heads.indexes[0];
}

/* {type definition} of element declarations (3.3.2.1): the type
   attribute's, the anonymous type, the first head's, xs:anyType. A chain
   of heads is walked with a stack; a cycle (reported below) ends in
   xs:anyType. */
static sxml_status resolve_element_types(resolver *state) {
    const uint32_t count = table_count(state, SXML_SCHEMA_TABLE_ELEMENT_DECLARATION);
    sxml_status status = clear_marks(state, count);
    for (uint32_t first_declaration = 1; status == SXML_STATUS_OK && first_declaration < count;
         first_declaration += 1) {
        state->stack_count = 0;
        uint32_t declaration = first_declaration;
        uint32_t type = 0;
        while (status == SXML_STATUS_OK && declaration != 0 && declaration < count) {
            const sxml_schema_element_declaration *entry = sxml_schema_get_element_declaration(
                state->components, (sxml_schema_element_declaration_index){.value = declaration});
            if (state->marks[declaration] == SXML_MARK_DONE) {
                type = entry->type_definition.value;
                break;
            }
            if (state->marks[declaration] == SXML_MARK_ACTIVE) {
                type = (uint32_t)SXML_TYPE_XS_ANY_TYPE;
                break;
            }
            state->marks[declaration] = SXML_MARK_ACTIVE;
            status = push(state, declaration);
            if (entry->type_reference.value != 0) {
                type = component_of(state, entry->type_reference);
                if (type == 0) {
                    type = (uint32_t)SXML_TYPE_XS_ANY_TYPE;
                }
                break;
            }
            if (entry->anonymous_type.value != 0) {
                type = entry->anonymous_type.value;
                break;
            }
            declaration = first_head(state, declaration);
            if (declaration == 0) {
                type = (uint32_t)SXML_TYPE_XS_ANY_TYPE;
            }
        }
        /* The declarations on the stack take the type at the end of the
           chain, the first one its own. */
        while (state->stack_count > 0) {
            const uint32_t chained = state->stack[state->stack_count - 1U];
            state->stack_count -= 1;
            sxml_schema_element_declaration *entry =
                edit(state, SXML_SCHEMA_TABLE_ELEMENT_DECLARATION, chained);
            if (entry != NULL) {
                entry->type_definition.value = type;
            }
            state->marks[chained] = SXML_MARK_DONE;
        }
    }
    return status;
}

/* The members of every head (3.3.6.2): each declaration joins every head
   its affiliations reach, transitively. Pairs (head, member) are gathered
   by member, in index order, then counted into each head's list.
   e-props-correct.6: a declaration that reaches itself. */
static sxml_status resolve_substitution_groups(resolver *state) {
    const uint32_t count = table_count(state, SXML_SCHEMA_TABLE_ELEMENT_DECLARATION);
    sxml_status status = clear_marks(state, count);
    state->pair_count = 0;
    for (uint32_t member = 1; status == SXML_STATUS_OK && member < count; member += 1) {
        if (first_head(state, member) == 0) {
            continue;
        }
        /* Marks hold the member that last reached a declaration. */
        state->stack_count = 0;
        status = push(state, member);
        bool is_circular = false;
        while (status == SXML_STATUS_OK && state->stack_count > 0) {
            const uint32_t declaration = state->stack[state->stack_count - 1U];
            state->stack_count -= 1;
            const sxml_schema_index_view heads = sxml_schema_get_list(
                state->components,
                sxml_schema_get_element_declaration(
                    state->components,
                    (sxml_schema_element_declaration_index){.value = declaration})
                    ->substitution_group_affiliations);
            for (size_t head_index = 0; status == SXML_STATUS_OK && head_index < heads.index_count;
                 head_index += 1) {
                const uint32_t head_declaration = heads.indexes[head_index];
                if (head_declaration == 0 || head_declaration >= count ||
                    state->marks[head_declaration] == member) {
                    continue;
                }
                state->marks[head_declaration] = member;
                if (head_declaration == member) {
                    is_circular = true;
                    continue;
                }
                status = reserve_indexes(state,
                                         (index_array){.indexes = &state->pairs,
                                                       .capacity = &state->pair_capacity,
                                                       .count = state->pair_count},
                                         state->pair_count + 2U);
                if (status == SXML_STATUS_OK) {
                    state->pairs[state->pair_count] = head_declaration;
                    state->pairs[state->pair_count + 1U] = member;
                    state->pair_count += 2U;
                    status = push(state, head_declaration);
                }
            }
        }
        if (status == SXML_STATUS_OK && is_circular) {
            const sxml_schema_element_declaration *declaration =
                sxml_schema_get_element_declaration(
                    state->components, (sxml_schema_element_declaration_index){.value = member});
            status = report(state, (resolve_error){.constraint_name = "e-props-correct.6",
                                                   .description = "An element declaration is in "
                                                                  "its own substitution group",
                                                   .name = declaration->element_name,
                                                   .position = declaration->position});
        }
    }
    /* Each head_declaration's members, in member order: one pass per head_declaration would be
       quadratic, so the pairs are counted by head_declaration first. */
    uint32_t *member_counts = NULL;
    size_t member_count_capacity = 0;
    if (status == SXML_STATUS_OK && state->pair_count != 0) {
        status = reserve_indexes(state,
                                 (index_array){.indexes = &member_counts,
                                               .capacity = &member_count_capacity,
                                               .count = 0},
                                 (size_t)count * 2U);
    }
    if (status == SXML_STATUS_OK && state->pair_count != 0) {
        memset(member_counts, 0, (size_t)count * 2U * sizeof(uint32_t));
        for (size_t pair_position = 0; pair_position < state->pair_count; pair_position += 2U) {
            member_counts[state->pairs[pair_position]] += 1;
        }
        /* member_counts[count + head_declaration]: the next place of head_declaration's members. */
        uint32_t offset = 0;
        for (uint32_t head_declaration = 0; head_declaration < count; head_declaration += 1) {
            member_counts[count + head_declaration] = offset;
            offset += member_counts[head_declaration];
        }
        status = reserve_indexes(
            state,
            (index_array){.indexes = &state->stack, .capacity = &state->stack_capacity, .count = 0},
            offset + 1U);
        uint32_t *members = state->stack;
        for (size_t pair_position = 0;
             status == SXML_STATUS_OK && pair_position < state->pair_count; pair_position += 2U) {
            const uint32_t head_declaration = state->pairs[pair_position];
            members[member_counts[count + head_declaration]] = state->pairs[pair_position + 1U];
            member_counts[count + head_declaration] += 1;
        }
        for (uint32_t head_declaration = 1; status == SXML_STATUS_OK && head_declaration < count;
             head_declaration += 1) {
            if (member_counts[head_declaration] == 0) {
                continue;
            }
            const uint32_t end = member_counts[count + head_declaration];
            sxml_schema_list list = {.first_entry_index = 0, .entry_count = 0};
            status = sxml_schema_components_add_list(
                state->components,
                (sxml_schema_index_view){.indexes =
                                             members + (end - member_counts[head_declaration]),
                                         .index_count = member_counts[head_declaration]},
                &list);
            sxml_schema_element_declaration *entry =
                edit(state, SXML_SCHEMA_TABLE_ELEMENT_DECLARATION, head_declaration);
            if (status == SXML_STATUS_OK && entry != NULL) {
                entry->substitution_group_members = list;
            }
        }
    }
    sxml_allocator_release(state->allocator, member_counts);
    state->pair_count = 0;
    return status;
}

/* The identity constraints of an element: a ref= one stands for the
   definition it names (XSD 1.1, 3.11.2), which must be of its category
   (src-identity-constraint). */
static sxml_status resolve_identity_constraint_lists(resolver *state) {
    const uint32_t count = table_count(state, SXML_SCHEMA_TABLE_ELEMENT_DECLARATION);
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t index = 1; status == SXML_STATUS_OK && index < count; index += 1) {
        const sxml_schema_list list =
            sxml_schema_get_element_declaration(
                state->components, (sxml_schema_element_declaration_index){.value = index})
                ->identity_constraints;
        for (uint32_t entry_index = 0; status == SXML_STATUS_OK && entry_index < list.entry_count;
             entry_index += 1) {
            uint32_t *entry =
                edit(state, SXML_SCHEMA_TABLE_LIST_ENTRY, list.first_entry_index + entry_index);
            if (entry == NULL) {
                continue;
            }
            const sxml_schema_identity_constraint *constraint = sxml_schema_get_identity_constraint(
                state->components, (sxml_schema_identity_constraint_index){.value = *entry});
            if (constraint->definition_reference.value == 0) {
                continue;
            }
            const uint32_t definition = component_of(state, constraint->definition_reference);
            if (definition == 0) {
                continue;
            }
            const sxml_schema_identity_constraint *referred = sxml_schema_get_identity_constraint(
                state->components, (sxml_schema_identity_constraint_index){.value = definition});
            if (referred->category != constraint->category) {
                status = report(state, (resolve_error){.constraint_name = "src-identity-constraint",
                                                       .description = "An identity constraint's "
                                                                      "ref names one of another "
                                                                      "category",
                                                       .name = referred->constraint_name,
                                                       .position = constraint->position});
            }
            *entry = definition;
        }
    }
    return status;
}

/* Attribute declarations and uses, particles, keyrefs and type
   alternatives (3.2.2.1, 3.5.2, 3.9.2, 3.11.2, 3.12.2). */
static void resolve_other_references(resolver *state) {
    const uint32_t attribute_count = table_count(state, SXML_SCHEMA_TABLE_ATTRIBUTE_DECLARATION);
    for (uint32_t index = 5; index < attribute_count; index += 1) {
        sxml_schema_attribute_declaration *declaration =
            edit(state, SXML_SCHEMA_TABLE_ATTRIBUTE_DECLARATION, index);
        if (declaration == NULL) {
            continue;
        }
        uint32_t type = declaration->anonymous_type.value;
        if (declaration->type_reference.value != 0) {
            type = component_of(state, declaration->type_reference);
        } else if (type == 0) {
            type = (uint32_t)SXML_TYPE_XS_ANY_SIMPLE_TYPE;
        }
        declaration->type_definition.value = type;
    }
    const uint32_t use_count = table_count(state, SXML_SCHEMA_TABLE_ATTRIBUTE_USE);
    for (uint32_t index = 1; index < use_count; index += 1) {
        sxml_schema_attribute_use *use = edit(state, SXML_SCHEMA_TABLE_ATTRIBUTE_USE, index);
        if (use == NULL) {
            continue;
        }
        if (use->attribute_declaration_reference.value == 0) {
            use->attribute_declaration = use->local_attribute_declaration;
            continue;
        }
        use->attribute_declaration.value =
            component_of(state, use->attribute_declaration_reference);
        /* {inheritable}: the reference's inheritable=, else the
           declaration's (3.2.2.2, "the actual value of the inheritable
           [attribute], if present, otherwise ... {inheritable}"). */
        if (!use->has_inheritable) {
            use->is_inheritable = sxml_schema_get_attribute_declaration(
                                      state->components, use->attribute_declaration)
                                      ->is_inheritable;
        }
    }
    const uint32_t particle_count = table_count(state, SXML_SCHEMA_TABLE_PARTICLE);
    for (uint32_t index = 3; index < particle_count; index += 1) {
        sxml_schema_particle *particle = edit(state, SXML_SCHEMA_TABLE_PARTICLE, index);
        if (particle == NULL || particle->term_reference.value == 0) {
            continue;
        }
        const uint32_t component = component_of(state, particle->term_reference);
        particle->term = particle->term_kind == SXML_SCHEMA_TERM_MODEL_GROUP
                             ? sxml_schema_get_model_group_definition(
                                   state->components,
                                   (sxml_schema_model_group_definition_index){.value = component})
                                   ->model_group.value
                             : component;
    }
    const uint32_t constraint_count = table_count(state, SXML_SCHEMA_TABLE_IDENTITY_CONSTRAINT);
    for (uint32_t index = 1; index < constraint_count; index += 1) {
        sxml_schema_identity_constraint *constraint =
            edit(state, SXML_SCHEMA_TABLE_IDENTITY_CONSTRAINT, index);
        if (constraint != NULL) {
            constraint->referenced_key.value =
                component_of(state, constraint->referenced_key_reference);
        }
    }
    const uint32_t alternative_count = table_count(state, SXML_SCHEMA_TABLE_TYPE_ALTERNATIVE);
    for (uint32_t index = 1; index < alternative_count; index += 1) {
        sxml_schema_type_alternative *alternative =
            edit(state, SXML_SCHEMA_TABLE_TYPE_ALTERNATIVE, index);
        if (alternative != NULL) {
            alternative->type_definition.value =
                alternative->type_reference.value != 0
                    ? component_of(state, alternative->type_reference)
                    : alternative->anonymous_type.value;
        }
    }
}

/* ---------------------------------------------------------- Circularity */

/* mg-props-correct.2 (3.8.6.1): no model group definition contains a
   reference to itself, directly or through nested groups and other
   definitions. Walks each definition's groups with a stack; marks hold
   the definition that last visited a model group. */
static sxml_status check_group_cycles(resolver *state) {
    const uint32_t definition_count = table_count(state, SXML_SCHEMA_TABLE_MODEL_GROUP_DEFINITION);
    const uint32_t group_count = table_count(state, SXML_SCHEMA_TABLE_MODEL_GROUP);
    sxml_status status = clear_marks(state, group_count);
    for (uint32_t definition = 1; status == SXML_STATUS_OK && definition < definition_count;
         definition += 1) {
        const sxml_schema_model_group_definition *entry = sxml_schema_get_model_group_definition(
            state->components, (sxml_schema_model_group_definition_index){.value = definition});
        state->stack_count = 0;
        bool is_circular = false;
        if (entry->model_group.value != 0 && entry->model_group.value < group_count) {
            state->marks[entry->model_group.value] = definition;
            status = push(state, entry->model_group.value);
        }
        while (status == SXML_STATUS_OK && state->stack_count > 0 && !is_circular) {
            const uint32_t group = state->stack[state->stack_count - 1U];
            state->stack_count -= 1;
            const sxml_schema_index_view particles = sxml_schema_get_list(
                state->components,
                sxml_schema_get_model_group(state->components,
                                            (sxml_schema_model_group_index){.value = group})
                    ->particles);
            for (size_t particle_index = 0;
                 status == SXML_STATUS_OK && particle_index < particles.index_count;
                 particle_index += 1) {
                const sxml_schema_particle *particle = sxml_schema_get_particle(
                    state->components,
                    (sxml_schema_particle_index){.value = particles.indexes[particle_index]});
                if (particle->term_kind != SXML_SCHEMA_TERM_MODEL_GROUP || particle->term == 0 ||
                    particle->term >= group_count) {
                    continue;
                }
                if (particle->term_reference.value != 0 &&
                    component_of(state, particle->term_reference) == definition) {
                    is_circular = true;
                    break;
                }
                if (state->marks[particle->term] != definition) {
                    state->marks[particle->term] = definition;
                    status = push(state, particle->term);
                }
            }
        }
        if (status == SXML_STATUS_OK && is_circular) {
            status = report(state, (resolve_error){.constraint_name = "mg-props-correct.2",
                                                   .description = "A model group definition "
                                                                  "refers to itself",
                                                   .name = entry->group_name,
                                                   .position = entry->position});
        }
    }
    return status;
}

sxml_status sxml_schema_resolve_references(sxml_schema_compilation *compilation) {
    sxml_processor *processor = compilation->configuration->processor;
    resolver state = {.compilation = compilation,
                      .components = compilation->components,
                      .name_pool = processor->name_pool,
                      .allocator = processor->allocator};
    sxml_status status = resolve_all_references(&state);
    if (status == SXML_STATUS_OK) {
        status = resolve_type_definitions(&state);
    }
    if (status == SXML_STATUS_OK) {
        status = check_type_cycles(&state);
    }
    if (status == SXML_STATUS_OK) {
        status = resolve_varieties(&state);
    }
    if (status == SXML_STATUS_OK) {
        status = resolve_affiliations(&state);
    }
    if (status == SXML_STATUS_OK) {
        status = resolve_element_types(&state);
    }
    if (status == SXML_STATUS_OK) {
        status = resolve_substitution_groups(&state);
    }
    if (status == SXML_STATUS_OK) {
        status = resolve_identity_constraint_lists(&state);
    }
    if (status == SXML_STATUS_OK) {
        resolve_other_references(&state);
        status = check_group_cycles(&state);
    }
    sxml_allocator_release(state.allocator, state.stack);
    sxml_allocator_release(state.allocator, state.marks);
    sxml_allocator_release(state.allocator, state.pairs);
    return status;
}
