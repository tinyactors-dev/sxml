/*
 * The filter-index pass (src/ir_pass_internal.h; docs/DECISIONS.md D224;
 * glossary `filter_index`, `index_key`, `index_probe`).
 *
 * A FILTER whose predicate must compare a key of the item with a value
 * that does not depend on the item, as in
 *
 *     $tracks[local:artist(.) = $artist]       (QT4 app-Demos itunes)
 *     $V[. instance of gnode()][. is $node]   (a pattern match="$V")
 *
 * gets a filter index (sxml_ir_filter_index): copies of the expressions
 * that compute the key operand from the focus value (the index key) and of
 * those that compute the other operand (the index probe). The predicate
 * itself is not changed. The evaluator (src/evaluator_index.c) can then
 * look the probe's value up in an index of the input by key, kept in the
 * evaluation memo, and test the predicate on the items found only: a
 * predicate that needs "key OPERATOR probe" to be true is false for every
 * other item (XPath 4.0, 4.4.1 and 4.11; an operand of "and" that is false
 * decides it, 4.13), so the result is the same.
 *
 * Which conjunct. The pass follows the predicate as far as its value must
 * be true: through a LET to its body, and through a CONDITIONAL whose else
 * branch is false() or () (the lowering of "and") to its condition and its
 * then branch. A condition read from a LET's slot is followed into the
 * LET's bound expression. Where this ends in a BINARY with
 * GENERAL_EQUAL, VALUE_EQUAL or IS, that comparison is a necessary
 * conjunct. The first one whose operands split is indexed:
 * - the key operand reads, through the LETs found on the way, the focus
 *   value and nothing else but globals and constants (so its value is a
 *   function of the item, the same in every evaluation of the FILTER);
 * - the probe operand reads, through those LETs, no focus at all;
 * - every expression copied is "plain": of the kinds the evaluator
 *   evaluates without effects or new identities (no constructor, no
 *   function item made or called dynamically, no try/catch, no
 *   templates), and a static call is of a native function that is
 *   deterministic, reads no dynamic context and calls no function items,
 *   a constructor function, or an IR function of the unit whose body is
 *   plain in the same sense (a fixpoint over the call graph).
 * The FILTER must read neither the context position nor the size: the
 * candidates are tested out of their positions.
 *
 * The copies keep their slots: they are evaluated on their own, the key
 * with the FILTER's focus value bound and the probe outside the focus, so
 * no slot is bound twice on one path (src/ir_pass_internal.h's verifier).
 *
 * Every function body and expression is walked with explicit stacks (D71).
 *
 * Golden file: src/ir_verify.c (a pass over a unit, explicit task stacks,
 * arrays in the unit's allocator).
 */
#include "ir_pass_internal.h"

#include "function_library_internal.h"
#include "memory_internal.h"
#include "xdm_internal.h"

#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------ Arrays */

/* A growable array of fixed-size elements in the pass's allocator. */
typedef struct pass_array {
    void *elements;
    size_t element_count;
    size_t element_capacity;
    size_t element_size;
    size_t element_alignment;
} pass_array;

static pass_array pass_array_start(size_t element_size, size_t element_alignment) {
    return (pass_array){.elements = NULL,
                        .element_count = 0,
                        .element_capacity = 0,
                        .element_size = element_size,
                        .element_alignment = element_alignment};
}

static void pass_array_finalize(sxml_allocator allocator, pass_array *array) {
    if (array->elements != NULL) {
        sxml_allocator_release(allocator, array->elements);
    }
    array->elements = NULL;
    array->element_count = 0;
    array->element_capacity = 0;
}

/* Makes room for added_count more elements than array has room for;
   returns the allocator's failures. */
static sxml_status pass_array_grow(sxml_allocator allocator, pass_array *array,
                                   size_t added_count) {
    size_t new_capacity = array->element_capacity < 16 ? 16 : array->element_capacity * 2;
    while (new_capacity - array->element_count < added_count) {
        new_capacity *= 2;
    }
    void *new_elements = NULL;
    const sxml_status status = sxml_allocator_allocate_array(
        allocator,
        (sxml_array_allocation_request){.element_count = new_capacity,
                                        .element_size = array->element_size,
                                        .element_alignment = array->element_alignment},
        &new_elements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (array->element_count > 0) {
        memcpy(new_elements, array->elements, array->element_count * array->element_size);
    }
    if (array->elements != NULL) {
        sxml_allocator_release(allocator, array->elements);
    }
    array->elements = new_elements;
    array->element_capacity = new_capacity;
    return SXML_STATUS_OK;
}

/* Makes room for added_count more elements; returns the allocator's
   failures. */
static sxml_status pass_array_reserve(sxml_allocator allocator, pass_array *array,
                                      size_t added_count) {
    if (added_count <= array->element_capacity - array->element_count) {
        return SXML_STATUS_OK;
    }
    return pass_array_grow(allocator, array, added_count);
}

/* Appends a copy of element; returns the allocator's failures. */
// ast-grep-ignore: pointer-parameter-without-length
static sxml_status pass_array_append(sxml_allocator allocator, pass_array *array,
                                     const void *element) {
    const sxml_status status = pass_array_reserve(allocator, array, 1);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memcpy((uint8_t *)array->elements + array->element_count * array->element_size, element,
           array->element_size);
    array->element_count += 1;
    return SXML_STATUS_OK;
}

/* ------------------------------------------------------- Expressions */

/* At most this many child expressions per expression of a plain kind. */
#define FILTER_INDEX_CHILD_COUNT 4

/* The child expressions of an expression of a plain kind. */
typedef struct expression_children {
    sxml_ir_expression_index children[FILTER_INDEX_CHILD_COUNT];
    size_t child_count;
} expression_children;

/* True for the kinds an index key or probe may copy: evaluated by the
   machine without effects and without making nodes or function items. */
static bool kind_is_plain(sxml_ir_expression_kind kind) {
    switch (kind) {
    case SXML_IR_EXPRESSION_KIND_ATOM:
    case SXML_IR_EXPRESSION_KIND_LET:
    case SXML_IR_EXPRESSION_KIND_SEQUENCE:
    case SXML_IR_EXPRESSION_KIND_CONDITIONAL:
    case SXML_IR_EXPRESSION_KIND_BINARY:
    case SXML_IR_EXPRESSION_KIND_UNARY:
    case SXML_IR_EXPRESSION_KIND_STATIC_CALL:
    case SXML_IR_EXPRESSION_KIND_LOOKUP:
    case SXML_IR_EXPRESSION_KIND_MAP:
    case SXML_IR_EXPRESSION_KIND_FILTER:
    case SXML_IR_EXPRESSION_KIND_QUANTIFIED:
    case SXML_IR_EXPRESSION_KIND_STEP:
    case SXML_IR_EXPRESSION_KIND_DOCUMENT_ORDER:
    case SXML_IR_EXPRESSION_KIND_CAST:
    case SXML_IR_EXPRESSION_KIND_CASTABLE:
    case SXML_IR_EXPRESSION_KIND_INSTANCE_OF:
    case SXML_IR_EXPRESSION_KIND_TREAT:
    case SXML_IR_EXPRESSION_KIND_COERCE:
    case SXML_IR_EXPRESSION_KIND_RAISE:
        return true;
    case SXML_IR_EXPRESSION_KIND_NONE:
    case SXML_IR_EXPRESSION_KIND_FUNCTION_DEFINITION:
    case SXML_IR_EXPRESSION_KIND_FUNCTION_REFERENCE:
    case SXML_IR_EXPRESSION_KIND_DYNAMIC_CALL:
    case SXML_IR_EXPRESSION_KIND_SORT:
    case SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_ARRAY_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_TRY_CATCH:
    case SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_SHALLOW_COPY:
    case SXML_IR_EXPRESSION_KIND_APPLY_TEMPLATES:
    case SXML_IR_EXPRESSION_KIND_STREAM:
    case SXML_IR_EXPRESSION_KIND_GLOBAL_READ:
    case SXML_IR_EXPRESSION_KIND_MERGE:
    case SXML_IR_EXPRESSION_KIND_COUNT:
        return false;
    }
    return false;
}

/* The child expressions of expression, a plain kind. */
static expression_children children_of(const sxml_ir_expression *expression) {
    expression_children children = {.child_count = 0};
    const sxml_ir_expression_index none = {.value = 0};
    sxml_ir_expression_index found[FILTER_INDEX_CHILD_COUNT] = {none, none, none, none};
    if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_LET) {
        found[0] = expression->let.bound_expression;
        found[1] = expression->let.body;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_CONDITIONAL) {
        found[0] = expression->conditional.then_branch;
        found[1] = expression->conditional.else_branch;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_MAP) {
        found[0] = expression->map.body;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_FILTER) {
        found[0] = expression->filter.predicate;
        found[1] = expression->filter.index.key;
        found[2] = expression->filter.index.probe;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_QUANTIFIED) {
        found[0] = expression->quantified.condition;
    }
    for (size_t child_index = 0; child_index < FILTER_INDEX_CHILD_COUNT; child_index += 1) {
        if (found[child_index].value != 0) {
            children.children[children.child_count] = found[child_index];
            children.child_count += 1;
        }
    }
    return children;
}

/* At most this many atoms an expression of a plain kind reads directly
   (beyond its atom lists). */
#define FILTER_INDEX_ATOM_COUNT 2

/* The atoms an expression of a plain kind reads, and its atom list. */
typedef struct expression_atoms {
    sxml_ir_atom atoms[FILTER_INDEX_ATOM_COUNT];
    size_t atom_count;
    sxml_ir_atom_list list;
} expression_atoms;

static expression_atoms atoms_of(const sxml_ir_expression *expression) {
    expression_atoms read = {.atom_count = 0, .list = {.first_atom_index = 0, .atom_count = 0}};
    if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_ATOM) {
        read.atoms[0] = expression->atom;
        read.atom_count = 1;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_SEQUENCE) {
        read.list = expression->sequence;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_CONDITIONAL) {
        read.atoms[0] = expression->conditional.condition;
        read.atom_count = 1;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_BINARY) {
        read.atoms[0] = expression->binary.left_operand;
        read.atoms[1] = expression->binary.right_operand;
        read.atom_count = 2;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_UNARY) {
        read.atoms[0] = expression->unary.operand;
        read.atom_count = 1;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_STATIC_CALL) {
        read.list = expression->static_call.arguments;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_LOOKUP) {
        read.atoms[0] = expression->lookup.value;
        read.atoms[1] = expression->lookup.key;
        read.atom_count = 2;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_MAP) {
        read.atoms[0] = expression->map.input;
        read.atom_count = 1;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_FILTER) {
        read.atoms[0] = expression->filter.input;
        read.atom_count = 1;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_QUANTIFIED) {
        read.atoms[0] = expression->quantified.input;
        read.atom_count = 1;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_STEP) {
        read.atoms[0] = expression->step.origin;
        read.atom_count = 1;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_DOCUMENT_ORDER) {
        read.atoms[0] = expression->document_order.input;
        read.atom_count = 1;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_CAST ||
        expression->expression_kind == SXML_IR_EXPRESSION_KIND_CASTABLE) {
        read.atoms[0] = expression->cast.value;
        read.atom_count = 1;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_INSTANCE_OF ||
        expression->expression_kind == SXML_IR_EXPRESSION_KIND_TREAT) {
        read.atoms[0] = expression->type_test.value;
        read.atom_count = 1;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_COERCE) {
        read.atoms[0] = expression->coercion.value;
        read.atom_count = 1;
    }
    return read;
}

/* ------------------------------------------------------------- The pass */

/* What the pass knows of a function: whether its body is plain. */
typedef enum function_plainness {
    SXML_FUNCTION_PLAINNESS_PLAIN = 0,
    SXML_FUNCTION_PLAINNESS_NOT_PLAIN = 1,
} function_plainness;

/* A LET met on the way to a necessary conjunct. */
typedef struct conjunct_let {
    sxml_ir_slot slot;
    sxml_ir_expression_index bound_expression;
    /* In the order found: an order in which every LET comes after the
       LETs it reads. */
    uint32_t order;
    /* Set when another LET found binds the same slot. */
    bool is_ambiguous;
    /* Whether its bound expression is plain and what it reads, made on
       first need. */
    bool is_analysed;
    bool is_plain;
    /* The run of slots its bound expression reads that it does not bind
       itself, in the pass's slot array. */
    size_t first_read_slot;
    size_t read_slot_count;
    /* Marks of the closure being computed. */
    bool is_in_closure;
} conjunct_let;

typedef struct loop_pass {
    const sxml_ir_pass_request *request;
    sxml_ir_unit *unit;
    sxml_allocator allocator;
    /* One per function of the unit (sxml_function_plainness), made on
       first need; NULL before. */
    uint8_t *function_plainness;
    /* The FILTER being looked at. */
    sxml_ir_slot focus_slot;
    /* conjunct_let, the LETs found for the FILTER. */
    pass_array lets;
    /* sxml_ir_expression_index: expressions whose truth is necessary,
       still to follow; BINARY comparisons found. */
    pass_array necessary;
    pass_array comparisons;
    /* sxml_ir_slot: the slots LETs read (conjunct_let's runs). */
    pass_array read_slots;
    /* sxml_ir_expression_index: a walk's stack, and the expressions of a
       subtree to copy. */
    pass_array walk;
    pass_array subtree;
    /* uint32_t: the slots of a closure to follow. */
    pass_array closure_slots;
    /* The demand pass's and the fusion pass's arrays (demand_member,
       demand_child), lent to one function at a time so their room is
       made once per pass, not once per function. */
    pass_array members;
    pass_array member_stack;
    /* uint32_t per expression of the unit: a copy's new index, 0 for an
       expression not copied (cleared after each copy). */
    uint32_t *copy_targets;
    size_t copy_target_count;
    /* uint32_t per expression lowering made: the function whose body
       holds it, 0 for none; made on first need. */
    uint32_t *owners;
    size_t owner_count;
    /* One flag per expression of owners: set for one inside a STREAM's
       predicate or body, which the passes leave alone (D223: a STREAM
       rebinds its counter and fold variables while it runs). */
    bool *is_in_stream;
    /* The constants true() and false(), added on first need. */
    sxml_ir_constant_index true_constant;
    sxml_ir_constant_index false_constant;
} loop_pass;

static sxml_ir_expression expression_at(const loop_pass *pass,
                                        sxml_ir_expression_index index) {
    return *sxml_ir_unit_get_expression(pass->unit, index);
}

/* True for a constant false() or (): the else branch of an "and". */
static bool expression_is_false(const loop_pass *pass,
                                sxml_ir_expression_index index) {
    const sxml_ir_expression expression = expression_at(pass, index);
    if (expression.expression_kind == SXML_IR_EXPRESSION_KIND_SEQUENCE) {
        return expression.sequence.atom_count == 0;
    }
    if (expression.expression_kind != SXML_IR_EXPRESSION_KIND_ATOM ||
        expression.atom.atom_kind != SXML_IR_ATOM_KIND_CONSTANT) {
        return false;
    }
    const sxml_sequence value = sxml_ir_unit_get_constant(pass->unit, expression.atom.constant);
    if (sxml_sequence_get_representation(value) == SXML_SEQUENCE_REPRESENTATION_EMPTY) {
        return true;
    }
    sxml_item item = {.representation = 0};
    if (sxml_sequence_get_representation(value) != SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM ||
        sxml_sequence_get_item(value, (sxml_item_request){.item_index = 0, .error_record = NULL},
                               &item) != SXML_STATUS_OK) {
        return false;
    }
    sxml_atomic_datum datum;
    return sxml_atomic_item_get_datum(item, &datum) == SXML_STATUS_OK &&
           datum.datum_kind == SXML_DATUM_KIND_BOOLEAN && !datum.datum_details.boolean;
}

/* --------------------------------------------------- Plain functions */

/* True when the call target may be copied: see the file comment. Reads
   the plainness of IR functions, which must be made. */
static bool target_is_plain(const loop_pass *pass, sxml_ir_function_target target) {
    switch (target.target_kind) {
    case SXML_IR_FUNCTION_TARGET_KIND_NATIVE: {
        const uint32_t excluded = (uint32_t)SXML_NATIVE_FUNCTION_USES_DYNAMIC_CONTEXT |
                                  (uint32_t)SXML_NATIVE_FUNCTION_IS_NONDETERMINISTIC |
                                  (uint32_t)SXML_NATIVE_FUNCTION_CALLS_FUNCTION_ITEMS |
                                  (uint32_t)SXML_NATIVE_FUNCTION_TAKES_CALLER_FOCUS;
        return target.native_function != NULL &&
               (target.native_function->properties & excluded) == 0;
    }
    case SXML_IR_FUNCTION_TARGET_KIND_IR_FUNCTION:
        return target.function.value != 0 &&
               target.function.value < pass->unit->function_count &&
               pass->function_plainness != NULL &&
               pass->function_plainness[target.function.value] ==
                   (uint8_t)SXML_FUNCTION_PLAINNESS_PLAIN;
    case SXML_IR_FUNCTION_TARGET_KIND_CONSTRUCTOR:
        return true;
    case SXML_IR_FUNCTION_TARGET_KIND_NONE:
        return false;
    }
    return false;
}

/* True when every expression of the subtree at root is of a plain kind
   and calls plain targets (with the current function plainness). Uses
   pass->walk. */
static sxml_status subtree_is_plain(loop_pass *pass, sxml_ir_expression_index root,
                                    bool *is_plain) {
    pass->walk.element_count = 0;
    sxml_status status = pass_array_append(pass->allocator, &pass->walk, &root);
    *is_plain = true;
    while (status == SXML_STATUS_OK && *is_plain && pass->walk.element_count > 0) {
        pass->walk.element_count -= 1;
        const sxml_ir_expression_index index =
            ((const sxml_ir_expression_index *)pass->walk.elements)[pass->walk.element_count];
        if (index.value == 0 || index.value >= pass->unit->expression_count) {
            *is_plain = false;
            break;
        }
        const sxml_ir_expression expression = expression_at(pass, index);
        if (!kind_is_plain(expression.expression_kind) ||
            (expression.expression_kind == SXML_IR_EXPRESSION_KIND_STATIC_CALL &&
             !target_is_plain(pass, expression.static_call.target))) {
            *is_plain = false;
            break;
        }
        const expression_children children = children_of(&expression);
        for (size_t child_index = 0; status == SXML_STATUS_OK && child_index < children.child_count;
             child_index += 1) {
            status = pass_array_append(pass->allocator, &pass->walk,
                                       &children.children[child_index]);
        }
    }
    return status;
}

/*
 * Makes the plainness of every function of the unit: a function is plain
 * when its body is, its calls of IR functions included. Starts from every
 * function plain and marks the ones that are not until nothing changes
 * (the greatest fixpoint, so recursive functions can be plain); each
 * round walks every body once.
 */
