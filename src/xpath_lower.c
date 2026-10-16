/*
 * Lowering of XPath 4.0 to the shared IR, and compilation
 * (src/xpath_static_internal.h; src/ir_internal.h documents the IR each
 * construct lowers to; docs/DECISIONS.md, D46, D55, D63).
 *
 * Golden file: src/memory.c; the frame machine follows the iterative
 * walks of src/xpath_parse.c.
 *
 * Administrative normal form. Every operand of an IR expression is an
 * atom, so an operand that is not a literal, a variable or the focus is
 * computed first and bound by LET. A lowering_block collects those LETs
 * while one expression is lowered: binding appends a LET whose body is
 * filled later (the next LET, or the block's result), so the LET chain
 * nests in evaluation order. Branches, loop bodies and function bodies are
 * blocks of their own.
 *
 * No recursion (D71). Lowering is a machine of frames in allocator memory
 * (see "The machine" below): nesting costs heap, bounded by
 * maximum_expression_depth, and the C stack stays the same however deep
 * the expression is.
 *
 * Chains. The grammar makes binary operators, paths, filters, lookups,
 * dynamic calls and arrows left-recursive (D29), so "1 + 2 + ... + 1000"
 * is an AST a thousand nodes deep along its left operands. Lowering walks
 * such a spine in a loop: down to its first operand, then up through the
 * parent links, so a chain costs one frame however long it is, and its
 * links do not count against maximum_expression_depth.
 *
 * Variables. A variable scope holds (name, function level, atom) entries,
 * innermost last. An inline function reads variables of enclosing
 * functions through captured slots (D38, D46): before its body is lowered,
 * its free variables are found by walking its AST (iteratively, through
 * the AST's parent links), and each becomes a slot after the parameters,
 * captured from the enclosing frame. Global (external) variables read
 * inside a function are captured the same way (src/evaluator_internal.h,
 * "Globals").
 *
 * The focus. Each IR function has a current focus: absent in an inline
 * function body (reading it lowers to a RAISE of XPDY0002, XPath 4.0,
 * section 4.5.6), slots bound by the main function, MAP or FILTER (made on
 * first read, so a part of the focus nobody reads stays 0), or fixed (the
 * argument of a focus function and the value of "->", with position and
 * size 1).
 */
#include "xpath_static_internal.h"

#include "decimal_internal.h"
#include "ir_pass_internal.h"

#include <math.h>
#include <stdalign.h>
#include <string.h>


/* ----------------------------------------------------------------- State */

typedef struct scope_entry {
    sxml_name_identifier variable_name;
    /* The level of the function that binds it: 0 for the main function,
       one more for each inline function inside. */
    uint32_t function_level;
    /* Where its value is, in that function's frame. */
    sxml_ir_atom atom;
} scope_entry;

/* A free variable of an inline function: a captured slot, or the
   constant a let bound it to. */
typedef struct capture_entry {
    sxml_name_identifier variable_name;
    sxml_ir_atom atom;
} capture_entry;

typedef enum focus_kind {
    /* No focus: an inline function body. */
    SXML_LOWERING_FOCUS_KIND_ABSENT,
    /* Slots made on first read: the main function, MAP and FILTER. */
    SXML_LOWERING_FOCUS_KIND_SLOTS,
    /* A fixed value, position and size 1: a focus function, "->". */
    SXML_LOWERING_FOCUS_KIND_FIXED,
} focus_kind;

typedef struct lowering_focus {
    focus_kind kind;
    sxml_ir_focus slots;
    sxml_ir_atom fixed_value;
} lowering_focus;

typedef struct lowering_function {
    sxml_ir_function_index function_index;
    uint32_t function_level;
    uint32_t slot_count;
    lowering_focus *focus;
    /* The free variables of an inline function and their slots. */
    capture_entry *captures;
    size_t capture_count;
} lowering_function;

typedef struct lowerer {
    const sxml_ast *ast;
    const sxml_xpath_static_context *static_context;
    sxml_error_record *error_record;
    sxml_ir_unit *unit;
    sxml_allocator allocator;
    sxml_name_pool *name_pool;
    sxml_type_registry *type_registry;
    sxml_item_heap *item_heap;
    scope_entry *scope_entries;
    size_t scope_entry_count;
    size_t scope_entry_capacity;
    lowering_function *function;
    /* Per AST node, whether its subtree has a variable reference; made
       for the first inline function (find_variable_subtrees). */
    bool *has_variable_reference;
    uint32_t depth;
    uint32_t maximum_depth;
    /* Constants made once per unit; index 0 until first needed. */
    sxml_ir_constant_index true_constant;
    sxml_ir_constant_index false_constant;
    sxml_ir_constant_index one_constant;
} lowerer;

/* The LETs bound while lowering one expression; see the file comment. */
typedef struct lowering_block {
    sxml_ir_expression_index first;
    sxml_ir_expression_index open_let;
} lowering_block;

/* ---------------------------------------------------------------- Errors */

/* code_and_description is "CODE description": an eight-character error
   code, a space and the description, static text. */
static sxml_status report_static_error(const lowerer *state,
                                       sxml_ast_node_index node,
                                       sxml_byte_view code_and_description) {
    return sxml_xpath_report_static_error(
        &(sxml_xpath_static_error){
            .ast = state->ast,
            .node = node,
            .code_local_name = {.bytes = code_and_description.bytes,
                                .byte_count = 8},
            .description = {.bytes = code_and_description.bytes + 9,
                            .byte_count = code_and_description.byte_count - 9}},
        state->error_record);
}

static sxml_status report_unsupported(const lowerer *state,
                                      sxml_ast_node_index node,
                                      sxml_byte_view description) {
    if (state->error_record != NULL) {
        state->error_record->diagnostic = (sxml_diagnostic){
            .description = description,
            .location = sxml_ast_get_source_location(state->ast, node)};
    }
    return SXML_STATUS_UNSUPPORTED_FEATURE;
}

/* ------------------------------------------------------------- AST reads */

static const sxml_ast_node *node_of(const lowerer *state,
                                    sxml_ast_node_index node) {
    return sxml_ast_get_node(state->ast, node);
}

static sxml_ast_node_index child_of(const lowerer *state,
                                    sxml_ast_node_index parent,
                                    sxml_ast_field field) {
    return sxml_ast_find_child(
        state->ast, (sxml_ast_child_query){.parent = parent, .field = field});
}

static sxml_ast_node_index next_of(const lowerer *state,
                                   sxml_ast_node_index node) {
    return sxml_ast_find_next_with_same_field(state->ast, node);
}

static sxml_byte_view text_of(const lowerer *state, sxml_ast_node_index node) {
    return sxml_ast_get_text(state->ast, node_of(state, node)->payload.text);
}

static sxml_ir_source_span span_of(const lowerer *state,
                                   sxml_ast_node_index node) {
    const sxml_ast_source_span span = node_of(state, node)->source_span;
    return (sxml_ir_source_span){.byte_offset = span.byte_offset,
                                 .byte_count = span.byte_count,
                                 .line_number = span.line_number,
                                 .column_number = span.column_number};
}

/* ------------------------------------------------------------ Memory */

typedef struct scratch_request {
    size_t element_count;
    size_t element_size;
    size_t element_alignment;
} scratch_request;

static sxml_status allocate_scratch(const lowerer *state,
                                    scratch_request request, void **memory) {
    /* At least one element, so that a successful call never gives NULL
       and code indexing the array needs no special case for none. */
    const size_t element_count =
        request.element_count == 0 ? 1 : request.element_count;
    size_t byte_count = 0;
    sxml_status status =
        sxml_size_multiply(element_count, request.element_size, &byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_allocator_allocate(
        state->allocator,
        (sxml_allocation_request){.byte_count = byte_count,
                                  .alignment = request.element_alignment},
        memory);
}

static void release_scratch(const lowerer *state, void *memory) {
    sxml_allocator_release(state->allocator, memory);
}

/* ---------------------------------------------------------- Expressions */

typedef struct expression_to_add {
    sxml_ast_node_index node;
    sxml_ir_expression expression;
} expression_to_add;

static sxml_status add_expression(lowerer *state, expression_to_add request,
                                  sxml_ir_expression_index *index) {
    request.expression.source_span = span_of(state, request.node);
    return sxml_ir_unit_add_expression(state->unit, &request.expression,
                                       index);
}

static sxml_status new_slot(lowerer *state, sxml_ir_slot *slot) {
    if (state->function->slot_count == UINT32_MAX - 1U) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    state->function->slot_count += 1;
    *slot = (sxml_ir_slot){.value = state->function->slot_count};
    return SXML_STATUS_OK;
}

static sxml_ir_atom slot_atom(sxml_ir_slot slot) {
    return (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_SLOT, .slot = slot};
}

static sxml_ir_atom constant_atom(sxml_ir_constant_index constant) {
    return (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_CONSTANT,
                          .constant = constant};
}

/* Sets the body of a LET whose body is still 0. */
static sxml_status set_let_body(lowerer *state, sxml_ir_expression_index let,
                                sxml_ir_expression_index body) {
    sxml_ir_expression_replacement replacement = {
        .index = let,
        .expression = *sxml_ir_unit_get_expression(state->unit, let)};
    replacement.expression.let.body = body;
    return sxml_ir_unit_replace_expression(state->unit, &replacement);
}

/* A value to bind in a block: appends "LET $new := expression". */
typedef struct binding_request {
    lowering_block *block;
    sxml_ir_expression_index expression;
} binding_request;

static sxml_status bind_in_block(lowerer *state, binding_request request,
                                 sxml_ir_atom *atom) {
    const sxml_ir_expression *bound =
        sxml_ir_unit_get_expression(state->unit, request.expression);
    if (bound->expression_kind == SXML_IR_EXPRESSION_KIND_ATOM) {
        /* An atom needs no slot of its own. */
        *atom = bound->atom;
        return SXML_STATUS_OK;
    }
    const sxml_ir_source_span span = bound->source_span;
    sxml_ir_slot slot = {0};
    sxml_status status = new_slot(state, &slot);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_expression_index let = {0};
    status = sxml_ir_unit_add_expression(
        state->unit,
        &(sxml_ir_expression){.expression_kind = SXML_IR_EXPRESSION_KIND_LET,
                              .source_span = span,
                              .let = {.variable = slot,
                                      .bound_expression = request.expression,
                                      .body = {.value = 0}}},
        &let);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    lowering_block *block = request.block;
    if (block->open_let.value != 0) {
        status = set_let_body(state, block->open_let, let);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    } else {
        block->first = let;
    }
    block->open_let = let;
    *atom = slot_atom(slot);
    return SXML_STATUS_OK;
}

/* Ends a block with result: the outermost LET, or result itself. */
typedef struct block_result {
    lowering_block block;
    sxml_ir_expression_index result;
} block_result;

static sxml_status finish_block(lowerer *state, block_result request,
                                sxml_ir_expression_index *expression) {
    if (request.block.open_let.value == 0) {
        *expression = request.result;
        return SXML_STATUS_OK;
    }
    sxml_status status =
        set_let_body(state, request.block.open_let, request.result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *expression = request.block.first;
    return SXML_STATUS_OK;
}

typedef struct atom_at {
    sxml_ast_node_index node;
    sxml_ir_atom atom;
} atom_at;

static sxml_status add_atom_expression(lowerer *state, atom_at request,
                                       sxml_ir_expression_index *expression) {
    return add_expression(
        state,
        (expression_to_add){
            .node = request.node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM,
                           .atom = request.atom}},
        expression);
}

static sxml_status add_empty_sequence(lowerer *state, sxml_ast_node_index node,
                                      sxml_ir_expression_index *expression) {
    return add_expression(
        state,
        (expression_to_add){
            .node = node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                           .sequence = {.first_atom_index = 0,
                                        .atom_count = 0}}},
        expression);
}

/* ------------------------------------------------------------ Constants */

