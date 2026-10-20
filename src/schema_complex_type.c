/*
 * Compile stage COMPLEX_TYPES (src/schema_internal.h; docs/DECISIONS.md
 * D165, D167): the properties of every complex type definition from its
 * base and its explicit content, attribute uses and wildcards, the
 * derivation checks, content models (src/schema_content_model.c),
 * registration in the type registry, the checks of particles and element
 * declarations, and the derivation question of the validator.
 *
 * Golden file: src/types.c (descriptions derived and registered in
 * order), src/schema_components.c (writing properties through the
 * accessors).
 *
 * Section numbers are of XSD 1.1 Part 1. Types are compiled base first,
 * with an explicit stack (D71): an extension's content and attribute uses
 * start from its base's properties (3.4.2.3.3, 3.4.2.4), and a
 * restriction's content model is checked against its base's (3.4.6.4).
 * Attribute groups may refer to each other in cycles (3.6.2.1): a type's
 * or a group's attribute uses are those of the transitive closure of its
 * references, found with a work list.
 */
#include "schema_internal.h"

#include "cast_internal.h"
#include "processor_internal.h"
#include "types_internal.h"

#include <stdalign.h>
#include <string.h>

/* ----------------------------------------------------------- The stage */

/* What the stage keeps while it runs. */
typedef struct complex_type_stage {
    sxml_schema_compilation *compilation;
    sxml_schema_components *components;
    sxml_allocator allocator;
    /* Per type definition: 0 not yet, 1 being compiled, 2 compiled. */
    uint8_t *type_marks;
    /* Per attribute group: whether the closure being walked has it. */
    uint8_t *group_marks;
    /* Types waiting for their base (uint32_t). */
    sxml_schema_scratch_array type_stack;
    sxml_schema_scratch_array work_list;
    sxml_schema_scratch_array visited_groups;
    sxml_schema_scratch_array indexes;
    sxml_schema_scratch_array namespaces;
    sxml_schema_scratch_array names;
} complex_type_stage;

static sxml_schema_type_definition *edit_type(complex_type_stage *stage, uint32_t type_index) {
    return sxml_schema_components_edit_entry(
        stage->components,
        (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_TYPE_DEFINITION, .index = type_index});
}

static const sxml_schema_type_definition *get_type(const sxml_schema_components *components,
                                                   uint32_t type_index) {
    return sxml_schema_get_type_definition(
        components, (sxml_schema_type_definition_index){.value = type_index});
}

static uint32_t table_count(const sxml_schema_components *components, sxml_schema_table table) {
    return components->tables[table].entry_count;
}

static sxml_status append_index(complex_type_stage *stage, sxml_schema_scratch_array *array,
                                uint32_t value) {
    return sxml_schema_scratch_array_append(stage->allocator, array, &value);
}

static uint32_t index_at(const sxml_schema_scratch_array *array, size_t entry) {
    return *(const uint32_t *)sxml_schema_scratch_array_at(array, entry);
}

/* An error at a component. */
typedef struct stage_report {
    const char *constraint_name;
    const char *description;
    sxml_schema_position position;
} stage_report;

static sxml_status report(complex_type_stage *stage, stage_report request) {
    return sxml_schema_compilation_report(
        stage->compilation,
        &(sxml_schema_report_request){
            .constraint_name = sxml_byte_view_from_c_string(request.constraint_name),
            .description = sxml_byte_view_from_c_string(request.description),
            .position = request.position});
}

/* --------------------------------------------------------- Derivation */

/* The method of one derivation step for blocking: simple steps count as
   restriction (3.16.6.3, clause 2.1). */
static sxml_schema_derivation_set
derivation_step_method(const sxml_schema_type_definition *type_definition) {
    if (type_definition->variety == SXML_TYPE_VARIETY_COMPLEX) {
        return (sxml_schema_derivation_set)type_definition->derivation_method;
    }
    return SXML_SCHEMA_DERIVATION_RESTRICTION;
}

/* Whether derived reaches base along {base type definition} without a
   blocked step (3.4.6.5, 3.16.6.3 clauses 1 to 2.2.3). */
static bool derives_along_bases(const sxml_schema_components *components,
                                const sxml_schema_derivation_request *request) {
    const uint32_t type_count = table_count(components, SXML_SCHEMA_TABLE_TYPE_DEFINITION);
    uint32_t current = request->derived_type.value;
    for (uint32_t step_index = 0; step_index <= type_count && current != 0; step_index += 1) {
        if (current == request->base_type.value) {
            return true;
        }
        const sxml_schema_type_definition *current_type = get_type(components, current);
        if ((derivation_step_method(current_type) & request->blocked_derivations) != 0 ||
            current_type->base_type.value == current) {
            return false;
        }
        current = current_type->base_type.value;
    }
    return false;
}

/* The most union types a derivation question walks through (2.2.4). */
#define UNION_WORK_CAPACITY 64U

sxml_status sxml_schema_check_derivation(const sxml_schema_components *components,
                                         const sxml_schema_derivation_request *request,
                                         sxml_schema_derivation_outcome *outcome) {
    outcome->is_derived = derives_along_bases(components, request);
    /* 3.16.6.3, clause 2.2.4: a member of a union without facets. */
    uint32_t unions[UNION_WORK_CAPACITY];
    size_t union_count = 0;
    const sxml_schema_type_definition *base = get_type(components, request->base_type.value);
    if (!outcome->is_derived && base->variety == SXML_TYPE_VARIETY_UNION &&
        base->facets.entry_count == 0) {
        unions[union_count] = request->base_type.value;
        union_count += 1;
    }
    while (!outcome->is_derived && union_count > 0) {
        union_count -= 1;
        const sxml_schema_index_view members = sxml_schema_get_list(
            components, get_type(components, unions[union_count])->member_types);
        for (size_t member = 0; !outcome->is_derived && member < members.index_count; member += 1) {
            sxml_schema_derivation_request member_request = *request;
            member_request.base_type.value = members.indexes[member];
            outcome->is_derived = derives_along_bases(components, &member_request);
            const sxml_schema_type_definition *member_type =
                get_type(components, members.indexes[member]);
            if (member_type->variety == SXML_TYPE_VARIETY_UNION &&
                member_type->facets.entry_count == 0 && union_count < UNION_WORK_CAPACITY) {
                unions[union_count] = members.indexes[member];
                union_count += 1;
            }
        }
    }
    return SXML_STATUS_OK;
}

/* ----------------------------------------------------------- Wildcards */

