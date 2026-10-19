#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "function_library_internal.h"
#include "sxml/memory.h"
#include "sxml/name.h"
#include "sxml/status.h"
#include "sxml/tree.h"
#include "sxml/types.h"
#include "sxml/view.h"
#include "sxml/xdm.h"
#include "sxml/xpath.h"

/*
 * The shared intermediate representation (docs/DECISIONS.md, D46, D55).
 * Internal to sxml.
 *
 * XPath, and later XQuery and XSLT, lower into this one IR; passes
 * (src/ir_pass_internal.h) rewrite it; the evaluator
 * (src/evaluator_internal.h) walks it. The shape follows Xee's xee-ir
 * (D5): expressions whose operands are atoms, `let` for every
 * intermediate value (administrative normal form), and nested expressions
 * only where evaluation is conditional or repeated (branches, loop
 * bodies, function bodies).
 *
 * Units, functions, slots
 * -----------------------
 * Everything one compilation produces is an sxml_ir_unit: functions,
 * expressions, constants, global variables and, for XSLT, modes and their
 * template rules. Function 1 of a unit is its main function, the body of
 * the expression or query.
 *
 * Variables are slots of their function's frame, numbered from 1 (0
 * means "none"): first the parameters (1 to parameter_count), then the
 * captured variables, then everything bound inside, up to slot_count.
 * Lowering resolves every variable reference to a slot or a global
 * variable, so the evaluator never looks names up.
 *
 * A FUNCTION_DEFINITION expression makes a function item from an IR
 * function, copying the values of the function's captured_variables
 * (atoms of the defining function's frame) into the item's captured values
 * (D38); a call copies them into the slots after the parameters. So a
 * closure holds values, never frames, and values cannot form cycles (D33).
 *
 * The focus (D46)
 * ---------------
 * The focus is not state of the evaluator: MAP, FILTER, SORT and
 * QUANTIFIED bind the context value, position and size to slots
 * (sxml_ir_focus), and `.`, fn:position() and fn:last() are reads of those
 * slots. A slot of the focus that nothing reads is 0, so the evaluator does
 * not compute a size nobody needs. A function body has no focus (XPath
 * 4.0, section 4.5.6): lowering turns `.` in a body that binds none into a
 * RAISE of XPDY0002. The main function's focus slots hold the context value
 * of the evaluation and may be absent; reading an absent slot raises
 * XPDY0002 at the expression that reads it (XPath 4.0, section 2.2.2).
 * Closures capture focus slots like any other variable (a focus function
 * such as fn:name#0 made where a focus exists, XPath 4.0, section 4.5.4).
 *
 * Paths
 * -----
 * STEP yields the nodes of one axis from one origin in axis order (reverse
 * axes nearest first), so a FILTER on a step counts positions as XPath 4.0,
 * section 4.6.5 requires; DOCUMENT_ORDER then sorts and removes
 * duplicates. "E1/E2" is MAP over E1 binding the focus, body E2, then
 * DOCUMENT_ORDER (section 4.6.3 "Path operator"). A pass may drop a
 * DOCUMENT_ORDER it proves unneeded.
 *
 * Storage and lifetime
 * --------------------
 * Every list (expressions, atoms, functions, ...) is one array in the
 * unit, addressed by an index type or a list (first index plus count);
 * index 0 of every array is reserved and means "none". The unit grows
 * through its allocator while it is built and is sealed before it is
 * evaluated; after sxml_ir_unit_seal nothing moves, so function items may
 * point into it. A unit is reference counted: the compiled expression owns
 * one reference and each function item whose body is an IR function holds
 * one, so inline function items outlive the expression (D38, D51).
 * Constants are values in the item heap, retained by the unit.
 *
 * The expression graph is a tree: every expression except a function body
 * has exactly one parent, so a pass that rewrites one place changes only
 * that place. The verifier (src/ir_pass_internal.h) checks this.
 *
 * Text form
 * ---------
 * sxml_ir_unit_write_text prints a unit for tests and debugging, one
 * function after another:
 *
 *     (function INDEX NAME (parameters N) (captures ATOM...) (slots N)
 *               (focus SLOT SLOT SLOT) BODY)
 *
 * NAME is the EQName or "anonymous", the focus line is left out when the
 * function binds none, and an unused focus slot prints as "_". Expressions
 * print as (KIND OPERANDS...) with the kind in lower case and hyphens
 * ("static-call", "document-order"), slots as $N, globals as $global:N,
 * constants as their value (4, "text", false(), #err:XPDY0002), types as
 * written in XPath 4.0; an ATOM expression prints as its atom. Packet
 * 1.12 compares dumps with golden text.
 *
 * Constructors print their name and value atoms, then on the same line
 * the binding lists that are not empty and the flags (no-preserve-
 * namespaces, no-inherit-namespaces, preserves-types, xslt-rules), then
 * the content nested. TRY_CATCH prints its body, each clause as (catch
 * (NAME-TEST...) SLOT HANDLER) and (finally BODY). tests/ir_test.c's
 * golden text for an XQuery unit:
 *
 *     (function 1 main (parameters 0) (captures) (slots 2)
 *       (let $2 (try-catch
 *                 (binary divide 1 0)
 *                 (catch (err:FOAR0001 *:local) $1
 *                   (lookup $1 "code"))
 *                 (catch (*) $1
 *                   (sequence))
 *                 (finally
 *                   (sequence)))
 *         (element-constructor "p:a" _ (namespaces p="urn:p" ""="urn:d")
 *            (name-namespaces p="urn:p") no-preserve-namespaces xslt-rules
 *           (sequence $2))))
 *
 * (the element constructor's line is wrapped here).
 *
 * Example 1: XPath `let $f := function($x){$x + .} return $f(4)`
 * (QT4 inline-fn-005: the body has no focus, XPDY0002)
 *
 *     (function 1 main (parameters 0) (captures) (slots 1)
 *       (let $1 (function-definition 2)
 *         (dynamic-call $1 4)))
 *     (function 2 anonymous (parameters 1) (captures) (slots 2)
 *       (let $2 (raise #err:XPDY0002 "the focus is absent in a function body")
 *         (binary add $1 $2)))
 *
 * Function 2's signature is function(item()*) as item()*. The dynamic
 * call coerces 4 to the parameter type (nothing to do) and calls; the
 * body binds $x to slot 1; `.` is not bound in function 2, so it lowers to
 * RAISE, bound by `let` because `+` takes atoms. Nothing is captured.
 *
 * Example 2: XPath `/a/b[2]`
 * ("/" is `fn:root(self::node()) treat as document-node()`, section 4.6.2)
 *
 *     (function 1 main (parameters 0) (captures) (slots 14) (focus $1 $2 $3)
 *       (let $4 (step $1 self node())
 *         (let $5 (static-call fn:root#1 $4)
 *           (let $6 (treat $5 document-node())
 *             (let $7 (map $6 (focus $8 _ _)
 *                       (step $8 child element(a)))
 *               (let $9 (document-order $7)
 *                 (let $10 (map $9 (focus $11 _ _)
 *                            (let $12 (step $11 child element(b))
 *                              (filter $12 (focus $13 $14 _) 2)))
 *                   (document-order $10))))))))
 *
 * Reading $1 raises XPDY0002 when the evaluation has no context value; the
 * step on it raises XPTY0004 when it is not a node; TREAT raises XPDY0050.
 * The predicate [2] is the constant 2: FILTER keeps the item whose
 * position equals a numeric predicate value (section 4.4.1), so position
 * slot $14 is bound and $13 is unused by the predicate but bound by the
 * filter; it could be "_".
 *
 * XPath 4.0 constructs that need no kind of their own
 * --------------------------------------------------
 * - "and", "or": CONDITIONAL on the effective boolean value of the left
 *   operand. "otherwise": LET, then CONDITIONAL on fn:exists.
 * - "=>" with a static target: STATIC_CALL with the value as first
 *   argument; with a dynamic target: DYNAMIC_CALL. "=!>": MAP over the
 *   value. "->": LET of the value as the focus value slot, position and
 *   size the constant 1 (section 4.20).
 * - Function calls with "?" placeholders: for a known function, a
 *   FUNCTION_DEFINITION of a closure that captures the supplied arguments
 *   and calls the function; for a dynamic one, STATIC_CALL of
 *   fn:partial-apply with a MAP_CONSTRUCTOR of the supplied arguments.
 *   Keyword arguments are reordered by parameter name; omitted
 *   parameters with a context-value default get the focus value slot.
 * - fn:position() and fn:last(): the focus slots (rows marked
 *   READS_CONTEXT_POSITION and READS_CONTEXT_SIZE); a reference such as
 *   fn:last#0 is a closure capturing the slot.
 * - Unary lookup "?k": LOOKUP on the focus value slot; "?*" has key NONE;
 *   "?(E)" evaluates E first.
 * - String templates and "||": CONCATENATE, or STATIC_CALL of
 *   fn:string-join for parts that are sequences.
 * - for, let, some, every and "!" as in XQuery below.
 *
 * Lowering XQuery 4.0 (Phase 3) without new kinds
 * -----------------------------------------------
 * - FLWOR: a for clause is MAP over its sequence binding the variable as
 *   the focus value slot and "at $i" as the position slot ("allowing
 *   empty" is SXML_IR_MAP_ALLOWING_EMPTY); let is LET; where is
 *   CONDITIONAL with an empty SEQUENCE as else; count is the position slot
 *   of a MAP over the tuple stream; return is the innermost body. Clauses
 *   that reorder tuples need the tuple as a value: the body before order
 *   by returns one array per tuple (ARRAY_CONSTRUCTOR, square form, one
 *   member per live variable, so a variable bound to a sequence stays
 *   one member), SORT orders those arrays with keys computed by LOOKUP
 *   from each array, and a MAP over the sorted arrays unpacks the members
 *   with LOOKUP into slots before the rest. For
 *
 *       for $x at $i in $in let $y := $x * 2 where $y gt 2
 *       order by $y descending return $i
 *
 *   (with $in in global 1):
 *
 *       (let $1 (map $global:1 (focus $2 $3 _)
 *                 (let $4 (binary multiply $2 2)
 *                   (let $5 (binary value-greater $4 2)
 *                     (conditional $5
 *                       (array-constructor square $3 $4)
 *                       (sequence)))))
 *         (let $6 (sort $1 (focus $7 _ _)
 *                   (key (lookup $7 2) descending empty-least))
 *           (map $6 (focus $8 _ _)
 *             (lookup $8 1))))
 *
 *   group by and the window clauses are the named extension point: native
 *   rows that are not in any function table, called by STATIC_CALL with a
 *   native target (src/function_library_internal.h), over the same tuple
 *   arrays; window conditions are passed as FUNCTION_DEFINITION closures.
 * - Prolog: each declared function is an IR function of the unit (found
 *   through a function table entry of kind IR_FUNCTION, called by
 *   STATIC_CALL, referenced as an item by FUNCTION_REFERENCE); each
 *   declared variable is a global variable whose initializer is an IR
 *   function of arity 0, evaluated on first read (a cycle raises
 *   XQDY0054); external variables are globals without an initializer.
 *   Imported library modules are lowered into the same unit.
 * - Constructors: the constructor kinds below build trees through an
 *   sxml_tree_builder; direct and computed constructors lower alike. The
 *   prolog's construction and copy-namespaces modes become
 *   construction_flags on each constructor (D110).
 * - try/catch: the TRY_CATCH kind, its catch clauses and name tests added
 *   with sxml_ir_unit_add_catch_clauses and sxml_ir_unit_add_name_tests.
 * - typeswitch and switch: CONDITIONAL chains over INSTANCE_OF and
 *   comparisons; ordered/unordered: their content.
 *
 * Lowering XSLT 4.0 (Phase 4) with one named extension point
 * -----------------------------------------------------------
 * Instructions lower like XQuery (xsl:variable is LET, xsl:for-each MAP,
 * xsl:sort SORT, xsl:choose CONDITIONAL, literal result elements and
 * xsl:element ELEMENT_CONSTRUCTOR, xsl:copy SHALLOW_COPY, xsl:function and
 * named templates IR functions, global xsl:variable and xsl:param
 * globals). Template rules are the named extension point, as in Xee: each
 * xsl:mode is an sxml_ir_mode with a list of sxml_ir_template_rule, each
 * rule pairs a match function (the pattern lowered to an IR function of
 * one parameter returning xs:boolean) with a template function (an IR
 * function whose parameters 1 to 3 are the focus, followed by the
 * non-tunnel and the tunnel parameters supplied, two maps from names to
 * values, D144), and xsl:apply-templates, xsl:next-match and
 * xsl:apply-imports are the APPLY_TEMPLATES kind; xsl:call-template is a
 * STATIC_CALL of the template function. For
 *
 *     <xsl:template match="b[@x]" mode="m" priority="1">
 *       <xsl:value-of select="@x"/>
 *     </xsl:template>
 *     ...
 *     <xsl:apply-templates select="$in" mode="m"/>
 *
 * mode 1 ("m") holds rule 1: match function 2, template function 3,
 * priority 1:
 *
 *     (function 2 anonymous (parameters 1) (captures) (slots 4)
 *       (let $2 (instance-of $1 element(b))
 *         (conditional $2
 *           (let $3 (step $1 attribute attribute(x))
 *             (let $4 (static-call fn:exists#1 $3) $4))
 *           false())))
 *     (function 3 anonymous (parameters 5) (captures) (slots 8) (focus $1 $2 $3)
 *       (let $6 (step $1 attribute attribute(x))
 *         (let $7 (coerce $6 xs:anyAtomicType* function-argument)
 *           (let $8 (static-call fn:string-join#2 $7 " ")
 *             (text-constructor $8)))))
 *
 * and the instruction is `(apply-templates $global:1 mode 1 from-start)`.
 * The evaluator binds each selected item with its position and size, calls
 * the match functions of the mode in rule order (precedence, priority,
 * declaration order, XSLT 4.0, section 6.8), calls the template function of
 * the first rule that matches, and falls back to the mode's built-in
 * template (6.10), itself a template function made by lowering.
 *
 * Thread safety: a unit is used by the thread of its item heap (D35).
 */