static sxml_status make_function_plainness(loop_pass *pass) {
    const size_t function_count = pass->unit->function_count;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        pass->allocator,
        (sxml_array_allocation_request){.element_count = function_count,
                                        .element_size = sizeof(uint8_t),
                                        .element_alignment = alignof(uint8_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    pass->function_plainness = memory;
    if (function_count > 0) {
        pass->function_plainness[0] = (uint8_t)SXML_FUNCTION_PLAINNESS_NOT_PLAIN;
    }
    bool is_changed = true;
    while (status == SXML_STATUS_OK && is_changed) {
        is_changed = false;
        for (size_t function_number = 1; status == SXML_STATUS_OK && function_number < function_count;
             function_number += 1) {
            if (pass->function_plainness[function_number] ==
                (uint8_t)SXML_FUNCTION_PLAINNESS_NOT_PLAIN) {
                continue;
            }
            const sxml_ir_function *function = sxml_ir_unit_get_function(
                pass->unit, (sxml_ir_function_index){.value = (uint32_t)function_number});
            bool is_plain = false;
            status = subtree_is_plain(pass, function->body, &is_plain);
            if (status == SXML_STATUS_OK && !is_plain) {
                pass->function_plainness[function_number] =
                    (uint8_t)SXML_FUNCTION_PLAINNESS_NOT_PLAIN;
                is_changed = true;
            }
        }
    }
    return status;
}

/* ---------------------------------------------------- The conjuncts */

static conjunct_let *let_at(const loop_pass *pass, size_t let_index) {
    return &((conjunct_let *)pass->lets.elements)[let_index];
}

/* The LET found for slot, or NULL when there is none. */
static conjunct_let *find_let(const loop_pass *pass, sxml_ir_slot slot) {
    for (size_t let_index = 0; let_index < pass->lets.element_count; let_index += 1) {
        conjunct_let *let = let_at(pass, let_index);
        if (let->slot.value == slot.value) {
            return let;
        }
    }
    return NULL;
}

static sxml_status add_let(loop_pass *pass, const sxml_ir_let *let) {
    conjunct_let *same = find_let(pass, let->variable);
    if (same != NULL) {
        same->is_ambiguous = true;
        return SXML_STATUS_OK;
    }
    const conjunct_let found = {.slot = let->variable,
                                .bound_expression = let->bound_expression,
                                .order = (uint32_t)pass->lets.element_count,
                                .is_ambiguous = false,
                                .is_analysed = false,
                                .is_plain = false,
                                .first_read_slot = 0,
                                .read_slot_count = 0,
                                .is_in_closure = false};
    return pass_array_append(pass->allocator, &pass->lets, &found);
}

/* True for a comparison whose own module names a default collation
   (sxml_ir_binary.module_number): the index, which checks the running
   module's, is not for it. */
static bool names_default_collation(const sxml_ir_unit *unit, const sxml_ir_binary *binary) {
    return binary->module_number != 0 && binary->module_number < unit->module_count &&
           unit->modules[binary->module_number].has_static_context &&
           unit->modules[binary->module_number].static_context.default_collation_uri.byte_count >
               0;
}

/*
 * Follows the predicate as far as its value must be true and collects the
 * LETs on the way and the necessary comparisons (see the file comment).
 */
static sxml_status find_conjuncts(loop_pass *pass, sxml_ir_expression_index predicate) {
    pass->lets.element_count = 0;
    pass->necessary.element_count = 0;
    pass->comparisons.element_count = 0;
    sxml_status status = pass_array_append(pass->allocator, &pass->necessary, &predicate);
    while (status == SXML_STATUS_OK && pass->necessary.element_count > 0) {
        pass->necessary.element_count -= 1;
        sxml_ir_expression_index index = ((const sxml_ir_expression_index *)
                                              pass->necessary.elements)[pass->necessary
                                                                               .element_count];
        while (status == SXML_STATUS_OK && index.value != 0) {
            const sxml_ir_expression_index current = index;
            const sxml_ir_expression expression = expression_at(pass, current);
            index = (sxml_ir_expression_index){.value = 0};
            if (expression.expression_kind == SXML_IR_EXPRESSION_KIND_LET) {
                status = add_let(pass, &expression.let);
                index = expression.let.body;
            } else if (expression.expression_kind == SXML_IR_EXPRESSION_KIND_CONDITIONAL &&
                       expression_is_false(pass, expression.conditional.else_branch)) {
                /* "C and T": C's value must be true, and T's. */
                const sxml_ir_atom condition = expression.conditional.condition;
                const conjunct_let *bound =
                    condition.atom_kind == SXML_IR_ATOM_KIND_SLOT
                        ? find_let(pass, condition.slot)
                        : NULL;
                if (bound != NULL && !bound->is_ambiguous) {
                    status = pass_array_append(pass->allocator, &pass->necessary,
                                               &bound->bound_expression);
                }
                index = expression.conditional.then_branch;
            } else if (expression.expression_kind == SXML_IR_EXPRESSION_KIND_BINARY &&
                       (expression.binary.operator == SXML_IR_BINARY_OPERATOR_GENERAL_EQUAL ||
                        expression.binary.operator == SXML_IR_BINARY_OPERATOR_VALUE_EQUAL ||
                        expression.binary.operator == SXML_IR_BINARY_OPERATOR_IS) &&
                       !names_default_collation(pass->unit, &expression.binary)) {
                status = pass_array_append(pass->allocator, &pass->comparisons, &current);
            }
        }
    }
    return status;
}

/* --------------------------------------------------------- Closures */

/* Appends the slot an atom reads, if it is a slot. */
static sxml_status note_read_atom(loop_pass *pass, sxml_ir_atom atom) {
    if (atom.atom_kind != SXML_IR_ATOM_KIND_SLOT || atom.slot.value == 0) {
        return SXML_STATUS_OK;
    }
    return pass_array_append(pass->allocator, &pass->read_slots, &atom.slot);
}

/* The slots an expression binds for its children (none for most). */
typedef struct bound_slots {
    sxml_ir_slot slots[3];
} bound_slots;

static bound_slots slots_bound_by(const sxml_ir_expression *expression) {
    const sxml_ir_slot none = {.value = 0};
    bound_slots bound = {.slots = {none, none, none}};
    if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_LET) {
        bound.slots[0] = expression->let.variable;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_MAP) {
        bound.slots[0] = expression->map.focus.context_value;
        bound.slots[1] = expression->map.focus.context_position;
        bound.slots[2] = expression->map.focus.context_size;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_FILTER) {
        bound.slots[0] = expression->filter.focus.context_value;
        bound.slots[1] = expression->filter.focus.context_position;
        bound.slots[2] = expression->filter.focus.context_size;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_QUANTIFIED) {
        bound.slots[0] = expression->quantified.variable;
    }
    return bound;
}

/*
 * Analyses the bound expression of let: whether it is plain, and the
 * slots it reads that it does not bind itself (a run of read_slots). A
 * slot bound anywhere in the subtree counts as its own: the IR binds a
 * slot once on a path.
 */
static sxml_status analyse_let(loop_pass *pass, size_t let_index) {
    conjunct_let *let = let_at(pass, let_index);
    if (let->is_analysed) {
        return SXML_STATUS_OK;
    }
    const sxml_ir_expression_index root = let->bound_expression;
    bool is_plain = false;
    sxml_status status = subtree_is_plain(pass, root, &is_plain);
    const size_t first_read_slot = pass->read_slots.element_count;
    /* Reads, then the slots bound inside, marked by a second walk. */
    size_t bound_start = 0;
    if (status == SXML_STATUS_OK && is_plain) {
        pass->walk.element_count = 0;
        status = pass_array_append(pass->allocator, &pass->walk, &root);
        while (status == SXML_STATUS_OK && pass->walk.element_count > 0) {
            pass->walk.element_count -= 1;
            const sxml_ir_expression_index index = ((const sxml_ir_expression_index *)
                                                        pass->walk.elements)[pass->walk
                                                                                    .element_count];
            const sxml_ir_expression expression = expression_at(pass, index);
            const expression_atoms read = atoms_of(&expression);
            for (size_t atom_index = 0; status == SXML_STATUS_OK && atom_index < read.atom_count;
                 atom_index += 1) {
                status = note_read_atom(pass, read.atoms[atom_index]);
            }
            const sxml_ir_atom_view list = sxml_ir_unit_get_atoms(pass->unit, read.list);
            for (size_t atom_index = 0; status == SXML_STATUS_OK && atom_index < list.atom_count;
                 atom_index += 1) {
                status = note_read_atom(pass, list.atoms[atom_index]);
            }
            const expression_children children = children_of(&expression);
            for (size_t child_index = 0;
                 status == SXML_STATUS_OK && child_index < children.child_count; child_index += 1) {
                status = pass_array_append(pass->allocator, &pass->walk,
                                           &children.children[child_index]);
            }
        }
        bound_start = pass->read_slots.element_count;
        pass->walk.element_count = 0;
        if (status == SXML_STATUS_OK) {
            status = pass_array_append(pass->allocator, &pass->walk, &root);
        }
        while (status == SXML_STATUS_OK && pass->walk.element_count > 0) {
            pass->walk.element_count -= 1;
            const sxml_ir_expression_index index = ((const sxml_ir_expression_index *)
                                                        pass->walk.elements)[pass->walk
                                                                                    .element_count];
            const sxml_ir_expression expression = expression_at(pass, index);
            const bound_slots bound = slots_bound_by(&expression);
            for (size_t slot_index = 0; status == SXML_STATUS_OK && slot_index < 3;
                 slot_index += 1) {
                if (bound.slots[slot_index].value != 0) {
                    status = pass_array_append(pass->allocator, &pass->read_slots,
                                               &bound.slots[slot_index]);
                }
            }
            const expression_children children = children_of(&expression);
            for (size_t child_index = 0;
                 status == SXML_STATUS_OK && child_index < children.child_count; child_index += 1) {
                status = pass_array_append(pass->allocator, &pass->walk,
                                           &children.children[child_index]);
            }
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* Keep the reads that are not bound inside, in place. */
    sxml_ir_slot *slots = pass->read_slots.elements;
    size_t kept_count = 0;
    for (size_t read_index = first_read_slot; read_index < bound_start; read_index += 1) {
        bool is_bound_inside = false;
        for (size_t bound_index = bound_start;
             bound_index < pass->read_slots.element_count && !is_bound_inside;
             bound_index += 1) {
            is_bound_inside = slots[bound_index].value == slots[read_index].value;
        }
        if (!is_bound_inside) {
            slots[first_read_slot + kept_count] = slots[read_index];
            kept_count += 1;
        }
    }
    pass->read_slots.element_count = first_read_slot + kept_count;
    let = let_at(pass, let_index);
    let->is_analysed = true;
    let->is_plain = is_plain;
    let->first_read_slot = first_read_slot;
    let->read_slot_count = kept_count;
    return SXML_STATUS_OK;
}

/* What an operand's closure must be. */
typedef enum closure_role {
    /* The key: reads the focus value, and no slot but it and LETs found. */
    SXML_CLOSURE_ROLE_KEY,
    /* The probe: never reads the focus value. */
    SXML_CLOSURE_ROLE_PROBE,
    /* A where clause's condition: may read any slot. */
    SXML_CLOSURE_ROLE_CONDITION,
} closure_role;

typedef struct closure_request {
    sxml_ir_atom operand;
    closure_role role;
} closure_request;

/*
 * Marks (is_in_closure) the LETs the operand reads, directly or through
 * other LETs, and writes whether the operand can play its role.
 */
static sxml_status mark_closure(loop_pass *pass, closure_request request,
                                bool *is_valid) {
    for (size_t let_index = 0; let_index < pass->lets.element_count; let_index += 1) {
        let_at(pass, let_index)->is_in_closure = false;
    }
    *is_valid = true;
    bool reads_focus = false;
    pass->closure_slots.element_count = 0;
    sxml_status status = SXML_STATUS_OK;
    if (request.operand.atom_kind == SXML_IR_ATOM_KIND_SLOT) {
        status = pass_array_append(pass->allocator, &pass->closure_slots,
                                   &request.operand.slot);
    }
    while (status == SXML_STATUS_OK && *is_valid && pass->closure_slots.element_count > 0) {
        pass->closure_slots.element_count -= 1;
        const sxml_ir_slot slot =
            ((const sxml_ir_slot *)pass->closure_slots.elements)[pass->closure_slots
                                                                        .element_count];
        if (slot.value == pass->focus_slot.value) {
            reads_focus = true;
            *is_valid = request.role != SXML_CLOSURE_ROLE_PROBE;
            continue;
        }
        conjunct_let *let = find_let(pass, slot);
        if (let == NULL) {
            /* A slot bound outside the predicate: the probe may read it. */
            *is_valid = request.role != SXML_CLOSURE_ROLE_KEY;
            continue;
        }
        if (let->is_in_closure) {
            continue;
        }
        const size_t let_index = (size_t)(let - let_at(pass, 0));
        status = analyse_let(pass, let_index);
        let = let_at(pass, let_index);
        if (status != SXML_STATUS_OK || let->is_ambiguous || !let->is_plain) {
            *is_valid = false;
            continue;
        }
        let->is_in_closure = true;
        for (size_t read_index = 0; status == SXML_STATUS_OK && read_index < let->read_slot_count;
             read_index += 1) {
            const sxml_ir_slot read =
                ((const sxml_ir_slot *)pass->read_slots.elements)[let->first_read_slot +
                                                                     read_index];
            status = pass_array_append(pass->allocator, &pass->closure_slots, &read);
            let = let_at(pass, let_index);
        }
    }
    if (status == SXML_STATUS_OK && request.role == SXML_CLOSURE_ROLE_KEY && !reads_focus) {
        /* A key that does not depend on the item is no key. */
        *is_valid = false;
    }
    return status;
}

/* ------------------------------------------------------------ Copies */

/* The child fields of expression mapped to the copies of the children. */
static void map_children(const loop_pass *pass, sxml_ir_expression *expression) {
    sxml_ir_expression_index *fields[FILTER_INDEX_CHILD_COUNT] = {NULL, NULL, NULL, NULL};
    if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_LET) {
        fields[0] = &expression->let.bound_expression;
        fields[1] = &expression->let.body;
        /* Copies are evaluated where nothing receives content. */
        expression->let.is_content = false;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_CONDITIONAL) {
        fields[0] = &expression->conditional.then_branch;
        fields[1] = &expression->conditional.else_branch;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_MAP) {
        fields[0] = &expression->map.body;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_FILTER) {
        fields[0] = &expression->filter.predicate;
        fields[1] = &expression->filter.index.key;
        fields[2] = &expression->filter.index.probe;
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_QUANTIFIED) {
        fields[0] = &expression->quantified.condition;
    }
    for (size_t field_index = 0; field_index < FILTER_INDEX_CHILD_COUNT; field_index += 1) {
        if (fields[field_index] != NULL && fields[field_index]->value != 0) {
            fields[field_index]->value = pass->copy_targets[fields[field_index]->value];
        }
    }
}

/* Makes copy_targets cover every expression of the unit. */
static sxml_status cover_copy_targets(loop_pass *pass) {
    const size_t expression_count = pass->unit->expression_count;
    if (pass->copy_target_count >= expression_count) {
        return SXML_STATUS_OK;
    }
    const size_t new_count = expression_count * 2;
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate_array(
        pass->allocator,
        (sxml_array_allocation_request){.element_count = new_count,
                                        .element_size = sizeof(uint32_t),
                                        .element_alignment = alignof(uint32_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (pass->copy_targets != NULL) {
        /* Every entry is 0 between copies. */
        sxml_allocator_release(pass->allocator, pass->copy_targets);
    }
    pass->copy_targets = memory;
    pass->copy_target_count = new_count;
    return SXML_STATUS_OK;
}

/* Adds a copy of the subtree at root (plain kinds only) and writes its
   index; children are copied before their parents. */
static sxml_status copy_subtree(loop_pass *pass, sxml_ir_expression_index root,
                                sxml_ir_expression_index *copy) {
    sxml_status status = cover_copy_targets(pass);
    pass->subtree.element_count = 0;
    pass->walk.element_count = 0;
    if (status == SXML_STATUS_OK) {
        status = pass_array_append(pass->allocator, &pass->walk, &root);
    }
    while (status == SXML_STATUS_OK && pass->walk.element_count > 0) {
        pass->walk.element_count -= 1;
        const sxml_ir_expression_index index =
            ((const sxml_ir_expression_index *)pass->walk.elements)[pass->walk.element_count];
        status = pass_array_append(pass->allocator, &pass->subtree, &index);
        const sxml_ir_expression expression = expression_at(pass, index);
        const expression_children children = children_of(&expression);
        for (size_t child_index = 0; status == SXML_STATUS_OK && child_index < children.child_count;
             child_index += 1) {
            status = pass_array_append(pass->allocator, &pass->walk,
                                       &children.children[child_index]);
        }
    }
    const sxml_ir_expression_index *subtree = pass->subtree.elements;
    for (size_t position = pass->subtree.element_count; status == SXML_STATUS_OK && position > 0;
         position -= 1) {
        const sxml_ir_expression_index source = subtree[position - 1];
        sxml_ir_expression expression = expression_at(pass, source);
        map_children(pass, &expression);
        sxml_ir_expression_index added = {.value = 0};
        status = sxml_ir_unit_add_expression(pass->unit, &expression, &added);
        if (status == SXML_STATUS_OK) {
            pass->copy_targets[source.value] = added.value;
        }
    }
    if (status == SXML_STATUS_OK) {
        *copy = (sxml_ir_expression_index){.value = pass->copy_targets[root.value]};
    }
    for (size_t position = 0; position < pass->subtree.element_count; position += 1) {
        pass->copy_targets[subtree[position].value] = 0;
    }
    return status;
}

/* The operand of an index key or probe and where it comes from. */
typedef struct operand_chain {
    sxml_ir_atom operand;
    sxml_ir_source_span source_span;
} operand_chain;

/*
 * Adds copies of the LETs of the marked closure, in their order, around
 * the expression at body, and writes the outermost.
 */
static sxml_status add_let_chain(loop_pass *pass, sxml_ir_expression_index body,
                                 sxml_ir_expression_index *expression) {
    sxml_status status = SXML_STATUS_OK;
    /* LETs were found in an order that puts each after the ones it
       reads: wrap the latest innermost. */
    for (size_t let_number = pass->lets.element_count;
         status == SXML_STATUS_OK && let_number > 0; let_number -= 1) {
        const conjunct_let let = *let_at(pass, let_number - 1);
        if (!let.is_in_closure) {
            continue;
        }
        sxml_ir_expression_index bound = {.value = 0};
        status = copy_subtree(pass, let.bound_expression, &bound);
        if (status == SXML_STATUS_OK) {
            status = sxml_ir_unit_add_expression(
                pass->unit,
                &(sxml_ir_expression){
                    .expression_kind = SXML_IR_EXPRESSION_KIND_LET,
                    .source_span = expression_at(pass, let.bound_expression).source_span,
                    .let = {.variable = let.slot,
                            .bound_expression = bound,
                            .body = body,
                            .is_content = false}},
                &body);
        }
    }
    if (status == SXML_STATUS_OK) {
        *expression = body;
    }
    return status;
}

/* Adds the expression that computes chain.operand: the marked closure's
   LETs around the operand as an ATOM. */
static sxml_status add_operand_chain(loop_pass *pass, operand_chain chain,
                                     sxml_ir_expression_index *expression) {
    sxml_ir_expression_index body = {.value = 0};
    const sxml_status status = sxml_ir_unit_add_expression(
        pass->unit,
        &(sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM,
                              .source_span = chain.source_span,
                              .atom = chain.operand},
        &body);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_let_chain(pass, body, expression);
}

/* ---------------------------------------------------------- Filters */

/* True when a probe operand may be looked up: not a constant, or a
   constant of strings for "=" and "eq" (the index answers only for
   string-like values and nodes, src/evaluator_index.c). */
static bool probe_may_answer(const loop_pass *pass, sxml_ir_atom probe,
                             sxml_ir_binary_operator key_operator) {
    if (probe.atom_kind != SXML_IR_ATOM_KIND_CONSTANT) {
        return true;
    }
    if (key_operator == SXML_IR_BINARY_OPERATOR_IS) {
        return false;
    }
    const sxml_sequence value = sxml_ir_unit_get_constant(pass->unit, probe.constant);
    sxml_sequence_iterator iterator = sxml_sequence_iterator_start(value);
    for (;;) {
        sxml_iteration_step step;
        if (sxml_sequence_iterator_next(&iterator, NULL, &step) != SXML_STATUS_OK) {
            return false;
        }
        if (!step.has_item) {
            return true;
        }
        const uint32_t primitive = sxml_atomic_item_get_primitive_type(step.item).index;
        if (primitive != SXML_TYPE_XS_STRING && primitive != SXML_TYPE_XS_ANY_URI &&
            primitive != SXML_TYPE_XS_UNTYPED_ATOMIC) {
            return false;
        }
    }
}

/* One way to split a comparison: which operand is the key. */
typedef struct comparison_split {
    sxml_ir_atom key;
    sxml_ir_atom probe;
} comparison_split;

/* Gives the FILTER at index a filter index when its predicate has a
   comparison that splits (see the file comment). */
static sxml_status index_filter(loop_pass *pass, sxml_ir_expression_index index) {
    sxml_ir_expression filter = expression_at(pass, index);
    if (filter.filter.index.key.value != 0 || filter.filter.focus.context_value.value == 0 ||
        filter.filter.focus.context_position.value != 0 ||
        filter.filter.focus.context_size.value != 0) {
        return SXML_STATUS_OK;
    }
    pass->focus_slot = filter.filter.focus.context_value;
    pass->read_slots.element_count = 0;
    sxml_status status = find_conjuncts(pass, filter.filter.predicate);
    /* A probe that is no constant first: a join's probe differs from one
       evaluation of the FILTER to the next, and selects fewer candidates
       than a constant, which is the same for every evaluation. */
    const size_t comparison_count = pass->comparisons.element_count;
    for (size_t attempt = 0; status == SXML_STATUS_OK && attempt < 2 * comparison_count;
         attempt += 1) {
        const bool takes_constants = attempt >= comparison_count;
        const size_t comparison_index = attempt % comparison_count;
        const sxml_ir_expression_index comparison_at =
            ((const sxml_ir_expression_index *)pass->comparisons.elements)[comparison_index];
        const sxml_ir_expression comparison = expression_at(pass, comparison_at);
        const comparison_split splits[2] = {
            {.key = comparison.binary.left_operand, .probe = comparison.binary.right_operand},
            {.key = comparison.binary.right_operand, .probe = comparison.binary.left_operand},
        };
        for (size_t split_index = 0; status == SXML_STATUS_OK && split_index < 2;
             split_index += 1) {
            bool is_valid = false;
            const bool is_constant =
                splits[split_index].probe.atom_kind == SXML_IR_ATOM_KIND_CONSTANT;
            if (is_constant != takes_constants ||
                !probe_may_answer(pass, splits[split_index].probe,
                                  comparison.binary.operator)) {
                continue;
            }
            status = mark_closure(pass,
                                  (closure_request){.operand = splits[split_index].probe,
                                                    .role = SXML_CLOSURE_ROLE_PROBE},
                                  &is_valid);
            if (status != SXML_STATUS_OK || !is_valid) {
                continue;
            }
            sxml_ir_expression_index probe = {.value = 0};
            status = add_operand_chain(
                pass,
                (operand_chain){.operand = splits[split_index].probe,
                                .source_span = comparison.source_span},
                &probe);
            if (status == SXML_STATUS_OK) {
                status = mark_closure(pass,
                                      (closure_request){.operand = splits[split_index].key,
                                                        .role = SXML_CLOSURE_ROLE_KEY},
                                      &is_valid);
            }
            if (status != SXML_STATUS_OK || !is_valid) {
                /* The probe copied is dead weight, as replaced
                   expressions are (src/ir_pass_internal.h). */
                continue;
            }
            sxml_ir_expression_index key = {.value = 0};
            status = add_operand_chain(pass,
                                       (operand_chain){.operand = splits[split_index].key,
                                                       .source_span = comparison.source_span},
                                       &key);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            filter = expression_at(pass, index);
            filter.filter.index = (sxml_ir_filter_index){
                .key = key, .probe = probe, .key_operator = comparison.binary.operator};
            return sxml_ir_unit_replace_expression(
                pass->unit,
                &(sxml_ir_expression_replacement){.index = index, .expression = filter});
        }
    }
    return status;
}

/* ------------------------------------------------- Where clauses */

/* Pushes index to pass->walk, which has room for it. */
static void push_walk_index(loop_pass *pass, sxml_ir_expression_index index) {
    ((sxml_ir_expression_index *)pass->walk.elements)[pass->walk.element_count] = index;
    pass->walk.element_count += 1;
}

/* Pushes the children of a TRY_CATCH, SORT or MERGE expression's list
   (catch handlers, sort keys, merge streams), as they are, to
   pass->walk. */
static sxml_status push_listed_children(loop_pass *pass, const sxml_ir_expression *expression) {
    sxml_status status = SXML_STATUS_OK;
    if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_TRY_CATCH) {
        const sxml_ir_catch_clause_view clauses =
            sxml_ir_unit_get_catch_clauses(pass->unit, expression->try_catch.catch_clauses);
        status = pass_array_reserve(pass->allocator, &pass->walk, clauses.catch_clause_count);
        for (size_t clause_index = 0;
             status == SXML_STATUS_OK && clause_index < clauses.catch_clause_count;
             clause_index += 1) {
            push_walk_index(pass, clauses.catch_clauses[clause_index].handler);
        }
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_SORT) {
        const sxml_ir_sort_key_view keys =
            sxml_ir_unit_get_sort_keys(pass->unit, expression->sort.keys);
        status = pass_array_reserve(pass->allocator, &pass->walk, keys.sort_key_count);
        for (size_t key_index = 0; status == SXML_STATUS_OK && key_index < keys.sort_key_count;
             key_index += 1) {
            push_walk_index(pass, keys.sort_keys[key_index].key);
        }
    } else if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_MERGE) {
        const sxml_ir_merge_stream_view streams =
            sxml_ir_unit_get_merge_streams(pass->unit, expression->merge.streams);
        status = pass_array_reserve(pass->allocator, &pass->walk, streams.merge_stream_count);
        for (size_t stream_index = 0;
             status == SXML_STATUS_OK && stream_index < streams.merge_stream_count;
             stream_index += 1) {
            push_walk_index(pass, streams.merge_streams[stream_index].stream);
        }
    }
    return status;
}

/* Pushes every child expression of the expression at index, of any kind,
   to pass->walk: those of its list (push_listed_children), then its
   other children that are not 0. Every walk of a whole body goes through
   here, so it writes the indices in place, room made once. */
static sxml_status push_every_child(loop_pass *pass, sxml_ir_expression_index index) {
    /* Read in place, not copied: appending to pass->walk does not move
       the unit's expressions. */
    const sxml_ir_expression *expression = sxml_ir_unit_get_expression(pass->unit, index);
    const sxml_ir_expression_index none = {.value = 0};
    sxml_ir_expression_index fixed[3] = {none, none, none};
    sxml_status status = SXML_STATUS_OK;
    switch (expression->expression_kind) {
    case SXML_IR_EXPRESSION_KIND_LET:
        fixed[0] = expression->let.bound_expression;
        fixed[1] = expression->let.body;
        break;
    case SXML_IR_EXPRESSION_KIND_CONDITIONAL:
        fixed[0] = expression->conditional.then_branch;
        fixed[1] = expression->conditional.else_branch;
        break;
    case SXML_IR_EXPRESSION_KIND_MAP:
        fixed[0] = expression->map.body;
        break;
    case SXML_IR_EXPRESSION_KIND_FILTER:
        fixed[0] = expression->filter.predicate;
        fixed[1] = expression->filter.index.key;
        fixed[2] = expression->filter.index.probe;
        break;
    case SXML_IR_EXPRESSION_KIND_QUANTIFIED:
        fixed[0] = expression->quantified.condition;
        break;
    case SXML_IR_EXPRESSION_KIND_TRY_CATCH:
        status = push_listed_children(pass, expression);
        fixed[0] = expression->try_catch.body;
        fixed[1] = expression->try_catch.finally_body;
        break;
    case SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_SHALLOW_COPY:
        fixed[0] = expression->node_construction.content;
        break;
    case SXML_IR_EXPRESSION_KIND_SORT:
        return push_listed_children(pass, expression);
    case SXML_IR_EXPRESSION_KIND_STREAM:
        fixed[0] = expression->stream.predicate;
        fixed[1] = expression->stream.body;
        break;
    case SXML_IR_EXPRESSION_KIND_MERGE:
        status = push_listed_children(pass, expression);
        fixed[0] = expression->merge.order;
        fixed[1] = expression->merge.body;
        break;
    case SXML_IR_EXPRESSION_KIND_NONE:
    case SXML_IR_EXPRESSION_KIND_ATOM:
    case SXML_IR_EXPRESSION_KIND_SEQUENCE:
    case SXML_IR_EXPRESSION_KIND_BINARY:
    case SXML_IR_EXPRESSION_KIND_UNARY:
    case SXML_IR_EXPRESSION_KIND_FUNCTION_DEFINITION:
    case SXML_IR_EXPRESSION_KIND_FUNCTION_REFERENCE:
    case SXML_IR_EXPRESSION_KIND_STATIC_CALL:
    case SXML_IR_EXPRESSION_KIND_DYNAMIC_CALL:
    case SXML_IR_EXPRESSION_KIND_LOOKUP:
    case SXML_IR_EXPRESSION_KIND_STEP:
    case SXML_IR_EXPRESSION_KIND_DOCUMENT_ORDER:
    case SXML_IR_EXPRESSION_KIND_CAST:
    case SXML_IR_EXPRESSION_KIND_CASTABLE:
    case SXML_IR_EXPRESSION_KIND_INSTANCE_OF:
    case SXML_IR_EXPRESSION_KIND_TREAT:
    case SXML_IR_EXPRESSION_KIND_COERCE:
    case SXML_IR_EXPRESSION_KIND_RAISE:
    case SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_ARRAY_CONSTRUCTOR:
    case SXML_IR_EXPRESSION_KIND_APPLY_TEMPLATES:
    case SXML_IR_EXPRESSION_KIND_GLOBAL_READ:
    case SXML_IR_EXPRESSION_KIND_COUNT:
        return SXML_STATUS_OK;
    }
    if (status == SXML_STATUS_OK) {
        status = pass_array_reserve(pass->allocator, &pass->walk, 3);
    }
    for (size_t fixed_index = 0; status == SXML_STATUS_OK && fixed_index < 3; fixed_index += 1) {
        if (fixed[fixed_index].value != 0) {
            push_walk_index(pass, fixed[fixed_index]);
        }
    }
    return status;
}

/* True for an expression the loop passes leave alone: unknown to owners
   (not in a function body when they were made) or inside a STREAM. */
static bool is_left_alone(const loop_pass *pass, sxml_ir_expression_index index) {
    return index.value >= pass->owner_count || pass->owners[index.value] == 0 ||
           pass->is_in_stream[index.value];
}

/* Makes owners: walks every function body. */
static sxml_status make_owners(loop_pass *pass, uint32_t expression_count) {
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        pass->allocator,
        (sxml_array_allocation_request){.element_count = expression_count,
                                        .element_size = sizeof(uint32_t),
                                        .element_alignment = alignof(uint32_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    pass->owners = memory;
    pass->owner_count = expression_count;
    status = sxml_allocator_allocate_array(
        pass->allocator,
        (sxml_array_allocation_request){.element_count = expression_count,
                                        .element_size = sizeof(bool),
                                        .element_alignment = alignof(bool)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    pass->is_in_stream = memory;
    for (uint32_t function_number = 1;
         status == SXML_STATUS_OK && function_number < pass->unit->function_count;
         function_number += 1) {
        const sxml_ir_function *function = sxml_ir_unit_get_function(
            pass->unit, (sxml_ir_function_index){.value = function_number});
        pass->walk.element_count = 0;
        if (function->body.value != 0) {
            status = pass_array_append(pass->allocator, &pass->walk, &function->body);
        }
        while (status == SXML_STATUS_OK && pass->walk.element_count > 0) {
            pass->walk.element_count -= 1;
            const sxml_ir_expression_index index = ((const sxml_ir_expression_index *)
                                                        pass->walk.elements)[pass->walk
                                                                                    .element_count];
            if (index.value == 0 || index.value >= expression_count ||
                pass->owners[index.value] != 0) {
                continue;
            }
            pass->owners[index.value] = function_number;
            const size_t first_child = pass->walk.element_count;
            status = push_every_child(pass, index);
            const sxml_ir_expression_kind kind =
                sxml_ir_unit_get_expression(pass->unit, index)->expression_kind;
            const bool marks_children = pass->is_in_stream[index.value] ||
                                        kind == SXML_IR_EXPRESSION_KIND_STREAM ||
                                        kind == SXML_IR_EXPRESSION_KIND_MERGE;
            for (size_t child = first_child; marks_children && child < pass->walk.element_count;
                 child += 1) {
                const sxml_ir_expression_index child_index =
                    ((const sxml_ir_expression_index *)pass->walk.elements)[child];
                if (child_index.value < expression_count) {
                    pass->is_in_stream[child_index.value] = true;
                }
            }
        }
    }
    return status;
}

/* The expressions of owners grouped by function, each group in index
   order: the expression numbers of function f are
   expression_numbers[starts[f]] to expression_numbers[starts[f + 1] - 1]. */
typedef struct owned_expressions {
    uint32_t *starts;
    uint32_t *expression_numbers;
} owned_expressions;

static void owned_expressions_finalize(const loop_pass *pass, owned_expressions *owned) {
    if (owned->starts != NULL) {
        sxml_allocator_release(pass->allocator, owned->starts);
    }
    if (owned->expression_numbers != NULL) {
        sxml_allocator_release(pass->allocator, owned->expression_numbers);
    }
    *owned = (owned_expressions){.starts = NULL, .expression_numbers = NULL};
}

/* Groups the expressions of owners by function, one counting sort, so a
   pass that visits each function's expressions reads each expression
   once, not once per function. */
static sxml_status make_owned_expressions(const loop_pass *pass, owned_expressions *owned) {
    const size_t function_count = pass->unit->function_count;
    void *memory = NULL;
    sxml_status status = sxml_allocator_allocate_array(
        pass->allocator,
        (sxml_array_allocation_request){.element_count = function_count + 1U,
                                        .element_size = sizeof(uint32_t),
                                        .element_alignment = alignof(uint32_t)},
        &memory);
    owned->starts = memory;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(
            pass->allocator,
            (sxml_array_allocation_request){.element_count = pass->owner_count + 1U,
                                            .element_size = sizeof(uint32_t),
                                            .element_alignment = alignof(uint32_t)},
            &memory);
        owned->expression_numbers = memory;
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* starts[f + 1] counts f's expressions, then holds where f's end. */
    for (size_t number = 1; number < pass->owner_count; number += 1) {
        const uint32_t owner = pass->owners[number];
        if (owner != 0 && owner < function_count) {
            owned->starts[owner + 1U] += 1;
        }
    }
    for (size_t function_number = 1; function_number < function_count; function_number += 1) {
        owned->starts[function_number + 1U] += owned->starts[function_number];
    }
    /* Ascending, so each group is in index order; starts[f] moves to
       where f's group ends and is set back below. */
    for (uint32_t number = 1; number < pass->owner_count; number += 1) {
        const uint32_t owner = pass->owners[number];
        if (owner != 0 && owner < function_count) {
            owned->expression_numbers[owned->starts[owner]] = number;
            owned->starts[owner] += 1;
        }
    }
    for (size_t function_number = function_count - 1U; function_number > 0;
         function_number -= 1) {
        owned->starts[function_number] = owned->starts[function_number - 1U];
    }
    owned->starts[0] = 0;
    return SXML_STATUS_OK;
}

/* The expressions of one function, a run of owned_expressions. */
typedef struct owned_function {
    uint32_t function_number;
    const uint32_t *expression_numbers;
    uint32_t first_owned_number;
    uint32_t end_owned_number;
} owned_function;

static owned_function owned_function_of(const owned_expressions *owned,
                                        uint32_t function_number) {
    return (owned_function){.function_number = function_number,
                            .expression_numbers = owned->expression_numbers,
                            .first_owned_number = owned->starts[function_number],
                            .end_owned_number = owned->starts[function_number + 1U]};
}

/* The constant of a boolean, added on first need. */
static sxml_status boolean_constant(loop_pass *pass, bool value,
                                    sxml_ir_constant_index *constant) {
    sxml_ir_constant_index *kept = value ? &pass->true_constant : &pass->false_constant;
    if (kept->value == 0) {
        const sxml_status status = sxml_ir_unit_add_constant(
            pass->unit,
            sxml_sequence_from_item(value ? sxml_atomic_item_true() : sxml_atomic_item_false()),
            kept);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    *constant = *kept;
    return SXML_STATUS_OK;
}

/* An ATOM expression of a constant. */
static sxml_status add_constant_atom(loop_pass *pass, sxml_ir_constant_index constant,
                                     sxml_ir_expression_index *expression) {
    return sxml_ir_unit_add_expression(
        pass->unit,
        &(sxml_ir_expression){
            .expression_kind = SXML_IR_EXPRESSION_KIND_ATOM,
            .source_span = {.byte_offset = 0, .byte_count = 0, .line_number = 0, .column_number = 0},
            .atom = {.atom_kind = SXML_IR_ATOM_KIND_CONSTANT, .constant = constant}},
        expression);
}

/*
 * The where clause of a for clause (XQuery 4.0, 4.13.2, 4.13.7): a MAP
 * over the for clause's input (no flags, no positional variable) whose
 * body, after LETs, is a CONDITIONAL with the where condition and an
 * empty else branch. When the FILTER of the input by the condition,
 *
 *     FILTER(input, focus, copies of the condition's LETs
 *                          CONDITIONAL(condition, true(), false()))
 *
 * gets a filter index, the MAP becomes LET(new slot, that FILTER, the MAP
 * over the new slot): the body runs for the items the condition holds
 * for, as before, and tests it again. Errors may then come in another
 * order, which XQuery 4.0, 2.4.4 allows.
 */
static sxml_status index_where_clause(loop_pass *pass, sxml_ir_expression_index index) {
    if (is_left_alone(pass, index)) {
        return SXML_STATUS_OK;
    }
    const sxml_ir_expression map = expression_at(pass, index);
    if (map.map.flags != 0 || map.map.focus.context_value.value == 0 ||
        map.map.focus.context_position.value != 0 || map.map.focus.context_size.value != 0) {
        return SXML_STATUS_OK;
    }
    pass->focus_slot = map.map.focus.context_value;
    pass->lets.element_count = 0;
    pass->read_slots.element_count = 0;
    sxml_ir_expression_index body = map.map.body;
    sxml_status status = SXML_STATUS_OK;
    sxml_ir_atom condition = {.atom_kind = SXML_IR_ATOM_KIND_NONE};
    while (status == SXML_STATUS_OK && body.value != 0) {
        const sxml_ir_expression expression = expression_at(pass, body);
        body = (sxml_ir_expression_index){.value = 0};
        if (expression.expression_kind == SXML_IR_EXPRESSION_KIND_LET) {
            status = add_let(pass, &expression.let);
            body = expression.let.body;
        } else if (expression.expression_kind == SXML_IR_EXPRESSION_KIND_CONDITIONAL &&
                   expression_is_false(pass, expression.conditional.else_branch)) {
            condition = expression.conditional.condition;
        }
    }
    if (status != SXML_STATUS_OK || condition.atom_kind != SXML_IR_ATOM_KIND_SLOT) {
        return status;
    }
    bool is_valid = false;
    status = mark_closure(
        pass, (closure_request){.operand = condition, .role = SXML_CLOSURE_ROLE_CONDITION},
        &is_valid);
    if (status != SXML_STATUS_OK || !is_valid) {
        return status;
    }
    sxml_ir_constant_index true_constant = {.value = 0};
    sxml_ir_constant_index false_constant = {.value = 0};
    sxml_ir_expression_index then_branch = {.value = 0};
    sxml_ir_expression_index else_branch = {.value = 0};
    sxml_ir_expression_index test = {.value = 0};
    sxml_ir_expression_index predicate = {.value = 0};
    sxml_ir_expression_index filter = {.value = 0};
    status = boolean_constant(pass, true, &true_constant);
    if (status == SXML_STATUS_OK) {
        status = boolean_constant(pass, false, &false_constant);
    }
    if (status == SXML_STATUS_OK) {
        status = add_constant_atom(pass, true_constant, &then_branch);
    }
    if (status == SXML_STATUS_OK) {
        status = add_constant_atom(pass, false_constant, &else_branch);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_expression(
            pass->unit,
            &(sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                                  .source_span = map.source_span,
                                  .conditional = {.condition = condition,
                                                  .then_branch = then_branch,
                                                  .else_branch = else_branch}},
            &test);
    }
    if (status == SXML_STATUS_OK) {
        status = add_let_chain(pass, test, &predicate);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_expression(
            pass->unit,
            &(sxml_ir_expression){
                .expression_kind = SXML_IR_EXPRESSION_KIND_FILTER,
                .source_span = map.source_span,
                .filter = {.input = map.map.input,
                           .focus = {.context_value = map.map.focus.context_value},
                           .predicate = predicate,
                           .index = {.key = {.value = 0}, .probe = {.value = 0}}}},
            &filter);
    }
    if (status == SXML_STATUS_OK) {
        status = index_filter(pass, filter);
    }
    if (status != SXML_STATUS_OK ||
        expression_at(pass, filter).filter.index.key.value == 0) {
        /* Not indexed: the FILTER made is dead weight. */
        return status;
    }
    /* The MAP's function gets a slot for the filtered input. */
    const uint32_t owner = index.value < pass->owner_count ? pass->owners[index.value] : 0;
    if (owner == 0) {
        return SXML_STATUS_OK;
    }
    sxml_ir_function function =
        *sxml_ir_unit_get_function(pass->unit, (sxml_ir_function_index){.value = owner});
    if (function.slot_count == UINT32_MAX) {
        return SXML_STATUS_OK;
    }
    function.slot_count += 1;
    const sxml_ir_slot filtered = {.value = function.slot_count};
    sxml_ir_expression new_map = map;
    new_map.map.input = sxml_ir_slot_atom(filtered);
    sxml_ir_expression_index moved_map = {.value = 0};
    status = sxml_ir_unit_add_expression(pass->unit, &new_map, &moved_map);
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_replace_function(
            pass->unit, &(sxml_ir_function_replacement){
                               .index = {.value = owner}, .function = function});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_replace_expression(
            pass->unit,
            &(sxml_ir_expression_replacement){
                .index = index,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_LET,
                               .source_span = map.source_span,
                               .let = {.variable = filtered,
                                       .bound_expression = filter,
                                       .body = moved_map,
                                       .is_content = false}}});
    }
    return status;
}

/* --------------------------------------------------------- Hoisting */

/* A LET at the start of a loop body. */
typedef struct prefix_let {
    sxml_ir_expression_index index;
    sxml_ir_let let;
    bool is_hoisted;
} prefix_let;

/* True for a LET the passes may move: not a content LET (D166), whose
   place in its block matters when it delivers. */
static bool is_movable_let(const sxml_ir_expression *expression) {
    return expression->expression_kind == SXML_IR_EXPRESSION_KIND_LET &&
           !expression->let.is_content;
}

/*
 * Flattens the LETs at the start of the expression at body: a LET whose
 * bound expression is a LET, LET(l, LET(a, e, B), C), becomes LET(a, e,
 * LET(l, B, C)), in place. The same value, computed in the same order: C
 * cannot read a, which is bound inside l's bound expression only.
 */
static sxml_status flatten_lets(loop_pass *pass, sxml_ir_expression_index body) {
    sxml_ir_expression_index current = body;
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && current.value != 0) {
        const sxml_ir_expression outer = expression_at(pass, current);
        if (!is_movable_let(&outer)) {
            break;
        }
        const sxml_ir_expression_index bound_index = outer.let.bound_expression;
        const sxml_ir_expression bound = expression_at(pass, bound_index);
        if (!is_movable_let(&bound)) {
            current = outer.let.body;
            continue;
        }
        sxml_ir_expression new_outer = bound;
        new_outer.let.body = bound_index;
        sxml_ir_expression new_inner = outer;
        new_inner.let.bound_expression = bound.let.body;
        status = sxml_ir_unit_replace_expression(
            pass->unit,
            &(sxml_ir_expression_replacement){.index = current, .expression = new_outer});
        if (status == SXML_STATUS_OK) {
            status = sxml_ir_unit_replace_expression(
                pass->unit,
                &(sxml_ir_expression_replacement){.index = bound_index, .expression = new_inner});
        }
        /* current now binds a; look at it again, its e may be a LET. */
    }
    return status;
}

/* True when slot is one of the loop's focus slots or bound by a LET of
   the prefix that stays in the loop. */
typedef struct variant_slots {
    sxml_ir_focus focus;
    const prefix_let *lets;
    size_t let_count;
} variant_slots;

static bool is_variant(variant_slots variants, sxml_ir_slot slot) {
    if (slot.value == variants.focus.context_value.value ||
        slot.value == variants.focus.context_position.value ||
        slot.value == variants.focus.context_size.value) {
        return slot.value != 0;
    }
    for (size_t let_index = 0; let_index < variants.let_count; let_index += 1) {
        if (!variants.lets[let_index].is_hoisted &&
            variants.lets[let_index].let.variable.value == slot.value) {
            return true;
        }
    }
    return false;
}

/* What hoisting a loop needs to know of it. */
typedef struct hoisted_loop {
    sxml_ir_expression_index index;
    sxml_ir_atom input;
    sxml_ir_focus focus;
    sxml_ir_expression_index body;
} hoisted_loop;

/* Marks the prefix LETs whose bound expression is plain and reads no slot
   the loop binds (its focus, or a LET of the prefix that stays). */
static sxml_status mark_invariant_lets(loop_pass *pass, const hoisted_loop *loop,
                                       pass_array *prefix) {
    sxml_status status = SXML_STATUS_OK;
    for (size_t let_index = 0; status == SXML_STATUS_OK && let_index < prefix->element_count;
         let_index += 1) {
        /* analyse_let works on pass->lets: one entry for this LET. */
        prefix_let *candidate = &((prefix_let *)prefix->elements)[let_index];
        pass->lets.element_count = 0;
        pass->read_slots.element_count = 0;
        status = add_let(pass, &candidate->let);
        if (status == SXML_STATUS_OK) {
            status = analyse_let(pass, 0);
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        const conjunct_let *analysed = let_at(pass, 0);
        bool is_invariant = analysed->is_plain;
        const sxml_ir_slot *reads = pass->read_slots.elements;
        for (size_t read_index = 0; is_invariant && read_index < analysed->read_slot_count;
             read_index += 1) {
            is_invariant = !is_variant(
                (variant_slots){.focus = loop->focus,
                                .lets = prefix->elements,
                                .let_count = let_index},
                reads[analysed->first_read_slot + read_index]);
        }
        candidate = &((prefix_let *)prefix->elements)[let_index];
        candidate->is_hoisted = is_invariant;
    }
    return status;
}

/* An expression the loop pass adds, at the loop's source span. */
static sxml_status add_loop_expression(loop_pass *pass, sxml_ir_expression expression,
                                       sxml_ir_expression_index *index) {
    return sxml_ir_unit_add_expression(pass->unit, &expression, index);
}

/*
 * Rebuilds the loop at loop->index with the marked LETs outside it:
 *
 *     LET(guard, QUANTIFIED(some, input, true()),
 *       LET(s1, CONDITIONAL(guard, e1, ()), ...
 *         LOOP with the other LETs))
 *
 * The guard keeps a hoisted expression from running for an empty input,
 * where the loop never ran its body (so no error comes that would not
 * have come).
 */
static sxml_status hoist_marked_lets(loop_pass *pass, const hoisted_loop *loop,
                                     const pass_array *prefix) {
    const prefix_let *lets = prefix->elements;
    const size_t let_count = prefix->element_count;
    const uint32_t owner = loop->index.value < pass->owner_count ? pass->owners[loop->index.value] : 0;
    if (owner == 0) {
        return SXML_STATUS_OK;
    }
    sxml_ir_function function =
        *sxml_ir_unit_get_function(pass->unit, (sxml_ir_function_index){.value = owner});
    if (function.slot_count > UINT32_MAX - 2) {
        return SXML_STATUS_OK;
    }
    const sxml_ir_slot guard = {.value = function.slot_count + 1};
    const sxml_ir_slot quantified = {.value = function.slot_count + 2};
    function.slot_count += 2;
    /* The body without the hoisted LETs: each LET that stays reads on to
       the next one that stays, the last to what followed the prefix. */
    sxml_ir_expression_index rest = expression_at(pass, lets[let_count - 1].index).let.body;
    sxml_status status = SXML_STATUS_OK;
    for (size_t let_number = let_count; status == SXML_STATUS_OK && let_number > 0;
         let_number -= 1) {
        const prefix_let *staying = &lets[let_number - 1];
        if (staying->is_hoisted) {
            continue;
        }
        sxml_ir_expression expression = expression_at(pass, staying->index);
        expression.let.body = rest;
        status = sxml_ir_unit_replace_expression(
            pass->unit,
            &(sxml_ir_expression_replacement){.index = staying->index, .expression = expression});
        rest = staying->index;
    }
    sxml_ir_expression moved = expression_at(pass, loop->index);
    if (moved.expression_kind == SXML_IR_EXPRESSION_KIND_MAP) {
        moved.map.body = rest;
    } else if (moved.expression_kind == SXML_IR_EXPRESSION_KIND_QUANTIFIED) {
        moved.quantified.condition = rest;
    } else {
        moved.filter.predicate = rest;
    }
    sxml_ir_expression_index inner = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = add_loop_expression(pass, moved, &inner);
    }
    const sxml_ir_source_span span = moved.source_span;
    for (size_t let_number = let_count; status == SXML_STATUS_OK && let_number > 0;
         let_number -= 1) {
        const prefix_let *hoisted = &lets[let_number - 1];
        if (!hoisted->is_hoisted) {
            continue;
        }
        sxml_ir_expression_index empty = {.value = 0};
        sxml_ir_expression_index guarded = {.value = 0};
        status = add_loop_expression(
            pass,
            (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                                 .source_span = span,
                                 .sequence = {.first_atom_index = 0, .atom_count = 0}},
            &empty);
        if (status == SXML_STATUS_OK) {
            status = add_loop_expression(
                pass,
                (sxml_ir_expression){
                    .expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                    .source_span = span,
                    .conditional = {.condition = sxml_ir_slot_atom(guard),
                                    .then_branch = hoisted->let.bound_expression,
                                    .else_branch = empty}},
                &guarded);
        }
        if (status == SXML_STATUS_OK) {
            sxml_ir_expression expression = expression_at(pass, hoisted->index);
            expression.let.bound_expression = guarded;
            expression.let.body = inner;
            status = sxml_ir_unit_replace_expression(
                pass->unit,
                &(sxml_ir_expression_replacement){.index = hoisted->index, .expression = expression});
            inner = hoisted->index;
        }
    }
    sxml_ir_constant_index true_constant = {.value = 0};
    sxml_ir_expression_index condition = {.value = 0};
    sxml_ir_expression_index exists = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = boolean_constant(pass, true, &true_constant);
    }
    if (status == SXML_STATUS_OK) {
        status = add_constant_atom(pass, true_constant, &condition);
    }
    if (status == SXML_STATUS_OK) {
        status = add_loop_expression(
            pass,
            (sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_QUANTIFIED,
                                 .source_span = span,
                                 .quantified = {.quantifier = SXML_IR_QUANTIFIER_SOME,
                                                .input = loop->input,
                                                .variable = quantified,
                                                .condition = condition}},
            &exists);
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_replace_function(
            pass->unit,
            &(sxml_ir_function_replacement){.index = {.value = owner}, .function = function});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_replace_expression(
            pass->unit,
            &(sxml_ir_expression_replacement){
                .index = loop->index,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_LET,
                               .source_span = span,
                               .let = {.variable = guard,
                                       .bound_expression = exists,
                                       .body = inner,
                                       .is_content = false}}});
    }
    return status;
}

/*
 * Moves the LETs at the start of a loop's body (a MAP without flags, a
 * FILTER) whose bound expressions are plain and read nothing the loop
 * binds out of the loop, so they are computed once and not once per item
 * (XQuery 4.0, 2.4.4: an expression's value does not depend on how often
 * it is computed; plain expressions make no nodes).
 */
static sxml_status hoist_loop(loop_pass *pass, sxml_ir_expression_index index) {
    if (is_left_alone(pass, index)) {
        return SXML_STATUS_OK;
    }
    const sxml_ir_expression expression = expression_at(pass, index);
    hoisted_loop loop = {.index = index};
    if (expression.expression_kind == SXML_IR_EXPRESSION_KIND_MAP) {
        if (expression.map.flags != 0) {
            return SXML_STATUS_OK;
        }
        loop.input = expression.map.input;
        loop.focus = expression.map.focus;
        loop.body = expression.map.body;
    } else if (expression.expression_kind == SXML_IR_EXPRESSION_KIND_QUANTIFIED) {
        /* The condition runs once per item with the variable bound: the
           variable is what the loop binds (D229). */
        loop.input = expression.quantified.input;
        loop.focus = (sxml_ir_focus){.context_value = expression.quantified.variable,
                                     .context_position = {.value = 0},
                                     .context_size = {.value = 0}};
        loop.body = expression.quantified.condition;
    } else {
        /* A FILTER with a filter index tests its predicate on the index's
           candidates only (D224), which may be none of a non-empty input:
           the guard (the input is not empty) would then run a hoisted
           LET the predicate never ran, and raise its error. The guard
           rule holds only for loops that run their body for the first
           item of every non-empty input (packet 4.21). */
        if (expression.filter.index.key.value != 0) {
            return SXML_STATUS_OK;
        }
        loop.input = expression.filter.input;
        loop.focus = expression.filter.focus;
        loop.body = expression.filter.predicate;
    }
    sxml_status status = flatten_lets(pass, loop.body);
    pass_array prefix = pass_array_start(sizeof(prefix_let), alignof(prefix_let));
    sxml_ir_expression_index current = loop.body;
    while (status == SXML_STATUS_OK && current.value != 0) {
        const sxml_ir_expression let = expression_at(pass, current);
        if (!is_movable_let(&let)) {
            break;
        }
        const prefix_let found = {.index = current, .let = let.let, .is_hoisted = false};
        status = pass_array_append(pass->allocator, &prefix, &found);
        current = let.let.body;
    }
    if (status == SXML_STATUS_OK && prefix.element_count > 0) {
        status = mark_invariant_lets(pass, &loop, &prefix);
    }
    bool has_hoisted = false;
    for (size_t let_index = 0; status == SXML_STATUS_OK && let_index < prefix.element_count;
         let_index += 1) {
        has_hoisted = has_hoisted || ((const prefix_let *)prefix.elements)[let_index].is_hoisted;
    }
    if (status == SXML_STATUS_OK && has_hoisted) {
        status = hoist_marked_lets(pass, &loop, &prefix);
    }
    pass_array_finalize(pass->allocator, &prefix);
    return status;
}

/* ---------------------------------------------------- Slot census */

/* What the body of one function does with one of its slots. */
typedef struct slot_use {
    uint32_t read_count;
    /* The LETs that bind it, and the last one found. */
    uint32_t let_count;
    sxml_ir_expression_index let;
    /* The other expressions that bind it (a focus, a variable). */
    uint32_t other_binding_count;
} slot_use;

/* The slot uses of one function: uses[slot], slot 0 unused. */
typedef struct slot_census {
    slot_use *uses;
    size_t use_count;
} slot_census;

static void count_read(slot_census *census, sxml_ir_atom atom) {
    if (atom.atom_kind == SXML_IR_ATOM_KIND_SLOT && atom.slot.value < census->use_count) {
        census->uses[atom.slot.value].read_count += 1;
    }
}

static void count_list_reads(const loop_pass *pass, slot_census *census, sxml_ir_atom_list list) {
    const sxml_ir_atom_view atoms = sxml_ir_unit_get_atoms(pass->unit, list);
    for (size_t atom_index = 0; atom_index < atoms.atom_count; atom_index += 1) {
        count_read(census, atoms.atoms[atom_index]);
    }
}

/* Counts the slots an expression of any kind reads in its own fields (as
   src/ir_global_read.c finds the globals it reads). */
static void count_own_reads(const loop_pass *pass, const sxml_ir_expression *expression,
                            slot_census *census) {
    const expression_atoms read = atoms_of(expression);
    for (size_t atom_index = 0; atom_index < read.atom_count; atom_index += 1) {
        count_read(census, read.atoms[atom_index]);
    }
    count_list_reads(pass, census, read.list);
    const sxml_ir_expression_kind kind = expression->expression_kind;
    if (kind == SXML_IR_EXPRESSION_KIND_FUNCTION_DEFINITION) {
        count_list_reads(
            pass, census,
            sxml_ir_unit_get_function(pass->unit, expression->function_definition.function)
                ->captured_variables);
    } else if (kind == SXML_IR_EXPRESSION_KIND_DYNAMIC_CALL) {
        count_read(census, expression->dynamic_call.function_item);
        count_list_reads(pass, census, expression->dynamic_call.arguments);
    } else if (kind == SXML_IR_EXPRESSION_KIND_SORT) {
        count_read(census, expression->sort.input);
        const sxml_ir_sort_key_view keys =
            sxml_ir_unit_get_sort_keys(pass->unit, expression->sort.keys);
        for (size_t key_index = 0; key_index < keys.sort_key_count; key_index += 1) {
            count_read(census, keys.sort_keys[key_index].collation);
            count_read(census, keys.sort_keys[key_index].order);
        }
    } else if (kind == SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR) {
        const sxml_ir_map_entry_view entries =
            sxml_ir_unit_get_map_entries(pass->unit, expression->map_constructor.entries);
        for (size_t entry_index = 0; entry_index < entries.map_entry_count; entry_index += 1) {
            count_read(census, entries.map_entries[entry_index].key);
            count_read(census, entries.map_entries[entry_index].value);
        }
    } else if (kind == SXML_IR_EXPRESSION_KIND_ARRAY_CONSTRUCTOR) {
        count_list_reads(pass, census, expression->array_constructor.members);
    } else if (kind == SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR ||
               kind == SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR ||
               kind == SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR ||
               kind == SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR ||
               kind == SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR ||
               kind == SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR ||
               kind == SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR ||
               kind == SXML_IR_EXPRESSION_KIND_SHALLOW_COPY) {
        count_read(census, expression->node_construction.name);
        count_read(census, expression->node_construction.value);
    } else if (kind == SXML_IR_EXPRESSION_KIND_APPLY_TEMPLATES) {
        count_read(census, expression->apply_templates.select);
        count_read(census, expression->apply_templates.parameters);
        count_read(census, expression->apply_templates.tunnel_parameters);
        count_read(census, expression->apply_templates.separator);
    } else if (kind == SXML_IR_EXPRESSION_KIND_STREAM) {
        count_read(census, expression->stream.origin);
    } else if (kind == SXML_IR_EXPRESSION_KIND_MERGE) {
        count_read(census, expression->merge.tuples);
        const sxml_ir_merge_stream_view streams =
            sxml_ir_unit_get_merge_streams(pass->unit, expression->merge.streams);
        for (size_t stream_index = 0; stream_index < streams.merge_stream_count;
             stream_index += 1) {
            count_read(census, streams.merge_streams[stream_index].anchors);
        }
    }
}

/* Counts the slots an expression binds for its children. */
static void count_bindings(sxml_ir_expression_index index, const sxml_ir_expression *expression,
                           slot_census *census) {
    if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_LET) {
        const uint32_t slot = expression->let.variable.value;
        if (slot < census->use_count) {
            census->uses[slot].let_count += 1;
            census->uses[slot].let = index;
        }
        return;
    }
    bound_slots bound = slots_bound_by(expression);
    if (expression->expression_kind == SXML_IR_EXPRESSION_KIND_SORT) {
        bound.slots[0] = expression->sort.focus.context_value;
        bound.slots[1] = expression->sort.focus.context_position;
        bound.slots[2] = expression->sort.focus.context_size;
    }
    for (size_t slot_index = 0; slot_index < 3; slot_index += 1) {
        const uint32_t slot = bound.slots[slot_index].value;
        if (slot != 0 && slot < census->use_count) {
            census->uses[slot].other_binding_count += 1;
        }
    }
}

static void slot_census_finalize(const loop_pass *pass, slot_census *census) {
    if (census->uses != NULL) {
        sxml_allocator_release(pass->allocator, census->uses);
    }
    census->uses = NULL;
    census->use_count = 0;
}

/* Starts the census of the body of the function at function_number with
   no uses; census_count_expression then counts each of its expressions,
   in any order. */
static sxml_status start_slot_census(const loop_pass *pass, uint32_t function_number,
                                     slot_census *census) {
    const sxml_ir_function *function = sxml_ir_unit_get_function(
        pass->unit, (sxml_ir_function_index){.value = function_number});
    census->use_count = (size_t)function->slot_count + 1;
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate_array(
        pass->allocator,
        (sxml_array_allocation_request){.element_count = census->use_count,
                                        .element_size = sizeof(slot_use),
                                        .element_alignment = alignof(slot_use)},
        &memory);
    if (status != SXML_STATUS_OK) {
        census->use_count = 0;
        return status;
    }
    census->uses = memory;
    memset(census->uses, 0, census->use_count * sizeof(slot_use));
    return SXML_STATUS_OK;
}

/* Counts the slots the expression at index reads and binds. */
static void census_count_expression(const loop_pass *pass, sxml_ir_expression_index index,
                                    slot_census *census) {
    const sxml_ir_expression *expression = sxml_ir_unit_get_expression(pass->unit, index);
    count_own_reads(pass, expression, census);
    count_bindings(index, expression, census);
}

/* Makes the census of the function at function_number from the
   expressions owners gives it (a body is a tree, so they are the ones a
   walk of the body finds), without a walk. */
static sxml_status make_slot_census(const loop_pass *pass, owned_function owned,
                                    slot_census *census) {
    const sxml_status status = start_slot_census(pass, owned.function_number, census);
    for (uint32_t owned_number = owned.first_owned_number;
         status == SXML_STATUS_OK && owned_number < owned.end_owned_number; owned_number += 1) {
        census_count_expression(
            pass, (sxml_ir_expression_index){.value = owned.expression_numbers[owned_number]},
            census);
    }
    return status;
}

/* The LET that binds slot, when one LET binds it and nothing else does;
   0 otherwise. */
static sxml_ir_expression_index single_let_of(const slot_census *census, sxml_ir_atom atom) {
    const sxml_ir_expression_index none = {.value = 0};
    if (atom.atom_kind != SXML_IR_ATOM_KIND_SLOT || atom.slot.value == 0 ||
        atom.slot.value >= census->use_count) {
        return none;
    }
    const slot_use *use = &census->uses[atom.slot.value];
    return use->let_count == 1 && use->other_binding_count == 0 ? use->let : none;
}

static uint32_t read_count_of(const slot_census *census, sxml_ir_slot slot) {
    return slot.value < census->use_count ? census->uses[slot.value].read_count : 0;
}

/* ------------------------------------------- Values that are no number */

/* True for an operator whose value has no number: comparisons give a
   boolean, union, intersect and except nodes, "||" a string (XPath 4.0,
   4.11, 4.7.5, 4.10.1). */
static bool binary_gives_no_number(sxml_ir_binary_operator binary_operator) {
    return binary_operator != SXML_IR_BINARY_OPERATOR_ADD &&
           binary_operator != SXML_IR_BINARY_OPERATOR_SUBTRACT &&
           binary_operator != SXML_IR_BINARY_OPERATOR_MULTIPLY &&
           binary_operator != SXML_IR_BINARY_OPERATOR_DIVIDE &&
           binary_operator != SXML_IR_BINARY_OPERATOR_INTEGER_DIVIDE &&
           binary_operator != SXML_IR_BINARY_OPERATOR_MODULO &&
           binary_operator != SXML_IR_BINARY_OPERATOR_RANGE;
}

/* True when the native row's result type is xs:boolean or xs:string (with
   any occurrence): its value has no number. */
static bool native_result_is_no_number(const sxml_native_function *row) {
    const sxml_byte_view result = row->result_type;
    const char *prefixes[] = {"xs:boolean", "xs:string"};
    for (size_t prefix_index = 0; prefix_index < 2; prefix_index += 1) {
        const size_t length = strlen(prefixes[prefix_index]);
        if (result.byte_count >= length &&
            memcmp(result.bytes, prefixes[prefix_index], length) == 0 &&
            (result.byte_count == length || result.bytes[length] == '?' ||
             result.bytes[length] == '*' || result.bytes[length] == '+')) {
            return true;
        }
    }
    return false;
}

/* True for a constant whose first item is not a number. */
static bool constant_is_no_number(const loop_pass *pass, sxml_ir_constant_index constant) {
    const sxml_sequence value = sxml_ir_unit_get_constant(pass->unit, constant);
    sxml_item first = {.representation = 0};
    if (sxml_sequence_get_representation(value) == SXML_SEQUENCE_REPRESENTATION_EMPTY) {
        return true;
    }
    if (sxml_sequence_get_item(value, (sxml_item_request){.item_index = 0, .error_record = NULL},
                               &first) != SXML_STATUS_OK) {
        return false;
    }
    return !sxml_atomic_item_is_numeric(first);
}

/* What value_has_property finds out about a value. */
typedef enum value_property {
    /* It never starts with a number, so that as a predicate it is taken
       by its effective boolean value and never compared with the
       position (XPath 4.0, 4.4.1). */
    SXML_VALUE_PROPERTY_NO_NUMBER,
    /* It has only nodes. */
    SXML_VALUE_PROPERTY_NODES,
} value_property;

/* A value of a function and the property asked about it. */
typedef struct value_question {
    const slot_census *census;
    sxml_ir_expression_index root;
    value_property property;
} value_question;

/* The answer to a question a pass asks about the IR. */
typedef struct pass_answer {
    bool is_true;
} pass_answer;

/*
 * Writes to answer whether the value of the expression at question.root
 * has question.property, known from what makes the value:
 * for NO_NUMBER, comparisons, quantified expressions, instance of and
 * castable, native functions of xs:boolean or xs:string results, axis
 * steps and paths, constants without a leading number; for NODES, axis
 * steps, paths and union, intersect and except of them, and the empty
 * constant; for both, LETs, conditionals, filters and slots LETs bind of
 * such values. Uses pass->necessary as its stack.
 */
static sxml_status value_has_property(loop_pass *pass, value_question question,
                                      pass_answer *answer) {
    const slot_census *census = question.census;
    bool *has_property = &answer->is_true;
    const bool wants_nodes = question.property == SXML_VALUE_PROPERTY_NODES;
    pass->necessary.element_count = 0;
    sxml_status status = pass_array_append(pass->allocator, &pass->necessary, &question.root);
    *has_property = true;
    size_t visited = 0;
    while (status == SXML_STATUS_OK && *has_property && pass->necessary.element_count > 0) {
        pass->necessary.element_count -= 1;
        const sxml_ir_expression_index index = ((const sxml_ir_expression_index *)
                                                    pass->necessary.elements)[pass->necessary
                                                                                     .element_count];
        visited += 1;
        if (index.value == 0 || index.value >= pass->unit->expression_count ||
            visited > pass->unit->expression_count) {
            *has_property = false;
            break;
        }
        const sxml_ir_expression expression = expression_at(pass, index);
        sxml_ir_expression_index next[2] = {{.value = 0}, {.value = 0}};
        sxml_ir_atom followed = {.atom_kind = SXML_IR_ATOM_KIND_NONE};
        const sxml_ir_expression_kind kind = expression.expression_kind;
        if (kind == SXML_IR_EXPRESSION_KIND_LET) {
            next[0] = expression.let.body;
        } else if (kind == SXML_IR_EXPRESSION_KIND_CONDITIONAL) {
            next[0] = expression.conditional.then_branch;
            next[1] = expression.conditional.else_branch;
        } else if (kind == SXML_IR_EXPRESSION_KIND_BINARY) {
            const sxml_ir_binary_operator binary_operator = expression.binary.operator;
            *has_property = wants_nodes ? binary_operator == SXML_IR_BINARY_OPERATOR_UNION ||
                                              binary_operator == SXML_IR_BINARY_OPERATOR_INTERSECT ||
                                              binary_operator == SXML_IR_BINARY_OPERATOR_EXCEPT
                                        : binary_gives_no_number(binary_operator);
        } else if (kind == SXML_IR_EXPRESSION_KIND_STEP) {
            /* Nodes. */
        } else if (kind == SXML_IR_EXPRESSION_KIND_QUANTIFIED ||
                   kind == SXML_IR_EXPRESSION_KIND_INSTANCE_OF ||
                   kind == SXML_IR_EXPRESSION_KIND_CASTABLE) {
            /* A boolean. */
            *has_property = !wants_nodes;
        } else if (kind == SXML_IR_EXPRESSION_KIND_STATIC_CALL) {
            *has_property =
                !wants_nodes &&
                expression.static_call.target.target_kind == SXML_IR_FUNCTION_TARGET_KIND_NATIVE &&
                expression.static_call.target.native_function != NULL &&
                native_result_is_no_number(expression.static_call.target.native_function);
        } else if (kind == SXML_IR_EXPRESSION_KIND_ATOM) {
            if (expression.atom.atom_kind == SXML_IR_ATOM_KIND_CONSTANT) {
                *has_property =
                    wants_nodes ? sxml_sequence_get_representation(sxml_ir_unit_get_constant(
                                      pass->unit, expression.atom.constant)) ==
                                      SXML_SEQUENCE_REPRESENTATION_EMPTY
                                : constant_is_no_number(pass, expression.atom.constant);
            } else {
                followed = expression.atom;
            }
        } else if (kind == SXML_IR_EXPRESSION_KIND_DOCUMENT_ORDER) {
            followed = expression.document_order.input;
        } else if (kind == SXML_IR_EXPRESSION_KIND_FILTER) {
            followed = expression.filter.input;
        } else if (kind == SXML_IR_EXPRESSION_KIND_MAP) {
            /* A path's steps select nodes; its last step may give other
               items, which its body shows. */
            *has_property = wants_nodes || (expression.map.flags & SXML_IR_MAP_REQUIRES_NODES) != 0;
            next[0] = expression.map.body;
        } else {
            *has_property = false;
        }
        if (followed.atom_kind != SXML_IR_ATOM_KIND_NONE) {
            const sxml_ir_expression_index let = single_let_of(census, followed);
            if (let.value == 0) {
                *has_property = false;
            } else {
                next[0] = expression_at(pass, let).let.bound_expression;
            }
        }
        for (size_t next_index = 0; status == SXML_STATUS_OK && *has_property && next_index < 2;
             next_index += 1) {
            if (next[next_index].value != 0) {
                status = pass_array_append(pass->allocator, &pass->necessary, &next[next_index]);
            }
        }
    }
    return status;
}

/* A FILTER of a function. */
typedef struct filter_question {
    const slot_census *census;
    const sxml_ir_filter *filter;
} filter_question;

/* Answers whether a FILTER selects each item by its predicate alone: the
   predicate reads neither the position nor the size and its value is no
   number, so the item's place in the input does not matter. */
static sxml_status filter_is_by_item(loop_pass *pass, filter_question question,
                                     pass_answer *answer) {
    const sxml_ir_filter *filter = question.filter;
    answer->is_true = false;
    if (filter->focus.context_position.value != 0 || filter->focus.context_size.value != 0 ||
        filter->index.key.value != 0) {
        return SXML_STATUS_OK;
    }
    return value_has_property(pass,
                              (value_question){.census = question.census,
                                               .root = filter->predicate,
                                               .property = SXML_VALUE_PROPERTY_NO_NUMBER},
                              answer);
}

/* ------------------------------------------------- Descendant paths */

/* One link of a path after a step: the expression and the slot of the
   value before it, in a function. */
typedef struct path_link {
    const slot_census *census;
    sxml_ir_expression_index index;
    sxml_ir_slot previous;
} path_link;

/*
 * Answers whether the link reads the value before it once
 * and goes on from each of its nodes alone, so that it gives the same
 * nodes (in document order after the path) for the nodes of several
 * origins together as for each origin apart: a FILTER that selects by the
 * item (filter_is_by_item), a path's MAP whose body gives nodes and reads
 * neither the position nor the size, a DOCUMENT_ORDER, or the value.
 */
static sxml_status link_is_per_node(loop_pass *pass, path_link link, pass_answer *answer) {
    const slot_census *census = link.census;
    answer->is_true = false;
    const sxml_ir_expression expression = expression_at(pass, link.index);
    sxml_ir_atom input = {.atom_kind = SXML_IR_ATOM_KIND_NONE};
    if (expression.expression_kind == SXML_IR_EXPRESSION_KIND_FILTER) {
        input = expression.filter.input;
    } else if (expression.expression_kind == SXML_IR_EXPRESSION_KIND_MAP) {
        input = expression.map.input;
    } else if (expression.expression_kind == SXML_IR_EXPRESSION_KIND_DOCUMENT_ORDER) {
        input = expression.document_order.input;
    } else if (expression.expression_kind == SXML_IR_EXPRESSION_KIND_ATOM) {
        input = expression.atom;
    }
    if (input.atom_kind != SXML_IR_ATOM_KIND_SLOT || link.previous.value == 0 ||
        input.slot.value != link.previous.value || read_count_of(census, link.previous) != 1) {
        return SXML_STATUS_OK;
    }
    if (expression.expression_kind == SXML_IR_EXPRESSION_KIND_FILTER) {
        return filter_is_by_item(
            pass, (filter_question){.census = census, .filter = &expression.filter}, answer);
    }
    if (expression.expression_kind == SXML_IR_EXPRESSION_KIND_MAP) {
        if (expression.map.flags != (uint32_t)SXML_IR_MAP_REQUIRES_NODES ||
            expression.map.focus.context_position.value != 0 ||
            expression.map.focus.context_size.value != 0) {
            return SXML_STATUS_OK;
        }
        return value_has_property(pass,
                                  (value_question){.census = census,
                                                   .root = expression.map.body,
                                                   .property = SXML_VALUE_PROPERTY_NODES},
                                  answer);
    }
    answer->is_true = true;
    return SXML_STATUS_OK;
}


/*
 * The descendant pass (glossary `descendant_path`). "E//T[P]" is
 * E/descendant-or-self::node()/child::T[P] (XPath 4.0, 4.6.3, 4.6.7):
 * a MAP over every node below E that steps to its T children and filters
 * each node's children apart. When the step is to the children and T
 * selects XNodes only, every predicate selects by the item alone
 * (filter_is_by_item), and the intermediate values are read once, it is
 * descendant::T[P] from E: the nodes are the same (a child of a node
 * below E is a node below E), and so are the ones P selects. The lowered
 * form
 *
 *     LET(m, MAP(E, f, STEP(f, descendant-or-self, node())),
 *       LET(d, DOCUMENT_ORDER(m), ...
 *         MAP(d, g, LET(c, STEP(g, child, T), FILTER(c, h, P)...))
 *
 * becomes
 *
 *     LET(m, MAP(E, f, STEP(f, descendant, T)),
 *       LET(d, DOCUMENT_ORDER(m), ...
 *         LET(c, d, FILTER(c, h, P)...))
 *
 * one FILTER over every T below E instead of one per node, whose input
 * no longer depends on an enclosing loop's variables (so the hoist pass
 * can compute it once, and a filter index find it the same each time).
 * Errors: E's nodes are required as before; P runs for the same nodes.
 */
static sxml_status shorten_descendant_path(loop_pass *pass, const slot_census *census,
                                           sxml_ir_expression_index index) {
    const sxml_ir_expression outer = expression_at(pass, index);
    const sxml_ir_map *map = &outer.map;
    if (map->flags != (uint32_t)SXML_IR_MAP_REQUIRES_NODES ||
        map->focus.context_position.value != 0 || map->focus.context_size.value != 0 ||
        map->focus.context_value.value == 0 ||
        read_count_of(census, map->focus.context_value) != 1) {
        return SXML_STATUS_OK;
    }
    /* d: the descendants in document order, read here only. */
    const sxml_ir_expression_index ordered_let = single_let_of(census, map->input);
    if (ordered_let.value == 0 || read_count_of(census, map->input.slot) != 1) {
        return SXML_STATUS_OK;
    }
    const sxml_ir_expression ordered =
        expression_at(pass, expression_at(pass, ordered_let).let.bound_expression);
    if (ordered.expression_kind != SXML_IR_EXPRESSION_KIND_DOCUMENT_ORDER) {
        return SXML_STATUS_OK;
    }
    const sxml_ir_expression_index descendants_let =
        single_let_of(census, ordered.document_order.input);
    if (descendants_let.value == 0 ||
        read_count_of(census, ordered.document_order.input.slot) != 1) {
        return SXML_STATUS_OK;
    }
    const sxml_ir_expression_index descendants_index =
        expression_at(pass, descendants_let).let.bound_expression;
    const sxml_ir_expression descendants = expression_at(pass, descendants_index);
    if (descendants.expression_kind != SXML_IR_EXPRESSION_KIND_MAP ||
        descendants.map.flags != (uint32_t)SXML_IR_MAP_REQUIRES_NODES ||
        descendants.map.focus.context_position.value != 0 ||
        descendants.map.focus.context_size.value != 0 || is_left_alone(pass, descendants_index)) {
        return SXML_STATUS_OK;
    }
    const sxml_ir_expression_index self_step_index = descendants.map.body;
    const sxml_ir_expression self_step = expression_at(pass, self_step_index);
    if (self_step.expression_kind != SXML_IR_EXPRESSION_KIND_STEP ||
        self_step.step.axis != SXML_AXIS_DESCENDANT_OR_SELF ||
        self_step.step.node_test.index != (uint32_t)SXML_TYPE_GENERIC_NODE ||
        self_step.step.origin.atom_kind != SXML_IR_ATOM_KIND_SLOT ||
        self_step.step.origin.slot.value != descendants.map.focus.context_value.value) {
        return SXML_STATUS_OK;
    }
    /* The body: the child step, alone or bound and filtered. */
    sxml_ir_expression_index child_step_index = map->body;
    const sxml_ir_expression body = expression_at(pass, map->body);
    sxml_ir_expression_index rest = {.value = 0};
    if (body.expression_kind == SXML_IR_EXPRESSION_KIND_LET && !body.let.is_content) {
        child_step_index = body.let.bound_expression;
        rest = body.let.body;
    }
    const sxml_ir_expression child_step = expression_at(pass, child_step_index);
    if (child_step.expression_kind != SXML_IR_EXPRESSION_KIND_STEP ||
        child_step.step.axis != SXML_AXIS_CHILD ||
        child_step.step.origin.atom_kind != SXML_IR_ATOM_KIND_SLOT ||
        child_step.step.origin.slot.value != map->focus.context_value.value ||
        !sxml_type_registry_is_subtype(
            pass->request->type_registry,
            (sxml_type_pair){.derived_type = child_step.step.node_test,
                             .base_type = sxml_builtin_type_identifier(SXML_TYPE_NODE)})) {
        return SXML_STATUS_OK;
    }
    /* The links after it: each reads the one before once, and goes on
       from each node alone. */
    sxml_ir_slot previous = body.expression_kind == SXML_IR_EXPRESSION_KIND_LET
                                ? body.let.variable
                                : (sxml_ir_slot){.value = 0};
    sxml_status status = SXML_STATUS_OK;
    while (rest.value != 0) {
        const sxml_ir_expression link = expression_at(pass, rest);
        sxml_ir_expression_index link_index = rest;
        rest = (sxml_ir_expression_index){.value = 0};
        sxml_ir_slot next_previous = {.value = 0};
        if (link.expression_kind == SXML_IR_EXPRESSION_KIND_LET && !link.let.is_content) {
            link_index = link.let.bound_expression;
            next_previous = link.let.variable;
            rest = link.let.body;
        }
        pass_answer is_per_node = {.is_true = false};
        status = link_is_per_node(
            pass, (path_link){.census = census, .index = link_index, .previous = previous},
            &is_per_node);
        if (status != SXML_STATUS_OK || !is_per_node.is_true) {
            return status;
        }
        previous = next_previous;
    }
    /* The descendant step, with the child step's test. */
    sxml_ir_expression descendant_step = self_step;
    descendant_step.step.axis = SXML_AXIS_DESCENDANT;
    descendant_step.step.node_test = child_step.step.node_test;
    descendant_step.step.has_qualified_name_test = child_step.step.has_qualified_name_test;
    descendant_step.step.is_xnode_type_test = child_step.step.is_xnode_type_test;
    status = sxml_ir_unit_replace_expression(
        pass->unit,
        &(sxml_ir_expression_replacement){.index = self_step_index, .expression = descendant_step});
    /* The child step becomes d, and the MAP its body. */
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_replace_expression(
            pass->unit, &(sxml_ir_expression_replacement){
                            .index = child_step_index,
                            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM,
                                           .source_span = child_step.source_span,
                                           .atom = map->input}});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_replace_expression(
            pass->unit,
            &(sxml_ir_expression_replacement){.index = index,
                                              .expression = expression_at(pass, map->body)});
    }
    return status;
}