/* Whether a namespace constraint allows a namespace (3.10.4.3). */
static bool allows_namespace(const sxml_schema_components *components,
                             const sxml_schema_wildcard *wildcard, uint32_t namespace_index) {
    const sxml_schema_index_view namespaces =
        sxml_schema_get_list(components, wildcard->namespaces);
    bool is_listed = false;
    for (size_t entry = 0; entry < namespaces.index_count; entry += 1) {
        is_listed = is_listed || namespaces.indexes[entry] == namespace_index;
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

typedef enum wildcard_operation {
    SXML_WILDCARD_OPERATION_UNION,
    SXML_WILDCARD_OPERATION_INTERSECTION,
} wildcard_operation;

/* Two wildcards to combine; the result has the first one's {process
   contents} (3.4.2.5, 3.6.2.2: the complete or local wildcard's). */
typedef struct wildcard_combination {
    wildcard_operation operation;
    sxml_schema_wildcard_index first_wildcard;
    sxml_schema_wildcard_index second_wildcard;
} wildcard_combination;

/* Appends to stage->namespaces the namespaces of one list that the other
   wildcard allows (keep_allowed) or does not allow. */
typedef struct namespace_filter {
    const sxml_schema_wildcard *source;
    const sxml_schema_wildcard *other;
    bool keeps_allowed;
} namespace_filter;

static sxml_status filter_namespaces(complex_type_stage *stage, namespace_filter filter) {
    const sxml_schema_index_view namespaces =
        sxml_schema_get_list(stage->components, filter.source->namespaces);
    sxml_status status = SXML_STATUS_OK;
    for (size_t entry = 0; status == SXML_STATUS_OK && entry < namespaces.index_count; entry += 1) {
        const bool is_allowed =
            allows_namespace(stage->components, filter.other, namespaces.indexes[entry]);
        bool is_present = false;
        for (size_t seen_index = 0; seen_index < stage->namespaces.entry_count; seen_index += 1) {
            is_present =
                is_present || index_at(&stage->namespaces, seen_index) == namespaces.indexes[entry];
        }
        if (is_allowed == filter.keeps_allowed && !is_present) {
            status = append_index(stage, &stage->namespaces, namespaces.indexes[entry]);
        }
    }
    return status;
}

/* Attribute Wildcard Union and Intersection (3.10.6.3, 3.10.6.4): a new
   wildcard written to *result. */
static sxml_status combine_wildcards(complex_type_stage *stage, wildcard_combination combination,
                                     sxml_schema_wildcard_index *result) {
    const sxml_schema_wildcard first =
        *sxml_schema_get_wildcard(stage->components, combination.first_wildcard);
    const sxml_schema_wildcard second =
        *sxml_schema_get_wildcard(stage->components, combination.second_wildcard);
    const bool is_union = combination.operation == SXML_WILDCARD_OPERATION_UNION;
    sxml_schema_wildcard combined = {.position = first.position,
                                     .process_contents = first.process_contents};
    stage->namespaces.entry_count = 0;
    sxml_status status = SXML_STATUS_OK;
    const sxml_schema_namespace_constraint_variety first_variety =
        first.namespace_constraint_variety;
    const sxml_schema_namespace_constraint_variety second_variety =
        second.namespace_constraint_variety;
    if (first_variety == SXML_SCHEMA_NAMESPACE_CONSTRAINT_ANY ||
        second_variety == SXML_SCHEMA_NAMESPACE_CONSTRAINT_ANY) {
        const sxml_schema_wildcard *any =
            first_variety == SXML_SCHEMA_NAMESPACE_CONSTRAINT_ANY ? &first : &second;
        const sxml_schema_wildcard *other = any == &first ? &second : &first;
        combined.namespace_constraint_variety =
            is_union ? SXML_SCHEMA_NAMESPACE_CONSTRAINT_ANY : other->namespace_constraint_variety;
        if (!is_union) {
            /* The other's namespaces, all of them (any allows each). */
            status = filter_namespaces(stage, (namespace_filter){other, any, true});
        }
    } else if (first_variety == SXML_SCHEMA_NAMESPACE_CONSTRAINT_ENUMERATION &&
               second_variety == SXML_SCHEMA_NAMESPACE_CONSTRAINT_ENUMERATION) {
        combined.namespace_constraint_variety = SXML_SCHEMA_NAMESPACE_CONSTRAINT_ENUMERATION;
        if (is_union) {
            status = filter_namespaces(stage, (namespace_filter){&first, &first, true});
            if (status == SXML_STATUS_OK) {
                status = filter_namespaces(stage, (namespace_filter){&second, &second, true});
            }
        } else {
            status = filter_namespaces(stage, (namespace_filter){&first, &second, true});
        }
    } else if (first_variety == SXML_SCHEMA_NAMESPACE_CONSTRAINT_NOT &&
               second_variety == SXML_SCHEMA_NAMESPACE_CONSTRAINT_NOT) {
        /* Union: not(A intersect B), any when empty; intersection:
           not(A union B). */
        combined.namespace_constraint_variety = SXML_SCHEMA_NAMESPACE_CONSTRAINT_NOT;
        if (is_union) {
            /* Members of A that B also excludes: those B does not allow. */
            status = filter_namespaces(stage, (namespace_filter){&first, &second, false});
            if (stage->namespaces.entry_count == 0) {
                combined.namespace_constraint_variety = SXML_SCHEMA_NAMESPACE_CONSTRAINT_ANY;
            }
        } else {
            status = filter_namespaces(stage, (namespace_filter){&first, &first, false});
            if (status == SXML_STATUS_OK) {
                status = filter_namespaces(stage, (namespace_filter){&second, &second, false});
            }
        }
    } else {
        /* One not(S1), one enumeration S2. */
        const sxml_schema_wildcard *negated =
            first_variety == SXML_SCHEMA_NAMESPACE_CONSTRAINT_NOT ? &first : &second;
        const sxml_schema_wildcard *listed =
            first_variety == SXML_SCHEMA_NAMESPACE_CONSTRAINT_NOT ? &second : &first;
        if (is_union) {
            /* not(S1 minus S2), any when empty. */
            combined.namespace_constraint_variety = SXML_SCHEMA_NAMESPACE_CONSTRAINT_NOT;
            status = filter_namespaces(stage, (namespace_filter){negated, listed, false});
            if (stage->namespaces.entry_count == 0) {
                combined.namespace_constraint_variety = SXML_SCHEMA_NAMESPACE_CONSTRAINT_ANY;
            }
        } else {
            /* S2 minus S1. */
            combined.namespace_constraint_variety = SXML_SCHEMA_NAMESPACE_CONSTRAINT_ENUMERATION;
            status = filter_namespaces(stage, (namespace_filter){listed, negated, true});
        }
    }
    /* {disallowed names} (3.10.6.3, 3.10.6.4): a union keeps each name
       the other wildcard does not allow either, an intersection each name
       whose namespace the other allows. */
    stage->names.entry_count = 0;
    const sxml_schema_wildcard *sources[] = {&first, &second};
    for (size_t source = 0; status == SXML_STATUS_OK && source < 2; source += 1) {
        const sxml_schema_wildcard *other = sources[1 - source];
        const sxml_schema_index_view disallowed =
            sxml_schema_get_list(stage->components, sources[source]->disallowed_names);
        for (size_t entry = 0; status == SXML_STATUS_OK && entry < disallowed.index_count;
             entry += 1) {
            sxml_name_parts parts = {.namespace_uri = {.index = 0}};
            status = sxml_name_pool_get_name_parts(
                stage->components->processor->name_pool,
                (sxml_name_identifier){.index = disallowed.indexes[entry]}, &parts);
            bool is_allowed_by_other =
                allows_namespace(stage->components, other, parts.namespace_uri.index);
            const bool namespace_allowed = is_allowed_by_other;
            const sxml_schema_index_view other_names =
                sxml_schema_get_list(stage->components, other->disallowed_names);
            for (size_t other_entry = 0; other_entry < other_names.index_count; other_entry += 1) {
                is_allowed_by_other = is_allowed_by_other &&
                                      other_names.indexes[other_entry] != disallowed.indexes[entry];
            }
            const bool keeps = is_union ? !is_allowed_by_other : namespace_allowed;
            bool is_present = false;
            for (size_t seen_index = 0; seen_index < stage->names.entry_count; seen_index += 1) {
                is_present =
                    is_present || index_at(&stage->names, seen_index) == disallowed.indexes[entry];
            }
            if (status == SXML_STATUS_OK && keeps && !is_present) {
                status = append_index(stage, &stage->names, disallowed.indexes[entry]);
            }
        }
    }
    combined.disallows_defined = is_union ? first.disallows_defined && second.disallows_defined
                                          : first.disallows_defined || second.disallows_defined;
    combined.disallows_sibling = is_union ? first.disallows_sibling && second.disallows_sibling
                                          : first.disallows_sibling || second.disallows_sibling;
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_components_add_list(
            stage->components,
            (sxml_schema_index_view){.indexes = sxml_schema_scratch_array_at(&stage->namespaces, 0),
                                     .index_count = stage->namespaces.entry_count},
            &combined.namespaces);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_components_add_list(
            stage->components,
            (sxml_schema_index_view){.indexes = sxml_schema_scratch_array_at(&stage->names, 0),
                                     .index_count = stage->names.entry_count},
            &combined.disallowed_names);
    }
    uint32_t new_index = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_components_add_entries(
            stage->components,
            &(sxml_schema_entry_request){
                .table = SXML_SCHEMA_TABLE_WILDCARD, .entries = &combined, .entry_count = 1},
            &new_index);
    }
    if (status == SXML_STATUS_OK) {
        result->value = new_index;
    }
    return status;
}

/* ---------------------------------------------------- Attribute uses */

/* The attribute name of an attribute use. */
static sxml_name_identifier use_name(const sxml_schema_components *components, uint32_t use) {
    const sxml_schema_attribute_use *entry =
        sxml_schema_get_attribute_use(components, (sxml_schema_attribute_use_index){.value = use});
    return sxml_schema_get_attribute_declaration(components, entry->attribute_declaration)
        ->attribute_name;
}

/* The attribute uses and wildcard of the closure of a type's or group's
   references (3.4.2.4, 3.4.2.5, 3.6.2): uses into stage->indexes. */
typedef struct attribute_closure {
    /* The own uses, as read (prohibited ones are left out), the own
       xs:attributeGroup references and xs:anyAttribute. */
    sxml_schema_list own_uses;
    sxml_schema_list group_references;
    sxml_schema_wildcard_index own_wildcard;
    /* The default attribute group, appended after the references. */
    sxml_schema_attribute_group_index default_group;
    /* Out: the complete wildcard (3.6.2.2), 0 for none. */
    sxml_schema_wildcard_index complete_wildcard;
} attribute_closure;

static sxml_status add_use(complex_type_stage *stage, uint32_t use) {
    const sxml_schema_attribute_use *entry = sxml_schema_get_attribute_use(
        stage->components, (sxml_schema_attribute_use_index){.value = use});
    if (entry->use_kind == SXML_SCHEMA_ATTRIBUTE_USE_PROHIBITED) {
        return SXML_STATUS_OK;
    }
    for (size_t seen_index = 0; seen_index < stage->indexes.entry_count; seen_index += 1) {
        if (index_at(&stage->indexes, seen_index) == use) {
            return SXML_STATUS_OK;
        }
    }
    return append_index(stage, &stage->indexes, use);
}

/* Visits an attribute group of the closure once. */
static sxml_status visit_group(complex_type_stage *stage, uint32_t group) {
    if (group == 0 || stage->group_marks[group] != 0) {
        return SXML_STATUS_OK;
    }
    stage->group_marks[group] = 1;
    sxml_status status = append_index(stage, &stage->visited_groups, group);
    if (status == SXML_STATUS_OK) {
        status = append_index(stage, &stage->work_list, group);
    }
    return status;
}

static sxml_status visit_group_references(complex_type_stage *stage, sxml_schema_list references) {
    const sxml_schema_index_view entries = sxml_schema_get_list(stage->components, references);
    sxml_status status = SXML_STATUS_OK;
    for (size_t entry = 0; status == SXML_STATUS_OK && entry < entries.index_count; entry += 1) {
        status = visit_group(stage, sxml_schema_get_reference(stage->components,
                                                              (sxml_schema_reference_index){
                                                                  .value = entries.indexes[entry]})
                                        ->component);
    }
    return status;
}

/* Intersects the closure's complete wildcard with another one. */
static sxml_status intersect_into(complex_type_stage *stage, attribute_closure *closure,
                                  sxml_schema_wildcard_index wildcard) {
    if (wildcard.value == 0) {
        return SXML_STATUS_OK;
    }
    if (closure->complete_wildcard.value == 0) {
        closure->complete_wildcard = wildcard;
        return SXML_STATUS_OK;
    }
    return combine_wildcards(
        stage,
        (wildcard_combination){.operation = SXML_WILDCARD_OPERATION_INTERSECTION,
                               .first_wildcard = closure->complete_wildcard,
                               .second_wildcard = wildcard},
        &closure->complete_wildcard);
}