/* A compiled unit; defined below. */
typedef struct sxml_ir_unit sxml_ir_unit;

/* ----------------------------------------------------------- Identifiers */

/* An expression of a unit; 0 means none. */
typedef struct sxml_ir_expression_index {
    uint32_t value;
} sxml_ir_expression_index;

/* A function of a unit; 0 means none, 1 is the main function. */
typedef struct sxml_ir_function_index {
    uint32_t value;
} sxml_ir_function_index;

/* A slot of the current function's frame, from 1; 0 means none. */
typedef struct sxml_ir_slot {
    uint32_t value;
} sxml_ir_slot;

/* A constant of a unit; 0 means none. */
typedef struct sxml_ir_constant_index {
    uint32_t value;
} sxml_ir_constant_index;

/* A global variable of a unit; 0 means none. */
typedef struct sxml_ir_global_index {
    uint32_t value;
} sxml_ir_global_index;

/* An XSLT mode of a unit (reserved for Phase 4); 0 means none. */
typedef struct sxml_ir_mode_index {
    uint32_t value;
} sxml_ir_mode_index;

/* Where an expression comes from, for error records (glossary
   `source_span`); the source name is once per unit. */
typedef struct sxml_ir_source_span {
    uint32_t byte_offset;
    uint32_t byte_count;
    uint32_t line_number;
    uint32_t column_number;
} sxml_ir_source_span;

/* ----------------------------------------------------------------- Atoms */

typedef enum sxml_ir_atom_kind {
    /* No atom: an absent optional operand. */
    SXML_IR_ATOM_KIND_NONE = 0,
    /* A constant of the unit. */
    SXML_IR_ATOM_KIND_CONSTANT,
    /* A slot of the current frame. */
    SXML_IR_ATOM_KIND_SLOT,
    /* A global variable of the unit. */
    SXML_IR_ATOM_KIND_GLOBAL_VARIABLE,
} sxml_ir_atom_kind;

/* An operand that needs no evaluation: a constant or a variable. */
typedef struct sxml_ir_atom {
    sxml_ir_atom_kind atom_kind;
    union {
        sxml_ir_constant_index constant;
        sxml_ir_slot slot;
        sxml_ir_global_index global_variable;
    };
} sxml_ir_atom;

/* The SLOT atom of slot. */
static inline sxml_ir_atom sxml_ir_slot_atom(sxml_ir_slot slot) {
    return (sxml_ir_atom){.atom_kind = SXML_IR_ATOM_KIND_SLOT, .slot = slot};
}

typedef struct sxml_ir_atom_view {
    const sxml_ir_atom *atoms;
    size_t atom_count;
} sxml_ir_atom_view;

/* A run of atoms in the unit's atom array. */
typedef struct sxml_ir_atom_list {
    uint32_t first_atom_index;
    uint32_t atom_count;
} sxml_ir_atom_list;

/* The slots an expression binds the focus to; 0 for a part not read. */
typedef struct sxml_ir_focus {
    sxml_ir_slot context_value;
    sxml_ir_slot context_position;
    sxml_ir_slot context_size;
} sxml_ir_focus;

/* ------------------------------------------------------------ Operators */

/* What a BINARY expression computes. Operands are atomized by the
   expression where the specification says so. */
typedef enum sxml_ir_binary_operator {
    /* Arithmetic (XPath 4.0, section 4.9): numeric promotion, xs:integer
       overflow into decimals (D36), dates and durations. */
    SXML_IR_BINARY_OPERATOR_ADD,
    SXML_IR_BINARY_OPERATOR_SUBTRACT,
    SXML_IR_BINARY_OPERATOR_MULTIPLY,
    SXML_IR_BINARY_OPERATOR_DIVIDE,
    SXML_IR_BINARY_OPERATOR_INTEGER_DIVIDE,
    SXML_IR_BINARY_OPERATOR_MODULO,
    /* Value comparisons (4.11.1). */
    SXML_IR_BINARY_OPERATOR_VALUE_EQUAL,
    SXML_IR_BINARY_OPERATOR_VALUE_NOT_EQUAL,
    SXML_IR_BINARY_OPERATOR_VALUE_LESS,
    SXML_IR_BINARY_OPERATOR_VALUE_LESS_OR_EQUAL,
    SXML_IR_BINARY_OPERATOR_VALUE_GREATER,
    SXML_IR_BINARY_OPERATOR_VALUE_GREATER_OR_EQUAL,
    /* General comparisons (4.11.2), existential, untypedAtomic rules. */
    SXML_IR_BINARY_OPERATOR_GENERAL_EQUAL,
    SXML_IR_BINARY_OPERATOR_GENERAL_NOT_EQUAL,
    SXML_IR_BINARY_OPERATOR_GENERAL_LESS,
    SXML_IR_BINARY_OPERATOR_GENERAL_LESS_OR_EQUAL,
    SXML_IR_BINARY_OPERATOR_GENERAL_GREATER,
    SXML_IR_BINARY_OPERATOR_GENERAL_GREATER_OR_EQUAL,
    /* Node comparisons (4.11.3) over generic nodes. */
    SXML_IR_BINARY_OPERATOR_IS,
    SXML_IR_BINARY_OPERATOR_IS_NOT,
    SXML_IR_BINARY_OPERATOR_PRECEDES,
    SXML_IR_BINARY_OPERATOR_FOLLOWS,
    SXML_IR_BINARY_OPERATOR_PRECEDES_OR_IS,
    SXML_IR_BINARY_OPERATOR_FOLLOWS_OR_IS,
    /* "to" (4.8.2): an integer range (D34). */
    SXML_IR_BINARY_OPERATOR_RANGE,
    /* Node sequences (4.8.4): results in document order, XPTY0004 for an
       item that is not a node. */
    SXML_IR_BINARY_OPERATOR_UNION,
    SXML_IR_BINARY_OPERATOR_INTERSECT,
    SXML_IR_BINARY_OPERATOR_EXCEPT,
    /* "||" (4.10): atomize, empty as "", concatenate. */
    SXML_IR_BINARY_OPERATOR_CONCATENATE,
} sxml_ir_binary_operator;