/*
 * Child paths from many origins (glossary `descendant_path`, D302).
 * "E/T[P]..." is a MAP over E's nodes that steps to each node's T
 * children and filters them apart:
 *
 *     LET(m, MAP(E, g, LET(c, STEP(g, child, T), FILTER(c, h, P)...)),
 *       DOCUMENT_ORDER(m))
 *
 * When T selects XNodes only, the links after the step go on from each
 * node alone (link_is_per_node: by-item filters, path steps, document
 * order), and the intermediate values are read once, the links run once
 * over the children of all of E's nodes together:
 *
 *     LET(m, LET(c, MAP(E, g, STEP(g, child, T)), FILTER(c, h, P)...),
 *       DOCUMENT_ORDER(m))
 *
 * The nodes are the same, and the DOCUMENT_ORDER after them puts them in
 * the same order (XPath 4.0, 4.6.2: a path's result is in document order
 * whatever the order of its steps' values). The FILTER's input then no
 * longer depends on an enclosing loop's variables when E does not, as for
 * the descendant pass: the hoist pass computes it once and a filter index
 * finds the same input each time ("/spans/span[@parent = $span/@id]").
 * Errors: E's nodes are required as before; P runs for the same nodes.
 */
/* A LET whose body is still to walk: its variable is the value before. */
typedef struct pending_link {
    sxml_ir_slot variable;
    sxml_ir_expression_index body;
} pending_link;