static sxml_status collect_attribute_closure(complex_type_stage *stage,
                                             attribute_closure *closure) {
    const sxml_schema_components *components = stage->components;
    stage->indexes.entry_count = 0;
    stage->work_list.entry_count = 0;
    stage->visited_groups.entry_count = 0;
    closure->complete_wildcard = closure->own_wildcard;
    const sxml_schema_index_view own_index = sxml_schema_get_list(components, closure->own_uses);
    sxml_status status = SXML_STATUS_OK;
    for (size_t entry = 0; status == SXML_STATUS_OK && entry < own_index.index_count; entry += 1) {
        status = add_use(stage, own_index.indexes[entry]);
    }
    if (status == SXML_STATUS_OK) {
        status = visit_group_references(stage, closure->group_references);
    }
    if (status == SXML_STATUS_OK) {
        status = visit_group(stage, closure->default_group.value);
    }
    /* Breadth first, so uses come in the order of the references. */
    for (size_t next_index = 0;
         status == SXML_STATUS_OK && next_index < stage->work_list.entry_count; next_index += 1) {
        const sxml_schema_attribute_group group = *sxml_schema_get_attribute_group(
            components,
            (sxml_schema_attribute_group_index){.value = index_at(&stage->work_list, next_index)});
        const sxml_schema_index_view uses = sxml_schema_get_list(components, group.attribute_uses);
        for (size_t entry = 0; status == SXML_STATUS_OK && entry < uses.index_count; entry += 1) {
            status = add_use(stage, uses.indexes[entry]);
        }
        if (status == SXML_STATUS_OK) {
            status = intersect_into(stage, closure, group.attribute_wildcard);
        }
        if (status == SXML_STATUS_OK) {
            status = visit_group_references(stage, group.attribute_group_references);
        }
    }
    for (size_t entry = 0; entry < stage->visited_groups.entry_count; entry += 1) {
        stage->group_marks[index_at(&stage->visited_groups, entry)] = 0;
    }
    return status;
}

/* No two uses of one attribute name (ct-props-correct.4, ag-props-correct.2). */
static sxml_status check_unique_uses(complex_type_stage *stage, stage_report error) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t first = 0; status == SXML_STATUS_OK && first < stage->indexes.entry_count;
         first += 1) {
        for (size_t second = first + 1;
             status == SXML_STATUS_OK && second < stage->indexes.entry_count; second += 1) {
            if (use_name(stage->components, index_at(&stage->indexes, first)).index ==
                use_name(stage->components, index_at(&stage->indexes, second)).index) {
                status = report(stage, error);
                return status;
            }
        }
    }
    return status;
}

/* The {attribute uses} and {attribute wildcard} of every attribute group
   (3.6.2), computed from what was read before any is written. */
static sxml_status compile_attribute_groups(complex_type_stage *stage) {
    sxml_schema_components *components = stage->components;
    const uint32_t group_count = table_count(components, SXML_SCHEMA_TABLE_ATTRIBUTE_GROUP);
    /* (uses list, wildcard) per group, written at the end. */
    sxml_schema_scratch_array results = {.entry_byte_count = sizeof(sxml_schema_attribute_group)};
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t group = 1; status == SXML_STATUS_OK && group < group_count; group += 1) {
        const sxml_schema_attribute_group read = *sxml_schema_get_attribute_group(
            components, (sxml_schema_attribute_group_index){.value = group});
        attribute_closure closure = {.own_uses = read.attribute_uses,
                                     .group_references = read.attribute_group_references,
                                     .own_wildcard = read.attribute_wildcard};
        stage->group_marks[group] = 1;
        status = collect_attribute_closure(stage, &closure);
        stage->group_marks[group] = 0;
        sxml_schema_attribute_group result = read;
        if (status == SXML_STATUS_OK) {
            status = check_unique_uses(
                stage, (stage_report){.constraint_name = "ag-props-correct.2",
                                      .description = "Two attribute uses of an attribute group "
                                                     "have the same name",
                                      .position = read.position});
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_schema_components_add_list(
                components,
                (sxml_schema_index_view){.indexes =
                                             sxml_schema_scratch_array_at(&stage->indexes, 0),
                                         .index_count = stage->indexes.entry_count},
                &result.attribute_uses);
            result.attribute_wildcard = closure.complete_wildcard;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_schema_scratch_array_append(stage->allocator, &results, &result);
        }
    }
    for (uint32_t group = 1; status == SXML_STATUS_OK && group < group_count; group += 1) {
        sxml_schema_attribute_group *entry = sxml_schema_components_edit_entry(
            components,
            (sxml_schema_entry_key){.table = SXML_SCHEMA_TABLE_ATTRIBUTE_GROUP, .index = group});
        const sxml_schema_attribute_group *result =
            sxml_schema_scratch_array_at(&results, group - 1);
        if (entry != NULL) {
            entry->attribute_uses = result->attribute_uses;
            entry->attribute_wildcard = result->attribute_wildcard;
        }
    }
    sxml_schema_scratch_array_release(stage->allocator, &results);
    return status;
}

/* ------------------------------------------------------------ Content */

/* A content type as COMPLEX_TYPES derives it (3.4.1, {content type}). */
typedef struct derived_content {
    sxml_content_type_variety variety;
    uint32_t particle;
    uint32_t simple_content_type;
    sxml_schema_open_content_mode open_content_mode;
    uint32_t open_content_wildcard;
} derived_content;

/* A particle (1..1) of a new model group of particles, at a type's
   position (3.4.2.3.3, clauses 3.1.1, 4.2.3 and 6.2). */
typedef struct group_request {
    sxml_schema_compositor compositor;
    sxml_schema_position position;
    sxml_schema_index_view particles;
    uint64_t minimum_occurrences;
} group_request;

static sxml_status add_group_particle(complex_type_stage *stage, group_request request,
                                      uint32_t *particle_index) {
    sxml_schema_model_group model_group = {.compositor = request.compositor,
                                           .position = request.position};
    sxml_status status = sxml_schema_components_add_list(stage->components, request.particles,
                                                         &model_group.particles);
    uint32_t group_index = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_components_add_entries(
            stage->components,
            &(sxml_schema_entry_request){
                .table = SXML_SCHEMA_TABLE_MODEL_GROUP, .entries = &model_group, .entry_count = 1},
            &group_index);
    }
    const sxml_schema_particle particle = {.position = request.position,
                                           .minimum_occurrences = request.minimum_occurrences,
                                           .maximum_occurrences = 1,
                                           .term_kind = SXML_SCHEMA_TERM_MODEL_GROUP,
                                           .term = group_index};
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_components_add_entries(
            stage->components,
            &(sxml_schema_entry_request){
                .table = SXML_SCHEMA_TABLE_PARTICLE, .entries = &particle, .entry_count = 1},
            particle_index);
    }
    return status;
}

static bool particle_is_all_group(const sxml_schema_components *components,
                                  uint32_t particle_index) {
    const sxml_schema_particle *particle =
        sxml_schema_get_particle(components, (sxml_schema_particle_index){.value = particle_index});
    return particle->term_kind == SXML_SCHEMA_TERM_MODEL_GROUP &&
           sxml_schema_get_model_group(components,
                                       (sxml_schema_model_group_index){.value = particle->term})
                   ->compositor == SXML_SCHEMA_COMPOSITOR_ALL;
}