/* What a UNARY expression computes (4.9). */
typedef enum sxml_ir_unary_operator {
    SXML_IR_UNARY_OPERATOR_PLUS,
    SXML_IR_UNARY_OPERATOR_MINUS,
} sxml_ir_unary_operator;

/* ------------------------------------------------------------ Expressions */

/*
 * The kinds of IR expression. "Atom" fields are atoms, "expression"
 * fields nested expressions evaluated by the expression itself. Values are
 * only ever appended.
 */
typedef enum sxml_ir_expression_kind {
    SXML_IR_EXPRESSION_KIND_NONE = 0,
    /* The value of an atom. */
    SXML_IR_EXPRESSION_KIND_ATOM,
    /* Binds a slot to the value of one expression and evaluates the body. */
    SXML_IR_EXPRESSION_KIND_LET,
    /* The concatenation of atoms ("," 4.8.1); no atoms is "()". */
    SXML_IR_EXPRESSION_KIND_SEQUENCE,
    /* By the effective boolean value of an atom (FORG0006), one of two
       expressions (4.14). and/or lower to it (4.12, short-circuit). */
    SXML_IR_EXPRESSION_KIND_CONDITIONAL,
    SXML_IR_EXPRESSION_KIND_BINARY,
    SXML_IR_EXPRESSION_KIND_UNARY,
    /* A function item from an IR function and its captured values (4.5.6
       inline functions, focus functions, partial applications of known
       functions). */
    SXML_IR_EXPRESSION_KIND_FUNCTION_DEFINITION,
    /* A function item for a known function and arity (4.5.4
       "Named Function References"). */
    SXML_IR_EXPRESSION_KIND_FUNCTION_REFERENCE,
    /* A call of a known function with coerced arguments (4.5.1). */
    SXML_IR_EXPRESSION_KIND_STATIC_CALL,
    /* A call of a function item (4.5.3): coerces the arguments to its
       signature, XPTY0004 for a wrong arity or a non-function. */
    SXML_IR_EXPRESSION_KIND_DYNAMIC_CALL,
    /* "?" (4.7.3): for each map or array of the value, the entries or
       members the key selects; XPTY0004 for other items. */
    SXML_IR_EXPRESSION_KIND_LOOKUP,
    /* For each item of the input, binds the focus and evaluates the body;
       the results concatenated ("!" 4.18, "/" 4.6.3, for clauses). */
    SXML_IR_EXPRESSION_KIND_MAP,
    /* The items of the input for which the predicate is true (4.4.1): a
       numeric predicate value is compared with the position. */
    SXML_IR_EXPRESSION_KIND_FILTER,
    /* some/every (4.15) over one binding; more bindings nest. */
    SXML_IR_EXPRESSION_KIND_QUANTIFIED,
    /* The input ordered by keys computed with each item as the focus
       (XQuery order by over tuple arrays, xsl:sort, fn:sort with keys). */
    SXML_IR_EXPRESSION_KIND_SORT,
    /* One axis step from one generic node, in axis order (4.6.4);
       XPTY0004 when the origin is not a node. */
    SXML_IR_EXPRESSION_KIND_STEP,
    /* Nodes sorted into document order without duplicates; non-nodes
       left as they are; XPTY0018 for a mix (4.6.3). */
    SXML_IR_EXPRESSION_KIND_DOCUMENT_ORDER,
    /* "cast as" and "castable as" (4.17). */
    SXML_IR_EXPRESSION_KIND_CAST,
    SXML_IR_EXPRESSION_KIND_CASTABLE,
    /* "instance of" (4.16.1). */
    SXML_IR_EXPRESSION_KIND_INSTANCE_OF,
    /* "treat as" (4.16.6): XPDY0050 when the value does not match. */
    SXML_IR_EXPRESSION_KIND_TREAT,
    /* The coercion rules (3.4): atomization, promotion, casting of
       untypedAtomic, function coercion; the error code by role. */
    SXML_IR_EXPRESSION_KIND_COERCE,
    /* A map from entries (4.7.1); XQDY0137 for a duplicate key. */
    SXML_IR_EXPRESSION_KIND_MAP_CONSTRUCTOR,
    /* An array (4.7.2). */
    SXML_IR_EXPRESSION_KIND_ARRAY_CONSTRUCTOR,
    /* Raises an error known at compile time, when evaluated: a dynamic
       error such as XPDY0002 for "." without focus, or a type error the
       static analysis proved. */
    SXML_IR_EXPRESSION_KIND_RAISE,

    /* --- Reserved for XQuery 4.0 (Phase 3) and XSLT 4.0 (Phase 4).
       The verifier rejects them until the evaluator implements them. --- */
    /* try/catch (XQuery 4.0, section "Try/Catch Expressions"). */
    SXML_IR_EXPRESSION_KIND_TRY_CATCH,
    /* Node constructors (XQuery 4.0, "Constructors"; XSLT 4.0, section
       11): each builds a new tree through an sxml_tree_builder (D40). */
    SXML_IR_EXPRESSION_KIND_DOCUMENT_CONSTRUCTOR,
    SXML_IR_EXPRESSION_KIND_ELEMENT_CONSTRUCTOR,
    SXML_IR_EXPRESSION_KIND_ATTRIBUTE_CONSTRUCTOR,
    SXML_IR_EXPRESSION_KIND_NAMESPACE_CONSTRUCTOR,
    SXML_IR_EXPRESSION_KIND_TEXT_CONSTRUCTOR,
    SXML_IR_EXPRESSION_KIND_COMMENT_CONSTRUCTOR,
    SXML_IR_EXPRESSION_KIND_PROCESSING_INSTRUCTION_CONSTRUCTOR,
    /* xsl:copy (XSLT 4.0, section 11.9.1): the node without its children,
       with the content added. */
    SXML_IR_EXPRESSION_KIND_SHALLOW_COPY,
    /* xsl:apply-templates, xsl:next-match, xsl:apply-imports (XSLT 4.0,
       section 6): the template rule extension point. */
    SXML_IR_EXPRESSION_KIND_APPLY_TEMPLATES,
    /* Streamed evaluation (XSLT 4.0, 18.1; D219): the nodes of one step
       from a node of a streamed document, selected as its events pass,
       each with its focus for the body. */
    SXML_IR_EXPRESSION_KIND_STREAM,

    /* One more than the largest kind. */
    SXML_IR_EXPRESSION_KIND_COUNT,
} sxml_ir_expression_kind;

/* LET: binds variable to the value of bound_expression, then body. */
typedef struct sxml_ir_let {
    sxml_ir_slot variable;
    sxml_ir_expression_index bound_expression;
    sxml_ir_expression_index body;
    /* True for a content LET (glossary `content_let`, D166): only the
       result its block ends with reads variable, after the variables of
       the content LETs before it and before every other atom it reads.
       In a delivering task the bound expression delivers its value into
       the construction and variable holds the empty sequence; elsewhere
       it is an ordinary LET. */
    bool is_content;
} sxml_ir_let;

/* CONDITIONAL. */
typedef struct sxml_ir_conditional {
    sxml_ir_atom condition;
    sxml_ir_expression_index then_branch;
    sxml_ir_expression_index else_branch;
} sxml_ir_conditional;

/* BINARY. */
typedef struct sxml_ir_binary {
    sxml_ir_binary_operator operator;
    sxml_ir_atom left_operand;
    sxml_ir_atom right_operand;
} sxml_ir_binary;

/* UNARY. */
typedef struct sxml_ir_unary {
    sxml_ir_unary_operator operator;
    sxml_ir_atom operand;
} sxml_ir_unary;

/* What a static call or function reference calls. */
typedef enum sxml_ir_function_target_kind {
    SXML_IR_FUNCTION_TARGET_KIND_NONE = 0,
    /* A native row: static const data, outside the unit. */
    SXML_IR_FUNCTION_TARGET_KIND_NATIVE,
    /* An IR function of the unit. */
    SXML_IR_FUNCTION_TARGET_KIND_IR_FUNCTION,
    /* An xs: constructor function: a cast that allows the empty sequence.
       Only for references; a static call of a constructor lowers to
       CAST. */
    SXML_IR_FUNCTION_TARGET_KIND_CONSTRUCTOR,
} sxml_ir_function_target_kind;

typedef struct sxml_ir_function_target {
    sxml_ir_function_target_kind target_kind;
    union {
        const sxml_native_function *native_function;
        sxml_ir_function_index function;
        sxml_type_identifier constructor_type;
    };
} sxml_ir_function_target;

/* FUNCTION_DEFINITION: function's captured_variables are read in the
   current frame. */
typedef struct sxml_ir_function_definition {
    sxml_ir_function_index function;
} sxml_ir_function_definition;

/* FUNCTION_REFERENCE: a function item of target with this arity and the
   signature, a registered function type, for that arity. */
typedef struct sxml_ir_function_reference {
    sxml_ir_function_target target;
    uint32_t arity;
    sxml_type_identifier signature;
} sxml_ir_function_reference;

/* STATIC_CALL: arguments already coerced by COERCE expressions where the
   parameter types need it; the arity is the atom count. */
typedef struct sxml_ir_static_call {
    sxml_ir_function_target target;
    sxml_ir_atom_list arguments;
    /* The module (glossary ir_module) whose static context the callee
       reads while it runs (the expression's, D163); 0 for its function's. */
    uint32_t module_number;
} sxml_ir_static_call;

