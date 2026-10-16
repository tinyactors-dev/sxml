/*
 * Lowering of XPath 4.0 to the shared IR, and compilation
 * (src/xpath_static_internal.h; src/ir_internal.h documents the IR each
 * construct lowers to; docs/DECISIONS.md, D46, D55, D63).
 *
 * Golden file: src/memory.c.
 *
 * Administrative normal form. Every operand of an IR expression is an
 * atom, so an operand that is not a literal, a variable or the focus is
 * computed first and bound by LET. A lowering_block collects those LETs
 * while one expression is lowered: binding appends a LET whose body is
 * filled later (the next LET, or the block's result), so the LET chain
 * nests in evaluation order. Branches, loop bodies and function bodies are
 * blocks of their own.
 *
 * Chains. The grammar makes binary operators, paths, filters, lookups,
 * dynamic calls and arrows left-recursive (D29), so "1 + 2 + ... + 1000"
 * is an AST a thousand nodes deep along its left operands. Lowering walks
 * such a spine in a loop: down to its first operand, then up through the
 * parent links, so the C stack grows with the nesting of other
 * constructs only, which maximum_expression_depth bounds.
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

/*
 * Keeps a function out of its callers. Lowering recurses once per level of
 * nesting, and inlining every construct's function into the recursive one
 * would give each level the frame of all of them; on the 64 KiB stack of a
 * WebAssembly module that limits nesting to a few dozen levels.
 */