/* The content type of complex content (3.4.2.3.3). */
static sxml_status derive_complex_content(complex_type_stage *stage, uint32_t type_index,
                                          derived_content *content) {
    const sxml_schema_components *components = stage->components;
    const sxml_schema_type_definition read = *get_type(components, type_index);
    const sxml_schema_type_definition base = *get_type(components, read.base_type.value);
    const bool is_extension = read.derivation_method == SXML_SCHEMA_DERIVATION_EXTENSION;
    sxml_status status = SXML_STATUS_OK;
    /* Clause 3: the effective content. */
    uint32_t effective_content = read.explicit_content.value;
    if (effective_content == 0 && read.is_mixed) {
        status = add_group_particle(stage,
                                    (group_request){.compositor = SXML_SCHEMA_COMPOSITOR_SEQUENCE,
                                                    .position = read.position,
                                                    .minimum_occurrences = 1},
                                    &effective_content);
    }
    /* Clause 4: the explicit content type. */
    const bool base_has_particle =
        base.variety == SXML_TYPE_VARIETY_COMPLEX &&
        (base.content_type_variety == SXML_CONTENT_TYPE_VARIETY_ELEMENT_ONLY ||
         base.content_type_variety == SXML_CONTENT_TYPE_VARIETY_MIXED);
    *content = (derived_content){.variety = SXML_CONTENT_TYPE_VARIETY_EMPTY};
    if (!is_extension || !base_has_particle) {
        if (effective_content != 0) {
            content->variety = read.is_mixed ? SXML_CONTENT_TYPE_VARIETY_MIXED
                                             : SXML_CONTENT_TYPE_VARIETY_ELEMENT_ONLY;
            content->particle = effective_content;
        }
    } else if (effective_content == 0) {
        content->variety = base.content_type_variety;
        content->particle = base.particle.value;
        content->open_content_mode = base.open_content_mode;
        content->open_content_wildcard = base.open_content_wildcard.value;
    } else {
        content->variety = read.is_mixed ? SXML_CONTENT_TYPE_VARIETY_MIXED
                                         : SXML_CONTENT_TYPE_VARIETY_ELEMENT_ONLY;
        content->open_content_mode = base.open_content_mode;
        content->open_content_wildcard = base.open_content_wildcard.value;
        if (read.explicit_content.value == 0 &&
            particle_is_all_group(components, base.particle.value)) {
            content->particle = base.particle.value;
        } else if (particle_is_all_group(components, base.particle.value) &&
                   particle_is_all_group(components, effective_content)) {
            /* Clause 4.2.3.2: one all group of both groups' particles. */
            const sxml_schema_particle *base_particle =
                sxml_schema_get_particle(components, base.particle);
            const sxml_schema_particle *own_particle = sxml_schema_get_particle(
                components, (sxml_schema_particle_index){.value = effective_content});
            const uint64_t minimum = own_particle->minimum_occurrences;
            if (minimum != base_particle->minimum_occurrences) {
                /* Particle Valid (Extension) (3.9.6.2), clause 3.1. */
                status =
                    report(stage, (stage_report){.constraint_name = "cos-ct-extends.1.4.3.2.2.2",
                                                 .description = "An all group extending an all "
                                                                "group must have its minOccurs",
                                                 .position = read.position});
            }
            const sxml_schema_index_view base_particles = sxml_schema_get_list(
                components, sxml_schema_get_model_group(
                                components, (sxml_schema_model_group_index){base_particle->term})
                                ->particles);
            const sxml_schema_index_view own_particles = sxml_schema_get_list(
                components, sxml_schema_get_model_group(
                                components, (sxml_schema_model_group_index){own_particle->term})
                                ->particles);
            stage->indexes.entry_count = 0;
            for (size_t entry = 0; status == SXML_STATUS_OK && entry < base_particles.index_count;
                 entry += 1) {
                status = append_index(stage, &stage->indexes, base_particles.indexes[entry]);
            }
            for (size_t entry = 0; status == SXML_STATUS_OK && entry < own_particles.index_count;
                 entry += 1) {
                status = append_index(stage, &stage->indexes, own_particles.indexes[entry]);
            }
            if (status == SXML_STATUS_OK) {
                status = add_group_particle(
                    stage,
                    (group_request){
                        .compositor = SXML_SCHEMA_COMPOSITOR_ALL,
                        .position = read.position,
                        .particles = {.indexes = sxml_schema_scratch_array_at(&stage->indexes, 0),
                                      .index_count = stage->indexes.entry_count},
                        .minimum_occurrences = minimum},
                    &content->particle);
            }
        } else {
            /* Clause 4.2.3.3: a sequence of the base's particle and the
               effective content. */
            const uint32_t particles[] = {base.particle.value, effective_content};
            status = add_group_particle(
                stage,
                (group_request){.compositor = SXML_SCHEMA_COMPOSITOR_SEQUENCE,
                                .position = read.position,
                                .particles = {.indexes = particles, .index_count = 2},
                                .minimum_occurrences = 1},
                &content->particle);
        }
    }
    /* Clauses 5 and 6: the wildcard element, as the reader gave it; a
       default one with appliesToEmpty false does not apply to an empty
       explicit content type (clause 6.1). */
    const bool skips_empty =
        read.open_content_skips_empty && content->variety == SXML_CONTENT_TYPE_VARIETY_EMPTY;
    if (status == SXML_STATUS_OK && read.open_content_mode != SXML_SCHEMA_OPEN_CONTENT_NONE &&
        read.open_content_wildcard.value != 0 && !skips_empty) {
        if (content->variety == SXML_CONTENT_TYPE_VARIETY_EMPTY) {
            content->variety = SXML_CONTENT_TYPE_VARIETY_ELEMENT_ONLY;
            status =
                add_group_particle(stage,
                                   (group_request){.compositor = SXML_SCHEMA_COMPOSITOR_SEQUENCE,
                                                   .position = read.position,
                                                   .minimum_occurrences = 1},
                                   &content->particle);
        }
        sxml_schema_wildcard_index wildcard = read.open_content_wildcard;
        if (status == SXML_STATUS_OK && content->open_content_wildcard != 0) {
            status = combine_wildcards(
                stage,
                (wildcard_combination){.operation = SXML_WILDCARD_OPERATION_UNION,
                                       .first_wildcard = read.open_content_wildcard,
                                       .second_wildcard = {content->open_content_wildcard}},
                &wildcard);
        }
        content->open_content_mode = read.open_content_mode;
        content->open_content_wildcard = wildcard.value;
    }
    return status;
}

/* Whether a content type's particle may be empty (3.9.6.3), through its
   content model when compiled. */
static bool content_is_emptiable(const sxml_schema_type_definition *type_definition) {
    if (type_definition->content_type_variety == SXML_CONTENT_TYPE_VARIETY_EMPTY) {
        return true;
    }
    if (type_definition->content_model == NULL) {
        return false;
    }
    /* The start state, every counter 0: a cursor without counter values
       reads each as 0 (src/schema_content_model.c). */
    uint64_t no_counters[1] = {0};
    sxml_schema_content_model_cursor cursor = {.content_model = type_definition->content_model,
                                               .counter_values = no_counters,
                                               .counter_count = 0};
    sxml_schema_content_model_start(&cursor);
    return sxml_schema_content_model_can_end(&cursor);
}

/* A simple type restricting base with the type's own facets and
   assertion facets (3.4.2.2, clauses 1 and 2), or base itself without
   any. */
typedef struct simple_restriction {
    uint32_t complex_type;
    uint32_t base_simple_type;
} simple_restriction;

static sxml_status restrict_simple_content(complex_type_stage *stage, simple_restriction request,
                                           uint32_t *simple_type) {
    const uint32_t base_simple_type = request.base_simple_type;
    const sxml_schema_type_definition read = *get_type(stage->components, request.complex_type);
    /* 3.4.2.2 clauses 1 and 2: the content's simple type restricts the
       base's. Without facets it is the base's, except for xs:anySimpleType
       and xs:anyAtomicType, which no restriction may have as its base (the
       note there; 3.16.6.2 clause 1.1): the synthesized type reports it. */
    const bool is_special_base = base_simple_type == (uint32_t)SXML_TYPE_XS_ANY_SIMPLE_TYPE ||
                                 base_simple_type == (uint32_t)SXML_TYPE_XS_ANY_ATOMIC_TYPE;
    if (read.facets.entry_count == 0 && read.facet_assertions.entry_count == 0 &&
        !is_special_base) {
        *simple_type = base_simple_type;
        return SXML_STATUS_OK;
    }
    const sxml_schema_type_definition *base = get_type(stage->components, base_simple_type);
    /* The facets and the assertions facet (Part 2, 4.3.13.2). */
    const sxml_schema_type_definition synthesized = {.variety = base->variety,
                                                     .position = read.position,
                                                     .base_type = {.value = base_simple_type},
                                                     .derivation_method =
                                                         SXML_SCHEMA_DERIVATION_RESTRICTION,
                                                     .facets = read.facets,
                                                     .assertions = read.facet_assertions};
    uint32_t new_index = 0;
    sxml_status status = sxml_schema_components_add_entries(
        stage->components,
        &(sxml_schema_entry_request){
            .table = SXML_SCHEMA_TABLE_TYPE_DEFINITION, .entries = &synthesized, .entry_count = 1},
        &new_index);
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_compile_simple_type(
            stage->compilation, (sxml_schema_type_definition_index){.value = new_index});
    }
    if (status == SXML_STATUS_OK) {
        *simple_type = new_index;
    }
    return status;
}

/* The content type of simple content (3.4.2.2), with the errors of its
   base (src-ct.2, ct-props-correct.2). */
static sxml_status derive_simple_content(complex_type_stage *stage, uint32_t type_index,
                                         derived_content *content) {
    const sxml_schema_type_definition read = *get_type(stage->components, type_index);
    const sxml_schema_type_definition base = *get_type(stage->components, read.base_type.value);
    const bool is_extension = read.derivation_method == SXML_SCHEMA_DERIVATION_EXTENSION;
    *content = (derived_content){.variety = SXML_CONTENT_TYPE_VARIETY_SIMPLE,
                                 .simple_content_type = (uint32_t)SXML_TYPE_XS_ANY_SIMPLE_TYPE};
    const bool base_is_complex = base.variety == SXML_TYPE_VARIETY_COMPLEX;
    if (base_is_complex && base.content_type_variety == SXML_CONTENT_TYPE_VARIETY_SIMPLE) {
        if (is_extension) {
            content->simple_content_type = base.simple_content_type.value;
            return SXML_STATUS_OK;
        }
        const uint32_t restricted = read.anonymous_base_type.value != 0
                                        ? read.anonymous_base_type.value
                                        : base.simple_content_type.value;
        return restrict_simple_content(
            stage, (simple_restriction){.complex_type = type_index, .base_simple_type = restricted},
            &content->simple_content_type);
    }
    if (base_is_complex && !is_extension &&
        base.content_type_variety == SXML_CONTENT_TYPE_VARIETY_MIXED &&
        content_is_emptiable(&base)) {
        const uint32_t restricted = read.anonymous_base_type.value != 0
                                        ? read.anonymous_base_type.value
                                        : (uint32_t)SXML_TYPE_XS_ANY_SIMPLE_TYPE;
        return restrict_simple_content(
            stage, (simple_restriction){.complex_type = type_index, .base_simple_type = restricted},
            &content->simple_content_type);
    }
    if (!base_is_complex && read.base_type.value != 0) {
        if (is_extension) {
            content->simple_content_type = read.base_type.value;
            return SXML_STATUS_OK;
        }
        /* ct-props-correct.2: a simple base only by extension. */
        return report(stage,
                      (stage_report){.constraint_name = "ct-props-correct.2",
                                     .description = "A complex type whose base is a simple type "
                                                    "must be derived by extension",
                                     .position = read.position});
    }
    if (read.base_type.value == 0) {
        return SXML_STATUS_OK;
    }
    /* Complex Type Definition Representation OK (3.4.3), clause 2. */
    return report(stage,
                  (stage_report){.constraint_name = "src-ct.2.1",
                                 .description = "The base of simple content must be a simple type "
                                                "or a complex type with simple content, or, for "
                                                "restriction, mixed and emptiable",
                                 .position = read.position});
}