static sxml_status pull_child_path_links(loop_pass *pass, const slot_census *census,
                                         sxml_ir_expression_index ordering_index) {
    const sxml_ir_expression ordering = expression_at(pass, ordering_index);
    const sxml_ir_expression_index map_let = single_let_of(census, ordering.document_order.input);
    if (map_let.value == 0 || read_count_of(census, ordering.document_order.input.slot) != 1) {
        return SXML_STATUS_OK;
    }
    const sxml_ir_expression_index index = expression_at(pass, map_let).let.bound_expression;
    const sxml_ir_expression outer = expression_at(pass, index);
    if (outer.expression_kind != SXML_IR_EXPRESSION_KIND_MAP || is_left_alone(pass, index)) {
        return SXML_STATUS_OK;
    }
    const sxml_ir_map *map = &outer.map;
    if (map->flags != (uint32_t)SXML_IR_MAP_REQUIRES_NODES ||
        map->focus.context_position.value != 0 || map->focus.context_size.value != 0 ||
        map->focus.context_value.value == 0 ||
        read_count_of(census, map->focus.context_value) != 1) {
        return SXML_STATUS_OK;
    }
    const sxml_ir_expression body = expression_at(pass, map->body);
    if (body.expression_kind != SXML_IR_EXPRESSION_KIND_LET || body.let.is_content) {
        return SXML_STATUS_OK;
    }
    const sxml_ir_expression child_step = expression_at(pass, body.let.bound_expression);
    if (child_step.expression_kind != SXML_IR_EXPRESSION_KIND_STEP ||
        child_step.step.axis != SXML_AXIS_CHILD ||
        child_step.step.origin.atom_kind != SXML_IR_ATOM_KIND_SLOT ||
        child_step.step.origin.slot.value != map->focus.context_value.value ||
        !sxml_type_registry_is_subtype(
            pass->request->type_registry,
            (sxml_type_pair){.derived_type = child_step.step.node_test,
                             .base_type = sxml_builtin_type_identifier(SXML_TYPE_NODE)})) {
        return SXML_STATUS_OK;
    }
    /* The links after the step, LETs of LETs flattened: each reads the
       value before it once and goes on from each node alone. Only a path
       with a FILTER gains (its input is then one sequence). */
    sxml_ir_expression_index cursor = body.let.body;
    sxml_ir_slot previous = body.let.variable;
    pending_link pending[16];
    size_t pending_count = 0;
    bool has_filter = false;
    sxml_status status = SXML_STATUS_OK;
    for (;;) {
        const sxml_ir_expression link = expression_at(pass, cursor);
        if (link.expression_kind == SXML_IR_EXPRESSION_KIND_LET) {
            if (link.let.is_content || pending_count == sizeof(pending) / sizeof(pending[0])) {
                return SXML_STATUS_OK;
            }
            pending[pending_count] =
                (pending_link){.variable = link.let.variable, .body = link.let.body};
            pending_count += 1;
            cursor = link.let.bound_expression;
            continue;
        }
        has_filter = has_filter || link.expression_kind == SXML_IR_EXPRESSION_KIND_FILTER;
        pass_answer is_per_node = {.is_true = false};
        status = link_is_per_node(
            pass, (path_link){.census = census, .index = cursor, .previous = previous},
            &is_per_node);
        if (status != SXML_STATUS_OK || !is_per_node.is_true) {
            return status;
        }
        if (pending_count == 0) {
            break;
        }
        pending_count -= 1;
        previous = pending[pending_count].variable;
        cursor = pending[pending_count].body;
    }
    if (!has_filter) {
        return SXML_STATUS_OK;
    }
    /* The three places are reused: the MAP's becomes the LET, the step's
       the MAP, the LET's the step. */
    const sxml_ir_expression_index step_index = body.let.bound_expression;
    sxml_ir_expression pulled_let = body;
    pulled_let.let.bound_expression = step_index;
    sxml_ir_expression children_map = outer;
    children_map.map.body = map->body;
    status = sxml_ir_unit_replace_expression(
        pass->unit, &(sxml_ir_expression_replacement){.index = map->body, .expression = child_step});
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_replace_expression(
            pass->unit,
            &(sxml_ir_expression_replacement){.index = step_index, .expression = children_map});
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_replace_expression(
            pass->unit, &(sxml_ir_expression_replacement){.index = index, .expression = pulled_let});
    }
    return status;
}