/* DYNAMIC_CALL. */
typedef struct sxml_ir_dynamic_call {
    sxml_ir_atom function_item;
    sxml_ir_atom_list arguments;
} sxml_ir_dynamic_call;

/* LOOKUP: key NONE is the wildcard "?*". A unary lookup has the focus
   slot as value. */
typedef struct sxml_ir_lookup {
    sxml_ir_atom value;
    sxml_ir_atom key;
} sxml_ir_lookup;

/* Flags of a MAP. */
typedef enum sxml_ir_map_flag {
    /* XQuery "allowing empty": an empty input evaluates the body once with
       the empty sequence as the focus value and position 0. */
    SXML_IR_MAP_ALLOWING_EMPTY = 1U << 0U,
    /* The MAP of a path "E1/E2" (and of "//"): an item of the input that
       is not a node raises XPTY0004 (XPath 4.0, section 4.7.3; D76). Not
       shown in the text form. */
    SXML_IR_MAP_REQUIRES_NODES = 1U << 1U,
    /* An axis step made of other expressions ("item::T", whose JNodes no
       STEP selects): navigational, as a STEP is, for the selector
       interpretation of a path over JNodes (XPath 4.0, section 4.7.3).
       Not shown in the text form. */
    SXML_IR_MAP_IS_AXIS_STEP = 1U << 2U,
} sxml_ir_map_flag;

/* MAP. */
typedef struct sxml_ir_map {
    sxml_ir_atom input;
    sxml_ir_focus focus;
    sxml_ir_expression_index body;
    /* Bits of sxml_ir_map_flag. */
    uint32_t flags;
} sxml_ir_map;

/* FILTER. */
typedef struct sxml_ir_filter {
    sxml_ir_atom input;
    sxml_ir_focus focus;
    sxml_ir_expression_index predicate;
} sxml_ir_filter;

typedef enum sxml_ir_quantifier {
    SXML_IR_QUANTIFIER_SOME,
    SXML_IR_QUANTIFIER_EVERY,
} sxml_ir_quantifier;

/* QUANTIFIED: the condition's effective boolean value for each item bound
   to variable. */
typedef struct sxml_ir_quantified {
    sxml_ir_quantifier quantifier;
    sxml_ir_atom input;
    sxml_ir_slot variable;
    sxml_ir_expression_index condition;
} sxml_ir_quantified;

typedef enum sxml_ir_sort_direction {
    SXML_IR_SORT_DIRECTION_ASCENDING,
    SXML_IR_SORT_DIRECTION_DESCENDING,
} sxml_ir_sort_direction;

typedef enum sxml_ir_empty_order {
    SXML_IR_EMPTY_ORDER_LEAST,
    SXML_IR_EMPTY_ORDER_GREATEST,
} sxml_ir_empty_order;

/*
 * One sort key: an expression evaluated with each input item as focus.
 * Its atomized values are compared as fn:compare does under the collation
 * (D116): the empty sequence before or after every other value as
 * empty_order says, and the direction reverses the whole order, empty
 * keys included (XQuery 4.0, 4.13.9; XSLT 4.0, 13.1.1).
 */
typedef struct sxml_ir_sort_key {
    sxml_ir_expression_index key;
    sxml_ir_sort_direction direction;
    sxml_ir_empty_order empty_order;
    /* An xs:string atom: a collation URI, resolved when evaluated as
       F&O 4.0, 5.2.7 says (D116; "" is the default collation); NONE for
       the codepoint collation. */
    sxml_ir_atom collation;
    /* An xs:string atom, "ascending" or "descending", that gives the
       direction when evaluated (an xsl:sort order attribute value
       template, D152); NONE keeps direction. */
    sxml_ir_atom order;
    /* A composite sort key (XSLT 4.0, 13.1.2): its value is a sequence of
       atomic items, compared item by item, a shorter sequence before a
       longer one it starts. Otherwise a key of more than one item is
       XPTY0004 (XQuery 4.0 "order by"). */
    bool is_composite;
} sxml_ir_sort_key;

typedef struct sxml_ir_sort_key_view {
    const sxml_ir_sort_key *sort_keys;
    size_t sort_key_count;
} sxml_ir_sort_key_view;

typedef struct sxml_ir_sort_key_list {
    uint32_t first_sort_key_index;
    uint32_t sort_key_count;
} sxml_ir_sort_key_list;

/* SORT: stable (items with equal keys keep their order). */
typedef struct sxml_ir_sort {
    sxml_ir_atom input;
    sxml_ir_focus focus;
    sxml_ir_sort_key_list keys;
} sxml_ir_sort;

/* STEP: node_test is a registered item type: a node test for the
   principal node kind of the axis (element, attribute or namespace) with
   the step's name test, node() for a kind test of any node, or a JNode
   test (XPath 4.0, section 4.6.5). */
typedef struct sxml_ir_step {
    sxml_ir_atom origin;
    sxml_axis axis;
    sxml_type_identifier node_test;
} sxml_ir_step;

/* DOCUMENT_ORDER. */
typedef struct sxml_ir_document_order {
    sxml_ir_atom input;
} sxml_ir_document_order;

/* CAST and CASTABLE: target_type is an atomic, list, union, enumeration
   or choice type that cast allows (4.17.1). */
typedef struct sxml_ir_cast {
    sxml_ir_atom value;
    sxml_type_identifier target_type;
    /* The "?" of the cast target: the empty sequence is allowed. */
    bool allows_empty;
} sxml_ir_cast;

/* INSTANCE_OF and TREAT. */
typedef struct sxml_ir_type_test {
    sxml_ir_atom value;
    sxml_sequence_type sequence_type;
} sxml_ir_type_test;

/* Why a value is coerced; decides the error code (XPath 4.0, section 3.4
   raises XPTY0004 for all of the Phase 1 roles). */
typedef enum sxml_ir_coercion_role {
    /* An argument of a static call. */
    SXML_IR_COERCION_ROLE_FUNCTION_ARGUMENT,
    /* The result of an IR function with a declared return type. */
    SXML_IR_COERCION_ROLE_FUNCTION_RESULT,
    /* A variable with a declared type (let, for, external, global). */
    SXML_IR_COERCION_ROLE_VARIABLE,
    /* An XSLT template or function parameter (XTTE0590, Phase 4). */
    SXML_IR_COERCION_ROLE_TEMPLATE_PARAMETER,
} sxml_ir_coercion_role;

/* COERCE. */
typedef struct sxml_ir_coercion {
    sxml_ir_atom value;
    sxml_sequence_type sequence_type;
    sxml_ir_coercion_role role;
} sxml_ir_coercion;

/* One entry of a map constructor. A NONE value means the key atom holds
   maps whose entries are added (a MapConstructorEntry without ":"). */
typedef struct sxml_ir_map_entry {
    sxml_ir_atom key;
    sxml_ir_atom value;
} sxml_ir_map_entry;

typedef struct sxml_ir_map_entry_view {
    const sxml_ir_map_entry *map_entries;
    size_t map_entry_count;
} sxml_ir_map_entry_view;

typedef struct sxml_ir_map_entry_list {
    uint32_t first_map_entry_index;
    uint32_t map_entry_count;
} sxml_ir_map_entry_list;

/* MAP_CONSTRUCTOR. */
typedef struct sxml_ir_map_constructor {
    sxml_ir_map_entry_list entries;
} sxml_ir_map_constructor;

typedef enum sxml_ir_array_form {
    /* "[a, b]": each atom is one member. */
    SXML_IR_ARRAY_FORM_SQUARE,
    /* "array { E }": one atom; each of its items is one member. */
    SXML_IR_ARRAY_FORM_CURLY,
} sxml_ir_array_form;

/* ARRAY_CONSTRUCTOR. */
typedef struct sxml_ir_array_constructor {
    sxml_ir_array_form array_form;
    sxml_ir_atom_list members;
} sxml_ir_array_constructor;

/* RAISE: static text and the status the error is raised with. */
typedef struct sxml_ir_raise {
    sxml_status error_status;
    sxml_error_code error_code;
    sxml_byte_view description;
} sxml_ir_raise;

/* --- XQuery payloads (Phase 3) --- */

/* Name tests to add to a unit (sxml_ir_unit_add_name_tests). */
typedef struct sxml_ir_name_test_view {
    const sxml_name_test *name_tests;
    size_t name_test_count;
} sxml_ir_name_test_view;

/* A run of name tests in the unit (the error tests of catch clauses). */
typedef struct sxml_ir_name_test_list {
    uint32_t first_name_test_index;
    uint32_t name_test_count;
} sxml_ir_name_test_list;

/* One catch clause: the errors it catches (one or more name tests, any
   kind) and its handler, evaluated with error_details (a slot of the
   function, bound in the handler only) bound to the map of the error
   details whose keys D109 lists, from which lowering reads the $err:
   variables. */
typedef struct sxml_ir_catch_clause {
    sxml_ir_name_test_list error_tests;
    sxml_ir_slot error_details;
    sxml_ir_expression_index handler;
} sxml_ir_catch_clause;

typedef struct sxml_ir_catch_clause_view {
    const sxml_ir_catch_clause *catch_clauses;
    size_t catch_clause_count;
} sxml_ir_catch_clause_view;

typedef struct sxml_ir_catch_clause_list {
    uint32_t first_catch_clause_index;
    uint32_t catch_clause_count;
} sxml_ir_catch_clause_list;

/* TRY_CATCH (XQuery 4.0, section 4.20): body is required; the clauses
   are tried in order; finally_body may be 0, but not when there are no
   clauses. */
typedef struct sxml_ir_try_catch {
    sxml_ir_expression_index body;
    sxml_ir_catch_clause_list catch_clauses;
    sxml_ir_expression_index finally_body;
} sxml_ir_try_catch;

/* A run of namespace bindings in the unit (in-scope namespaces written on
   a direct element constructor, or the namespaces a computed name is
   resolved with), added with sxml_ir_unit_add_namespace_bindings. */