/* ---------------------------------------------- Attributes of a type */

/* Whether the own uses of a type prohibit an attribute name (3.4.2.4,
   clause 3.2.2). */
static bool is_prohibited_name(const sxml_schema_components *components, sxml_schema_list own_uses,
                               sxml_name_identifier name) {
    const sxml_schema_index_view uses = sxml_schema_get_list(components, own_uses);
    for (size_t entry = 0; entry < uses.index_count; entry += 1) {
        const sxml_schema_attribute_use *use = sxml_schema_get_attribute_use(
            components, (sxml_schema_attribute_use_index){.value = uses.indexes[entry]});
        if (use->use_kind == SXML_SCHEMA_ATTRIBUTE_USE_PROHIBITED &&
            use_name(components, uses.indexes[entry]).index == name.index) {
            return true;
        }
    }
    return false;
}

/* The use of base_uses with the name, or 0. */
static uint32_t find_use(const sxml_schema_components *components, sxml_schema_list uses,
                         sxml_name_identifier name) {
    const sxml_schema_index_view entries = sxml_schema_get_list(components, uses);
    for (size_t entry = 0; entry < entries.index_count; entry += 1) {
        if (use_name(components, entries.indexes[entry]).index == name.index) {
            return entries.indexes[entry];
        }
    }
    return 0;
}

/* {attribute uses} and {attribute wildcard} of a type (3.4.2.4, 3.4.2.5),
   uses in stage->indexes, the own (not inherited) ones first. */
typedef struct derived_attributes {
    size_t own_use_count;
    sxml_schema_wildcard_index attribute_wildcard;
} derived_attributes;

static sxml_status derive_attributes(complex_type_stage *stage, uint32_t type_index,
                                     derived_attributes *attributes) {
    const sxml_schema_components *components = stage->components;
    const sxml_schema_type_definition read = *get_type(components, type_index);
    const sxml_schema_type_definition base = *get_type(components, read.base_type.value);
    attribute_closure closure = {.own_uses = read.attribute_uses,
                                 .group_references = read.attribute_group_references,
                                 .own_wildcard = read.attribute_wildcard,
                                 .default_group = read.applies_default_attributes
                                                      ? read.default_attribute_group
                                                      : (sxml_schema_attribute_group_index){0}};
    sxml_status status = collect_attribute_closure(stage, &closure);
    attributes->own_use_count = stage->indexes.entry_count;
    attributes->attribute_wildcard = closure.complete_wildcard;
    if (status != SXML_STATUS_OK || base.variety != SXML_TYPE_VARIETY_COMPLEX ||
        read.restricts_attribute_group) {
        return status;
    }
    const bool is_extension = read.derivation_method == SXML_SCHEMA_DERIVATION_EXTENSION;
    const sxml_schema_index_view base_uses = sxml_schema_get_list(components, base.attribute_uses);
    for (size_t entry = 0; status == SXML_STATUS_OK && entry < base_uses.index_count; entry += 1) {
        const sxml_name_identifier name = use_name(components, base_uses.indexes[entry]);
        bool is_overridden = false;
        for (size_t own_index = 0; own_index < attributes->own_use_count; own_index += 1) {
            is_overridden =
                is_overridden ||
                use_name(components, index_at(&stage->indexes, own_index)).index == name.index;
        }
        if (is_extension ||
            (!is_overridden && !is_prohibited_name(components, read.attribute_uses, name))) {
            status = add_use(stage, base_uses.indexes[entry]);
        }
    }
    if (status == SXML_STATUS_OK && is_extension && base.attribute_wildcard.value != 0) {
        if (attributes->attribute_wildcard.value == 0) {
            attributes->attribute_wildcard = base.attribute_wildcard;
        } else {
            status = combine_wildcards(
                stage,
                (wildcard_combination){.operation = SXML_WILDCARD_OPERATION_UNION,
                                       .first_wildcard = attributes->attribute_wildcard,
                                       .second_wildcard = base.attribute_wildcard},
                &attributes->attribute_wildcard);
        }
    }
    return status;
}

/* The strength of a wildcard's {process contents}: skip, lax, strict. */
static uint32_t process_contents_strength(const sxml_schema_components *components,
                                          sxml_schema_wildcard_index wildcard) {
    switch (sxml_schema_get_wildcard(components, wildcard)->process_contents) {
    case SXML_SCHEMA_PROCESS_CONTENTS_SKIP:
        return 0;
    case SXML_SCHEMA_PROCESS_CONTENTS_LAX:
        return 1;
    case SXML_SCHEMA_PROCESS_CONTENTS_STRICT:
        return 2;
    }
    return 2;
}

/* The value constraint of a use: its own, else its declaration's
   (3.5.1, effective value constraint). */
static sxml_schema_value_constraint
effective_value_constraint(const sxml_schema_components *components, uint32_t use) {
    const sxml_schema_attribute_use *entry =
        sxml_schema_get_attribute_use(components, (sxml_schema_attribute_use_index){.value = use});
    if (entry->value_constraint.variety != SXML_SCHEMA_VALUE_CONSTRAINT_NONE) {
        return entry->value_constraint;
    }
    return sxml_schema_get_attribute_declaration(components, entry->attribute_declaration)
        ->value_constraint;
}

/* Derivation Valid (Restriction, Complex), clause 3, for attributes: the
   restriction's attributes are allowed by the base and bound as strictly
   (the rules of XSD 1.0's clauses 2 to 4, which XSD 1.1 states as
   subsumption). Not checked against xs:anyType. */
static sxml_status check_attribute_restriction(complex_type_stage *stage, uint32_t type_index,
                                               derived_attributes attributes) {
    const sxml_schema_components *components = stage->components;
    const sxml_schema_type_definition *type_definition = get_type(components, type_index);
    const sxml_schema_type_definition *base =
        get_type(components, type_definition->base_type.value);
    if (type_definition->base_type.value == (uint32_t)SXML_TYPE_XS_ANY_TYPE) {
        return SXML_STATUS_OK;
    }
    const stage_report error = {.constraint_name = "derivation-ok-restriction.3",
                                .description = "The attributes of a restriction are not allowed by "
                                               "its base",
                                .position = type_definition->position};
    for (size_t own_index = 0; own_index < attributes.own_use_count; own_index += 1) {
        const uint32_t use = index_at(&stage->indexes, own_index);
        const sxml_name_identifier name = use_name(components, use);
        const uint32_t base_use = find_use(components, base->attribute_uses, name);
        if (base_use == 0) {
            if (base->attribute_wildcard.value == 0 ||
                !sxml_schema_wildcard_allows_name(
                    components,
                    &(sxml_schema_wildcard_test){
                        .wildcard = base->attribute_wildcard,
                        .name = name,
                        .symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_DECLARATION})) {
                return report(stage, error);
            }
            continue;
        }
        const sxml_schema_attribute_use *own_entry = sxml_schema_get_attribute_use(
            components, (sxml_schema_attribute_use_index){.value = use});
        const sxml_schema_attribute_use *base_entry = sxml_schema_get_attribute_use(
            components, (sxml_schema_attribute_use_index){.value = base_use});
        sxml_schema_derivation_outcome outcome = {.is_derived = false};
        sxml_status status = sxml_schema_check_derivation(
            components,
            &(sxml_schema_derivation_request){
                .derived_type = sxml_schema_get_attribute_declaration(
                                    components, own_entry->attribute_declaration)
                                    ->type_definition,
                .base_type = sxml_schema_get_attribute_declaration(
                                 components, base_entry->attribute_declaration)
                                 ->type_definition},
            &outcome);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const sxml_schema_value_constraint base_value =
            effective_value_constraint(components, base_use);
        const sxml_schema_value_constraint own_value = effective_value_constraint(components, use);
        bool is_value_compatible = base_value.variety != SXML_SCHEMA_VALUE_CONSTRAINT_FIXED;
        if (!is_value_compatible && own_value.variety == SXML_SCHEMA_VALUE_CONSTRAINT_FIXED) {
            /* 3.4.6.4, subsumption: a fixed value equal or identical to
               the base's, in the restriction's type. */
            is_value_compatible = sxml_schema_value_constraints_are_equal(
                components, &(sxml_schema_value_comparison){
                                .first = base_value,
                                .second = own_value,
                                .type_definition = sxml_schema_get_attribute_declaration(
                                                       components, own_entry->attribute_declaration)
                                                       ->type_definition});
        }
        const bool keeps_required = base_entry->use_kind != SXML_SCHEMA_ATTRIBUTE_USE_REQUIRED ||
                                    own_entry->use_kind == SXML_SCHEMA_ATTRIBUTE_USE_REQUIRED;
        /* 3.4.6.4 clause 5.4 (Attribute Uses): G.{inheritable} =
           S.{inheritable}. */
        if (!outcome.is_derived || !is_value_compatible || !keeps_required ||
            own_entry->is_inheritable != base_entry->is_inheritable) {
            return report(stage, error);
        }
    }
    /* A required use of the base stays (XSD 1.0 clause 3): its name has
       a use in the restriction's {attribute uses}, which hold no
       prohibited ones (3.4.2.5). */
    const sxml_schema_index_view base_uses = sxml_schema_get_list(components, base->attribute_uses);
    for (size_t entry = 0; entry < base_uses.index_count; entry += 1) {
        const sxml_schema_attribute_use *base_entry = sxml_schema_get_attribute_use(
            components, (sxml_schema_attribute_use_index){.value = base_uses.indexes[entry]});
        if (base_entry->use_kind == SXML_SCHEMA_ATTRIBUTE_USE_REQUIRED &&
            find_use(components, type_definition->attribute_uses,
                     use_name(components, base_uses.indexes[entry])) == 0) {
            return report(stage, error);
        }
    }
    /* The wildcard: within the base's, no weaker (XSD 1.0 clause 4). */
    if (attributes.attribute_wildcard.value != 0 &&
        (base->attribute_wildcard.value == 0 ||
         !sxml_schema_wildcard_is_subset(
             components,
             &(sxml_schema_wildcard_subset_request){
                 .subset = attributes.attribute_wildcard,
                 .superset = base->attribute_wildcard,
                 .symbol_space = SXML_SCHEMA_SYMBOL_SPACE_ATTRIBUTE_DECLARATION}) ||
         process_contents_strength(components, attributes.attribute_wildcard) <
             process_contents_strength(components, base->attribute_wildcard))) {
        return report(stage, error);
    }
    return SXML_STATUS_OK;
}

