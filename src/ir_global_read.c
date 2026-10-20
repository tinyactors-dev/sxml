/*
 * The global-read pass (D234; glossary `global_read`): globals are
 * computed on first read. An expression of the unit reads a global with
 * an initializer when it holds it as one of its own atoms (its operands,
 * atom lists, map entries, sort keys' collation and order, a function
 * definition's captured variables). The globals the expressions of one
 * LET chain read (an expression no LET holds, with the bound expressions
 * and bodies of the LETs below it, which all run once it starts) get a
 * GLOBAL_READ each at the chain's head, whose body is the head, moved:
 * a global is computed (src/evaluator_global.c) when a chain that needs
 * it starts, before the chain's own work (so nodes it makes are made
 * before the chain's, as when globals were computed first), and never
 * when no such chain runs (XQuery 4.0, 2.4.4 and "Variable Declaration";
 * XSLT 4.0, 9.12: a variable is evaluated when its value is needed). The
 * branches of a conditional, loop bodies and the other nested expressions
 * are chains of their own. Several globals of one chain: outermost first
 * in the order of first reading.
 *
 * The pass runs last (the loop passes see globals as atoms, D224), over
 * the expressions there are when it starts. Globals without an initializer
 * (external variables, the host's context value) are bound before the
 * evaluation starts and need no GLOBAL_READ. A GLOBAL_READ of a computed
 * global costs no step (src/evaluator.c), so step counts do not change
 * once each global is computed (D89).
 *
 * Golden file: src/ir_loop_passes.c.
 */
#include "ir_internal.h"
#include "ir_pass_internal.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>

/* The globals with an initializer one expression reads, each once, in
   the order first read; capacity grows in allocator memory. */
typedef struct global_reads {
    sxml_allocator allocator;
    uint32_t *global_numbers;
    size_t global_count;
    size_t global_capacity;
} global_reads;

/* Adds the global atom reads, when it is a global with an initializer
   not added before. */