/* ---------------------------------------------------- Item demands */

/* Where an expression sits in its parent, for the demand pass. */
typedef enum demand_role {
    /* The function's body, or a place whose value is read whole. */
    SXML_DEMAND_ROLE_OTHER = 0,
    SXML_DEMAND_ROLE_LET_BOUND,
    SXML_DEMAND_ROLE_LET_BODY,
    /* A branch of a CONDITIONAL. */
    SXML_DEMAND_ROLE_BRANCH,
    SXML_DEMAND_ROLE_MAP_BODY,
    /* A FILTER's predicate whose value is no number: its effective
       boolean value is read. */
    SXML_DEMAND_ROLE_BOOLEAN_PREDICATE,
    /* A QUANTIFIED's condition: its effective boolean value is read. */
    SXML_DEMAND_ROLE_CONDITION,
    /* A FILTER's predicate not yet looked at (it needs the whole census):
       the demand pass makes it SXML_DEMAND_ROLE_BOOLEAN_PREDICATE or
       SXML_DEMAND_ROLE_OTHER; read whole until then. */
    SXML_DEMAND_ROLE_PREDICATE,
} demand_role;

/* An expression of the function being looked at, its parent and its
   item demand so far. */
typedef struct demand_member {
    sxml_ir_expression_index index;
    demand_role role;
    /* The member number of the parent; the parent comes first. */
    size_t parent;
    uint32_t demand;
    /* The kind of the expression at index. */
    sxml_ir_expression_kind kind;
    /* Inside a STREAM's or MERGE's children, which the passes leave
       alone (as make_owners marks them). */
    bool is_in_stream;
} demand_member;

