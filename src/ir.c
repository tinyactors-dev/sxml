/*
 * The shared intermediate representation: units, their arrays, the text
 * form and the pass list (src/ir_internal.h, src/ir_pass_internal.h;
 * docs/DECISIONS.md, D46, D55).
 *
 * Golden file: src/memory.c (lifecycle, checked size arithmetic). A unit
 * places its fixed part and a copy of its source name in the memory given
 * to initialize; every array grows through the configuration's allocator,
 * geometrically, by allocating anew and copying (D22).
 */
#include "ir_internal.h"
#include "ir_pass_internal.h"

#include "decimal_internal.h"
#include "types_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------ Lifecycle */

/* Where the parts of a unit's block go: the unit, then its source name,
   then (for a created unit) the allocator that made the block. */
typedef struct unit_layout {
    sxml_memory_requirements requirements;
    size_t source_name_offset;
    size_t creating_allocator_offset;
} unit_layout;

typedef struct unit_layout_request {
    size_t source_name_byte_count;
    bool is_created;
} unit_layout_request;

static sxml_status compute_unit_layout(unit_layout_request request,
                                       unit_layout *layout) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    size_t unit_offset = 0;
    sxml_status status = sxml_memory_requirements_append(
        &requirements,
        (sxml_memory_requirements){.byte_count = sizeof(sxml_ir_unit),
                                   .alignment = alignof(sxml_ir_unit)},
        &unit_offset);
    size_t source_name_offset = 0;
    if (status == SXML_STATUS_OK && request.source_name_byte_count > 0) {
        status = sxml_memory_requirements_append(
            &requirements,
            (sxml_memory_requirements){
                .byte_count = request.source_name_byte_count, .alignment = 1},
            &source_name_offset);
    }
    size_t creating_allocator_offset = 0;
    if (status == SXML_STATUS_OK && request.is_created) {
        status = sxml_memory_requirements_append(
            &requirements,
            (sxml_memory_requirements){.byte_count = sizeof(sxml_allocator),
                                       .alignment = alignof(sxml_allocator)},
            &creating_allocator_offset);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *layout = (unit_layout){
        .requirements = requirements,
        .source_name_offset = source_name_offset,
        .creating_allocator_offset = creating_allocator_offset,
    };
    return SXML_STATUS_OK;
}