#if defined(__GNUC__) || defined(__clang__)
#define LOWERING_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define LOWERING_NOINLINE __declspec(noinline)
#else
#define LOWERING_NOINLINE
#endif

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
LOWERING_NOINLINE static sxml_status add_integer_constant(lowerer *state, integer_digits request,
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

LOWERING_NOINLINE static sxml_status add_double_constant(lowerer *state, sxml_byte_view text,
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

LOWERING_NOINLINE static sxml_status lower_variable_reference(lowerer *state,
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
LOWERING_NOINLINE static sxml_status read_focus(lowerer *state, focus_read request,
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

/* The next node of a preorder walk of root's subtree, through the AST's
   links; 0 after the last. */
static sxml_ast_node_index next_in_subtree(const lowerer *state,
                                           sxml_ast_node_index root,
                                           sxml_ast_node_index node) {
    const sxml_ast_node *current = node_of(state, node);
    if (current->first_child.value != 0) {
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

LOWERING_NOINLINE static sxml_status find_free_variables(lowerer *state,
                                       free_variable_request request,
                                       free_variables *variables) {
    const sxml_ast_node_index body =
        child_of(state, request.function_node, SXML_AST_FIELD_BODY);
    sxml_status status = SXML_STATUS_OK;
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

static sxml_status lower_expression(lowerer *state, node_in_block request,
                                    sxml_ir_expression_index *expression);

static sxml_status lower_to_atom(lowerer *state, node_in_block request,
                                 sxml_ir_atom *atom);

/* Lowers node as a block of its own (a branch, a body). */
static sxml_status lower_body(lowerer *state, sxml_ast_node_index node,
                              sxml_ir_expression_index *expression) {
    lowering_block block = {{0}, {0}};
    sxml_ir_expression_index result = {0};
    sxml_status status = lower_expression(
        state, (node_in_block){.block = &block, .node = node}, &result);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_block(state, (block_result){.block = block, .result = result},
                        expression);
}

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

static sxml_status lower_focused_body(lowerer *state, focused_body request,
                                      focused_result *result) {
    lowering_focus focus = {.kind = SXML_LOWERING_FOCUS_KIND_SLOTS,
                            .slots = request.preset_slots};
    lowering_focus *outer_focus = state->function->focus;
    state->function->focus = &focus;
    sxml_ir_expression_index body = {0};
    sxml_status status = lower_body(state, request.node, &body);
    state->function->focus = outer_focus;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *result = (focused_result){.body = body, .focus = focus.slots};
    return SXML_STATUS_OK;
}

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

LOWERING_NOINLINE static sxml_status lower_qname_literal(lowerer *state, sxml_ast_node_index node,
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
LOWERING_NOINLINE static sxml_status lower_atom_node(lowerer *state, sxml_ast_node_index node,
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

static sxml_status lower_to_atom(lowerer *state, node_in_block request,
                                 sxml_ir_atom *atom) {
    const sxml_ast_kind kind = node_of(state, request.node)->kind;
    if (is_atom_kind(kind)) {
        return lower_atom_node(state, request.node, atom);
    }
    if (kind == SXML_AST_KIND_CONTEXT_VALUE_REFERENCE) {
        return read_focus(state,
                          (focus_read){.block = request.block,
                                       .node = request.node,
                                       .part = SXML_LOWERING_FOCUS_PART_VALUE},
                          atom);
    }
    sxml_ir_expression_index expression = {0};
    sxml_status status =
        lower_expression(state, request, &expression);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return bind_expression(
        state,
        (expression_in_block){.block = request.block, .expression = expression},
        atom);
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

LOWERING_NOINLINE static sxml_status coerce_atom(lowerer *state, coercion_request request,
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

LOWERING_NOINLINE static sxml_status emit_call(lowerer *state, call_emission request,
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
LOWERING_NOINLINE static sxml_status emit_partial_application(lowerer *state,
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

LOWERING_NOINLINE static sxml_status emit_prepared_call(lowerer *state, call_emission request,
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

/* A function call to prepare: FUNCTION_CALL node, and the value an arrow
   passes as the first argument (atom kind NONE when there is none). */
typedef struct call_preparation {
    lowering_block *block;
    sxml_ast_node_index call_node;
    /* For "=>": the value, first argument. For "=!>" the first argument
       is filled per item, so has_leading_argument without an atom. */
    bool has_leading_argument;
    sxml_ir_atom leading_argument;
} call_preparation;

LOWERING_NOINLINE static sxml_status prepare_call(lowerer *state, call_preparation request,
                                prepared_call *call) {
    const sxml_ast_node_index call_node = request.call_node;
    const sxml_ast_node_index list =
        child_of(state, call_node, SXML_AST_FIELD_ARGUMENTS);
    size_t positional_count = request.has_leading_argument ? 1U : 0U;
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
    void *memory = NULL;
    status = allocate_scratch(
        state,
        (scratch_request){.element_count = arity == 0 ? 1 : arity,
                          .element_size = sizeof(call_argument),
                          .element_alignment = alignof(call_argument)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    call_argument *arguments = memory;
    bool *is_supplied = NULL;
    void *supplied_memory = NULL;
    status = allocate_scratch(
        state,
        (scratch_request){.element_count = arity == 0 ? 1 : arity,
                          .element_size = sizeof(bool),
                          .element_alignment = alignof(bool)},
        &supplied_memory);
    is_supplied = supplied_memory;
    if (status == SXML_STATUS_OK) {
        memset(is_supplied, 0, (arity == 0 ? 1 : arity) * sizeof(bool));
        memset(arguments, 0, (arity == 0 ? 1 : arity) * sizeof(call_argument));
    }
    size_t placeholder_count = 0;
    size_t position = 0;
    if (status == SXML_STATUS_OK && request.has_leading_argument) {
        arguments[0] = (call_argument){.is_placeholder = false,
                                       .atom = request.leading_argument};
        is_supplied[0] = true;
        position = 1;
    }
    for (sxml_ast_node_index argument =
             child_of(state, list, SXML_AST_FIELD_ARGUMENT);
         status == SXML_STATUS_OK && argument.value != 0;
         argument = next_of(state, argument)) {
        size_t target = position;
        sxml_ast_node_index value = argument;
        if (node_of(state, argument)->kind == SXML_AST_KIND_KEYWORD_ARGUMENT) {
            target = find_parameter(
                &entry,
                sxml_ast_get_text(
                    state->ast,
                    node_of(state, child_of(state, argument, SXML_AST_FIELD_NAME))
                        ->payload.name.local_name));
            value = child_of(state, argument, SXML_AST_FIELD_VALUE);
            if (target >= arity || is_supplied[target]) {
                status = report_static_error(
                    state, argument,
                    SXML_BYTE_VIEW_FROM_LITERAL("XPST0017 a parameter is given two arguments"));
                break;
            }
        } else {
            position += 1;
        }
        is_supplied[target] = true;
        if (node_of(state, value)->kind == SXML_AST_KIND_ARGUMENT_PLACEHOLDER) {
            arguments[target].is_placeholder = true;
            placeholder_count += 1;
            continue;
        }
        status = lower_to_atom(
            state, (node_in_block){.block = request.block, .node = value},
            &arguments[target].atom);
    }
    for (size_t argument_index = 0;
         status == SXML_STATUS_OK && argument_index < arity;
         argument_index += 1) {
        if (is_supplied[argument_index]) {
            continue;
        }
        const sxml_parameter_default parameter_default =
            argument_index < entry.parameters.function_parameter_count
                ? entry.parameters.function_parameters[argument_index]
                      .parameter_default
                : SXML_PARAMETER_DEFAULT_REQUIRED;
        if (parameter_default == SXML_PARAMETER_DEFAULT_CONTEXT_VALUE) {
            status = read_focus(state,
                                (focus_read){.block = request.block,
                                             .node = call_node,
                                             .part = SXML_LOWERING_FOCUS_PART_VALUE},
                                &arguments[argument_index].atom);
        } else if (parameter_default == SXML_PARAMETER_DEFAULT_REQUIRED) {
            status = report_static_error(
                state, call_node, SXML_BYTE_VIEW_FROM_LITERAL("XPST0017 a required parameter has no argument"));
        } else {
            status = report_unsupported(
                state, call_node,
                SXML_BYTE_VIEW_FROM_LITERAL("a keyword argument after an omitted parameter whose default "
                "the function supplies"));
        }
    }
    release_scratch(state, supplied_memory);
    if (status != SXML_STATUS_OK) {
        release_scratch(state, memory);
        return status;
    }
    *call = (prepared_call){.node = call_node,
                            .entry = entry,
                            .arguments = arguments,
                            .argument_count = arity,
                            .placeholder_count = placeholder_count};
    return SXML_STATUS_OK;
}

/* Lowers a static function call (XPath 4.0, section 4.5.1), with an
   arrow's value as first argument when has_leading_argument. */
LOWERING_NOINLINE static sxml_status lower_function_call(lowerer *state, call_preparation request,
                                       sxml_ir_expression_index *expression) {
    prepared_call call = {0};
    sxml_status status = prepare_call(state, request, &call);
    if (status != SXML_STATUS_OK) {
        return status;
    }
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
                .block = request.block,
                .node = call.node,
                .part = (properties & SXML_NATIVE_FUNCTION_READS_CONTEXT_POSITION) != 0
                            ? SXML_LOWERING_FOCUS_PART_POSITION
                            : SXML_LOWERING_FOCUS_PART_SIZE},
            &atom);
        if (status == SXML_STATUS_OK) {
            status = add_atom_expression(
                state, (atom_at){.node = call.node, .atom = atom}, expression);
        }
    } else {
        status = emit_prepared_call(state,
                                    (call_emission){.block = request.block,
                                                    .call = &call,
                                                    .coerces_arguments = true},
                                    expression);
    }
    release_scratch(state, call.arguments);
    return status;
}

/* A named function reference (XPath 4.0, section 4.5.4). */
LOWERING_NOINLINE static sxml_status lower_named_function_reference(
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

/* ------------------------------------------------------- Dynamic calls */

/* A dynamic call (XPath 4.0, section 4.5.3) of function_item with the
   arguments of list (a POSITIONAL_ARGUMENT_LIST), after an arrow's
   value when has_leading_argument. With placeholders, fn:partial-apply
   with a map from positions to the supplied arguments (section 4.5.2.5). */
typedef struct dynamic_call_request {
    lowering_block *block;
    sxml_ast_node_index node;
    sxml_ir_atom function_item;
    sxml_ast_node_index list;
    bool has_leading_argument;
    sxml_ir_atom leading_argument;
} dynamic_call_request;

LOWERING_NOINLINE static sxml_status lower_dynamic_call(lowerer *state,
                                      dynamic_call_request request,
                                      sxml_ir_expression_index *expression) {
    size_t argument_count = request.has_leading_argument ? 1U : 0U;
    size_t placeholder_count = 0;
    for (sxml_ast_node_index argument =
             child_of(state, request.list, SXML_AST_FIELD_ARGUMENT);
         argument.value != 0; argument = next_of(state, argument)) {
        argument_count += 1;
        if (node_of(state, argument)->kind ==
            SXML_AST_KIND_ARGUMENT_PLACEHOLDER) {
            placeholder_count += 1;
        }
    }
    void *atoms_memory = NULL;
    void *entries_memory = NULL;
    sxml_status status = allocate_scratch(
        state,
        (scratch_request){.element_count = argument_count,
                          .element_size = sizeof(sxml_ir_atom),
                          .element_alignment = alignof(sxml_ir_atom)},
        &atoms_memory);
    if (status == SXML_STATUS_OK && placeholder_count > 0) {
        status = allocate_scratch(
            state,
            (scratch_request){.element_count = argument_count,
                              .element_size = sizeof(sxml_ir_map_entry),
                              .element_alignment = alignof(sxml_ir_map_entry)},
            &entries_memory);
    }
    sxml_ir_atom *atoms = atoms_memory;
    sxml_ir_map_entry *entries = entries_memory;
    size_t argument_index = 0;
    size_t entry_count = 0;
    if (status == SXML_STATUS_OK && request.has_leading_argument) {
        atoms[0] = request.leading_argument;
        argument_index = 1;
    }
    for (sxml_ast_node_index argument =
             child_of(state, request.list, SXML_AST_FIELD_ARGUMENT);
         status == SXML_STATUS_OK && argument.value != 0;
         argument = next_of(state, argument)) {
        if (node_of(state, argument)->kind ==
            SXML_AST_KIND_ARGUMENT_PLACEHOLDER) {
            atoms[argument_index] =
                (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_NONE};
            argument_index += 1;
            continue;
        }
        status = lower_to_atom(
            state, (node_in_block){.block = request.block, .node = argument},
            &atoms[argument_index]);
        argument_index += 1;
    }
    if (status == SXML_STATUS_OK && placeholder_count > 0) {
        /* Positions of the supplied arguments as xs:positiveInteger keys,
           the key type of fn:partial-apply's map (F&O 4.0). */
        for (size_t position = 0;
             status == SXML_STATUS_OK && position < argument_count;
             position += 1) {
            const bool is_placeholder =
                atoms[position].atom_kind == SXML_IR_ATOM_KIND_NONE;
            if (is_placeholder) {
                continue;
            }
            sxml_ir_atom key = {0};
            status = add_atomic_constant(
                state,
                (atomic_constant){
                    .type = sxml_builtin_type_identifier(
                        SXML_TYPE_XS_POSITIVE_INTEGER),
                    .datum = {.datum_kind = SXML_DATUM_KIND_INTEGER,
                              .datum_details = {.integer =
                                                    (int64_t)position + 1}}},
                &key);
            entries[entry_count] =
                (sxml_ir_map_entry){.key = key, .value = atoms[position]};
            entry_count += 1;
        }
    }
    if (status == SXML_STATUS_OK && placeholder_count > 0) {
        sxml_ir_map_entry_list entry_list = {0};
        status = sxml_ir_unit_add_map_entries(
            state->unit,
            (sxml_ir_map_entry_view){.map_entries = entries,
                                     .map_entry_count = entry_count},
            &entry_list);
        sxml_ir_expression_index map = {0};
        if (status == SXML_STATUS_OK) {
            status = add_expression(
                state,
                (expression_to_add){
                    .node = request.node,
                    .expression = {
                        .expression_kind = SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR,
                        .map_constructor = {.entries = entry_list}}},
                &map);
        }
        call_argument call_arguments[2] = {
            {.is_placeholder = false, .atom = request.function_item},
            {.is_placeholder = false, .atom = {0}}};
        if (status == SXML_STATUS_OK) {
            status = bind_expression(
                state,
                (expression_in_block){.block = request.block, .expression = map},
                &call_arguments[1].atom);
        }
        sxml_function_entry entry = {0};
        if (status == SXML_STATUS_OK) {
            status = find_builtin_function(
                state,
                (builtin_function_request){
                    .node = request.node,
                    .local_name = SXML_BYTE_VIEW_FROM_LITERAL("partial-apply"),
                    .arity = 2},
                &entry);
        }
        if (status == SXML_STATUS_OK) {
            const prepared_call call = {.node = request.node,
                                        .entry = entry,
                                        .arguments = call_arguments,
                                        .argument_count = 2,
                                        .placeholder_count = 0};
            status = emit_call(state,
                               (call_emission){.block = request.block,
                                               .call = &call,
                                               .coerces_arguments = false},
                               expression);
        }
    } else if (status == SXML_STATUS_OK) {
        sxml_ir_atom_list arguments = {0};
        status = sxml_ir_unit_add_atoms(
            state->unit,
            (sxml_ir_atom_view){.atoms = atoms, .atom_count = argument_count},
            &arguments);
        if (status == SXML_STATUS_OK) {
            status = add_expression(
                state,
                (expression_to_add){
                    .node = request.node,
                    .expression = {
                        .expression_kind = SXML_IR_EXPRESSION_KIND_DYNAMIC_CALL,
                        .dynamic_call = {.function_item = request.function_item,
                                         .arguments = arguments}}},
                expression);
        }
    }
    release_scratch(state, entries_memory);
    release_scratch(state, atoms_memory);
    return status;
}

/* ----------------------------------------------------- Inline functions */

LOWERING_NOINLINE static sxml_status lower_inline_function(lowerer *state,
                                         sxml_ast_node_index node,
                                         sxml_ir_expression_index *expression) {
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
    void *types_memory = NULL;
    void *names_memory = NULL;
    sxml_status status = allocate_scratch(
        state,
        (scratch_request){.element_count = parameter_count,
                          .element_size = sizeof(sxml_sequence_type),
                          .element_alignment = alignof(sxml_sequence_type)},
        &types_memory);
    if (status == SXML_STATUS_OK) {
        status = allocate_scratch(
            state,
            (scratch_request){.element_count = parameter_count,
                              .element_size = sizeof(sxml_name_identifier),
                              .element_alignment = alignof(sxml_name_identifier)},
            &names_memory);
    }
    sxml_sequence_type *parameter_types = types_memory;
    sxml_name_identifier *parameter_names = names_memory;
    if (status == SXML_STATUS_OK && !has_signature) {
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
    sxml_sequence_type result_type = {0};
    if (status == SXML_STATUS_OK) {
        status = resolve_declared_type(
            state, child_of(state, node, SXML_AST_FIELD_RETURN_TYPE),
            &result_type);
    }
    sxml_type_identifier signature = {0};
    if (status == SXML_STATUS_OK) {
        status = register_signature(
            state,
            (signature_request){
                .parameter_types = {.sequence_types = parameter_types,
                                    .sequence_type_count = parameter_count},
                .result_type = result_type},
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
    sxml_ir_function_index function = {0};
    if (status == SXML_STATUS_OK) {
        status = add_closure_function(
            state,
            (closure_request){
                .node = node,
                .signature = signature,
                .parameter_count = (uint32_t)parameter_count,
                .captured_atoms = {.atoms = variables.outer_atoms,
                                   .atom_count = variables.captured_count}},
            &function);
    }
    if (status == SXML_STATUS_OK) {
        function_frame frame = {
            .function = {.function_index = function,
                         .function_level = state->function->function_level + 1U,
                         .slot_count = (uint32_t)(parameter_count +
                                                  variables.captured_count),
                         .captures = variables.captures,
                         .capture_count = variables.capture_count},
            .focus = {.kind = has_signature ? SXML_LOWERING_FOCUS_KIND_ABSENT
                                            : SXML_LOWERING_FOCUS_KIND_FIXED,
                      .fixed_value = slot_atom((sxml_ir_slot){.value = 1})},
        };
        enter_function(state, &frame);
        for (size_t name_index = 0;
             status == SXML_STATUS_OK && has_signature &&
             name_index < parameter_count;
             name_index += 1) {
            status = push_variable(
                state,
                (scope_entry){
                    .variable_name = parameter_names[name_index],
                    .function_level = frame.function.function_level,
                    .atom = slot_atom(
                        (sxml_ir_slot){.value = (uint32_t)name_index + 1U})});
        }
        lowering_block block = {{0}, {0}};
        sxml_ir_expression_index result = {0};
        const sxml_ast_node_index body_node =
            child_of(state, node, SXML_AST_FIELD_BODY);
        if (status == SXML_STATUS_OK) {
            status = lower_expression(
                state, (node_in_block){.block = &block, .node = body_node},
                &result);
        }
        if (status == SXML_STATUS_OK && !accepts_everything(result_type)) {
            /* The declared result type is checked by coercion (XPath 4.0,
               section 4.5.6). */
            sxml_ir_atom value = {0};
            status = bind_expression(
                state, (expression_in_block){.block = &block, .expression = result},
                &value);
            if (status == SXML_STATUS_OK) {
                status = add_expression(
                    state,
                    (expression_to_add){
                        .node = body_node,
                        .expression = {
                            .expression_kind = SXML_IR_EXPRESSION_KIND_COERCE,
                            .coercion = {
                                .value = value,
                                .sequence_type = result_type,
                                .role = SXML_IR_COERCION_ROLE_FUNCTION_RESULT}}},
                    &result);
            }
        }
        sxml_ir_expression_index body = {0};
        if (status == SXML_STATUS_OK) {
            status = finish_block(
                state, (block_result){.block = block, .result = result}, &body);
        }
        leave_function(state, &frame);
        if (status == SXML_STATUS_OK) {
            sxml_ir_function_replacement replacement = {
                .index = function,
                .function = *sxml_ir_unit_get_function(state->unit, function)};
            replacement.function.body = body;
            replacement.function.slot_count = frame.function.slot_count;
            status = sxml_ir_unit_replace_function(state->unit, &replacement);
        }
    }
    release_scratch(state, variables.outer_atoms);
    release_scratch(state, variables.captures);
    release_scratch(state, names_memory);
    release_scratch(state, types_memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_expression(
        state,
        (expression_to_add){
            .node = node,
            .expression = {
                .expression_kind = SXML_IR_EXPRESSION_KIND_FUNCTION_DEFINITION,
                .function_definition = {.function = function}}},
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

LOWERING_NOINLINE static sxml_status resolve_node_test(lowerer *state, node_test_request request,
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
    case SXML_AST_KIND_UNION_NODE_TEST: {
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
            status = resolve_node_test(
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
                                           .type_identifier_count =
                                               member_count}}},
                               node_test);
            if (member_count == 1) {
                *node_test = members[0];
            }
        }
        release_scratch(state, memory);
        return status;
    }
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
    sxml_name_test name_test = {.name_test_kind = SXML_NAME_TEST_KIND_ANY_NAME};
    if (principal_kind == SXML_NODE_KIND_NAMESPACE &&
        (test->kind != SXML_AST_KIND_WILDCARD ||
         test->detail.name_test_kind != SXML_NAME_TEST_KIND_ANY_NAME)) {
        /* The type registry has no namespace node test with a name yet
           (docs/DECISIONS.md, Proposed). */
        return report_unsupported(state, request.test,
                                  SXML_BYTE_VIEW_FROM_LITERAL("a name test on the namespace axis is not "
                                  "lowered in Phase 1"));
    }
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

/* FILTER of input by the expression of predicate (XPath 4.0, 4.4.1). */
typedef struct filter_request {
    sxml_ast_node_index node;
    sxml_ir_atom input;
    sxml_ast_node_index predicate;
} filter_request;

LOWERING_NOINLINE static sxml_status add_filter(lowerer *state, filter_request request,
                              sxml_ir_expression_index *expression) {
    focused_result predicate = {0};
    sxml_status status = lower_focused_body(
        state,
        (focused_body){
            .node = child_of(state, request.predicate, SXML_AST_FIELD_EXPRESSION),
            .preset_slots = {{0}, {0}, {0}}},
        &predicate);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_expression(
        state,
        (expression_to_add){
            .node = request.node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_FILTER,
                           .filter = {.input = request.input,
                                      .focus = predicate.focus,
                                      .predicate = predicate.body}}},
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

/* An axis step (XPath 4.0, section 4.6.4) with its predicates. */
LOWERING_NOINLINE static sxml_status lower_axis_step(lowerer *state, node_in_block request,
                                   sxml_ir_expression_index *expression) {
    const sxml_ast_node_index node = request.node;
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
                            (focus_read){.block = request.block,
                                         .node = node,
                                         .part = SXML_LOWERING_FOCUS_PART_VALUE},
                            &origin);
    }
    sxml_ir_expression_index result = {0};
    if (status == SXML_STATUS_OK) {
        status = add_step(state,
                          (step_request){.block = request.block,
                                         .node = node,
                                         .origin = origin,
                                         .axis = axis,
                                         .node_test = node_test},
                          &result);
    }
    for (sxml_ast_node_index predicate =
             child_of(state, node, SXML_AST_FIELD_PREDICATE);
         status == SXML_STATUS_OK && predicate.value != 0;
         predicate = next_of(state, predicate)) {
        sxml_ir_atom input = {0};
        status = bind_expression(
            state, (expression_in_block){.block = request.block, .expression = result},
            &input);
        if (status == SXML_STATUS_OK) {
            status = add_filter(state,
                                (filter_request){.node = predicate,
                                                 .input = input,
                                                 .predicate = predicate},
                                &result);
        }
    }
    if (status == SXML_STATUS_OK && is_reverse_axis(axis)) {
        /* A step's value is in document order (XPath 4.0, section 4.6.4);
           a reverse axis yields its nodes nearest first. */
        sxml_ir_atom input = {0};
        status = bind_expression(
            state, (expression_in_block){.block = request.block, .expression = result},
            &input);
        if (status == SXML_STATUS_OK) {
            status = add_document_order(
                state, (atom_at){.node = node, .atom = input}, &result);
        }
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *expression = result;
    return SXML_STATUS_OK;
}

/* "E1/E2" over the atom of E1 (XPath 4.0, section 4.6.3): MAP binding
   each item as the focus, then DOCUMENT_ORDER. */
typedef struct path_step_request {
    lowering_block *block;
    sxml_ast_node_index node;
    sxml_ir_atom input;
    sxml_ast_node_index right;
} path_step_request;

LOWERING_NOINLINE static sxml_status add_path_map(lowerer *state, path_step_request request,
                                sxml_ir_expression_index *expression) {
    focused_result body = {0};
    sxml_status status = lower_focused_body(
        state,
        (focused_body){.node = request.right, .preset_slots = {{0}, {0}, {0}}},
        &body);
    sxml_ir_expression_index map = {0};
    if (status == SXML_STATUS_OK) {
        status = add_expression(
            state,
            (expression_to_add){
                .node = request.node,
                .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                               .map = {.input = request.input,
                                       .focus = body.focus,
                                       .body = body.body,
                                       .flags = 0}}},
            &map);
    }
    sxml_ir_atom mapped = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_expression(
            state, (expression_in_block){.block = request.block, .expression = map},
            &mapped);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_document_order(state,
                              (atom_at){.node = request.node, .atom = mapped},
                              expression);
}

/* "E1//" : E1/descendant-or-self::node(), a MAP of one step (XPath 4.0,
   section 4.6.3), in document order and bound. */
typedef struct descendant_request {
    lowering_block *block;
    sxml_ast_node_index node;
    sxml_ir_atom input;
} descendant_request;

LOWERING_NOINLINE static sxml_status add_descendants(lowerer *state, descendant_request request,
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
                                       .flags = 0}}},
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

/* "/" at the start of a path (XPath 4.0, section 4.6.2):
   fn:root(self::node()) treat as document-node(). */
LOWERING_NOINLINE static sxml_status lower_absolute_path(lowerer *state, node_in_block request,
                                       sxml_ir_expression_index *expression) {
    const sxml_ast_node_index node = request.node;
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
                            (focus_read){.block = request.block,
                                         .node = node,
                                         .part = SXML_LOWERING_FOCUS_PART_VALUE},
                            &focus);
    }
    sxml_ir_expression_index self = {0};
    if (status == SXML_STATUS_OK) {
        status = add_step(
            state,
            (step_request){
                .block = request.block,
                .node = node,
                .origin = focus,
                .axis = SXML_AXIS_SELF,
                .node_test = sxml_builtin_type_identifier(SXML_TYPE_GENERIC_NODE)},
            &self);
    }
    call_argument root_argument = {.is_placeholder = false, .atom = {0}};
    if (status == SXML_STATUS_OK) {
        status = bind_expression(
            state, (expression_in_block){.block = request.block, .expression = self},
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
                           (call_emission){.block = request.block,
                                           .call = &call,
                                           .coerces_arguments = false},
                           &root);
    }
    sxml_ir_atom root_atom = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_expression(
            state, (expression_in_block){.block = request.block, .expression = root},
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
    const sxml_ast_node_index path = child_of(state, node, SXML_AST_FIELD_PATH);
    if (status != SXML_STATUS_OK || path.value == 0) {
        if (status == SXML_STATUS_OK) {
            *expression = document;
        }
        return status;
    }
    sxml_ir_atom input = {0};
    status = bind_expression(
        state, (expression_in_block){.block = request.block, .expression = document},
        &input);
    if (status == SXML_STATUS_OK &&
        node_of(state, node)->detail.operator == SXML_AST_OPERATOR_DOUBLE_SLASH) {
        status = add_descendants(state,
                                 (descendant_request){.block = request.block,
                                                      .node = node,
                                                      .input = input},
                                 &input);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_path_map(state,
                        (path_step_request){.block = request.block,
                                            .node = node,
                                            .input = input,
                                            .right = path},
                        expression);
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

/* The effective boolean value of the right operand of "and"/"or", as
   true() or false() (XPath 4.0, section 4.12). */
LOWERING_NOINLINE static sxml_status lower_boolean_operand(lowerer *state,
                                         sxml_ast_node_index node,
                                         sxml_ir_expression_index *expression) {
    lowering_block block = {{0}, {0}};
    sxml_ir_atom value = {0};
    sxml_status status = lower_to_atom(
        state, (node_in_block){.block = &block, .node = node}, &value);
    sxml_ir_expression_index true_branch = {0};
    sxml_ir_expression_index false_branch = {0};
    if (status == SXML_STATUS_OK) {
        status = add_shared_constant_expression(
            state,
            (shared_constant_at){.node = node,
                                 .which = SXML_LOWERING_SHARED_CONSTANT_TRUE},
            &true_branch);
    }
    if (status == SXML_STATUS_OK) {
        status = add_shared_constant_expression(
            state,
            (shared_constant_at){.node = node,
                                 .which = SXML_LOWERING_SHARED_CONSTANT_FALSE},
            &false_branch);
    }
    sxml_ir_expression_index conditional = {0};
    if (status == SXML_STATUS_OK) {
        status = add_conditional(state,
                                 (conditional_request){.node = node,
                                                       .condition = value,
                                                       .then_branch = true_branch,
                                                       .else_branch = false_branch},
                                 &conditional);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return finish_block(
        state, (block_result){.block = block, .result = conditional}, expression);
}

/* One link of a chain: node applied to left, the atom of its left
   operand (see the file comment). */
typedef struct chain_link {
    lowering_block *block;
    sxml_ast_node_index node;
    sxml_ir_atom left;
} chain_link;

static sxml_status lower_lookup_key(lowerer *state, node_in_block request,
                                    sxml_ir_atom *key) {
    const sxml_ast_node *key_node = node_of(state, request.node);
    if (key_node->kind == SXML_AST_KIND_LOOKUP_WILDCARD) {
        /* "?*": every entry or member (XPath 4.0, section 4.7.3). */
        *key = (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_NONE};
        return SXML_STATUS_OK;
    }
    if (key_node->kind == SXML_AST_KIND_NCNAME) {
        /* An NCName key is the string of that name. */
        return add_string_constant(
            state, sxml_ast_get_text(state->ast, key_node->payload.name.local_name),
            key);
    }
    return lower_to_atom(state, request, key);
}

/* A lookup "?key" of value (XPath 4.0, section 4.7.3); lookup is the
   LOOKUP or UNARY_LOOKUP node holding the key. */
typedef struct lookup_request {
    lowering_block *block;
    sxml_ast_node_index node;
    sxml_ir_atom value;
    sxml_ast_node_index key;
} lookup_request;

LOWERING_NOINLINE static sxml_status add_lookup(lowerer *state, lookup_request request,
                              sxml_ir_expression_index *expression) {
    sxml_ir_atom key = {0};
    sxml_status status = lower_lookup_key(
        state, (node_in_block){.block = request.block, .node = request.key},
        &key);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_expression(
        state,
        (expression_to_add){
            .node = request.node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_LOOKUP,
                           .lookup = {.value = request.value, .key = key}}},
        expression);
}

LOWERING_NOINLINE static sxml_status lower_arrow(lowerer *state, chain_link link,
                               sxml_ir_expression_index *expression) {
    const sxml_ast_node_index node = link.node;
    const sxml_ast_node_index target = child_of(state, node, SXML_AST_FIELD_TARGET);
    const bool is_mapping = node_of(state, node)->detail.operator ==
                            SXML_AST_OPERATOR_MAPPING_ARROW;
    const bool is_static =
        node_of(state, target)->kind == SXML_AST_KIND_FUNCTION_CALL;
    if (!is_mapping) {
        /* "=>": the value is the first argument (XPath 4.0, 4.20.1). */
        if (is_static) {
            return lower_function_call(
                state,
                (call_preparation){.block = link.block,
                                   .call_node = target,
                                   .has_leading_argument = true,
                                   .leading_argument = link.left},
                expression);
        }
        sxml_ir_atom function_item = {0};
        sxml_status status = lower_to_atom(
            state,
            (node_in_block){
                .block = link.block,
                .node = child_of(state, target, SXML_AST_FIELD_FUNCTION)},
            &function_item);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        return lower_dynamic_call(
            state,
            (dynamic_call_request){
                .block = link.block,
                .node = target,
                .function_item = function_item,
                .list = child_of(state, target, SXML_AST_FIELD_ARGUMENTS),
                .has_leading_argument = true,
                .leading_argument = link.left},
            expression);
    }
    /* "=!>": a MAP over the value calling the target with each item first
       (XPath 4.0, 4.20.2); the other arguments are evaluated once, before
       it, with the outer focus. */
    sxml_ir_slot item = {0};
    sxml_status status = new_slot(state, &item);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    lowering_block body_block = {{0}, {0}};
    sxml_ir_expression_index result = {0};
    if (is_static) {
        prepared_call call = {0};
        status = prepare_call(state,
                              (call_preparation){.block = link.block,
                                                 .call_node = target,
                                                 .has_leading_argument = true,
                                                 .leading_argument = slot_atom(item)},
                              &call);
        if (status != SXML_STATUS_OK) {
            return status;
        }
        status = emit_prepared_call(state,
                                    (call_emission){.block = &body_block,
                                                    .call = &call,
                                                    .coerces_arguments = true},
                                    &result);
        release_scratch(state, call.arguments);
    } else {
        sxml_ir_atom function_item = {0};
        status = lower_to_atom(
            state,
            (node_in_block){
                .block = link.block,
                .node = child_of(state, target, SXML_AST_FIELD_FUNCTION)},
            &function_item);
        /* The arguments are lowered in the body, but with the outer
           focus: the MAP binds only the item slot, not the lowering's
           focus, and the outer focus slots stay bound inside it. */
        if (status == SXML_STATUS_OK) {
            status = lower_dynamic_call(
                state,
                (dynamic_call_request){
                    .block = &body_block,
                    .node = target,
                    .function_item = function_item,
                    .list = child_of(state, target, SXML_AST_FIELD_ARGUMENTS),
                    .has_leading_argument = true,
                    .leading_argument = slot_atom(item)},
                &result);
        }
    }
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = finish_block(
            state, (block_result){.block = body_block, .result = result}, &body);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_expression(
        state,
        (expression_to_add){
            .node = node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                           .map = {.input = link.left,
                                   .focus = {.context_value = item},
                                   .body = body,
                                   .flags = 0}}},
        expression);
}

/* "and" and "or" (XPath 4.0, section 4.12): a CONDITIONAL on the left
   operand's effective boolean value; the right one is evaluated only when
   it decides. */
LOWERING_NOINLINE static sxml_status lower_boolean_link(
    lowerer *state, chain_link link, sxml_ir_expression_index *expression) {
    const bool is_or =
        node_of(state, link.node)->kind == SXML_AST_KIND_OR_EXPRESSION;
    sxml_ir_expression_index decided = {0};
    sxml_ir_expression_index right_value = {0};
    sxml_status status = add_shared_constant_expression(
        state,
        (shared_constant_at){.node = link.node,
                             .which = is_or ? SXML_LOWERING_SHARED_CONSTANT_TRUE
                                            : SXML_LOWERING_SHARED_CONSTANT_FALSE},
        &decided);
    if (status == SXML_STATUS_OK) {
        status = lower_boolean_operand(
            state, child_of(state, link.node, SXML_AST_FIELD_RIGHT),
            &right_value);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_conditional(
        state,
        (conditional_request){.node = link.node,
                              .condition = link.left,
                              .then_branch = is_or ? decided : right_value,
                              .else_branch = is_or ? right_value : decided},
        expression);
}

/* "otherwise" (XPath 4.0, section 4.14.2): the left value when it is not
   empty. */
LOWERING_NOINLINE static sxml_status lower_otherwise_link(
    lowerer *state, chain_link link, sxml_ir_expression_index *expression) {
    const sxml_ast_node_index node = link.node;
    sxml_ir_expression_index exists = {0};
    sxml_status status = add_expression(
        state,
        (expression_to_add){
            .node = node,
            .expression = {
                .expression_kind = SXML_IR_EXPRESSION_KIND_INSTANCE_OF,
                .type_test = {
                    .value = link.left,
                    .sequence_type = {
                        .item_type = sxml_builtin_type_identifier(SXML_TYPE_ITEM),
                        .occurrence = SXML_OCCURRENCE_ONE_OR_MORE}}}},
        &exists);
    sxml_ir_atom condition = {0};
    if (status == SXML_STATUS_OK) {
        status = bind_expression(
            state, (expression_in_block){.block = link.block, .expression = exists},
            &condition);
    }
    sxml_ir_expression_index left_value = {0};
    if (status == SXML_STATUS_OK) {
        status = add_atom_expression(
            state, (atom_at){.node = node, .atom = link.left}, &left_value);
    }
    sxml_ir_expression_index right_value = {0};
    if (status == SXML_STATUS_OK) {
        status = lower_body(state, child_of(state, node, SXML_AST_FIELD_RIGHT),
                            &right_value);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_conditional(state,
                           (conditional_request){.node = node,
                                                 .condition = condition,
                                                 .then_branch = left_value,
                                                 .else_branch = right_value},
                           expression);
}

/* An operator that is one BINARY expression. */
LOWERING_NOINLINE static sxml_status lower_binary_link(
    lowerer *state, chain_link link, sxml_ir_expression_index *expression) {
    sxml_ir_binary_operator operator = SXML_IR_BINARY_OPERATOR_ADD;
    if (!binary_operator_of(node_of(state, link.node)->detail.operator,
                            &operator)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    sxml_ir_atom right_atom = {0};
    sxml_status status = lower_to_atom(
        state,
        (node_in_block){.block = link.block,
                        .node = child_of(state, link.node, SXML_AST_FIELD_RIGHT)},
        &right_atom);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_expression(
        state,
        (expression_to_add){
            .node = link.node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_BINARY,
                           .binary = {.operator = operator,
                                      .left_operand = link.left,
                                      .right_operand = right_atom}}},
        expression);
}

/* "->" (XPath 4.0, section 4.19): the right operand with the left value as
   the context value, position and size 1. */
LOWERING_NOINLINE static sxml_status lower_pipeline_link(
    lowerer *state, chain_link link, sxml_ir_expression_index *expression) {
    lowering_focus focus = {.kind = SXML_LOWERING_FOCUS_KIND_FIXED, .fixed_value = link.left};
    lowering_focus *outer_focus = state->function->focus;
    state->function->focus = &focus;
    const sxml_status status = lower_expression(
        state,
        (node_in_block){.block = link.block,
                        .node = child_of(state, link.node, SXML_AST_FIELD_RIGHT)},
        expression);
    state->function->focus = outer_focus;
    return status;
}

/* "!" (XPath 4.0, section 4.18): MAP binding each item. */
LOWERING_NOINLINE static sxml_status lower_simple_map_link(
    lowerer *state, chain_link link, sxml_ir_expression_index *expression) {
    focused_result body = {0};
    const sxml_status status = lower_focused_body(
        state,
        (focused_body){.node = child_of(state, link.node, SXML_AST_FIELD_RIGHT),
                       .preset_slots = {{0}, {0}, {0}}},
        &body);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_expression(
        state,
        (expression_to_add){
            .node = link.node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                           .map = {.input = link.left,
                                   .focus = body.focus,
                                   .body = body.body,
                                   .flags = 0}}},
        expression);
}

/* "/" and "//" between two operands (XPath 4.0, section 4.6.3). */
LOWERING_NOINLINE static sxml_status lower_relative_path_link(
    lowerer *state, chain_link link, sxml_ir_expression_index *expression) {
    sxml_ir_atom input = link.left;
    if (node_of(state, link.node)->detail.operator ==
        SXML_AST_OPERATOR_DOUBLE_SLASH) {
        const sxml_status status = add_descendants(
            state,
            (descendant_request){
                .block = link.block, .node = link.node, .input = input},
            &input);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return add_path_map(
        state,
        (path_step_request){.block = link.block,
                            .node = link.node,
                            .input = input,
                            .right = child_of(state, link.node, SXML_AST_FIELD_RIGHT)},
        expression);
}

LOWERING_NOINLINE static sxml_status lower_chain_link(lowerer *state, chain_link link,
                                    sxml_ir_expression_index *expression) {
    const sxml_ast_node_index node = link.node;
    switch ((uint32_t)node_of(state, node)->kind) {
    case SXML_AST_KIND_OR_EXPRESSION:
    case SXML_AST_KIND_AND_EXPRESSION:
        return lower_boolean_link(state, link, expression);
    case SXML_AST_KIND_OTHERWISE_EXPRESSION:
        return lower_otherwise_link(state, link, expression);
    case SXML_AST_KIND_COMPARISON_EXPRESSION:
    case SXML_AST_KIND_STRING_CONCATENATION_EXPRESSION:
    case SXML_AST_KIND_RANGE_EXPRESSION:
    case SXML_AST_KIND_ADDITIVE_EXPRESSION:
    case SXML_AST_KIND_MULTIPLICATIVE_EXPRESSION:
    case SXML_AST_KIND_UNION_EXPRESSION:
    case SXML_AST_KIND_INTERSECT_EXCEPT_EXPRESSION:
        return lower_binary_link(state, link, expression);
    case SXML_AST_KIND_PIPELINE_EXPRESSION:
        return lower_pipeline_link(state, link, expression);
    case SXML_AST_KIND_SIMPLE_MAP_EXPRESSION:
        return lower_simple_map_link(state, link, expression);
    case SXML_AST_KIND_RELATIVE_PATH_EXPRESSION:
        return lower_relative_path_link(state, link, expression);
    case SXML_AST_KIND_FILTER_EXPRESSION:
        return add_filter(
            state,
            (filter_request){
                .node = node,
                .input = link.left,
                .predicate = child_of(state, node, SXML_AST_FIELD_PREDICATE)},
            expression);
    case SXML_AST_KIND_LOOKUP_EXPRESSION:
        return add_lookup(
            state,
            (lookup_request){
                .block = link.block,
                .node = node,
                .value = link.left,
                .key = child_of(state,
                                child_of(state, node, SXML_AST_FIELD_LOOKUP),
                                SXML_AST_FIELD_KEY)},
            expression);
    case SXML_AST_KIND_DYNAMIC_FUNCTION_CALL:
        return lower_dynamic_call(
            state,
            (dynamic_call_request){
                .block = link.block,
                .node = node,
                .function_item = link.left,
                .list = child_of(state, node, SXML_AST_FIELD_ARGUMENTS),
                .has_leading_argument = false},
            expression);
    case SXML_AST_KIND_ARROW_EXPRESSION:
        return lower_arrow(state, link, expression);
    default:
        return SXML_STATUS_INVALID_ARGUMENT;
    }
}

/* Lowers a chain (see the file comment) in a loop over its spine. */
LOWERING_NOINLINE static sxml_status lower_chain(lowerer *state, node_in_block request,
                               sxml_ir_expression_index *expression) {
    sxml_ast_node_index bottom = request.node;
    sxml_ast_field field = chain_field(node_of(state, bottom)->kind);
    while (field != SXML_AST_FIELD_NONE) {
        bottom = child_of(state, bottom, field);
        field = chain_field(node_of(state, bottom)->kind);
    }
    sxml_ir_atom left = {0};
    sxml_status status = lower_to_atom(
        state, (node_in_block){.block = request.block, .node = bottom}, &left);
    sxml_ast_node_index link = node_of(state, bottom)->parent;
    while (status == SXML_STATUS_OK) {
        sxml_ir_expression_index result = {0};
        status = lower_chain_link(
            state,
            (chain_link){.block = request.block, .node = link, .left = left},
            &result);
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (link.value == request.node.value) {
            *expression = result;
            break;
        }
        status = bind_expression(
            state,
            (expression_in_block){.block = request.block, .expression = result},
            &left);
        link = node_of(state, link)->parent;
    }
    return status;
}

/* ------------------------------------------------------- FLWOR, some */

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

typedef struct binding_chain {
    lowering_block *block;
    binding_cursor cursor;
    /* FLWOR: the return value; quantified: the condition. */
    sxml_ast_node_index result;
    /* QUANTIFIED's quantifier, for a quantified expression. */
    bool is_quantified;
    sxml_ir_quantifier quantifier;
} binding_chain;

static sxml_status lower_bindings(lowerer *state, binding_chain chain,
                                  sxml_ir_expression_index *expression);

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

/* A for or quantifier binding: a MAP or QUANTIFIED over its sequence
   whose body is the rest of the bindings. */
LOWERING_NOINLINE static sxml_status lower_iterating_binding(lowerer *state, binding_chain chain,
                                           sxml_ir_expression_index *expression) {
    const sxml_ast_node_index binding = chain.cursor.binding;
    sxml_ir_atom input = {0};
    sxml_status status = lower_to_atom(
        state,
        (node_in_block){.block = chain.block,
                        .node = child_of(state, binding, SXML_AST_FIELD_SEQUENCE)},
        &input);
    sxml_ir_slot item = {0};
    sxml_ir_slot position = {0};
    if (status == SXML_STATUS_OK) {
        status = new_slot(state, &item);
    }
    const sxml_ast_node_index positional =
        child_of(state, binding, SXML_AST_FIELD_POSITION);
    if (status == SXML_STATUS_OK && positional.value != 0) {
        status = new_slot(state, &position);
    }
    const size_t scope_mark = state->scope_entry_count;
    lowering_block body_block = {{0}, {0}};
    if (status == SXML_STATUS_OK) {
        status = push_typed_variable(
            state,
            (typed_variable){
                .block = &body_block,
                .variable = child_of(state, binding, SXML_AST_FIELD_VARIABLE),
                .value = slot_atom(item)});
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
                              .atom = slot_atom(position)});
        }
    }
    sxml_ir_expression_index rest = {0};
    if (status == SXML_STATUS_OK) {
        binding_chain inner = chain;
        inner.block = &body_block;
        inner.cursor = next_binding(state, chain.cursor);
        status = lower_bindings(state, inner, &rest);
    }
    sxml_ir_expression_index body = {0};
    if (status == SXML_STATUS_OK) {
        status = finish_block(
            state, (block_result){.block = body_block, .result = rest}, &body);
    }
    state->scope_entry_count = scope_mark;
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (chain.is_quantified) {
        return add_expression(
            state,
            (expression_to_add){
                .node = binding,
                .expression = {
                    .expression_kind = SXML_IR_EXPRESSION_KIND_QUANTIFIED,
                    .quantified = {.quantifier = chain.quantifier,
                                   .input = input,
                                   .variable = item,
                                   .condition = body}}},
            expression);
    }
    return add_expression(
        state,
        (expression_to_add){
            .node = binding,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_MAP,
                           .map = {.input = input,
                                   .focus = {.context_value = item,
                                             .context_position = position},
                                   .body = body,
                                   .flags = 0}}},
        expression);
}

static sxml_status lower_bindings(lowerer *state, binding_chain chain,
                                  sxml_ir_expression_index *expression) {
    const sxml_ast_node_index binding = chain.cursor.binding;
    if (binding.value == 0) {
        /* The return value, or the condition whose effective boolean value
           QUANTIFIED takes (XPath 4.0, section 4.15). */
        return lower_expression(
            state, (node_in_block){.block = chain.block, .node = chain.result},
            expression);
    }
    switch ((uint32_t)node_of(state, binding)->kind) {
    case SXML_AST_KIND_FOR_ITEM_BINDING:
    case SXML_AST_KIND_QUANTIFIER_BINDING:
        return lower_iterating_binding(state, chain, expression);
    case SXML_AST_KIND_LET_VALUE_BINDING: {
        /* "let" (XPath 4.0, section 4.13.2): the value bound once. */
        sxml_ir_atom value = {0};
        sxml_status status = lower_to_atom(
            state,
            (node_in_block){.block = chain.block,
                            .node = child_of(state, binding, SXML_AST_FIELD_VALUE)},
            &value);
        const size_t scope_mark = state->scope_entry_count;
        if (status == SXML_STATUS_OK) {
            status = push_typed_variable(
                state,
                (typed_variable){
                    .block = chain.block,
                    .variable = child_of(state, binding, SXML_AST_FIELD_VARIABLE),
                    .value = value});
        }
        if (status == SXML_STATUS_OK) {
            binding_chain inner = chain;
            inner.cursor = next_binding(state, chain.cursor);
            status = lower_bindings(state, inner, expression);
        }
        state->scope_entry_count = scope_mark;
        return status;
    }
    default:
        return report_unsupported(
            state, binding,
            SXML_BYTE_VIEW_FROM_LITERAL("for member, for key/value and destructuring let bindings are not "
            "lowered in Phase 1"));
    }
}

LOWERING_NOINLINE static sxml_status lower_flwor(lowerer *state, node_in_block request,
                               sxml_ir_expression_index *expression) {
    const sxml_ast_node_index clause =
        child_of(state, request.node, SXML_AST_FIELD_CLAUSE);
    return lower_bindings(
        state,
        (binding_chain){
            .block = request.block,
            .cursor = {.clause = clause,
                       .binding = child_of(state, clause, SXML_AST_FIELD_BINDING)},
            .result = child_of(state,
                               child_of(state, request.node, SXML_AST_FIELD_RETURN),
                               SXML_AST_FIELD_VALUE),
            .is_quantified = false},
        expression);
}

LOWERING_NOINLINE static sxml_status lower_quantified(lowerer *state, node_in_block request,
                                    sxml_ir_expression_index *expression) {
    return lower_bindings(
        state,
        (binding_chain){
            .block = request.block,
            .cursor = {.clause = {.value = 0},
                       .binding = child_of(state, request.node,
                                           SXML_AST_FIELD_BINDING)},
            .result = child_of(state, request.node, SXML_AST_FIELD_CONDITION),
            .is_quantified = true,
            .quantifier = node_of(state, request.node)->detail.operator ==
                                  SXML_AST_OPERATOR_EVERY
                              ? SXML_IR_QUANTIFIER_EVERY
                              : SXML_IR_QUANTIFIER_SOME},
        expression);
}

/* -------------------------------------------------- Other expressions */

/* Atoms of the children of parent with field, in order. */
typedef struct atom_list_request {
    lowering_block *block;
    sxml_ast_node_index parent;
    sxml_ast_field field;
} atom_list_request;

LOWERING_NOINLINE static sxml_status lower_atom_list(lowerer *state, atom_list_request request,
                                   sxml_ir_atom_list *list) {
    size_t count = 0;
    for (sxml_ast_node_index child =
             child_of(state, request.parent, request.field);
         child.value != 0; child = next_of(state, child)) {
        count += 1;
    }
    void *memory = NULL;
    sxml_status status = allocate_scratch(
        state,
        (scratch_request){.element_count = count,
                          .element_size = sizeof(sxml_ir_atom),
                          .element_alignment = alignof(sxml_ir_atom)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_atom *atoms = memory;
    size_t atom_index = 0;
    for (sxml_ast_node_index child =
             child_of(state, request.parent, request.field);
         status == SXML_STATUS_OK && child.value != 0;
         child = next_of(state, child)) {
        status = lower_to_atom(
            state, (node_in_block){.block = request.block, .node = child},
            &atoms[atom_index]);
        atom_index += 1;
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_atoms(
            state->unit, (sxml_ir_atom_view){.atoms = atoms, .atom_count = count},
            list);
    }
    release_scratch(state, memory);
    return status;
}

LOWERING_NOINLINE static sxml_status lower_map_constructor(lowerer *state, node_in_block request,
                                         sxml_ir_expression_index *expression) {
    size_t count = 0;
    for (sxml_ast_node_index entry =
             child_of(state, request.node, SXML_AST_FIELD_ENTRY);
         entry.value != 0; entry = next_of(state, entry)) {
        count += 1;
    }
    void *memory = NULL;
    sxml_status status = allocate_scratch(
        state,
        (scratch_request){.element_count = count,
                          .element_size = sizeof(sxml_ir_map_entry),
                          .element_alignment = alignof(sxml_ir_map_entry)},
        &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_ir_map_entry *entries = memory;
    size_t entry_index = 0;
    for (sxml_ast_node_index entry =
             child_of(state, request.node, SXML_AST_FIELD_ENTRY);
         status == SXML_STATUS_OK && entry.value != 0;
         entry = next_of(state, entry)) {
        entries[entry_index] = (sxml_ir_map_entry){{0}, {0}};
        status = lower_to_atom(
            state,
            (node_in_block){.block = request.block,
                            .node = child_of(state, entry, SXML_AST_FIELD_KEY)},
            &entries[entry_index].key);
        const sxml_ast_node_index value =
            child_of(state, entry, SXML_AST_FIELD_VALUE);
        if (status == SXML_STATUS_OK && value.value != 0) {
            status = lower_to_atom(
                state, (node_in_block){.block = request.block, .node = value},
                &entries[entry_index].value);
        }
        entry_index += 1;
    }
    sxml_ir_map_entry_list list = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_ir_unit_add_map_entries(
            state->unit,
            (sxml_ir_map_entry_view){.map_entries = entries,
                                     .map_entry_count = count},
            &list);
    }
    release_scratch(state, memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_expression(
        state,
        (expression_to_add){
            .node = request.node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR,
                           .map_constructor = {.entries = list}}},
        expression);
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

LOWERING_NOINLINE static sxml_status lower_string_template(lowerer *state, node_in_block request,
                                         sxml_ir_expression_index *expression) {
    sxml_ir_atom result = {0};
    bool has_result = false;
    sxml_status status = SXML_STATUS_OK;
    for (sxml_ast_node_index template_part = child_of(state, request.node,
                                             SXML_AST_FIELD_PART);
         status == SXML_STATUS_OK && template_part.value != 0;
         template_part = next_of(state, template_part)) {
        sxml_ir_atom part_atom = {0};
        if (node_of(state, template_part)->kind ==
            SXML_AST_KIND_STRING_TEMPLATE_FIXED_PART) {
            status = add_string_constant(state, text_of(state, template_part), &part_atom);
        } else {
            /* XPath 4.0, section 4.9.3: an enclosed expression's value is
               atomized and joined with spaces. */
            sxml_ir_atom value = {0};
            sxml_function_entry entry = {0};
            status = find_builtin_function(
                state,
                (builtin_function_request){
                    .node = template_part,
                    .local_name = SXML_BYTE_VIEW_FROM_LITERAL("string-join"),
                    .arity = 2},
                &entry);
            const sxml_ast_node_index enclosed =
                child_of(state, template_part, SXML_AST_FIELD_EXPRESSION);
            if (status == SXML_STATUS_OK && enclosed.value != 0) {
                status = lower_to_atom(
                    state,
                    (node_in_block){.block = request.block, .node = enclosed},
                    &value);
            } else if (status == SXML_STATUS_OK) {
                sxml_ir_expression_index empty = {0};
                status = add_empty_sequence(state, template_part, &empty);
                if (status == SXML_STATUS_OK) {
                    status = bind_expression(
                        state,
                        (expression_in_block){.block = request.block,
                                              .expression = empty},
                        &value);
                }
            }
            call_argument arguments[2] = {
                {.is_placeholder = false, .atom = value},
                {.is_placeholder = false, .atom = {0}}};
            if (status == SXML_STATUS_OK) {
                status = add_string_constant(
                    state, SXML_BYTE_VIEW_FROM_LITERAL(" "), &arguments[1].atom);
            }
            sxml_ir_expression_index joined = {0};
            if (status == SXML_STATUS_OK) {
                const prepared_call call = {.node = template_part,
                                            .entry = entry,
                                            .arguments = arguments,
                                            .argument_count = 2,
                                            .placeholder_count = 0};
                status = emit_call(state,
                                   (call_emission){.block = request.block,
                                                   .call = &call,
                                                   .coerces_arguments = true},
                                   &joined);
            }
            if (status == SXML_STATUS_OK) {
                status = bind_expression(
                    state,
                    (expression_in_block){.block = request.block,
                                          .expression = joined},
                    &part_atom);
            }
        }
        if (status != SXML_STATUS_OK) {
            break;
        }
        if (!has_result) {
            result = part_atom;
            has_result = true;
            continue;
        }
        sxml_ir_expression_index concatenation = {0};
        status = add_expression(
            state,
            (expression_to_add){
                .node = template_part,
                .expression = {
                    .expression_kind = SXML_IR_EXPRESSION_KIND_BINARY,
                    .binary = {.operator = SXML_IR_BINARY_OPERATOR_CONCATENATE,
                               .left_operand = result,
                               .right_operand = part_atom}}},
            &concatenation);
        if (status == SXML_STATUS_OK) {
            status = bind_expression(
                state,
                (expression_in_block){.block = request.block,
                                      .expression = concatenation},
                &result);
        }
    }
    if (status == SXML_STATUS_OK && !has_result) {
        status = add_string_constant(state, (sxml_byte_view){0}, &result);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_atom_expression(
        state, (atom_at){.node = request.node, .atom = result}, expression);
}

/* A type test expression: INSTANCE_OF or TREAT. */
typedef struct type_test_request {
    lowering_block *block;
    sxml_ast_node_index node;
    sxml_ir_expression_kind expression_kind;
} type_test_request;

LOWERING_NOINLINE static sxml_status lower_type_test(lowerer *state, type_test_request request,
                                   sxml_ir_expression_index *expression) {
    sxml_ir_atom value = {0};
    sxml_status status = lower_to_atom(
        state,
        (node_in_block){.block = request.block,
                        .node = child_of(state, request.node, SXML_AST_FIELD_VALUE)},
        &value);
    sxml_sequence_type sequence_type = {0};
    if (status == SXML_STATUS_OK) {
        status = resolve_sequence_type(
            state, child_of(state, request.node, SXML_AST_FIELD_TYPE),
            &sequence_type);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_expression(
        state,
        (expression_to_add){
            .node = request.node,
            .expression = {.expression_kind = request.expression_kind,
                           .type_test = {.value = value,
                                         .sequence_type = sequence_type}}},
        expression);
}

LOWERING_NOINLINE static sxml_status lower_cast(lowerer *state, type_test_request request,
                              sxml_ir_expression_index *expression) {
    sxml_ir_atom value = {0};
    sxml_status status = lower_to_atom(
        state,
        (node_in_block){.block = request.block,
                        .node = child_of(state, request.node, SXML_AST_FIELD_VALUE)},
        &value);
    const sxml_ast_node_index target =
        child_of(state, request.node, SXML_AST_FIELD_TYPE);
    sxml_type_identifier target_type = {0};
    if (status == SXML_STATUS_OK) {
        status = resolve_cast_target(state, target, &target_type);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_expression(
        state,
        (expression_to_add){
            .node = request.node,
            .expression = {.expression_kind = request.expression_kind,
                           .cast = {.value = value,
                                    .target_type = target_type,
                                    .allows_empty = (node_of(state, target)->flags &
                                                     SXML_AST_FLAG_OPTIONAL) != 0}}},
        expression);
}

LOWERING_NOINLINE static sxml_status lower_if(lowerer *state, node_in_block request,
                            sxml_ir_expression_index *expression) {
    /* XPath 4.0, section 4.14.1. */
    sxml_ir_atom condition = {0};
    sxml_status status = lower_to_atom(
        state,
        (node_in_block){
            .block = request.block,
            .node = child_of(state, request.node, SXML_AST_FIELD_CONDITION)},
        &condition);
    sxml_ir_expression_index then_branch = {0};
    if (status == SXML_STATUS_OK) {
        status = lower_body(state,
                            child_of(state, request.node, SXML_AST_FIELD_THEN),
                            &then_branch);
    }
    sxml_ir_expression_index else_branch = {0};
    const sxml_ast_node_index else_node =
        child_of(state, request.node, SXML_AST_FIELD_ELSE);
    if (status == SXML_STATUS_OK) {
        status = else_node.value == 0
                     ? add_empty_sequence(state, request.node, &else_branch)
                     : lower_body(state, else_node, &else_branch);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_conditional(state,
                           (conditional_request){.node = request.node,
                                                 .condition = condition,
                                                 .then_branch = then_branch,
                                                 .else_branch = else_branch},
                           expression);
}

/* A literal or variable reference as an ATOM expression. */
LOWERING_NOINLINE static sxml_status lower_atom_expression(
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
LOWERING_NOINLINE static sxml_status lower_context_value(
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

/* "E1, E2, ..." (XPath 4.0, section 4.8.1). */
LOWERING_NOINLINE static sxml_status lower_sequence_expression(
    lowerer *state, node_in_block request, sxml_ir_expression_index *expression) {
    sxml_ir_atom_list atoms = {0};
    const sxml_status status = lower_atom_list(
        state,
        (atom_list_request){.block = request.block,
                            .parent = request.node,
                            .field = SXML_AST_FIELD_ITEM},
        &atoms);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_expression(
        state,
        (expression_to_add){
            .node = request.node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_SEQUENCE,
                           .sequence = atoms}},
        expression);
}

/* "-E" and "+E" (XPath 4.0, section 4.9). */
LOWERING_NOINLINE static sxml_status lower_unary_expression(
    lowerer *state, node_in_block request, sxml_ir_expression_index *expression) {
    sxml_ir_atom operand = {0};
    const sxml_status status = lower_to_atom(
        state,
        (node_in_block){.block = request.block,
                        .node = child_of(state, request.node,
                                         SXML_AST_FIELD_OPERAND)},
        &operand);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    const bool is_minus = node_of(state, request.node)->detail.operator ==
                          SXML_AST_OPERATOR_MINUS;
    return add_expression(
        state,
        (expression_to_add){
            .node = request.node,
            .expression = {.expression_kind = SXML_IR_EXPRESSION_KIND_UNARY,
                           .unary = {.operator = is_minus
                                                     ? SXML_IR_UNARY_OPERATOR_MINUS
                                                     : SXML_IR_UNARY_OPERATOR_PLUS,
                                     .operand = operand}}},
        expression);
}

/* "?key" on the context value (XPath 4.0, section 4.7.3.2). */
LOWERING_NOINLINE static sxml_status lower_unary_lookup(
    lowerer *state, node_in_block request, sxml_ir_expression_index *expression) {
    sxml_ir_atom value = {0};
    const sxml_status status = read_focus(state,
                                          (focus_read){.block = request.block,
                                                       .node = request.node,
                                                       .part = SXML_LOWERING_FOCUS_PART_VALUE},
                                          &value);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_lookup(
        state,
        (lookup_request){.block = request.block,
                         .node = request.node,
                         .value = value,
                         .key = child_of(state, request.node, SXML_AST_FIELD_KEY)},
        expression);
}

/* "[...]" and "array { ... }" (XPath 4.0, section 4.7.2). */
LOWERING_NOINLINE static sxml_status lower_array_constructor(
    lowerer *state, node_in_block request, sxml_ir_expression_index *expression) {
    const bool is_curly = node_of(state, request.node)->kind ==
                          SXML_AST_KIND_CURLY_ARRAY_CONSTRUCTOR;
    sxml_ir_atom_list members = {0};
    sxml_status status = SXML_STATUS_OK;
    if (is_curly) {
        sxml_ir_atom content = {0};
        status = lower_to_atom(
            state,
            (node_in_block){.block = request.block,
                            .node = child_of(state, request.node,
                                             SXML_AST_FIELD_CONTENT)},
            &content);
        if (status == SXML_STATUS_OK) {
            status = sxml_ir_unit_add_atoms(
                state->unit, (sxml_ir_atom_view){.atoms = &content, .atom_count = 1},
                &members);
        }
    } else {
        status = lower_atom_list(state,
                                 (atom_list_request){.block = request.block,
                                                     .parent = request.node,
                                                     .field = SXML_AST_FIELD_MEMBER},
                                 &members);
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return add_expression(
        state,
        (expression_to_add){
            .node = request.node,
            .expression = {
                .expression_kind = SXML_IR_EXPRESSION_KIND_ARRAY_CONSTRUCTOR,
                .array_constructor = {.array_form = is_curly
                                                        ? SXML_IR_ARRAY_FORM_CURLY
                                                        : SXML_IR_ARRAY_FORM_SQUARE,
                                      .members = members}}},
        expression);
}

/* A type test or cast: which IR kind the AST kind lowers to. */
LOWERING_NOINLINE static sxml_status lower_typed_expression(lowerer *state, node_in_block request,
                                          sxml_ir_expression_index *expression) {
    switch ((uint32_t)node_of(state, request.node)->kind) {
    case SXML_AST_KIND_INSTANCE_OF_EXPRESSION:
        return lower_type_test(
            state,
            (type_test_request){.block = request.block,
                                .node = request.node,
                                .expression_kind =
                                    SXML_IR_EXPRESSION_KIND_INSTANCE_OF},
            expression);
    case SXML_AST_KIND_TREAT_EXPRESSION:
        return lower_type_test(
            state,
            (type_test_request){.block = request.block,
                                .node = request.node,
                                .expression_kind = SXML_IR_EXPRESSION_KIND_TREAT},
            expression);
    case SXML_AST_KIND_CAST_EXPRESSION:
        return lower_cast(
            state,
            (type_test_request){.block = request.block,
                                .node = request.node,
                                .expression_kind = SXML_IR_EXPRESSION_KIND_CAST},
            expression);
    default:
        return lower_cast(
            state,
            (type_test_request){.block = request.block,
                                .node = request.node,
                                .expression_kind =
                                    SXML_IR_EXPRESSION_KIND_CASTABLE},
            expression);
    }
}

LOWERING_NOINLINE static sxml_status lower_expression_kind(
    lowerer *state, node_in_block request, sxml_ir_expression_index *expression) {
    lowering_block *block = request.block;
    const sxml_ast_node_index node = request.node;
    const sxml_ast_kind kind = node_of(state, node)->kind;
    if (chain_field(kind) != SXML_AST_FIELD_NONE) {
        return lower_chain(state, request, expression);
    }
    if (is_atom_kind(kind)) {
        return lower_atom_expression(state, request, expression);
    }
    switch ((uint32_t)kind) {
    case SXML_AST_KIND_XPATH:
        return lower_expression(
            state,
            (node_in_block){.block = block,
                            .node = child_of(state, node, SXML_AST_FIELD_BODY)},
            expression);
    case SXML_AST_KIND_PARENTHESIZED_EXPRESSION:
    case SXML_AST_KIND_ENCLOSED_EXPRESSION: {
        const sxml_ast_node_index inner =
            child_of(state, node, SXML_AST_FIELD_EXPRESSION);
        if (inner.value == 0) {
            return add_empty_sequence(state, node, expression);
        }
        return lower_expression(
            state, (node_in_block){.block = block, .node = inner}, expression);
    }
    case SXML_AST_KIND_CONTEXT_VALUE_REFERENCE:
        return lower_context_value(state, request, expression);
    case SXML_AST_KIND_SEQUENCE_EXPRESSION:
        return lower_sequence_expression(state, request, expression);
    case SXML_AST_KIND_FLWOR_EXPRESSION:
        return lower_flwor(state, request, expression);
    case SXML_AST_KIND_QUANTIFIED_EXPRESSION:
        return lower_quantified(state, request, expression);
    case SXML_AST_KIND_IF_EXPRESSION:
        return lower_if(state, request, expression);
    case SXML_AST_KIND_INSTANCE_OF_EXPRESSION:
    case SXML_AST_KIND_TREAT_EXPRESSION:
    case SXML_AST_KIND_CAST_EXPRESSION:
    case SXML_AST_KIND_CASTABLE_EXPRESSION:
        return lower_typed_expression(state, request, expression);
    case SXML_AST_KIND_UNARY_EXPRESSION:
        return lower_unary_expression(state, request, expression);
    case SXML_AST_KIND_ABSOLUTE_PATH_EXPRESSION:
        return lower_absolute_path(state, request, expression);
    case SXML_AST_KIND_AXIS_STEP:
        return lower_axis_step(state, request, expression);
    case SXML_AST_KIND_UNARY_LOOKUP:
        return lower_unary_lookup(state, request, expression);
    case SXML_AST_KIND_FUNCTION_CALL:
        return lower_function_call(
            state,
            (call_preparation){.block = block,
                               .call_node = node,
                               .has_leading_argument = false},
            expression);
    case SXML_AST_KIND_NAMED_FUNCTION_REFERENCE:
        return lower_named_function_reference(state, request, expression);
    case SXML_AST_KIND_INLINE_FUNCTION_EXPRESSION:
        return lower_inline_function(state, node, expression);
    case SXML_AST_KIND_MAP_CONSTRUCTOR:
        return lower_map_constructor(state, request, expression);
    case SXML_AST_KIND_SQUARE_ARRAY_CONSTRUCTOR:
    case SXML_AST_KIND_CURLY_ARRAY_CONSTRUCTOR:
        return lower_array_constructor(state, request, expression);
    case SXML_AST_KIND_STRING_TEMPLATE:
        return lower_string_template(state, request, expression);
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

static sxml_status lower_expression(lowerer *state, node_in_block request,
                                    sxml_ir_expression_index *expression) {
    const sxml_ast_node_index node = request.node;
    if (state->depth >= state->maximum_depth) {
        if (state->error_record != NULL) {
            state->error_record->diagnostic = (sxml_diagnostic){
                .description = SXML_BYTE_VIEW_FROM_LITERAL(
                    "the expression is nested deeper than "
                    "maximum_expression_depth"),
                .location = sxml_ast_get_source_location(state->ast, node)};
        }
        return SXML_STATUS_LIMIT_EXCEEDED;
    }
    state->depth += 1;
    const sxml_status status =
        lower_expression_kind(state, request, expression);
    state->depth -= 1;
    return status;
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
