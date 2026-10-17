/*
 * The function library: the per-processor index over the native function
 * parts (src/function_library_internal.h, docs/DECISIONS.md D55, D78), and
 * the helpers the core parts share (src/function_library_core_internal.h).
 *
 * Golden file: src/memory.c (lifecycle, memory requirements, comments).
 *
 * The library keeps one indexed row per row of the parts it serves, sorted
 * by interned name, so a query is a binary search over names and a check of
 * arities. The type text of a row is parsed and resolved the first time the
 * row is found (XPath 4.0 SequenceType syntax, F&O 4.0, section 1.5
 * "Function signatures"); the resolved types live as long as the library,
 * so the views of a function entry stay valid while it does.
 */
#include "evaluator_internal.h"
#include "function_library_core_internal.h"
#include "function_library_internal.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/types.h"
#include "xpath_ast_internal.h"
#include "xpath_static_internal.h"

#include <math.h>
#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------ Structures */

/*
 * The parameter types of a variadic row for arities beyond its parameter
 * count: the last parameter's type repeated. Kept in a chain so that views
 * handed out earlier stay valid.
 */
typedef struct variadic_types {
    struct variadic_types *next;
    size_t type_count;
    sxml_sequence_type *types;
} variadic_types;

/* One row of a part, with its interned name and resolved types. */
typedef struct indexed_row {
    sxml_name_identifier function_name;
    const sxml_native_function *native_function;
    /* Parameters with SXML_PARAMETER_DEFAULT_REQUIRED. */
    size_t required_count;
    bool is_resolved;
    /* function_parameter_count resolved types (NULL before resolving, or
       for a row without parameters). */
    sxml_sequence_type *parameter_types;
    sxml_sequence_type result_type;
    variadic_types *variadic_types;
} indexed_row;

struct sxml_function_library {
    sxml_allocator allocator;
    /* The allocator of sxml_function_library_create; the null allocator
       after initialize. */
    sxml_allocator creating_allocator;
    sxml_item_heap *item_heap;
    indexed_row *rows;
    size_t row_count;
    /* xs:anyAtomicType?, the parameter of every constructor function. */
    sxml_sequence_type constructor_parameter_type;
};

/* ------------------------------------------------------------- Lifecycle */