/* Adds item, owned, as a constant; the item is released either way. */
static sxml_status add_item_constant(lowerer *state, sxml_item item,
                                     sxml_ir_atom *atom) {
    const sxml_sequence value = sxml_sequence_from_item(item);
    sxml_ir_constant_index constant = {0};
    sxml_status status =
        sxml_ir_unit_add_constant(state->unit, value, &constant);
    sxml_sequence_release(value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *atom = constant_atom(constant);
    return SXML_STATUS_OK;
}

typedef enum shared_constant {
    SXML_LOWERING_SHARED_CONSTANT_TRUE,
    SXML_LOWERING_SHARED_CONSTANT_FALSE,
    SXML_LOWERING_SHARED_CONSTANT_ONE,
} shared_constant;

static sxml_status get_shared_constant(lowerer *state, shared_constant which,
                                       sxml_ir_atom *atom) {
    sxml_ir_constant_index *cached = NULL;
    sxml_item item = {0};
    switch (which) {
    case SXML_LOWERING_SHARED_CONSTANT_TRUE:
        cached = &state->true_constant;
        item = sxml_atomic_item_true();
        break;
    case SXML_LOWERING_SHARED_CONSTANT_FALSE:
        cached = &state->false_constant;
        item = sxml_atomic_item_false();
        break;
    case SXML_LOWERING_SHARED_CONSTANT_ONE:
        cached = &state->one_constant;
        item = sxml_atomic_item_from_integer(1);
        break;
    }
    if (cached->value != 0) {
        *atom = constant_atom(*cached);
        return SXML_STATUS_OK;
    }
    sxml_ir_atom new_atom = {0};
    sxml_status status = add_item_constant(state, item, &new_atom);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *cached = new_atom.constant;
    *atom = new_atom;
    return SXML_STATUS_OK;
}

typedef struct atomic_constant {
    sxml_type_identifier type;
    sxml_atomic_datum datum;
} atomic_constant;

static sxml_status add_atomic_constant(lowerer *state, atomic_constant request,
                                       sxml_ir_atom *atom) {
    sxml_item item = {0};
    sxml_status status = sxml_atomic_item_make(
        state->item_heap,
        &(sxml_atomic_item_request){.type = request.type,
                                    .datum = request.datum},
        &item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_item_constant(state, item, atom);
}

static sxml_status add_string_constant(lowerer *state, sxml_byte_view text,
                                       sxml_ir_atom *atom) {
    sxml_string string = {0};
    sxml_status status = sxml_string_from_utf8(text, &string);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_atomic_constant(
        state,
        (atomic_constant){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_STRING),
            .datum = {.datum_kind = SXML_DATUM_KIND_STRING,
                      .datum_details = {.string = string}}},
        atom);
}

/* Decimal digits of an unsigned integer written in base 2, 10 or 16, as
   the canonical text xs:integer cells take (D36). */
typedef struct integer_digits {
    sxml_byte_view digits;
    uint32_t base;
} integer_digits;

static uint32_t digit_value(uint8_t digit) {
    if (digit >= '0' && digit <= '9') {
        return (uint32_t)(digit - '0');
    }
    if (digit >= 'a' && digit <= 'f') {
        return (uint32_t)(digit - 'a') + 10U;
    }
    return (uint32_t)(digit - 'A') + 10U;
}

/* An integer literal (XPath 4.0, A.3.1, IntegerLiteral, HexIntegerLiteral,
   BinaryIntegerLiteral): inline when it fits int64_t, otherwise a
   decimal cell (D36). */
static sxml_status add_integer_constant(lowerer *state, integer_digits request,
                                        sxml_ir_atom *atom) {
    uint64_t value = 0;
    bool fits = true;
    for (size_t digit_index = 0; digit_index < request.digits.byte_count;
         digit_index += 1) {
        const uint32_t digit = digit_value(request.digits.bytes[digit_index]);
        if (value > ((uint64_t)INT64_MAX - digit) / request.base) {
            fits = false;
            break;
        }
        value = value * request.base + digit;
    }
    if (fits) {
        return add_item_constant(
            state, sxml_atomic_item_from_integer((int64_t)value), atom);
    }
    /* Convert to decimal digits: each input digit multiplies the decimal
       digits so far by the base; base 10 is already decimal. A literal
       of n digits in base b has at most n * log10(b) + 1 < 2n + 1 decimal
       digits. */
    sxml_byte_view decimal_text = request.digits;
    void *memory = NULL;
    if (request.base != 10) {
        size_t capacity = 0;
        sxml_status status =
            sxml_size_multiply(request.digits.byte_count, 2, &capacity);
        if (status == SXML_STATUS_OK) {
            status = sxml_size_add(capacity, 1, &capacity);
        }
        if (status == SXML_STATUS_OK) {
            status = allocate_scratch(
                state,
                (scratch_request){.element_count = capacity,
                                  .element_size = 1,
                                  .element_alignment = 1},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        uint8_t *digits = memory;
        /* digits[0 .. digit_count) holds the value, least significant
           first. */
        size_t digit_count = 1;
        digits[0] = 0;
        for (size_t input_index = 0; input_index < request.digits.byte_count;
             input_index += 1) {
            uint32_t carry = digit_value(request.digits.bytes[input_index]);
            for (size_t digit_index = 0; digit_index < digit_count;
                 digit_index += 1) {
                const uint32_t product =
                    (uint32_t)digits[digit_index] * request.base + carry;
                digits[digit_index] = (uint8_t)(product % 10U);
                carry = product / 10U;
            }
            while (carry > 0) {
                digits[digit_count] = (uint8_t)(carry % 10U);
                digit_count += 1;
                carry /= 10U;
            }
        }
        for (size_t digit_index = 0; digit_index < digit_count / 2;
             digit_index += 1) {
            const uint8_t swapped = digits[digit_index];
            digits[digit_index] = digits[digit_count - 1 - digit_index];
            digits[digit_count - 1 - digit_index] = swapped;
        }
        for (size_t digit_index = 0; digit_index < digit_count;
             digit_index += 1) {
            digits[digit_index] = (uint8_t)('0' + digits[digit_index]);
        }
        decimal_text = (sxml_byte_view){.bytes = digits,
                                        .byte_count = digit_count};
    }
    sxml_status status = add_atomic_constant(
        state,
        (atomic_constant){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_INTEGER),
            .datum = {.datum_kind = SXML_DATUM_KIND_DECIMAL,
                      .datum_details = {.decimal_text = decimal_text}}},
        atom);
    release_scratch(state, memory);
    return status;
}

/*
 * A double literal (XPath 4.0, A.3.1, DoubleLiteral): the significant
 * digits of the mantissa and a power of ten are written as xs:decimal text
 * and converted with one correct rounding (D57). Values whose magnitude is
 * beyond the double range need no text: they are infinity or zero.
 */
#define DOUBLE_DIGIT_RANGE 400

static sxml_status add_double_constant(lowerer *state, sxml_byte_view text,
                                       sxml_ir_atom *atom) {
    size_t exponent_index = 0;
    while (exponent_index < text.byte_count &&
           text.bytes[exponent_index] != 'e' &&
           text.bytes[exponent_index] != 'E') {
        exponent_index += 1;
    }
    /* The exponent, saturated: beyond the digit range it makes zero or
       infinity anyway. */
    int64_t exponent = 0;
    bool is_negative_exponent = false;
    for (size_t byte_index = exponent_index + 1; byte_index < text.byte_count;
         byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        if (byte == '-') {
            is_negative_exponent = true;
        } else if (byte >= '0' && byte <= '9' && exponent < 1000000) {
            exponent = exponent * 10 + (int64_t)(byte - '0');
        }
    }
    if (is_negative_exponent) {
        exponent = -exponent;
    }
    /* value = (significant digits) * 10^scale */
    size_t digit_count = 0;
    int64_t scale = exponent;
    bool is_after_point = false;
    for (size_t byte_index = 0; byte_index < exponent_index; byte_index += 1) {
        const uint8_t byte = text.bytes[byte_index];
        if (byte == '.') {
            is_after_point = true;
            continue;
        }
        if (byte != '0' || digit_count > 0) {
            digit_count += 1;
        }
        if (is_after_point) {
            scale -= 1;
        }
    }
    const int64_t magnitude = (int64_t)digit_count + scale;
    double double_number = 0.0;
    if (digit_count > 0 && magnitude > DOUBLE_DIGIT_RANGE) {
        double_number = INFINITY;
    } else if (digit_count > 0 && magnitude >= -DOUBLE_DIGIT_RANGE) {
        const size_t zero_count = scale >= 0 ? (size_t)scale : (size_t)(-scale);
        const size_t capacity = digit_count + zero_count + 3;
        void *memory = NULL;
        sxml_status status = allocate_scratch(
            state,
            (scratch_request){
                .element_count = capacity, .element_size = 1, .element_alignment = 1},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        uint8_t *decimal_text = memory;
        size_t text_count = 0;
        /* With a negative scale, the point goes before the last
           fraction_count digits, after leading zeros when there are
           fewer digits than that. */
        const size_t fraction_count = scale < 0 ? zero_count : 0;
        if (fraction_count >= digit_count && fraction_count > 0) {
            decimal_text[text_count] = '0';
            decimal_text[text_count + 1] = '.';
            text_count += 2;
            for (size_t zero_index = digit_count; zero_index < fraction_count;
                 zero_index += 1) {
                decimal_text[text_count] = '0';
                text_count += 1;
            }
        }
        size_t written_count = 0;
        for (size_t byte_index = 0; byte_index < exponent_index;
             byte_index += 1) {
            const uint8_t byte = text.bytes[byte_index];
            if (byte == '.' || (byte == '0' && written_count == 0)) {
                continue;
            }
            if (fraction_count > 0 && fraction_count < digit_count &&
                written_count == digit_count - fraction_count) {
                decimal_text[text_count] = '.';
                text_count += 1;
            }
            decimal_text[text_count] = byte;
            text_count += 1;
            written_count += 1;
        }
        for (int64_t zero_index = 0; zero_index < scale; zero_index += 1) {
            decimal_text[text_count] = '0';
            text_count += 1;
        }
        sxml_decimal decimal = {0};
        const sxml_decimal_configuration configuration = {
            .allocator = state->allocator,
            /* At least the division digits, which the configuration
               requires even though nothing is divided. */
            .maximum_digit_count =
                capacity < SXML_DECIMAL_DEFAULT_DIVISION_DIGIT_COUNT
                    ? SXML_DECIMAL_DEFAULT_DIVISION_DIGIT_COUNT
                    : (uint32_t)capacity,
            .division_digit_count = SXML_DECIMAL_DEFAULT_DIVISION_DIGIT_COUNT,
        };
        status = sxml_decimal_parse(
            &configuration,
            (sxml_byte_view){.bytes = decimal_text, .byte_count = text_count},
            &decimal);
        release_scratch(state, memory);
        if (status != SXML_STATUS_OK) {
            /* The front end only gives well-formed literals. */
            return status == SXML_STATUS_MALFORMED_INPUT
                       ? SXML_STATUS_INVALID_ARGUMENT
                       : status;
        }
        double_number = sxml_decimal_to_double(&decimal);
        sxml_decimal_finalize(&decimal);
    }
    return add_item_constant(state, sxml_atomic_item_from_double(double_number),
                             atom);
}

/* ------------------------------------------------------------- Variables */

static sxml_status push_variable(lowerer *state, scope_entry entry) {
    if (state->scope_entry_count == state->scope_entry_capacity) {
        const size_t new_capacity = state->scope_entry_capacity < 16
                                        ? 16
                                        : state->scope_entry_capacity * 2;
        void *memory = NULL;
        sxml_status status = allocate_scratch(
            state,
            (scratch_request){.element_count = new_capacity,
                              .element_size = sizeof(scope_entry),
                              .element_alignment = alignof(scope_entry)},
            &memory);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (state->scope_entry_count > 0) {
            memcpy(memory, state->scope_entries,
                   state->scope_entry_count * sizeof(scope_entry));
        }
        release_scratch(state, state->scope_entries);
        state->scope_entries = memory;
        state->scope_entry_capacity = new_capacity;
    }
    state->scope_entries[state->scope_entry_count] = entry;
    state->scope_entry_count += 1;
    return SXML_STATUS_OK;
}

/* The expanded name of a variable name node (no default namespace). */
static sxml_status resolve_variable_name(lowerer *state,
                                         sxml_ast_node_index name_node,
                                         sxml_name_identifier *name) {
    sxml_xpath_resolved_name resolved = {0};
    sxml_status status = sxml_xpath_resolve_name(
        &(sxml_xpath_name_request){
            .ast = state->ast,
            .name_node = name_node,
            .name_role = SXML_XPATH_NAME_ROLE_NO_NAMESPACE,
            .static_context = state->static_context,
            .error_record = state->error_record},
        &resolved);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_name_pool_intern_name(
        state->name_pool,
        &(sxml_name_request){.namespace_uri = resolved.namespace_uri,
                             .local_name = resolved.local_name},
        name);
}

/* Where a variable is, seen from the current function. */
typedef struct variable_lookup {
    bool is_found;
    sxml_ir_atom atom;
} variable_lookup;

static sxml_status look_up_variable(const lowerer *state,
                                    sxml_name_identifier name,
                                    variable_lookup *lookup) {
    const lowering_function *function = state->function;
    bool is_outer = false;
    sxml_ir_atom atom = {0};
    bool is_found = false;
    for (size_t entry_number = state->scope_entry_count; entry_number > 0;
         entry_number -= 1) {
        const scope_entry *entry = &state->scope_entries[entry_number - 1];
        if (entry->variable_name.index == name.index) {
            is_found = true;
            is_outer = entry->function_level != function->function_level;
            atom = entry->atom;
            break;
        }
    }
    if (!is_found) {
        const sxml_variable_declaration_view declarations =
            state->static_context->variable_declarations;
        for (size_t declaration_index = 0;
             declaration_index < declarations.variable_declaration_count;
             declaration_index += 1) {
            if (declarations.variable_declarations[declaration_index]
                    .variable_name.index == name.index) {
                is_found = true;
                is_outer = function->function_level != 0;
                atom = (sxml_ir_atom){
                    .atom_kind = SXML_IR_ATOM_KIND_GLOBAL_VARIABLE,
                    .global_variable = {.value =
                                            (uint32_t)declaration_index + 1U}};
                break;
            }
        }
    }
    if (is_found && is_outer) {
        /* A variable of an enclosing function or a global read inside a
           function: its captured slot. */
        is_found = false;
        for (size_t capture_index = 0; capture_index < function->capture_count;
             capture_index += 1) {
            if (function->captures[capture_index].variable_name.index ==
                name.index) {
                atom = function->captures[capture_index].atom;
                is_found = true;
                break;
            }
        }
        if (!is_found) {
            /* The free variable analysis missed it: an error in sxml. */
            return SXML_STATUS_INVALID_ARGUMENT;
        }
    }
    *lookup = (variable_lookup){.is_found = is_found, .atom = atom};
    return SXML_STATUS_OK;
}

static sxml_status lower_variable_reference(lowerer *state,
                                            sxml_ast_node_index node,
                                            sxml_ir_atom *atom) {
    sxml_name_identifier name = {0};
    sxml_status status =
        resolve_variable_name(state, child_of(state, node, SXML_AST_FIELD_NAME),
                              &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    variable_lookup lookup = {0};
    status = look_up_variable(state, name, &lookup);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!lookup.is_found) {
        /* XPath 4.0, section 4.3.3: the variable must be in scope. */
        return report_static_error(state, node,
                                   SXML_BYTE_VIEW_FROM_LITERAL("XPST0008 the variable is not declared"));
    }
    *atom = lookup.atom;
    return SXML_STATUS_OK;
}

/* ----------------------------------------------------------------- Focus */

typedef enum focus_part {
    SXML_LOWERING_FOCUS_PART_VALUE,
    SXML_LOWERING_FOCUS_PART_POSITION,
    SXML_LOWERING_FOCUS_PART_SIZE,
} focus_part;

/* A raise of an error known statically, bound in block. */
typedef struct raise_request {
    sxml_ast_node_index node;
    sxml_status error_status;
    sxml_byte_view code_local_name;
    sxml_byte_view description;
} raise_request;

static sxml_status add_raise(lowerer *state, raise_request request,
                             sxml_ir_expression_index *expression) {
    return add_expression(
        state,
        (expression_to_add){
            .node = request.node,
            .expression = {
                .expression_kind = SXML_IR_EXPRESSION_KIND_RAISE,
                .raise = {.error_status = request.error_status,
                          .error_code = {.namespace_uri = SXML_BYTE_VIEW_FROM_LITERAL(
                                             SXML_ERROR_NAMESPACE_URI),
                                         .local_name = request.code_local_name,
                                         .namespace_prefix =
                                             SXML_BYTE_VIEW_FROM_LITERAL("err")},
                          .description = request.description}}},
        expression);
}

static sxml_status add_absent_focus_raise(lowerer *state,
                                          sxml_ast_node_index node,
                                          sxml_ir_expression_index *expression) {
    /* XPath 4.0, section 4.5.6: the body of an inline function has no
       focus; reading it raises XPDY0002 when evaluated. */
    return add_raise(
        state,
        (raise_request){
            .node = node,
            .error_status = SXML_STATUS_DYNAMIC_ERROR,
            .code_local_name = SXML_BYTE_VIEW_FROM_LITERAL("XPDY0002"),
            .description = SXML_BYTE_VIEW_FROM_LITERAL(
                "the focus is absent in a function body")},
        expression);
}

typedef struct focus_read {
    lowering_block *block;
    sxml_ast_node_index node;
    focus_part part;
} focus_read;

/* The atom of one part of the current focus, made on first read. */
static sxml_status read_focus(lowerer *state, focus_read request,
                              sxml_ir_atom *atom) {
    lowering_focus *focus = state->function->focus;
    switch (focus->kind) {
    case SXML_LOWERING_FOCUS_KIND_ABSENT: {
        sxml_ir_expression_index raise = {0};
        sxml_status status =
            add_absent_focus_raise(state, request.node, &raise);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return bind_in_block(
            state,
            (binding_request){.block = request.block, .expression = raise},
            atom);
    }
    case SXML_LOWERING_FOCUS_KIND_FIXED:
        if (request.part == SXML_LOWERING_FOCUS_PART_VALUE) {
            *atom = focus->fixed_value;
            return SXML_STATUS_OK;
        }
        return get_shared_constant(state, SXML_LOWERING_SHARED_CONSTANT_ONE, atom);
    case SXML_LOWERING_FOCUS_KIND_SLOTS:
        break;
    }
    sxml_ir_slot *slot = request.part == SXML_LOWERING_FOCUS_PART_VALUE
                             ? &focus->slots.context_value
                         : request.part == SXML_LOWERING_FOCUS_PART_POSITION
                             ? &focus->slots.context_position
                             : &focus->slots.context_size;
    if (slot->value == 0) {
        sxml_status status = new_slot(state, slot);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    *atom = slot_atom(*slot);
    return SXML_STATUS_OK;
}

/* ---------------------------------------------------- Free variables */

/*
 * Which AST nodes have a variable reference in their subtree: one flag per
 * node, found once per lowering with a walk through the AST's links. The
 * free variable analysis of an inline function skips subtrees without
 * one, so nested inline functions without variables cost linear time, not
 * a walk of every enclosing function's whole body each.
 */
static sxml_status find_variable_subtrees(lowerer *state) {
    const sxml_ast *ast = state->ast;
    void *memory = NULL;
    sxml_status status = allocate_scratch(
        state,
        (scratch_request){.element_count = ast->node_count,
                          .element_size = sizeof(bool),
                          .element_alignment = alignof(bool)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    bool *has_reference = memory;
    for (uint32_t node_index = 0; node_index < ast->node_count; node_index += 1) {
        has_reference[node_index] =
            ast->nodes[node_index].kind == SXML_AST_KIND_VARIABLE_REFERENCE;
    }
    /* Postorder: a node is done when the walk leaves it, and passes its
       flag to its parent. */
    sxml_ast_node_index node = ast->root;
    while (node.value != 0) {
        const sxml_ast_node *current = node_of(state, node);
        if (current->first_child.value != 0) {
            node = current->first_child;
            continue;
        }
        for (;;) {
            const sxml_ast_node *done = node_of(state, node);
            if (node.value == ast->root.value) {
                node = (sxml_ast_node_index){.value = 0};
                break;
            }
            if (has_reference[node.value]) {
                has_reference[done->parent.value] = true;
            }
            if (done->next_sibling.value != 0) {
                node = done->next_sibling;
                break;
            }
            node = done->parent;
        }
    }
    state->has_variable_reference = has_reference;
    return SXML_STATUS_OK;
}

/* The next node of a preorder walk of root's subtree, through the AST's
   links, skipping subtrees without a variable reference; 0 after the
   last. */
static sxml_ast_node_index next_in_subtree(const lowerer *state,
                                           sxml_ast_node_index root,
                                           sxml_ast_node_index node) {
    const sxml_ast_node *current = node_of(state, node);
    if (current->first_child.value != 0 &&
        state->has_variable_reference[node.value]) {
        return current->first_child;
    }
    while (node.value != root.value) {
        const sxml_ast_node *walked = node_of(state, node);
        if (walked->next_sibling.value != 0) {
            return walked->next_sibling;
        }
        node = walked->parent;
    }
    return (sxml_ast_node_index){.value = 0};
}

/* A variable name to look for among the bindings of one AST node. */
typedef struct declared_name_query {
    sxml_ast_node_index binding;
    sxml_name_identifier name;
} declared_name_query;

typedef struct declared_name {
    bool is_declared;
} declared_name;

/* Whether a binding node (a for, let or quantifier binding, a variable
   name and type, a positional variable) declares query.name. */
static sxml_status binding_declares(lowerer *state, declared_name_query query,
                                    declared_name *result) {
    sxml_ast_node_index candidates[4] = {{0}, {0}, {0}, {0}};
    size_t candidate_count = 0;
    const sxml_ast_kind kind = node_of(state, query.binding)->kind;
    if (kind == SXML_AST_KIND_VARIABLE_NAME_AND_TYPE ||
        kind == SXML_AST_KIND_POSITIONAL_VARIABLE) {
        candidates[candidate_count] = query.binding;
        candidate_count += 1;
    } else {
        for (sxml_ast_node_index child = node_of(state, query.binding)->first_child;
             child.value != 0 && candidate_count < 4;
             child = node_of(state, child)->next_sibling) {
            const sxml_ast_kind child_kind = node_of(state, child)->kind;
            if (child_kind == SXML_AST_KIND_VARIABLE_NAME_AND_TYPE ||
                child_kind == SXML_AST_KIND_POSITIONAL_VARIABLE) {
                candidates[candidate_count] = child;
                candidate_count += 1;
            } else if (child_kind == SXML_AST_KIND_FOR_ENTRY_KEY_BINDING ||
                       child_kind == SXML_AST_KIND_FOR_ENTRY_VALUE_BINDING) {
                candidates[candidate_count] =
                    child_of(state, child, SXML_AST_FIELD_VARIABLE);
                candidate_count += 1;
            }
        }
    }
    for (size_t candidate_index = 0; candidate_index < candidate_count;
         candidate_index += 1) {
        sxml_name_identifier name = {0};
        sxml_status status = resolve_variable_name(
            state,
            child_of(state, candidates[candidate_index], SXML_AST_FIELD_NAME),
            &name);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (name.index == query.name.index) {
            result->is_declared = true;
            return SXML_STATUS_OK;
        }
    }
    result->is_declared = false;
    return SXML_STATUS_OK;
}

/* Whether a sibling binding before limit (all of them when limit is 0),
   with the given field, declares name. */
typedef struct earlier_binding_query {
    sxml_ast_node_index parent;
    sxml_ast_field field;
    sxml_ast_node_index limit;
    sxml_name_identifier name;
} earlier_binding_query;

static sxml_status earlier_binding_declares(lowerer *state,
                                            earlier_binding_query query,
                                            declared_name *result) {
    for (sxml_ast_node_index binding = child_of(state, query.parent, query.field);
         binding.value != 0 && binding.value != query.limit.value;
         binding = next_of(state, binding)) {
        sxml_status status = binding_declares(
            state, (declared_name_query){.binding = binding, .name = query.name},
            result);
        if (status != SXML_STATUS_OK || result->is_declared) {
            return status;
        }
    }
    result->is_declared = false;
    return SXML_STATUS_OK;
}

/* Whether the variable reference at node, named name, is bound by a
   construct between it and the inline function function_node (XPath 4.0,
   sections 4.13, 4.15 and 4.5.6 give the scopes). */
typedef struct bound_query {
    sxml_ast_node_index reference;
    sxml_ast_node_index function_node;
    sxml_name_identifier name;
} bound_query;

static sxml_status is_bound_inside(lowerer *state, bound_query query,
                                   declared_name *result) {
    sxml_ast_node_index child = query.reference;
    sxml_ast_node_index ancestor = node_of(state, child)->parent;
    declared_name found = {.is_declared = false};
    sxml_status status = SXML_STATUS_OK;
    while (status == SXML_STATUS_OK && !found.is_declared &&
           ancestor.value != 0) {
        const sxml_ast_node *ancestor_node = node_of(state, ancestor);
        const sxml_ast_field child_field = node_of(state, child)->field;
        switch ((uint32_t)ancestor_node->kind) {
        case SXML_AST_KIND_INLINE_FUNCTION_EXPRESSION:
            if (child_field == SXML_AST_FIELD_BODY) {
                status = earlier_binding_declares(
                    state,
                    (earlier_binding_query){.parent = ancestor,
                                            .field = SXML_AST_FIELD_PARAMETER,
                                            .limit = {.value = 0},
                                            .name = query.name},
                    &found);
            }
            break;
        case SXML_AST_KIND_FOR_CLAUSE:
        case SXML_AST_KIND_LET_CLAUSE:
            /* Earlier bindings of the same clause are in scope. */
            status = earlier_binding_declares(
                state,
                (earlier_binding_query){.parent = ancestor,
                                        .field = SXML_AST_FIELD_BINDING,
                                        .limit = child,
                                        .name = query.name},
                &found);
            break;
        case SXML_AST_KIND_FLWOR_EXPRESSION:
            /* Every binding of every earlier clause, or of all clauses
               from the return clause. */
            for (sxml_ast_node_index clause =
                     child_of(state, ancestor, SXML_AST_FIELD_CLAUSE);
                 status == SXML_STATUS_OK && !found.is_declared &&
                 clause.value != 0 && clause.value != child.value;
                 clause = next_of(state, clause)) {
                status = earlier_binding_declares(
                    state,
                    (earlier_binding_query){.parent = clause,
                                            .field = SXML_AST_FIELD_BINDING,
                                            .limit = {.value = 0},
                                            .name = query.name},
                    &found);
            }
            break;
        case SXML_AST_KIND_QUANTIFIED_EXPRESSION:
            status = earlier_binding_declares(
                state,
                (earlier_binding_query){
                    .parent = ancestor,
                    .field = SXML_AST_FIELD_BINDING,
                    .limit = child_field == SXML_AST_FIELD_BINDING
                                 ? child
                                 : (sxml_ast_node_index){.value = 0},
                    .name = query.name},
                &found);
            break;
        default:
            break;
        }
        if (ancestor.value == query.function_node.value) {
            break;
        }
        child = ancestor;
        ancestor = ancestor_node->parent;
    }
    result->is_declared = found.is_declared;
    return status;
}

/* The free variables of an inline function and what they are in the
   enclosing function's frame. */
typedef struct free_variables {
    /* Every free variable, capture_count of them. */
    capture_entry *captures;
    size_t capture_count;
    size_t capture_capacity;
    /* The atoms the captured ones (not constants) have in the enclosing
       frame, captured_count of them. */
    sxml_ir_atom *outer_atoms;
    size_t captured_count;
} free_variables;

static sxml_status add_free_variable(lowerer *state, free_variables *variables,
                                     capture_entry capture) {
    if (variables->capture_count == variables->capture_capacity) {
        const size_t new_capacity =
            variables->capture_capacity < 4 ? 4
                                            : variables->capture_capacity * 2;
        void *capture_memory = NULL;
        void *atom_memory = NULL;
        sxml_status status = allocate_scratch(
            state,
            (scratch_request){.element_count = new_capacity,
                              .element_size = sizeof(capture_entry),
                              .element_alignment = alignof(capture_entry)},
            &capture_memory);
        if (status == SXML_STATUS_OK) {
            status = allocate_scratch(
                state,
                (scratch_request){.element_count = new_capacity,
                                  .element_size = sizeof(sxml_ir_atom),
                                  .element_alignment = alignof(sxml_ir_atom)},
                &atom_memory);
        }
        if (status != SXML_STATUS_OK) {
            release_scratch(state, capture_memory);
            return status;
        }
        if (variables->capture_count > 0) {
            memcpy(capture_memory, variables->captures,
                   variables->capture_count * sizeof(capture_entry));
            memcpy(atom_memory, variables->outer_atoms,
                   variables->capture_count * sizeof(sxml_ir_atom));
        }
        release_scratch(state, variables->captures);
        release_scratch(state, variables->outer_atoms);
        variables->captures = capture_memory;
        variables->outer_atoms = atom_memory;
        variables->capture_capacity = new_capacity;
    }
    variables->captures[variables->capture_count] = capture;
    variables->capture_count += 1;
    return SXML_STATUS_OK;
}

/* Finds the free variables of the inline function at function_node; slots
   are numbered from first_slot. */
typedef struct free_variable_request {
    sxml_ast_node_index function_node;
    uint32_t first_slot;
} free_variable_request;

static sxml_status find_free_variables(lowerer *state,
                                       free_variable_request request,
                                       free_variables *variables) {
    const sxml_ast_node_index body =
        child_of(state, request.function_node, SXML_AST_FIELD_BODY);
    sxml_status status = SXML_STATUS_OK;
    if (state->has_variable_reference == NULL) {
        status = find_variable_subtrees(state);
    }
    for (sxml_ast_node_index walked_node = body;
         status == SXML_STATUS_OK && walked_node.value != 0;
         walked_node = next_in_subtree(state, body, walked_node)) {
        const sxml_ast_node_index node = walked_node;
        if (node_of(state, node)->kind != SXML_AST_KIND_VARIABLE_REFERENCE) {
            continue;
        }
        sxml_name_identifier name = {0};
        status = resolve_variable_name(
            state, child_of(state, node, SXML_AST_FIELD_NAME), &name);
        if (status != SXML_STATUS_OK) {
            break;
        }
        bool is_known = false;
        for (size_t capture_index = 0;
             capture_index < variables->capture_count; capture_index += 1) {
            if (variables->captures[capture_index].variable_name.index ==
                name.index) {
                is_known = true;
                break;
            }
        }
        if (is_known) {
            continue;
        }
        declared_name bound = {.is_declared = false};
        status = is_bound_inside(
            state,
            (bound_query){.reference = node,
                          .function_node = request.function_node,
                          .name = name},
            &bound);
        if (status != SXML_STATUS_OK || bound.is_declared) {
            continue;
        }
        variable_lookup lookup = {0};
        status = look_up_variable(state, name, &lookup);
        if (status != SXML_STATUS_OK || !lookup.is_found) {
            /* Not declared anywhere: lowering the reference reports
               XPST0008 at it. */
            continue;
        }
        /* A constant needs no capture: the function reads it directly. */
        const bool is_constant =
            lookup.atom.atom_kind == SXML_IR_ATOM_KIND_CONSTANT;
        status = add_free_variable(
            state, variables,
            (capture_entry){
                .variable_name = name,
                .atom = is_constant
                            ? lookup.atom
                            : slot_atom((sxml_ir_slot){
                                  .value = request.first_slot +
                                           (uint32_t)variables->captured_count})});
        if (status == SXML_STATUS_OK && !is_constant) {
            variables->outer_atoms[variables->captured_count] = lookup.atom;
            variables->captured_count += 1;
        }
    }
    return status;
}

/* ------------------------------------------------------------- Lowering */

/* An AST node to lower and the block its LETs go to. */
typedef struct node_in_block {
    lowering_block *block;
    sxml_ast_node_index node;
} node_in_block;

/* Binds expression in block unless it is an atom. */
typedef struct expression_in_block {
    lowering_block *block;
    sxml_ir_expression_index expression;
} expression_in_block;

static sxml_status bind_expression(lowerer *state, expression_in_block request,
                                   sxml_ir_atom *atom) {
    return bind_in_block(state,
                         (binding_request){.block = request.block,
                                           .expression = request.expression},
                         atom);
}

/* Lowers a body with a new focus of kind SLOTS, and writes the slots it
   bound to *focus_slots. */
typedef struct focused_body {
    sxml_ast_node_index node;
    /* Slots already chosen (the value slot of a for clause); others are
       made on first read. */
    sxml_ir_focus preset_slots;
} focused_body;

typedef struct focused_result {
    sxml_ir_expression_index body;
    sxml_ir_focus focus;
} focused_result;

/* ----------------------------------------------------------- Literals */

static bool is_atom_kind(sxml_ast_kind kind) {
    switch ((uint32_t)kind) {
    case SXML_AST_KIND_INTEGER_LITERAL:
    case SXML_AST_KIND_HEX_INTEGER_LITERAL:
    case SXML_AST_KIND_BINARY_INTEGER_LITERAL:
    case SXML_AST_KIND_DECIMAL_LITERAL:
    case SXML_AST_KIND_DOUBLE_LITERAL:
    case SXML_AST_KIND_STRING_LITERAL:
    case SXML_AST_KIND_QNAME_LITERAL:
    case SXML_AST_KIND_VARIABLE_REFERENCE:
        return true;
    default:
        return false;
    }
}

static sxml_status lower_qname_literal(lowerer *state, sxml_ast_node_index node,
                                       sxml_ir_atom *atom) {
    const sxml_ast_node_index name_node =
        child_of(state, node, SXML_AST_FIELD_NAME);
    /* XPath 4.0, section 4.3.1.3: an unprefixed QName literal is in no
       namespace. */
    sxml_xpath_resolved_name resolved = {0};
    sxml_status status = sxml_xpath_resolve_name(
        &(sxml_xpath_name_request){
            .ast = state->ast,
            .name_node = name_node,
            .name_role = SXML_XPATH_NAME_ROLE_NO_NAMESPACE,
            .static_context = state->static_context,
            .error_record = state->error_record},
        &resolved);
    sxml_name_identifier name = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_name(
            state->name_pool,
            &(sxml_name_request){.namespace_uri = resolved.namespace_uri,
                                 .local_name = resolved.local_name},
            &name);
    }
    sxml_namespace_prefix_identifier prefix = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_name_pool_intern_namespace_prefix(
            state->name_pool,
            sxml_ast_get_text(state->ast,
                              node_of(state, name_node)->payload.name.namespace_prefix),
            &prefix);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_atomic_constant(
        state,
        (atomic_constant){
            .type = sxml_builtin_type_identifier(SXML_TYPE_XS_QNAME),
            .datum = {.datum_kind = SXML_DATUM_KIND_QNAME,
                      .datum_details = {.qname = {.expanded_name = name,
                                                  .namespace_prefix = prefix}}}},
        atom);
}

/* The atom of a literal or variable reference. */
static sxml_status lower_atom_node(lowerer *state, sxml_ast_node_index node,
                                   sxml_ir_atom *atom) {
    switch ((uint32_t)node_of(state, node)->kind) {
    case SXML_AST_KIND_INTEGER_LITERAL:
        return add_integer_constant(
            state,
            (integer_digits){.digits = text_of(state, node), .base = 10},
            atom);
    case SXML_AST_KIND_HEX_INTEGER_LITERAL:
        return add_integer_constant(
            state,
            (integer_digits){.digits = text_of(state, node), .base = 16},
            atom);
    case SXML_AST_KIND_BINARY_INTEGER_LITERAL:
        return add_integer_constant(
            state, (integer_digits){.digits = text_of(state, node), .base = 2},
            atom);
    case SXML_AST_KIND_DECIMAL_LITERAL:
        /* XPath 4.0, A.3.1, DecimalLiteral: an xs:decimal; its text is an
           xs:decimal lexical form. */
        return add_atomic_constant(
            state,
            (atomic_constant){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_DECIMAL),
                .datum = {.datum_kind = SXML_DATUM_KIND_DECIMAL,
                          .datum_details = {.decimal_text =
                                                text_of(state, node)}}},
            atom);
    case SXML_AST_KIND_DOUBLE_LITERAL:
        return add_double_constant(state, text_of(state, node), atom);
    case SXML_AST_KIND_STRING_LITERAL:
        return add_string_constant(state, text_of(state, node), atom);
    case SXML_AST_KIND_QNAME_LITERAL:
        return lower_qname_literal(state, node, atom);
    case SXML_AST_KIND_VARIABLE_REFERENCE:
        return lower_variable_reference(state, node, atom);
    default:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
}

/* ------------------------------------------------------------ Types */

static sxml_status resolve_sequence_type(lowerer *state,
                                         sxml_ast_node_index node,
                                         sxml_sequence_type *sequence_type) {
    return sxml_xpath_resolve_sequence_type(
        &(sxml_xpath_type_request){.ast = state->ast,
                                   .type_node = node,
                                   .static_context = state->static_context,
                                   .error_record = state->error_record},
        sequence_type);
}

static sxml_status resolve_item_type(lowerer *state, sxml_ast_node_index node,
                                     sxml_type_identifier *item_type) {
    return sxml_xpath_resolve_item_type(
        &(sxml_xpath_type_request){.ast = state->ast,
                                   .type_node = node,
                                   .static_context = state->static_context,
                                   .error_record = state->error_record},
        item_type);
}

/* The sequence type of an optional TYPE_DECLARATION; item()* without. */
static sxml_status resolve_declared_type(lowerer *state,
                                         sxml_ast_node_index declaration,
                                         sxml_sequence_type *sequence_type) {
    if (declaration.value == 0) {
        *sequence_type = (sxml_sequence_type){
            .item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
            .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};
        return SXML_STATUS_OK;
    }
    return resolve_sequence_type(
        state, child_of(state, declaration, SXML_AST_FIELD_TYPE), sequence_type);
}

static bool accepts_everything(sxml_sequence_type sequence_type) {
    return sequence_type.item_type.index == SXML_TYPE_ITEM &&
           sequence_type.occurrence == SXML_OCCURRENCE_ZERO_OR_MORE;
}

/* A value coerced to a type (XPath 4.0, section 3.4), bound in block; the
   value itself when the type accepts everything. */
typedef struct coercion_request {
    lowering_block *block;
    sxml_ast_node_index node;
    sxml_ir_atom value;
    sxml_sequence_type sequence_type;
    sxml_ir_coercion_role role;
} coercion_request;

/* True when a constant atom already matches sequence_type, so coercing
   it would change nothing: one atomic item whose type is a subtype of the
   item type (XPath 4.0, section 3.4.2, no conversion applies). */
typedef struct constant_match {
    sxml_ir_atom value;
    sxml_sequence_type sequence_type;
} constant_match;

static bool constant_matches(const lowerer *state, constant_match request) {
    if (request.value.atom_kind != SXML_IR_ATOM_KIND_CONSTANT ||
        (request.sequence_type.occurrence & SXML_OCCURRENCE_EXACTLY_ONE) == 0) {
        return false;
    }
    const sxml_sequence value =
        sxml_ir_unit_get_constant(state->unit, request.value.constant);
    uint64_t item_count = 0;
    sxml_item item = {0};
    if (sxml_sequence_count(value, NULL, &item_count) != SXML_STATUS_OK ||
        item_count != 1 ||
        sxml_sequence_get_item(
            value, (sxml_item_request){.item_index = 0, .error_record = NULL},
            &item) != SXML_STATUS_OK ||
        sxml_item_get_kind(item) != SXML_ITEM_KIND_ATOMIC) {
        return false;
    }
    return sxml_type_registry_is_subtype(
        state->type_registry,
        (sxml_type_pair){.derived_type = sxml_item_get_type_annotation(item),
                         .base_type = request.sequence_type.item_type});
}

static sxml_status coerce_atom(lowerer *state, coercion_request request,
                               sxml_ir_atom *atom) {
    if (accepts_everything(request.sequence_type) ||
        constant_matches(state,
                         (constant_match){.value = request.value,
                                          .sequence_type =
                                              request.sequence_type})) {
        *atom = request.value;
        return SXML_STATUS_OK;
    }
    sxml_ir_expression_index coercion = {0};
    sxml_status status = add_expression(
        state,
        (expression_to_add){
            .node = request.node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_COERCE,
                           .coercion = {.value = request.value,
                                        .sequence_type = request.sequence_type,
                                        .role = request.role}}},
        &coercion);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind_expression(
        state,
        (expression_in_block){.block = request.block, .expression = coercion},
        atom);
}

/* Registers function(parameter_types) as result_type. */
typedef struct signature_request {
    sxml_sequence_type_view parameter_types;
    sxml_sequence_type result_type;
} signature_request;

static sxml_status register_signature(lowerer *state, signature_request request,
                                      sxml_type_identifier *signature) {
    return sxml_type_registry_register_item_type(
        state->type_registry,
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_FUNCTION,
            .variety_details = {.function_type = {.is_any_function = false,
                                                  .parameter_types =
                                                      request.parameter_types,
                                                  .result_type =
                                                      request.result_type}}},
        signature);
}

/* ------------------------------------------------------------ Functions */

/* Enters a new IR function: its frame, focus and scope level. */
typedef struct function_frame {
    lowering_function function;
    lowering_focus focus;
    lowering_function *outer_function;
    size_t outer_scope_entry_count;
} function_frame;

static void enter_function(lowerer *state, function_frame *frame) {
    frame->outer_function = state->function;
    frame->outer_scope_entry_count = state->scope_entry_count;
    frame->function.focus = &frame->focus;
    state->function = &frame->function;
}

static void leave_function(lowerer *state, function_frame *frame) {
    state->function = frame->outer_function;
    state->scope_entry_count = frame->outer_scope_entry_count;
}

/* A function of the unit made by lowering itself: a partial application,
   a focus-dependent function reference. Its body is one expression over
   its parameters and captures. */
typedef struct closure_request {
    sxml_ast_node_index node;
    sxml_type_identifier signature;
    uint32_t parameter_count;
    sxml_ir_atom_view captured_atoms;
} closure_request;

/* Adds the closure's function (body still 0) and writes its index. */
static sxml_status add_closure_function(lowerer *state, closure_request request,
                                        sxml_ir_function_index *function) {
    sxml_ir_atom_list captures = {0};
    sxml_status status =
        sxml_ir_unit_add_atoms(state->unit, request.captured_atoms, &captures);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const uint64_t slot_count =
        (uint64_t)request.parameter_count + request.captured_atoms.atom_count;
    if (slot_count >= UINT32_MAX) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    return sxml_ir_unit_add_function(
        state->unit,
        &(sxml_ir_function){
            .function_name = {.index = 0},
            .signature = request.signature,
            .parameter_count = request.parameter_count,
            .captured_variables = captures,
            .defining_function = state->function->function_index,
            .slot_count = (uint32_t)slot_count,
            .body = {.value = 0},
            .source_span = span_of(state, request.node)},
        function);
}

typedef struct closure_body {
    sxml_ir_function_index function;
    sxml_ir_expression body;
    sxml_ast_node_index node;
} closure_body;

/* Sets a closure's body and writes the FUNCTION_DEFINITION that makes it. */
static sxml_status finish_closure(lowerer *state, closure_body request,
                                  sxml_ir_expression_index *definition) {
    sxml_ir_expression_index body = {0};
    sxml_status status = add_expression(
        state, (expression_to_add){.node = request.node,
                                   .expression = request.body},
        &body);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_function_replacement replacement = {
        .index = request.function,
        .function = *sxml_ir_unit_get_function(state->unit, request.function)};
    replacement.function.body = body;
    status = sxml_ir_unit_replace_function(state->unit, &replacement);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_expression(
        state,
        (expression_to_add){
            .node = request.node,
            .expression = {
                .expression_kind = SXML_IR_EXPRESSION_KIND_FUNCTION_DEFINITION,
                .function_definition = {.function = request.function}}},
        definition);
}

/* --------------------------------------------------- Static calls */

/* One argument of a call being prepared. */
typedef struct call_argument {
    bool is_placeholder;
    sxml_ir_atom atom;
} call_argument;

/* A static call or partial application, resolved, with its arguments as
   atoms (placeholders aside), not yet emitted. */
typedef struct prepared_call {
    sxml_ast_node_index node;
    sxml_function_entry entry;
    call_argument *arguments;
    size_t argument_count;
    size_t placeholder_count;
} prepared_call;

static sxml_ir_function_target target_of(const sxml_function_entry *entry) {
    switch (entry->entry_kind) {
    case SXML_FUNCTION_ENTRY_KIND_NATIVE:
        return (sxml_ir_function_target){
            .target_kind = SXML_IR_FUNCTION_TARGET_KIND_NATIVE,
            .native_function = entry->native_function};
    case SXML_FUNCTION_ENTRY_KIND_CONSTRUCTOR:
        return (sxml_ir_function_target){
            .target_kind = SXML_IR_FUNCTION_TARGET_KIND_CONSTRUCTOR,
            .constructor_type = entry->constructor_type};
    case SXML_FUNCTION_ENTRY_KIND_IR_FUNCTION:
        return (sxml_ir_function_target){
            .target_kind = SXML_IR_FUNCTION_TARGET_KIND_IR_FUNCTION,
            .function = {.value = entry->ir_function_number}};
    case SXML_FUNCTION_ENTRY_KIND_NONE:
        break;
    }
    return (sxml_ir_function_target){.target_kind =
                                         SXML_IR_FUNCTION_TARGET_KIND_NONE};
}

static uint32_t native_properties(const sxml_function_entry *entry) {
    return entry->entry_kind == SXML_FUNCTION_ENTRY_KIND_NATIVE &&
                   entry->native_function != NULL
               ? entry->native_function->properties
               : 0U;
}

/* The parameter type of argument argument_index, item()* beyond the
   entry's list (which has one per argument). A constructor function's
   entry gives xs:anyAtomicType? (XPath 4.0, section 4.5.1.3). */
static sxml_sequence_type parameter_type_of(const sxml_function_entry *entry,
                                            size_t argument_index) {
    if (argument_index < entry->parameter_types.sequence_type_count) {
        return entry->parameter_types.sequence_types[argument_index];
    }
    return (sxml_sequence_type){
        .item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
        .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};
}

/* A function to find by its resolved name. */
typedef struct function_lookup {
    sxml_name_identifier function_name;
    size_t arity;
} function_lookup;

static sxml_status find_function(lowerer *state, function_lookup lookup,
                                 sxml_function_entry *entry) {
    if (lookup.function_name.index == 0) {
        *entry = (sxml_function_entry){
            .entry_kind = SXML_FUNCTION_ENTRY_KIND_NONE};
        return SXML_STATUS_OK;
    }
    return sxml_xpath_find_function(
        state->static_context->function_table,
        &(sxml_function_query){.function_name = lookup.function_name,
                               .arity = lookup.arity},
        entry);
}

/* The name identifier of a function name node, or 0 when the name was
   never interned (then no function has it). */
static sxml_status resolve_function_name(lowerer *state,
                                         sxml_ast_node_index name_node,
                                         sxml_name_identifier *name) {
    sxml_xpath_resolved_name resolved = {0};
    sxml_status status = sxml_xpath_resolve_name(
        &(sxml_xpath_name_request){.ast = state->ast,
                                   .name_node = name_node,
                                   .name_role = SXML_XPATH_NAME_ROLE_FUNCTION,
                                   .static_context = state->static_context,
                                   .error_record = state->error_record},
        &resolved);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_name_identifier found = {.index = 0};
    (void)sxml_name_pool_find_name(
        state->name_pool,
        &(sxml_name_request){.namespace_uri = resolved.namespace_uri,
                             .local_name = resolved.local_name},
        &found);
    *name = found;
    return SXML_STATUS_OK;
}

/* A well-known function lowering itself calls (fn:root, fn:partial-apply,
   fn:string-join), looked up by local name in the fn namespace. */
typedef struct builtin_function_request {
    sxml_ast_node_index node;
    sxml_byte_view local_name;
    size_t arity;
} builtin_function_request;

static sxml_status find_builtin_function(lowerer *state,
                                         builtin_function_request request,
                                         sxml_function_entry *entry) {
    sxml_name_identifier name = {.index = 0};
    (void)sxml_name_pool_find_name(
        state->name_pool,
        &(sxml_name_request){
            .namespace_uri =
                sxml_well_known_namespace_identifier(SXML_NAMESPACE_FUNCTIONS),
            .local_name = request.local_name},
        &name);
    sxml_status status = find_function(
        state, (function_lookup){.function_name = name, .arity = request.arity},
        entry);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (entry->entry_kind == SXML_FUNCTION_ENTRY_KIND_NONE) {
        return report_unsupported(
            state, request.node,
            SXML_BYTE_VIEW_FROM_LITERAL("the function table lacks a function this construct is lowered "
            "to (fn:root, fn:partial-apply or fn:string-join)"));
    }
    return SXML_STATUS_OK;
}

/* Emits the call of a prepared call without placeholders: coerces the
   arguments to the parameter types and makes the STATIC_CALL, or a CAST
   for a constructor function. */
typedef struct call_emission {
    lowering_block *block;
    const prepared_call *call;
    /* Coerce arguments (not for calls lowering makes of its own values). */
    bool coerces_arguments;
} call_emission;

static sxml_status emit_call(lowerer *state, call_emission request,
                             sxml_ir_expression_index *expression) {
    const prepared_call *call = request.call;
    if (call->entry.entry_kind == SXML_FUNCTION_ENTRY_KIND_CONSTRUCTOR) {
        /* XPath 4.0, section 4.5.1.3: a constructor function call is a
           cast that accepts the empty sequence. */
        return add_expression(
            state,
            (expression_to_add){
                .node = call->node,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_CAST,
                               .cast = {.value = call->arguments[0].atom,
                                        .target_type =
                                            call->entry.constructor_type,
                                        .allows_empty = true}}},
            expression);
    }
    void *memory = NULL;
    sxml_status status = allocate_scratch(
        state,
        (scratch_request){.element_count = call->argument_count,
                          .element_size = sizeof(sxml_ir_atom),
                          .element_alignment = alignof(sxml_ir_atom)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_atom *atoms = memory;
    for (size_t argument_index = 0;
         status == SXML_STATUS_OK && argument_index < call->argument_count;
         argument_index += 1) {
        atoms[argument_index] = call->arguments[argument_index].atom;
        if (request.coerces_arguments) {
            status = coerce_atom(
                state,
                (coercion_request){
                    .block = request.block,
                    .node = call->node,
                    .value = call->arguments[argument_index].atom,
                    .sequence_type =
                        parameter_type_of(&call->entry, argument_index),
                    .role = SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT},
                &atoms[argument_index]);
        }
    }
    sxml_ir_atom_list arguments = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_atoms(
            state->unit,
            (sxml_ir_atom_view){.atoms = atoms,
                                .atom_count = call->argument_count},
            &arguments);
    }
    release_scratch(state, memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_expression(
        state,
        (expression_to_add){
            .node = call->node,
            .expression = {
                .expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
                .static_call = {.target = target_of(&call->entry),
                                .arguments = arguments}}},
        expression);
}

/*
 * A partial application of a known function (XPath 4.0, section 4.5.2.5):
 * the supplied arguments are coerced now and captured by a closure whose
 * parameters are the placeholders, in order, and whose body calls the
 * function.
 */
static sxml_status emit_partial_application(lowerer *state,
                                            call_emission request,
                                            sxml_ir_expression_index *expression) {
    const prepared_call *call = request.call;
    const size_t supplied_count = call->argument_count - call->placeholder_count;
    void *types_memory = NULL;
    void *captures_memory = NULL;
    void *arguments_memory = NULL;
    sxml_status status = allocate_scratch(
        state,
        (scratch_request){.element_count = call->placeholder_count,
                          .element_size = sizeof(sxml_sequence_type),
                          .element_alignment = alignof(sxml_sequence_type)},
        &types_memory);
    if (status == SXML_STATUS_OK) {
        status = allocate_scratch(
            state,
            (scratch_request){.element_count = supplied_count,
                              .element_size = sizeof(sxml_ir_atom),
                              .element_alignment = alignof(sxml_ir_atom)},
            &captures_memory);
    }
    if (status == SXML_STATUS_OK) {
        status = allocate_scratch(
            state,
            (scratch_request){.element_count = call->argument_count,
                              .element_size = sizeof(sxml_ir_atom),
                              .element_alignment = alignof(sxml_ir_atom)},
            &arguments_memory);
    }
    sxml_sequence_type *placeholder_types = types_memory;
    sxml_ir_atom *captured_atoms = captures_memory;
    sxml_ir_atom *body_arguments = arguments_memory;
    size_t placeholder_index = 0;
    size_t supplied_index = 0;
    for (size_t argument_index = 0;
         status == SXML_STATUS_OK && argument_index < call->argument_count;
         argument_index += 1) {
        const sxml_sequence_type parameter_type =
            parameter_type_of(&call->entry, argument_index);
        if (call->arguments[argument_index].is_placeholder) {
            placeholder_types[placeholder_index] = parameter_type;
            placeholder_index += 1;
            body_arguments[argument_index] =
                slot_atom((sxml_ir_slot){.value = (uint32_t)placeholder_index});
            continue;
        }
        sxml_ir_atom supplied = {0};
        status = coerce_atom(
            state,
            (coercion_request){.block = request.block,
                               .node = call->node,
                               .value = call->arguments[argument_index].atom,
                               .sequence_type = parameter_type,
                               .role = SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT},
            &supplied);
        if (supplied.atom_kind == SXML_IR_ATOM_KIND_CONSTANT) {
            /* A constant needs no capture: the body passes it directly. */
            body_arguments[argument_index] = supplied;
            continue;
        }
        captured_atoms[supplied_index] = supplied;
        supplied_index += 1;
        body_arguments[argument_index] = slot_atom((sxml_ir_slot){
            .value = (uint32_t)(call->placeholder_count + supplied_index)});
    }
    sxml_type_identifier signature = {0};
    if (status == SXML_STATUS_OK) {
        status = register_signature(
            state,
            (signature_request){
                .parameter_types = {.sequence_types = placeholder_types,
                                    .sequence_type_count =
                                        call->placeholder_count},
                .result_type = call->entry.result_type},
            &signature);
    }
    sxml_ir_function_index function = {0};
    if (status == SXML_STATUS_OK) {
        status = add_closure_function(
            state,
            (closure_request){
                .node = call->node,
                .signature = signature,
                .parameter_count = (uint32_t)call->placeholder_count,
                .captured_atoms = {.atoms = captured_atoms,
                                   .atom_count = supplied_index}},
            &function);
    }
    sxml_ir_expression body = {0};
    if (status == SXML_STATUS_OK) {
        if (call->entry.entry_kind == SXML_FUNCTION_ENTRY_KIND_CONSTRUCTOR) {
            body = (sxml_ir_expression){
                .expression_kind = SXML_IR_EXPRESSION_KIND_CAST,
                .cast = {.value = body_arguments[0],
                         .target_type = call->entry.constructor_type,
                         .allows_empty = true}};
        } else {
            sxml_ir_atom_list arguments = {0};
            status = sxml_ir_unit_add_atoms(
                state->unit,
                (sxml_ir_atom_view){.atoms = body_arguments,
                                    .atom_count = call->argument_count},
                &arguments);
            body = (sxml_ir_expression){
                .expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
                .static_call = {.target = target_of(&call->entry),
                                .arguments = arguments}};
        }
    }
    if (status == SXML_STATUS_OK) {
        status = finish_closure(state,
                                (closure_body){.function = function,
                                               .body = body,
                                               .node = call->node},
                                expression);
    }
    release_scratch(state, arguments_memory);
    release_scratch(state, captures_memory);
    release_scratch(state, types_memory);
    return status;
}

static sxml_status emit_prepared_call(lowerer *state, call_emission request,
                                      sxml_ir_expression_index *expression) {
    if (request.call->placeholder_count > 0) {
        return emit_partial_application(state, request, expression);
    }
    return emit_call(state, request, expression);
}

/* The parameter index named by a keyword argument, or SIZE_MAX. */
static size_t find_parameter(const sxml_function_entry *entry,
                             sxml_byte_view parameter_name) {
    for (size_t parameter_index = 0;
         parameter_index < entry->parameters.function_parameter_count;
         parameter_index += 1) {
        if (sxml_byte_view_equals(
                entry->parameters.function_parameters[parameter_index]
                    .parameter_name,
                parameter_name)) {
            return parameter_index;
        }
    }
    return SIZE_MAX;
}

/* A named function reference (XPath 4.0, section 4.5.4). */
static sxml_status lower_named_function_reference(
    lowerer *state, node_in_block request, sxml_ir_expression_index *expression) {
    const sxml_ast_node_index node = request.node;
    sxml_name_identifier name = {0};
    sxml_status status = resolve_function_name(
        state, child_of(state, node, SXML_AST_FIELD_NAME), &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_byte_view arity_text =
        text_of(state, child_of(state, node, SXML_AST_FIELD_ARITY));
    uint64_t arity = 0;
    for (size_t digit_index = 0; digit_index < arity_text.byte_count;
         digit_index += 1) {
        arity = arity * 10U + (uint64_t)(arity_text.bytes[digit_index] - '0');
        if (arity > UINT32_MAX) {
            break;
        }
    }
    sxml_function_entry entry = {0};
    if (arity <= UINT32_MAX) {
        status = find_function(
            state,
            (function_lookup){.function_name = name, .arity = (size_t)arity},
            &entry);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (entry.entry_kind == SXML_FUNCTION_ENTRY_KIND_NONE) {
        return report_static_error(
            state, node,
            SXML_BYTE_VIEW_FROM_LITERAL("XPST0017 no function with this name and arity is known"));
    }
    const uint32_t properties = native_properties(&entry);
    const bool reads_position_or_size =
        (properties & (SXML_NATIVE_FUNCTION_READS_CONTEXT_POSITION |
                       SXML_NATIVE_FUNCTION_READS_CONTEXT_SIZE)) != 0;
    const bool omits_context_value =
        arity < entry.parameters.function_parameter_count &&
        entry.parameters.function_parameters[arity].parameter_default ==
            SXML_PARAMETER_DEFAULT_CONTEXT_VALUE;
    if (!reads_position_or_size && !omits_context_value) {
        return add_expression(
            state,
            (expression_to_add){
                .node = node,
                .expression = {
                    .expression_kind = SXML_IR_EXPRESSION_KIND_FUNCTION_REFERENCE,
                    .function_reference = {.target = target_of(&entry),
                                           .arity = (uint32_t)arity,
                                           .signature = entry.signature}}},
            expression);
    }
    /* A focus-dependent reference captures the focus when it is made
       (XPath 4.0, section 4.5.4): a closure of the same arity. */
    sxml_ir_atom captured = {0};
    sxml_function_entry call_entry = entry;
    if (reads_position_or_size) {
        status = read_focus(
            state,
            (focus_read){
                .block = request.block,
                .node = node,
                .part = (properties & SXML_NATIVE_FUNCTION_READS_CONTEXT_POSITION) != 0
                            ? SXML_LOWERING_FOCUS_PART_POSITION
                            : SXML_LOWERING_FOCUS_PART_SIZE},
            &captured);
    } else {
        /* The closure calls the next arity with its parameters and the
           captured context value. */
        status = find_function(
            state,
            (function_lookup){.function_name = name,
                              .arity = (size_t)arity + 1U},
            &call_entry);
        if (status == SXML_STATUS_OK &&
            call_entry.entry_kind == SXML_FUNCTION_ENTRY_KIND_NONE) {
            /* The table lists a context-value default it does not serve. */
            status = SXML_STATUS_INVALID_ARGUMENT;
        }
        sxml_ir_atom value = {0};
        if (status == SXML_STATUS_OK) {
            status = read_focus(state,
                                (focus_read){.block = request.block,
                                             .node = node,
                                             .part = SXML_LOWERING_FOCUS_PART_VALUE},
                                &value);
        }
        if (status == SXML_STATUS_OK) {
            status = coerce_atom(
                state,
                (coercion_request){
                    .block = request.block,
                    .node = node,
                    .value = value,
                    .sequence_type = parameter_type_of(&call_entry, (size_t)arity),
                    .role = SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT},
                &captured);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_function_index function = {0};
    status = add_closure_function(
        state,
        (closure_request){.node = node,
                          .signature = entry.signature,
                          .parameter_count = (uint32_t)arity,
                          .captured_atoms = {.atoms = &captured,
                                             .atom_count = 1}},
        &function);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (reads_position_or_size) {
        return finish_closure(
            state,
            (closure_body){
                .function = function,
                .body = {.expression_kind = SXML_IR_EXPRESSION_KIND_ATOM,
                         .atom = slot_atom((sxml_ir_slot){.value = 1})},
                .node = node},
            expression);
    }
    void *memory = NULL;
    status = allocate_scratch(
        state,
        (scratch_request){.element_count = (size_t)arity + 1U,
                          .element_size = sizeof(sxml_ir_atom),
                          .element_alignment = alignof(sxml_ir_atom)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_atom *atoms = memory;
    for (uint32_t parameter_number = 1; parameter_number <= arity + 1U;
         parameter_number += 1) {
        atoms[parameter_number - 1] =
            slot_atom((sxml_ir_slot){.value = parameter_number});
    }
    sxml_ir_atom_list arguments = {0};
    status = sxml_ir_unit_add_atoms(
        state->unit,
        (sxml_ir_atom_view){.atoms = atoms, .atom_count = (size_t)arity + 1U},
        &arguments);
    release_scratch(state, memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_closure(
        state,
        (closure_body){
            .function = function,
            .body = {.expression_kind = SXML_IR_EXPRESSION_KIND_STATIC_CALL,
                     .static_call = {.target = target_of(&call_entry),
                                     .arguments = arguments}},
            .node = node},
        expression);
}

/* --------------------------------------------------------------- Paths */

static bool is_reverse_axis(sxml_axis axis) {
    switch (axis) {
    case SXML_AXIS_PARENT:
    case SXML_AXIS_ANCESTOR:
    case SXML_AXIS_ANCESTOR_OR_SELF:
    case SXML_AXIS_PRECEDING_SIBLING:
    case SXML_AXIS_PRECEDING_SIBLING_OR_SELF:
    case SXML_AXIS_PRECEDING:
    case SXML_AXIS_PRECEDING_OR_SELF:
        return true;
    case SXML_AXIS_SELF:
    case SXML_AXIS_CHILD:
    case SXML_AXIS_DESCENDANT:
    case SXML_AXIS_DESCENDANT_OR_SELF:
    case SXML_AXIS_ATTRIBUTE:
    case SXML_AXIS_NAMESPACE:
    case SXML_AXIS_FOLLOWING_SIBLING:
    case SXML_AXIS_FOLLOWING_SIBLING_OR_SELF:
    case SXML_AXIS_FOLLOWING:
    case SXML_AXIS_FOLLOWING_OR_SELF:
        return false;
    }
    return false;
}

/* The sxml_axis of an axis written in a step. */
static bool axis_of(sxml_ast_axis written, sxml_axis *axis) {
    switch (written) {
    case SXML_AST_AXIS_ANCESTOR:
        *axis = SXML_AXIS_ANCESTOR;
        return true;
    case SXML_AST_AXIS_ANCESTOR_OR_SELF:
        *axis = SXML_AXIS_ANCESTOR_OR_SELF;
        return true;
    case SXML_AST_AXIS_ATTRIBUTE:
        *axis = SXML_AXIS_ATTRIBUTE;
        return true;
    case SXML_AST_AXIS_CHILD:
        *axis = SXML_AXIS_CHILD;
        return true;
    case SXML_AST_AXIS_DESCENDANT:
        *axis = SXML_AXIS_DESCENDANT;
        return true;
    case SXML_AST_AXIS_DESCENDANT_OR_SELF:
        *axis = SXML_AXIS_DESCENDANT_OR_SELF;
        return true;
    case SXML_AST_AXIS_FOLLOWING:
        *axis = SXML_AXIS_FOLLOWING;
        return true;
    case SXML_AST_AXIS_FOLLOWING_OR_SELF:
        *axis = SXML_AXIS_FOLLOWING_OR_SELF;
        return true;
    case SXML_AST_AXIS_FOLLOWING_SIBLING:
        *axis = SXML_AXIS_FOLLOWING_SIBLING;
        return true;
    case SXML_AST_AXIS_FOLLOWING_SIBLING_OR_SELF:
        *axis = SXML_AXIS_FOLLOWING_SIBLING_OR_SELF;
        return true;
    case SXML_AST_AXIS_NAMESPACE:
        *axis = SXML_AXIS_NAMESPACE;
        return true;
    case SXML_AST_AXIS_PARENT:
        *axis = SXML_AXIS_PARENT;
        return true;
    case SXML_AST_AXIS_PRECEDING:
        *axis = SXML_AXIS_PRECEDING;
        return true;
    case SXML_AST_AXIS_PRECEDING_OR_SELF:
        *axis = SXML_AXIS_PRECEDING_OR_SELF;
        return true;
    case SXML_AST_AXIS_PRECEDING_SIBLING:
        *axis = SXML_AXIS_PRECEDING_SIBLING;
        return true;
    case SXML_AST_AXIS_PRECEDING_SIBLING_OR_SELF:
        *axis = SXML_AXIS_PRECEDING_SIBLING_OR_SELF;
        return true;
    case SXML_AST_AXIS_SELF:
        *axis = SXML_AXIS_SELF;
        return true;
    case SXML_AST_AXIS_NONE:
    case SXML_AST_AXIS_ITEM:
        break;
    }
    return false;
}

/* The node test of a step as a registered item type (src/ir_internal.h,
   STEP): a name test or wildcard gets the principal node kind of the axis
   (XPath 4.0, section 4.6.5.1). */
typedef struct node_test_request {
    sxml_ast_node_index test;
    sxml_axis axis;
} node_test_request;

/* A node test that is not a union of node tests. */
static sxml_status resolve_simple_node_test(lowerer *state,
                                            node_test_request request,
                                            sxml_type_identifier *node_test) {
    const sxml_ast_node *test = node_of(state, request.test);
    switch ((uint32_t)test->kind) {
    case SXML_AST_KIND_NONE:
        *node_test = sxml_builtin_type_identifier(SXML_TYPE_GENERIC_NODE);
        return SXML_STATUS_OK;
    case SXML_AST_KIND_NCNAME:
    case SXML_AST_KIND_PREFIXED_NAME:
    case SXML_AST_KIND_URI_QUALIFIED_NAME:
    case SXML_AST_KIND_WILDCARD:
        break;
    case SXML_AST_KIND_DYNAMIC_NODE_TEST:
        return report_unsupported(state, request.test,
                                  SXML_BYTE_VIEW_FROM_LITERAL("dynamic node tests are not lowered in "
                                  "Phase 1"));
    default:
        return resolve_item_type(state, request.test, node_test);
    }
    sxml_node_kind principal_kind = SXML_NODE_KIND_ELEMENT;
    sxml_xpath_name_role name_role = SXML_XPATH_NAME_ROLE_ELEMENT_OR_TYPE;
    if (request.axis == SXML_AXIS_ATTRIBUTE) {
        principal_kind = SXML_NODE_KIND_ATTRIBUTE;
        name_role = SXML_XPATH_NAME_ROLE_NO_NAMESPACE;
    } else if (request.axis == SXML_AXIS_NAMESPACE) {
        principal_kind = SXML_NODE_KIND_NAMESPACE;
        name_role = SXML_XPATH_NAME_ROLE_NO_NAMESPACE;
    }
    /* A name test on the namespace axis names the prefix, a name in no
       namespace (D77); a prefixed name there matches nothing. */
    sxml_name_test name_test = {.name_test_kind = SXML_NAME_TEST_KIND_ANY_NAME};
    if (test->kind == SXML_AST_KIND_WILDCARD) {
        switch (test->detail.name_test_kind) {
        case SXML_NAME_TEST_KIND_ANY_NAME:
            break;
        case SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD:
            name_test = (sxml_name_test){
                .name_test_kind = SXML_NAME_TEST_KIND_LOCAL_NAME_WILDCARD,
                .local_name =
                    sxml_ast_get_text(state->ast, test->payload.name.local_name)};
            break;
        case SXML_NAME_TEST_KIND_NAMESPACE_WILDCARD:
        case SXML_NAME_TEST_KIND_EXPANDED_NAME: {
            sxml_xpath_resolved_name resolved = {0};
            sxml_status status = sxml_xpath_resolve_name(
                &(sxml_xpath_name_request){
                    .ast = state->ast,
                    .name_node = request.test,
                    .name_role = name_role,
                    .static_context = state->static_context,
                    .error_record = state->error_record},
                &resolved);
            if (status != SXML_STATUS_OK) {
                return status;
            }
            name_test = (sxml_name_test){
                .name_test_kind = SXML_NAME_TEST_KIND_NAMESPACE_WILDCARD,
                .namespace_uri = resolved.namespace_uri};
            break;
        }
        }
    } else {
        sxml_xpath_resolved_name resolved = {0};
        sxml_status status = sxml_xpath_resolve_name(
            &(sxml_xpath_name_request){.ast = state->ast,
                                       .name_node = request.test,
                                       .name_role = name_role,
                                       .static_context = state->static_context,
                                       .error_record = state->error_record},
            &resolved);
        sxml_name_identifier name = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_name_pool_intern_name(
                state->name_pool,
                &(sxml_name_request){.namespace_uri = resolved.namespace_uri,
                                     .local_name = resolved.local_name},
                &name);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        name_test = (sxml_name_test){
            .name_test_kind = SXML_NAME_TEST_KIND_EXPANDED_NAME,
            .expanded_name = name};
    }
    return sxml_type_registry_register_item_type(
        state->type_registry,
        &(sxml_type_description){
            .variety = SXML_TYPE_VARIETY_NODE_TEST,
            .variety_details = {.node_test = {.node_kind = principal_kind,
                                              .name_test = name_test}}},
        node_test);
}

/* A node test; a union of node tests (XPath 4.0, 4.6.5.1) is a choice of
   its members, which are simple node tests (UnionNodeTest). */
static sxml_status resolve_node_test(lowerer *state, node_test_request request,
                                     sxml_type_identifier *node_test) {
    if (node_of(state, request.test)->kind != SXML_AST_KIND_UNION_NODE_TEST) {
        return resolve_simple_node_test(state, request, node_test);
    }
    size_t member_count = 0;
    for (sxml_ast_node_index member =
             child_of(state, request.test, SXML_AST_FIELD_TEST);
         member.value != 0; member = next_of(state, member)) {
        member_count += 1;
    }
    void *memory = NULL;
    sxml_status status = allocate_scratch(
        state,
        (scratch_request){.element_count = member_count,
                          .element_size = sizeof(sxml_type_identifier),
                          .element_alignment = alignof(sxml_type_identifier)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_type_identifier *members = memory;
    size_t member_index = 0;
    for (sxml_ast_node_index member =
             child_of(state, request.test, SXML_AST_FIELD_TEST);
         status == SXML_STATUS_OK && member.value != 0;
         member = next_of(state, member)) {
        status = resolve_simple_node_test(
            state, (node_test_request){.test = member, .axis = request.axis},
            &members[member_index]);
        member_index += 1;
    }
    if (status == SXML_STATUS_OK) {
        status = member_count == 1
                     ? SXML_STATUS_OK
                     : sxml_type_registry_register_item_type(
                           state->type_registry,
                           &(sxml_type_description){
                               .variety = SXML_TYPE_VARIETY_CHOICE,
                               .variety_details = {
                                   .member_types = {
                                       .type_identifiers = members,
                                       .type_identifier_count = member_count}}},
                           node_test);
        if (member_count == 1) {
            *node_test = members[0];
        }
    }
    release_scratch(state, memory);
    return status;
}

/* A STEP from origin, bound in block. */
typedef struct step_request {
    lowering_block *block;
    sxml_ast_node_index node;
    sxml_ir_atom origin;
    sxml_axis axis;
    sxml_type_identifier node_test;
} step_request;

static sxml_status add_step(lowerer *state, step_request request,
                            sxml_ir_expression_index *expression) {
    return add_expression(
        state,
        (expression_to_add){
            .node = request.node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_STEP,
                           .step = {.origin = request.origin,
                                    .axis = request.axis,
                                    .node_test = request.node_test}}},
        expression);
}

static sxml_status add_document_order(lowerer *state, atom_at request,
                                      sxml_ir_expression_index *expression) {
    return add_expression(
        state,
        (expression_to_add){
            .node = request.node,
            .expression = {
                .expression_kind = SXML_IR_EXPRESSION_KIND_DOCUMENT_ORDER,
                .document_order = {.input = request.atom}}},
        expression);
}

/* "E1//" : E1/descendant-or-self::node(), a MAP of one step (XPath 4.0,
   section 4.6.3), in document order and bound. */
typedef struct descendant_request {
    lowering_block *block;
    sxml_ast_node_index node;
    sxml_ir_atom input;
} descendant_request;

static sxml_status add_descendants(lowerer *state, descendant_request request,
                                   sxml_ir_atom *atom) {
    sxml_ir_slot item = {0};
    sxml_status status = new_slot(state, &item);
    sxml_ir_expression_index step = {0};
    if (status == SXML_STATUS_OK) {
        status = add_step(
            state,
            (step_request){
                .block = request.block,
                .node = request.node,
                .origin = slot_atom(item),
                .axis = SXML_AXIS_DESCENDANT_OR_SELF,
                .node_test = sxml_builtin_type_identifier(SXML_TYPE_GENERIC_NODE)},
            &step);
    }
    sxml_ir_expression_index map = {0};
    if (status == SXML_STATUS_OK) {
        status = add_expression(
            state,
            (expression_to_add){
                .node = request.node,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                               .map = {.input = request.input,
                                       .focus = {.context_value = item},
                                       .body = step,
                                       .flags = SXML_IR_MAP_REQUIRES_NODES}}},
            &map);
    }
    sxml_ir_atom mapped = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_expression(
            state, (expression_in_block){.block = request.block, .expression = map},
            &mapped);
    }
    sxml_ir_expression_index ordered = {0};
    if (status == SXML_STATUS_OK) {
        status = add_document_order(
            state, (atom_at){.node = request.node, .atom = mapped}, &ordered);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind_expression(
        state, (expression_in_block){.block = request.block, .expression = ordered},
        atom);
}

/* ------------------------------------------------------------- Chains */

/* The field of a chain node's left operand, NONE for other kinds. */
static sxml_ast_field chain_field(sxml_ast_kind kind) {
    switch ((uint32_t)kind) {
    case SXML_AST_KIND_OR_EXPRESSION:
    case SXML_AST_KIND_AND_EXPRESSION:
    case SXML_AST_KIND_COMPARISON_EXPRESSION:
    case SXML_AST_KIND_OTHERWISE_EXPRESSION:
    case SXML_AST_KIND_STRING_CONCATENATION_EXPRESSION:
    case SXML_AST_KIND_RANGE_EXPRESSION:
    case SXML_AST_KIND_ADDITIVE_EXPRESSION:
    case SXML_AST_KIND_MULTIPLICATIVE_EXPRESSION:
    case SXML_AST_KIND_UNION_EXPRESSION:
    case SXML_AST_KIND_INTERSECT_EXCEPT_EXPRESSION:
    case SXML_AST_KIND_PIPELINE_EXPRESSION:
    case SXML_AST_KIND_SIMPLE_MAP_EXPRESSION:
    case SXML_AST_KIND_RELATIVE_PATH_EXPRESSION:
        return SXML_AST_FIELD_LEFT;
    case SXML_AST_KIND_FILTER_EXPRESSION:
    case SXML_AST_KIND_LOOKUP_EXPRESSION:
    case SXML_AST_KIND_ARROW_EXPRESSION:
        return SXML_AST_FIELD_VALUE;
    case SXML_AST_KIND_DYNAMIC_FUNCTION_CALL:
        return SXML_AST_FIELD_FUNCTION;
    default:
        return SXML_AST_FIELD_NONE;
    }
}

static bool binary_operator_of(sxml_ast_operator written,
                               sxml_ir_binary_operator *operator) {
    switch (written) {
    case SXML_AST_OPERATOR_VALUE_EQUAL:
        *operator = SXML_IR_BINARY_OPERATOR_VALUE_EQUAL;
        return true;
    case SXML_AST_OPERATOR_VALUE_NOT_EQUAL:
        *operator = SXML_IR_BINARY_OPERATOR_VALUE_NOT_EQUAL;
        return true;
    case SXML_AST_OPERATOR_VALUE_LESS:
        *operator = SXML_IR_BINARY_OPERATOR_VALUE_LESS;
        return true;
    case SXML_AST_OPERATOR_VALUE_LESS_OR_EQUAL:
        *operator = SXML_IR_BINARY_OPERATOR_VALUE_LESS_OR_EQUAL;
        return true;
    case SXML_AST_OPERATOR_VALUE_GREATER:
        *operator = SXML_IR_BINARY_OPERATOR_VALUE_GREATER;
        return true;
    case SXML_AST_OPERATOR_VALUE_GREATER_OR_EQUAL:
        *operator = SXML_IR_BINARY_OPERATOR_VALUE_GREATER_OR_EQUAL;
        return true;
    case SXML_AST_OPERATOR_GENERAL_EQUAL:
        *operator = SXML_IR_BINARY_OPERATOR_GENERAL_EQUAL;
        return true;
    case SXML_AST_OPERATOR_GENERAL_NOT_EQUAL:
        *operator = SXML_IR_BINARY_OPERATOR_GENERAL_NOT_EQUAL;
        return true;
    case SXML_AST_OPERATOR_GENERAL_LESS:
        *operator = SXML_IR_BINARY_OPERATOR_GENERAL_LESS;
        return true;
    case SXML_AST_OPERATOR_GENERAL_LESS_OR_EQUAL:
        *operator = SXML_IR_BINARY_OPERATOR_GENERAL_LESS_OR_EQUAL;
        return true;
    case SXML_AST_OPERATOR_GENERAL_GREATER:
        *operator = SXML_IR_BINARY_OPERATOR_GENERAL_GREATER;
        return true;
    case SXML_AST_OPERATOR_GENERAL_GREATER_OR_EQUAL:
        *operator = SXML_IR_BINARY_OPERATOR_GENERAL_GREATER_OR_EQUAL;
        return true;
    case SXML_AST_OPERATOR_IS:
        *operator = SXML_IR_BINARY_OPERATOR_IS;
        return true;
    case SXML_AST_OPERATOR_IS_NOT:
        *operator = SXML_IR_BINARY_OPERATOR_IS_NOT;
        return true;
    case SXML_AST_OPERATOR_PRECEDES:
        *operator = SXML_IR_BINARY_OPERATOR_PRECEDES;
        return true;
    case SXML_AST_OPERATOR_FOLLOWS:
        *operator = SXML_IR_BINARY_OPERATOR_FOLLOWS;
        return true;
    case SXML_AST_OPERATOR_PRECEDES_OR_IS:
        *operator = SXML_IR_BINARY_OPERATOR_PRECEDES_OR_IS;
        return true;
    case SXML_AST_OPERATOR_FOLLOWS_OR_IS:
        *operator = SXML_IR_BINARY_OPERATOR_FOLLOWS_OR_IS;
        return true;
    case SXML_AST_OPERATOR_CONCATENATE:
        *operator = SXML_IR_BINARY_OPERATOR_CONCATENATE;
        return true;
    case SXML_AST_OPERATOR_TO:
        *operator = SXML_IR_BINARY_OPERATOR_RANGE;
        return true;
    case SXML_AST_OPERATOR_PLUS:
        *operator = SXML_IR_BINARY_OPERATOR_ADD;
        return true;
    case SXML_AST_OPERATOR_MINUS:
        *operator = SXML_IR_BINARY_OPERATOR_SUBTRACT;
        return true;
    case SXML_AST_OPERATOR_MULTIPLY:
        *operator = SXML_IR_BINARY_OPERATOR_MULTIPLY;
        return true;
    case SXML_AST_OPERATOR_DIVIDE:
        *operator = SXML_IR_BINARY_OPERATOR_DIVIDE;
        return true;
    case SXML_AST_OPERATOR_INTEGER_DIVIDE:
        *operator = SXML_IR_BINARY_OPERATOR_INTEGER_DIVIDE;
        return true;
    case SXML_AST_OPERATOR_MODULO:
        *operator = SXML_IR_BINARY_OPERATOR_MODULO;
        return true;
    case SXML_AST_OPERATOR_UNION:
        *operator = SXML_IR_BINARY_OPERATOR_UNION;
        return true;
    case SXML_AST_OPERATOR_INTERSECT:
        *operator = SXML_IR_BINARY_OPERATOR_INTERSECT;
        return true;
    case SXML_AST_OPERATOR_EXCEPT:
        *operator = SXML_IR_BINARY_OPERATOR_EXCEPT;
        return true;
    case SXML_AST_OPERATOR_NONE:
    case SXML_AST_OPERATOR_OR:
    case SXML_AST_OPERATOR_AND:
    case SXML_AST_OPERATOR_OTHERWISE:
    case SXML_AST_OPERATOR_SIMPLE_MAP:
    case SXML_AST_OPERATOR_SLASH:
    case SXML_AST_OPERATOR_DOUBLE_SLASH:
    case SXML_AST_OPERATOR_PIPELINE:
    case SXML_AST_OPERATOR_SEQUENCE_ARROW:
    case SXML_AST_OPERATOR_MAPPING_ARROW:
    case SXML_AST_OPERATOR_SOME:
    case SXML_AST_OPERATOR_EVERY:
        break;
    }
    return false;
}

/* A CONDITIONAL on condition with two branches already lowered. */
typedef struct conditional_request {
    sxml_ast_node_index node;
    sxml_ir_atom condition;
    sxml_ir_expression_index then_branch;
    sxml_ir_expression_index else_branch;
} conditional_request;

static sxml_status add_conditional(lowerer *state, conditional_request request,
                                   sxml_ir_expression_index *expression) {
    return add_expression(
        state,
        (expression_to_add){
            .node = request.node,
            .expression = {
                .expression_kind = SXML_IR_EXPRESSION_KIND_CONDITIONAL,
                .conditional = {.condition = request.condition,
                                .then_branch = request.then_branch,
                                .else_branch = request.else_branch}}},
        expression);
}

/* A shared constant as an ATOM expression at node. */
typedef struct shared_constant_at {
    sxml_ast_node_index node;
    shared_constant which;
} shared_constant_at;

static sxml_status add_shared_constant_expression(
    lowerer *state, shared_constant_at request,
    sxml_ir_expression_index *expression) {
    const sxml_ast_node_index node = request.node;
    const shared_constant which = request.which;
    sxml_ir_atom atom = {0};
    sxml_status status = get_shared_constant(state, which, &atom);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_atom_expression(state, (atom_at){.node = node, .atom = atom},
                               expression);
}

/* Lowers the bindings of a FLWOR or quantified expression from binding
   (whose clause is clause, for FLWOR) on, then result. */
typedef struct binding_cursor {
    sxml_ast_node_index clause;
    sxml_ast_node_index binding;
} binding_cursor;

/* The binding after cursor, moving to the next clause of a FLWOR. */
static binding_cursor next_binding(const lowerer *state, binding_cursor cursor) {
    sxml_ast_node_index binding = next_of(state, cursor.binding);
    sxml_ast_node_index clause = cursor.clause;
    while (binding.value == 0 && clause.value != 0) {
        clause = next_of(state, clause);
        binding = child_of(state, clause, SXML_AST_FIELD_BINDING);
    }
    return (binding_cursor){.clause = clause, .binding = binding};
}

/* A variable bound to slot, coerced in block when declared with a type,
   then pushed on the scope. */
typedef struct typed_variable {
    lowering_block *block;
    sxml_ast_node_index variable;
    sxml_ir_atom value;
} typed_variable;

static sxml_status push_typed_variable(lowerer *state, typed_variable request) {
    sxml_name_identifier name = {0};
    sxml_status status = resolve_variable_name(
        state, child_of(state, request.variable, SXML_AST_FIELD_NAME), &name);
    sxml_sequence_type declared_type = {0};
    if (status == SXML_STATUS_OK) {
        status = resolve_declared_type(
            state, child_of(state, request.variable, SXML_AST_FIELD_TYPE),
            &declared_type);
    }
    sxml_ir_atom value = request.value;
    if (status == SXML_STATUS_OK) {
        status = coerce_atom(state,
                             (coercion_request){
                                 .block = request.block,
                                 .node = request.variable,
                                 .value = request.value,
                                 .sequence_type = declared_type,
                                 .role = SXML_IR_COERCION_ROLE_VARIABLE},
                             &value);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return push_variable(
        state, (scope_entry){.variable_name = name,
                             .function_level = state->function->function_level,
                             .atom = value});
}

/* A cast target type name (XPath 4.0, section 4.17.1): an atomic, list or
   union type; XPST0080 for the abstract ones, XPST0051 for others. */
static sxml_status resolve_cast_target(lowerer *state,
                                       sxml_ast_node_index target,
                                       sxml_type_identifier *target_type) {
    const sxml_ast_node_index type_node =
        child_of(state, target, SXML_AST_FIELD_TYPE);
    const sxml_ast_kind kind = node_of(state, type_node)->kind;
    if (kind != SXML_AST_KIND_NCNAME && kind != SXML_AST_KIND_PREFIXED_NAME &&
        kind != SXML_AST_KIND_URI_QUALIFIED_NAME) {
        return resolve_item_type(state, type_node, target_type);
    }
    sxml_xpath_resolved_name resolved = {0};
    sxml_status status = sxml_xpath_resolve_name(
        &(sxml_xpath_name_request){
            .ast = state->ast,
            .name_node = type_node,
            .name_role = SXML_XPATH_NAME_ROLE_ELEMENT_OR_TYPE,
            .static_context = state->static_context,
            .error_record = state->error_record},
        &resolved);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_name_identifier name = {0};
    sxml_type_identifier type = {0};
    sxml_type_description description = {0};
    if (!sxml_name_pool_find_name(
            state->name_pool,
            &(sxml_name_request){.namespace_uri = resolved.namespace_uri,
                                 .local_name = resolved.local_name},
            &name) ||
        !sxml_type_registry_find_type(state->type_registry, name, &type) ||
        sxml_type_registry_describe_type(state->type_registry, type,
                                         &description) != SXML_STATUS_OK) {
        return report_static_error(state, type_node,
                                   SXML_BYTE_VIEW_FROM_LITERAL("XPST0051 the cast target type is not "
                                   "defined"));
    }
    if (type.index == SXML_TYPE_XS_NOTATION ||
        type.index == SXML_TYPE_XS_ANY_ATOMIC_TYPE ||
        type.index == SXML_TYPE_XS_ANY_SIMPLE_TYPE) {
        return report_static_error(state, type_node,
                                   SXML_BYTE_VIEW_FROM_LITERAL("XPST0080 the cast target type is abstract"));
    }
    switch (description.variety) {
    case SXML_TYPE_VARIETY_ATOMIC:
    case SXML_TYPE_VARIETY_LIST:
    case SXML_TYPE_VARIETY_UNION:
    case SXML_TYPE_VARIETY_ENUMERATION:
    case SXML_TYPE_VARIETY_CHOICE:
        *target_type = type;
        return SXML_STATUS_OK;
    case SXML_TYPE_VARIETY_COMPLEX:
    case SXML_TYPE_VARIETY_ANY_SIMPLE:
    case SXML_TYPE_VARIETY_ANY_ITEM:
    case SXML_TYPE_VARIETY_NODE_TEST:
    case SXML_TYPE_VARIETY_JSON_NODE_TEST:
    case SXML_TYPE_VARIETY_FUNCTION:
    case SXML_TYPE_VARIETY_MAP:
    case SXML_TYPE_VARIETY_ARRAY:
    case SXML_TYPE_VARIETY_RECORD:
        break;
    }
    return report_static_error(state, type_node,
                               SXML_BYTE_VIEW_FROM_LITERAL("XPST0051 the cast target is not a simple type"));
}

/* A literal or variable reference as an ATOM expression. */
static sxml_status lower_atom_expression(
    lowerer *state, node_in_block request, sxml_ir_expression_index *expression) {
    sxml_ir_atom atom = {0};
    const sxml_status status = lower_atom_node(state, request.node, &atom);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_atom_expression(state, (atom_at){.node = request.node, .atom = atom},
                               expression);
}

/* "." (XPath 4.0, section 4.3.4). */
static sxml_status lower_context_value(
    lowerer *state, node_in_block request, sxml_ir_expression_index *expression) {
    if (state->function->focus->kind == SXML_LOWERING_FOCUS_KIND_ABSENT) {
        return add_absent_focus_raise(state, request.node, expression);
    }
    sxml_ir_atom atom = {0};
    const sxml_status status = read_focus(state,
                                          (focus_read){.block = request.block,
                                                       .node = request.node,
                                                       .part = SXML_LOWERING_FOCUS_PART_VALUE},
                                          &atom);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_atom_expression(state, (atom_at){.node = request.node, .atom = atom},
                               expression);
}

/* -------------------------------------------------------- The machine */

/*
 * Lowering without C recursion (D71). Each construct that lowers parts of
 * itself (operands, branches, bodies) is a procedure run by a frame: a
 * resumable step function and the locals it keeps between steps. A step
 * either finishes the frame, delivering its result, or asks for a part
 * with call_expression, call_atom, call_body or call_focused_body, which
 * lowers the part at once when it needs no frame (a literal, a variable,
 * "."), or pushes a frame for it, and returns; the machine runs the frame
 * on top until it finishes, then the one below it again. So the nesting
 * of an expression costs frames in allocator memory, not C stack, and
 * maximum_expression_depth bounds them.
 *
 * Frames are allocated one by one and never move: a part's result is
 * written through a pointer into the frame (or scratch array) that asked
 * for it, and blocks and foci of a frame are pointed to by the frames
 * above it.
 */

typedef enum lowering_procedure {
    /* Left-recursive chains (see the file comment) and their links. */
    SXML_LOWERING_PROCEDURE_CHAIN,
    SXML_LOWERING_PROCEDURE_BOOLEAN_LINK,
    SXML_LOWERING_PROCEDURE_OTHERWISE_LINK,
    SXML_LOWERING_PROCEDURE_BINARY_LINK,
    SXML_LOWERING_PROCEDURE_PIPELINE_LINK,
    SXML_LOWERING_PROCEDURE_SIMPLE_MAP_LINK,
    SXML_LOWERING_PROCEDURE_ARROW_MAP,
    /* Paths. */
    SXML_LOWERING_PROCEDURE_PATH_MAP,
    SXML_LOWERING_PROCEDURE_FILTER,
    SXML_LOWERING_PROCEDURE_AXIS_STEP,
    SXML_LOWERING_PROCEDURE_ABSOLUTE_PATH,
    SXML_LOWERING_PROCEDURE_LOOKUP,
    SXML_LOWERING_PROCEDURE_FOCUSED_BODY,
    /* Calls and functions. */
    SXML_LOWERING_PROCEDURE_FUNCTION_CALL,
    SXML_LOWERING_PROCEDURE_DYNAMIC_CALL,
    SXML_LOWERING_PROCEDURE_INLINE_FUNCTION,
    /* Other expressions. */
    SXML_LOWERING_PROCEDURE_BINDING,
    SXML_LOWERING_PROCEDURE_ATOM_LIST,
    SXML_LOWERING_PROCEDURE_MAP_CONSTRUCTOR,
    SXML_LOWERING_PROCEDURE_STRING_TEMPLATE,
    SXML_LOWERING_PROCEDURE_TYPED_EXPRESSION,
    SXML_LOWERING_PROCEDURE_IF,
    SXML_LOWERING_PROCEDURE_UNARY,
} lowering_procedure;

/* Where a frame's result goes: an expression index, or the result bound
   in atom_block and its atom, or (FOCUSED_BODY) a body and its focus. */
typedef struct lowering_destination {
    sxml_ir_expression_index *expression;
    sxml_ir_atom *atom;
    lowering_block *atom_block;
    focused_result *focused;
} lowering_destination;

/* Scratch arrays a frame owns, released when it is popped. */
#define FRAME_OWNED_MEMORY_COUNT 4U

typedef struct lowering_frame lowering_frame;

struct lowering_frame {
    /* The frame that pushed this one; NULL for the first. */
    lowering_frame *below;
    lowering_procedure procedure;
    /* Which step comes next; 0 for the first. */
    uint32_t step;
    /* Levels of maximum_expression_depth this frame holds. */
    uint32_t depth_units;
    bool is_finished;
    sxml_ast_node_index node;
    /* The block the procedure's LETs go to: the caller's, or own_block
       when the frame lowers a body. */
    lowering_block *block;
    lowering_block own_block;
    bool is_body;
    lowering_destination destination;
    void *owned_memory[FRAME_OWNED_MEMORY_COUNT];
    /* The procedure's locals, right after the frame in its allocation. */
    void *locals;
};

typedef struct lowering_machine {
    lowerer *state;
    lowering_frame *top;
} lowering_machine;

/* A frame to push. */
typedef struct frame_request {
    lowering_procedure procedure;
    sxml_ast_node_index node;
    lowering_block *block;
    bool is_body;
    lowering_destination destination;
    uint32_t depth_units;
    /* sizeof the procedure's locals, which start zeroed. */
    size_t locals_size;
} frame_request;

/* The locals start at this offset in a frame's allocation. */
#define FRAME_LOCALS_OFFSET                                                  \
    ((sizeof(lowering_frame) + alignof(max_align_t) - 1U) /                  \
     alignof(max_align_t) * alignof(max_align_t))

static sxml_status push_frame(lowering_machine *machine,
                              const frame_request *request,
                              lowering_frame **frame) {
    lowerer *state = machine->state;
    void *memory = NULL;
    const size_t byte_count = FRAME_LOCALS_OFFSET + request->locals_size;
    sxml_status status = allocate_scratch(
        state,
        (scratch_request){.element_count = byte_count,
                          .element_size = 1,
                          .element_alignment = alignof(max_align_t)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    memset(memory, 0, byte_count);
    lowering_frame *new_frame = memory;
    new_frame->below = machine->top;
    new_frame->procedure = request->procedure;
    new_frame->depth_units = request->depth_units;
    new_frame->node = request->node;
    new_frame->is_body = request->is_body;
    new_frame->block = request->is_body ? &new_frame->own_block : request->block;
    new_frame->destination = request->destination;
    new_frame->locals = (uint8_t *)memory + FRAME_LOCALS_OFFSET;
    state->depth += request->depth_units;
    machine->top = new_frame;
    *frame = new_frame;
    return SXML_STATUS_OK;
}

static void pop_frame(lowering_machine *machine) {
    lowerer *state = machine->state;
    lowering_frame *frame = machine->top;
    machine->top = frame->below;
    state->depth -= frame->depth_units;
    for (size_t memory_index = 0; memory_index < FRAME_OWNED_MEMORY_COUNT;
         memory_index += 1) {
        release_scratch(state, frame->owned_memory[memory_index]);
    }
    release_scratch(state, frame);
}

/* Allocates a scratch array the frame owns, as slot memory_index. */
typedef struct owned_request {
    size_t memory_index;
    scratch_request scratch;
} owned_request;

static sxml_status allocate_owned(lowering_machine *machine,
                                  lowering_frame *frame, owned_request request) {
    return allocate_scratch(machine->state, request.scratch,
                            &frame->owned_memory[request.memory_index]);
}

/* Delivers result (an expression lowered for destination) as the
   destination asks: through the body's block first when the expression
   was lowered as a body, then bound in atom_block for an atom. */
typedef struct delivery {
    lowering_destination destination;
    /* The block result's LETs are in when it is a body; else NULL. */
    const lowering_block *body_block;
    sxml_ir_expression_index result;
} delivery;

static sxml_status deliver(lowerer *state, delivery request) {
    sxml_ir_expression_index result = request.result;
    if (request.body_block != NULL) {
        sxml_status status = finish_block(
            state, (block_result){.block = *request.body_block, .result = result},
            &result);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    if (request.destination.atom != NULL) {
        return bind_expression(
            state,
            (expression_in_block){.block = request.destination.atom_block,
                                  .expression = result},
            request.destination.atom);
    }
    *request.destination.expression = result;
    return SXML_STATUS_OK;
}

/* Finishes frame with result. */
static sxml_status finish_frame(lowering_machine *machine, lowering_frame *frame,
                                sxml_ir_expression_index result) {
    frame->is_finished = true;
    return deliver(machine->state,
                   (delivery){.destination = frame->destination,
                              .body_block = frame->is_body ? &frame->own_block : NULL,
                              .result = result});
}

/* A part to lower: node, for destination, in block or as a body. */
typedef struct expression_call {
    sxml_ast_node_index node;
    lowering_block *block;
    bool is_body;
    lowering_destination destination;
} expression_call;

static sxml_status report_depth_exceeded(const lowerer *state,
                                         sxml_ast_node_index node) {
    if (state->error_record != NULL) {
        state->error_record->diagnostic = (sxml_diagnostic){
            .description = SXML_BYTE_VIEW_FROM_LITERAL(
                "the expression is nested deeper than "
                "maximum_expression_depth"),
            .location = sxml_ast_get_source_location(state->ast, node)};
    }
    return SXML_STATUS_LIMIT_EXCEEDED;
}

/* The procedure lowering an expression of kind; false for kinds lowered
   at once (leaf_expression). */
static bool procedure_of(sxml_ast_kind kind, lowering_procedure *procedure,
                         size_t *locals_size);

/* Lowers an expression that needs no frame into block. */
static sxml_status lower_leaf_expression(lowerer *state, node_in_block request,
                                         sxml_ir_expression_index *expression);


/* Lowers call.node for call.destination: at once when it needs no frame,
   else by pushing its frame. Parentheses and the XPATH root hold their
   inner expression's levels of maximum_expression_depth but no frame of
   their own. */
static sxml_status call_expression(lowering_machine *machine,
                                   expression_call call) {
    lowerer *state = machine->state;
    sxml_ast_node_index node = call.node;
    uint32_t depth_units = 0;
    for (;;) {
        if (state->depth + depth_units >= state->maximum_depth) {
            return report_depth_exceeded(state, node);
        }
        depth_units += 1;
        const sxml_ast_kind kind = node_of(state, node)->kind;
        sxml_ast_node_index inner = {.value = 0};
        if (kind == SXML_AST_KIND_XPATH) {
            inner = child_of(state, node, SXML_AST_FIELD_BODY);
            if (inner.value == 0) {
                /* Reported as unsupported at the missing node, as any
                   node of kind NONE. */
                node = inner;
                continue;
            }
        } else if (kind == SXML_AST_KIND_PARENTHESIZED_EXPRESSION ||
                   kind == SXML_AST_KIND_ENCLOSED_EXPRESSION) {
            inner = child_of(state, node, SXML_AST_FIELD_EXPRESSION);
        }
        if (inner.value == 0) {
            break;
        }
        node = inner;
    }
    lowering_procedure procedure = SXML_LOWERING_PROCEDURE_CHAIN;
    size_t locals_size = 0;
    if (!procedure_of(node_of(state, node)->kind, &procedure, &locals_size)) {
        lowering_block leaf_block = {{0}, {0}};
        sxml_ir_expression_index result = {0};
        sxml_status status = lower_leaf_expression(
            state,
            (node_in_block){.block = call.is_body ? &leaf_block : call.block,
                            .node = node},
            &result);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return deliver(state,
                       (delivery){.destination = call.destination,
                                  .body_block = call.is_body ? &leaf_block : NULL,
                                  .result = result});
    }
    lowering_frame *frame = NULL;
    return push_frame(machine,
                      &(frame_request){.procedure = procedure,
                                       .node = node,
                                       .block = call.block,
                                       .is_body = call.is_body,
                                       .destination = call.destination,
                                       .depth_units = depth_units,
                                       .locals_size = locals_size},
                      &frame);
}

/* An operand to lower to an atom in block (lower_to_atom): literals,
   variables and "." at once, other expressions bound in block. */
typedef struct atom_call {
    sxml_ast_node_index node;
    lowering_block *block;
    sxml_ir_atom *atom;
} atom_call;

static sxml_status call_atom(lowering_machine *machine, atom_call call) {
    lowerer *state = machine->state;
    const sxml_ast_kind kind = node_of(state, call.node)->kind;
    if (is_atom_kind(kind)) {
        return lower_atom_node(state, call.node, call.atom);
    }
    if (kind == SXML_AST_KIND_CONTEXT_VALUE_REFERENCE) {
        return read_focus(state,
                          (focus_read){.block = call.block,
                                       .node = call.node,
                                       .part = SXML_LOWERING_FOCUS_PART_VALUE},
                          call.atom);
    }
    return call_expression(
        machine,
        (expression_call){.node = call.node,
                          .block = call.block,
                          .is_body = false,
                          .destination = {.atom = call.atom,
                                          .atom_block = call.block}});
}

/* Lowers node as a block of its own (a branch, a body). */
static sxml_status call_body(lowering_machine *machine,
                             sxml_ast_node_index node,
                             sxml_ir_expression_index *expression) {
    return call_expression(
        machine, (expression_call){.node = node,
                                   .block = NULL,
                                   .is_body = true,
                                   .destination = {.expression = expression}});
}

/* Pushes a frame for a part of a construct (no level of
   maximum_expression_depth of its own) and writes it to *frame. */
typedef struct part_frame_request {
    lowering_procedure procedure;
    sxml_ast_node_index node;
    lowering_block *block;
    lowering_destination destination;
    size_t locals_size;
    /* The step it starts at. */
    uint32_t step;
} part_frame_request;

static sxml_status push_part_frame(lowering_machine *machine,
                                   part_frame_request request,
                                   lowering_frame **frame) {
    sxml_status status = push_frame(
        machine,
        &(frame_request){.procedure = request.procedure,
                         .node = request.node,
                         .block = request.block,
                         .is_body = false,
                         .destination = request.destination,
                         .depth_units = 0,
                         .locals_size = request.locals_size},
        frame);
    if (status == SXML_STATUS_OK) {
        (*frame)->step = request.step;
    }
    return status;
}

/* ------------------------------------------------------- Focused body */

/* A body lowered with a new focus of kind SLOTS (lower_focused_body). */
typedef struct focused_body_locals {
    sxml_ir_focus preset_slots;
    lowering_focus focus;
    lowering_focus *outer_focus;
    sxml_ir_expression_index body;
} focused_body_locals;

/* Lowers node as a body with a new focus of kind SLOTS and writes the
   body and the slots it bound to *result. */
typedef struct focused_call {
    sxml_ast_node_index node;
    /* Slots already chosen; others are made on first read. */
    sxml_ir_focus preset_slots;
    focused_result *result;
} focused_call;

static sxml_status call_focused_body(lowering_machine *machine,
                                     focused_call call) {
    lowering_frame *frame = NULL;
    sxml_status status = push_part_frame(
        machine,
        (part_frame_request){.procedure = SXML_LOWERING_PROCEDURE_FOCUSED_BODY,
                             .node = call.node,
                             .block = NULL,
                             .destination = {.focused = call.result},
                             .locals_size = sizeof(focused_body_locals),
                             .step = 0},
        &frame);
    if (status == SXML_STATUS_OK) {
        focused_body_locals *locals = frame->locals;
        locals->preset_slots = call.preset_slots;
    }
    return status;
}

static sxml_status continue_focused_body(lowering_machine *machine,
                                         lowering_frame *frame) {
    lowerer *state = machine->state;
    focused_body_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        locals->focus = (lowering_focus){.kind = SXML_LOWERING_FOCUS_KIND_SLOTS,
                                         .slots = locals->preset_slots};
        locals->outer_focus = state->function->focus;
        state->function->focus = &locals->focus;
        return call_body(machine, frame->node, &locals->body);
    }
    state->function->focus = locals->outer_focus;
    frame->is_finished = true;
    *frame->destination.focused =
        (focused_result){.body = locals->body, .focus = locals->focus.slots};
    return SXML_STATUS_OK;
}


/* --------------------------------------------------------------- Paths */

/* FILTER of input by the expression of predicate (XPath 4.0, 4.4.1);
   the frame's node is where the FILTER is. */
typedef struct filter_locals {
    sxml_ir_atom input;
    sxml_ast_node_index predicate;
    focused_result predicate_body;
} filter_locals;

typedef struct filter_request {
    sxml_ast_node_index node;
    sxml_ir_atom input;
    sxml_ast_node_index predicate;
} filter_request;

static sxml_status push_filter(lowering_machine *machine, filter_request request,
                               sxml_ir_expression_index *expression) {
    lowering_frame *frame = NULL;
    sxml_status status = push_part_frame(
        machine,
        (part_frame_request){.procedure = SXML_LOWERING_PROCEDURE_FILTER,
                             .node = request.node,
                             .block = NULL,
                             .destination = {.expression = expression},
                             .locals_size = sizeof(filter_locals),
                             .step = 0},
        &frame);
    if (status == SXML_STATUS_OK) {
        filter_locals *locals = frame->locals;
        locals->input = request.input;
        locals->predicate = request.predicate;
    }
    return status;
}

static sxml_status continue_filter(lowering_machine *machine,
                                   lowering_frame *frame) {
    lowerer *state = machine->state;
    filter_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        return call_focused_body(
            machine,
            (focused_call){.node = child_of(state, locals->predicate,
                                            SXML_AST_FIELD_EXPRESSION),
                           .preset_slots = {{0}, {0}, {0}},
                           .result = &locals->predicate_body});
    }
    sxml_ir_expression_index result = {0};
    sxml_status status = add_expression(
        state,
        (expression_to_add){
            .node = frame->node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_FILTER,
                           .filter = {.input = locals->input,
                                      .focus = locals->predicate_body.focus,
                                      .predicate = locals->predicate_body.body}}},
        &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_frame(machine, frame, result);
}

/* "E1/E2" over the atom of E1 (XPath 4.0, section 4.6.3): MAP binding
   each item as the focus, then DOCUMENT_ORDER. */
typedef struct path_map_locals {
    sxml_ir_atom input;
    sxml_ast_node_index right;
    focused_result body;
} path_map_locals;

typedef struct path_map_request {
    sxml_ast_node_index node;
    lowering_block *block;
    sxml_ir_atom input;
    sxml_ast_node_index right;
} path_map_request;

static sxml_status push_path_map(lowering_machine *machine,
                                 path_map_request request,
                                 sxml_ir_expression_index *expression) {
    lowering_frame *frame = NULL;
    sxml_status status = push_part_frame(
        machine,
        (part_frame_request){.procedure = SXML_LOWERING_PROCEDURE_PATH_MAP,
                             .node = request.node,
                             .block = request.block,
                             .destination = {.expression = expression},
                             .locals_size = sizeof(path_map_locals),
                             .step = 0},
        &frame);
    if (status == SXML_STATUS_OK) {
        path_map_locals *locals = frame->locals;
        locals->input = request.input;
        locals->right = request.right;
    }
    return status;
}

static sxml_status continue_path_map(lowering_machine *machine,
                                     lowering_frame *frame) {
    lowerer *state = machine->state;
    path_map_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        return call_focused_body(
            machine, (focused_call){.node = locals->right,
                                    .preset_slots = {{0}, {0}, {0}},
                                    .result = &locals->body});
    }
    sxml_ir_expression_index map = {0};
    sxml_status status = add_expression(
        state,
        (expression_to_add){
            .node = frame->node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                           .map = {.input = locals->input,
                                   .focus = locals->body.focus,
                                   .body = locals->body.body,
                                   .flags = SXML_IR_MAP_REQUIRES_NODES}}},
        &map);
    sxml_ir_atom mapped = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_expression(
            state, (expression_in_block){.block = frame->block, .expression = map},
            &mapped);
    }
    sxml_ir_expression_index ordered = {0};
    if (status == SXML_STATUS_OK) {
        status = add_document_order(
            state, (atom_at){.node = frame->node, .atom = mapped}, &ordered);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_frame(machine, frame, ordered);
}

/* An axis step (XPath 4.0, section 4.6.4) with its predicates. */
typedef struct axis_step_locals {
    sxml_axis axis;
    sxml_ir_expression_index result;
    /* The next predicate. */
    sxml_ast_node_index predicate;
} axis_step_locals;

static sxml_status begin_axis_step(lowering_machine *machine,
                                   lowering_frame *frame) {
    lowerer *state = machine->state;
    axis_step_locals *locals = frame->locals;
    const sxml_ast_node_index node = frame->node;
    const sxml_ast_node *step = node_of(state, node);
    const sxml_ast_node_index test = child_of(state, node, SXML_AST_FIELD_TEST);
    const sxml_ast_kind test_kind = node_of(state, test)->kind;
    sxml_axis axis = SXML_AXIS_CHILD;
    if (step->detail.axis == SXML_AST_AXIS_NONE) {
        /* XPath 4.0, section 4.6.5: the default axis is attribute for an
           attribute test, namespace for a namespace-node() test, child
           otherwise. */
        if (test_kind == SXML_AST_KIND_ATTRIBUTE_NODE_TYPE ||
            test_kind == SXML_AST_KIND_SCHEMA_ATTRIBUTE_NODE_TYPE) {
            axis = SXML_AXIS_ATTRIBUTE;
        } else if (test_kind == SXML_AST_KIND_NAMESPACE_NODE_TYPE) {
            axis = SXML_AXIS_NAMESPACE;
        }
    } else if (!axis_of(step->detail.axis, &axis)) {
        return report_unsupported(state, node,
                                  SXML_BYTE_VIEW_FROM_LITERAL("the item axis is not lowered in Phase 1"));
    }
    sxml_type_identifier node_test = {0};
    sxml_status status = resolve_node_test(
        state, (node_test_request){.test = test, .axis = axis}, &node_test);
    sxml_ir_atom origin = {0};
    if (status == SXML_STATUS_OK) {
        status = read_focus(state,
                            (focus_read){.block = frame->block,
                                         .node = node,
                                         .part = SXML_LOWERING_FOCUS_PART_VALUE},
                            &origin);
    }
    if (status == SXML_STATUS_OK) {
        status = add_step(state,
                          (step_request){.block = frame->block,
                                         .node = node,
                                         .origin = origin,
                                         .axis = axis,
                                         .node_test = node_test},
                          &locals->result);
    }
    locals->axis = axis;
    locals->predicate = child_of(state, node, SXML_AST_FIELD_PREDICATE);
    return status;
}

static sxml_status continue_axis_step(lowering_machine *machine,
                                      lowering_frame *frame) {
    lowerer *state = machine->state;
    axis_step_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        return begin_axis_step(machine, frame);
    }
    if (locals->predicate.value != 0) {
        const sxml_ast_node_index predicate = locals->predicate;
        locals->predicate = next_of(state, predicate);
        sxml_ir_atom input = {0};
        sxml_status status = bind_expression(
            state,
            (expression_in_block){.block = frame->block, .expression = locals->result},
            &input);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return push_filter(machine,
                           (filter_request){.node = predicate,
                                            .input = input,
                                            .predicate = predicate},
                           &locals->result);
    }
    if (is_reverse_axis(locals->axis)) {
        /* A step's value is in document order (XPath 4.0, section 4.6.4);
           a reverse axis yields its nodes nearest first. */
        sxml_ir_atom input = {0};
        sxml_status status = bind_expression(
            state,
            (expression_in_block){.block = frame->block, .expression = locals->result},
            &input);
        if (status == SXML_STATUS_OK) {
            status = add_document_order(
                state, (atom_at){.node = frame->node, .atom = input},
                &locals->result);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return finish_frame(machine, frame, locals->result);
}

/* "/" at the start of a path (XPath 4.0, section 4.6.2):
   fn:root(self::node()) treat as document-node(). */
typedef struct absolute_path_locals {
    sxml_ir_expression_index result;
} absolute_path_locals;

static sxml_status begin_absolute_path(lowering_machine *machine,
                                       lowering_frame *frame) {
    lowerer *state = machine->state;
    absolute_path_locals *locals = frame->locals;
    const sxml_ast_node_index node = frame->node;
    lowering_block *block = frame->block;
    sxml_function_entry root_entry = {0};
    sxml_status status = find_builtin_function(
        state,
        (builtin_function_request){.node = node,
                                   .local_name =
                                       SXML_BYTE_VIEW_FROM_LITERAL("root"),
                                   .arity = 1},
        &root_entry);
    sxml_ir_atom focus = {0};
    if (status == SXML_STATUS_OK) {
        status = read_focus(state,
                            (focus_read){.block = block,
                                         .node = node,
                                         .part = SXML_LOWERING_FOCUS_PART_VALUE},
                            &focus);
    }
    sxml_ir_expression_index self = {0};
    if (status == SXML_STATUS_OK) {
        status = add_step(
            state,
            (step_request){
                .block = block,
                .node = node,
                .origin = focus,
                .axis = SXML_AXIS_SELF,
                .node_test = sxml_builtin_type_identifier(SXML_TYPE_GENERIC_NODE)},
            &self);
    }
    call_argument root_argument = {.is_placeholder = false, .atom = {0}};
    if (status == SXML_STATUS_OK) {
        status = bind_expression(
            state, (expression_in_block){.block = block, .expression = self},
            &root_argument.atom);
    }
    sxml_ir_expression_index root = {0};
    if (status == SXML_STATUS_OK) {
        const prepared_call call = {.node = node,
                                    .entry = root_entry,
                                    .arguments = &root_argument,
                                    .argument_count = 1,
                                    .placeholder_count = 0};
        status = emit_call(state,
                           (call_emission){.block = block,
                                           .call = &call,
                                           .coerces_arguments = false},
                           &root);
    }
    sxml_ir_atom root_atom = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_expression(
            state, (expression_in_block){.block = block, .expression = root},
            &root_atom);
    }
    sxml_ir_expression_index document = {0};
    if (status == SXML_STATUS_OK) {
        status = add_expression(
            state,
            (expression_to_add){
                .node = node,
                .expression = {
                    .expression_kind = SXML_IR_EXPRESSION_KIND_TREAT,
                    .type_test = {
                        .value = root_atom,
                        .sequence_type = {
                            .item_type = sxml_builtin_type_identifier(
                                SXML_TYPE_DOCUMENT_NODE),
                            .occurrence = SXML_OCCURRENCE_EXACTLY_ONE}}}},
            &document);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const sxml_ast_node_index path = child_of(state, node, SXML_AST_FIELD_PATH);
    if (path.value == 0) {
        return finish_frame(machine, frame, document);
    }
    sxml_ir_atom input = {0};
    status = bind_expression(
        state, (expression_in_block){.block = block, .expression = document},
        &input);
    if (status == SXML_STATUS_OK &&
        node_of(state, node)->detail.operator == SXML_AST_OPERATOR_DOUBLE_SLASH) {
        status = add_descendants(
            state,
            (descendant_request){.block = block, .node = node, .input = input},
            &input);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return push_path_map(machine,
                         (path_map_request){.node = node,
                                            .block = block,
                                            .input = input,
                                            .right = path},
                         &locals->result);
}

static sxml_status continue_absolute_path(lowering_machine *machine,
                                          lowering_frame *frame) {
    if (frame->step == 0) {
        frame->step = 1;
        return begin_absolute_path(machine, frame);
    }
    const absolute_path_locals *locals = frame->locals;
    return finish_frame(machine, frame, locals->result);
}

/* A lookup "?key" of value (XPath 4.0, section 4.7.3); the frame's node is
   the LOOKUP or UNARY_LOOKUP node. */
typedef struct lookup_locals {
    sxml_ir_atom value;
    sxml_ast_node_index key_node;
    sxml_ir_atom key;
} lookup_locals;

static sxml_status continue_lookup(lowering_machine *machine,
                                   lowering_frame *frame) {
    lowerer *state = machine->state;
    lookup_locals *locals = frame->locals;
    if (frame->step == 0) {
        /* "?key" on the context value (XPath 4.0, section 4.7.3.2). */
        frame->step = 1;
        locals->key_node = child_of(state, frame->node, SXML_AST_FIELD_KEY);
        return read_focus(state,
                          (focus_read){.block = frame->block,
                                       .node = frame->node,
                                       .part = SXML_LOWERING_FOCUS_PART_VALUE},
                          &locals->value);
    }
    if (frame->step == 1) {
        frame->step = 2;
        const sxml_ast_node *key_node = node_of(state, locals->key_node);
        if (key_node->kind == SXML_AST_KIND_LOOKUP_WILDCARD) {
            /* "?*": every entry or member (XPath 4.0, section 4.7.3). */
            locals->key = (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_NONE};
            return SXML_STATUS_OK;
        }
        if (key_node->kind == SXML_AST_KIND_NCNAME) {
            /* An NCName key is the string of that name. */
            return add_string_constant(
                state,
                sxml_ast_get_text(state->ast, key_node->payload.name.local_name),
                &locals->key);
        }
        return call_atom(machine, (atom_call){.node = locals->key_node,
                                              .block = frame->block,
                                              .atom = &locals->key});
    }
    sxml_ir_expression_index result = {0};
    sxml_status status = add_expression(
        state,
        (expression_to_add){
            .node = frame->node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_LOOKUP,
                           .lookup = {.value = locals->value, .key = locals->key}}},
        &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_frame(machine, frame, result);
}


/* ------------------------------------------------------------- Chains */

/* The locals of a link frame start with the atom of its left operand. */
typedef struct link_locals {
    sxml_ir_atom left;
} link_locals;

/* "and" and "or" (XPath 4.0, section 4.12): a CONDITIONAL on the left
   operand's effective boolean value; the right one is evaluated only when
   it decides, as true() or false(). */
typedef struct boolean_link_locals {
    sxml_ir_atom left;
    sxml_ir_expression_index decided;
    /* The right operand is a block of its own. */
    lowering_block operand_block;
    sxml_ir_atom value;
} boolean_link_locals;

static sxml_status continue_boolean_link(lowering_machine *machine,
                                         lowering_frame *frame) {
    lowerer *state = machine->state;
    boolean_link_locals *locals = frame->locals;
    const bool is_or =
        node_of(state, frame->node)->kind == SXML_AST_KIND_OR_EXPRESSION;
    const sxml_ast_node_index right =
        child_of(state, frame->node, SXML_AST_FIELD_RIGHT);
    if (frame->step == 0) {
        frame->step = 1;
        sxml_status status = add_shared_constant_expression(
            state,
            (shared_constant_at){
                .node = frame->node,
                .which = is_or ? SXML_LOWERING_SHARED_CONSTANT_TRUE
                               : SXML_LOWERING_SHARED_CONSTANT_FALSE},
            &locals->decided);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return call_atom(machine, (atom_call){.node = right,
                                              .block = &locals->operand_block,
                                              .atom = &locals->value});
    }
    sxml_ir_expression_index true_branch = {0};
    sxml_ir_expression_index false_branch = {0};
    sxml_status status = add_shared_constant_expression(
        state,
        (shared_constant_at){.node = right,
                             .which = SXML_LOWERING_SHARED_CONSTANT_TRUE},
        &true_branch);
    if (status == SXML_STATUS_OK) {
        status = add_shared_constant_expression(
            state,
            (shared_constant_at){.node = right,
                                 .which = SXML_LOWERING_SHARED_CONSTANT_FALSE},
            &false_branch);
    }
    sxml_ir_expression_index conditional = {0};
    if (status == SXML_STATUS_OK) {
        status = add_conditional(state,
                                 (conditional_request){.node = right,
                                                       .condition = locals->value,
                                                       .then_branch = true_branch,
                                                       .else_branch = false_branch},
                                 &conditional);
    }
    sxml_ir_expression_index right_value = {0};
    if (status == SXML_STATUS_OK) {
        status = finish_block(
            state,
            (block_result){.block = locals->operand_block, .result = conditional},
            &right_value);
    }
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        status = add_conditional(
            state,
            (conditional_request){
                .node = frame->node,
                .condition = locals->left,
                .then_branch = is_or ? locals->decided : right_value,
                .else_branch = is_or ? right_value : locals->decided},
            &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_frame(machine, frame, result);
}

/* "otherwise" (XPath 4.0, section 4.14.2): the left value when it is not
   empty. */
typedef struct otherwise_link_locals {
    sxml_ir_atom left;
    sxml_ir_atom condition;
    sxml_ir_expression_index left_value;
    sxml_ir_expression_index right_value;
} otherwise_link_locals;

static sxml_status continue_otherwise_link(lowering_machine *machine,
                                           lowering_frame *frame) {
    lowerer *state = machine->state;
    otherwise_link_locals *locals = frame->locals;
    const sxml_ast_node_index node = frame->node;
    if (frame->step == 0) {
        frame->step = 1;
        sxml_ir_expression_index exists = {0};
        sxml_status status = add_expression(
            state,
            (expression_to_add){
                .node = node,
                .expression = {
                    .expression_kind = SXML_IR_EXPRESSION_KIND_INSTANCE_OF,
                    .type_test = {
                        .value = locals->left,
                        .sequence_type = {
                            .item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                            .occurrence = SXML_OCCURRENCE_ONE_OR_MORE}}}},
            &exists);
        if (status == SXML_STATUS_OK) {
            status = bind_expression(
                state,
                (expression_in_block){.block = frame->block, .expression = exists},
                &locals->condition);
        }
        if (status == SXML_STATUS_OK) {
            status = add_atom_expression(
                state, (atom_at){.node = node, .atom = locals->left},
                &locals->left_value);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return call_body(machine, child_of(state, node, SXML_AST_FIELD_RIGHT),
                         &locals->right_value);
    }
    sxml_ir_expression_index result = {0};
    sxml_status status =
        add_conditional(state,
                        (conditional_request){.node = node,
                                              .condition = locals->condition,
                                              .then_branch = locals->left_value,
                                              .else_branch = locals->right_value},
                        &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_frame(machine, frame, result);
}

/* An operator that is one BINARY expression. */
typedef struct binary_link_locals {
    sxml_ir_atom left;
    sxml_ir_atom right;
} binary_link_locals;

static sxml_status continue_binary_link(lowering_machine *machine,
                                        lowering_frame *frame) {
    lowerer *state = machine->state;
    binary_link_locals *locals = frame->locals;
    sxml_ir_binary_operator operator = SXML_IR_BINARY_OPERATOR_ADD;
    if (!binary_operator_of(node_of(state, frame->node)->detail.operator,
                            &operator)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    if (frame->step == 0) {
        frame->step = 1;
        return call_atom(
            machine,
            (atom_call){.node = child_of(state, frame->node, SXML_AST_FIELD_RIGHT),
                        .block = frame->block,
                        .atom = &locals->right});
    }
    sxml_ir_expression_index result = {0};
    sxml_status status = add_expression(
        state,
        (expression_to_add){
            .node = frame->node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_BINARY,
                           .binary = {.operator = operator,
                                      .left_operand = locals->left,
                                      .right_operand = locals->right}}},
        &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_frame(machine, frame, result);
}

/* "->" (XPath 4.0, section 4.19): the right operand with the left value as
   the context value, position and size 1. */
typedef struct pipeline_link_locals {
    sxml_ir_atom left;
    lowering_focus focus;
    lowering_focus *outer_focus;
    sxml_ir_expression_index result;
} pipeline_link_locals;

static sxml_status continue_pipeline_link(lowering_machine *machine,
                                          lowering_frame *frame) {
    lowerer *state = machine->state;
    pipeline_link_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        locals->focus = (lowering_focus){.kind = SXML_LOWERING_FOCUS_KIND_FIXED,
                                         .fixed_value = locals->left};
        locals->outer_focus = state->function->focus;
        state->function->focus = &locals->focus;
        return call_expression(
            machine,
            (expression_call){
                .node = child_of(state, frame->node, SXML_AST_FIELD_RIGHT),
                .block = frame->block,
                .is_body = false,
                .destination = {.expression = &locals->result}});
    }
    state->function->focus = locals->outer_focus;
    return finish_frame(machine, frame, locals->result);
}

/* "!" (XPath 4.0, section 4.18): MAP binding each item. */
typedef struct simple_map_link_locals {
    sxml_ir_atom left;
    focused_result body;
} simple_map_link_locals;

static sxml_status continue_simple_map_link(lowering_machine *machine,
                                            lowering_frame *frame) {
    lowerer *state = machine->state;
    simple_map_link_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        return call_focused_body(
            machine,
            (focused_call){
                .node = child_of(state, frame->node, SXML_AST_FIELD_RIGHT),
                .preset_slots = {{0}, {0}, {0}},
                .result = &locals->body});
    }
    sxml_ir_expression_index result = {0};
    sxml_status status = add_expression(
        state,
        (expression_to_add){
            .node = frame->node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                           .map = {.input = locals->left,
                                   .focus = locals->body.focus,
                                   .body = locals->body.body,
                                   .flags = 0}}},
        &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_frame(machine, frame, result);
}

/* "=!>" (XPath 4.0, 4.20.2): a MAP over the value calling the target with
   each item first; the other arguments are evaluated once, before it,
   with the outer focus. */
typedef struct arrow_map_locals {
    sxml_ir_atom left;
    sxml_ir_slot item;
    lowering_block body_block;
    sxml_ir_expression_index result;
} arrow_map_locals;

/* A static call, prepared in one block and emitted in another. */
typedef struct function_call_request {
    sxml_ast_node_index call_node;
    lowering_block *preparation_block;
    lowering_block *emission_block;
    /* For "=>" and "=!>": the value or item, first argument. */
    bool has_leading_argument;
    sxml_ir_atom leading_argument;
} function_call_request;

static sxml_status push_function_call(lowering_machine *machine,
                                      function_call_request request,
                                      sxml_ir_expression_index *expression);

/* A dynamic call of the atom of function_node (lowered in function_block)
   or of function_item, with the arguments of list lowered in block. */
typedef struct dynamic_call_request {
    sxml_ast_node_index node;
    lowering_block *block;
    sxml_ast_node_index function_node;
    lowering_block *function_block;
    sxml_ir_atom function_item;
    sxml_ast_node_index list;
    bool has_leading_argument;
    sxml_ir_atom leading_argument;
} dynamic_call_request;

static sxml_status push_dynamic_call(lowering_machine *machine,
                                     const dynamic_call_request *request,
                                     sxml_ir_expression_index *expression);

static sxml_status continue_arrow_map(lowering_machine *machine,
                                      lowering_frame *frame) {
    lowerer *state = machine->state;
    arrow_map_locals *locals = frame->locals;
    const sxml_ast_node_index target =
        child_of(state, frame->node, SXML_AST_FIELD_TARGET);
    if (frame->step == 0) {
        frame->step = 1;
        sxml_status status = new_slot(state, &locals->item);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (node_of(state, target)->kind == SXML_AST_KIND_FUNCTION_CALL) {
            return push_function_call(
                machine,
                (function_call_request){.call_node = target,
                                        .preparation_block = frame->block,
                                        .emission_block = &locals->body_block,
                                        .has_leading_argument = true,
                                        .leading_argument = slot_atom(locals->item)},
                &locals->result);
        }
        /* The arguments are lowered in the body, but with the outer focus:
           the MAP binds only the item slot, not the lowering's focus, and
           the outer focus slots stay bound inside it. */
        return push_dynamic_call(
            machine,
            &(dynamic_call_request){
                .node = target,
                .block = &locals->body_block,
                .function_node = child_of(state, target, SXML_AST_FIELD_FUNCTION),
                .function_block = frame->block,
                .list = child_of(state, target, SXML_AST_FIELD_ARGUMENTS),
                .has_leading_argument = true,
                .leading_argument = slot_atom(locals->item)},
            &locals->result);
    }
    sxml_ir_expression_index body = {0};
    sxml_status status = finish_block(
        state, (block_result){.block = locals->body_block, .result = locals->result},
        &body);
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        status = add_expression(
            state,
            (expression_to_add){
                .node = frame->node,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                               .map = {.input = locals->left,
                                       .focus = {.context_value = locals->item},
                                       .body = body,
                                       .flags = 0}}},
            &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_frame(machine, frame, result);
}

/* A chain, lowered in a loop over its spine: its first operand, then one
   link frame per operator up through the parent links. */
typedef struct chain_locals {
    sxml_ir_atom left;
    /* The link being lowered. */
    sxml_ast_node_index link;
    sxml_ir_expression_index result;
} chain_locals;

/* Pushes the frame of a link whose locals start with link_locals. */
typedef struct link_frame_request {
    lowering_procedure procedure;
    size_t locals_size;
} link_frame_request;

static sxml_status push_link_frame(lowering_machine *machine,
                                   lowering_frame *chain,
                                   link_frame_request request) {
    chain_locals *locals = chain->locals;
    lowering_frame *frame = NULL;
    sxml_status status = push_part_frame(
        machine,
        (part_frame_request){.procedure = request.procedure,
                             .node = locals->link,
                             .block = chain->block,
                             .destination = {.expression = &locals->result},
                             .locals_size = request.locals_size,
                             .step = 0},
        &frame);
    if (status == SXML_STATUS_OK) {
        link_locals *link = frame->locals;
        link->left = locals->left;
    }
    return status;
}

/* "=>" and "=!>" (XPath 4.0, 4.20). */
static sxml_status begin_arrow(lowering_machine *machine,
                               lowering_frame *chain) {
    lowerer *state = machine->state;
    chain_locals *locals = chain->locals;
    const sxml_ast_node_index node = locals->link;
    const sxml_ast_node_index target = child_of(state, node, SXML_AST_FIELD_TARGET);
    if (node_of(state, node)->detail.operator == SXML_AST_OPERATOR_MAPPING_ARROW) {
        return push_link_frame(
            machine, chain,
            (link_frame_request){.procedure = SXML_LOWERING_PROCEDURE_ARROW_MAP,
                                 .locals_size = sizeof(arrow_map_locals)});
    }
    /* "=>": the value is the first argument (XPath 4.0, 4.20.1). */
    if (node_of(state, target)->kind == SXML_AST_KIND_FUNCTION_CALL) {
        return push_function_call(
            machine,
            (function_call_request){.call_node = target,
                                    .preparation_block = chain->block,
                                    .emission_block = chain->block,
                                    .has_leading_argument = true,
                                    .leading_argument = locals->left},
            &locals->result);
    }
    return push_dynamic_call(
        machine,
        &(dynamic_call_request){
            .node = target,
            .block = chain->block,
            .function_node = child_of(state, target, SXML_AST_FIELD_FUNCTION),
            .function_block = chain->block,
            .list = child_of(state, target, SXML_AST_FIELD_ARGUMENTS),
            .has_leading_argument = true,
            .leading_argument = locals->left},
        &locals->result);
}

/* Starts lowering the chain's current link, applied to its left atom. */
static sxml_status begin_chain_link(lowering_machine *machine,
                                    lowering_frame *chain) {
    lowerer *state = machine->state;
    chain_locals *locals = chain->locals;
    const sxml_ast_node_index node = locals->link;
    switch ((uint32_t)node_of(state, node)->kind) {
    case SXML_AST_KIND_OR_EXPRESSION:
    case SXML_AST_KIND_AND_EXPRESSION:
        return push_link_frame(
            machine, chain,
            (link_frame_request){.procedure = SXML_LOWERING_PROCEDURE_BOOLEAN_LINK,
                                 .locals_size = sizeof(boolean_link_locals)});
    case SXML_AST_KIND_OTHERWISE_EXPRESSION:
        return push_link_frame(
            machine, chain,
            (link_frame_request){.procedure = SXML_LOWERING_PROCEDURE_OTHERWISE_LINK,
                                 .locals_size = sizeof(otherwise_link_locals)});
    case SXML_AST_KIND_COMPARISON_EXPRESSION:
    case SXML_AST_KIND_STRING_CONCATENATION_EXPRESSION:
    case SXML_AST_KIND_RANGE_EXPRESSION:
    case SXML_AST_KIND_ADDITIVE_EXPRESSION:
    case SXML_AST_KIND_MULTIPLICATIVE_EXPRESSION:
    case SXML_AST_KIND_UNION_EXPRESSION:
    case SXML_AST_KIND_INTERSECT_EXCEPT_EXPRESSION:
        return push_link_frame(
            machine, chain,
            (link_frame_request){.procedure = SXML_LOWERING_PROCEDURE_BINARY_LINK,
                                 .locals_size = sizeof(binary_link_locals)});
    case SXML_AST_KIND_PIPELINE_EXPRESSION:
        return push_link_frame(
            machine, chain,
            (link_frame_request){.procedure = SXML_LOWERING_PROCEDURE_PIPELINE_LINK,
                                 .locals_size = sizeof(pipeline_link_locals)});
    case SXML_AST_KIND_SIMPLE_MAP_EXPRESSION:
        return push_link_frame(
            machine, chain,
            (link_frame_request){.procedure = SXML_LOWERING_PROCEDURE_SIMPLE_MAP_LINK,
                                 .locals_size = sizeof(simple_map_link_locals)});
    case SXML_AST_KIND_RELATIVE_PATH_EXPRESSION: {
        /* "/" and "//" between two operands (XPath 4.0, section 4.6.3). */
        sxml_ir_atom input = locals->left;
        if (node_of(state, node)->detail.operator ==
            SXML_AST_OPERATOR_DOUBLE_SLASH) {
            const sxml_status status = add_descendants(
                state,
                (descendant_request){
                    .block = chain->block, .node = node, .input = input},
                &input);
            if (status != SXML_STATUS_OK) {
                return status;
            }
        }
        return push_path_map(
            machine,
            (path_map_request){.node = node,
                               .block = chain->block,
                               .input = input,
                               .right = child_of(state, node, SXML_AST_FIELD_RIGHT)},
            &locals->result);
    }
    case SXML_AST_KIND_FILTER_EXPRESSION:
        return push_filter(
            machine,
            (filter_request){
                .node = node,
                .input = locals->left,
                .predicate = child_of(state, node, SXML_AST_FIELD_PREDICATE)},
            &locals->result);
    case SXML_AST_KIND_LOOKUP_EXPRESSION: {
        lowering_frame *frame = NULL;
        sxml_status status = push_part_frame(
            machine,
            (part_frame_request){.procedure = SXML_LOWERING_PROCEDURE_LOOKUP,
                                 .node = node,
                                 .block = chain->block,
                                 .destination = {.expression = &locals->result},
                                 .locals_size = sizeof(lookup_locals),
                                 .step = 1},
            &frame);
        if (status == SXML_STATUS_OK) {
            lookup_locals *lookup = frame->locals;
            lookup->value = locals->left;
            lookup->key_node = child_of(
                state, child_of(state, node, SXML_AST_FIELD_LOOKUP),
                SXML_AST_FIELD_KEY);
        }
        return status;
    }
    case SXML_AST_KIND_DYNAMIC_FUNCTION_CALL:
        return push_dynamic_call(
            machine,
            &(dynamic_call_request){
                .node = node,
                .block = chain->block,
                .function_item = locals->left,
                .list = child_of(state, node, SXML_AST_FIELD_ARGUMENTS),
                .has_leading_argument = false},
            &locals->result);
    case SXML_AST_KIND_ARROW_EXPRESSION:
        return begin_arrow(machine, chain);
    default:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
}

static sxml_status continue_chain(lowering_machine *machine,
                                  lowering_frame *frame) {
    lowerer *state = machine->state;
    chain_locals *locals = frame->locals;
    switch (frame->step) {
    case 0: {
        sxml_ast_node_index bottom = frame->node;
        sxml_ast_field field = chain_field(node_of(state, bottom)->kind);
        while (field != SXML_AST_FIELD_NONE) {
            bottom = child_of(state, bottom, field);
            field = chain_field(node_of(state, bottom)->kind);
        }
        frame->step = 1;
        locals->link = node_of(state, bottom)->parent;
        return call_atom(machine, (atom_call){.node = bottom,
                                              .block = frame->block,
                                              .atom = &locals->left});
    }
    case 1:
        frame->step = 2;
        return begin_chain_link(machine, frame);
    default:
        if (locals->link.value == frame->node.value) {
            return finish_frame(machine, frame, locals->result);
        }
        frame->step = 1;
        locals->link = node_of(state, locals->link)->parent;
        return bind_expression(
            state,
            (expression_in_block){.block = frame->block,
                                  .expression = locals->result},
            &locals->left);
    }
}


/* -------------------------------------------------------------- Calls */

/* A static function call (XPath 4.0, section 4.5.1): resolved, its
   arguments lowered to atoms in the preparation block one per step, then
   emitted in the emission block. */
typedef struct function_call_locals {
    function_call_request request;
    sxml_ast_node_index list;
    sxml_function_entry entry;
    size_t arity;
    size_t positional_count;
    size_t placeholder_count;
    /* The next argument and the position of the next positional one. */
    sxml_ast_node_index argument;
    size_t position;
    /* arity of each (owned memory 0 and 1). */
    call_argument *arguments;
    bool *is_supplied;
} function_call_locals;

static sxml_status push_function_call(lowering_machine *machine,
                                      function_call_request request,
                                      sxml_ir_expression_index *expression) {
    lowering_frame *frame = NULL;
    sxml_status status = push_part_frame(
        machine,
        (part_frame_request){.procedure = SXML_LOWERING_PROCEDURE_FUNCTION_CALL,
                             .node = request.call_node,
                             .block = request.preparation_block,
                             .destination = {.expression = expression},
                             .locals_size = sizeof(function_call_locals),
                             .step = 1},
        &frame);
    if (status == SXML_STATUS_OK) {
        function_call_locals *locals = frame->locals;
        locals->request = request;
    }
    return status;
}

/* The function of a call and its arity, keyword arguments checked, the
   arguments allocated (prepare_call up to its arguments). */
static sxml_status begin_function_call(lowering_machine *machine,
                                       lowering_frame *frame) {
    lowerer *state = machine->state;
    function_call_locals *locals = frame->locals;
    const sxml_ast_node_index call_node = frame->node;
    const sxml_ast_node_index list =
        child_of(state, call_node, SXML_AST_FIELD_ARGUMENTS);
    size_t positional_count = locals->request.has_leading_argument ? 1U : 0U;
    size_t keyword_count = 0;
    for (sxml_ast_node_index argument =
             child_of(state, list, SXML_AST_FIELD_ARGUMENT);
         argument.value != 0; argument = next_of(state, argument)) {
        if (node_of(state, argument)->kind == SXML_AST_KIND_KEYWORD_ARGUMENT) {
            keyword_count += 1;
        } else {
            positional_count += 1;
        }
    }
    sxml_name_identifier name = {0};
    sxml_status status = resolve_function_name(
        state, child_of(state, call_node, SXML_AST_FIELD_NAME), &name);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t arity = positional_count + keyword_count;
    sxml_function_entry entry = {0};
    status = find_function(
        state, (function_lookup){.function_name = name, .arity = arity}, &entry);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (entry.entry_kind == SXML_FUNCTION_ENTRY_KIND_NONE) {
        /* XPath 4.0, section 4.5.1.1: the name and arity must match a
           statically known function definition. */
        return report_static_error(
            state, call_node,
            SXML_BYTE_VIEW_FROM_LITERAL("XPST0017 no function with this name and arity is known"));
    }
    /* Keyword arguments (XPath 4.0, section 4.5.1.1) name parameters after
       the positional ones; the arity is the last one named. */
    size_t needed_arity = positional_count;
    for (sxml_ast_node_index argument =
             child_of(state, list, SXML_AST_FIELD_ARGUMENT);
         argument.value != 0; argument = next_of(state, argument)) {
        if (node_of(state, argument)->kind != SXML_AST_KIND_KEYWORD_ARGUMENT) {
            continue;
        }
        const size_t parameter_index = find_parameter(
            &entry,
            sxml_ast_get_text(
                state->ast,
                node_of(state, child_of(state, argument, SXML_AST_FIELD_NAME))
                    ->payload.name.local_name));
        if (parameter_index == SIZE_MAX || parameter_index < positional_count) {
            return report_static_error(
                state, argument,
                SXML_BYTE_VIEW_FROM_LITERAL("XPST0017 a keyword argument names no parameter after the "
                "positional arguments"));
        }
        if (parameter_index + 1 > needed_arity) {
            needed_arity = parameter_index + 1;
        }
    }
    /* A parameter whose default is the context value, left out, is
       supplied by the caller (F&O 4.0, section 1.5). */
    while (needed_arity < entry.parameters.function_parameter_count &&
           entry.parameters.function_parameters[needed_arity].parameter_default ==
               SXML_PARAMETER_DEFAULT_CONTEXT_VALUE) {
        needed_arity += 1;
    }
    if (needed_arity != arity) {
        arity = needed_arity;
        status = find_function(
            state, (function_lookup){.function_name = name, .arity = arity},
            &entry);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        if (entry.entry_kind == SXML_FUNCTION_ENTRY_KIND_NONE) {
            return report_static_error(
                state, call_node,
                SXML_BYTE_VIEW_FROM_LITERAL("XPST0017 no function with this name and arity is known"));
        }
    }
    const size_t element_count = arity == 0 ? 1 : arity;
    status = allocate_owned(
        machine, frame,
        (owned_request){.memory_index = 0,
                        .scratch = {.element_count = element_count,
                                    .element_size = sizeof(call_argument),
                                    .element_alignment = alignof(call_argument)}});
    if (status == SXML_STATUS_OK) {
        status = allocate_owned(
            machine, frame,
            (owned_request){.memory_index = 1,
                            .scratch = {.element_count = element_count,
                                        .element_size = sizeof(bool),
                                        .element_alignment = alignof(bool)}});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    locals->arguments = frame->owned_memory[0];
    locals->is_supplied = frame->owned_memory[1];
    memset(locals->is_supplied, 0, element_count * sizeof(bool));
    memset(locals->arguments, 0, element_count * sizeof(call_argument));
    locals->list = list;
    locals->entry = entry;
    locals->arity = arity;
    locals->positional_count = positional_count;
    if (locals->request.has_leading_argument) {
        locals->arguments[0] = (call_argument){
            .is_placeholder = false, .atom = locals->request.leading_argument};
        locals->is_supplied[0] = true;
        locals->position = 1;
    }
    locals->argument = child_of(state, list, SXML_AST_FIELD_ARGUMENT);
    return SXML_STATUS_OK;
}

/* Lowers the next argument that is not a placeholder; false when none is
   left. */
static bool next_call_argument(lowering_machine *machine, lowering_frame *frame,
                               sxml_status *status) {
    lowerer *state = machine->state;
    function_call_locals *locals = frame->locals;
    while (locals->argument.value != 0) {
        const sxml_ast_node_index argument = locals->argument;
        locals->argument = next_of(state, argument);
        size_t target = locals->position;
        sxml_ast_node_index value = argument;
        if (node_of(state, argument)->kind == SXML_AST_KIND_KEYWORD_ARGUMENT) {
            target = find_parameter(
                &locals->entry,
                sxml_ast_get_text(
                    state->ast,
                    node_of(state, child_of(state, argument, SXML_AST_FIELD_NAME))
                        ->payload.name.local_name));
            value = child_of(state, argument, SXML_AST_FIELD_VALUE);
            if (target >= locals->arity || locals->is_supplied[target]) {
                *status = report_static_error(
                    state, argument,
                    SXML_BYTE_VIEW_FROM_LITERAL("XPST0017 a parameter is given two arguments"));
                return true;
            }
        } else {
            locals->position += 1;
        }
        locals->is_supplied[target] = true;
        if (node_of(state, value)->kind == SXML_AST_KIND_ARGUMENT_PLACEHOLDER) {
            locals->arguments[target].is_placeholder = true;
            locals->placeholder_count += 1;
            continue;
        }
        *status = call_atom(machine,
                            (atom_call){.node = value,
                                        .block = locals->request.preparation_block,
                                        .atom = &locals->arguments[target].atom});
        return true;
    }
    return false;
}

/* After the arguments: the parameters left out, then the call. */
static sxml_status finish_function_call(lowering_machine *machine,
                                        lowering_frame *frame) {
    lowerer *state = machine->state;
    function_call_locals *locals = frame->locals;
    const sxml_ast_node_index call_node = frame->node;
    const sxml_function_entry *entry = &locals->entry;
    sxml_status status = SXML_STATUS_OK;
    for (size_t argument_index = 0;
         status == SXML_STATUS_OK && argument_index < locals->arity;
         argument_index += 1) {
        if (locals->is_supplied[argument_index]) {
            continue;
        }
        const sxml_parameter_default parameter_default =
            argument_index < entry->parameters.function_parameter_count
                ? entry->parameters.function_parameters[argument_index]
                      .parameter_default
                : SXML_PARAMETER_DEFAULT_REQUIRED;
        if (parameter_default == SXML_PARAMETER_DEFAULT_CONTEXT_VALUE) {
            status = read_focus(state,
                                (focus_read){.block = locals->request.preparation_block,
                                             .node = call_node,
                                             .part = SXML_LOWERING_FOCUS_PART_VALUE},
                                &locals->arguments[argument_index].atom);
        } else if (parameter_default == SXML_PARAMETER_DEFAULT_REQUIRED) {
            status = report_static_error(
                state, call_node, SXML_BYTE_VIEW_FROM_LITERAL("XPST0017 a required parameter has no argument"));
        } else if (argument_index < entry->parameter_types.sequence_type_count &&
                   ((unsigned)entry->parameter_types.sequence_types[argument_index]
                        .occurrence &
                    (unsigned)SXML_OCCURRENCE_ZERO) != 0) {
            /* A parameter left out before a keyword argument, whose
               default the body supplies: the empty sequence. Every such
               default in F&O 4.0 is "()" or marked (:default-on-empty:)
               (section 1.5), so () has the default's effect; the
               exception, $timezone of the adjust-*-to-timezone functions,
               is a last parameter, never left out before another. */
            sxml_ir_expression_index empty = {0};
            status = add_empty_sequence(state, call_node, &empty);
            if (status == SXML_STATUS_OK) {
                status = bind_expression(
                    state,
                    (expression_in_block){.block = locals->request.preparation_block,
                                          .expression = empty},
                    &locals->arguments[argument_index].atom);
            }
        } else {
            status = report_unsupported(
                state, call_node,
                SXML_BYTE_VIEW_FROM_LITERAL("a keyword argument after an omitted parameter whose default "
                "the function supplies and whose type does not allow ()"));
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const prepared_call call = {.node = call_node,
                                .entry = locals->entry,
                                .arguments = locals->arguments,
                                .argument_count = locals->arity,
                                .placeholder_count = locals->placeholder_count};
    lowering_block *block = locals->request.emission_block;
    sxml_ir_expression_index result = {0};
    const uint32_t properties = native_properties(&call.entry);
    if ((properties & (SXML_NATIVE_FUNCTION_READS_CONTEXT_POSITION |
                       SXML_NATIVE_FUNCTION_READS_CONTEXT_SIZE)) != 0 &&
        call.argument_count == 0) {
        /* fn:position() and fn:last() read the focus (XPath 4.0, section
           2.2.2). */
        sxml_ir_atom atom = {0};
        status = read_focus(
            state,
            (focus_read){
                .block = block,
                .node = call.node,
                .part = (properties & SXML_NATIVE_FUNCTION_READS_CONTEXT_POSITION) != 0
                            ? SXML_LOWERING_FOCUS_PART_POSITION
                            : SXML_LOWERING_FOCUS_PART_SIZE},
            &atom);
        if (status == SXML_STATUS_OK) {
            status = add_atom_expression(
                state, (atom_at){.node = call.node, .atom = atom}, &result);
        }
    } else {
        status = emit_prepared_call(state,
                                    (call_emission){.block = block,
                                                    .call = &call,
                                                    .coerces_arguments = true},
                                    &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_frame(machine, frame, result);
}

static sxml_status continue_function_call(lowering_machine *machine,
                                          lowering_frame *frame) {
    function_call_locals *locals = frame->locals;
    switch (frame->step) {
    case 0:
        /* A call written as such, not after an arrow. */
        locals->request = (function_call_request){
            .call_node = frame->node,
            .preparation_block = frame->block,
            .emission_block = frame->block,
            .has_leading_argument = false};
        frame->step = 1;
        return SXML_STATUS_OK;
    case 1:
        frame->step = 2;
        return begin_function_call(machine, frame);
    default: {
        sxml_status status = SXML_STATUS_OK;
        if (next_call_argument(machine, frame, &status)) {
            return status;
        }
        return finish_function_call(machine, frame);
    }
    }
}

/* A dynamic call (XPath 4.0, section 4.5.3) of function_item with the
   arguments of list (a POSITIONAL_ARGUMENT_LIST), after an arrow's
   value when has_leading_argument. With placeholders, fn:partial-apply
   with a map from positions to the supplied arguments (section 4.5.2.5). */
typedef struct dynamic_call_locals {
    dynamic_call_request request;
    size_t argument_count;
    size_t placeholder_count;
    /* The next argument and its index. */
    sxml_ast_node_index argument;
    size_t argument_index;
    /* argument_count atoms (owned memory 0) and, with placeholders, map
       entries (owned memory 1). */
    sxml_ir_atom *atoms;
    sxml_ir_map_entry *entries;
} dynamic_call_locals;

static sxml_status push_dynamic_call(lowering_machine *machine,
                                     const dynamic_call_request *request,
                                     sxml_ir_expression_index *expression) {
    lowering_frame *frame = NULL;
    sxml_status status = push_part_frame(
        machine,
        (part_frame_request){.procedure = SXML_LOWERING_PROCEDURE_DYNAMIC_CALL,
                             .node = request->node,
                             .block = request->block,
                             .destination = {.expression = expression},
                             .locals_size = sizeof(dynamic_call_locals),
                             .step = 0},
        &frame);
    if (status == SXML_STATUS_OK) {
        dynamic_call_locals *locals = frame->locals;
        locals->request = *request;
    }
    return status;
}

static sxml_status begin_dynamic_call_arguments(lowering_machine *machine,
                                                lowering_frame *frame) {
    lowerer *state = machine->state;
    dynamic_call_locals *locals = frame->locals;
    const dynamic_call_request *request = &locals->request;
    size_t argument_count = request->has_leading_argument ? 1U : 0U;
    size_t placeholder_count = 0;
    for (sxml_ast_node_index argument =
             child_of(state, request->list, SXML_AST_FIELD_ARGUMENT);
         argument.value != 0; argument = next_of(state, argument)) {
        argument_count += 1;
        if (node_of(state, argument)->kind ==
            SXML_AST_KIND_ARGUMENT_PLACEHOLDER) {
            placeholder_count += 1;
        }
    }
    sxml_status status = allocate_owned(
        machine, frame,
        (owned_request){.memory_index = 0,
                        .scratch = {.element_count = argument_count,
                                    .element_size = sizeof(sxml_ir_atom),
                                    .element_alignment = alignof(sxml_ir_atom)}});
    if (status == SXML_STATUS_OK && placeholder_count > 0) {
        status = allocate_owned(
            machine, frame,
            (owned_request){
                .memory_index = 1,
                .scratch = {.element_count = argument_count,
                            .element_size = sizeof(sxml_ir_map_entry),
                            .element_alignment = alignof(sxml_ir_map_entry)}});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    locals->atoms = frame->owned_memory[0];
    locals->entries = frame->owned_memory[1];
    locals->argument_count = argument_count;
    locals->placeholder_count = placeholder_count;
    if (request->has_leading_argument) {
        locals->atoms[0] = request->leading_argument;
        locals->argument_index = 1;
    }
    locals->argument = child_of(state, request->list, SXML_AST_FIELD_ARGUMENT);
    return SXML_STATUS_OK;
}

/* The call itself, after the arguments. */
static sxml_status finish_dynamic_call(lowering_machine *machine,
                                       lowering_frame *frame) {
    lowerer *state = machine->state;
    dynamic_call_locals *locals = frame->locals;
    const dynamic_call_request *request = &locals->request;
    lowering_block *block = frame->block;
    sxml_ir_atom *atoms = locals->atoms;
    sxml_ir_expression_index result = {0};
    sxml_status status = SXML_STATUS_OK;
    if (locals->placeholder_count == 0) {
        sxml_ir_atom_list arguments = {0};
        status = sxml_ir_unit_add_atoms(
            state->unit,
            (sxml_ir_atom_view){.atoms = atoms,
                                .atom_count = locals->argument_count},
            &arguments);
        if (status == SXML_STATUS_OK) {
            status = add_expression(
                state,
                (expression_to_add){
                    .node = frame->node,
                    .expression = {
                        .expression_kind = SXML_IR_EXPRESSION_KIND_DYNAMIC_CALL,
                        .dynamic_call = {.function_item = request->function_item,
                                         .arguments = arguments}}},
                &result);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return finish_frame(machine, frame, result);
    }
    /* Positions of the supplied arguments as xs:positiveInteger keys, the
       key type of fn:partial-apply's map (F&O 4.0). */
    size_t entry_count = 0;
    for (size_t position = 0;
         status == SXML_STATUS_OK && position < locals->argument_count;
         position += 1) {
        if (atoms[position].atom_kind == SXML_IR_ATOM_KIND_NONE) {
            continue;
        }
        sxml_ir_atom key = {0};
        status = add_atomic_constant(
            state,
            (atomic_constant){
                .type = sxml_builtin_type_identifier(SXML_TYPE_XS_POSITIVE_INTEGER),
                .datum = {.datum_kind = SXML_DATUM_KIND_INTEGER,
                          .datum_details = {.integer = (int64_t)position + 1}}},
            &key);
        locals->entries[entry_count] =
            (sxml_ir_map_entry){.key = key, .value = atoms[position]};
        entry_count += 1;
    }
    sxml_ir_map_entry_list entry_list = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_map_entries(
            state->unit,
            (sxml_ir_map_entry_view){.map_entries = locals->entries,
                                     .map_entry_count = entry_count},
            &entry_list);
    }
    sxml_ir_expression_index map = {0};
    if (status == SXML_STATUS_OK) {
        status = add_expression(
            state,
            (expression_to_add){
                .node = frame->node,
                .expression = {
                    .expression_kind = SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR,
                    .map_constructor = {.entries = entry_list}}},
            &map);
    }
    /* One partially applied function for each function item of the
       target (XPath 4.0, section 4.6.4): a MAP over the target whose body
       applies fn:partial-apply to the focus. */
    sxml_ir_slot function_slot = {0};
    if (status == SXML_STATUS_OK) {
        status = new_slot(state, &function_slot);
    }
    call_argument call_arguments[2] = {
        {.is_placeholder = false, .atom = slot_atom(function_slot)},
        {.is_placeholder = false, .atom = {0}}};
    if (status == SXML_STATUS_OK) {
        status = bind_expression(
            state, (expression_in_block){.block = block, .expression = map},
            &call_arguments[1].atom);
    }
    sxml_function_entry entry = {0};
    if (status == SXML_STATUS_OK) {
        status = find_builtin_function(
            state,
            (builtin_function_request){
                .node = frame->node,
                .local_name = SXML_BYTE_VIEW_FROM_LITERAL("partial-apply"),
                .arity = 2},
            &entry);
    }
    if (status == SXML_STATUS_OK) {
        const prepared_call call = {.node = frame->node,
                                    .entry = entry,
                                    .arguments = call_arguments,
                                    .argument_count = 2,
                                    .placeholder_count = 0};
        status = emit_call(state,
                           (call_emission){.block = block,
                                           .call = &call,
                                           .coerces_arguments = false},
                           &result);
    }
    if (status == SXML_STATUS_OK) {
        const sxml_ir_expression_index body = result;
        status = add_expression(
            state,
            (expression_to_add){
                .node = frame->node,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                               .map = {.input = request->function_item,
                                       .focus = {.context_value = function_slot},
                                       .body = body,
                                       .flags = 0}}},
            &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_frame(machine, frame, result);
}

static sxml_status continue_dynamic_call(lowering_machine *machine,
                                         lowering_frame *frame) {
    lowerer *state = machine->state;
    dynamic_call_locals *locals = frame->locals;
    switch (frame->step) {
    case 0:
        frame->step = 1;
        if (locals->request.function_node.value == 0) {
            return SXML_STATUS_OK;
        }
        return call_atom(machine,
                         (atom_call){.node = locals->request.function_node,
                                     .block = locals->request.function_block,
                                     .atom = &locals->request.function_item});
    case 1:
        frame->step = 2;
        return begin_dynamic_call_arguments(machine, frame);
    default:
        while (locals->argument.value != 0) {
            const sxml_ast_node_index argument = locals->argument;
            locals->argument = next_of(state, argument);
            const size_t argument_index = locals->argument_index;
            locals->argument_index += 1;
            if (node_of(state, argument)->kind ==
                SXML_AST_KIND_ARGUMENT_PLACEHOLDER) {
                locals->atoms[argument_index] =
                    (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_NONE};
                continue;
            }
            return call_atom(machine,
                             (atom_call){.node = argument,
                                         .block = frame->block,
                                         .atom = &locals->atoms[argument_index]});
        }
        return finish_dynamic_call(machine, frame);
    }
}

/* ----------------------------------------------------- Inline functions */

typedef struct inline_function_locals {
    bool has_signature;
    size_t parameter_count;
    sxml_sequence_type result_type;
    sxml_ir_function_index function;
    function_frame function_frame;
    /* The body's block, in the new function. */
    lowering_block block;
    sxml_ast_node_index body_node;
    sxml_ir_expression_index result;
} inline_function_locals;

/* The signature and free variables of an inline function, then its body
   in a new IR function. */
static sxml_status begin_inline_function(lowering_machine *machine,
                                         lowering_frame *frame) {
    lowerer *state = machine->state;
    inline_function_locals *locals = frame->locals;
    const sxml_ast_node_index node = frame->node;
    const bool has_signature =
        (node_of(state, node)->flags & SXML_AST_FLAG_HAS_SIGNATURE) != 0;
    size_t parameter_count = 0;
    for (sxml_ast_node_index parameter =
             child_of(state, node, SXML_AST_FIELD_PARAMETER);
         parameter.value != 0; parameter = next_of(state, parameter)) {
        parameter_count += 1;
    }
    if (!has_signature) {
        /* A focus function (XPath 4.0, section 4.5.6.2): one parameter of
           type item()*, which becomes the context value. */
        parameter_count = 1;
    }
    if (parameter_count >= UINT32_MAX / 2) {
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    sxml_status status = allocate_owned(
        machine, frame,
        (owned_request){.memory_index = 0,
                        .scratch = {.element_count = parameter_count,
                                    .element_size = sizeof(sxml_sequence_type),
                                    .element_alignment =
                                        alignof(sxml_sequence_type)}});
    if (status == SXML_STATUS_OK) {
        status = allocate_owned(
            machine, frame,
            (owned_request){.memory_index = 1,
                            .scratch = {.element_count = parameter_count,
                                        .element_size = sizeof(sxml_name_identifier),
                                        .element_alignment =
                                            alignof(sxml_name_identifier)}});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_sequence_type *parameter_types = frame->owned_memory[0];
    sxml_name_identifier *parameter_names = frame->owned_memory[1];
    if (!has_signature) {
        parameter_types[0] = (sxml_sequence_type){
            .item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
            .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE};
        parameter_names[0] = (sxml_name_identifier){.index = 0};
    }
    size_t parameter_index = 0;
    for (sxml_ast_node_index parameter =
             has_signature ? child_of(state, node, SXML_AST_FIELD_PARAMETER)
                           : (sxml_ast_node_index){.value = 0};
         status == SXML_STATUS_OK && parameter.value != 0;
         parameter = next_of(state, parameter)) {
        status = resolve_variable_name(
            state, child_of(state, parameter, SXML_AST_FIELD_NAME),
            &parameter_names[parameter_index]);
        for (size_t earlier_index = 0;
             status == SXML_STATUS_OK && earlier_index < parameter_index;
             earlier_index += 1) {
            if (parameter_names[earlier_index].index ==
                parameter_names[parameter_index].index) {
                /* XPath 4.0, section 4.5.6: parameter names are distinct. */
                status = report_static_error(
                    state, parameter,
                    SXML_BYTE_VIEW_FROM_LITERAL("XPST0039 two parameters of a function have the same name"));
            }
        }
        if (status == SXML_STATUS_OK) {
            status = resolve_declared_type(
                state, child_of(state, parameter, SXML_AST_FIELD_TYPE),
                &parameter_types[parameter_index]);
        }
        parameter_index += 1;
    }
    if (status == SXML_STATUS_OK) {
        status = resolve_declared_type(
            state, child_of(state, node, SXML_AST_FIELD_RETURN_TYPE),
            &locals->result_type);
    }
    sxml_type_identifier signature = {0};
    if (status == SXML_STATUS_OK) {
        status = register_signature(
            state,
            (signature_request){
                .parameter_types = {.sequence_types = parameter_types,
                                    .sequence_type_count = parameter_count},
                .result_type = locals->result_type},
            &signature);
    }
    free_variables variables = {0};
    if (status == SXML_STATUS_OK) {
        status = find_free_variables(
            state,
            (free_variable_request){
                .function_node = node,
                .first_slot = (uint32_t)parameter_count + 1U},
            &variables);
    }
    /* The frame owns them from here on. */
    frame->owned_memory[2] = variables.outer_atoms;
    frame->owned_memory[3] = variables.captures;
    if (status == SXML_STATUS_OK) {
        status = add_closure_function(
            state,
            (closure_request){
                .node = node,
                .signature = signature,
                .parameter_count = (uint32_t)parameter_count,
                .captured_atoms = {.atoms = variables.outer_atoms,
                                   .atom_count = variables.captured_count}},
            &locals->function);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    locals->has_signature = has_signature;
    locals->parameter_count = parameter_count;
    locals->function_frame = (function_frame){
        .function = {.function_index = locals->function,
                     .function_level = state->function->function_level + 1U,
                     .slot_count =
                         (uint32_t)(parameter_count + variables.captured_count),
                     .captures = variables.captures,
                     .capture_count = variables.capture_count},
        .focus = {.kind = has_signature ? SXML_LOWERING_FOCUS_KIND_ABSENT
                                        : SXML_LOWERING_FOCUS_KIND_FIXED,
                  .fixed_value = slot_atom((sxml_ir_slot){.value = 1})},
    };
    enter_function(state, &locals->function_frame);
    for (size_t name_index = 0;
         status == SXML_STATUS_OK && has_signature && name_index < parameter_count;
         name_index += 1) {
        status = push_variable(
            state,
            (scope_entry){
                .variable_name = parameter_names[name_index],
                .function_level = locals->function_frame.function.function_level,
                .atom = slot_atom((sxml_ir_slot){.value = (uint32_t)name_index + 1U})});
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    locals->body_node = child_of(state, node, SXML_AST_FIELD_BODY);
    return call_expression(
        machine, (expression_call){.node = locals->body_node,
                                   .block = &locals->block,
                                   .is_body = false,
                                   .destination = {.expression = &locals->result}});
}

static sxml_status finish_inline_function(lowering_machine *machine,
                                          lowering_frame *frame) {
    lowerer *state = machine->state;
    inline_function_locals *locals = frame->locals;
    sxml_ir_expression_index result = locals->result;
    sxml_status status = SXML_STATUS_OK;
    if (!accepts_everything(locals->result_type)) {
        /* The declared result type is checked by coercion (XPath 4.0,
           section 4.5.6). */
        sxml_ir_atom value = {0};
        status = bind_expression(
            state, (expression_in_block){.block = &locals->block, .expression = result},
            &value);
        if (status == SXML_STATUS_OK) {
            status = add_expression(
                state,
                (expression_to_add){
                    .node = locals->body_node,
                    .expression = {
                        .expression_kind = SXML_IR_EXPRESSION_KIND_COERCE,
                        .coercion = {.value = value,
                                     .sequence_type = locals->result_type,
                                     .role = SXML_IR_COERCION_ROLE_FUNCTION_RESULT}}},
                &result);
        }
    }
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = finish_block(
            state, (block_result){.block = locals->block, .result = result}, &body);
    }
    leave_function(state, &locals->function_frame);
    if (status == SXML_STATUS_OK) {
        sxml_ir_function_replacement replacement = {
            .index = locals->function,
            .function = *sxml_ir_unit_get_function(state->unit, locals->function)};
        replacement.function.body = body;
        replacement.function.slot_count =
            locals->function_frame.function.slot_count;
        status = sxml_ir_unit_replace_function(state->unit, &replacement);
    }
    sxml_ir_expression_index definition = {0};
    if (status == SXML_STATUS_OK) {
        status = add_expression(
            state,
            (expression_to_add){
                .node = frame->node,
                .expression = {
                    .expression_kind = SXML_IR_EXPRESSION_KIND_FUNCTION_DEFINITION,
                    .function_definition = {.function = locals->function}}},
            &definition);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_frame(machine, frame, definition);
}

static sxml_status continue_inline_function(lowering_machine *machine,
                                            lowering_frame *frame) {
    if (frame->step == 0) {
        frame->step = 1;
        return begin_inline_function(machine, frame);
    }
    return finish_inline_function(machine, frame);
}


/* ------------------------------------------------------- FLWOR, some */

/* One binding of a FLWOR or quantified expression, from cursor on, then
   result; each binding is a frame whose body is the next one's. */
typedef struct binding_locals {
    binding_cursor cursor;
    /* FLWOR: the return value; quantified: the condition. */
    sxml_ast_node_index result;
    /* QUANTIFIED's quantifier, for a quantified expression. */
    bool is_quantified;
    sxml_ir_quantifier quantifier;
    sxml_ir_atom value;
    sxml_ir_slot item;
    sxml_ir_slot position;
    size_t scope_mark;
    /* A for or quantifier binding's body. */
    lowering_block body_block;
    sxml_ir_expression_index rest;
} binding_locals;

/* Steps of a binding frame. */
enum {
    SXML_LOWERING_BINDING_STEP_FROM_EXPRESSION = 0,
    SXML_LOWERING_BINDING_STEP_BINDING = 1,
    SXML_LOWERING_BINDING_STEP_ITERATING_VALUE = 2,
    SXML_LOWERING_BINDING_STEP_LET_VALUE = 3,
    SXML_LOWERING_BINDING_STEP_ITERATING_REST = 4,
    SXML_LOWERING_BINDING_STEP_LET_REST = 5,
    SXML_LOWERING_BINDING_STEP_RESULT = 6,
};

/* Pushes the frame of the binding after the current one, lowered in
   block. */
static sxml_status push_next_binding(lowering_machine *machine,
                                     lowering_frame *frame,
                                     lowering_block *block) {
    binding_locals *locals = frame->locals;
    lowering_frame *next = NULL;
    sxml_status status = push_part_frame(
        machine,
        (part_frame_request){.procedure = SXML_LOWERING_PROCEDURE_BINDING,
                             .node = frame->node,
                             .block = block,
                             .destination = {.expression = &locals->rest},
                             .locals_size = sizeof(binding_locals),
                             .step = SXML_LOWERING_BINDING_STEP_BINDING},
        &next);
    if (status == SXML_STATUS_OK) {
        binding_locals *next_locals = next->locals;
        next_locals->cursor = next_binding(machine->state, locals->cursor);
        next_locals->result = locals->result;
        next_locals->is_quantified = locals->is_quantified;
        next_locals->quantifier = locals->quantifier;
    }
    return status;
}

/* The binding at the cursor; the result after the last. */
static sxml_status begin_binding(lowering_machine *machine,
                                 lowering_frame *frame) {
    lowerer *state = machine->state;
    binding_locals *locals = frame->locals;
    const sxml_ast_node_index binding = locals->cursor.binding;
    if (binding.value == 0) {
        /* The return value, or the condition whose effective boolean value
           QUANTIFIED takes (XPath 4.0, section 4.15). */
        frame->step = SXML_LOWERING_BINDING_STEP_RESULT;
        return call_expression(
            machine, (expression_call){.node = locals->result,
                                       .block = frame->block,
                                       .is_body = false,
                                       .destination = {.expression = &locals->rest}});
    }
    switch ((uint32_t)node_of(state, binding)->kind) {
    case SXML_AST_KIND_FOR_ITEM_BINDING:
    case SXML_AST_KIND_QUANTIFIER_BINDING:
        frame->step = SXML_LOWERING_BINDING_STEP_ITERATING_VALUE;
        return call_atom(
            machine,
            (atom_call){.node = child_of(state, binding, SXML_AST_FIELD_SEQUENCE),
                        .block = frame->block,
                        .atom = &locals->value});
    case SXML_AST_KIND_LET_VALUE_BINDING:
        /* "let" (XPath 4.0, section 4.13.2): the value bound once. */
        frame->step = SXML_LOWERING_BINDING_STEP_LET_VALUE;
        return call_atom(
            machine,
            (atom_call){.node = child_of(state, binding, SXML_AST_FIELD_VALUE),
                        .block = frame->block,
                        .atom = &locals->value});
    default:
        return report_unsupported(
            state, binding,
            SXML_BYTE_VIEW_FROM_LITERAL("for member, for key/value and destructuring let bindings are not "
            "lowered in Phase 1"));
    }
}

/* A for or quantifier binding, its sequence lowered: a MAP or QUANTIFIED
   over it whose body is the rest of the bindings. */
static sxml_status continue_iterating_binding(lowering_machine *machine,
                                              lowering_frame *frame) {
    lowerer *state = machine->state;
    binding_locals *locals = frame->locals;
    const sxml_ast_node_index binding = locals->cursor.binding;
    const sxml_ast_node_index positional =
        child_of(state, binding, SXML_AST_FIELD_POSITION);
    if (frame->step == SXML_LOWERING_BINDING_STEP_ITERATING_VALUE) {
        frame->step = SXML_LOWERING_BINDING_STEP_ITERATING_REST;
        sxml_status status = new_slot(state, &locals->item);
        if (status == SXML_STATUS_OK && positional.value != 0) {
            status = new_slot(state, &locals->position);
        }
        locals->scope_mark = state->scope_entry_count;
        if (status == SXML_STATUS_OK) {
            status = push_typed_variable(
                state,
                (typed_variable){
                    .block = &locals->body_block,
                    .variable = child_of(state, binding, SXML_AST_FIELD_VARIABLE),
                    .value = slot_atom(locals->item)});
        }
        if (status == SXML_STATUS_OK && positional.value != 0) {
            sxml_name_identifier position_name = {0};
            status = resolve_variable_name(
                state, child_of(state, positional, SXML_AST_FIELD_NAME),
                &position_name);
            if (status == SXML_STATUS_OK) {
                status = push_variable(
                    state,
                    (scope_entry){.variable_name = position_name,
                                  .function_level = state->function->function_level,
                                  .atom = slot_atom(locals->position)});
            }
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return push_next_binding(machine, frame, &locals->body_block);
    }
    sxml_ir_expression_index body = {0};
    sxml_status status = finish_block(
        state, (block_result){.block = locals->body_block, .result = locals->rest},
        &body);
    state->scope_entry_count = locals->scope_mark;
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK && locals->is_quantified) {
        status = add_expression(
            state,
            (expression_to_add){
                .node = binding,
                .expression = {
                    .expression_kind = SXML_IR_EXPRESSION_KIND_QUANTIFIED,
                    .quantified = {.quantifier = locals->quantifier,
                                   .input = locals->value,
                                   .variable = locals->item,
                                   .condition = body}}},
            &result);
    } else if (status == SXML_STATUS_OK) {
        status = add_expression(
            state,
            (expression_to_add){
                .node = binding,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                               .map = {.input = locals->value,
                                       .focus = {.context_value = locals->item,
                                                 .context_position =
                                                     locals->position},
                                       .body = body,
                                       .flags = 0}}},
            &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_frame(machine, frame, result);
}

static sxml_status continue_binding(lowering_machine *machine,
                                    lowering_frame *frame) {
    lowerer *state = machine->state;
    binding_locals *locals = frame->locals;
    switch (frame->step) {
    case SXML_LOWERING_BINDING_STEP_FROM_EXPRESSION: {
        const sxml_ast_node_index node = frame->node;
        frame->step = SXML_LOWERING_BINDING_STEP_BINDING;
        if (node_of(state, node)->kind == SXML_AST_KIND_FLWOR_EXPRESSION) {
            const sxml_ast_node_index clause =
                child_of(state, node, SXML_AST_FIELD_CLAUSE);
            locals->cursor = (binding_cursor){
                .clause = clause,
                .binding = child_of(state, clause, SXML_AST_FIELD_BINDING)};
            locals->result =
                child_of(state, child_of(state, node, SXML_AST_FIELD_RETURN),
                         SXML_AST_FIELD_VALUE);
            return SXML_STATUS_OK;
        }
        locals->cursor = (binding_cursor){
            .clause = {.value = 0},
            .binding = child_of(state, node, SXML_AST_FIELD_BINDING)};
        locals->result = child_of(state, node, SXML_AST_FIELD_CONDITION);
        locals->is_quantified = true;
        locals->quantifier =
            node_of(state, node)->detail.operator == SXML_AST_OPERATOR_EVERY
                ? SXML_IR_QUANTIFIER_EVERY
                : SXML_IR_QUANTIFIER_SOME;
        return SXML_STATUS_OK;
    }
    case SXML_LOWERING_BINDING_STEP_BINDING:
        return begin_binding(machine, frame);
    case SXML_LOWERING_BINDING_STEP_ITERATING_VALUE:
    case SXML_LOWERING_BINDING_STEP_ITERATING_REST:
        return continue_iterating_binding(machine, frame);
    case SXML_LOWERING_BINDING_STEP_LET_VALUE: {
        frame->step = SXML_LOWERING_BINDING_STEP_LET_REST;
        locals->scope_mark = state->scope_entry_count;
        const sxml_status status = push_typed_variable(
            state,
            (typed_variable){
                .block = frame->block,
                .variable = child_of(state, locals->cursor.binding,
                                     SXML_AST_FIELD_VARIABLE),
                .value = locals->value});
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return push_next_binding(machine, frame, frame->block);
    }
    case SXML_LOWERING_BINDING_STEP_LET_REST:
        state->scope_entry_count = locals->scope_mark;
        return finish_frame(machine, frame, locals->rest);
    default:
        return finish_frame(machine, frame, locals->rest);
    }
}

/* -------------------------------------------------- Other expressions */

/* "E1, E2, ..." (XPath 4.0, section 4.8.1), "[...]" and "array { ... }"
   (section 4.7.2): the atoms of the items or members. */
typedef struct atom_list_locals {
    /* The next child and its index. */
    sxml_ast_node_index child;
    size_t atom_index;
    size_t atom_count;
    /* atom_count atoms (owned memory 0). */
    sxml_ir_atom *atoms;
    sxml_ir_atom content;
} atom_list_locals;

static sxml_status continue_atom_list(lowering_machine *machine,
                                      lowering_frame *frame) {
    lowerer *state = machine->state;
    atom_list_locals *locals = frame->locals;
    const sxml_ast_kind kind = node_of(state, frame->node)->kind;
    const bool is_curly = kind == SXML_AST_KIND_CURLY_ARRAY_CONSTRUCTOR;
    if (frame->step == 0) {
        frame->step = 1;
        if (is_curly) {
            return call_atom(
                machine,
                (atom_call){.node = child_of(state, frame->node,
                                             SXML_AST_FIELD_CONTENT),
                            .block = frame->block,
                            .atom = &locals->content});
        }
        const sxml_ast_field field = kind == SXML_AST_KIND_SEQUENCE_EXPRESSION
                                         ? SXML_AST_FIELD_ITEM
                                         : SXML_AST_FIELD_MEMBER;
        locals->child = child_of(state, frame->node, field);
        for (sxml_ast_node_index child = locals->child; child.value != 0;
             child = next_of(state, child)) {
            locals->atom_count += 1;
        }
        const sxml_status status = allocate_owned(
            machine, frame,
            (owned_request){.memory_index = 0,
                            .scratch = {.element_count = locals->atom_count,
                                        .element_size = sizeof(sxml_ir_atom),
                                        .element_alignment = alignof(sxml_ir_atom)}});
        locals->atoms = frame->owned_memory[0];
        return status;
    }
    if (!is_curly && locals->child.value != 0) {
        const sxml_ast_node_index child = locals->child;
        locals->child = next_of(state, child);
        const size_t atom_index = locals->atom_index;
        locals->atom_index += 1;
        return call_atom(machine, (atom_call){.node = child,
                                              .block = frame->block,
                                              .atom = &locals->atoms[atom_index]});
    }
    sxml_ir_atom_list atoms = {0};
    sxml_status status = sxml_ir_unit_add_atoms(
        state->unit,
        is_curly ? (sxml_ir_atom_view){.atoms = &locals->content, .atom_count = 1}
                 : (sxml_ir_atom_view){.atoms = locals->atoms,
                                       .atom_count = locals->atom_count},
        &atoms);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_expression expression = {
        .expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE, .sequence = atoms};
    if (kind != SXML_AST_KIND_SEQUENCE_EXPRESSION) {
        expression = (sxml_ir_expression){
            .expression_kind = SXML_IR_EXPRESSION_KIND_ARRAY_CONSTRUCTOR,
            .array_constructor = {.array_form = is_curly ? SXML_IR_ARRAY_FORM_CURLY
                                                         : SXML_IR_ARRAY_FORM_SQUARE,
                                  .members = atoms}};
    }
    sxml_ir_expression_index result = {0};
    status = add_expression(
        state, (expression_to_add){.node = frame->node, .expression = expression},
        &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_frame(machine, frame, result);
}

/* "map { K: V, ... }" (XPath 4.0, section 4.7.1). */
typedef struct map_constructor_locals {
    /* The next entry and its index. */
    sxml_ast_node_index entry;
    size_t entry_index;
    size_t entry_count;
    /* entry_count entries (owned memory 0). */
    sxml_ir_map_entry *entries;
} map_constructor_locals;

static sxml_status continue_map_constructor(lowering_machine *machine,
                                            lowering_frame *frame) {
    lowerer *state = machine->state;
    map_constructor_locals *locals = frame->locals;
    switch (frame->step) {
    case 0: {
        frame->step = 1;
        locals->entry = child_of(state, frame->node, SXML_AST_FIELD_ENTRY);
        for (sxml_ast_node_index entry = locals->entry; entry.value != 0;
             entry = next_of(state, entry)) {
            locals->entry_count += 1;
        }
        const sxml_status status = allocate_owned(
            machine, frame,
            (owned_request){
                .memory_index = 0,
                .scratch = {.element_count = locals->entry_count,
                            .element_size = sizeof(sxml_ir_map_entry),
                            .element_alignment = alignof(sxml_ir_map_entry)}});
        locals->entries = frame->owned_memory[0];
        return status;
    }
    case 1:
        if (locals->entry.value != 0) {
            frame->step = 2;
            sxml_ir_map_entry *entry = &locals->entries[locals->entry_index];
            *entry = (sxml_ir_map_entry){{0}, {0}};
            return call_atom(
                machine,
                (atom_call){.node = child_of(state, locals->entry, SXML_AST_FIELD_KEY),
                            .block = frame->block,
                            .atom = &entry->key});
        }
        break;
    default: {
        frame->step = 1;
        const sxml_ast_node_index value =
            child_of(state, locals->entry, SXML_AST_FIELD_VALUE);
        sxml_ir_map_entry *entry = &locals->entries[locals->entry_index];
        locals->entry = next_of(state, locals->entry);
        locals->entry_index += 1;
        if (value.value == 0) {
            return SXML_STATUS_OK;
        }
        return call_atom(machine, (atom_call){.node = value,
                                              .block = frame->block,
                                              .atom = &entry->value});
    }
    }
    sxml_ir_map_entry_list list = {0};
    sxml_status status = sxml_ir_unit_add_map_entries(
        state->unit,
        (sxml_ir_map_entry_view){.map_entries = locals->entries,
                                 .map_entry_count = locals->entry_count},
        &list);
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        status = add_expression(
            state,
            (expression_to_add){
                .node = frame->node,
                .expression = {
                    .expression_kind = SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR,
                    .map_constructor = {.entries = list}}},
            &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_frame(machine, frame, result);
}

/* A string template (XPath 4.0, section 4.9.3): its parts joined by
   "||", an enclosed expression's value by fn:string-join#2. */
typedef struct string_template_locals {
    /* The next part, and the one whose enclosed expression is lowered. */
    sxml_ast_node_index part;
    sxml_ast_node_index current_part;
    sxml_ir_atom result;
    bool has_result;
    sxml_ir_atom value;
    sxml_function_entry entry;
} string_template_locals;

/* Appends the atom of a part to the template's result. */
static sxml_status append_template_part(lowerer *state,
                                        lowering_frame *frame,
                                        sxml_ir_atom part_atom) {
    string_template_locals *locals = frame->locals;
    if (!locals->has_result) {
        locals->result = part_atom;
        locals->has_result = true;
        return SXML_STATUS_OK;
    }
    sxml_ir_expression_index concatenation = {0};
    sxml_status status = add_expression(
        state,
        (expression_to_add){
            .node = locals->current_part,
            .expression = {
                .expression_kind = SXML_IR_EXPRESSION_KIND_BINARY,
                .binary = {.operator = SXML_IR_BINARY_OPERATOR_CONCATENATE,
                           .left_operand = locals->result,
                           .right_operand = part_atom}}},
        &concatenation);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind_expression(
        state,
        (expression_in_block){.block = frame->block, .expression = concatenation},
        &locals->result);
}

/* The enclosed expression's value joined with spaces, appended. */
static sxml_status append_joined_part(lowering_machine *machine,
                                      lowering_frame *frame) {
    lowerer *state = machine->state;
    string_template_locals *locals = frame->locals;
    call_argument arguments[2] = {{.is_placeholder = false, .atom = locals->value},
                                  {.is_placeholder = false, .atom = {0}}};
    sxml_status status = add_string_constant(
        state, SXML_BYTE_VIEW_FROM_LITERAL(" "), &arguments[1].atom);
    sxml_ir_expression_index joined = {0};
    if (status == SXML_STATUS_OK) {
        const prepared_call call = {.node = locals->current_part,
                                    .entry = locals->entry,
                                    .arguments = arguments,
                                    .argument_count = 2,
                                    .placeholder_count = 0};
        status = emit_call(state,
                           (call_emission){.block = frame->block,
                                           .call = &call,
                                           .coerces_arguments = true},
                           &joined);
    }
    sxml_ir_atom part_atom = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_expression(
            state, (expression_in_block){.block = frame->block, .expression = joined},
            &part_atom);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return append_template_part(state, frame, part_atom);
}

static sxml_status continue_string_template(lowering_machine *machine,
                                            lowering_frame *frame) {
    lowerer *state = machine->state;
    string_template_locals *locals = frame->locals;
    if (frame->step == 0) {
        frame->step = 1;
        locals->part = child_of(state, frame->node, SXML_AST_FIELD_PART);
    } else if (frame->step == 2) {
        frame->step = 1;
        const sxml_status status = append_joined_part(machine, frame);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    while (locals->part.value != 0) {
        const sxml_ast_node_index template_part = locals->part;
        locals->current_part = template_part;
        locals->part = next_of(state, template_part);
        sxml_status status = SXML_STATUS_OK;
        if (node_of(state, template_part)->kind ==
            SXML_AST_KIND_STRING_TEMPLATE_FIXED_PART) {
            sxml_ir_atom part_atom = {0};
            status = add_string_constant(state, text_of(state, template_part),
                                         &part_atom);
            if (status == SXML_STATUS_OK) {
                status = append_template_part(state, frame, part_atom);
            }
            if (status != SXML_STATUS_OK) {
                return status;
            }
            continue;
        }
        /* XPath 4.0, section 4.9.3: an enclosed expression's value is
           atomized and joined with spaces. */
        status = find_builtin_function(
            state,
            (builtin_function_request){
                .node = template_part,
                .local_name = SXML_BYTE_VIEW_FROM_LITERAL("string-join"),
                .arity = 2},
            &locals->entry);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        const sxml_ast_node_index enclosed =
            child_of(state, template_part, SXML_AST_FIELD_EXPRESSION);
        frame->step = 2;
        if (enclosed.value != 0) {
            return call_atom(machine, (atom_call){.node = enclosed,
                                                  .block = frame->block,
                                                  .atom = &locals->value});
        }
        sxml_ir_expression_index empty = {0};
        status = add_empty_sequence(state, template_part, &empty);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return bind_expression(
            state, (expression_in_block){.block = frame->block, .expression = empty},
            &locals->value);
    }
    sxml_status status = SXML_STATUS_OK;
    if (!locals->has_result) {
        status = add_string_constant(state, (sxml_byte_view){0}, &locals->result);
    }
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        status = add_atom_expression(
            state, (atom_at){.node = frame->node, .atom = locals->result}, &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_frame(machine, frame, result);
}

/* A type test or cast: INSTANCE_OF, TREAT, CAST or CASTABLE. */
typedef struct typed_expression_locals {
    sxml_ir_atom value;
} typed_expression_locals;

static sxml_status continue_typed_expression(lowering_machine *machine,
                                             lowering_frame *frame) {
    lowerer *state = machine->state;
    typed_expression_locals *locals = frame->locals;
    const sxml_ast_node_index node = frame->node;
    if (frame->step == 0) {
        frame->step = 1;
        return call_atom(
            machine,
            (atom_call){.node = child_of(state, node, SXML_AST_FIELD_VALUE),
                        .block = frame->block,
                        .atom = &locals->value});
    }
    const sxml_ast_node_index type_node = child_of(state, node, SXML_AST_FIELD_TYPE);
    sxml_ir_expression expression = {0};
    sxml_status status = SXML_STATUS_OK;
    switch ((uint32_t)node_of(state, node)->kind) {
    case SXML_AST_KIND_INSTANCE_OF_EXPRESSION:
    case SXML_AST_KIND_TREAT_EXPRESSION: {
        sxml_sequence_type sequence_type = {0};
        status = resolve_sequence_type(state, type_node, &sequence_type);
        expression = (sxml_ir_expression){
            .expression_kind =
                node_of(state, node)->kind == SXML_AST_KIND_INSTANCE_OF_EXPRESSION
                    ? SXML_IR_EXPRESSION_KIND_INSTANCE_OF
                    : SXML_IR_EXPRESSION_KIND_TREAT,
            .type_test = {.value = locals->value, .sequence_type = sequence_type}};
        break;
    }
    default: {
        sxml_type_identifier target_type = {0};
        status = resolve_cast_target(state, type_node, &target_type);
        expression = (sxml_ir_expression){
            .expression_kind =
                node_of(state, node)->kind == SXML_AST_KIND_CAST_EXPRESSION
                    ? SXML_IR_EXPRESSION_KIND_CAST
                    : SXML_IR_EXPRESSION_KIND_CASTABLE,
            .cast = {.value = locals->value,
                     .target_type = target_type,
                     .allows_empty = (node_of(state, type_node)->flags &
                                      SXML_AST_FLAG_OPTIONAL) != 0}};
        break;
    }
    }
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        status = add_expression(
            state, (expression_to_add){.node = node, .expression = expression},
            &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_frame(machine, frame, result);
}

/* "if (C) then T else E" (XPath 4.0, section 4.14.1). */
typedef struct if_locals {
    sxml_ir_atom condition;
    sxml_ir_expression_index then_branch;
    sxml_ir_expression_index else_branch;
} if_locals;

static sxml_status continue_if(lowering_machine *machine,
                               lowering_frame *frame) {
    lowerer *state = machine->state;
    if_locals *locals = frame->locals;
    const sxml_ast_node_index node = frame->node;
    switch (frame->step) {
    case 0:
        frame->step = 1;
        return call_atom(
            machine,
            (atom_call){.node = child_of(state, node, SXML_AST_FIELD_CONDITION),
                        .block = frame->block,
                        .atom = &locals->condition});
    case 1:
        frame->step = 2;
        return call_body(machine, child_of(state, node, SXML_AST_FIELD_THEN),
                         &locals->then_branch);
    case 2: {
        frame->step = 3;
        const sxml_ast_node_index else_node =
            child_of(state, node, SXML_AST_FIELD_ELSE);
        if (else_node.value == 0) {
            return add_empty_sequence(state, node, &locals->else_branch);
        }
        return call_body(machine, else_node, &locals->else_branch);
    }
    default: {
        sxml_ir_expression_index result = {0};
        const sxml_status status = add_conditional(
            state,
            (conditional_request){.node = node,
                                  .condition = locals->condition,
                                  .then_branch = locals->then_branch,
                                  .else_branch = locals->else_branch},
            &result);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return finish_frame(machine, frame, result);
    }
    }
}

/* "-E" and "+E" (XPath 4.0, section 4.9). */
typedef struct unary_locals {
    sxml_ir_atom operand;
} unary_locals;

static sxml_status continue_unary(lowering_machine *machine,
                                  lowering_frame *frame) {
    lowerer *state = machine->state;
    unary_locals *locals = frame->locals;
    const sxml_ast_node_index node = frame->node;
    if (frame->step == 0) {
        frame->step = 1;
        return call_atom(
            machine,
            (atom_call){.node = child_of(state, node, SXML_AST_FIELD_OPERAND),
                        .block = frame->block,
                        .atom = &locals->operand});
    }
    const bool is_minus =
        node_of(state, node)->detail.operator == SXML_AST_OPERATOR_MINUS;
    sxml_ir_expression_index result = {0};
    const sxml_status status = add_expression(
        state,
        (expression_to_add){
            .node = node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_UNARY,
                           .unary = {.operator = is_minus
                                                     ? SXML_IR_UNARY_OPERATOR_MINUS
                                                     : SXML_IR_UNARY_OPERATOR_PLUS,
                                     .operand = locals->operand}}},
        &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_frame(machine, frame, result);
}

/* --------------------------------------------------- Dispatch and run */

static bool procedure_of(sxml_ast_kind kind, lowering_procedure *procedure,
                         size_t *locals_size) {
    if (chain_field(kind) != SXML_AST_FIELD_NONE) {
        *procedure = SXML_LOWERING_PROCEDURE_CHAIN;
        *locals_size = sizeof(chain_locals);
        return true;
    }
    switch ((uint32_t)kind) {
    case SXML_AST_KIND_SEQUENCE_EXPRESSION:
    case SXML_AST_KIND_SQUARE_ARRAY_CONSTRUCTOR:
    case SXML_AST_KIND_CURLY_ARRAY_CONSTRUCTOR:
        *procedure = SXML_LOWERING_PROCEDURE_ATOM_LIST;
        *locals_size = sizeof(atom_list_locals);
        return true;
    case SXML_AST_KIND_FLWOR_EXPRESSION:
    case SXML_AST_KIND_QUANTIFIED_EXPRESSION:
        *procedure = SXML_LOWERING_PROCEDURE_BINDING;
        *locals_size = sizeof(binding_locals);
        return true;
    case SXML_AST_KIND_IF_EXPRESSION:
        *procedure = SXML_LOWERING_PROCEDURE_IF;
        *locals_size = sizeof(if_locals);
        return true;
    case SXML_AST_KIND_INSTANCE_OF_EXPRESSION:
    case SXML_AST_KIND_TREAT_EXPRESSION:
    case SXML_AST_KIND_CAST_EXPRESSION:
    case SXML_AST_KIND_CASTABLE_EXPRESSION:
        *procedure = SXML_LOWERING_PROCEDURE_TYPED_EXPRESSION;
        *locals_size = sizeof(typed_expression_locals);
        return true;
    case SXML_AST_KIND_UNARY_EXPRESSION:
        *procedure = SXML_LOWERING_PROCEDURE_UNARY;
        *locals_size = sizeof(unary_locals);
        return true;
    case SXML_AST_KIND_ABSOLUTE_PATH_EXPRESSION:
        *procedure = SXML_LOWERING_PROCEDURE_ABSOLUTE_PATH;
        *locals_size = sizeof(absolute_path_locals);
        return true;
    case SXML_AST_KIND_AXIS_STEP:
        *procedure = SXML_LOWERING_PROCEDURE_AXIS_STEP;
        *locals_size = sizeof(axis_step_locals);
        return true;
    case SXML_AST_KIND_UNARY_LOOKUP:
        *procedure = SXML_LOWERING_PROCEDURE_LOOKUP;
        *locals_size = sizeof(lookup_locals);
        return true;
    case SXML_AST_KIND_FUNCTION_CALL:
        *procedure = SXML_LOWERING_PROCEDURE_FUNCTION_CALL;
        *locals_size = sizeof(function_call_locals);
        return true;
    case SXML_AST_KIND_INLINE_FUNCTION_EXPRESSION:
        *procedure = SXML_LOWERING_PROCEDURE_INLINE_FUNCTION;
        *locals_size = sizeof(inline_function_locals);
        return true;
    case SXML_AST_KIND_MAP_CONSTRUCTOR:
        *procedure = SXML_LOWERING_PROCEDURE_MAP_CONSTRUCTOR;
        *locals_size = sizeof(map_constructor_locals);
        return true;
    case SXML_AST_KIND_STRING_TEMPLATE:
        *procedure = SXML_LOWERING_PROCEDURE_STRING_TEMPLATE;
        *locals_size = sizeof(string_template_locals);
        return true;
    default:
        return false;
    }
}

static sxml_status lower_leaf_expression(lowerer *state, node_in_block request,
                                         sxml_ir_expression_index *expression) {
    const sxml_ast_node_index node = request.node;
    const sxml_ast_kind kind = node_of(state, node)->kind;
    if (is_atom_kind(kind)) {
        return lower_atom_expression(state, request, expression);
    }
    switch ((uint32_t)kind) {
    case SXML_AST_KIND_PARENTHESIZED_EXPRESSION:
    case SXML_AST_KIND_ENCLOSED_EXPRESSION:
        /* "()" (call_expression lowers the inner expression of others). */
        return add_empty_sequence(state, node, expression);
    case SXML_AST_KIND_CONTEXT_VALUE_REFERENCE:
        return lower_context_value(state, request, expression);
    case SXML_AST_KIND_NAMED_FUNCTION_REFERENCE:
        return lower_named_function_reference(state, request, expression);
    case SXML_AST_KIND_BOOLEAN_CONSTANT:
        return add_shared_constant_expression(
            state,
            (shared_constant_at){
                .node = node,
                .which = (node_of(state, node)->flags & SXML_AST_FLAG_TRUE) != 0
                             ? SXML_LOWERING_SHARED_CONSTANT_TRUE
                             : SXML_LOWERING_SHARED_CONSTANT_FALSE},
            expression);
    default:
        return report_unsupported(state, node,
                                  SXML_BYTE_VIEW_FROM_LITERAL("this construct is not lowered in Phase 1"));
    }
}

/* Runs one step of frame. */
static sxml_status continue_frame(lowering_machine *machine,
                                  lowering_frame *frame) {
    switch (frame->procedure) {
    case SXML_LOWERING_PROCEDURE_CHAIN:
        return continue_chain(machine, frame);
    case SXML_LOWERING_PROCEDURE_BOOLEAN_LINK:
        return continue_boolean_link(machine, frame);
    case SXML_LOWERING_PROCEDURE_OTHERWISE_LINK:
        return continue_otherwise_link(machine, frame);
    case SXML_LOWERING_PROCEDURE_BINARY_LINK:
        return continue_binary_link(machine, frame);
    case SXML_LOWERING_PROCEDURE_PIPELINE_LINK:
        return continue_pipeline_link(machine, frame);
    case SXML_LOWERING_PROCEDURE_SIMPLE_MAP_LINK:
        return continue_simple_map_link(machine, frame);
    case SXML_LOWERING_PROCEDURE_ARROW_MAP:
        return continue_arrow_map(machine, frame);
    case SXML_LOWERING_PROCEDURE_PATH_MAP:
        return continue_path_map(machine, frame);
    case SXML_LOWERING_PROCEDURE_FILTER:
        return continue_filter(machine, frame);
    case SXML_LOWERING_PROCEDURE_AXIS_STEP:
        return continue_axis_step(machine, frame);
    case SXML_LOWERING_PROCEDURE_ABSOLUTE_PATH:
        return continue_absolute_path(machine, frame);
    case SXML_LOWERING_PROCEDURE_LOOKUP:
        return continue_lookup(machine, frame);
    case SXML_LOWERING_PROCEDURE_FOCUSED_BODY:
        return continue_focused_body(machine, frame);
    case SXML_LOWERING_PROCEDURE_FUNCTION_CALL:
        return continue_function_call(machine, frame);
    case SXML_LOWERING_PROCEDURE_DYNAMIC_CALL:
        return continue_dynamic_call(machine, frame);
    case SXML_LOWERING_PROCEDURE_INLINE_FUNCTION:
        return continue_inline_function(machine, frame);
    case SXML_LOWERING_PROCEDURE_BINDING:
        return continue_binding(machine, frame);
    case SXML_LOWERING_PROCEDURE_ATOM_LIST:
        return continue_atom_list(machine, frame);
    case SXML_LOWERING_PROCEDURE_MAP_CONSTRUCTOR:
        return continue_map_constructor(machine, frame);
    case SXML_LOWERING_PROCEDURE_STRING_TEMPLATE:
        return continue_string_template(machine, frame);
    case SXML_LOWERING_PROCEDURE_TYPED_EXPRESSION:
        return continue_typed_expression(machine, frame);
    case SXML_LOWERING_PROCEDURE_IF:
        return continue_if(machine, frame);
    case SXML_LOWERING_PROCEDURE_UNARY:
        return continue_unary(machine, frame);
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* Runs the machine, after a first call whose status is status, until no
   frame is left or a step fails; then pops every frame. */
static sxml_status run_machine(lowering_machine *machine, sxml_status status) {
    while (status == SXML_STATUS_OK && machine->top != NULL) {
        lowering_frame *frame = machine->top;
        if (frame->is_finished) {
            pop_frame(machine);
            continue;
        }
        status = continue_frame(machine, frame);
    }
    while (machine->top != NULL) {
        pop_frame(machine);
    }
    return status;
}

/* Lowers node as a body of the current function. */
static sxml_status lower_body(lowerer *state, sxml_ast_node_index node,
                              sxml_ir_expression_index *expression) {
    lowering_machine machine = {.state = state, .top = NULL};
    return run_machine(&machine, call_body(&machine, node, expression));
}

/* --------------------------------------------------------- Entry points */

/* Checks the static context against the rules of
   sxml_static_context_configuration. */
static sxml_status check_static_context(const sxml_xpath_static_context *context) {
    if (context->item_heap == NULL || sxml_allocator_is_null(context->allocator)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_name_pool *name_pool = sxml_item_heap_get_name_pool(context->item_heap);
    const sxml_namespace_binding_view bindings = context->namespace_bindings;
    for (size_t binding_index = 0;
         binding_index < bindings.namespace_binding_count; binding_index += 1) {
        const sxml_namespace_binding *binding =
            &bindings.namespace_bindings[binding_index];
        sxml_byte_view prefix = {0};
        if (sxml_name_pool_get_namespace_prefix(name_pool,
                                                binding->namespace_prefix,
                                                &prefix) != SXML_STATUS_OK ||
            (binding->namespace_uri.index == 0 &&
             binding->namespace_prefix.index != 0) ||
            sxml_byte_view_equals(prefix, SXML_BYTE_VIEW_FROM_LITERAL("xml"))) {
            return SXML_STATUS_INVALID_ARGUMENT;
        }
        for (size_t earlier_index = 0; earlier_index < binding_index;
             earlier_index += 1) {
            if (bindings.namespace_bindings[earlier_index].namespace_prefix.index ==
                binding->namespace_prefix.index) {
                return SXML_STATUS_INVALID_ARGUMENT;
            }
        }
    }
    const sxml_variable_declaration_view declarations =
        context->variable_declarations;
    for (size_t declaration_index = 0;
         declaration_index < declarations.variable_declaration_count;
         declaration_index += 1) {
        for (size_t earlier_index = 0; earlier_index < declaration_index;
             earlier_index += 1) {
            if (declarations.variable_declarations[earlier_index]
                    .variable_name.index ==
                declarations.variable_declarations[declaration_index]
                    .variable_name.index) {
                return SXML_STATUS_INVALID_ARGUMENT;
            }
        }
    }
    return SXML_STATUS_OK;
}

static sxml_status lower_main_function(lowerer *state) {
    const sxml_variable_declaration_view declarations =
        state->static_context->variable_declarations;
    sxml_status status = SXML_STATUS_OK;
    for (size_t declaration_index = 0;
         status == SXML_STATUS_OK &&
         declaration_index < declarations.variable_declaration_count;
         declaration_index += 1) {
        const sxml_variable_declaration *declaration =
            &declarations.variable_declarations[declaration_index];
        sxml_ir_global_index global = {0};
        status = sxml_ir_unit_add_global_variable(
            state->unit,
            &(sxml_ir_global_variable){.variable_name = declaration->variable_name,
                                       .declared_type = declaration->declared_type,
                                       .initializer = {.value = 0}},
            &global);
    }
    /* The main function: no parameters, any result. */
    sxml_type_identifier signature = {0};
    if (status == SXML_STATUS_OK) {
        status = register_signature(
            state,
            (signature_request){
                .parameter_types = {.sequence_types = NULL,
                                    .sequence_type_count = 0},
                .result_type = {.item_type =
                                    sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                                .occurrence = SXML_OCCURRENCE_ZERO_OR_MORE}},
            &signature);
    }
    sxml_ir_function_index main_function = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_function(
            state->unit,
            &(sxml_ir_function){.signature = signature,
                                .source_span = span_of(state, state->ast->root)},
            &main_function);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    function_frame frame = {
        .function = {.function_index = main_function,
                     .function_level = 0,
                     .slot_count = 0,
                     .captures = NULL,
                     .capture_count = 0},
        .focus = {.kind = SXML_LOWERING_FOCUS_KIND_SLOTS},
    };
    enter_function(state, &frame);
    sxml_ir_expression_index body = {0};
    status = lower_body(state, state->ast->root, &body);
    leave_function(state, &frame);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_function_replacement replacement = {
        .index = main_function,
        .function = *sxml_ir_unit_get_function(state->unit, main_function)};
    replacement.function.body = body;
    replacement.function.slot_count = frame.function.slot_count;
    replacement.function.focus = frame.focus.slots;
    return sxml_ir_unit_replace_function(state->unit, &replacement);
}

sxml_status sxml_xpath_lower(const sxml_xpath_lowering_request *request,
                             sxml_ir_unit **unit) {
    if (request == NULL || request->ast == NULL ||
        request->static_context == NULL ||
        sxml_ast_get_node(request->ast, request->ast->root)->kind !=
            SXML_AST_KIND_XPATH) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_xpath_static_context *context = request->static_context;
    sxml_status status = check_static_context(context);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_unit *new_unit = NULL;
    status = sxml_ir_unit_create(
        context->allocator,
        &(sxml_ir_unit_configuration){
            .allocator = context->allocator,
            .item_heap = context->item_heap,
            .source_name = request->ast->source_name,
            .maximum_expression_count = request->maximum_expression_count},
        &new_unit);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    lowerer state = {
        .ast = request->ast,
        .static_context = context,
        .error_record = request->error_record,
        .unit = new_unit,
        .allocator = context->allocator,
        .name_pool = sxml_item_heap_get_name_pool(context->item_heap),
        .type_registry = sxml_item_heap_get_type_registry(context->item_heap),
        .item_heap = context->item_heap,
        .depth = 0,
        .maximum_depth = request->maximum_expression_depth == 0
                             ? SXML_XPATH_DEFAULT_MAXIMUM_EXPRESSION_DEPTH
                             : request->maximum_expression_depth,
    };
    status = lower_main_function(&state);
    release_scratch(&state, state.scope_entries);
    release_scratch(&state, state.has_variable_reference);
    if (status == SXML_STATUS_OK) {
        /* For casts to xs:QName at run time (D84). */
        sxml_xpath_namespace_collection namespaces = {.bindings = NULL, .binding_count = 0};
        status = sxml_xpath_collect_namespaces(request->ast, context, &namespaces);
        if (status == SXML_STATUS_OK) {
            status = sxml_ir_unit_set_static_namespaces(
                new_unit,
                (sxml_namespace_binding_view){.namespace_bindings = namespaces.bindings,
                                              .namespace_binding_count =
                                                  namespaces.binding_count});
            if (namespaces.bindings != NULL) {
                sxml_allocator_release(context->allocator, namespaces.bindings);
            }
        }
    }
    if (status != SXML_STATUS_OK) {
        sxml_ir_unit_release(new_unit);
        return status;
    }
    *unit = new_unit;
    return SXML_STATUS_OK;
}

sxml_status sxml_xpath_compile(const sxml_xpath_compile_request *request,
                               sxml_ir_unit **unit) {
    if (request == NULL || request->static_context == NULL) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_ast *ast = NULL;
    sxml_status status = sxml_xpath_build_ast(
        &(sxml_ast_build_request){
            .expression_text = request->expression_text,
            .source_name = request->source_name,
            .allocator = request->static_context->allocator,
            .maximum_expression_depth = request->maximum_expression_depth,
            .error_record = request->error_record},
        &ast);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_unit *new_unit = NULL;
    status = sxml_xpath_lower(
        &(sxml_xpath_lowering_request){
            .ast = ast,
            .static_context = request->static_context,
            .maximum_expression_count = request->maximum_expression_count,
            .maximum_expression_depth = request->maximum_expression_depth,
            .error_record = request->error_record},
        &new_unit);
    sxml_ast_destroy(ast);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_diagnostic diagnostic = {0};
    status = sxml_ir_run_passes(&(sxml_ir_pass_list_request){
        .pass_request = {.unit = new_unit,
                         .type_registry = sxml_item_heap_get_type_registry(
                             request->static_context->item_heap),
                         .error_record = request->error_record,
                         .diagnostic = &diagnostic},
        .passes = sxml_ir_standard_passes,
        .verifies_after_each_pass = false});
    if (status != SXML_STATUS_OK) {
        if (status == SXML_STATUS_INVALID_ARGUMENT &&
            request->error_record != NULL) {
            request->error_record->diagnostic = diagnostic;
        }
        sxml_ir_unit_release(new_unit);
        return status;
    }
    sxml_ir_unit_seal(new_unit);
    *unit = new_unit;
    return SXML_STATUS_OK;
}