typedef struct sxml_ir_namespace_binding_list {
    uint32_t first_namespace_binding_index;
    uint32_t namespace_binding_count;
} sxml_ir_namespace_binding_list;

/*
 * Bits of construction_flags (D110). 0, the default, is XQuery's
 * construction strip and copy-namespaces preserve, inherit (XQuery 4.0,
 * "Construction Declaration", "Copy-Namespaces Declaration") with
 * XQuery's content rules.
 */
typedef enum sxml_ir_construction_flag {
    /* copy-namespaces no-preserve: copied elements keep only the
       namespaces their names and attributes use. */
    SXML_IR_CONSTRUCTION_NO_PRESERVE_NAMESPACES = 1U << 0U,
    /* copy-namespaces no-inherit: copied elements do not inherit the
       in-scope namespaces of the constructed element. */
    SXML_IR_CONSTRUCTION_NO_INHERIT_NAMESPACES = 1U << 1U,
    /* construction preserve: a constructed element is xs:anyType and
       copied nodes keep their type annotations (XQuery 4.0, 4.12.1.3). */
    SXML_IR_CONSTRUCTION_PRESERVES_TYPES = 1U << 2U,
    /* XSLT 4.0, section 5.7.1 for the content (literal result elements,
       xsl:element, xsl:document): later duplicate attributes replace
       earlier ones, XTDE and XTTE codes. SHALLOW_COPY always has them. */
    SXML_IR_CONSTRUCTION_XSLT_RULES = 1U << 3U,
    /* TEXT only: the text node asks to be serialized as CDATA sections
       (glossary cdata_section; XSLT 4.0, section 11.4.3, the cdata
       attribute; D163). */
    SXML_IR_CONSTRUCTION_CDATA_SECTION = 1U << 4U,
    /* TEXT only: the text node's characters are serialized unescaped
       (glossary disable_output_escaping; XSLT 4.0, section 26.1.8;
       D188). */
    SXML_IR_CONSTRUCTION_DISABLES_OUTPUT_ESCAPING = 1U << 5U,
} sxml_ir_construction_flag;

/* Every bit of sxml_ir_construction_flag. */
#define SXML_IR_CONSTRUCTION_KNOWN_FLAGS                                      \
    ((uint32_t)SXML_IR_CONSTRUCTION_NO_PRESERVE_NAMESPACES |                  \
     (uint32_t)SXML_IR_CONSTRUCTION_NO_INHERIT_NAMESPACES |                   \
     (uint32_t)SXML_IR_CONSTRUCTION_PRESERVES_TYPES |                         \
     (uint32_t)SXML_IR_CONSTRUCTION_XSLT_RULES |                              \
     (uint32_t)SXML_IR_CONSTRUCTION_CDATA_SECTION |                           \
     (uint32_t)SXML_IR_CONSTRUCTION_DISABLES_OUTPUT_ESCAPING)

/*
 * The node constructor kinds and SHALLOW_COPY. Which fields apply:
 * DOCUMENT content, construction_flags; ELEMENT name, namespace_bindings,
 * name_namespaces, content, construction_flags; ATTRIBUTE name,
 * name_namespaces, value; NAMESPACE name (the prefix), value (the URI);
 * TEXT value, construction_flags (CDATA_SECTION and
 * DISABLES_OUTPUT_ESCAPING only); COMMENT value;
 * PROCESSING_INSTRUCTION name (the target), value;
 * SHALLOW_COPY value (the item), content, construction_flags.
 * The others are empty (the verifier checks). A NONE name or value is the
 * empty sequence. content is a nested expression whose items are
 * added to the new node as XQuery 4.0 "Content" and XSLT 4.0 section
 * 5.7.1 describe (D110: a SEQUENCE lists the enclosed expressions).
 */
typedef struct sxml_ir_node_construction {
    sxml_ir_atom name;
    sxml_ir_atom value;
    sxml_ir_expression_index content;
    /* Sent as the element's own namespace bindings. */
    sxml_ir_namespace_binding_list namespace_bindings;
    /* The namespaces a name given as a string is resolved with: the
       statically known namespaces extended by the declarations of
       enclosing direct constructors (prefix 0 is the default element
       namespace), as in QT4 Constr-compelem-compname. Empty means the
       unit's static namespaces. */
    sxml_ir_namespace_binding_list name_namespaces;
    /* Bits of sxml_ir_construction_flag. */
    uint32_t construction_flags;
    /* As sxml_ir_static_call's: the module whose static base URI the new
       node takes (an XSLT instruction's, xml:base included); 0 for its
       function's. */
    uint32_t module_number;
} sxml_ir_node_construction;

/* Which template rules APPLY_TEMPLATES considers (XSLT 4.0, section 6.8). */
typedef enum sxml_ir_rule_search {
    /* xsl:apply-templates: every rule of the mode. */
    SXML_IR_RULE_SEARCH_FROM_START,
    /* xsl:next-match: the rules after the current template rule. */
    SXML_IR_RULE_SEARCH_AFTER_CURRENT_RULE,
    /* xsl:apply-imports: rules of lower import precedence only. */
    SXML_IR_RULE_SEARCH_IMPORTED_ONLY,
} sxml_ir_rule_search;

/*
 * APPLY_TEMPLATES (XSLT 4.0, sections 6.6 to 6.12; D144): for each item of
 * select, with its position and the size of select as the focus, the
 * first template rule of the mode (in the mode's order, under
 * rule_search) whose match function returns true, else the mode's
 * built-in template; its template function is called with the item,
 * position, size, parameters and tunnel_parameters, and the results are
 * concatenated, separator between two. The current mode and template
 * rule of the dynamic context are the selected ones while it runs.
 */
typedef struct sxml_ir_apply_templates {
    /* The items to process; NONE: the focus of the function the
       expression is in, as one item with its position and size
       (xsl:next-match, xsl:apply-imports). */
    sxml_ir_atom select;
    /* NONE: the current mode (mode="#current", next-match,
       apply-imports); outside every template, the unnamed mode. */
    sxml_ir_mode_index mode;
    sxml_ir_rule_search rule_search;
    /* The non-tunnel parameters supplied, a map(xs:QName, item()*); NONE
       for none. */
    sxml_ir_atom parameters;
    /* The tunnel parameters passed on (10.1.6), a map(xs:QName, item()*);
       NONE for none. */
    sxml_ir_atom tunnel_parameters;
    /* Put between the results of two adjacent items (the separator
       attribute, 6.7); NONE for none. */
    sxml_ir_atom separator;
} sxml_ir_apply_templates;

/* Bits of sxml_ir_stream's flags. */
typedef enum sxml_ir_stream_flag {
    /* origin is the URI of a document to read as a streamed document
       (glossary `streamed_document`), not a node: the body runs once with
       its document node as the focus; axis and node_test are not read. */
    SXML_IR_STREAM_OPENS_DOCUMENT = 1U << 0U,
    /* A selected element of a streamed document gets a shallow stream
       window (glossary `stream_window`): its ancestors and attributes, not
       its content, which a STREAM of the body whose origin it is reads as
       it passes (it is the open node, glossary `open_node`). Without the
       flag the window holds the element's subtree. */
    SXML_IR_STREAM_SHALLOW_WINDOWS = 1U << 1U,
} sxml_ir_stream_flag;

/* Every bit of sxml_ir_stream_flag. */
#define SXML_IR_STREAM_KNOWN_FLAGS                                            \
    ((uint32_t)SXML_IR_STREAM_OPENS_DOCUMENT | (uint32_t)SXML_IR_STREAM_SHALLOW_WINDOWS)

/*
 * STREAM (XSLT 4.0, 18.1 and 19; D219), lowered from a stream point
 * (glossary `stream_point`) of a streamed construct. Without
 * OPENS_DOCUMENT: the nodes of axis (CHILD or DESCENDANT) from origin, one
 * node, that match node_test (a registered node test) and, when predicate
 * is not 0, whose predicate is true; for each, in document order, the body
 * with focus.context_value bound to the node and focus.context_position to
 * its position, the values concatenated (delivered, D166, when the STREAM
 * delivers). The predicate runs in the same focus, the position then being
 * the node's among the nodes of the step that match node_test; a numeric
 * predicate value is compared with it (XPath 4.0, 4.4.1). The position the
 * body sees is the node's among the selected nodes, or, when
 * position_counter is not 0, one more than that slot's xs:integer, which
 * the STREAM then rebinds (the position in the whole path when nested
 * STREAMs select it step by step). focus.context_size is 0: a streamed
 * selection has no size.
 *
 * When origin is the open node of a streamed document, the nodes are read
 * from its events (src/evaluator_stream.c); otherwise from origin's tree,
 * so the same IR serves streamed and unstreamed nodes.
 */
typedef struct sxml_ir_stream {
    sxml_ir_atom origin;
    sxml_axis axis;
    sxml_type_identifier node_test;
    sxml_ir_expression_index predicate;
    sxml_ir_focus focus;
    sxml_ir_slot position_counter;
    sxml_ir_expression_index body;
    /* Bits of sxml_ir_stream_flag. */
    uint32_t flags;
} sxml_ir_stream;

/* One expression: its kind, where it comes from, and the payload its kind
   names. */