sxml_status sxml_function_library_memory_requirements(
    const sxml_function_library_configuration *configuration,
    sxml_memory_requirements *requirements) {
    if (configuration == NULL || configuration->item_heap == NULL ||
        sxml_allocator_is_null(configuration->allocator)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *requirements = (sxml_memory_requirements){
        .byte_count = sizeof(sxml_function_library),
        .alignment = alignof(sxml_function_library)};
    return SXML_STATUS_OK;
}

static size_t count_required_parameters(const sxml_native_function *row) {
    size_t required_count = 0;
    for (size_t parameter_index = 0;
         parameter_index < row->parameters.function_parameter_count;
         parameter_index += 1) {
        if (row->parameters.function_parameters[parameter_index]
                .parameter_default == SXML_PARAMETER_DEFAULT_REQUIRED) {
            required_count += 1;
        }
    }
    return required_count;
}

static bool row_is_variadic(const indexed_row *row) {
    return (row->native_function->properties & SXML_NATIVE_FUNCTION_IS_VARIADIC) != 0;
}

/* True when two rows of the same name accept a common arity. */
static bool arities_overlap(const indexed_row *first, const indexed_row *second) {
    const size_t first_last = first->native_function->parameters.function_parameter_count;
    const size_t second_last = second->native_function->parameters.function_parameter_count;
    const bool first_ends_before =
        !row_is_variadic(first) && first_last < second->required_count;
    const bool second_ends_before =
        !row_is_variadic(second) && second_last < first->required_count;
    return !first_ends_before && !second_ends_before;
}

/* Sorts rows by name index (insertion sort: a few hundred rows, once). */
static void sort_rows(indexed_row *rows, size_t row_count) {
    for (size_t row_index = 1; row_index < row_count; row_index += 1) {
        const indexed_row moved = rows[row_index];
        size_t position = row_index;
        while (position > 0 &&
               rows[position - 1].function_name.index > moved.function_name.index) {
            rows[position] = rows[position - 1];
            position -= 1;
        }
        rows[position] = moved;
    }
}

static void release_row_types(sxml_function_library *library, indexed_row *row) {
    sxml_allocator_release(library->allocator, row->parameter_types);
    variadic_types *chain = row->variadic_types;
    while (chain != NULL) {
        variadic_types *next = chain->next;
        sxml_allocator_release(library->allocator, chain->types);
        sxml_allocator_release(library->allocator, chain);
        chain = next;
    }
    row->parameter_types = NULL;
    row->variadic_types = NULL;
    row->is_resolved = false;
}

/* Counts the rows of every part, checking the configuration. */
static sxml_status count_rows(const sxml_function_library_configuration *configuration,
                              size_t *row_count) {
    size_t total = 0;
    const sxml_native_function_view_list parts = configuration->parts;
    if (parts.native_function_view_count > 0 && parts.native_function_views == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    for (size_t part_index = 0; part_index < parts.native_function_view_count;
         part_index += 1) {
        const sxml_native_function_view *part = parts.native_function_views[part_index];
        if (part == NULL || (part->native_function_count > 0 && part->native_functions == NULL)) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        sxml_status status = sxml_size_add(total, part->native_function_count, &total);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    *row_count = total;
    return SXML_STATUS_OK;
}

/* Interns the names of every row into library->rows. */
static sxml_status index_rows(sxml_function_library *library,
                              const sxml_function_library_configuration *configuration) {
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(library->item_heap);
    const sxml_native_function_view_list parts = configuration->parts;
    size_t row_index = 0;
    for (size_t part_index = 0; part_index < parts.native_function_view_count;
         part_index += 1) {
        const sxml_native_function_view *part = parts.native_function_views[part_index];
        for (size_t function_index = 0; function_index < part->native_function_count;
             function_index += 1) {
            const sxml_native_function *row = &part->native_functions[function_index];
            if ((row->call == NULL) == (row->resumable_body == NULL) &&
                (row->properties & (SXML_NATIVE_FUNCTION_READS_CONTEXT_POSITION |
                                    SXML_NATIVE_FUNCTION_READS_CONTEXT_SIZE)) == 0) {
                /* A row has a call or a resumable body, never both. */
                return SXML_STATUS_INVALID_ARGUMENT;
            }
            sxml_name_identifier function_name = {.index = 0};
            sxml_status status = sxml_name_pool_intern_name(
                name_pool,
                &(sxml_name_request){.namespace_uri = sxml_well_known_namespace_identifier(
                                         row->function_namespace),
                                     .local_name = row->local_name},
                &function_name);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            library->rows[row_index] = (indexed_row){
                .function_name = function_name,
                .native_function = row,
                .required_count = count_required_parameters(row),
                .is_resolved = false,
                .parameter_types = NULL,
                .result_type = {.item_type = {.index = SXML_TYPE_NONE},
                                .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE},
                .variadic_types = NULL};
            row_index += 1;
        }
    }
    sort_rows(library->rows, library->row_count);
    for (size_t index = 1; index < library->row_count; index += 1) {
        for (size_t earlier = index; earlier > 0; earlier -= 1) {
            const indexed_row *first = &library->rows[earlier - 1];
            if (first->function_name.index != library->rows[index].function_name.index) {
                break;
            }
            if (arities_overlap(first, &library->rows[index])) {
                /* A name and arity served by two rows. */
                return SXML_STATUS_INVALID_ARGUMENT;
            }
        }
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_function_library_initialize(
    sxml_memory_view memory, const sxml_function_library_configuration *configuration,
    sxml_function_library **library) {
    sxml_memory_requirements requirements;
    sxml_status status =
        sxml_function_library_memory_requirements(configuration, &requirements);
    if (status == SXML_STATUS_OK) {
        status = sxml_memory_view_check_requirements(memory, requirements);
    }
    size_t row_count = 0;
    if (status == SXML_STATUS_OK) {
        status = count_rows(configuration, &row_count);
    }
    size_t byte_count = 0;
    if (status == SXML_STATUS_OK) {
        status = sxml_size_multiply(row_count == 0 ? 1 : row_count, sizeof(indexed_row),
                                    &byte_count);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *placement = memory.bytes;
    sxml_function_library *placed = placement;
    *placed = (sxml_function_library){
        .allocator = configuration->allocator,
        .creating_allocator = {.state = NULL, .operations = NULL},
        .item_heap = configuration->item_heap,
        .rows = NULL,
        .row_count = row_count,
        .constructor_parameter_type = {
            .item_type = sxml_builtin_type_identifier(SXML_TYPE_XS_ANY_ATOMIC_TYPE),
            .occurrence = SXML_OCCURRENCE_ZERO_OR_ONE}};
    void *rows = NULL;
    status = sxml_allocator_allocate(
        placed->allocator,
        (sxml_allocation_request){.byte_count = byte_count,
                                  .alignment = alignof(indexed_row)},
        &rows);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    placed->rows = rows;
    status = index_rows(placed, configuration);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(placed->allocator, placed->rows);
        return status;
    }
    *library = placed;
    return SXML_STATUS_OK;
}

void sxml_function_library_finalize(sxml_function_library *library) {
    if (library == NULL) {
        return;
    }
    for (size_t row_index = 0; row_index < library->row_count; row_index += 1) {
        release_row_types(library, &library->rows[row_index]);
    }
    sxml_allocator_release(library->allocator, library->rows);
    library->rows = NULL;
    library->row_count = 0;
}

sxml_status sxml_function_library_create(
    sxml_allocator allocator, const sxml_function_library_configuration *configuration,
    sxml_function_library **library) {
    sxml_memory_requirements requirements;
    sxml_status status =
        sxml_function_library_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){.byte_count = requirements.byte_count,
                                  .alignment = requirements.alignment},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_function_library *created = NULL;
    status = sxml_function_library_initialize(
        (sxml_memory_view){.bytes = memory, .byte_count = requirements.byte_count},
        configuration, &created);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    created->creating_allocator = allocator;
    *library = created;
    return SXML_STATUS_OK;
}

void sxml_function_library_destroy(sxml_function_library *library) {
    if (library == NULL) {
        return;
    }
    const sxml_allocator allocator = library->creating_allocator;
    sxml_function_library_finalize(library);
    sxml_allocator_release(allocator, library);
}

/* -------------------------------------------------------- Type resolution */

/*
 * Parses and resolves one type text of a row (D55): the predeclared
 * prefixes xs, fn, map, array, math and err are bound by static analysis.
 * Text that does not parse or resolve is an error in sxml itself.
 */
static sxml_status resolve_type_text(const sxml_function_library *library,
                                     sxml_byte_view type_text,
                                     sxml_sequence_type *sequence_type) {
    sxml_ast *ast = NULL;
    sxml_status status = sxml_xpath_build_sequence_type_ast(
        &(sxml_ast_build_request){
            .expression_text = type_text,
            .source_name = {.bytes = NULL, .byte_count = 0},
            .allocator = library->allocator,
            .maximum_expression_depth = 0,
            .error_record = NULL},
        &ast);
    if (status == SXML_STATUS_OK) {
        const sxml_xpath_static_context static_context = {
            .item_heap = library->item_heap,
            .allocator = library->allocator,
            .namespace_bindings = {.namespace_bindings = NULL,
                                   .namespace_binding_count = 0},
            .variable_declarations = {.variable_declarations = NULL,
                                      .variable_declaration_count = 0},
            .function_table = {.state = NULL, .operations = NULL}};
        status = sxml_xpath_resolve_sequence_type(
            &(sxml_xpath_type_request){.ast = ast,
                                       .type_node = ast->root,
                                       .static_context = &static_context,
                                       .error_record = NULL},
            sequence_type);
        sxml_ast_destroy(ast);
    }
    if (sxml_status_has_error_record(status)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return status;
}

static sxml_status allocate_types(const sxml_function_library *library,
                                  size_t type_count, sxml_sequence_type **types) {
    size_t byte_count = 0;
    sxml_status status = sxml_size_multiply(type_count, sizeof(sxml_sequence_type),
                                            &byte_count);
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate(
            library->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(sxml_sequence_type)},
            &memory);
    }
    if (status == SXML_STATUS_OK) {
        *types = memory;
    }
    return status;
}

/* Resolves the parameter and result types of row, once. */
static sxml_status resolve_row(sxml_function_library *library, indexed_row *row) {
    if (row->is_resolved) {
        return SXML_STATUS_OK;
    }
    const sxml_function_parameter_view parameters = row->native_function->parameters;
    sxml_sequence_type *parameter_types = NULL;
    sxml_status status = SXML_STATUS_OK;
    if (parameters.function_parameter_count > 0) {
        status = allocate_types(library, parameters.function_parameter_count,
                                &parameter_types);
    }
    for (size_t parameter_index = 0;
         status == SXML_STATUS_OK && parameter_index < parameters.function_parameter_count;
         parameter_index += 1) {
        status = resolve_type_text(
            library, parameters.function_parameters[parameter_index].parameter_type,
            &parameter_types[parameter_index]);
    }
    sxml_sequence_type result_type = {.item_type = {.index = SXML_TYPE_NONE},
                                      .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};
    if (status == SXML_STATUS_OK) {
        status = resolve_type_text(library, row->native_function->result_type,
                                   &result_type);
    }
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(library->allocator, parameter_types);
        return status;
    }
    row->parameter_types = parameter_types;
    row->result_type = result_type;
    row->is_resolved = true;
    return SXML_STATUS_OK;
}

/* A resolved row and the arity of a query. */
typedef struct row_arity {
    indexed_row *row;
    size_t arity;
} row_arity;

/* The parameter types of a resolved row for an arity. */
static sxml_status get_parameter_types(sxml_function_library *library, row_arity query,
                                       sxml_sequence_type_view *types) {
    indexed_row *row = query.row;
    const size_t arity = query.arity;
    const size_t parameter_count =
        row->native_function->parameters.function_parameter_count;
    if (arity <= parameter_count) {
        *types = (sxml_sequence_type_view){.sequence_types = row->parameter_types,
                                           .sequence_type_count = arity};
        return SXML_STATUS_OK;
    }
    for (variadic_types *chain = row->variadic_types; chain != NULL; chain = chain->next) {
        if (chain->type_count >= arity) {
            *types = (sxml_sequence_type_view){.sequence_types = chain->types,
                                               .sequence_type_count = arity};
            return SXML_STATUS_OK;
        }
    }
    /* A variadic row (its last parameter repeats, F&O 4.0, section 1.5.4);
       room for twice the arity, so few arrays are ever made. */
    size_t type_count = arity;
    if (arity <= SIZE_MAX / 2) {
        type_count = arity * 2;
    }
    void *chain_memory = NULL;
    sxml_status status = sxml_allocator_allocate(
        library->allocator,
        (sxml_allocation_request){.byte_count = sizeof(variadic_types),
                                  .alignment = alignof(variadic_types)},
        &chain_memory);
    sxml_sequence_type *repeated = NULL;
    if (status == SXML_STATUS_OK) {
        status = allocate_types(library, type_count, &repeated);
        if (status != SXML_STATUS_OK) {
            sxml_allocator_release(library->allocator, chain_memory);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    for (size_t type_index = 0; type_index < type_count; type_index += 1) {
        repeated[type_index] =
            row->parameter_types[type_index < parameter_count ? type_index
                                                              : parameter_count - 1];
    }
    variadic_types *chain = chain_memory;
    *chain = (variadic_types){
        .next = row->variadic_types, .type_count = type_count, .types = repeated};
    row->variadic_types = chain;
    *types = (sxml_sequence_type_view){.sequence_types = repeated,
                                       .sequence_type_count = arity};
    return SXML_STATUS_OK;
}

static sxml_status register_signature(const sxml_function_library *library,
                                      const sxml_function_entry *entry,
                                      sxml_type_identifier *signature) {
    return sxml_type_registry_register_item_type(
        sxml_item_heap_get_type_registry(library->item_heap),
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_FUNCTION,
            .variety_details = {.function_type = {.is_any_function = false,
                                                  .parameter_types = entry->parameter_types,
                                                  .result_type = entry->result_type}}},
        signature);
}

/* ---------------------------------------------------------- Finding rows */

static bool row_accepts(const indexed_row *row, size_t arity) {
    if (arity < row->required_count) {
        return false;
    }
    return row_is_variadic(row) ||
           arity <= row->native_function->parameters.function_parameter_count;
}

/*
 * xs: constructor functions (XPath 4.0, section 4.5.1.3, "Constructor
 * Functions"): one for every atomic, list and union type of the registry
 * except xs:anyAtomicType and xs:NOTATION, of arity 1, taking
 * xs:anyAtomicType? and giving the type (zero or one item; any number of
 * items for a list type).
 */
static sxml_status find_constructor(sxml_function_library *library,
                                    const sxml_function_query *query,
                                    sxml_function_entry *entry) {
    sxml_type_registry *registry = sxml_item_heap_get_type_registry(library->item_heap);
    sxml_type_identifier type = {.index = SXML_TYPE_NONE};
    if (query->arity != 1 ||
        !sxml_type_registry_find_type(registry, query->function_name, &type) ||
        type.index == SXML_TYPE_XS_ANY_ATOMIC_TYPE || type.index == SXML_TYPE_XS_NOTATION) {
        return SXML_STATUS_OK;
    }
    sxml_type_description description;
    sxml_status status = sxml_type_registry_describe_type(registry, type, &description);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence_type result_type = {.item_type = type,
                                      .occurrence = SXML_OCCURRENCE_ZERO_OR_ONE};
    switch (description.variety) {
    case SXML_TYPE_VARIETY_ATOMIC:
    case SXML_TYPE_VARIETY_UNION:
        break;
    case SXML_TYPE_VARIETY_LIST:
        result_type = (sxml_sequence_type){
            .item_type = description.variety_details.list_item_type,
            .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};
        break;
    case SXML_TYPE_VARIETY_COMPLEX:
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_ANY_ITEM:
    case SXML_TYPE_VARIETY_NODE_TEST:
    case SXML_TYPE_VARIETY_JSON_NODE_TEST:
    case SXML_TYPE_VARIETY_FUNCTION:
    case SXML_TYPE_VARIETY_MAP:
    case SXML_TYPE_VARIETY_ARRAY:
    case SXML_TYPE_VARIETY_RECORD:
    case SXML_TYPE_VARIETY_ENUMERATION:
    case SXML_TYPE_VARIETY_CHOICE:
        return SXML_STATUS_OK;
    }
    sxml_function_entry found = {
        .entry_kind = SXML_FUNCTION_ENTRY_KIND_CONSTRUCTOR,
        .native_function = NULL,
        .constructor_type = type,
        .ir_function_number = 0,
        .parameters = {.function_parameters = NULL, .function_parameter_count = 0},
        .signature = {.index = SXML_TYPE_NONE},
        .parameter_types = {.sequence_types = &library->constructor_parameter_type,
                            .sequence_type_count = 1},
        .result_type = result_type};
    status = register_signature(library, &found, &found.signature);
    if (status == SXML_STATUS_OK) {
        *entry = found;
    }
    return status;
}

/* The first row whose name index is not below name_index. */
static size_t lower_bound(const sxml_function_library *library,
                          sxml_name_identifier function_name) {
    size_t low = 0;
    size_t high = library->row_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (library->rows[middle].function_name.index < function_name.index) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

static sxml_status find_library_function(void *state, const sxml_function_query *query,
                                         sxml_function_entry *entry) {
    sxml_function_library *library = state;
    sxml_name_parts parts;
    sxml_status status = sxml_name_pool_get_name_parts(
        sxml_item_heap_get_name_pool(library->item_heap), query->function_name, &parts);
    if (status == SXML_STATUS_OK &&
        parts.namespace_uri.index == SXML_NAMESPACE_XML_SCHEMA) {
        *entry = (sxml_function_entry){.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
        return find_constructor(library, query, entry);
    }
    for (size_t row_index = lower_bound(library, query->function_name);
         row_index < library->row_count &&
         library->rows[row_index].function_name.index == query->function_name.index;
         row_index += 1) {
        indexed_row *row = &library->rows[row_index];
        if (!row_accepts(row, query->arity)) {
            continue;
        }
        status = resolve_row(library, row);
        sxml_function_entry found = {
            .entry_kind = SXML_FUNCTION_ENTRY_KIND_NATIVE,
            .native_function = row->native_function,
            .constructor_type = {.index = SXML_TYPE_NONE},
            .ir_function_number = 0,
            .parameters = row->native_function->parameters,
            .signature = {.index = SXML_TYPE_NONE},
            .parameter_types = {.sequence_types = NULL, .sequence_type_count = 0},
            .result_type = row->result_type};
        if (status == SXML_STATUS_OK) {
            status = get_parameter_types(library,
                                         (row_arity){.row = row, .arity = query->arity},
                                         &found.parameter_types);
        }
        if (status == SXML_STATUS_OK) {
            status = register_signature(library, &found, &found.signature);
        }
        if (status == SXML_STATUS_OK) {
            *entry = found;
        }
        return status;
    }
    *entry = (sxml_function_entry){.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
    return SXML_STATUS_OK;
}

static const sxml_function_table_operations library_table_operations = {
    .find_function = find_library_function,
};

sxml_function_table sxml_function_library_as_table(sxml_function_library *library) {
    return (sxml_function_table){.state = library, .operations = &library_table_operations};
}

sxml_status sxml_function_table_find(sxml_function_table table,
                                     const sxml_function_query *query,
                                     sxml_function_entry *entry) {
    if (table.operations == NULL || table.operations->find_function == NULL) {
        *entry = (sxml_function_entry){.entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
        return SXML_STATUS_OK;
    }
    return table.operations->find_function(table.state, query, entry);
}

/* ------------------------------------------------- Helpers for the parts */

sxml_sequence sxml_function_library_get_argument(const sxml_function_call *call,
                                                 size_t argument_index) {
    if (argument_index >= call->arguments.sequence_count) {
        return sxml_sequence_empty();
    }
    return call->arguments.sequences[argument_index];
}

sxml_status sxml_function_library_read_optional_item(const sxml_function_call *call,
                                                     size_t argument_index,
                                                     sxml_optional_item *optional_item) {
    const sxml_sequence argument = sxml_function_library_get_argument(call, argument_index);
    sxml_item item = {.representation = 0};
    sxml_status status = sxml_sequence_get_item(
        argument, (sxml_item_request){.item_index = 0, .error_record = call->error_record},
        &item);
    if (status == SXML_STATUS_INDEX_OUT_OF_RANGE) {
        *optional_item = (sxml_optional_item){.has_item = false, .item = item};
        return SXML_STATUS_OK;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_item second = {.representation = 0};
    status = sxml_sequence_get_item(
        argument, (sxml_item_request){.item_index = 1, .error_record = call->error_record},
        &second);
    if (status == SXML_STATUS_OK) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (status != SXML_STATUS_INDEX_OUT_OF_RANGE) {
        return status;
    }
    *optional_item = (sxml_optional_item){.has_item = true, .item = item};
    return SXML_STATUS_OK;
}

sxml_status sxml_function_library_read_string_argument(const sxml_function_call *call,
                                                       size_t argument_index,
                                                       sxml_string *string) {
    sxml_optional_item optional_item;
    sxml_status status =
        sxml_function_library_read_optional_item(call, argument_index, &optional_item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!optional_item.has_item) {
        *string = (sxml_string){.utf8 = {.bytes = NULL, .byte_count = 0},
                                .codepoint_count = 0,
                                .is_ascii = true};
        return SXML_STATUS_OK;
    }
    sxml_atomic_datum datum;
    status = sxml_atomic_item_get_datum(optional_item.item, &datum);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (datum.datum_kind != SXML_DATUM_KIND_STRING) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    *string = datum.datum_details.string;
    return SXML_STATUS_OK;
}

sxml_byte_view sxml_function_library_codepoint_collation_uri(void) {
    return SXML_BYTE_VIEW_FROM_LITERAL(
        "http://www.w3.org/2005/xpath-functions/collation/codepoint");
}

static const sxml_static_error unsupported_collation_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOCH0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "The collation is not supported: only the Unicode codepoint collation is "
        "(F&O 4.0, section 5.3.2)"),
};

sxml_status sxml_function_library_check_collation(const sxml_function_call *call,
                                                  size_t argument_index) {
    sxml_string collation;
    sxml_status status =
        sxml_function_library_read_string_argument(call, argument_index, &collation);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (collation.utf8.byte_count == 0 ||
        sxml_byte_view_equals(collation.utf8,
                              sxml_function_library_codepoint_collation_uri())) {
        return SXML_STATUS_OK;
    }
    return sxml_function_library_report(call, &unsupported_collation_error);
}

sxml_status sxml_function_library_make_string(sxml_item_heap *item_heap,
                                              sxml_byte_view utf8,
                                              sxml_sequence *result) {
    sxml_item item = {.representation = 0};
    sxml_status status = sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = {.utf8 = utf8,
                                                   .codepoint_count = 0,
                                                   .is_ascii = false}}}},
        &item);
    if (status == SXML_STATUS_OK) {
        *result = sxml_sequence_from_item(item);
    }
    return status;
}

sxml_sequence sxml_function_library_make_boolean(sxml_boolean_result value) {
    return sxml_sequence_from_item(value.is_true ? sxml_atomic_item_true()
                                                 : sxml_atomic_item_false());
}

sxml_status sxml_function_library_report(const sxml_function_call *call,
                                         const sxml_static_error *error) {
    return sxml_error_record_report(call->error_record, error);
}

/* ----------------------------------------------------------------- Steps */

sxml_status sxml_function_library_charge_steps(const sxml_function_call *call,
                                               uint64_t step_count) {
    if (call->dynamic_context == NULL || step_count == 0) {
        return SXML_STATUS_OK;
    }
    return sxml_dynamic_context_charge_steps(call->dynamic_context, step_count);
}

sxml_status sxml_function_library_charge_items(const sxml_function_call *call,
                                               sxml_sequence sequence) {
    if (call->dynamic_context == NULL) {
        return SXML_STATUS_OK;
    }
    uint64_t item_count = 0;
    const sxml_status status = sxml_sequence_count(sequence, call->error_record, &item_count);
    return status == SXML_STATUS_OK
               ? sxml_dynamic_context_charge_steps(call->dynamic_context, item_count)
               : status;
}

/* ------------------------------------------------------------- Positions */

/* fn:round of a double, half toward positive infinity (F&O 4.0, section
   4.4.4). */
static double round_half_up(double value) {
    const double floor_value = floor(value);
    return value - floor_value >= 0.5 ? floor_value + 1.0 : floor_value;
}

/* A $start or $length of fn:subsequence: a double, or an integer read
   exactly (QT4 subsequence-40-011, PR2543: integers at the limits of
   xs:double precision must still select the right items). */
typedef struct position_argument {
    bool has_value;
    bool is_double;
    double double_value;
    /* Already rounded (fn:round) when the argument was an xs:decimal. */
    int64_t integer_value;
} position_argument;

static const sxml_static_error position_overflow_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOAR0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "A position of fn:subsequence or fn:substring is beyond the supported "
        "integers (F&O 4.0, sections 5.4.3 and 14.1.14)"),
};