static sxml_status check_unit_configuration(
    const sxml_ir_unit_configuration *configuration) {
    if (configuration == NULL ||
        sxml_allocator_is_null(configuration->allocator) ||
        configuration->item_heap == NULL ||
        (configuration->source_name.byte_count > 0 &&
         configuration->source_name.bytes == NULL)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_ir_unit_memory_requirements(
    const sxml_ir_unit_configuration *configuration,
    sxml_memory_requirements *requirements) {
    sxml_status status = check_unit_configuration(configuration);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    unit_layout layout = {0};
    status = compute_unit_layout(
        (unit_layout_request){
            .source_name_byte_count = configuration->source_name.byte_count,
            .is_created = false},
        &layout);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *requirements = layout.requirements;
    return SXML_STATUS_OK;
}

sxml_status sxml_ir_unit_initialize(
    sxml_memory_view memory, const sxml_ir_unit_configuration *configuration,
    sxml_ir_unit **unit) {
    sxml_memory_requirements requirements = {0};
    sxml_status status =
        sxml_ir_unit_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_memory_view_check_requirements(memory, requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* The memory was checked for the unit's size and alignment. */
    void *unit_memory = memory.bytes;
    sxml_ir_unit *new_unit = unit_memory;
    memset(new_unit, 0, sizeof(*new_unit));
    new_unit->reference_count = 1;
    new_unit->allocator = configuration->allocator;
    new_unit->item_heap = configuration->item_heap;
    new_unit->maximum_expression_count =
        configuration->maximum_expression_count;
    if (configuration->source_name.byte_count > 0) {
        /* The source name follows the unit (compute_unit_layout). */
        uint8_t *name_bytes = memory.bytes + sizeof(sxml_ir_unit);
        memcpy(name_bytes, configuration->source_name.bytes,
               configuration->source_name.byte_count);
        new_unit->source_name = (sxml_byte_view){
            .bytes = name_bytes,
            .byte_count = configuration->source_name.byte_count};
    }
    *unit = new_unit;
    return SXML_STATUS_OK;
}

void sxml_ir_unit_finalize(sxml_ir_unit *unit) {
    if (unit == NULL) {
        return;
    }
    for (uint32_t constant_index = 1; constant_index < unit->constant_count;
         constant_index += 1) {
        sxml_sequence_release(unit->constants[constant_index]);
    }
    sxml_allocator allocator = unit->allocator;
    sxml_allocator_release(allocator, unit->expressions);
    sxml_allocator_release(allocator, unit->atoms);
    sxml_allocator_release(allocator, unit->map_entries);
    sxml_allocator_release(allocator, unit->sort_keys);
    sxml_allocator_release(allocator, unit->functions);
    sxml_allocator_release(allocator, unit->constants);
    sxml_allocator_release(allocator, unit->global_variables);
    sxml_allocator_release(allocator, unit->name_tests);
    sxml_allocator_release(allocator, unit->catch_clauses);
    sxml_allocator_release(allocator, unit->namespace_bindings);
    sxml_allocator_release(allocator, unit->parameter_bindings);
    sxml_allocator_release(allocator, unit->modes);
    sxml_allocator_release(allocator, unit->template_rules);
    memset(unit, 0, sizeof(*unit));
}

sxml_status sxml_ir_unit_create(sxml_allocator allocator,
                                const sxml_ir_unit_configuration *configuration,
                                sxml_ir_unit **unit) {
    sxml_status status = check_unit_configuration(configuration);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    unit_layout layout = {0};
    status = compute_unit_layout(
        (unit_layout_request){
            .source_name_byte_count = configuration->source_name.byte_count,
            .is_created = true},
        &layout);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *block = NULL;
    status = sxml_allocator_allocate(
        allocator,
        (sxml_allocation_request){
            .byte_count = layout.requirements.byte_count,
            .alignment = layout.requirements.alignment},
        &block);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_unit *new_unit = NULL;
    status = sxml_ir_unit_initialize(
        (sxml_memory_view){.bytes = block,
                           .byte_count = layout.requirements.byte_count},
        configuration, &new_unit);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, block);
        return status;
    }
    memcpy((uint8_t *)block + layout.creating_allocator_offset, &allocator,
           sizeof(allocator));
    new_unit->is_created = true;
    *unit = new_unit;
    return SXML_STATUS_OK;
}

void sxml_ir_unit_destroy(sxml_ir_unit *unit) {
    if (unit == NULL) {
        return;
    }
    unit_layout layout = {0};
    /* The layout was computed the same way by create, so it cannot fail. */
    if (compute_unit_layout(
            (unit_layout_request){
                .source_name_byte_count = unit->source_name.byte_count,
                .is_created = true},
            &layout) != SXML_STATUS_OK) {
        return;
    }
    /* create put the allocator after the unit and its source name. */
    const void *unit_memory = unit;
    const uint8_t *block_bytes = unit_memory;
    sxml_allocator creating_allocator = {0};
    memcpy(&creating_allocator, block_bytes + layout.creating_allocator_offset,
           sizeof(creating_allocator));
    sxml_ir_unit_finalize(unit);
    sxml_allocator_release(creating_allocator, unit);
}

void sxml_ir_unit_retain(sxml_ir_unit *unit) {
    if (unit != NULL && unit->reference_count < UINT32_MAX) {
        unit->reference_count += 1;
    }
}

void sxml_ir_unit_release(sxml_ir_unit *unit) {
    /* A count that reached UINT32_MAX stays there: the unit is immortal,
       as cells are (D33). */
    if (unit == NULL || unit->reference_count == UINT32_MAX) {
        return;
    }
    unit->reference_count -= 1;
    if (unit->reference_count > 0) {
        return;
    }
    if (unit->is_created) {
        sxml_ir_unit_destroy(unit);
    } else {
        sxml_ir_unit_finalize(unit);
    }
}

void sxml_ir_unit_seal(sxml_ir_unit *unit) { unit->is_sealed = true; }

/* --------------------------------------------------------------- Arrays */

/* One array of a unit to make room in. */
typedef struct array_room_request {
    void *elements;
    uint32_t element_count;
    uint32_t element_capacity;
    size_t element_size;
    size_t element_alignment;
    /* Entries to add after the count; the reserved entry 0 is added too
       when the array is still empty. */
    uint32_t added_count;
} array_room_request;

/* The array after making room: elements is unchanged when it had room. */
typedef struct array_room {
    void *elements;
    uint32_t element_count;
    uint32_t element_capacity;
} array_room;

/* The most entries any array of a unit may hold (UINT32_MAX - 1 entries,
   plus the reserved entry 0). */
#define UNIT_ARRAY_MAXIMUM_COUNT ((uint32_t)UINT32_MAX)

static sxml_status make_array_room(sxml_ir_unit *unit,
                                   const array_room_request *request,
                                   array_room *room) {
    if (unit->is_sealed) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    /* An empty array gets its reserved, all-zero entry 0 first. */
    const uint32_t current_count =
        request->element_count == 0 ? 1U : request->element_count;
    if (request->added_count > UNIT_ARRAY_MAXIMUM_COUNT - current_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    const uint32_t needed_count = current_count + request->added_count;
    if (needed_count <= request->element_capacity) {
        *room = (array_room){.elements = request->elements,
                             .element_count = current_count,
                             .element_capacity = request->element_capacity};
        return SXML_STATUS_OK;
    }
    /* Grow geometrically so that adding n entries costs O(n) copies. */
    uint32_t new_capacity =
        request->element_capacity < 8U ? 8U : request->element_capacity;
    while (new_capacity < needed_count) {
        new_capacity = new_capacity > UNIT_ARRAY_MAXIMUM_COUNT / 2U
                           ? UNIT_ARRAY_MAXIMUM_COUNT
                           : new_capacity * 2U;
    }
    size_t byte_count = 0;
    sxml_status status =
        sxml_size_multiply(new_capacity, request->element_size, &byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *new_elements = NULL;
    status = sxml_allocator_allocate(
        unit->allocator,
        (sxml_allocation_request){.byte_count = byte_count,
                                  .alignment = request->element_alignment},
        &new_elements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (request->element_count > 0) {
        memcpy(new_elements, request->elements,
               (size_t)request->element_count * request->element_size);
    } else {
        memset(new_elements, 0, request->element_size);
    }
    sxml_allocator_release(unit->allocator, request->elements);
    *room = (array_room){.elements = new_elements,
                         .element_count = current_count,
                         .element_capacity = new_capacity};
    return SXML_STATUS_OK;
}

sxml_status sxml_ir_unit_add_expression(sxml_ir_unit *unit,
                                        const sxml_ir_expression *expression,
                                        sxml_ir_expression_index *index) {
    if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_NONE ||
        expression->expression_kind >= SXML_IR_EXPRESSION_KIND_COUNT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const uint32_t expression_count =
        unit->expression_count == 0 ? 0U : unit->expression_count - 1U;
    if (unit->maximum_expression_count != 0 &&
        expression_count >= unit->maximum_expression_count) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    array_room room = {0};
    sxml_status status = make_array_room(
        unit,
        &(array_room_request){.elements = unit->expressions,
                              .element_count = unit->expression_count,
                              .element_capacity = unit->expression_capacity,
                              .element_size = sizeof(sxml_ir_expression),
                              .element_alignment = alignof(sxml_ir_expression),
                              .added_count = 1},
        &room);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    unit->expressions = room.elements;
    unit->expression_capacity = room.element_capacity;
    unit->expressions[room.element_count] = *expression;
    unit->expression_count = room.element_count + 1U;
    *index = (sxml_ir_expression_index){.value = room.element_count};
    return SXML_STATUS_OK;
}

sxml_status sxml_ir_unit_replace_expression(
    sxml_ir_unit *unit, const sxml_ir_expression_replacement *replacement) {
    if (unit->is_sealed || replacement->index.value == 0 ||
        replacement->index.value >= unit->expression_count ||
        replacement->expression.expression_kind ==
            SXML_IR_EXPRESSION_KIND_NONE ||
        replacement->expression.expression_kind >=
            SXML_IR_EXPRESSION_KIND_COUNT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    unit->expressions[replacement->index.value] = replacement->expression;
    return SXML_STATUS_OK;
}

sxml_status sxml_ir_unit_add_atoms(sxml_ir_unit *unit, sxml_ir_atom_view atoms,
                                   sxml_ir_atom_list *list) {
    if (atoms.atom_count == 0) {
        if (unit->is_sealed) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        *list = (sxml_ir_atom_list){.first_atom_index = 0, .atom_count = 0};
        return SXML_STATUS_OK;
    }
    if (atoms.atom_count > UNIT_ARRAY_MAXIMUM_COUNT) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    array_room room = {0};
    sxml_status status = make_array_room(
        unit,
        &(array_room_request){.elements = unit->atoms,
                              .element_count = unit->atom_count,
                              .element_capacity = unit->atom_capacity,
                              .element_size = sizeof(sxml_ir_atom),
                              .element_alignment = alignof(sxml_ir_atom),
                              .added_count = (uint32_t)atoms.atom_count},
        &room);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    unit->atoms = room.elements;
    unit->atom_capacity = room.element_capacity;
    memcpy(unit->atoms + room.element_count, atoms.atoms,
           atoms.atom_count * sizeof(sxml_ir_atom));
    unit->atom_count = room.element_count + (uint32_t)atoms.atom_count;
    *list = (sxml_ir_atom_list){.first_atom_index = room.element_count,
                                .atom_count = (uint32_t)atoms.atom_count};
    return SXML_STATUS_OK;
}

sxml_status sxml_ir_unit_add_map_entries(sxml_ir_unit *unit,
                                         sxml_ir_map_entry_view map_entries,
                                         sxml_ir_map_entry_list *list) {
    if (map_entries.map_entry_count == 0) {
        if (unit->is_sealed) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        *list = (sxml_ir_map_entry_list){.first_map_entry_index = 0,
                                         .map_entry_count = 0};
        return SXML_STATUS_OK;
    }
    if (map_entries.map_entry_count > UNIT_ARRAY_MAXIMUM_COUNT) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    array_room room = {0};
    sxml_status status = make_array_room(
        unit,
        &(array_room_request){
            .elements = unit->map_entries,
            .element_count = unit->map_entry_count,
            .element_capacity = unit->map_entry_capacity,
            .element_size = sizeof(sxml_ir_map_entry),
            .element_alignment = alignof(sxml_ir_map_entry),
            .added_count = (uint32_t)map_entries.map_entry_count},
        &room);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    unit->map_entries = room.elements;
    unit->map_entry_capacity = room.element_capacity;
    memcpy(unit->map_entries + room.element_count, map_entries.map_entries,
           map_entries.map_entry_count * sizeof(sxml_ir_map_entry));
    unit->map_entry_count =
        room.element_count + (uint32_t)map_entries.map_entry_count;
    *list = (sxml_ir_map_entry_list){
        .first_map_entry_index = room.element_count,
        .map_entry_count = (uint32_t)map_entries.map_entry_count};
    return SXML_STATUS_OK;
}

sxml_status sxml_ir_unit_add_sort_keys(sxml_ir_unit *unit,
                                       sxml_ir_sort_key_view sort_keys,
                                       sxml_ir_sort_key_list *list) {
    if (sort_keys.sort_key_count == 0) {
        if (unit->is_sealed) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        *list = (sxml_ir_sort_key_list){.first_sort_key_index = 0,
                                        .sort_key_count = 0};
        return SXML_STATUS_OK;
    }
    if (sort_keys.sort_key_count > UNIT_ARRAY_MAXIMUM_COUNT) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    array_room room = {0};
    sxml_status status = make_array_room(
        unit,
        &(array_room_request){.elements = unit->sort_keys,
                              .element_count = unit->sort_key_count,
                              .element_capacity = unit->sort_key_capacity,
                              .element_size = sizeof(sxml_ir_sort_key),
                              .element_alignment = alignof(sxml_ir_sort_key),
                              .added_count =
                                  (uint32_t)sort_keys.sort_key_count},
        &room);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    unit->sort_keys = room.elements;
    unit->sort_key_capacity = room.element_capacity;
    memcpy(unit->sort_keys + room.element_count, sort_keys.sort_keys,
           sort_keys.sort_key_count * sizeof(sxml_ir_sort_key));
    unit->sort_key_count =
        room.element_count + (uint32_t)sort_keys.sort_key_count;
    *list = (sxml_ir_sort_key_list){
        .first_sort_key_index = room.element_count,
        .sort_key_count = (uint32_t)sort_keys.sort_key_count};
    return SXML_STATUS_OK;
}

sxml_status sxml_ir_unit_set_static_namespaces(sxml_ir_unit *unit,
                                               sxml_namespace_binding_view bindings) {
    if (unit->static_namespaces.namespace_binding_count != 0) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (bindings.namespace_binding_count == 0) {
        return SXML_STATUS_OK;
    }
    if (bindings.namespace_binding_count > UNIT_ARRAY_MAXIMUM_COUNT) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    array_room room = {0};
    sxml_status status = make_array_room(
        unit,
        &(array_room_request){.elements = unit->namespace_bindings,
                              .element_count = unit->namespace_binding_count,
                              .element_capacity = unit->namespace_binding_capacity,
                              .element_size = sizeof(sxml_namespace_binding),
                              .element_alignment = alignof(sxml_namespace_binding),
                              .added_count = (uint32_t)bindings.namespace_binding_count},
        &room);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    unit->namespace_bindings = room.elements;
    unit->namespace_binding_capacity = room.element_capacity;
    memcpy(unit->namespace_bindings + room.element_count, bindings.namespace_bindings,
           bindings.namespace_binding_count * sizeof(sxml_namespace_binding));
    unit->namespace_binding_count =
        room.element_count + (uint32_t)bindings.namespace_binding_count;
    unit->static_namespaces = (sxml_ir_namespace_binding_list){
        .first_namespace_binding_index = room.element_count,
        .namespace_binding_count = (uint32_t)bindings.namespace_binding_count};
    return SXML_STATUS_OK;
}

sxml_status sxml_ir_unit_add_constant(sxml_ir_unit *unit, sxml_sequence value,
                                      sxml_ir_constant_index *index) {
    array_room room = {0};
    sxml_status status = make_array_room(
        unit,
        &(array_room_request){.elements = unit->constants,
                              .element_count = unit->constant_count,
                              .element_capacity = unit->constant_capacity,
                              .element_size = sizeof(sxml_sequence),
                              .element_alignment = alignof(sxml_sequence),
                              .added_count = 1},
        &room);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    unit->constants = room.elements;
    unit->constant_capacity = room.element_capacity;
    sxml_sequence_retain(value);
    unit->constants[room.element_count] = value;
    unit->constant_count = room.element_count + 1U;
    *index = (sxml_ir_constant_index){.value = room.element_count};
    return SXML_STATUS_OK;
}

sxml_status sxml_ir_unit_add_function(sxml_ir_unit *unit,
                                      const sxml_ir_function *function,
                                      sxml_ir_function_index *index) {
    array_room room = {0};
    sxml_status status = make_array_room(
        unit,
        &(array_room_request){.elements = unit->functions,
                              .element_count = unit->function_count,
                              .element_capacity = unit->function_capacity,
                              .element_size = sizeof(sxml_ir_function),
                              .element_alignment = alignof(sxml_ir_function),
                              .added_count = 1},
        &room);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    unit->functions = room.elements;
    unit->function_capacity = room.element_capacity;
    unit->functions[room.element_count] = *function;
    unit->functions[room.element_count].unit = unit;
    unit->function_count = room.element_count + 1U;
    *index = (sxml_ir_function_index){.value = room.element_count};
    return SXML_STATUS_OK;
}

sxml_status sxml_ir_unit_replace_function(
    sxml_ir_unit *unit, const sxml_ir_function_replacement *replacement) {
    if (unit->is_sealed || replacement->index.value == 0 ||
        replacement->index.value >= unit->function_count) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    unit->functions[replacement->index.value] = replacement->function;
    unit->functions[replacement->index.value].unit = unit;
    return SXML_STATUS_OK;
}

sxml_status sxml_ir_unit_add_global_variable(
    sxml_ir_unit *unit, const sxml_ir_global_variable *global_variable,
    sxml_ir_global_index *index) {
    array_room room = {0};
    sxml_status status = make_array_room(
        unit,
        &(array_room_request){
            .elements = unit->global_variables,
            .element_count = unit->global_variable_count,
            .element_capacity = unit->global_variable_capacity,
            .element_size = sizeof(sxml_ir_global_variable),
            .element_alignment = alignof(sxml_ir_global_variable),
            .added_count = 1},
        &room);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    unit->global_variables = room.elements;
    unit->global_variable_capacity = room.element_capacity;
    unit->global_variables[room.element_count] = *global_variable;
    unit->global_variable_count = room.element_count + 1U;
    *index = (sxml_ir_global_index){.value = room.element_count};
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------------ Text form */

/*
 * The text form (src/ir_internal.h, "Text form"). Layout rules, so golden
 * files can be written by hand:
 * - each function starts a line "(function INDEX NAME (parameters N)
 *   (captures ATOM...) (slots N)" with " (focus V P S)" when it binds a
 *   focus, then its body on the next line, indented by two;
 * - an expression is "(kind inline-operands...)"; each nested expression
 *   operand starts a new line indented two columns past the "(" of its
 *   parent, except the bound expression of a LET, which follows
 *   "(let $N " on the same line;
 * - an ATOM expression is its atom: a slot $N, a global $global:N, a
 *   constant as its value.
 * The printer keeps an explicit stack of pending work, so long LET chains
 * (one per operator of the source) cost no C stack.
 */

typedef struct text_writer {
    sxml_memory_view output;
    /* Bytes the whole text needs; may exceed the output's byte count. */
    size_t byte_count;
    /* Bytes since the last newline: the column of the next byte. */
    size_t column;
    const sxml_ir_unit *unit;
    sxml_name_pool *name_pool;
    sxml_type_registry *type_registry;
    /* The first failure that is not a full output (a heap failure while
       reading a decimal's text); the text goes on without it. */
    sxml_status status;
} text_writer;

static void write_bytes(text_writer *writer, sxml_byte_view bytes) {
    for (size_t byte_index = 0; byte_index < bytes.byte_count;
         byte_index += 1) {
        const uint8_t byte = bytes.bytes[byte_index];
        if (writer->byte_count < writer->output.byte_count) {
            writer->output.bytes[writer->byte_count] = byte;
        }
        writer->byte_count += 1;
        writer->column = byte == '\n' ? 0 : writer->column + 1;
    }
}

// ast-grep-ignore: pointer-parameter-without-length
static void write_text(text_writer *writer, const char *text) {
    write_bytes(writer, sxml_byte_view_from_c_string(text));
}

static void write_unsigned(text_writer *writer, uint64_t number) {
    uint8_t digits[20];
    size_t digit_count = 0;
    do {
        digits[sizeof(digits) - 1 - digit_count] =
            (uint8_t)('0' + (uint8_t)(number % 10U));
        number /= 10U;
        digit_count += 1;
    } while (number != 0);
    write_bytes(writer,
                (sxml_byte_view){.bytes = digits + sizeof(digits) - digit_count,
                                 .byte_count = digit_count});
}

static void write_signed(text_writer *writer, int64_t number) {
    if (number < 0) {
        write_text(writer, "-");
        /* Negating INT64_MIN overflows; go through the unsigned value. */
        write_unsigned(writer, (uint64_t)0 - (uint64_t)number);
        return;
    }
    write_unsigned(writer, (uint64_t)number);
}

static void write_newline(text_writer *writer, size_t indent) {
    write_text(writer, "\n");
    for (size_t space_index = 0; space_index < indent; space_index += 1) {
        write_text(writer, " ");
    }
}

/* A string as an XPath string literal: in double quotes, quotes doubled
   (XPath 4.0, A.3.1, StringLiteral). */
static void write_string_literal(text_writer *writer, sxml_byte_view text) {
    write_text(writer, "\"");
    for (size_t byte_index = 0; byte_index < text.byte_count;
         byte_index += 1) {
        if (text.bytes[byte_index] == '"') {
            write_text(writer, "\"");
        }
        write_bytes(writer, (sxml_byte_view){.bytes = text.bytes + byte_index,
                                             .byte_count = 1});
    }
    write_text(writer, "\"");
}

/* The usual prefix of a well-known namespace, or none. */
static const char *well_known_prefix(sxml_namespace_uri_identifier uri) {
    switch (uri.index) {
    case SXML_NAMESPACE_XML:
        return "xml";
    case SXML_NAMESPACE_XML_SCHEMA:
        return "xs";
    case SXML_NAMESPACE_XML_SCHEMA_INSTANCE:
        return "xsi";
    case SXML_NAMESPACE_FUNCTIONS:
        return "fn";
    case SXML_NAMESPACE_MAP_FUNCTIONS:
        return "map";
    case SXML_NAMESPACE_ARRAY_FUNCTIONS:
        return "array";
    case SXML_NAMESPACE_MATH_FUNCTIONS:
        return "math";
    case SXML_NAMESPACE_ERRORS:
        return "err";
    default:
        return NULL;
    }
}

/* An expanded name: prefix:local for the predeclared namespaces, local
   in no namespace, Q{uri}local otherwise. */
static void write_name(text_writer *writer, sxml_name_identifier name) {
    sxml_name_parts parts = {0};
    if (sxml_name_pool_get_name_parts(writer->name_pool, name, &parts) !=
        SXML_STATUS_OK) {
        write_text(writer, "Q{}?");
        return;
    }
    const char *prefix = well_known_prefix(parts.namespace_uri);
    if (prefix != NULL) {
        write_text(writer, prefix);
        write_text(writer, ":");
    } else if (parts.namespace_uri.index != SXML_NAMESPACE_NONE) {
        sxml_byte_view uri = {0};
        if (sxml_name_pool_get_namespace_uri(
                writer->name_pool, parts.namespace_uri, &uri) !=
            SXML_STATUS_OK) {
            uri = (sxml_byte_view){0};
        }
        write_text(writer, "Q{");
        write_bytes(writer, uri);
        write_text(writer, "}");
    }
    write_bytes(writer, parts.local_name);
}

static void write_namespace_uri(text_writer *writer,
                                sxml_namespace_uri_identifier uri) {
    sxml_byte_view uri_text = {0};
    if (sxml_name_pool_get_namespace_uri(writer->name_pool, uri, &uri_text) !=
        SXML_STATUS_OK) {
        uri_text = (sxml_byte_view){0};
    }
    write_bytes(writer, uri_text);
}

/* Deepest nesting of item types printed; deeper parts print as "...". */
#define TEXT_TYPE_DEPTH_LIMIT 64U

static const char *occurrence_text(sxml_occurrence occurrence) {
    switch (occurrence) {
    case SXML_OCCURRENCE_ZERO:
    case SXML_OCCURRENCE_EXACTLY_ONE:
        break;
    case SXML_OCCURRENCE_ZERO_OR_ONE:
        return "?";
    case SXML_OCCURRENCE_ONE_OR_MORE:
        return "+";
    case SXML_OCCURRENCE_ZERO_OR_MORE:
        return "*";
    }
    return "";
}

static void write_name_test(text_writer *writer, sxml_name_test name_test) {
    switch (name_test.name_test_kind) {
    case SXML_NAME_TEST_KIND_ANY_NAME:
        write_text(writer, "*");
        break;
    case SXML_NAME_TEST_KIND_EXPANDED_NAME:
        write_name(writer, name_test.expanded_name);
        break;
    case SXML_NAME_TEST_KIND_NAMESPACE_WILDCARD:
        write_text(writer, "Q{");
        write_namespace_uri(writer, name_test.namespace_uri);
        write_text(writer, "}*");
        break;
    case SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD:
        write_text(writer, "*:");
        write_bytes(writer, name_test.local_name);
        break;
    }
}

/*
 * Item types nest (map(K, array(map(...)))). Without C recursion (D71)
 * they are written with a stack of frames, one per registered item type
 * being written, at most TEXT_TYPE_DEPTH_LIMIT: a frame writes its text
 * in steps, and a step that reaches a part pushes a frame for it (or
 * writes it at once when it is a built-in type); the frame continues
 * after the part's text when it is on top again.
 */
typedef struct type_text_frame {
    sxml_type_identifier type;
    /* Which step comes next; 0 for the first. */
    uint32_t step;
    bool is_finished;
    /* Written after the type: an occurrence indicator, or "?" after the
       type annotation of a nillable element test. */
    const char *suffix;
} type_text_frame;

typedef struct type_text_stack {
    type_text_frame frames[TEXT_TYPE_DEPTH_LIMIT];
    uint32_t frame_count;
} type_text_stack;

/* An item type to write, then its suffix. */
typedef struct type_text_part {
    sxml_type_identifier type;
    const char *suffix;
} type_text_part;

/* Writes a built-in type, "..." beyond the depth limit or an unknown
   type now; pushes a frame for any other registered type. */
static void begin_item_type_text(text_writer *writer, type_text_stack *stack,
                                 type_text_part part) {
    static const char *const generic_names[] = {
        [SXML_TYPE_ITEM - SXML_TYPE_ITEM] = "item()",
        [SXML_TYPE_GENERIC_NODE - SXML_TYPE_ITEM] = "node()",
        [SXML_TYPE_NODE - SXML_TYPE_ITEM] = "xnode()",
        [SXML_TYPE_DOCUMENT_NODE - SXML_TYPE_ITEM] = "document-node()",
        [SXML_TYPE_ELEMENT_NODE - SXML_TYPE_ITEM] = "element()",
        [SXML_TYPE_ATTRIBUTE_NODE - SXML_TYPE_ITEM] = "attribute()",
        [SXML_TYPE_TEXT_NODE - SXML_TYPE_ITEM] = "text()",
        [SXML_TYPE_COMMENT_NODE - SXML_TYPE_ITEM] = "comment()",
        [SXML_TYPE_PROCESSING_INSTRUCTION_NODE - SXML_TYPE_ITEM] =
            "processing-instruction()",
        [SXML_TYPE_NAMESPACE_NODE - SXML_TYPE_ITEM] = "namespace-node()",
        [SXML_TYPE_JSON_NODE - SXML_TYPE_ITEM] = "jnode()",
        [SXML_TYPE_FUNCTION - SXML_TYPE_ITEM] = "function(*)",
        [SXML_TYPE_MAP - SXML_TYPE_ITEM] = "map(*)",
        [SXML_TYPE_ARRAY - SXML_TYPE_ITEM] = "array(*)",
        [SXML_TYPE_RECORD - SXML_TYPE_ITEM] = "record(*)",
    };
    const uint32_t index = part.type.index;
    if (index >= SXML_TYPE_ITEM && index < SXML_TYPE_BUILTIN_COUNT) {
        write_text(writer, generic_names[index - SXML_TYPE_ITEM]);
    } else if (index > SXML_TYPE_NONE && index < SXML_TYPE_ITEM) {
        write_text(writer, "xs:");
        write_bytes(writer,
                    sxml_type_builtin_local_name((sxml_builtin_type)index));
    } else if (stack->frame_count >= TEXT_TYPE_DEPTH_LIMIT) {
        write_text(writer, "...");
    } else {
        stack->frames[stack->frame_count] =
            (type_text_frame){.type = part.type, .suffix = part.suffix};
        stack->frame_count += 1;
        return;
    }
    write_text(writer, part.suffix);
}

static void begin_sequence_type_text(text_writer *writer,
                                     type_text_stack *stack,
                                     sxml_sequence_type sequence_type) {
    if (sequence_type.occurrence == SXML_OCCURRENCE_ZERO) {
        write_text(writer, "empty-sequence()");
        return;
    }
    begin_item_type_text(
        writer, stack,
        (type_text_part){.type = sequence_type.item_type,
                         .suffix = occurrence_text(sequence_type.occurrence)});
}

/* A step of a frame: its type's description and the stack. */
typedef struct type_text_step {
    type_text_stack *stack;
    type_text_frame *frame;
    const sxml_type_description *description;
} type_text_step;

static void continue_node_test_text(text_writer *writer, type_text_step step) {
    static const char *const kind_names[] = {
        [SXML_NODE_KIND_DOCUMENT] = "document-node",
        [SXML_NODE_KIND_ELEMENT] = "element",
        [SXML_NODE_KIND_ATTRIBUTE] = "attribute",
        [SXML_NODE_KIND_TEXT] = "text",
        [SXML_NODE_KIND_COMMENT] = "comment",
        [SXML_NODE_KIND_PROCESSING_INSTRUCTION] = "processing-instruction",
        [SXML_NODE_KIND_NAMESPACE] = "namespace-node",
        [SXML_NODE_KIND_ANY] = "xnode",
    };
    const sxml_node_test *node_test =
        &step.description->variety_details.node_test;
    if (step.frame->step == 1) {
        write_text(writer, ")");
        step.frame->is_finished = true;
        return;
    }
    step.frame->step = 1;
    if (node_test->is_schema_declaration) {
        write_text(writer, node_test->node_kind == SXML_NODE_KIND_ELEMENT
                               ? "schema-element"
                               : "schema-attribute");
    } else {
        write_text(writer, kind_names[node_test->node_kind]);
    }
    write_text(writer, "(");
    switch (node_test->node_kind) {
    case SXML_NODE_KIND_DOCUMENT:
        if (node_test->document_element_test.index != SXML_TYPE_NONE) {
            begin_item_type_text(
                writer, step.stack,
                (type_text_part){.type = node_test->document_element_test,
                                 .suffix = ""});
        }
        break;
    case SXML_NODE_KIND_ELEMENT:
    case SXML_NODE_KIND_ATTRIBUTE:
        if (node_test->name_test.name_test_kind !=
                SXML_NAME_TEST_KIND_ANY_NAME ||
            node_test->type_annotation.index != SXML_TYPE_NONE) {
            write_name_test(writer, node_test->name_test);
        }
        if (node_test->type_annotation.index != SXML_TYPE_NONE) {
            write_text(writer, ", ");
            begin_item_type_text(
                writer, step.stack,
                (type_text_part){.type = node_test->type_annotation,
                                 .suffix = node_test->allows_nilled ? "?" : ""});
        }
        break;
    case SXML_NODE_KIND_PROCESSING_INSTRUCTION:
    case SXML_NODE_KIND_NAMESPACE:
        if (node_test->name_test.name_test_kind ==
            SXML_NAME_TEST_KIND_EXPANDED_NAME) {
            write_name(writer, node_test->name_test.expanded_name);
        }
        break;
    case SXML_NODE_KIND_TEXT:
    case SXML_NODE_KIND_COMMENT:
    case SXML_NODE_KIND_ANY:
        break;
    }
}

static void continue_json_node_test_text(text_writer *writer,
                                         type_text_step step) {
    const sxml_json_node_test *test =
        &step.description->variety_details.json_node_test;
    if (step.frame->step == 1) {
        write_text(writer, ")");
        step.frame->is_finished = true;
        return;
    }
    step.frame->step = 1;
    write_text(writer, "jnode(");
    switch (test->key_constraint) {
    case SXML_JSON_KEY_CONSTRAINT_ANY:
        write_text(writer, "*");
        break;
    case SXML_JSON_KEY_CONSTRAINT_ROOT:
        write_text(writer, "()");
        break;
    case SXML_JSON_KEY_CONSTRAINT_CONSTANT:
        write_string_literal(writer, test->key_lexical_form);
        break;
    }
    if (test->has_value_type) {
        write_text(writer, ", ");
        begin_sequence_type_text(writer, step.stack, test->value_type);
    }
}

/* function(P1, P2) as R: step k < n writes parameter k, step n the
   result type. */
static void continue_function_type_text(text_writer *writer,
                                        type_text_step step) {
    const sxml_function_type *function_type =
        &step.description->variety_details.function_type;
    const size_t parameter_count =
        function_type->parameter_types.sequence_type_count;
    const size_t parameter_index = step.frame->step;
    step.frame->step += 1;
    if (parameter_index == 0) {
        write_text(writer, "function(");
    }
    if (parameter_index < parameter_count) {
        if (parameter_index > 0) {
            write_text(writer, ", ");
        }
        begin_sequence_type_text(
            writer, step.stack,
            function_type->parameter_types.sequence_types[parameter_index]);
    } else if (parameter_index == parameter_count) {
        write_text(writer, ") as ");
        begin_sequence_type_text(writer, step.stack, function_type->result_type);
    } else {
        step.frame->is_finished = true;
    }
}

static void continue_map_type_text(text_writer *writer, type_text_step step) {
    const sxml_map_type *map_type = &step.description->variety_details.map_type;
    const uint32_t step_index = step.frame->step;
    step.frame->step += 1;
    if (step_index == 0) {
        write_text(writer, "map(");
        begin_item_type_text(
            writer, step.stack,
            (type_text_part){.type = map_type->key_type, .suffix = ""});
    } else if (step_index == 1) {
        write_text(writer, ", ");
        begin_sequence_type_text(writer, step.stack, map_type->value_type);
    } else {
        write_text(writer, ")");
        step.frame->is_finished = true;
    }
}

/* record(N as T, ...): step k < n writes field k. */
static void continue_record_type_text(text_writer *writer,
                                      type_text_step step) {
    if (step.description->type_name.index != 0) {
        write_name(writer, step.description->type_name);
        step.frame->is_finished = true;
        return;
    }
    const sxml_record_field_view fields =
        step.description->variety_details.record_type.record_fields;
    const size_t field_index = step.frame->step;
    step.frame->step += 1;
    if (field_index == 0) {
        write_text(writer, "record(");
    }
    if (field_index >= fields.record_field_count) {
        write_text(writer, ")");
        step.frame->is_finished = true;
        return;
    }
    if (field_index > 0) {
        write_text(writer, ", ");
    }
    write_string_literal(writer, fields.record_fields[field_index].field_name);
    write_text(writer, " as ");
    begin_sequence_type_text(writer, step.stack,
                             fields.record_fields[field_index].field_type);
}

/* (M1 | M2 | ...): step k < n writes member k. */
static void continue_choice_type_text(text_writer *writer,
                                      type_text_step step) {
    const sxml_type_identifier_view members =
        step.description->variety_details.member_types;
    const size_t member_index = step.frame->step;
    step.frame->step += 1;
    if (member_index == 0) {
        write_text(writer, "(");
    }
    if (member_index >= members.type_identifier_count) {
        write_text(writer, ")");
        step.frame->is_finished = true;
        return;
    }
    if (member_index > 0) {
        write_text(writer, " | ");
    }
    begin_item_type_text(
        writer, step.stack,
        (type_text_part){.type = members.type_identifiers[member_index],
                         .suffix = ""});
}

/* Writes the next part of a registered item type in XPath 4.0 syntax
   (section 3). */
static void continue_type_text(text_writer *writer, type_text_step step) {
    const sxml_type_description *description = step.description;
    switch (description->variety) {
    case SXML_TYPE_VARIETY_NODE_TEST:
        continue_node_test_text(writer, step);
        return;
    case SXML_TYPE_VARIETY_JSON_NODE_TEST:
        continue_json_node_test_text(writer, step);
        return;
    case SXML_TYPE_VARIETY_FUNCTION:
        continue_function_type_text(writer, step);
        return;
    case SXML_TYPE_VARIETY_MAP:
        continue_map_type_text(writer, step);
        return;
    case SXML_TYPE_VARIETY_ARRAY:
        if (step.frame->step == 0) {
            step.frame->step = 1;
            write_text(writer, "array(");
            begin_sequence_type_text(
                writer, step.stack,
                description->variety_details.array_type.member_type);
        } else {
            write_text(writer, ")");
            step.frame->is_finished = true;
        }
        return;
    case SXML_TYPE_VARIETY_RECORD:
        continue_record_type_text(writer, step);
        return;
    case SXML_TYPE_VARIETY_ENUMERATION: {
        const sxml_byte_view_list values =
            description->variety_details.enumeration_values;
        write_text(writer, "enum(");
        for (size_t value_index = 0; value_index < values.byte_view_count;
             value_index += 1) {
            if (value_index > 0) {
                write_text(writer, ", ");
            }
            write_string_literal(writer, values.byte_views[value_index]);
        }
        write_text(writer, ")");
        step.frame->is_finished = true;
        return;
    }
    case SXML_TYPE_VARIETY_CHOICE:
        continue_choice_type_text(writer, step);
        return;
    case SXML_TYPE_VARIETY_COMPLEX:
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_ATOMIC:
    case SXML_TYPE_VARIETY_LIST:
    case SXML_TYPE_VARIETY_UNION:
    case SXML_TYPE_VARIETY_ANY_ITEM:
        break;
    }
    if (description->type_name.index != 0) {
        write_name(writer, description->type_name);
    } else {
        write_text(writer, "anonymous-type()");
    }
    step.frame->is_finished = true;
}

/* Runs the frames until the stack is empty. */
static void run_type_text(text_writer *writer, type_text_stack *stack) {
    while (stack->frame_count > 0) {
        type_text_frame *frame = &stack->frames[stack->frame_count - 1];
        if (frame->is_finished) {
            write_text(writer, frame->suffix);
            stack->frame_count -= 1;
            continue;
        }
        sxml_type_description description = {0};
        if (sxml_type_registry_describe_type(writer->type_registry, frame->type,
                                             &description) != SXML_STATUS_OK) {
            write_text(writer, "unknown-type(");
            write_unsigned(writer, frame->type.index);
            write_text(writer, ")");
            frame->is_finished = true;
            continue;
        }
        continue_type_text(writer, (type_text_step){.stack = stack,
                                                    .frame = frame,
                                                    .description = &description});
    }
}

static void write_item_type(text_writer *writer, sxml_type_identifier type) {
    type_text_stack stack = {.frame_count = 0};
    begin_item_type_text(writer, &stack,
                         (type_text_part){.type = type, .suffix = ""});
    run_type_text(writer, &stack);
}

static void write_sequence_type(text_writer *writer,
                                sxml_sequence_type sequence_type) {
    type_text_stack stack = {.frame_count = 0};
    begin_sequence_type_text(writer, &stack, sequence_type);
    run_type_text(writer, &stack);
}

/* An atomic item as an XPath expression that makes it. */
static void write_atomic_item(text_writer *writer, sxml_item item) {
    sxml_atomic_datum datum = {0};
    sxml_status status = sxml_atomic_item_get_datum(item, &datum);
    if (status != SXML_STATUS_OK) {
        if (writer->status == SXML_STATUS_OK) {
            writer->status = status;
        }
        write_text(writer, "?");
        return;
    }
    const sxml_type_identifier type = sxml_item_get_type_annotation(item);
    switch (datum.datum_kind) {
    case SXML_DATUM_KIND_BOOLEAN:
        write_text(writer, datum.datum_details.boolean ? "true()" : "false()");
        return;
    case SXML_DATUM_KIND_INTEGER:
        if (type.index == SXML_TYPE_XS_INTEGER) {
            write_signed(writer, datum.datum_details.integer);
            return;
        }
        break;
    case SXML_DATUM_KIND_DECIMAL:
        if (type.index == SXML_TYPE_XS_DECIMAL ||
            type.index == SXML_TYPE_XS_INTEGER) {
            write_bytes(writer, datum.datum_details.decimal_text);
            /* An integral xs:decimal keeps a "." to show its type. */
            if (type.index == SXML_TYPE_XS_DECIMAL &&
                memchr(datum.datum_details.decimal_text.bytes, '.',
                       datum.datum_details.decimal_text.byte_count) == NULL) {
                write_text(writer, ".0");
            }
            return;
        }
        break;
    case SXML_DATUM_KIND_STRING:
        if (type.index == SXML_TYPE_XS_STRING) {
            write_string_literal(writer, datum.datum_details.string.utf8);
            return;
        }
        break;
    case SXML_DATUM_KIND_QNAME:
        if (type.index == SXML_TYPE_XS_QNAME) {
            write_text(writer, "#");
            write_name(writer, datum.datum_details.qname.expanded_name);
            return;
        }
        break;
    case SXML_DATUM_KIND_DOUBLE:
    case SXML_DATUM_KIND_FLOAT:
    case SXML_DATUM_KIND_DATE_TIME:
    case SXML_DATUM_KIND_DURATION:
    case SXML_DATUM_KIND_BINARY:
        break;
    }
    if (datum.datum_kind == SXML_DATUM_KIND_DOUBLE &&
        datum.datum_details.double_number ==
            datum.datum_details.double_number &&
        datum.datum_details.double_number - datum.datum_details.double_number ==
            0.0) {
        /* A finite double: its shortest decimal with an exponent, a double
           literal (XPath 4.0, A.3.1, DoubleLiteral). */
        sxml_decimal decimal = {0};
        const sxml_decimal_configuration configuration = {
            .allocator = writer->unit->allocator,
            .maximum_digit_count = SXML_DECIMAL_DEFAULT_MAXIMUM_DIGIT_COUNT,
            .division_digit_count = SXML_DECIMAL_DEFAULT_DIVISION_DIGIT_COUNT,
        };
        status = sxml_decimal_from_double_shortest(
            &configuration, datum.datum_details.double_number, &decimal);
        if (status == SXML_STATUS_OK) {
            uint8_t digits[64];
            sxml_byte_view text = {0};
            status = sxml_decimal_write_canonical_text(
                &decimal,
                (sxml_memory_view){.bytes = digits, .byte_count = sizeof(digits)},
                &text);
            sxml_decimal_finalize(&decimal);
            if (status == SXML_STATUS_OK) {
                write_bytes(writer, text);
                write_text(writer, "e0");
                return;
            }
        }
        if (writer->status == SXML_STATUS_OK) {
            writer->status = status;
        }
        write_text(writer, "?");
        return;
    }
    /* Anything else as a constructor call on its type name. */
    write_item_type(writer, type);
    write_text(writer, "(...)");
}

static void write_item(text_writer *writer, sxml_item item) {
    switch (sxml_item_get_kind(item)) {
    case SXML_ITEM_KIND_ATOMIC:
        write_atomic_item(writer, item);
        return;
    case SXML_ITEM_KIND_NODE:
        write_text(writer, "node-item()");
        return;
    case SXML_ITEM_KIND_JSON_NODE:
        write_text(writer, "jnode-item()");
        return;
    case SXML_ITEM_KIND_FUNCTION:
        write_text(writer, "function-item()");
        return;
    case SXML_ITEM_KIND_MAP:
        write_text(writer, "map-item()");
        return;
    case SXML_ITEM_KIND_ARRAY:
        write_text(writer, "array-item()");
        return;
    }
}

static void write_constant(text_writer *writer, sxml_ir_constant_index index) {
    const sxml_sequence value = sxml_ir_unit_get_constant(writer->unit, index);
    uint64_t item_count = 0;
    sxml_status status = sxml_sequence_count(value, NULL, &item_count);
    if (status != SXML_STATUS_OK) {
        if (writer->status == SXML_STATUS_OK) {
            writer->status = status;
        }
        write_text(writer, "?");
        return;
    }
    if (item_count != 1) {
        write_text(writer, "(");
    }
    for (uint64_t item_index = 0; item_index < item_count; item_index += 1) {
        sxml_item item = {0};
        status = sxml_sequence_get_item(
            value,
            (sxml_item_request){.item_index = item_index, .error_record = NULL},
            &item);
        if (status != SXML_STATUS_OK) {
            if (writer->status == SXML_STATUS_OK) {
                writer->status = status;
            }
            break;
        }
        if (item_index > 0) {
            write_text(writer, ", ");
        }
        write_item(writer, item);
    }
    if (item_count != 1) {
        write_text(writer, ")");
    }
}

static void write_atom(text_writer *writer, sxml_ir_atom atom) {
    switch (atom.atom_kind) {
    case SXML_IR_ATOM_KIND_NONE:
        write_text(writer, "_");
        return;
    case SXML_IR_ATOM_KIND_CONSTANT:
        write_constant(writer, atom.constant);
        return;
    case SXML_IR_ATOM_KIND_SLOT:
        write_text(writer, "$");
        write_unsigned(writer, atom.slot.value);
        return;
    case SXML_IR_ATOM_KIND_GLOBAL_VARIABLE:
        write_text(writer, "$global:");
        write_unsigned(writer, atom.global_variable.value);
        return;
    }
    write_text(writer, "?");
}

static void write_atom_list(text_writer *writer, sxml_ir_atom_list list) {
    const sxml_ir_atom_view atoms = sxml_ir_unit_get_atoms(writer->unit, list);
    for (size_t atom_index = 0; atom_index < atoms.atom_count;
         atom_index += 1) {
        write_text(writer, " ");
        write_atom(writer, atoms.atoms[atom_index]);
    }
}

static void write_slot(text_writer *writer, sxml_ir_slot slot) {
    if (slot.value == 0) {
        write_text(writer, "_");
        return;
    }
    write_text(writer, "$");
    write_unsigned(writer, slot.value);
}

static void write_focus(text_writer *writer, sxml_ir_focus focus) {
    write_text(writer, "(focus ");
    write_slot(writer, focus.context_value);
    write_text(writer, " ");
    write_slot(writer, focus.context_position);
    write_text(writer, " ");
    write_slot(writer, focus.context_size);
    write_text(writer, ")");
}

static const char *const expression_kind_names[] = {
    [SXML_IR_EXPRESSION_KIND_NONE] = "none",
    [SXML_IR_EXPRESSION_KIND_ATOM] = "atom",
    [SXML_IR_EXPRESSION_KIND_LET] = "let",
    [SXML_IR_EXPRESSION_KIND_SEQUENCE] = "sequence",
    [SXML_IR_EXPRESSION_KIND_CONDITIONAL] = "conditional",
    [SXML_IR_EXPRESSION_KIND_BINARY] = "binary",
    [SXML_IR_EXPRESSION_KIND_UNARY] = "unary",
    [SXML_IR_EXPRESSION_KIND_FUNCTION_DEFINITION] = "function-definition",
    [SXML_IR_EXPRESSION_KIND_FUNCTION_REFERENCE] = "function-reference",
    [SXML_IR_EXPRESSION_KIND_STATIC_CALL] = "static-call",
    [SXML_IR_EXPRESSION_KIND_DYNAMIC_CALL] = "dynamic-call",
    [SXML_IR_EXPRESSION_KIND_LOOKUP] = "lookup",
    [SXML_IR_EXPRESSION_KIND_MAP] = "map",
    [SXML_IR_EXPRESSION_KIND_FILTER] = "filter",
    [SXML_IR_EXPRESSION_KIND_QUANTIFIED] = "quantified",
    [SXML_IR_EXPRESSION_KIND_SORT] = "sort",
    [SXML_IR_EXPRESSION_KIND_STEP] = "step",
    [SXML_IR_EXPRESSION_KIND_DOCUMENT_ORDER] = "document-order",
    [SXML_IR_EXPRESSION_KIND_CAST] = "cast",
    [SXML_IR_EXPRESSION_KIND_CASTABLE] = "castable",
    [SXML_IR_EXPRESSION_KIND_INSTANCE_OF] = "instance-of",
    [SXML_IR_EXPRESSION_KIND_TREAT] = "treat",
    [SXML_IR_EXPRESSION_KIND_COERCE] = "coerce",
    [SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR] = "map-constructor",
    [SXML_IR_EXPRESSION_KIND_ARRAY_CONSTRUCTOR] = "array-constructor",
    [SXML_IR_EXPRESSION_KIND_RAISE] = "raise",
    [SXML_IR_EXPRESSION_KIND_TRY_CATCH] = "try-catch",
    [SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR] = "document-constructor",
    [SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR] = "element-constructor",
    [SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR] = "attribute-constructor",
    [SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR] = "namespace-constructor",
    [SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR] = "text-constructor",
    [SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR] = "comment-constructor",
    [SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR] =
        "processing-instruction-constructor",
    [SXML_IR_EXPRESSION_KIND_SHALLOW_COPY] = "shallow-copy",
    [SXML_IR_EXPRESSION_KIND_APPLY_TEMPLATES] = "apply-templates",
};

static const char *binary_operator_name(sxml_ir_binary_operator operator) {
    switch (operator) {
    case SXML_IR_BINARY_OPERATOR_ADD:
        return "add";
    case SXML_IR_BINARY_OPERATOR_SUBTRACT:
        return "subtract";
    case SXML_IR_BINARY_OPERATOR_MULTIPLY:
        return "multiply";
    case SXML_IR_BINARY_OPERATOR_DIVIDE:
        return "divide";
    case SXML_IR_BINARY_OPERATOR_INTEGER_DIVIDE:
        return "integer-divide";
    case SXML_IR_BINARY_OPERATOR_MODULO:
        return "modulo";
    case SXML_IR_BINARY_OPERATOR_VALUE_EQUAL:
        return "value-equal";
    case SXML_IR_BINARY_OPERATOR_VALUE_NOT_EQUAL:
        return "value-not-equal";
    case SXML_IR_BINARY_OPERATOR_VALUE_LESS:
        return "value-less";
    case SXML_IR_BINARY_OPERATOR_VALUE_LESS_OR_EQUAL:
        return "value-less-or-equal";
    case SXML_IR_BINARY_OPERATOR_VALUE_GREATER:
        return "value-greater";
    case SXML_IR_BINARY_OPERATOR_VALUE_GREATER_OR_EQUAL:
        return "value-greater-or-equal";
    case SXML_IR_BINARY_OPERATOR_GENERAL_EQUAL:
        return "general-equal";
    case SXML_IR_BINARY_OPERATOR_GENERAL_NOT_EQUAL:
        return "general-not-equal";
    case SXML_IR_BINARY_OPERATOR_GENERAL_LESS:
        return "general-less";
    case SXML_IR_BINARY_OPERATOR_GENERAL_LESS_OR_EQUAL:
        return "general-less-or-equal";
    case SXML_IR_BINARY_OPERATOR_GENERAL_GREATER:
        return "general-greater";
    case SXML_IR_BINARY_OPERATOR_GENERAL_GREATER_OR_EQUAL:
        return "general-greater-or-equal";
    case SXML_IR_BINARY_OPERATOR_IS:
        return "is";
    case SXML_IR_BINARY_OPERATOR_IS_NOT:
        return "is-not";
    case SXML_IR_BINARY_OPERATOR_PRECEDES:
        return "precedes";
    case SXML_IR_BINARY_OPERATOR_FOLLOWS:
        return "follows";
    case SXML_IR_BINARY_OPERATOR_PRECEDES_OR_IS:
        return "precedes-or-is";
    case SXML_IR_BINARY_OPERATOR_FOLLOWS_OR_IS:
        return "follows-or-is";
    case SXML_IR_BINARY_OPERATOR_RANGE:
        return "range";
    case SXML_IR_BINARY_OPERATOR_UNION:
        return "union";
    case SXML_IR_BINARY_OPERATOR_INTERSECT:
        return "intersect";
    case SXML_IR_BINARY_OPERATOR_EXCEPT:
        return "except";
    case SXML_IR_BINARY_OPERATOR_CONCATENATE:
        return "concatenate";
    }
    return "?";
}

static const char *axis_name(sxml_axis axis) {
    switch (axis) {
    case SXML_AXIS_SELF:
        return "self";
    case SXML_AXIS_CHILD:
        return "child";
    case SXML_AXIS_DESCENDANT:
        return "descendant";
    case SXML_AXIS_DESCENDANT_OR_SELF:
        return "descendant-or-self";
    case SXML_AXIS_ATTRIBUTE:
        return "attribute";
    case SXML_AXIS_NAMESPACE:
        return "namespace";
    case SXML_AXIS_FOLLOWING_SIBLING:
        return "following-sibling";
    case SXML_AXIS_FOLLOWING_SIBLING_OR_SELF:
        return "following-sibling-or-self";
    case SXML_AXIS_FOLLOWING:
        return "following";
    case SXML_AXIS_FOLLOWING_OR_SELF:
        return "following-or-self";
    case SXML_AXIS_PARENT:
        return "parent";
    case SXML_AXIS_ANCESTOR:
        return "ancestor";
    case SXML_AXIS_ANCESTOR_OR_SELF:
        return "ancestor-or-self";
    case SXML_AXIS_PRECEDING_SIBLING:
        return "preceding-sibling";
    case SXML_AXIS_PRECEDING_SIBLING_OR_SELF:
        return "preceding-sibling-or-self";
    case SXML_AXIS_PRECEDING:
        return "preceding";
    case SXML_AXIS_PRECEDING_OR_SELF:
        return "preceding-or-self";
    }
    return "?";
}

static const char *coercion_role_name(sxml_ir_coercion_role role) {
    switch (role) {
    case SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT:
        return "function-argument";
    case SXML_IR_COERCION_ROLE_FUNCTION_RESULT:
        return "function-result";
    case SXML_IR_COERCION_ROLE_VARIABLE:
        return "variable";
    case SXML_IR_COERCION_ROLE_TEMPLATE_PARAMETER:
        return "template-parameter";
    }
    return "?";
}

static void write_function_target(text_writer *writer,
                                   sxml_ir_function_target target) {
    switch (target.target_kind) {
    case SXML_IR_FUNCTION_TARGET_KIND_NONE:
        write_text(writer, "none");
        return;
    case SXML_IR_FUNCTION_TARGET_KIND_NATIVE: {
        const char *prefix =
            target.native_function == NULL
                ? NULL
                : well_known_prefix(sxml_well_known_namespace_identifier(
                      target.native_function->function_namespace));
        write_text(writer, prefix == NULL ? "Q{}" : prefix);
        if (prefix != NULL) {
            write_text(writer, ":");
        }
        if (target.native_function != NULL) {
            write_bytes(writer, target.native_function->local_name);
        }
        return;
    }
    case SXML_IR_FUNCTION_TARGET_KIND_IR_FUNCTION:
        write_text(writer, "function:");
        write_unsigned(writer, target.function.value);
        return;
    case SXML_IR_FUNCTION_TARGET_KIND_CONSTRUCTOR:
        write_item_type(writer, target.constructor_type);
        return;
    }
}

static void write_raise(text_writer *writer, const sxml_ir_raise *raise) {
    write_text(writer, " #");
    if (raise->error_code.namespace_prefix.byte_count > 0) {
        write_bytes(writer, raise->error_code.namespace_prefix);
        write_text(writer, ":");
    } else if (raise->error_code.namespace_uri.byte_count > 0) {
        write_text(writer, "Q{");
        write_bytes(writer, raise->error_code.namespace_uri);
        write_text(writer, "}");
    }
    write_bytes(writer, raise->error_code.local_name);
    write_text(writer, " ");
    write_string_literal(writer, raise->description);
}

/* What is left to print, innermost last. */
typedef enum print_task_kind {
    /* An expression starting at the current column. */
    SXML_PRINT_TASK_KIND_EXPRESSION,
    /* A newline and value spaces. */
    SXML_PRINT_TASK_KIND_NEWLINE,
    /* The ")" that closes an expression. */
    SXML_PRINT_TASK_KIND_CLOSE,
    /* "(key DIRECTION EMPTY-ORDER [COLLATION]" of sort key value. */
    SXML_PRINT_TASK_KIND_SORT_KEY,
} print_task_kind;

typedef struct print_task {
    print_task_kind task_kind;
    /* An expression index, an indent or a sort key index. */
    uint32_t value;
} print_task;

typedef struct print_stack {
    print_task *tasks;
    size_t task_count;
    size_t task_capacity;
    sxml_allocator allocator;
} print_stack;

static sxml_status push_print_task(print_stack *stack, print_task task) {
    if (stack->task_count == stack->task_capacity) {
        const size_t new_capacity =
            stack->task_capacity < 32 ? 32 : stack->task_capacity * 2;
        size_t byte_count = 0;
        sxml_status status =
            sxml_size_multiply(new_capacity, sizeof(print_task), &byte_count);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        void *new_tasks = NULL;
        status = sxml_allocator_allocate(
            stack->allocator,
            (sxml_allocation_request){.byte_count = byte_count,
                                      .alignment = alignof(print_task)},
            &new_tasks);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (stack->task_count > 0) {
            memcpy(new_tasks, stack->tasks,
                   stack->task_count * sizeof(print_task));
        }
        sxml_allocator_release(stack->allocator, stack->tasks);
        stack->tasks = new_tasks;
        stack->task_capacity = new_capacity;
    }
    stack->tasks[stack->task_count] = task;
    stack->task_count += 1;
    return SXML_STATUS_OK;
}

static uint32_t indent_after(size_t column) {
    const size_t indent = column + 2;
    return indent > UINT32_MAX ? UINT32_MAX : (uint32_t)indent;
}

/* Pushes a nested expression operand: a newline at indent, then the
   expression. Tasks run in reverse order of pushing. */
typedef struct nested_operand {
    sxml_ir_expression_index expression;
    uint32_t indent;
} nested_operand;

static sxml_status push_nested_operand(print_stack *stack,
                                       nested_operand operand) {
    sxml_status status = push_print_task(
        stack, (print_task){.task_kind = SXML_PRINT_TASK_KIND_EXPRESSION,
                            .value = operand.expression.value});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return push_print_task(stack,
                           (print_task){.task_kind = SXML_PRINT_TASK_KIND_NEWLINE,
                                        .value = operand.indent});
}

/* Writes the opening and inline operands of one expression and pushes its
   nested operands and closing parenthesis. */
static sxml_status print_expression(text_writer *writer, print_stack *stack,
                                    sxml_ir_expression_index index) {
    const sxml_ir_expression *expression =
        sxml_ir_unit_get_expression(writer->unit, index);
    const sxml_ir_expression_kind kind = expression->expression_kind;
    if (kind == SXML_IR_EXPRESSION_KIND_ATOM) {
        write_atom(writer, expression->atom);
        return SXML_STATUS_OK;
    }
    const uint32_t indent = indent_after(writer->column);
    write_text(writer, "(");
    write_text(writer, kind < SXML_IR_EXPRESSION_KIND_COUNT
                           ? expression_kind_names[kind]
                           : "unknown");
    sxml_status status = push_print_task(
        stack, (print_task){.task_kind = SXML_PRINT_TASK_KIND_CLOSE, .value = 0});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    switch (kind) {
    case SXML_IR_EXPRESSION_KIND_NONE:
    case SXML_IR_EXPRESSION_KIND_ATOM:
    case SXML_IR_EXPRESSION_KIND_COUNT:
        return SXML_STATUS_OK;
    case SXML_IR_EXPRESSION_KIND_LET:
        write_text(writer, " ");
        write_slot(writer, expression->let.variable);
        write_text(writer, " ");
        status = push_nested_operand(
            stack, (nested_operand){.expression = expression->let.body,
                                    .indent = indent});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return push_print_task(
            stack,
            (print_task){.task_kind = SXML_PRINT_TASK_KIND_EXPRESSION,
                         .value = expression->let.bound_expression.value});
    case SXML_IR_EXPRESSION_KIND_SEQUENCE:
        write_atom_list(writer, expression->sequence);
        return SXML_STATUS_OK;
    case SXML_IR_EXPRESSION_KIND_CONDITIONAL:
        write_text(writer, " ");
        write_atom(writer, expression->conditional.condition);
        status = push_nested_operand(
            stack,
            (nested_operand){.expression = expression->conditional.else_branch,
                             .indent = indent});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return push_nested_operand(
            stack,
            (nested_operand){.expression = expression->conditional.then_branch,
                             .indent = indent});
    case SXML_IR_EXPRESSION_KIND_BINARY:
        write_text(writer, " ");
        write_text(writer, binary_operator_name(expression->binary.operator));
        write_text(writer, " ");
        write_atom(writer, expression->binary.left_operand);
        write_text(writer, " ");
        write_atom(writer, expression->binary.right_operand);
        return SXML_STATUS_OK;
    case SXML_IR_EXPRESSION_KIND_UNARY:
        write_text(writer, expression->unary.operator ==
                                   SXML_IR_UNARY_OPERATOR_MINUS
                               ? " minus "
                               : " plus ");
        write_atom(writer, expression->unary.operand);
        return SXML_STATUS_OK;
    case SXML_IR_EXPRESSION_KIND_FUNCTION_DEFINITION:
        write_text(writer, " ");
        write_unsigned(writer, expression->function_definition.function.value);
        return SXML_STATUS_OK;
    case SXML_IR_EXPRESSION_KIND_FUNCTION_REFERENCE:
        write_text(writer, " ");
        write_function_target(writer, expression->function_reference.target);
        write_text(writer, "#");
        write_unsigned(writer, expression->function_reference.arity);
        return SXML_STATUS_OK;
    case SXML_IR_EXPRESSION_KIND_STATIC_CALL:
        write_text(writer, " ");
        write_function_target(writer, expression->static_call.target);
        write_text(writer, "#");
        write_unsigned(writer, expression->static_call.arguments.atom_count);
        write_atom_list(writer, expression->static_call.arguments);
        return SXML_STATUS_OK;
    case SXML_IR_EXPRESSION_KIND_DYNAMIC_CALL:
        write_text(writer, " ");
        write_atom(writer, expression->dynamic_call.function_item);
        write_atom_list(writer, expression->dynamic_call.arguments);
        return SXML_STATUS_OK;
    case SXML_IR_EXPRESSION_KIND_LOOKUP:
        write_text(writer, " ");
        write_atom(writer, expression->lookup.value);
        write_text(writer, " ");
        if (expression->lookup.key.atom_kind == SXML_IR_ATOM_KIND_NONE) {
            write_text(writer, "*");
        } else {
            write_atom(writer, expression->lookup.key);
        }
        return SXML_STATUS_OK;
    case SXML_IR_EXPRESSION_KIND_MAP:
        write_text(writer, " ");
        write_atom(writer, expression->map.input);
        write_text(writer, " ");
        write_focus(writer, expression->map.focus);
        if ((expression->map.flags & SXML_IR_MAP_ALLOWING_EMPTY) != 0) {
            write_text(writer, " allowing-empty");
        }
        return push_nested_operand(
            stack, (nested_operand){.expression = expression->map.body,
                                    .indent = indent});
    case SXML_IR_EXPRESSION_KIND_FILTER:
        write_text(writer, " ");
        write_atom(writer, expression->filter.input);
        write_text(writer, " ");
        write_focus(writer, expression->filter.focus);
        return push_nested_operand(
            stack, (nested_operand){.expression = expression->filter.predicate,
                                    .indent = indent});
    case SXML_IR_EXPRESSION_KIND_QUANTIFIED:
        write_text(writer, expression->quantified.quantifier ==
                                   SXML_IR_QUANTIFIER_EVERY
                               ? " every "
                               : " some ");
        write_slot(writer, expression->quantified.variable);
        write_text(writer, " ");
        write_atom(writer, expression->quantified.input);
        return push_nested_operand(
            stack,
            (nested_operand){.expression = expression->quantified.condition,
                             .indent = indent});
    case SXML_IR_EXPRESSION_KIND_SORT: {
        write_text(writer, " ");
        write_atom(writer, expression->sort.input);
        write_text(writer, " ");
        write_focus(writer, expression->sort.focus);
        const sxml_ir_sort_key_list keys = expression->sort.keys;
        for (uint32_t key_number = keys.sort_key_count; key_number > 0;
             key_number -= 1) {
            const uint32_t key_index = keys.first_sort_key_index + key_number - 1;
            const sxml_ir_sort_key_view key_view = sxml_ir_unit_get_sort_keys(
                writer->unit,
                (sxml_ir_sort_key_list){.first_sort_key_index = key_index,
                                        .sort_key_count = 1});
            if (key_view.sort_key_count != 1) {
                continue;
            }
            status = push_print_task(
                stack, (print_task){.task_kind = SXML_PRINT_TASK_KIND_CLOSE});
            if (status == SXML_STATUS_OK) {
                status = push_nested_operand(
                    stack, (nested_operand){
                               .expression = key_view.sort_keys[0].key,
                               .indent = indent + 2});
            }
            if (status == SXML_STATUS_OK) {
                status = push_print_task(
                    stack, (print_task){.task_kind = SXML_PRINT_TASK_KIND_SORT_KEY,
                                        .value = key_index});
            }
            if (status == SXML_STATUS_OK) {
                status = push_print_task(
                    stack, (print_task){.task_kind = SXML_PRINT_TASK_KIND_NEWLINE,
                                        .value = indent});
            }
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        return SXML_STATUS_OK;
    }
    case SXML_IR_EXPRESSION_KIND_STEP:
        write_text(writer, " ");
        write_atom(writer, expression->step.origin);
        write_text(writer, " ");
        write_text(writer, axis_name(expression->step.axis));
        write_text(writer, " ");
        write_item_type(writer, expression->step.node_test);
        return SXML_STATUS_OK;
    case SXML_IR_EXPRESSION_KIND_DOCUMENT_ORDER:
        write_text(writer, " ");
        write_atom(writer, expression->document_order.input);
        return SXML_STATUS_OK;
    case SXML_IR_EXPRESSION_KIND_CAST:
    case SXML_IR_EXPRESSION_KIND_CASTABLE:
        write_text(writer, " ");
        write_atom(writer, expression->cast.value);
        write_text(writer, " ");
        write_item_type(writer, expression->cast.target_type);
        if (expression->cast.allows_empty) {
            write_text(writer, "?");
        }
        return SXML_STATUS_OK;
    case SXML_IR_EXPRESSION_KIND_INSTANCE_OF:
    case SXML_IR_EXPRESSION_KIND_TREAT:
        write_text(writer, " ");
        write_atom(writer, expression->type_test.value);
        write_text(writer, " ");
        write_sequence_type(writer, expression->type_test.sequence_type);
        return SXML_STATUS_OK;
    case SXML_IR_EXPRESSION_KIND_COERCE:
        write_text(writer, " ");
        write_atom(writer, expression->coercion.value);
        write_text(writer, " ");
        write_sequence_type(writer, expression->coercion.sequence_type);
        write_text(writer, " ");
        write_text(writer, coercion_role_name(expression->coercion.role));
        return SXML_STATUS_OK;
    case SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR: {
        const sxml_ir_map_entry_view entries = sxml_ir_unit_get_map_entries(
            writer->unit, expression->map_constructor.entries);
        for (size_t entry_index = 0; entry_index < entries.map_entry_count;
             entry_index += 1) {
            write_text(writer, " (");
            write_atom(writer, entries.map_entries[entry_index].key);
            if (entries.map_entries[entry_index].value.atom_kind !=
                SXML_IR_ATOM_KIND_NONE) {
                write_text(writer, " ");
                write_atom(writer, entries.map_entries[entry_index].value);
            }
            write_text(writer, ")");
        }
        return SXML_STATUS_OK;
    }
    case SXML_IR_EXPRESSION_KIND_ARRAY_CONSTRUCTOR:
        write_text(writer, expression->array_constructor.array_form ==
                                   SXML_IR_ARRAY_FORM_CURLY
                               ? " curly"
                               : " square");
        write_atom_list(writer, expression->array_constructor.members);
        return SXML_STATUS_OK;
    case SXML_IR_EXPRESSION_KIND_RAISE:
        write_raise(writer, &expression->raise);
        return SXML_STATUS_OK;
    case SXML_IR_EXPRESSION_KIND_TRY_CATCH:
        return push_nested_operand(
            stack, (nested_operand){.expression = expression->try_catch.body,
                                    .indent = indent});
    case SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_SHALLOW_COPY:
        write_text(writer, " ");
        write_atom(writer, expression->node_construction.name);
        write_text(writer, " ");
        write_atom(writer, expression->node_construction.value);
        if (expression->node_construction.content.value == 0) {
            return SXML_STATUS_OK;
        }
        return push_nested_operand(
            stack,
            (nested_operand){.expression = expression->node_construction.content,
                             .indent = indent});
    case SXML_IR_EXPRESSION_KIND_APPLY_TEMPLATES:
        write_text(writer, " ");
        write_atom(writer, expression->apply_templates.select);
        write_text(writer, " mode ");
        write_unsigned(writer, expression->apply_templates.mode.value);
        return SXML_STATUS_OK;
    }
    return SXML_STATUS_OK;
}

static void print_sort_key(text_writer *writer, uint32_t sort_key_index) {
    const sxml_ir_sort_key_view key_view = sxml_ir_unit_get_sort_keys(
        writer->unit, (sxml_ir_sort_key_list){
                          .first_sort_key_index = sort_key_index,
                          .sort_key_count = 1});
    if (key_view.sort_key_count != 1) {
        return;
    }
    const sxml_ir_sort_key *key = &key_view.sort_keys[0];
    write_text(writer, "(key ");
    write_text(writer, key->direction == SXML_IR_SORT_DIRECTION_DESCENDING
                           ? "descending"
                           : "ascending");
    write_text(writer, key->empty_order == SXML_IR_EMPTY_ORDER_GREATEST
                           ? " empty-greatest"
                           : " empty-least");
    if (key->collation.atom_kind != SXML_IR_ATOM_KIND_NONE) {
        write_text(writer, " collation ");
        write_atom(writer, key->collation);
    }
}

static sxml_status print_function(text_writer *writer, print_stack *stack,
                                  uint32_t function_number) {
    const sxml_ir_function *function = sxml_ir_unit_get_function(
        writer->unit, (sxml_ir_function_index){.value = function_number});
    write_text(writer, "(function ");
    write_unsigned(writer, function_number);
    write_text(writer, " ");
    if (function_number == 1) {
        write_text(writer, "main");
    } else if (function->function_name.index != 0) {
        write_name(writer, function->function_name);
    } else {
        write_text(writer, "anonymous");
    }
    write_text(writer, " (parameters ");
    write_unsigned(writer, function->parameter_count);
    write_text(writer, ") (captures");
    write_atom_list(writer, function->captured_variables);
    write_text(writer, ") (slots ");
    write_unsigned(writer, function->slot_count);
    write_text(writer, ")");
    if (function->focus.context_value.value != 0 ||
        function->focus.context_position.value != 0 ||
        function->focus.context_size.value != 0) {
        write_text(writer, " ");
        write_focus(writer, function->focus);
    }
    stack->task_count = 0;
    sxml_status status = push_print_task(
        stack, (print_task){.task_kind = SXML_PRINT_TASK_KIND_CLOSE, .value = 0});
    if (status == SXML_STATUS_OK) {
        status = push_nested_operand(
            stack,
            (nested_operand){.expression = function->body, .indent = 2});
    }
    while (status == SXML_STATUS_OK && stack->task_count > 0) {
        stack->task_count -= 1;
        const print_task task = stack->tasks[stack->task_count];
        switch (task.task_kind) {
        case SXML_PRINT_TASK_KIND_EXPRESSION:
            status = print_expression(
                writer, stack, (sxml_ir_expression_index){.value = task.value});
            break;
        case SXML_PRINT_TASK_KIND_NEWLINE:
            write_newline(writer, task.value);
            break;
        case SXML_PRINT_TASK_KIND_CLOSE:
            write_text(writer, ")");
            break;
        case SXML_PRINT_TASK_KIND_SORT_KEY:
            print_sort_key(writer, task.value);
            break;
        }
    }
    write_text(writer, "\n");
    return status;
}

sxml_status sxml_ir_unit_write_text(const sxml_ir_unit *unit,
                                    sxml_memory_view output,
                                    size_t *byte_count) {
    text_writer writer = {
        .output = output,
        .byte_count = 0,
        .column = 0,
        .unit = unit,
        .name_pool = sxml_item_heap_get_name_pool(unit->item_heap),
        .type_registry = sxml_item_heap_get_type_registry(unit->item_heap),
        .status = SXML_STATUS_OK,
    };
    print_stack stack = {.tasks = NULL,
                         .task_count = 0,
                         .task_capacity = 0,
                         .allocator = unit->allocator};
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t function_number = 1;
         status == SXML_STATUS_OK && function_number < unit->function_count;
         function_number += 1) {
        status = print_function(&writer, &stack, function_number);
    }
    sxml_allocator_release(unit->allocator, stack.tasks);
    if (status == SXML_STATUS_OK) {
        status = writer.status;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *byte_count = writer.byte_count;
    if (writer.byte_count > output.byte_count) {
        return SXML_STATUS_CAPACITY_EXCEEDED;
    }
    return SXML_STATUS_OK;
}

/* --------------------------------------------------------------- Passes */

static const sxml_ir_pass standard_passes[] = {
    {.pass_name = SXML_BYTE_VIEW_INITIALIZER("verify"), .run = sxml_ir_verify},
};

const sxml_ir_pass_view sxml_ir_standard_passes = {
    .passes = standard_passes,
    .pass_count = sizeof(standard_passes) / sizeof(standard_passes[0]),
};

sxml_status sxml_ir_run_passes(const sxml_ir_pass_list_request *request) {
    for (size_t pass_index = 0; pass_index < request->passes.pass_count;
         pass_index += 1) {
        const sxml_ir_pass *pass = &request->passes.passes[pass_index];
        sxml_status status = pass->run(&request->pass_request);
        if (status == SXML_STATUS_OK && request->verifies_after_each_pass &&
            pass->run != sxml_ir_verify) {
            status = sxml_ir_verify(&request->pass_request);
        }
        if (status != SXML_STATUS_OK) {
            /* Name the pass that failed when the failure is an invariant
               of the unit (an error in sxml), so tests can tell which pass
               broke it; specification errors keep their own text. */
            if (status == SXML_STATUS_INVALID_ARGUMENT &&
                request->pass_request.diagnostic != NULL &&
                request->pass_request.diagnostic->description.byte_count ==
                    0) {
                request->pass_request.diagnostic->description =
                    pass->pass_name;
            }
            return status;
        }
    }
    return SXML_STATUS_OK;
}