/* What the demand pass keeps for one function. */
typedef struct demand_state {
    /* Filled as the members are collected. */
    slot_census *census;
    /* demand_member, in an order that puts every parent before its
       children. */
    pass_array members;
    /* Per slot of the function: the reads that read a known number of
       leading items, the largest such number, and the slot's demand. */
    uint32_t *known_read_counts;
    uint32_t *read_demands;
    uint32_t *slot_demands;
    /* The same counts and largest numbers of the reads that do not depend
       on a member's demand (all but a DOCUMENT_ORDER's), noted once. */
    uint32_t *fixed_read_counts;
    uint32_t *fixed_read_demands;
} demand_state;

/* The effective boolean value reads two items at most: the first, and
   whether there is a second when the first is no node (XPath 4.0,
   2.5.4.1, FORG0006). */
#define DEMAND_OF_BOOLEAN 2U

static demand_member *member_at(const demand_state *state, size_t member_number) {
    return &((demand_member *)state->members.elements)[member_number];
}

/* The item demand a native function's argument has: fn:exists, fn:empty
   and fn:head read one item, fn:boolean, fn:not, fn:zero-or-one and
   fn:exactly-one two (F&O 4.0, 14.1.3 to 14.1.6, 7.3.1, 7.3.2, 14.2.1,
   14.2.2); 0 for every other. */
static uint32_t native_argument_demand(const sxml_native_function *row) {
    if (row == NULL || row->function_namespace != SXML_NAMESPACE_FUNCTIONS) {
        return 0;
    }
    static const struct {
        sxml_byte_view local_name;
        uint32_t demand;
    } demands[] = {
        {SXML_BYTE_VIEW_INITIALIZER("exists"), 1},
        {SXML_BYTE_VIEW_INITIALIZER("empty"), 1},
        {SXML_BYTE_VIEW_INITIALIZER("head"), 1},
        {SXML_BYTE_VIEW_INITIALIZER("boolean"), DEMAND_OF_BOOLEAN},
        {SXML_BYTE_VIEW_INITIALIZER("not"), DEMAND_OF_BOOLEAN},
        {SXML_BYTE_VIEW_INITIALIZER("zero-or-one"), DEMAND_OF_BOOLEAN},
        {SXML_BYTE_VIEW_INITIALIZER("exactly-one"), DEMAND_OF_BOOLEAN},
    };
    for (size_t name_index = 0; name_index < sizeof(demands) / sizeof(demands[0]);
         name_index += 1) {
        if (sxml_byte_view_equals(row->local_name, demands[name_index].local_name)) {
            return demands[name_index].demand;
        }
    }
    return 0;
}

/* True for an axis that gives one origin's nodes in document order. */
static bool axis_is_forward(sxml_axis axis) {
    return axis == SXML_AXIS_SELF || axis == SXML_AXIS_CHILD || axis == SXML_AXIS_DESCENDANT ||
           axis == SXML_AXIS_DESCENDANT_OR_SELF || axis == SXML_AXIS_ATTRIBUTE ||
           axis == SXML_AXIS_FOLLOWING_SIBLING || axis == SXML_AXIS_FOLLOWING_SIBLING_OR_SELF ||
           axis == SXML_AXIS_FOLLOWING || axis == SXML_AXIS_FOLLOWING_OR_SELF;
}

/* A path's MAP of a function. */
typedef struct path_map_question {
    const slot_census *census;
    const sxml_ir_map *map;
} path_map_question;

/*
 * True for a path's MAP (no other flag) whose body gives the nodes of one
 * origin in document order: a forward axis step from the focus, the
 * document order of a path, or filters of such a value, through LETs.
 * Then the first n nodes of several origins in document order are among
 * the first n of each origin, and the first n of one origin are the
 * MAP's first n.
 */
static bool path_map_is_ordered(const loop_pass *pass, path_map_question question) {
    const sxml_ir_map *map = question.map;
    if (map->flags != (uint32_t)SXML_IR_MAP_REQUIRES_NODES || map->focus.context_value.value == 0) {
        return false;
    }
    sxml_ir_expression_index index = map->body;
    for (size_t visited = 0; visited < pass->unit->expression_count; visited += 1) {
        const sxml_ir_expression expression = expression_at(pass, index);
        sxml_ir_atom followed = {.atom_kind = SXML_IR_ATOM_KIND_NONE};
        if (expression.expression_kind == SXML_IR_EXPRESSION_KIND_LET) {
            index = expression.let.body;
            continue;
        }
        if (expression.expression_kind == SXML_IR_EXPRESSION_KIND_STEP) {
            return axis_is_forward(expression.step.axis) &&
                   expression.step.origin.atom_kind == SXML_IR_ATOM_KIND_SLOT &&
                   expression.step.origin.slot.value == map->focus.context_value.value;
        }
        if (expression.expression_kind == SXML_IR_EXPRESSION_KIND_DOCUMENT_ORDER) {
            return true;
        }
        if (expression.expression_kind == SXML_IR_EXPRESSION_KIND_FILTER) {
            followed = expression.filter.input;
        } else if (expression.expression_kind == SXML_IR_EXPRESSION_KIND_ATOM) {
            followed = expression.atom;
        }
        const sxml_ir_expression_index let = single_let_of(question.census, followed);
        if (let.value == 0) {
            return false;
        }
        index = expression_at(pass, let).let.bound_expression;
    }
    return false;
}