/* ----------------------------------------------------------- Checking */

/* Derivation Valid (Extension) (3.4.6.2) and (Restriction, Complex)
   (3.4.6.3) on the derived properties; content restriction itself is the
   content model's (cos-content-act-restrict). */
static sxml_status check_type_derivation(complex_type_stage *stage, uint32_t type_index) {
    const sxml_schema_components *components = stage->components;
    const sxml_schema_type_definition *type_definition = get_type(components, type_index);
    const sxml_schema_type_definition *base =
        get_type(components, type_definition->base_type.value);
    const sxml_schema_position position = type_definition->position;
    if (type_definition->base_type.value == 0) {
        return SXML_STATUS_OK;
    }
    if (base->variety != SXML_TYPE_VARIETY_COMPLEX && !type_definition->has_simple_content) {
        return report(stage, (stage_report){.constraint_name = "src-ct.1",
                                            .description = "The base of complex content must be a "
                                                           "complex type",
                                            .position = position});
    }
    if (type_definition->derivation_method == SXML_SCHEMA_DERIVATION_EXTENSION) {
        if ((base->final_derivations & SXML_SCHEMA_DERIVATION_EXTENSION) != 0) {
            return report(
                stage, (stage_report){.constraint_name = base->variety == SXML_TYPE_VARIETY_COMPLEX
                                                             ? "cos-ct-extends.1.1"
                                                             : "cos-ct-extends.2.2",
                                      .description = "The base type's final forbids extension",
                                      .position = position});
        }
        if (base->variety != SXML_TYPE_VARIETY_COMPLEX) {
            return SXML_STATUS_OK;
        }
        const sxml_content_type_variety derived = type_definition->content_type_variety;
        const sxml_content_type_variety inherited = base->content_type_variety;
        const bool is_simple_pair = derived == SXML_CONTENT_TYPE_VARIETY_SIMPLE &&
                                    inherited == SXML_CONTENT_TYPE_VARIETY_SIMPLE;
        const bool is_empty_pair = derived == SXML_CONTENT_TYPE_VARIETY_EMPTY &&
                                   inherited == SXML_CONTENT_TYPE_VARIETY_EMPTY;
        const bool has_particle = derived == SXML_CONTENT_TYPE_VARIETY_ELEMENT_ONLY ||
                                  derived == SXML_CONTENT_TYPE_VARIETY_MIXED;
        const bool is_particle_extension =
            has_particle && (inherited == SXML_CONTENT_TYPE_VARIETY_EMPTY || inherited == derived);
        if (!is_simple_pair && !is_empty_pair && !is_particle_extension) {
            return report(stage,
                          (stage_report){.constraint_name = "cos-ct-extends.1.4",
                                         .description = "An extension's content type does not "
                                                        "extend its base's",
                                         .position = position});
        }
        if (is_particle_extension && inherited != SXML_CONTENT_TYPE_VARIETY_EMPTY &&
            base->open_content_mode == SXML_SCHEMA_OPEN_CONTENT_INTERLEAVE &&
            type_definition->open_content_mode == SXML_SCHEMA_OPEN_CONTENT_SUFFIX) {
            return report(stage,
                          (stage_report){.constraint_name = "cos-ct-extends.1.4.3.2.2.3",
                                         .description = "An extension of interleaved open content "
                                                        "must interleave",
                                         .position = position});
        }
        return SXML_STATUS_OK;
    }
    if ((base->final_derivations & SXML_SCHEMA_DERIVATION_RESTRICTION) != 0) {
        return report(stage,
                      (stage_report){.constraint_name = "derivation-ok-restriction.1",
                                     .description = "The base type's final forbids restriction",
                                     .position = position});
    }
    if (type_definition->base_type.value == (uint32_t)SXML_TYPE_XS_ANY_TYPE ||
        base->variety != SXML_TYPE_VARIETY_COMPLEX) {
        return SXML_STATUS_OK;
    }
    const sxml_content_type_variety derived = type_definition->content_type_variety;
    const sxml_content_type_variety restricted = base->content_type_variety;
    bool is_valid = false;
    if (derived == SXML_CONTENT_TYPE_VARIETY_SIMPLE) {
        sxml_schema_derivation_outcome outcome = {.is_derived = false};
        sxml_status status = SXML_STATUS_OK;
        if (restricted == SXML_CONTENT_TYPE_VARIETY_SIMPLE) {
            status = sxml_schema_check_derivation(
                components,
                &(sxml_schema_derivation_request){.derived_type =
                                                      type_definition->simple_content_type,
                                                  .base_type = base->simple_content_type},
                &outcome);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        is_valid = outcome.is_derived ||
                   (restricted == SXML_CONTENT_TYPE_VARIETY_MIXED && content_is_emptiable(base));
    } else if (derived == SXML_CONTENT_TYPE_VARIETY_EMPTY) {
        is_valid = restricted != SXML_CONTENT_TYPE_VARIETY_SIMPLE && content_is_emptiable(base);
    } else {
        is_valid = (derived == SXML_CONTENT_TYPE_VARIETY_ELEMENT_ONLY &&
                    (restricted == SXML_CONTENT_TYPE_VARIETY_ELEMENT_ONLY ||
                     restricted == SXML_CONTENT_TYPE_VARIETY_MIXED)) ||
                   (derived == SXML_CONTENT_TYPE_VARIETY_MIXED &&
                    restricted == SXML_CONTENT_TYPE_VARIETY_MIXED);
    }
    if (!is_valid) {
        return report(stage, (stage_report){.constraint_name = "derivation-ok-restriction.2",
                                            .description = "A restriction's content type does not "
                                                           "restrict its base's",
                                            .position = position});
    }
    return SXML_STATUS_OK;
}

/* Registers a complex type (D165): its base's registered type (a simple
   one for simple content, 3.4.1; xs:anyType when the base did not
   register) and its content type. */
static sxml_status register_complex_type(complex_type_stage *stage, uint32_t type_index) {
    const sxml_schema_components *components = stage->components;
    const sxml_schema_type_definition *type_definition = get_type(components, type_index);
    const sxml_schema_type_definition *base =
        get_type(components, type_definition->base_type.value);
    const bool takes_base =
        base->registered_type.index != 0 &&
        (base->variety == SXML_TYPE_VARIETY_COMPLEX ||
         type_definition->content_type_variety == SXML_CONTENT_TYPE_VARIETY_SIMPLE);
    const sxml_type_identifier base_type =
        takes_base ? base->registered_type : sxml_builtin_type_identifier(SXML_TYPE_XS_ANY_TYPE);
    sxml_type_description description = {.variety = SXML_TYPE_VARIETY_COMPLEX,
                                         .type_name = type_definition->type_name,
                                         .base_type = base_type,
                                         .is_builtin = false};
    description.variety_details.complex_type = (sxml_complex_type){
        .content_type_variety = type_definition->content_type_variety,
        .simple_content_type =
            type_definition->content_type_variety == SXML_CONTENT_TYPE_VARIETY_SIMPLE
                ? get_type(components, type_definition->simple_content_type.value)->registered_type
                : (sxml_type_identifier){.index = SXML_TYPE_NONE}};
    if (type_definition->content_type_variety == SXML_CONTENT_TYPE_VARIETY_SIMPLE &&
        description.variety_details.complex_type.simple_content_type.index == SXML_TYPE_NONE) {
        /* The simple type did not register (its errors are reported). */
        description.variety_details.complex_type.simple_content_type =
            sxml_builtin_type_identifier(SXML_TYPE_XS_ANY_SIMPLE_TYPE);
    }
    sxml_type_identifier registered = {.index = SXML_TYPE_NONE};
    const sxml_status status = sxml_type_registry_register_schema_type(
        components->processor->type_registry, &description, &registered);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_schema_type_definition *edited = edit_type(stage, type_index);
    if (edited != NULL) {
        edited->registered_type = registered;
    }
    return SXML_STATUS_OK;
}

/* Derives, checks, compiles and registers one complex type whose base
   is compiled. */
static sxml_status compile_complex_type(complex_type_stage *stage, uint32_t type_index) {
    const size_t error_count = stage->compilation->error_count;
    const sxml_schema_type_definition read = *get_type(stage->components, type_index);
    derived_content content = {.variety = SXML_CONTENT_TYPE_VARIETY_EMPTY};
    sxml_status status = read.has_simple_content
                             ? derive_simple_content(stage, type_index, &content)
                             : derive_complex_content(stage, type_index, &content);
    derived_attributes attributes = {.own_use_count = 0};
    if (status == SXML_STATUS_OK) {
        status = derive_attributes(stage, type_index, &attributes);
    }
    if (status == SXML_STATUS_OK) {
        status = check_unique_uses(stage,
                                   (stage_report){.constraint_name = "ct-props-correct.4",
                                                  .description = "Two attribute uses of a "
                                                                 "complex type have the same name",
                                                  .position = read.position});
    }
    sxml_schema_list attribute_uses = {.first_entry_index = 0, .entry_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_components_add_list(
            stage->components,
            (sxml_schema_index_view){.indexes = sxml_schema_scratch_array_at(&stage->indexes, 0),
                                     .index_count = stage->indexes.entry_count},
            &attribute_uses);
    }
    /* {assertions}: the base's (a complex base's), then the own ones. */
    const sxml_schema_type_definition base = *get_type(stage->components, read.base_type.value);
    sxml_schema_list assertions = read.assertions;
    if (status == SXML_STATUS_OK && base.variety == SXML_TYPE_VARIETY_COMPLEX &&
        base.assertions.entry_count != 0 && read.base_type.value != type_index) {
        stage->work_list.entry_count = 0;
        const sxml_schema_index_view base_assertions =
            sxml_schema_get_list(stage->components, base.assertions);
        const sxml_schema_index_view own_assertions =
            sxml_schema_get_list(stage->components, read.assertions);
        for (size_t entry = 0; status == SXML_STATUS_OK && entry < base_assertions.index_count;
             entry += 1) {
            status = append_index(stage, &stage->work_list, base_assertions.indexes[entry]);
        }
        for (size_t entry = 0; status == SXML_STATUS_OK && entry < own_assertions.index_count;
             entry += 1) {
            status = append_index(stage, &stage->work_list, own_assertions.indexes[entry]);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_schema_components_add_list(
                stage->components,
                (sxml_schema_index_view){.indexes =
                                             sxml_schema_scratch_array_at(&stage->work_list, 0),
                                         .index_count = stage->work_list.entry_count},
                &assertions);
        }
    }
    sxml_schema_type_definition *edited =
        status == SXML_STATUS_OK ? edit_type(stage, type_index) : NULL;
    if (edited != NULL) {
        edited->content_type_variety = content.variety;
        edited->particle.value = content.particle;
        edited->simple_content_type.value = content.simple_content_type;
        edited->open_content_mode = content.open_content_wildcard != 0
                                        ? content.open_content_mode
                                        : SXML_SCHEMA_OPEN_CONTENT_NONE;
        edited->open_content_wildcard.value = content.open_content_wildcard;
        edited->attribute_uses = attribute_uses;
        edited->attribute_wildcard = attributes.attribute_wildcard;
        edited->assertions = assertions;
    }
    if (status == SXML_STATUS_OK && read.derivation_method == SXML_SCHEMA_DERIVATION_RESTRICTION &&
        base.variety == SXML_TYPE_VARIETY_COMPLEX) {
        status = check_attribute_restriction(stage, type_index, attributes);
    }
    if (status == SXML_STATUS_OK) {
        status = check_type_derivation(stage, type_index);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_schema_compile_content_model(
            stage->compilation, (sxml_schema_type_definition_index){.value = type_index});
    }
    if (status == SXML_STATUS_OK && stage->compilation->error_count == error_count) {
        status = register_complex_type(stage, type_index);
    }
    return status;
}

/* ------------------------------------------------------ Value constraints */

/* The simple type a value constraint's values have, as registered; NONE
   for one no cast targets (xs:anySimpleType and xs:anyAtomicType take
   every value, Part 2, 3.2.1 and 3.2.2) or that did not register. */
static sxml_type_identifier value_type_of(const sxml_schema_components *components,
                                          uint32_t type_index) {
    const sxml_schema_type_definition *type_definition = get_type(components, type_index);
    if (type_definition->variety == SXML_TYPE_VARIETY_COMPLEX) {
        if (type_definition->content_type_variety != SXML_CONTENT_TYPE_VARIETY_SIMPLE) {
            return (sxml_type_identifier){.index = SXML_TYPE_NONE};
        }
        type_definition = get_type(components, type_definition->simple_content_type.value);
    }
    const sxml_type_identifier registered = type_definition->registered_type;
    if (registered.index == (uint32_t)SXML_TYPE_XS_ANY_SIMPLE_TYPE ||
        registered.index == (uint32_t)SXML_TYPE_XS_ANY_ATOMIC_TYPE) {
        return (sxml_type_identifier){.index = SXML_TYPE_NONE};
    }
    return registered;
}

/* A value constraint cast to a registered simple type. */
typedef struct value_cast {
    sxml_schema_value_constraint value_constraint;
    sxml_type_identifier target_type;
} value_cast;

static sxml_status cast_value_constraint(const sxml_schema_components *components,
                                         value_cast request, sxml_sequence *value) {
    const sxml_schema_namespace_context *context =
        sxml_schema_get_namespace_context(components, request.value_constraint.namespace_context);
    sxml_error_record ignored;
    return sxml_cast_make_value_from_text(
        sxml_processor_get_item_heap(components->processor),
        &(sxml_text_cast_request){
            .text = sxml_schema_get_text(components, request.value_constraint.lexical_form),
            .target_type = request.target_type,
            .namespace_bindings =
                sxml_schema_get_namespace_bindings(components, context->namespace_bindings),
            .error_record = &ignored},
        value);
}

/* The primitive type of an atomic item, for Part 2, 2.2.3: values of
   different primitive types are never equal. */
static sxml_type_identifier primitive_of(const sxml_schema_components *components,
                                         sxml_item item) {
    sxml_type_description description;
    if (sxml_type_registry_describe_type(components->processor->type_registry,
                                         sxml_item_get_type_annotation(item),
                                         &description) != SXML_STATUS_OK) {
        return (sxml_type_identifier){.index = SXML_TYPE_NONE};
    }
    return description.primitive_type;
}

/* Two cast values: equal or identical item for item (Part 2, 2.2.3,
   2.2.4; a list is equal to a list of as many pairwise equal items). */
typedef struct value_pair {
    sxml_sequence first;
    sxml_sequence second;
} value_pair;

static bool values_are_equal(const sxml_schema_components *components, value_pair pair) {
    sxml_sequence_iterator first = sxml_sequence_iterator_start(pair.first);
    sxml_sequence_iterator second = sxml_sequence_iterator_start(pair.second);
    for (;;) {
        sxml_iteration_step first_step = {.has_item = false};
        sxml_iteration_step second_step = {.has_item = false};
        if (sxml_sequence_iterator_next(&first, NULL, &first_step) != SXML_STATUS_OK ||
            sxml_sequence_iterator_next(&second, NULL, &second_step) != SXML_STATUS_OK ||
            first_step.has_item != second_step.has_item) {
            return false;
        }
        if (!first_step.has_item) {
            return true;
        }
        if (!sxml_atomic_item_is_atomic_equal(first_step.item, second_step.item) ||
            primitive_of(components, first_step.item).index !=
                primitive_of(components, second_step.item).index) {
            return false;
        }
    }
}

bool sxml_schema_value_constraints_are_equal(const sxml_schema_components *components,
                                             const sxml_schema_value_comparison *comparison) {
    const sxml_byte_view first_text =
        sxml_schema_get_text(components, comparison->first.lexical_form);
    const sxml_byte_view second_text =
        sxml_schema_get_text(components, comparison->second.lexical_form);
    if (sxml_byte_view_equals(first_text, second_text)) {
        return true;
    }
    const sxml_type_identifier target_type =
        value_type_of(components, comparison->type_definition.value);
    if (target_type.index == SXML_TYPE_NONE) {
        return false;
    }
    sxml_sequence first = sxml_sequence_empty();
    sxml_sequence second = sxml_sequence_empty();
    bool is_equal =
        cast_value_constraint(components,
                              (value_cast){.value_constraint = comparison->first,
                                           .target_type = target_type},
                              &first) == SXML_STATUS_OK &&
        cast_value_constraint(components,
                              (value_cast){.value_constraint = comparison->second,
                                           .target_type = target_type},
                              &second) == SXML_STATUS_OK;
    if (is_equal) {
        is_equal = values_are_equal(components, (value_pair){.first = first, .second = second});
    }
    sxml_sequence_release(first);
    sxml_sequence_release(second);
    return is_equal;
}

/* ------------------------------------------------ Element declarations */

/* Element Default Valid (Immediate) (3.3.6.2), clause 1: the value
   constraint of a declaration with a simple type or simple content is a
   valid value of it (Simple Default Valid, 3.2.6.2), checked by casting
   (src/cast.c). A type that did not register was reported already. */
static sxml_status check_simple_value_constraint(complex_type_stage *stage,
                                                 uint32_t declaration_index) {
    const sxml_schema_components *components = stage->components;
    const sxml_schema_element_declaration *declaration = sxml_schema_get_element_declaration(
        components, (sxml_schema_element_declaration_index){.value = declaration_index});
    if (declaration->value_constraint.variety == SXML_SCHEMA_VALUE_CONSTRAINT_NONE) {
        return SXML_STATUS_OK;
    }
    const sxml_type_identifier target_type =
        value_type_of(components, declaration->type_definition.value);
    if (target_type.index == SXML_TYPE_NONE) {
        return SXML_STATUS_OK;
    }
    sxml_sequence value = sxml_sequence_empty();
    const sxml_status status = cast_value_constraint(
        components,
        (value_cast){.value_constraint = declaration->value_constraint, .target_type = target_type},
        &value);
    if (status == SXML_STATUS_OK) {
        sxml_sequence_release(value);
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_DYNAMIC_ERROR && status != SXML_STATUS_TYPE_ERROR &&
        status != SXML_STATUS_STATIC_ERROR && status != SXML_STATUS_INVALID_ARGUMENT) {
        return status;
    }
    return report(stage,
                  (stage_report){.constraint_name = "e-props-correct.2",
                                 .description = "An element's value constraint is not a valid "
                                                "value of its type",
                                 .position = declaration->position});
}

/* Element Declaration Properties Correct (3.3.6.1), clauses 2 (for
   complex types; Element Default Valid (Immediate), 3.3.6.2, clause 2),
   4 (substitution group affiliations), 6 and 7 (type tables). */
static sxml_status check_element_declaration(complex_type_stage *stage,
                                             uint32_t declaration_index) {
    const sxml_schema_components *components = stage->components;
    const sxml_schema_element_declaration *declaration = sxml_schema_get_element_declaration(
        components, (sxml_schema_element_declaration_index){.value = declaration_index});
    const sxml_schema_type_definition *type_definition =
        get_type(components, declaration->type_definition.value);
    const sxml_schema_position position = declaration->position;
    if (declaration->value_constraint.variety != SXML_SCHEMA_VALUE_CONSTRAINT_NONE &&
        type_definition->variety == SXML_TYPE_VARIETY_COMPLEX &&
        type_definition->content_type_variety != SXML_CONTENT_TYPE_VARIETY_SIMPLE &&
        (type_definition->content_type_variety != SXML_CONTENT_TYPE_VARIETY_MIXED ||
         !content_is_emptiable(type_definition))) {
        return report(stage,
                      (stage_report){.constraint_name = "e-props-correct.2",
                                     .description = "A value constraint needs simple content or "
                                                    "mixed content that may be empty",
                                     .position = position});
    }
    sxml_status status = check_simple_value_constraint(stage, declaration_index);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_schema_index_view heads =
        sxml_schema_get_list(components, declaration->substitution_group_affiliations);
    for (size_t entry = 0; entry < heads.index_count; entry += 1) {
        const sxml_schema_element_declaration *head = sxml_schema_get_element_declaration(
            components, (sxml_schema_element_declaration_index){.value = heads.indexes[entry]});
        sxml_schema_derivation_outcome outcome = {.is_derived = false};
        status = sxml_schema_check_derivation(
            components,
            &(sxml_schema_derivation_request){.derived_type = declaration->type_definition,
                                              .base_type = head->type_definition,
                                              .blocked_derivations =
                                                  head->substitution_group_exclusions},
            &outcome);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!outcome.is_derived) {
            return report(stage,
                          (stage_report){.constraint_name = "e-props-correct.4",
                                         .description = "A member's type is not validly derived "
                                                        "from its head's type",
                                         .position = position});
        }
    }
    const sxml_schema_index_view alternatives =
        sxml_schema_get_list(components, declaration->type_alternatives);
    for (size_t entry = 0; entry < alternatives.index_count; entry += 1) {
        const sxml_schema_type_alternative *alternative = sxml_schema_get_type_alternative(
            components, (sxml_schema_type_alternative_index){.value = alternatives.indexes[entry]});
        if (alternative->test.value == 0 && entry + 1 < alternatives.index_count) {
            return report(stage,
                          (stage_report){.constraint_name = "e-props-correct.6",
                                         .description = "Only the last type alternative may lack "
                                                        "a test",
                                         .position = alternative->position});
        }
        if (alternative->type_definition.value == (uint32_t)SXML_TYPE_XS_ERROR ||
            alternative->type_definition.value == 0) {
            continue;
        }
        sxml_schema_derivation_outcome outcome = {.is_derived = false};
        status = sxml_schema_check_derivation(
            components,
            &(sxml_schema_derivation_request){
                .derived_type = alternative->type_definition,
                .base_type = declaration->type_definition,
                .blocked_derivations =
                    declaration->disallowed_substitutions &
                    (sxml_schema_derivation_set)(SXML_SCHEMA_DERIVATION_EXTENSION |
                                                 SXML_SCHEMA_DERIVATION_RESTRICTION)},
            &outcome);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (!outcome.is_derived) {
            return report(stage,
                          (stage_report){.constraint_name = "e-props-correct.7",
                                         .description = "A type alternative's type is not validly "
                                                        "substitutable for the declared type",
                                         .position = alternative->position});
        }
    }
    return SXML_STATUS_OK;
}