typedef struct sxml_ir_expression {
    sxml_ir_expression_kind expression_kind;
    sxml_ir_source_span source_span;
    union {
        sxml_ir_atom atom;
        sxml_ir_let let;
        sxml_ir_atom_list sequence;
        sxml_ir_conditional conditional;
        sxml_ir_binary binary;
        sxml_ir_unary unary;
        sxml_ir_function_definition function_definition;
        sxml_ir_function_reference function_reference;
        sxml_ir_static_call static_call;
        sxml_ir_dynamic_call dynamic_call;
        sxml_ir_lookup lookup;
        sxml_ir_map map;
        sxml_ir_filter filter;
        sxml_ir_quantified quantified;
        sxml_ir_sort sort;
        sxml_ir_step step;
        sxml_ir_document_order document_order;
        /* CAST and CASTABLE. */
        sxml_ir_cast cast;
        /* INSTANCE_OF and TREAT. */
        sxml_ir_type_test type_test;
        sxml_ir_coercion coercion;
        sxml_ir_map_constructor map_constructor;
        sxml_ir_array_constructor array_constructor;
        sxml_ir_raise raise;
        sxml_ir_try_catch try_catch;
        /* The constructor kinds and SHALLOW_COPY. */
        sxml_ir_node_construction node_construction;
        sxml_ir_apply_templates apply_templates;
        sxml_ir_stream stream;
    };
} sxml_ir_expression;

/* ------------------------------------------------------------- Functions */

/*
 * A function of a unit: the main function, an inline function, a focus
 * function, a closure made by lowering (a partial application, a focus
 * function reference), a match or template function, an XQuery declared
 * function or a global variable's initializer.
 */
typedef struct sxml_ir_function {
    /* The unit the function belongs to; set by the unit. Function items
       of this function retain and release it. */
    sxml_ir_unit *unit;
    /* 0 for an anonymous function. */
    sxml_name_identifier function_name;
    /* A registered function type: parameter and result types. Arguments
       arrive coerced to them; a declared result type is checked by a
       COERCE in the body. */
    sxml_type_identifier signature;
    /* The arity: parameters are slots 1 to parameter_count. */
    uint32_t parameter_count;
    /* Atoms of defining_function's frame, copied into slots
       parameter_count + 1 onwards (D38). */
    sxml_ir_atom_list captured_variables;
    /* The function whose body contains the FUNCTION_DEFINITION; 0 for
       functions defined at the top level. */
    sxml_ir_function_index defining_function;
    /* Every slot the body uses is at most slot_count. */
    uint32_t slot_count;
    /* For the main function: the slots the evaluation's context value,
       position and size go to. For a template function: slots 1, 2, 3.
       For a default function (glossary): of slots 1, 2, 3 those whose
       part its body reads, which callers pass. Otherwise all 0. */
    sxml_ir_focus focus;
    sxml_ir_expression_index body;
    /* %annotations (XQuery); borrowed from the unit. */
    sxml_function_annotation_view function_annotations;
    sxml_ir_source_span source_span;
    /* The library module the function was written in (the unit's
       modules[module_number]), whose static base URI and source name it
       reads; 0 for the unit's own. */
    uint32_t module_number;
} sxml_ir_function;

/* --------------------------------------------------------------- Globals */

/*
 * A variable of the unit outside every function: an external variable
 * (XPath static context, XQuery "external", XSLT xsl:param) whose value
 * the evaluation request supplies, or a declared variable computed by its
 * initializer on first read.
 */
typedef struct sxml_ir_global_variable {
    sxml_name_identifier variable_name;
    /* The value is coerced to it (XPTY0004). */
    sxml_sequence_type declared_type;
    /* An IR function of arity 0; 0 for an external variable. */
    sxml_ir_function_index initializer;
} sxml_ir_global_variable;

/* ------------------------------------------------------- XSLT modes */

/* What a mode does with an item no rule matches (XSLT 4.0, section 6.10):
   its on-no-match; the mode's built-in template does it. */
typedef enum sxml_ir_built_in_rule {
    SXML_IR_BUILT_IN_RULE_TEXT_ONLY_COPY,
    SXML_IR_BUILT_IN_RULE_SHALLOW_COPY,
    SXML_IR_BUILT_IN_RULE_DEEP_COPY,
    SXML_IR_BUILT_IN_RULE_SHALLOW_SKIP,
    SXML_IR_BUILT_IN_RULE_DEEP_SKIP,
    SXML_IR_BUILT_IN_RULE_FAIL,
} sxml_ir_built_in_rule;

/*
 * One template rule of a mode. A template function has five parameters:
 * the focus (item()?, xs:integer, xs:integer: slots 1 to 3, its focus),
 * the non-tunnel parameters and the tunnel parameters supplied (two
 * map(xs:QName, item()*)); it binds its declared parameters from them.
 */
typedef struct sxml_ir_template_rule {
    /* Arity 1, result xs:boolean: the pattern. */
    sxml_ir_function_index match_function;
    sxml_ir_function_index template_function;
    double priority;
    /* Of the stylesheet level the rule is declared in. */
    int32_t import_precedence;
    /* The lowest import precedence of the stylesheet levels that level
       imports, directly or not (6.11, apply-imports); import_precedence
       when it imports none. */
    int32_t lowest_imported_precedence;
    uint32_t declaration_order;
} sxml_ir_template_rule;

typedef struct sxml_ir_template_rule_view {
    const sxml_ir_template_rule *template_rules;
    size_t template_rule_count;
} sxml_ir_template_rule_view;

typedef struct sxml_ir_template_rule_list {
    uint32_t first_template_rule_index;
    uint32_t template_rule_count;
} sxml_ir_template_rule_list;

/* One mode: its rules in the order they are tried (6.8: import
   precedence, then priority, then the last in declaration order first). */
typedef struct sxml_ir_mode {
    /* 0 for the unnamed mode. */
    sxml_name_identifier mode_name;
    sxml_ir_template_rule_list template_rules;
    sxml_ir_built_in_rule built_in_rule;
    /* A template function (as the rules') that processes an item no rule
       matches. */
    sxml_ir_function_index built_in_template;
    /* on-multiple-match="fail": XTDE0540 when two rules of the best
       import precedence and priority match. */
    bool fails_on_multiple_match;
} sxml_ir_mode;

typedef struct sxml_ir_mode_view {
    const sxml_ir_mode *modes;
    size_t mode_count;
} sxml_ir_mode_view;

/* ------------------------------------------------------------------ Unit */

typedef struct sxml_ir_unit_configuration {
    /* Memory for the unit's arrays as they grow. Must not be the null
       allocator; must outlive the unit. */
    sxml_allocator allocator;
    /* Constants are values in this heap; type and name identifiers come
       from its registry and pool. Kept by reference; must outlive the
       unit. */
    sxml_item_heap *item_heap;
    /* Copied. For error records. */
    sxml_byte_view source_name;
    /* Most expressions the unit may hold, a bound for untrusted input; 0
       means no bound. Reaching it gives SXML_STATUS_LIMIT_EXCEEDED. */
    uint32_t maximum_expression_count;
} sxml_ir_unit_configuration;

/*
 * A compiled unit. The layout is visible for the inline accessors below;
 * only the functions of this header write it. Each array is allocated on
 * the first add, with entry 0 reserved (all zero), so a count is 0 or at
 * least 2.
 */
/*
 * The parts of a static context that functions read while they run
 * (XPath 4.0, section 2.2.1): the statically known decimal formats
 * (fn:format-number), collations (every collation argument) and the
 * in-scope schemas (validate, fn:xsd-validator; D174). Views are borrowed
 * where a caller hands it in; a unit holds its own copy (of the schema
 * pointers, not of the schemas, which outlive the unit).
 */
typedef struct sxml_ir_static_context {
    sxml_decimal_format_view decimal_formats;
    /* Empty for the Unicode codepoint collation. */
    sxml_byte_view default_collation_uri;
    sxml_collation_binding_view collation_bindings;
    sxml_schema_view in_scope_schemas;
} sxml_ir_static_context;

/*
 * A static context the unit keeps for code that reads it while it runs
 * (glossary `ir_module`): an XQuery library module whose functions the
 * unit holds (XQuery 4.0, 5.2; D148), or the static context of XSLT
 * expressions and instructions (an element's xml:base, namespaces, default
 * collation and its package's decimal formats; D163), which STATIC_CALL
 * and the node constructors name. Views of unit memory.
 */
typedef struct sxml_ir_module {
    /* The module's source name, for error locations (err:module). */
    sxml_byte_view source_name;
    /* Its static base URI (XPath 4.0, section 2.2.1); empty when absent. */
    sxml_byte_view static_base_uri;
    /* The memory both view, released when the unit is finalized. */
    void *text_memory;
    /* True when the module has decimal formats and collations of its own
       (static_context); false reads the unit's. */
    bool has_static_context;
    sxml_ir_static_context static_context;
    void *static_context_memory;
    /* True when the module has statically known namespaces of its own
       (names given as strings, casts to xs:QName); false reads the
       unit's. */
    bool has_static_namespaces;
    sxml_namespace_binding_view static_namespaces;
    void *namespace_memory;
} sxml_ir_module;

/* One copy of sxml_ir_unit_add_function_annotations: the annotations and
   the values they view, both unit memory, the values retained. */
typedef struct sxml_ir_annotation_copy {
    sxml_function_annotation *annotations;
    size_t annotation_count;
    sxml_item *values;
    size_t value_count;
} sxml_ir_annotation_copy;

struct sxml_ir_unit {
    uint32_t reference_count;
    bool is_sealed;
    /* True when made by sxml_ir_unit_create: the last release destroys,
       otherwise it finalizes. */
    bool is_created;
    sxml_allocator allocator;
    sxml_item_heap *item_heap;
    sxml_byte_view source_name;
    uint32_t maximum_expression_count;
    /* The static base URI of the unit's static context (XPath 4.0,
       section 2.2.1), in allocator memory; empty when absent. Set by
       sxml_ir_unit_set_static_base_uri; has_static_base_uri tells a unit
       whose static base URI is absent from one that never said (D91). */
    bool has_static_base_uri;
    sxml_byte_view static_base_uri;
    /* The memory static_base_uri views, released when finalized. */
    void *static_base_uri_memory;
    /* The decimal formats and collations of the unit's static context
       (XPath 4.0, section 2.2.1), copied into one block of allocator
       memory (static_context_memory, NULL when all are empty) by
       sxml_ir_unit_set_static_context; read lexically, as the static base
       URI is (sxml_dynamic_context_get_decimal_formats). */
    sxml_ir_static_context static_context;
    void *static_context_memory;