/* A constant positive integer k that a FILTER's predicate is, reading no
   focus: the FILTER reads k items of its input; 0 otherwise. */
static uint32_t constant_position_of(const loop_pass *pass, const sxml_ir_filter *filter) {
    if (filter->focus.context_value.value != 0 || filter->focus.context_position.value != 0 ||
        filter->focus.context_size.value != 0) {
        return 0;
    }
    const sxml_ir_expression predicate = expression_at(pass, filter->predicate);
    if (predicate.expression_kind != SXML_IR_EXPRESSION_KIND_ATOM ||
        predicate.atom.atom_kind != SXML_IR_ATOM_KIND_CONSTANT) {
        return 0;
    }
    const sxml_sequence value = sxml_ir_unit_get_constant(pass->unit, predicate.atom.constant);
    if (sxml_sequence_get_representation(value) != SXML_SEQUENCE_REPRESENTATION_SINGLE_ITEM ||
        sxml_item_get_representation(value.content) != SXML_ITEM_REPRESENTATION_INTEGER ||
        value.content.payload.integer < 1 || value.content.payload.integer > INT32_MAX) {
        return 0;
    }
    return (uint32_t)value.content.payload.integer;
}

/* Notes a read of atom that reads its first demand items (0: every
   item, which is not noted: the slot's reads are then not all known). */
static void note_known_read(demand_state *state, sxml_ir_atom atom, uint32_t demand) {
    if (demand == 0 || atom.atom_kind != SXML_IR_ATOM_KIND_SLOT || atom.slot.value == 0 ||
        atom.slot.value >= state->census->use_count) {
        return;
    }
    state->known_read_counts[atom.slot.value] += 1;
    if (demand > state->read_demands[atom.slot.value]) {
        state->read_demands[atom.slot.value] = demand;
    }
}

/* Notes the reads of a member that read a known number of items, but
   not a DOCUMENT_ORDER's (note_ordered_reads). */
static void note_fixed_reads(const loop_pass *pass, demand_state *state,
                             const demand_member *member) {
    const sxml_ir_expression *expression = sxml_ir_unit_get_expression(pass->unit, member->index);
    const sxml_ir_expression_kind kind = expression->expression_kind;
    if (kind == SXML_IR_EXPRESSION_KIND_STATIC_CALL &&
        expression->static_call.target.target_kind == SXML_IR_FUNCTION_TARGET_KIND_NATIVE &&
        expression->static_call.arguments.atom_count == 1) {
        const sxml_ir_atom_view arguments =
            sxml_ir_unit_get_atoms(pass->unit, expression->static_call.arguments);
        if (arguments.atom_count != 1 || arguments.atoms == NULL) {
            return;
        }
        note_known_read(state, arguments.atoms[0],
                        native_argument_demand(expression->static_call.target.native_function));
    } else if (kind == SXML_IR_EXPRESSION_KIND_CONDITIONAL) {
        note_known_read(state, expression->conditional.condition, DEMAND_OF_BOOLEAN);
    } else if (kind == SXML_IR_EXPRESSION_KIND_FILTER) {
        note_known_read(state, expression->filter.input,
                        constant_position_of(pass, &expression->filter));
    }
}

/* Notes the read of a DOCUMENT_ORDER member that reads a known number
   of items: it depends on the member's demand. */
static void note_ordered_reads(const loop_pass *pass, demand_state *state,
                               const demand_member *member) {
    const sxml_ir_expression *expression = sxml_ir_unit_get_expression(pass->unit, member->index);
    if (member->demand != 0) {
        /* The first n nodes of a path in document order are the first n
           the path's MAP makes, when its body is ordered. */
        const sxml_ir_expression_index let =
            single_let_of(state->census, expression->document_order.input);
        if (let.value != 0) {
            const sxml_ir_expression bound =
                expression_at(pass, expression_at(pass, let).let.bound_expression);
            if (bound.expression_kind == SXML_IR_EXPRESSION_KIND_MAP &&
                path_map_is_ordered(pass,
                                    (path_map_question){.census = state->census, .map = &bound.map})) {
                note_known_read(state, expression->document_order.input, member->demand);
            }
        }
    }
}

/* The demand of a member from its parent's and the slots' demands. */
static uint32_t member_demand(const loop_pass *pass, const demand_state *state,
                              const demand_member *member) {
    if (member->role == SXML_DEMAND_ROLE_OTHER || member->role == SXML_DEMAND_ROLE_PREDICATE) {
        return 0;
    }
    if (member->role == SXML_DEMAND_ROLE_BOOLEAN_PREDICATE ||
        member->role == SXML_DEMAND_ROLE_CONDITION) {
        return DEMAND_OF_BOOLEAN;
    }
    const demand_member *parent = member_at(state, member->parent);
    const sxml_ir_expression *parent_expression =
        sxml_ir_unit_get_expression(pass->unit, parent->index);
    if (member->role == SXML_DEMAND_ROLE_LET_BOUND) {
        const sxml_ir_let *let = &parent_expression->let;
        if (let->is_content || let->variable.value >= state->census->use_count ||
            single_let_of(state->census, sxml_ir_slot_atom(let->variable)).value !=
                parent->index.value) {
            return 0;
        }
        return state->slot_demands[let->variable.value];
    }
    if (member->role == SXML_DEMAND_ROLE_MAP_BODY) {
        const sxml_ir_map *map = &parent_expression->map;
        const bool is_plain = map->flags == 0;
        if (!is_plain &&
            !path_map_is_ordered(pass, (path_map_question){.census = state->census, .map = map})) {
            return 0;
        }
    }
    return parent->demand;
}

/* An expression to add as a member, with its parent and role. */
typedef struct demand_child {
    sxml_ir_expression_index index;
    size_t parent;
    demand_role role;
} demand_child;

static sxml_status push_demand_child(loop_pass *pass, pass_array *stack, demand_child child) {
    if (child.index.value == 0) {
        return SXML_STATUS_OK;
    }
    return pass_array_append(pass->allocator, stack, &child);
}

/* A member whose children go on a stack of demand_child. */
typedef struct member_children {
    pass_array *stack;
    size_t member_number;
} member_children;

/* Pushes the children of a member: those whose demand follows from it
   with their roles, every other one read whole. Uses pass->walk. */
static sxml_status push_member_children(loop_pass *pass, const demand_state *state,
                                        member_children request) {
    pass_array *stack = request.stack;
    const size_t member_number = request.member_number;
    const sxml_ir_expression_index index = member_at(state, member_number)->index;
    const sxml_ir_expression *expression = sxml_ir_unit_get_expression(pass->unit, index);
    const sxml_ir_expression_kind kind = expression->expression_kind;
    const demand_child none = {
        .index = {.value = 0}, .parent = member_number, .role = SXML_DEMAND_ROLE_OTHER};
    demand_child children[2] = {none, none};
    sxml_status status = SXML_STATUS_OK;
    if (kind == SXML_IR_EXPRESSION_KIND_LET) {
        children[0].index = expression->let.bound_expression;
        children[0].role = SXML_DEMAND_ROLE_LET_BOUND;
        children[1].index = expression->let.body;
        children[1].role = SXML_DEMAND_ROLE_LET_BODY;
    } else if (kind == SXML_IR_EXPRESSION_KIND_CONDITIONAL) {
        children[0].index = expression->conditional.then_branch;
        children[0].role = SXML_DEMAND_ROLE_BRANCH;
        children[1].index = expression->conditional.else_branch;
        children[1].role = SXML_DEMAND_ROLE_BRANCH;
    } else if (kind == SXML_IR_EXPRESSION_KIND_MAP) {
        children[0].index = expression->map.body;
        children[0].role = SXML_DEMAND_ROLE_MAP_BODY;
    } else if (kind == SXML_IR_EXPRESSION_KIND_QUANTIFIED) {
        children[0].index = expression->quantified.condition;
        children[0].role = SXML_DEMAND_ROLE_CONDITION;
    } else if (kind == SXML_IR_EXPRESSION_KIND_FILTER) {
        children[0].index = expression->filter.predicate;
        children[0].role = SXML_DEMAND_ROLE_PREDICATE;
        /* The index's key and probe are read whole. */
        children[1].index = expression->filter.index.key;
        status = push_demand_child(pass, stack,
                                   (demand_child){.index = expression->filter.index.probe,
                                                  .parent = member_number,
                                                  .role = SXML_DEMAND_ROLE_OTHER});
    } else {
        pass->walk.element_count = 0;
        status = push_every_child(pass, index);
        for (size_t other = 0; status == SXML_STATUS_OK && other < pass->walk.element_count;
             other += 1) {
            demand_child child = none;
            child.index = ((const sxml_ir_expression_index *)pass->walk.elements)[other];
            status = push_demand_child(pass, stack, child);
        }
    }
    for (size_t child_index = 0; status == SXML_STATUS_OK && child_index < 2; child_index += 1) {
        status = push_demand_child(pass, stack, children[child_index]);
    }
    return status;
}

/* Collects the members of the function's body, parents first, and
   counts each in state->census, started empty: one walk of the body. */
static sxml_status collect_demand_members(loop_pass *pass, demand_state *state,
                                          sxml_ir_expression_index body) {
    pass_array stack = pass->member_stack;
    stack.element_count = 0;
    sxml_status status = push_demand_child(
        pass, &stack, (demand_child){.index = body, .parent = 0, .role = SXML_DEMAND_ROLE_OTHER});
    while (status == SXML_STATUS_OK && stack.element_count > 0) {
        stack.element_count -= 1;
        const demand_child child = ((const demand_child *)stack.elements)[stack.element_count];
        if (child.index.value >= pass->unit->expression_count) {
            continue;
        }
        const size_t member_number = state->members.element_count;
        bool is_in_stream = false;
        if (member_number > 0) {
            const demand_member *parent = member_at(state, child.parent);
            const sxml_ir_expression_kind parent_kind =
                sxml_ir_unit_get_expression(pass->unit, parent->index)->expression_kind;
            is_in_stream = parent->is_in_stream || parent_kind == SXML_IR_EXPRESSION_KIND_STREAM ||
                           parent_kind == SXML_IR_EXPRESSION_KIND_MERGE;
        }
        const demand_member member = {
            .index = child.index,
            .role = child.role,
            .parent = child.parent,
            .demand = 0,
            .kind = sxml_ir_unit_get_expression(pass->unit, child.index)->expression_kind,
            .is_in_stream = is_in_stream};
        census_count_expression(pass, child.index, state->census);
        status = pass_array_append(pass->allocator, &state->members, &member);
        if (status == SXML_STATUS_OK) {
            status = push_member_children(
                pass, state,
                (member_children){.stack = &stack, .member_number = member_number});
        }
    }
    pass->member_stack = stack;
    return status;
}

/* Computes the demands of the members until nothing changes: a slot's
   demand is the largest of its reads' when every read reads a known
   number of leading items. */
static void settle_demands(const loop_pass *pass, demand_state *state) {
    const size_t slot_count = state->census->use_count;
    for (size_t member_number = 0; member_number < state->members.element_count;
         member_number += 1) {
        note_fixed_reads(pass, state, member_at(state, member_number));
    }
    memcpy(state->fixed_read_counts, state->known_read_counts, slot_count * sizeof(uint32_t));
    memcpy(state->fixed_read_demands, state->read_demands, slot_count * sizeof(uint32_t));
    bool is_changed = true;
    for (size_t round = 0; is_changed && round <= state->members.element_count + 1; round += 1) {
        is_changed = false;
        for (size_t member_number = 0; member_number < state->members.element_count;
             member_number += 1) {
            demand_member *member = member_at(state, member_number);
            const uint32_t demand = member_demand(pass, state, member);
            if (demand != member->demand) {
                member->demand = demand;
                is_changed = true;
            }
        }
        memcpy(state->known_read_counts, state->fixed_read_counts, slot_count * sizeof(uint32_t));
        memcpy(state->read_demands, state->fixed_read_demands, slot_count * sizeof(uint32_t));
        for (size_t member_number = 0; member_number < state->members.element_count;
             member_number += 1) {
            const demand_member *member = member_at(state, member_number);
            if (member->kind == SXML_IR_EXPRESSION_KIND_DOCUMENT_ORDER) {
                note_ordered_reads(pass, state, member);
            }
        }
        for (size_t slot_number = 1; slot_number < slot_count; slot_number += 1) {
            const uint32_t read_count = state->census->uses[slot_number].read_count;
            const uint32_t demand =
                read_count > 0 && state->known_read_counts[slot_number] == read_count
                    ? state->read_demands[slot_number]
                    : 0;
            if (demand != state->slot_demands[slot_number]) {
                state->slot_demands[slot_number] = demand;
                is_changed = true;
            }
        }
    }
}

/* Sets the item demands of the body of the function at function_number. */
static sxml_status set_function_demands(loop_pass *pass, uint32_t function_number) {
    slot_census census = {.uses = NULL, .use_count = 0};
    sxml_status status = start_slot_census(pass, function_number, &census);
    demand_state state = {.census = &census,
                          .members = pass->members,
                          .known_read_counts = NULL,
                          .read_demands = NULL,
                          .slot_demands = NULL,
                          .fixed_read_counts = NULL,
                          .fixed_read_demands = NULL};
    state.members.element_count = 0;
    void *memory = NULL;
    if (status == SXML_STATUS_OK) {
        status = sxml_allocator_allocate_array(
            pass->allocator,
            (sxml_array_allocation_request){.element_count = 5 * census.use_count,
                                            .element_size = sizeof(uint32_t),
                                            .element_alignment = alignof(uint32_t)},
            &memory);
    }
    if (status == SXML_STATUS_OK) {
        state.known_read_counts = memory;
        state.read_demands = state.known_read_counts + census.use_count;
        state.slot_demands = state.read_demands + census.use_count;
        state.fixed_read_counts = state.slot_demands + census.use_count;
        state.fixed_read_demands = state.fixed_read_counts + census.use_count;
        const sxml_ir_function *function = sxml_ir_unit_get_function(
            pass->unit, (sxml_ir_function_index){.value = function_number});
        status = collect_demand_members(pass, &state, function->body);
    }
    /* A predicate's role needs the whole census. */
    for (size_t member_number = 0; status == SXML_STATUS_OK &&
                                   member_number < state.members.element_count;
         member_number += 1) {
        demand_member *member = member_at(&state, member_number);
        if (member->role != SXML_DEMAND_ROLE_PREDICATE) {
            continue;
        }
        const sxml_ir_filter filter =
            sxml_ir_unit_get_expression(pass->unit, member_at(&state, member->parent)->index)
                ->filter;
        pass_answer is_boolean = {.is_true = false};
        status = filter_is_by_item(pass, (filter_question){.census = &census, .filter = &filter},
                                   &is_boolean);
        member->role =
            is_boolean.is_true ? SXML_DEMAND_ROLE_BOOLEAN_PREDICATE : SXML_DEMAND_ROLE_OTHER;
    }
    if (status == SXML_STATUS_OK) {
        settle_demands(pass, &state);
    }
    for (size_t member_number = 0; status == SXML_STATUS_OK &&
                                   member_number < state.members.element_count;
         member_number += 1) {
        const demand_member *member = member_at(&state, member_number);
        const sxml_ir_expression *expression =
            sxml_ir_unit_get_expression(pass->unit, member->index);
        if (expression->item_demand == member->demand || member->is_in_stream) {
            continue;
        }
        sxml_ir_expression changed = *expression;
        changed.item_demand = member->demand;
        status = sxml_ir_unit_replace_expression(
            pass->unit,
            &(sxml_ir_expression_replacement){.index = member->index, .expression = changed});
    }
    if (memory != NULL) {
        sxml_allocator_release(pass->allocator, memory);
    }
    pass->members = state.members;
    slot_census_finalize(pass, &census);
    return status;
}

/* -------------------------------------------- Quantified producers */

/*
 * The fusion pass (glossary `quantified_fusion`). "some $x in E[P]
 * satisfies C" and "some $x in (for $y in E return B) satisfies C" first
 * made the whole input, then stopped at the first item that decides
 * (XPath 4.0, 4.15). When the input is a slot read once, by the
 * QUANTIFIED alone, bound in the same evaluation (the LET's body reaches
 * the QUANTIFIED through LETs and conditional branches only, so the
 * input is made once either way) to
 * - a FILTER that selects by the item alone and has no filter index: the
 *   QUANTIFIED runs over E with the FILTER's focus as its variable and
 *   the condition LET(p, P, CONDITIONAL(p, LET(x, focus, C), k)), k
 *   false() for some and true() for every: "some/every $y in E satisfies
 *   (if (P) then C else k)";
 * - a MAP without flags that reads neither the position nor the size:
 *   it runs over E with the MAP's focus as its variable and the condition
 *   LET(t, B, QUANTIFIED(t, x, C)), the same quantifier;
 * so items are made as the QUANTIFIED reads them, and none after the one
 * that decides. The LET of the input is replaced by its body. The
 * predicate and the body run for the same items up to the deciding one
 * (their errors after it are not raised, XPath 4.0, 2.6.4).
 */

/* The members of one function and, per expression of the unit, its
   member number + 1 (0 for none). */
typedef struct fusion_state {
    slot_census census;
    demand_state demand;
    uint32_t *member_numbers;
    size_t member_number_count;
} fusion_state;

/* True when the member at member_number is evaluated once each time the
   LET at let_index is, after its bound expression: reached from the
   LET's body through LETs and conditional branches only. */
typedef struct once_question {
    size_t member_number;
    sxml_ir_expression_index let_index;
} once_question;

static bool is_once_per_let(const loop_pass *pass, const fusion_state *state,
                            once_question question) {
    size_t member_number = question.member_number;
    bool came_from_body = false;
    for (size_t climbed = 0; climbed <= state->demand.members.element_count; climbed += 1) {
        const demand_member *member = member_at(&state->demand, member_number);
        if (member->index.value == question.let_index.value) {
            return came_from_body;
        }
        if (member_number == 0) {
            return false;
        }
        const demand_member *parent = member_at(&state->demand, member->parent);
        const sxml_ir_expression_kind parent_kind = expression_at(pass, parent->index).expression_kind;
        if (member->role == SXML_DEMAND_ROLE_LET_BODY) {
            came_from_body = parent->index.value == question.let_index.value;
        } else if (member->role != SXML_DEMAND_ROLE_LET_BOUND &&
                   !(member->role == SXML_DEMAND_ROLE_BRANCH &&
                     parent_kind == SXML_IR_EXPRESSION_KIND_CONDITIONAL)) {
            return false;
        }
        member_number = member->parent;
    }
    return false;
}

