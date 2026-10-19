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

/* Appends a copy of element; returns the allocator's failures. */
// ast-grep-ignore: pointer-parameter-without-length
static sxml_status pass_array_append(sxml_allocator allocator, pass_array *array,
                                     const void *element) {
    if (array->element_count == array->element_capacity) {
        const size_t new_capacity =
            array->element_capacity < 16 ? 16 : array->element_capacity * 2;
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
                        expression.binary.operator == SXML_IR_BINARY_OPERATOR_IS)) {
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
    for (size_t comparison_index = 0;
         status == SXML_STATUS_OK && comparison_index < pass->comparisons.element_count;
         comparison_index += 1) {
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
            if (!probe_may_answer(pass, splits[split_index].probe,
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

/* Pushes every child expression of the expression at index, of any kind,
   to pass->walk. */
static sxml_status push_every_child(loop_pass *pass, sxml_ir_expression_index index) {
    const sxml_ir_expression expression = expression_at(pass, index);
    const expression_children children = children_of(&expression);
    sxml_status status = SXML_STATUS_OK;
    for (size_t child_index = 0; status == SXML_STATUS_OK && child_index < children.child_count;
         child_index += 1) {
        status =
            pass_array_append(pass->allocator, &pass->walk, &children.children[child_index]);
    }
    sxml_ir_expression_index extra[2] = {{.value = 0}, {.value = 0}};
    const sxml_ir_expression_kind kind = expression.expression_kind;
    if (kind == SXML_IR_EXPRESSION_KIND_TRY_CATCH) {
        extra[0] = expression.try_catch.body;
        extra[1] = expression.try_catch.finally_body;
        const sxml_ir_catch_clause_view clauses =
            sxml_ir_unit_get_catch_clauses(pass->unit, expression.try_catch.catch_clauses);
        for (size_t clause_index = 0;
             status == SXML_STATUS_OK && clause_index < clauses.catch_clause_count;
             clause_index += 1) {
            status = pass_array_append(pass->allocator, &pass->walk,
                                       &clauses.catch_clauses[clause_index].handler);
        }
    } else if (kind == SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR ||
               kind == SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR ||
               kind == SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR ||
               kind == SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR ||
               kind == SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR ||
               kind == SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR ||
               kind == SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR ||
               kind == SXML_IR_EXPRESSION_KIND_SHALLOW_COPY) {
        extra[0] = expression.node_construction.content;
    } else if (kind == SXML_IR_EXPRESSION_KIND_SORT) {
        const sxml_ir_sort_key_view keys =
            sxml_ir_unit_get_sort_keys(pass->unit, expression.sort.keys);
        for (size_t key_index = 0; status == SXML_STATUS_OK && key_index < keys.sort_key_count;
             key_index += 1) {
            status =
                pass_array_append(pass->allocator, &pass->walk, &keys.sort_keys[key_index].key);
        }
    } else if (kind == SXML_IR_EXPRESSION_KIND_STREAM) {
        extra[0] = expression.stream.predicate;
        extra[1] = expression.stream.body;
    }
    for (size_t extra_index = 0; status == SXML_STATUS_OK && extra_index < 2; extra_index += 1) {
        if (extra[extra_index].value != 0) {
            status = pass_array_append(pass->allocator, &pass->walk, &extra[extra_index]);
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
            const bool marks_children =
                pass->is_in_stream[index.value] ||
                expression_at(pass, index).expression_kind == SXML_IR_EXPRESSION_KIND_STREAM;
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
    } else {
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

static void loop_pass_finalize(loop_pass *pass) {
    pass_array *arrays[] = {&pass->lets,       &pass->necessary, &pass->comparisons,
                            &pass->read_slots, &pass->walk,      &pass->subtree,
                            &pass->closure_slots};
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
        if (kind != SXML_IR_EXPRESSION_KIND_FILTER && kind != SXML_IR_EXPRESSION_KIND_MAP) {
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