static sxml_status add_read(const sxml_ir_unit *unit, global_reads *reads, sxml_ir_atom atom) {
    if (atom.atom_kind != SXML_IR_ATOM_KIND_GLOBAL_VARIABLE || atom.global_variable.value == 0 ||
        atom.global_variable.value >= unit->global_variable_count ||
        sxml_ir_unit_get_global_variable(unit, atom.global_variable)->initializer.value == 0) {
        return SXML_STATUS_OK;
    }
    for (size_t read_index = 0; read_index < reads->global_count; read_index += 1) {
        if (reads->global_numbers[read_index] == atom.global_variable.value) {
            return SXML_STATUS_OK;
        }
    }
    if (reads->global_count == reads->global_capacity) {
        const size_t capacity = reads->global_capacity == 0 ? 4 : reads->global_capacity * 2;
        void *memory = NULL;
        const sxml_status status = sxml_allocator_allocate(
            reads->allocator,
            (sxml_allocation_request){.byte_count = capacity * sizeof(uint32_t),
                                      .alignment = alignof(uint32_t)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        uint32_t *numbers = memory;
        for (size_t read_index = 0; read_index < reads->global_count; read_index += 1) {
            numbers[read_index] = reads->global_numbers[read_index];
        }
        if (reads->global_numbers != NULL) {
            sxml_allocator_release(reads->allocator, reads->global_numbers);
        }
        reads->global_numbers = numbers;
        reads->global_capacity = capacity;
    }
    reads->global_numbers[reads->global_count] = atom.global_variable.value;
    reads->global_count += 1;
    return SXML_STATUS_OK;
}

static sxml_status add_list_reads(const sxml_ir_unit *unit, global_reads *reads,
                                  sxml_ir_atom_list list) {
    const sxml_ir_atom_view atoms = sxml_ir_unit_get_atoms(unit, list);
    sxml_status status = SXML_STATUS_OK;
    for (size_t atom_index = 0; status == SXML_STATUS_OK && atom_index < atoms.atom_count;
         atom_index += 1) {
        status = add_read(unit, reads, atoms.atoms[atom_index]);
    }
    return status;
}

/* At most this many atoms an expression holds in its own fields. */
#define OWN_ATOM_COUNT 4

/* The atoms an expression holds in its own fields and its atom list. */
typedef struct own_atoms {
    sxml_ir_atom atoms[OWN_ATOM_COUNT];
    size_t atom_count;
    sxml_ir_atom_list list;
} own_atoms;

static own_atoms own_atoms_of(const sxml_ir_unit *unit, const sxml_ir_expression *expression) {
    own_atoms found = {.atom_count = 0, .list = {.first_atom_index = 0, .atom_count = 0}};
    const sxml_ir_atom none = {.atom_kind = SXML_IR_ATOM_KIND_NONE};
    sxml_ir_atom first = none;
    sxml_ir_atom second = none;
    sxml_ir_atom third = none;
    sxml_ir_atom fourth = none;
    switch (expression->expression_kind) {
    case SXML_IR_EXPRESSION_KIND_ATOM:
        first = expression->atom;
        break;
    case SXML_IR_EXPRESSION_KIND_SEQUENCE:
        found.list = expression->sequence;
        break;
    case SXML_IR_EXPRESSION_KIND_CONDITIONAL:
        first = expression->conditional.condition;
        break;
    case SXML_IR_EXPRESSION_KIND_BINARY:
        first = expression->binary.left_operand;
        second = expression->binary.right_operand;
        break;
    case SXML_IR_EXPRESSION_KIND_UNARY:
        first = expression->unary.operand;
        break;
    case SXML_IR_EXPRESSION_KIND_FUNCTION_DEFINITION:
        found.list = sxml_ir_unit_get_function(unit, expression->function_definition.function)
                         ->captured_variables;
        break;
    case SXML_IR_EXPRESSION_KIND_STATIC_CALL:
        found.list = expression->static_call.arguments;
        break;
    case SXML_IR_EXPRESSION_KIND_DYNAMIC_CALL:
        first = expression->dynamic_call.function_item;
        found.list = expression->dynamic_call.arguments;
        break;
    case SXML_IR_EXPRESSION_KIND_LOOKUP:
        first = expression->lookup.value;
        second = expression->lookup.key;
        break;
    case SXML_IR_EXPRESSION_KIND_MAP:
        first = expression->map.input;
        break;
    case SXML_IR_EXPRESSION_KIND_FILTER:
        first = expression->filter.input;
        break;
    case SXML_IR_EXPRESSION_KIND_QUANTIFIED:
        first = expression->quantified.input;
        break;
    case SXML_IR_EXPRESSION_KIND_SORT:
        first = expression->sort.input;
        break;
    case SXML_IR_EXPRESSION_KIND_STEP:
        first = expression->step.origin;
        break;
    case SXML_IR_EXPRESSION_KIND_DOCUMENT_ORDER:
        first = expression->document_order.input;
        break;
    case SXML_IR_EXPRESSION_KIND_CAST:
    case SXML_IR_EXPRESSION_KIND_CASTABLE:
        first = expression->cast.value;
        break;
    case SXML_IR_EXPRESSION_KIND_INSTANCE_OF:
    case SXML_IR_EXPRESSION_KIND_TREAT:
        first = expression->type_test.value;
        break;
    case SXML_IR_EXPRESSION_KIND_COERCE:
        first = expression->coercion.value;
        break;
    case SXML_IR_EXPRESSION_KIND_ARRAY_CONSTRUCTOR:
        found.list = expression->array_constructor.members;
        break;
    case SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_SHALLOW_COPY:
        first = expression->node_construction.name;
        second = expression->node_construction.value;
        break;
    case SXML_IR_EXPRESSION_KIND_APPLY_TEMPLATES:
        first = expression->apply_templates.select;
        second = expression->apply_templates.parameters;
        third = expression->apply_templates.tunnel_parameters;
        fourth = expression->apply_templates.separator;
        break;
    case SXML_IR_EXPRESSION_KIND_STREAM:
        first = expression->stream.origin;
        break;
    case SXML_IR_EXPRESSION_KIND_NONE:
    case SXML_IR_EXPRESSION_KIND_LET:
    case SXML_IR_EXPRESSION_KIND_FUNCTION_REFERENCE:
    case SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_RAISE:
    case SXML_IR_EXPRESSION_KIND_TRY_CATCH:
    case SXML_IR_EXPRESSION_KIND_GLOBAL_READ:
    case SXML_IR_EXPRESSION_KIND_COUNT:
        break;
    }
    const sxml_ir_atom candidates[OWN_ATOM_COUNT] = {first, second, third, fourth};
    for (size_t candidate_index = 0; candidate_index < OWN_ATOM_COUNT; candidate_index += 1) {
        if (candidates[candidate_index].atom_kind != SXML_IR_ATOM_KIND_NONE) {
            found.atoms[found.atom_count] = candidates[candidate_index];
            found.atom_count += 1;
        }
    }
    return found;
}

/* Collects into reads the globals with an initializer that expression
   reads itself. */
static sxml_status collect_reads(const sxml_ir_unit *unit, const sxml_ir_expression *expression,
                                 global_reads *reads) {
    const own_atoms found = own_atoms_of(unit, expression);
    sxml_status status = SXML_STATUS_OK;
    for (size_t atom_index = 0; status == SXML_STATUS_OK && atom_index < found.atom_count;
         atom_index += 1) {
        status = add_read(unit, reads, found.atoms[atom_index]);
    }
    if (status == SXML_STATUS_OK) {
        status = add_list_reads(unit, reads, found.list);
    }
    if (status == SXML_STATUS_OK &&
        expression->expression_kind == SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR) {
        const sxml_ir_map_entry_view entries =
            sxml_ir_unit_get_map_entries(unit, expression->map_constructor.entries);
        for (size_t entry_index = 0;
             status == SXML_STATUS_OK && entry_index < entries.map_entry_count; entry_index += 1) {
            status = add_read(unit, reads, entries.map_entries[entry_index].key);
            if (status == SXML_STATUS_OK) {
                status = add_read(unit, reads, entries.map_entries[entry_index].value);
            }
        }
    }
    if (status == SXML_STATUS_OK && expression->expression_kind == SXML_IR_EXPRESSION_KIND_SORT) {
        const sxml_ir_sort_key_view keys = sxml_ir_unit_get_sort_keys(unit, expression->sort.keys);
        for (size_t key_index = 0; status == SXML_STATUS_OK && key_index < keys.sort_key_count;
             key_index += 1) {
            status = add_read(unit, reads, keys.sort_keys[key_index].collation);
            if (status == SXML_STATUS_OK) {
                status = add_read(unit, reads, keys.sort_keys[key_index].order);
            }
        }
    }
    return status;
}

/* Puts a GLOBAL_READ of each global of reads at index, the expression
   there moved to a new index as the innermost body. */
static sxml_status wrap_expression(sxml_ir_unit *unit, sxml_ir_expression_index index,
                                   const global_reads *reads) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t read_number = reads->global_count; status == SXML_STATUS_OK && read_number > 0;
         read_number -= 1) {
        const sxml_ir_expression moved = *sxml_ir_unit_get_expression(unit, index);
        sxml_ir_expression_index body = {.value = 0};
        status = sxml_ir_unit_add_expression(unit, &moved, &body);
        if (status == SXML_STATUS_OK) {
            status = sxml_ir_unit_replace_expression(
                unit,
                &(sxml_ir_expression_replacement){
                    .index = index,
                    .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_GLOBAL_READ,
                                   .source_span = moved.source_span,
                                   .global_read = {.global = {.value = reads->global_numbers
                                                                           [read_number - 1]},
                                                   .body = body}}});
        }
    }
    return status;
}