static sxml_status read_position_argument(const sxml_function_call *call,
                                          size_t argument_index,
                                          position_argument *argument) {
    *argument = (position_argument){.has_value = false};
    sxml_optional_item value;
    sxml_status status = sxml_function_library_read_optional_item(call, argument_index, &value);
    sxml_atomic_datum datum;
    if (status == SXML_STATUS_OK && value.has_item) {
        status = sxml_atomic_item_get_datum(value.item, &datum);
    }
    if (status != SXML_STATUS_OK || !value.has_item) {
        return status;
    }
    argument->has_value = true;
    switch (datum.datum_kind) {
    case SXML_DATUM_KIND_DOUBLE:
        argument->is_double = true;
        argument->double_value = datum.datum_details.double_number;
        return SXML_STATUS_OK;
    case SXML_DATUM_KIND_FLOAT:
        argument->is_double = true;
        argument->double_value = (double)datum.datum_details.float_number;
        return SXML_STATUS_OK;
    case SXML_DATUM_KIND_INTEGER:
        argument->integer_value = datum.datum_details.integer;
        return SXML_STATUS_OK;
    case SXML_DATUM_KIND_DECIMAL:
        break;
    case SXML_DATUM_KIND_BOOLEAN:
    case SXML_DATUM_KIND_STRING:
    case SXML_DATUM_KIND_QNAME:
    case SXML_DATUM_KIND_DATE_TIME:
    case SXML_DATUM_KIND_DURATION:
    case SXML_DATUM_KIND_BINARY:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_decimal *decimal = sxml_atomic_item_get_decimal(value.item);
    if (decimal == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_decimal rounded;
    status = sxml_decimal_round(
        sxml_item_heap_get_decimal_configuration(call->item_heap),
        &(sxml_decimal_rounding){.decimal = decimal,
                                 .precision = 0,
                                 .rounding_mode = SXML_ROUNDING_MODE_HALF_TO_CEILING},
        &rounded);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_decimal_to_integer(&rounded, &argument->integer_value);
    sxml_decimal_finalize(&rounded);
    if (status == SXML_STATUS_ARITHMETIC_OVERFLOW) {
        return sxml_function_library_report(call, &position_overflow_error);
    }
    return status;
}


/* The span of positions p with first <= p < end, in doubles. */
static sxml_position_span span_of_doubles(position_argument start, position_argument length) {
    const double start_value = start.is_double ? start.double_value : (double)start.integer_value;
    const double length_value =
        length.is_double ? length.double_value : (double)length.integer_value;
    const double first = round_half_up(start_value);
    const double end = length.has_value ? first + round_half_up(length_value) : INFINITY;
    /* 2^64: positions from here on exist in no sequence. */
    const double limit = 0x1p64;
    const double first_position = first < 1.0 ? 1.0 : first;
    const bool is_empty =
        isnan(first) || isnan(end) || !(first_position < end) || first_position >= limit;
    sxml_position_span outcome = {.is_empty = is_empty,
                                    .has_overflow = false,
                                    .first_index = 0,
                                    .has_end = false,
                                    .end_index = 0};
    if (!is_empty) {
        outcome.first_index = (uint64_t)first_position - 1;
        outcome.has_end = end < limit;
        if (outcome.has_end) {
            outcome.end_index = (uint64_t)end - 1;
        }
    }
    return outcome;
}

/* The span of positions with exact integer arithmetic. */
static sxml_position_span span_of_integers(position_argument start, position_argument length) {
    sxml_position_span outcome = {.is_empty = false,
                                    .has_overflow = false,
                                    .first_index = 0,
                                    .has_end = length.has_value,
                                    .end_index = 0};
    int64_t end = INT64_MAX;
    if (length.has_value) {
        outcome.has_overflow =
            (length.integer_value > 0 && start.integer_value > INT64_MAX - length.integer_value) ||
            (length.integer_value < 0 && start.integer_value < INT64_MIN - length.integer_value);
        if (outcome.has_overflow) {
            return outcome;
        }
        end = start.integer_value + length.integer_value;
    }
    const int64_t first_position = start.integer_value < 1 ? 1 : start.integer_value;
    outcome.is_empty = end <= first_position;
    outcome.first_index = (uint64_t)first_position - 1;
    outcome.end_index = outcome.is_empty ? 0 : (uint64_t)end - 1;
    return outcome;
}

sxml_status sxml_function_library_read_position_span(const sxml_function_call *call,
                                                     sxml_position_arguments arguments,
                                                     sxml_position_span *span) {
    position_argument start;
    position_argument length;
    sxml_status status = read_position_argument(call, arguments.start_argument, &start);
    if (status == SXML_STATUS_OK) {
        status = read_position_argument(call, arguments.length_argument, &length);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_position_span outcome = start.is_double || (length.has_value && length.is_double)
                                           ? span_of_doubles(start, length)
                                           : span_of_integers(start, length);
    if (outcome.has_overflow) {
        return sxml_function_library_report(call, &position_overflow_error);
    }
    *span = outcome;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------------ Text */

sxml_status sxml_function_library_text_append(sxml_library_text *text,
                                              sxml_byte_view bytes) {
    if (bytes.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    size_t required = 0;
    sxml_status status = sxml_size_add(text->byte_count, bytes.byte_count, &required);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (required > text->byte_capacity) {
        size_t capacity = text->byte_capacity < 64 ? 64 : text->byte_capacity;
        while (capacity < required) {
            status = sxml_size_multiply(capacity, 2, &capacity);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        void *memory = NULL;
        status = sxml_item_heap_allocate_bytes(
            text->item_heap,
            (sxml_allocation_request){.byte_count = capacity, .alignment = 1}, &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (text->byte_count > 0) {
            memcpy(memory, text->bytes, text->byte_count);
        }
        sxml_item_heap_release_bytes(
            text->item_heap,
            (sxml_heap_bytes){.memory = text->bytes, .byte_count = text->byte_capacity});
        text->bytes = memory;
        text->byte_capacity = capacity;
    }
    memcpy(text->bytes + text->byte_count, bytes.bytes, bytes.byte_count);
    text->byte_count = required;
    return SXML_STATUS_OK;
}

sxml_status sxml_function_library_text_append_codepoint(sxml_library_text *text,
                                                        uint32_t codepoint) {
    uint8_t encoded[4];
    size_t encoded_byte_count = 0;
    if (codepoint < 0x80U) {
        encoded[0] = (uint8_t)codepoint;
        encoded_byte_count = 1;
    } else if (codepoint < 0x800U) {
        encoded[0] = (uint8_t)(0xC0U | (codepoint >> 6U));
        encoded[1] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        encoded_byte_count = 2;
    } else if (codepoint < 0x10000U) {
        encoded[0] = (uint8_t)(0xE0U | (codepoint >> 12U));
        encoded[1] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
        encoded[2] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        encoded_byte_count = 3;
    } else {
        encoded[0] = (uint8_t)(0xF0U | (codepoint >> 18U));
        encoded[1] = (uint8_t)(0x80U | ((codepoint >> 12U) & 0x3FU));
        encoded[2] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
        encoded[3] = (uint8_t)(0x80U | (codepoint & 0x3FU));
        encoded_byte_count = 4;
    }
    return sxml_function_library_text_append(
        text, (sxml_byte_view){.bytes = encoded, .byte_count = encoded_byte_count});
}

sxml_byte_view sxml_function_library_text_view(const sxml_library_text *text) {
    return (sxml_byte_view){.bytes = text->bytes, .byte_count = text->byte_count};
}

void sxml_function_library_text_release(sxml_library_text *text) {
    sxml_item_heap_release_bytes(
        text->item_heap,
        (sxml_heap_bytes){.memory = text->bytes, .byte_count = text->byte_capacity});
    text->bytes = NULL;
    text->byte_count = 0;
    text->byte_capacity = 0;
}