/* ----------------------------------------------------------- The stage */

/* Particle Correct (3.9.6.1), clause 2.1, for the schema's particles. */
static sxml_status check_particles(complex_type_stage *stage) {
    const sxml_schema_components *components = stage->components;
    const uint32_t particle_count = table_count(components, SXML_SCHEMA_TABLE_PARTICLE);
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t particle_index = 3; status == SXML_STATUS_OK && particle_index < particle_count;
         particle_index += 1) {
        const sxml_schema_particle *particle = sxml_schema_get_particle(
            components, (sxml_schema_particle_index){.value = particle_index});
        if (particle->maximum_occurrences != SXML_SCHEMA_UNBOUNDED &&
            particle->minimum_occurrences > particle->maximum_occurrences) {
            status =
                report(stage, (stage_report){.constraint_name = "p-props-correct.2.1",
                                             .description = "minOccurs is greater than maxOccurs",
                                             .position = particle->position});
        }
    }
    return status;
}

/* Compiles the complex types, each after its base (an explicit stack). */
static sxml_status compile_types_in_order(complex_type_stage *stage, uint32_t type_count) {
    sxml_schema_components *components = stage->components;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t type_index = 1; status == SXML_STATUS_OK && type_index < type_count;
         type_index += 1) {
        const sxml_schema_type_definition *type_definition = get_type(components, type_index);
        if (type_definition->is_builtin || type_definition->variety != SXML_TYPE_VARIETY_COMPLEX ||
            stage->type_marks[type_index] != 0) {
            continue;
        }
        stage->type_stack.entry_count = 0;
        stage->type_marks[type_index] = 1;
        status = append_index(stage, &stage->type_stack, type_index);
        while (status == SXML_STATUS_OK && stage->type_stack.entry_count > 0) {
            const uint32_t current =
                index_at(&stage->type_stack, stage->type_stack.entry_count - 1);
            const uint32_t base = get_type(components, current)->base_type.value;
            const sxml_schema_type_definition *base_type = get_type(components, base);
            if (base != 0 && base < type_count && !base_type->is_builtin &&
                base_type->variety == SXML_TYPE_VARIETY_COMPLEX && stage->type_marks[base] == 0) {
                /* A circular derivation (ct-props-correct.3) is RESOLVE's
                   error: a base being compiled is not waited for. */
                stage->type_marks[base] = 1;
                status = append_index(stage, &stage->type_stack, base);
                continue;
            }
            stage->type_stack.entry_count -= 1;
            status = compile_complex_type(stage, current);
            stage->type_marks[current] = 2;
        }
    }
    return status;
}