/* Adds an expression to the unit and writes its index. */
static sxml_status add_pass_expression(loop_pass *pass, const sxml_ir_expression *expression,
                                       sxml_ir_expression_index *index) {
    return sxml_ir_unit_add_expression(pass->unit, expression, index);
}

/* The quantified expression to fuse, its producer and a new slot. */
typedef struct fusion_parts {
    sxml_ir_expression quantified;
    sxml_ir_expression producer;
    sxml_ir_slot new_slot;
} fusion_parts;

/* The condition of the fused QUANTIFIED over a FILTER's input. */
static sxml_status fuse_filter_condition(loop_pass *pass, const fusion_parts *parts,
                                         sxml_ir_expression_index *condition) {
    const sxml_ir_quantified *quantified = &parts->quantified.quantified;
    const sxml_ir_filter *filter = &parts->producer.filter;
    const sxml_ir_source_span span = parts->quantified.source_span;
    sxml_ir_constant_index decided = {.value = 0};
    sxml_status status =
        boolean_constant(pass, quantified->quantifier == SXML_IR_QUANTIFIER_EVERY, &decided);
    sxml_ir_expression_index otherwise = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = add_constant_atom(pass, decided, &otherwise);
    }
    sxml_ir_expression_index focus = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = add_pass_expression(
            pass,
            &(sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM,
                                  .source_span = span,
                                  .atom = sxml_ir_slot_atom(filter->focus.context_value)},
            &focus);
    }
    sxml_ir_expression_index bound_variable = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = add_pass_expression(
            pass,
            &(sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_LET,
                                  .source_span = span,
                                  .let = {.variable = quantified->variable,
                                          .bound_expression = focus,
                                          .body = quantified->condition,
                                          .is_content = false}},
            &bound_variable);
    }
    sxml_ir_expression_index chosen = {.value = 0};
    if (status == SXML_STATUS_OK) {
        status = add_pass_expression(
            pass,
            &(sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                                  .source_span = span,
                                  .conditional = {.condition = sxml_ir_slot_atom(parts->new_slot),
                                                  .then_branch = bound_variable,
                                                  .else_branch = otherwise}},
            &chosen);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_pass_expression(
        pass,
        &(sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_LET,
                              .source_span = span,
                              .let = {.variable = parts->new_slot,
                                      .bound_expression = filter->predicate,
                                      .body = chosen,
                                      .is_content = false}},
        condition);
}

/* The condition of the fused QUANTIFIED over a MAP's input. */
static sxml_status fuse_map_condition(loop_pass *pass, const fusion_parts *parts,
                                      sxml_ir_expression_index *condition) {
    sxml_ir_expression inner = parts->quantified;
    inner.quantified.input = sxml_ir_slot_atom(parts->new_slot);
    inner.item_demand = 0;
    sxml_ir_expression_index inner_index = {.value = 0};
    const sxml_status status = add_pass_expression(pass, &inner, &inner_index);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_pass_expression(
        pass,
        &(sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_LET,
                              .source_span = parts->quantified.source_span,
                              .let = {.variable = parts->new_slot,
                                      .bound_expression = parts->producer.map.body,
                                      .body = inner_index,
                                      .is_content = false}},
        condition);
}

/* What fuse_quantified did. */
typedef struct fusion_outcome {
    bool is_fused;
} fusion_outcome;

/* A QUANTIFIED member of the function being fused. */
typedef struct fusion_request {
    const fusion_state *state;
    uint32_t function_number;
    size_t member_number;
} fusion_request;

/* Fuses the QUANTIFIED at the member with the producer of its input,
   when it may (see above). */
static sxml_status fuse_quantified(loop_pass *pass, fusion_request request,
                                   fusion_outcome *outcome) {
    const fusion_state *state = request.state;
    outcome->is_fused = false;
    const sxml_ir_expression_index index = member_at(&state->demand, request.member_number)->index;
    fusion_parts parts = {.quantified = expression_at(pass, index)};
    const sxml_ir_quantified *quantified = &parts.quantified.quantified;
    const sxml_ir_atom input = quantified->input;
    const sxml_ir_expression_index let_index = single_let_of(&state->census, input);
    if (is_left_alone(pass, index) || let_index.value == 0 || is_left_alone(pass, let_index) ||
        read_count_of(&state->census, input.slot) != 1) {
        return SXML_STATUS_OK;
    }
    const sxml_ir_expression let = expression_at(pass, let_index);
    if (let.let.is_content ||
        !is_once_per_let(pass, state,
                         (once_question){.member_number = request.member_number,
                                         .let_index = let_index})) {
        return SXML_STATUS_OK;
    }
    parts.producer = expression_at(pass, let.let.bound_expression);
    sxml_ir_slot variable = {.value = 0};
    if (parts.producer.expression_kind == SXML_IR_EXPRESSION_KIND_FILTER) {
        pass_answer is_by_item = {.is_true = false};
        const sxml_status status = filter_is_by_item(
            pass, (filter_question){.census = &state->census, .filter = &parts.producer.filter},
            &is_by_item);
        if (status != SXML_STATUS_OK || !is_by_item.is_true) {
            return status;
        }
        variable = parts.producer.filter.focus.context_value;
    } else if (parts.producer.expression_kind == SXML_IR_EXPRESSION_KIND_MAP) {
        const sxml_ir_map *map = &parts.producer.map;
        if (map->flags != 0 || map->focus.context_position.value != 0 ||
            map->focus.context_size.value != 0) {
            return SXML_STATUS_OK;
        }
        variable = map->focus.context_value;
    }
    if (variable.value == 0) {
        return SXML_STATUS_OK;
    }
    sxml_ir_function function = *sxml_ir_unit_get_function(
        pass->unit, (sxml_ir_function_index){.value = request.function_number});
    if (function.slot_count == UINT32_MAX) {
        return SXML_STATUS_OK;
    }
    function.slot_count += 1;
    parts.new_slot = (sxml_ir_slot){.value = function.slot_count};
    sxml_ir_expression_index condition = {.value = 0};
    sxml_status status = parts.producer.expression_kind == SXML_IR_EXPRESSION_KIND_FILTER
                             ? fuse_filter_condition(pass, &parts, &condition)
                             : fuse_map_condition(pass, &parts, &condition);
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_replace_function(
            pass->unit, &(sxml_ir_function_replacement){
                            .index = {.value = request.function_number}, .function = function});
    }
    sxml_ir_expression fused = parts.quantified;
    fused.quantified.input = parts.producer.expression_kind == SXML_IR_EXPRESSION_KIND_FILTER
                                 ? parts.producer.filter.input
                                 : parts.producer.map.input;
    fused.quantified.variable = variable;
    fused.quantified.condition = condition;
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_replace_expression(
            pass->unit, &(sxml_ir_expression_replacement){.index = index, .expression = fused});
    }
    /* The LET of the input becomes its body. */
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_replace_expression(
            pass->unit, &(sxml_ir_expression_replacement){
                            .index = let_index, .expression = expression_at(pass, let.let.body)});
    }
    outcome->is_fused = status == SXML_STATUS_OK;
    return status;
}

static void fusion_state_finalize(loop_pass *pass, fusion_state *state) {
    pass->members = state->demand.members;
    slot_census_finalize(pass, &state->census);
    if (state->member_numbers != NULL) {
        sxml_allocator_release(pass->allocator, state->member_numbers);
    }
    state->member_numbers = NULL;
}

/* Writes, per function, whether its body holds a QUANTIFIED, from owners
   (one look at each expression): most functions hold none, and fusion
   only makes a QUANTIFIED of one, so they are spared the census and the
   members. */
static sxml_status find_quantified_functions(const loop_pass *pass, uint8_t **has_quantified) {
    void *memory = NULL;
    const sxml_status status = sxml_allocator_allocate_array(
        pass->allocator,
        (sxml_array_allocation_request){.element_count = pass->unit->function_count,
                                        .element_size = sizeof(uint8_t),
                                        .element_alignment = alignof(uint8_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *flags = memory;
    memset(flags, 0, pass->unit->function_count);
    for (uint32_t number = 1; number < pass->owner_count; number += 1) {
        const uint32_t owner = pass->owners[number];
        if (owner != 0 && owner < pass->unit->function_count &&
            sxml_ir_unit_get_expression(pass->unit, (sxml_ir_expression_index){.value = number})
                    ->expression_kind == SXML_IR_EXPRESSION_KIND_QUANTIFIED) {
            flags[owner] = 1;
        }
    }
    *has_quantified = flags;
    return SXML_STATUS_OK;
}

/* Fuses one QUANTIFIED of the function, if any may be, and writes
   whether it did. */
static sxml_status fuse_one_quantified(loop_pass *pass, uint32_t function_number,
                                       fusion_outcome *outcome) {
    outcome->is_fused = false;
    fusion_state state = {
        .census = {.uses = NULL, .use_count = 0},
        .demand = {.census = NULL,
                   .members = pass->members,
                   .known_read_counts = NULL,
                   .read_demands = NULL,
                   .slot_demands = NULL,
                   .fixed_read_counts = NULL,
                   .fixed_read_demands = NULL},
        .member_numbers = NULL,
        .member_number_count = 0};
    state.demand.census = &state.census;
    state.demand.members.element_count = 0;
    sxml_status status = start_slot_census(pass, function_number, &state.census);
    if (status == SXML_STATUS_OK) {
        const sxml_ir_function *function = sxml_ir_unit_get_function(
            pass->unit, (sxml_ir_function_index){.value = function_number});
        status = collect_demand_members(pass, &state.demand, function->body);
    }
    for (size_t member_number = 0;
         status == SXML_STATUS_OK && !outcome->is_fused &&
         member_number < state.demand.members.element_count;
         member_number += 1) {
        if (expression_at(pass, member_at(&state.demand, member_number)->index).expression_kind !=
            SXML_IR_EXPRESSION_KIND_QUANTIFIED) {
            continue;
        }
        status = fuse_quantified(pass,
                                 (fusion_request){.state = &state,
                                                  .function_number = function_number,
                                                  .member_number = member_number},
                                 outcome);
    }
    fusion_state_finalize(pass, &state);
    return status;
}

static void loop_pass_finalize(loop_pass *pass) {
    pass_array *arrays[] = {&pass->lets,          &pass->necessary, &pass->comparisons,
                            &pass->read_slots,    &pass->walk,      &pass->subtree,
                            &pass->closure_slots, &pass->members,   &pass->member_stack};
    for (size_t array_index = 0; array_index < sizeof(arrays) / sizeof(arrays[0]);
         array_index += 1) {
        pass_array_finalize(pass->allocator, arrays[array_index]);
    }
    if (pass->function_plainness != NULL) {
        sxml_allocator_release(pass->allocator, pass->function_plainness);
    }
    if (pass->copy_targets != NULL) {
        sxml_allocator_release(pass->allocator, pass->copy_targets);
    }
    if (pass->owners != NULL) {
        sxml_allocator_release(pass->allocator, pass->owners);
    }
    if (pass->is_in_stream != NULL) {
        sxml_allocator_release(pass->allocator, pass->is_in_stream);
    }
}

/* A loop pass over one unit, with empty arrays. */
static loop_pass loop_pass_start(const sxml_ir_pass_request *request) {
    return (loop_pass){
        .request = request,
        .unit = request->unit,
        .allocator = request->unit->allocator,
        .function_plainness = NULL,
        .focus_slot = {.value = 0},
        .lets = pass_array_start(sizeof(conjunct_let), alignof(conjunct_let)),
        .necessary = pass_array_start(sizeof(sxml_ir_expression_index),
                                      alignof(sxml_ir_expression_index)),
        .comparisons = pass_array_start(sizeof(sxml_ir_expression_index),
                                        alignof(sxml_ir_expression_index)),
        .read_slots = pass_array_start(sizeof(sxml_ir_slot), alignof(sxml_ir_slot)),
        .walk = pass_array_start(sizeof(sxml_ir_expression_index),
                                 alignof(sxml_ir_expression_index)),
        .subtree = pass_array_start(sizeof(sxml_ir_expression_index),
                                    alignof(sxml_ir_expression_index)),
        .closure_slots = pass_array_start(sizeof(sxml_ir_slot), alignof(sxml_ir_slot)),
        .members = pass_array_start(sizeof(demand_member), alignof(demand_member)),
        .member_stack = pass_array_start(sizeof(demand_child), alignof(demand_child)),
        .copy_targets = NULL,
        .copy_target_count = 0,
        .owners = NULL,
        .owner_count = 0,
        .is_in_stream = NULL,
        .true_constant = {.value = 0},
        .false_constant = {.value = 0},
    };
}

sxml_status sxml_ir_hoist_invariant_lets(const sxml_ir_pass_request *request) {
    loop_pass pass = loop_pass_start(request);
    const uint32_t expression_count = request->unit->expression_count;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t expression_number = 1;
         status == SXML_STATUS_OK && expression_number < expression_count;
         expression_number += 1) {
        const sxml_ir_expression_index index = {.value = expression_number};
        const sxml_ir_expression_kind kind = expression_at(&pass, index).expression_kind;
        if (kind != SXML_IR_EXPRESSION_KIND_FILTER && kind != SXML_IR_EXPRESSION_KIND_MAP &&
            kind != SXML_IR_EXPRESSION_KIND_QUANTIFIED) {
            continue;
        }
        if (pass.function_plainness == NULL) {
            status = make_function_plainness(&pass);
        }
        if (status == SXML_STATUS_OK && pass.owners == NULL) {
            status = make_owners(&pass, expression_count);
        }
        if (status == SXML_STATUS_OK) {
            status = hoist_loop(&pass, index);
        }
    }
    loop_pass_finalize(&pass);
    return status == SXML_STATUS_LIMIT_EXCEEDED ? SXML_STATUS_OK : status;
}

sxml_status sxml_ir_index_filters(const sxml_ir_pass_request *request) {
    sxml_ir_unit *unit = request->unit;
    loop_pass pass = loop_pass_start(request);
    /* The expressions lowering made; copies added below are not looked
       at again. */
    const uint32_t expression_count = unit->expression_count;
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t expression_number = 1;
         status == SXML_STATUS_OK && expression_number < expression_count;
         expression_number += 1) {
        const sxml_ir_expression_index index = {.value = expression_number};
        const sxml_ir_expression_kind kind = expression_at(&pass, index).expression_kind;
        if (kind != SXML_IR_EXPRESSION_KIND_FILTER && kind != SXML_IR_EXPRESSION_KIND_MAP) {
            continue;
        }
        if (pass.function_plainness == NULL) {
            status = make_function_plainness(&pass);
        }
        if (status == SXML_STATUS_OK && pass.owners == NULL) {
            status = make_owners(&pass, expression_count);
        }
        if (status != SXML_STATUS_OK || is_left_alone(&pass, index)) {
            continue;
        }
        status = kind == SXML_IR_EXPRESSION_KIND_FILTER ? index_filter(&pass, index)
                                                         : index_where_clause(&pass, index);
    }
    loop_pass_finalize(&pass);
    /* A unit at its expression limit keeps the filters it could index. */
    return status == SXML_STATUS_LIMIT_EXCEEDED ? SXML_STATUS_OK : status;
}

sxml_status sxml_ir_shorten_descendant_paths(const sxml_ir_pass_request *request) {
    loop_pass pass = loop_pass_start(request);
    const uint32_t expression_count = request->unit->expression_count;
    owned_expressions owned = {.starts = NULL, .expression_numbers = NULL};
    sxml_status status = make_owners(&pass, expression_count);
    if (status == SXML_STATUS_OK && request->unit->function_count > 1) {
        status = make_owned_expressions(&pass, &owned);
    }
    for (uint32_t function_number = 1;
         status == SXML_STATUS_OK && function_number < request->unit->function_count;
         function_number += 1) {
        const owned_function function = owned_function_of(&owned, function_number);
        slot_census census = {.uses = NULL, .use_count = 0};
        status = make_slot_census(&pass, function, &census);
        for (uint32_t owned_number = function.first_owned_number;
             status == SXML_STATUS_OK && owned_number < function.end_owned_number;
             owned_number += 1) {
            const uint32_t expression_number = owned.expression_numbers[owned_number];
            const sxml_ir_expression_index index = {.value = expression_number};
            if (is_left_alone(&pass, index)) {
                continue;
            }
            const sxml_ir_expression_kind kind = expression_at(&pass, index).expression_kind;
            if (kind == SXML_IR_EXPRESSION_KIND_MAP) {
                status = shorten_descendant_path(&pass, &census, index);
            } else if (kind == SXML_IR_EXPRESSION_KIND_DOCUMENT_ORDER) {
                status = pull_child_path_links(&pass, &census, index);
            }
        }
        slot_census_finalize(&pass, &census);
    }
    owned_expressions_finalize(&pass, &owned);
    loop_pass_finalize(&pass);
    return status;
}

sxml_status sxml_ir_set_item_demands(const sxml_ir_pass_request *request) {
    loop_pass pass = loop_pass_start(request);
    /* No owners: a body is a tree, and each member knows whether it is
       inside a STREAM. */
    sxml_status status = SXML_STATUS_OK;
    for (uint32_t function_number = 1;
         status == SXML_STATUS_OK && function_number < request->unit->function_count;
         function_number += 1) {
        status = set_function_demands(&pass, function_number);
    }
    loop_pass_finalize(&pass);
    return status;
}

sxml_status sxml_ir_fuse_quantified_inputs(const sxml_ir_pass_request *request) {
    loop_pass pass = loop_pass_start(request);
    uint8_t *has_quantified = NULL;
    sxml_status status = make_owners(&pass, request->unit->expression_count);
    if (status == SXML_STATUS_OK && request->unit->function_count > 1) {
        status = find_quantified_functions(&pass, &has_quantified);
    }
    for (uint32_t function_number = 1;
         status == SXML_STATUS_OK && function_number < request->unit->function_count;
         function_number += 1) {
        /* One at a time: a fused QUANTIFIED may fuse again with the
           producer of its new input. */
        fusion_outcome outcome = {.is_fused = has_quantified[function_number] != 0};
        for (size_t round = 0; status == SXML_STATUS_OK && outcome.is_fused &&
                               round < request->unit->expression_count;
             round += 1) {
            status = fuse_one_quantified(&pass, function_number, &outcome);
        }
    }
    if (has_quantified != NULL) {
        sxml_allocator_release(pass.allocator, has_quantified);
    }
    loop_pass_finalize(&pass);
    return status == SXML_STATUS_LIMIT_EXCEEDED ? SXML_STATUS_OK : status;
}