/* Per expression of the unit: the LET whose bound expression or body it
   is (0 for none), and the lists of expressions by the head of their LET
   chain: first[head] and next[expression], 0 ending them. */
typedef struct chain_lists {
    uint32_t *let_parents;
    uint32_t *first;
    uint32_t *next;
} chain_lists;

static sxml_status make_chain_lists(const sxml_ir_unit *unit, chain_lists *lists) {
    const size_t count = unit->expression_count;
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate(
        unit->allocator,
        (sxml_allocation_request){.byte_count = 3U * count * sizeof(uint32_t),
                                  .alignment = alignof(uint32_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t *arrays = memory;
    *lists = (chain_lists){
        .let_parents = arrays, .first = arrays + count, .next = arrays + 2U * count};
    for (size_t index = 0; index < 3U * count; index += 1) {
        arrays[index] = 0;
    }
    for (uint32_t number = 1; number < count; number += 1) {
        const sxml_ir_expression *expression =
            sxml_ir_unit_get_expression(unit, (sxml_ir_expression_index){.value = number});
        if (expression->expression_kind != SXML_IR_EXPRESSION_KIND_LET) {
            continue;
        }
        const uint32_t children[2] = {expression->let.bound_expression.value,
                                      expression->let.body.value};
        for (size_t child_index = 0; child_index < 2; child_index += 1) {
            if (children[child_index] != 0 && children[child_index] < count) {
                lists->let_parents[children[child_index]] = number;
            }
        }
    }
    /* Backwards, so each list is in index order. */
    for (uint32_t number = (uint32_t)count; number > 1; number -= 1) {
        uint32_t chain_head = number - 1;
        size_t climbed = 0;
        while (lists->let_parents[chain_head] != 0 && climbed < count) {
            chain_head = lists->let_parents[chain_head];
            climbed += 1;
        }
        lists->next[number - 1] = lists->first[chain_head];
        lists->first[chain_head] = number - 1;
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_ir_read_globals_lazily(const sxml_ir_pass_request *request) {
    sxml_ir_unit *unit = request->unit;
    if (unit->global_variable_count < 2) {
        return SXML_STATUS_OK;
    }
    const uint32_t expression_count = unit->expression_count;
    chain_lists lists = {.let_parents = NULL, .first = NULL, .next = NULL};
    sxml_status status = make_chain_lists(unit, &lists);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    global_reads reads = {.allocator = unit->allocator,
                          .global_numbers = NULL,
                          .global_count = 0,
                          .global_capacity = 0};
    /* Each LET chain (an expression no LET holds, with the LETs below it)
       runs whole once it starts: its globals are read at its head. */
    for (uint32_t chain_head = 1; status == SXML_STATUS_OK && chain_head < expression_count;
         chain_head += 1) {
        reads.global_count = 0;
        for (uint32_t member = lists.first[chain_head]; status == SXML_STATUS_OK && member != 0;
             member = lists.next[member]) {
            status = collect_reads(
                unit, sxml_ir_unit_get_expression(unit, (sxml_ir_expression_index){.value = member}),
                &reads);
        }
        if (status == SXML_STATUS_OK && reads.global_count > 0) {
            status =
                wrap_expression(unit, (sxml_ir_expression_index){.value = chain_head}, &reads);
        }
    }
    sxml_allocator_release(unit->allocator, lists.let_parents);
    if (reads.global_numbers != NULL) {
        sxml_allocator_release(reads.allocator, reads.global_numbers);
    }
    return status;
}