    sxml_ir_expression *expressions;
    uint32_t expression_count;
    uint32_t expression_capacity;
    sxml_ir_atom *atoms;
    uint32_t atom_count;
    uint32_t atom_capacity;
    sxml_ir_map_entry *map_entries;
    uint32_t map_entry_count;
    uint32_t map_entry_capacity;
    sxml_ir_sort_key *sort_keys;
    uint32_t sort_key_count;
    uint32_t sort_key_capacity;
    sxml_ir_function *functions;
    uint32_t function_count;
    uint32_t function_capacity;
    /* Owned references, released when the unit is finalized. */
    sxml_sequence *constants;
    uint32_t constant_count;
    uint32_t constant_capacity;
    sxml_ir_global_variable *global_variables;
    uint32_t global_variable_count;
    uint32_t global_variable_capacity;

    /* Name tests whose LOCAL_NAME_WILDCARD local names are interned in
       the item heap's name pool, so they live as long as it. */
    sxml_name_test *name_tests;
    uint32_t name_test_count;
    uint32_t name_test_capacity;
    sxml_ir_catch_clause *catch_clauses;
    uint32_t catch_clause_count;
    uint32_t catch_clause_capacity;
    /* Also holds the static namespaces below (D84). */
    sxml_namespace_binding *namespace_bindings;
    uint32_t namespace_binding_count;
    uint32_t namespace_binding_capacity;
    /* The statically known namespaces beyond the predeclared prefixes
       (XPath 4.0, section 2.2.1), for casts to xs:QName at run time (F&O
       4.0, section 19.1.9); a run of namespace_bindings. */
    sxml_ir_namespace_binding_list static_namespaces;
    /* XSLT modes and their template rules (sxml_ir_unit_add_modes). */
    sxml_ir_mode *modes;
    uint32_t mode_count;
    uint32_t mode_capacity;
    sxml_ir_template_rule *template_rules;
    uint32_t template_rule_count;
    uint32_t template_rule_capacity;
    /* fn:load-xquery-module made the unit without trust (F&O 4.0, 18.2,
       the trusted option): its functions cannot access external
       resources (FODC0016). Set by sxml_ir_unit_set_untrusted. */
    bool is_untrusted;
    /* Function annotations the unit owns (each copy of
       sxml_ir_unit_add_function_annotations, values retained). */
    sxml_ir_annotation_copy *annotation_copies;
    uint32_t annotation_copy_count;
    uint32_t annotation_copy_capacity;
    /* Library modules (sxml_ir_unit_add_module), from module 1. */
    sxml_ir_module *modules;
    uint32_t module_count;
    uint32_t module_capacity;

    /* All zero: what the accessors return for index 0 or an index out of
       range, so they need no array (arrays are allocated on first add). */
    sxml_ir_expression no_expression;
    sxml_ir_function no_function;
    sxml_ir_global_variable no_global_variable;
};

/*
 * The functions below are defined in src/ir.c (packet 1.12). They follow
 * the conventions of sxml/memory.h; on failure outputs and the unit are
 * unchanged (D20). Every add function returns
 * SXML_STATUS_INVALID_ARGUMENT on a sealed unit, SXML_STATUS_LIMIT_EXCEEDED
 * past maximum_expression_count or UINT32_MAX - 1 entries, and the
 * allocator's failures.
 */

SXML_NODISCARD sxml_status sxml_ir_unit_memory_requirements(
    const sxml_ir_unit_configuration *configuration,
    sxml_memory_requirements *requirements);

/* Places an empty unit (reference count 1, no functions) in memory. */
SXML_NODISCARD sxml_status sxml_ir_unit_initialize(
    sxml_memory_view memory, const sxml_ir_unit_configuration *configuration,
    sxml_ir_unit **unit);

/* Releases the constants and arrays; accepts NULL. Called by the last
   sxml_ir_unit_release of an initialized unit. */
void sxml_ir_unit_finalize(sxml_ir_unit *unit);

SXML_NODISCARD sxml_status sxml_ir_unit_create(
    sxml_allocator allocator, const sxml_ir_unit_configuration *configuration,
    sxml_ir_unit **unit);

/* Finalizes and frees a created unit; accepts NULL. Called by the last
   sxml_ir_unit_release of a created unit. */
void sxml_ir_unit_destroy(sxml_ir_unit *unit);

/* Adds one reference. */
void sxml_ir_unit_retain(sxml_ir_unit *unit);

/* Drops one reference; the last finalizes or destroys the unit. Accepts
   NULL. */
void sxml_ir_unit_release(sxml_ir_unit *unit);

/* Ends building: no array may grow afterwards, so pointers into the unit
   stay valid for its lifetime. Passes run before. */
void sxml_ir_unit_seal(sxml_ir_unit *unit);

SXML_NODISCARD sxml_status sxml_ir_unit_add_expression(
    sxml_ir_unit *unit, const sxml_ir_expression *expression,
    sxml_ir_expression_index *index);

/* An expression to overwrite in place, for lowering that fills a node
   after its children and for passes. */
typedef struct sxml_ir_expression_replacement {
    sxml_ir_expression_index index;
    sxml_ir_expression expression;
} sxml_ir_expression_replacement;

SXML_NODISCARD sxml_status sxml_ir_unit_replace_expression(
    sxml_ir_unit *unit, const sxml_ir_expression_replacement *replacement);

SXML_NODISCARD sxml_status sxml_ir_unit_add_atoms(
    sxml_ir_unit *unit, sxml_ir_atom_view atoms, sxml_ir_atom_list *list);

SXML_NODISCARD sxml_status sxml_ir_unit_add_map_entries(
    sxml_ir_unit *unit, sxml_ir_map_entry_view map_entries,
    sxml_ir_map_entry_list *list);

SXML_NODISCARD sxml_status sxml_ir_unit_add_sort_keys(
    sxml_ir_unit *unit, sxml_ir_sort_key_view sort_keys,
    sxml_ir_sort_key_list *list);

/* Copies name_tests into the unit; the local name of a
   LOCAL_NAME_WILDCARD test is interned in the item heap's name pool
   (empty: SXML_STATUS_INVALID_ARGUMENT; NCName syntax is the caller's
   to check), so the
   view may be released after the call. An empty view gives an empty
   list. */
SXML_NODISCARD sxml_status sxml_ir_unit_add_name_tests(
    sxml_ir_unit *unit, sxml_ir_name_test_view name_tests,
    sxml_ir_name_test_list *list);

/* Copies catch_clauses into the unit (their name tests added before). */
SXML_NODISCARD sxml_status sxml_ir_unit_add_catch_clauses(
    sxml_ir_unit *unit, sxml_ir_catch_clause_view catch_clauses,
    sxml_ir_catch_clause_list *list);

/* Copies bindings into the unit as a run for namespace_bindings or
   name_namespaces of a constructor. */
SXML_NODISCARD sxml_status sxml_ir_unit_add_namespace_bindings(
    sxml_ir_unit *unit, sxml_namespace_binding_view bindings,
    sxml_ir_namespace_binding_list *list);

/* Copies template_rules into the unit, for the template_rules of a mode
   added after them. */
SXML_NODISCARD sxml_status sxml_ir_unit_add_template_rules(
    sxml_ir_unit *unit, sxml_ir_template_rule_view template_rules,
    sxml_ir_template_rule_list *list);

/* Copies modes into the unit as the next modes and writes the index of
   the first to *first_mode (modes number from 1 in the order added). */
SXML_NODISCARD sxml_status sxml_ir_unit_add_modes(sxml_ir_unit *unit,
                                                  sxml_ir_mode_view modes,
                                                  sxml_ir_mode_index *first_mode);

/* Retains value; the unit releases it when finalized. */
SXML_NODISCARD sxml_status sxml_ir_unit_add_constant(
    sxml_ir_unit *unit, sxml_sequence value, sxml_ir_constant_index *index);

/* Copies bindings into the unit as its static namespaces (replacing
   none: call once, before sealing). */
SXML_NODISCARD sxml_status sxml_ir_unit_set_static_namespaces(
    sxml_ir_unit *unit, sxml_namespace_binding_view bindings);

/* Copies context's views (names, property texts and URIs) into the unit
   as the decimal formats and collations of its static context. Call at
   most once, before sealing. Returns SXML_STATUS_INVALID_ARGUMENT on a
   sealed unit or a second call, the allocator's failures and
   SXML_STATUS_ARITHMETIC_OVERFLOW. */
SXML_NODISCARD sxml_status sxml_ir_unit_set_static_context(
    sxml_ir_unit *unit, const sxml_ir_static_context *context);

/* Copies static_base_uri (empty: absent) into the unit as the static base
   URI of its static context, which every function of the unit sees, even
   when it is called after the unit's evaluation ended (D91). Call once,
   before sealing. */
SXML_NODISCARD sxml_status sxml_ir_unit_set_static_base_uri(
    sxml_ir_unit *unit, sxml_byte_view static_base_uri);

/* Copies module's source name, static base URI and, when it has them,
   its static context and static namespaces into the unit as a new module
   and writes its number (from 1), which functions of the module carry
   (sxml_ir_function's module_number), and STATIC_CALL and node
   constructors name. Before sealing. Returns
   SXML_STATUS_INVALID_ARGUMENT on a sealed unit and the allocator's
   failures. */
SXML_NODISCARD sxml_status sxml_ir_unit_add_module(sxml_ir_unit *unit,
                                                   const sxml_ir_module *module,
                                                   uint32_t *module_number);

/* Copies annotations (their names, and their values retained) into the
   unit and writes a view of the copy, which lives as long as the unit:
   what sxml_ir_function's function_annotations borrows (XDM 4.0, 8.1).
   An empty view for none. Before sealing. Returns the allocator's
   failures. */