/* One allocation of count marks, zeroed. */
static sxml_status allocate_marks(complex_type_stage *stage, uint32_t count, uint8_t **marks) {
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        stage->allocator,
        (sxml_allocation_request){.byte_count = (size_t)count + 1, .alignment = 1}, &memory);
    if (status == SXML_STATUS_OK) {
        memset(memory, 0, (size_t)count + 1);
        *marks = memory;
    }
    return status;
}

sxml_status sxml_schema_compile_complex_types(sxml_schema_compilation *compilation) {
    sxml_schema_components *components = compilation->components;
    complex_type_stage stage = {.compilation = compilation,
                                .components = components,
                                .allocator = components->allocator,
                                .type_stack = {.entry_byte_count = sizeof(uint32_t)},
                                .work_list = {.entry_byte_count = sizeof(uint32_t)},
                                .visited_groups = {.entry_byte_count = sizeof(uint32_t)},
                                .indexes = {.entry_byte_count = sizeof(uint32_t)},
                                .namespaces = {.entry_byte_count = sizeof(uint32_t)},
                                .names = {.entry_byte_count = sizeof(uint32_t)}};
    /* Types synthesized for simple content are added while the stage runs:
       marks cover the types read, which are all it orders. */
    const uint32_t type_count = table_count(components, SXML_SCHEMA_TABLE_TYPE_DEFINITION);
    sxml_status status = allocate_marks(&stage, type_count, &stage.type_marks);
    if (status == SXML_STATUS_OK) {
        status = allocate_marks(&stage, table_count(components, SXML_SCHEMA_TABLE_ATTRIBUTE_GROUP),
                                &stage.group_marks);
    }
    if (status == SXML_STATUS_OK) {
        status = check_particles(&stage);
    }
    if (status == SXML_STATUS_OK) {
        status = compile_attribute_groups(&stage);
    }
    /* xs:anyType and xs:untyped have content models too (3.4.7). */
    for (uint32_t builtin_type = SXML_TYPE_XS_ANY_TYPE;
         status == SXML_STATUS_OK && builtin_type <= SXML_TYPE_XS_UNTYPED; builtin_type += 1) {
        if (get_type(components, builtin_type)->content_model == NULL) {
            status = sxml_schema_compile_content_model(
                compilation, (sxml_schema_type_definition_index){.value = builtin_type});
        }
    }
    if (status == SXML_STATUS_OK) {
        status = compile_types_in_order(&stage, type_count);
    }
    const uint32_t declaration_count =
        table_count(components, SXML_SCHEMA_TABLE_ELEMENT_DECLARATION);
    for (uint32_t declaration = 1; status == SXML_STATUS_OK && declaration < declaration_count;
         declaration += 1) {
        status = check_element_declaration(&stage, declaration);
    }
    sxml_allocator_release(stage.allocator, stage.type_marks);
    sxml_allocator_release(stage.allocator, stage.group_marks);
    sxml_schema_scratch_array_release(stage.allocator, &stage.type_stack);
    sxml_schema_scratch_array_release(stage.allocator, &stage.work_list);
    sxml_schema_scratch_array_release(stage.allocator, &stage.visited_groups);
    sxml_schema_scratch_array_release(stage.allocator, &stage.indexes);
    sxml_schema_scratch_array_release(stage.allocator, &stage.namespaces);
    sxml_schema_scratch_array_release(stage.allocator, &stage.names);
    return status;
}