SXML_NODISCARD sxml_status sxml_ir_unit_add_function_annotations(
    sxml_ir_unit *unit, sxml_function_annotation_view annotations,
    sxml_function_annotation_view *copy);

/* Marks the unit untrusted (is_untrusted). Also after sealing, before the
   unit's first evaluation. */
void sxml_ir_unit_set_untrusted(sxml_ir_unit *unit);

/* Adds a function (its body may still be 0) and sets its unit field. The
   first function added is the main function. */
SXML_NODISCARD sxml_status sxml_ir_unit_add_function(
    sxml_ir_unit *unit, const sxml_ir_function *function,
    sxml_ir_function_index *index);

typedef struct sxml_ir_function_replacement {
    sxml_ir_function_index index;
    sxml_ir_function function;
} sxml_ir_function_replacement;

/* Overwrites a function (to set its body and slot count once known). */
SXML_NODISCARD sxml_status sxml_ir_unit_replace_function(
    sxml_ir_unit *unit, const sxml_ir_function_replacement *replacement);

SXML_NODISCARD sxml_status sxml_ir_unit_add_global_variable(
    sxml_ir_unit *unit, const sxml_ir_global_variable *global_variable,
    sxml_ir_global_index *index);

/*
 * Writes the text form of unit (see above) into output and the number of
 * bytes it needs to *byte_count. Unlike D20, *byte_count is written on
 * SXML_STATUS_CAPACITY_EXCEEDED too, so the caller can retry with that
 * many bytes; output then holds a prefix. Values are printed through the
 * item heap.
 */
SXML_NODISCARD sxml_status sxml_ir_unit_write_text(const sxml_ir_unit *unit,
                                                   sxml_memory_view output,
                                                   size_t *byte_count);

/* Two functions to compare. */
typedef struct sxml_ir_function_pair {
    const sxml_ir_function *first_function;
    const sxml_ir_function *second_function;
} sxml_ir_function_pair;

typedef struct sxml_ir_static_form_comparison {
    bool is_same;
} sxml_ir_static_form_comparison;

/*
 * Writes to comparison->is_same whether two functions have the same
 * static form (F&O 4.0, section 1.9.5, "equivalent" function items, used
 * by fn:deep-equal; D143): the same parameter count, slots, focus slots and
 * body, the functions they define compared alike, up to the renaming of
 * bound variables (slots are numbered by position), ignoring their own
 * names (the caller compares those), annotations, source spans and which
 * atoms of the defining function their own captured variables are (the
 * caller compares the captured values); a defined function keeps its
 * name, as the closure of fn:last#0 has one. It compares the text forms
 * (see "Text form"), written with each FUNCTION_DEFINITION as the
 * function it defines, in memory of the unit's allocator. Functions of two units are never the same. Returns
 * the allocator's and the item heap's failures.
 */
SXML_NODISCARD sxml_status sxml_ir_function_has_same_static_form(
    sxml_ir_function_pair pair, sxml_ir_static_form_comparison *comparison);

/* ------------------------------------------------------------- Accessors */

/* The expression at index, borrowed; a zero expression (kind NONE) for 0
   or an index out of range. */
static inline const sxml_ir_expression *sxml_ir_unit_get_expression(
    const sxml_ir_unit *unit, sxml_ir_expression_index index) {
    if (index.value == 0 || index.value >= unit->expression_count) {
        return &unit->no_expression;
    }
    return &unit->expressions[index.value];
}

/* The function at index, borrowed; a zero function for 0 or an index out
   of range. Stable once the unit is sealed. */
static inline const sxml_ir_function *sxml_ir_unit_get_function(
    const sxml_ir_unit *unit, sxml_ir_function_index index) {
    if (index.value == 0 || index.value >= unit->function_count) {
        return &unit->no_function;
    }
    return &unit->functions[index.value];
}

/* The atoms of list, borrowed; empty when list is out of range. */
static inline sxml_ir_atom_view sxml_ir_unit_get_atoms(
    const sxml_ir_unit *unit, sxml_ir_atom_list list) {
    if (list.atom_count == 0 || list.first_atom_index > unit->atom_count ||
        list.atom_count > unit->atom_count - list.first_atom_index) {
        return (sxml_ir_atom_view){.atoms = NULL, .atom_count = 0};
    }
    return (sxml_ir_atom_view){.atoms = unit->atoms + list.first_atom_index,
                               .atom_count = list.atom_count};
}

/* The map entries of list, borrowed; empty when out of range. */
static inline sxml_ir_map_entry_view sxml_ir_unit_get_map_entries(
    const sxml_ir_unit *unit, sxml_ir_map_entry_list list) {
    if (list.map_entry_count == 0 ||
        list.first_map_entry_index > unit->map_entry_count ||
        list.map_entry_count >
            unit->map_entry_count - list.first_map_entry_index) {
        return (sxml_ir_map_entry_view){.map_entries = NULL,
                                        .map_entry_count = 0};
    }
    return (sxml_ir_map_entry_view){
        .map_entries = unit->map_entries + list.first_map_entry_index,
        .map_entry_count = list.map_entry_count};
}

/* The sort keys of list, borrowed; empty when out of range. */
static inline sxml_ir_sort_key_view sxml_ir_unit_get_sort_keys(
    const sxml_ir_unit *unit, sxml_ir_sort_key_list list) {
    if (list.sort_key_count == 0 ||
        list.first_sort_key_index > unit->sort_key_count ||
        list.sort_key_count > unit->sort_key_count - list.first_sort_key_index) {
        return (sxml_ir_sort_key_view){.sort_keys = NULL, .sort_key_count = 0};
    }
    return (sxml_ir_sort_key_view){
        .sort_keys = unit->sort_keys + list.first_sort_key_index,
        .sort_key_count = list.sort_key_count};
}

/* The name tests of list, borrowed; empty when out of range. */
static inline sxml_ir_name_test_view sxml_ir_unit_get_name_tests(
    const sxml_ir_unit *unit, sxml_ir_name_test_list list) {
    if (list.name_test_count == 0 ||
        list.first_name_test_index > unit->name_test_count ||
        list.name_test_count > unit->name_test_count - list.first_name_test_index) {
        return (sxml_ir_name_test_view){.name_tests = NULL, .name_test_count = 0};
    }
    return (sxml_ir_name_test_view){
        .name_tests = unit->name_tests + list.first_name_test_index,
        .name_test_count = list.name_test_count};
}

/* The catch clauses of list, borrowed; empty when out of range. */
static inline sxml_ir_catch_clause_view sxml_ir_unit_get_catch_clauses(
    const sxml_ir_unit *unit, sxml_ir_catch_clause_list list) {
    if (list.catch_clause_count == 0 ||
        list.first_catch_clause_index > unit->catch_clause_count ||
        list.catch_clause_count >
            unit->catch_clause_count - list.first_catch_clause_index) {
        return (sxml_ir_catch_clause_view){.catch_clauses = NULL,
                                           .catch_clause_count = 0};
    }
    return (sxml_ir_catch_clause_view){
        .catch_clauses = unit->catch_clauses + list.first_catch_clause_index,
        .catch_clause_count = list.catch_clause_count};
}

/* The namespace bindings of list, borrowed; empty when out of range. */
static inline sxml_namespace_binding_view sxml_ir_unit_get_namespace_bindings(
    const sxml_ir_unit *unit, sxml_ir_namespace_binding_list list) {
    if (list.namespace_binding_count == 0 ||
        list.first_namespace_binding_index > unit->namespace_binding_count ||
        list.namespace_binding_count >
            unit->namespace_binding_count - list.first_namespace_binding_index) {
        return (sxml_namespace_binding_view){.namespace_bindings = NULL,
                                             .namespace_binding_count = 0};
    }
    return (sxml_namespace_binding_view){
        .namespace_bindings = unit->namespace_bindings + list.first_namespace_binding_index,
        .namespace_binding_count = list.namespace_binding_count};
}

/* The constant at index, borrowed from the unit; the empty sequence for 0
   or an index out of range. */
static inline sxml_sequence sxml_ir_unit_get_constant(
    const sxml_ir_unit *unit, sxml_ir_constant_index index) {
    if (index.value == 0 || index.value >= unit->constant_count) {
        return (sxml_sequence){0};
    }
    return unit->constants[index.value];
}

/* The static namespaces of the unit, borrowed; empty when none were set. */
static inline sxml_namespace_binding_view sxml_ir_unit_get_static_namespaces(
    const sxml_ir_unit *unit) {
    return sxml_ir_unit_get_namespace_bindings(unit, unit->static_namespaces);
}

/* The mode at index, borrowed; NULL for 0 or out of range. */
static inline const sxml_ir_mode *sxml_ir_unit_get_mode(const sxml_ir_unit *unit,
                                                        sxml_ir_mode_index index) {
    if (index.value == 0 || index.value >= unit->mode_count) {
        return NULL;
    }
    return &unit->modes[index.value];
}

/* The template rules of list, borrowed; empty when out of range. */
static inline sxml_ir_template_rule_view sxml_ir_unit_get_template_rules(
    const sxml_ir_unit *unit, sxml_ir_template_rule_list list) {
    if (list.template_rule_count == 0 ||
        list.first_template_rule_index > unit->template_rule_count ||
        list.template_rule_count >
            unit->template_rule_count - list.first_template_rule_index) {
        return (sxml_ir_template_rule_view){.template_rules = NULL,
                                            .template_rule_count = 0};
    }
    return (sxml_ir_template_rule_view){
        .template_rules = unit->template_rules + list.first_template_rule_index,
        .template_rule_count = list.template_rule_count};
}

/* The global variable at index, borrowed; a zero one for 0 or out of
   range. */
static inline const sxml_ir_global_variable *sxml_ir_unit_get_global_variable(
    const sxml_ir_unit *unit, sxml_ir_global_index index) {
    if (index.value == 0 || index.value >= unit->global_variable_count) {
        return &unit->no_global_variable;
    }
    return &unit->global_variables[index.value];
}
