#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/types.h"
#include "sxml/view.h"
#include "sxml/xpath.h"

/*
 * The abstract syntax tree of the query languages (docs/DECISIONS.md, D24,
 * D29, D52). Internal to sxml.
 *
 * A front end walks the concrete syntax tree that a tree-sitter grammar
 * produces and builds one of these: one node per visible production of the
 * grammar, the grammar's field names as the roles of children, names as
 * written (static analysis, packet 1.12, resolves prefixes), literals
 * decoded, and a source span per node. The concrete syntax tree is freed
 * before the front end returns (D12); the AST is what static analysis and
 * lowering to the IR (src/ir_internal.h) read. The AST is never evaluated.
 *
 * Grammar mapping
 * ---------------
 * - Every named, visible node type of grammars/xpath/src/node-types.json
 *   is an sxml_ast_kind (SXML_AST_KIND_<NODE TYPE IN UPPER CASE>), with
 *   these exceptions:
 *   - `comment` is an extra and is dropped;
 *   - `axis` and `occurrence_indicator` hold only a keyword or a glyph and
 *     become the detail of their parent (axis_step, sequence_type);
 *   - anonymous tokens a field names (`operator`, `quantifier`,
 *     `abbreviation`, `keyword`, `empty`, `optional`, `nillable`, the value
 *     of boolean_constant) become the detail or flags of their node.
 * - Every field of the grammar is an sxml_ast_field. A child keeps the
 *   field it has in its parent, and children are kept in source order, so a
 *   field that repeats (`argument`, `predicate`, `item`) is the list of
 *   children with that field.
 * - Hidden choice productions (`_expression_single`, `_or_level`, ...)
 *   leave no node, as in the concrete syntax tree.
 * - Synonymous glyphs give one operator: `<<` and `precedes`, `*` and `×`,
 *   `div` and `÷`, `union` and `|`.
 * - Keywords used as names (D29) are SXML_AST_KIND_NCNAME nodes.
 *
 * What the front end decodes, and what it leaves as written
 * ---------------------------------------------------------
 * - String literals: the text without delimiters, with `""` and `''`
 *   unescaped (XPath 4.0, A.3.1, StringLiteral).
 * - Numeric literals: the digits with `_` separators removed and without the
 *   `0x` or `0b` prefix; the kind gives the base and the type (A.3.1,
 *   IntegerLiteral ... DoubleLiteral). Lowering makes the atomic item.
 * - String template fixed parts: the text with `{{`, `}}` and the doubled
 *   backtick unescaped (XPath 4.0, section 4.9.3 "String Templates").
 * - Names: prefix, local name and braced URI exactly as written; nothing is
 *   looked up. A braced URI keeps its whitespace; lowering normalizes it.
 * - Checks the grammar leaves to the front end (D29: A.3.2 delimitation,
 *   prefixed lookup key) are made before the AST is complete; any failure
 *   is XPST0003 with the location of the offending node.
 *
 * Storage (D24)
 * -------------
 * Nodes live in one array and are addressed by sxml_ast_node_index; index 0
 * is reserved and means "no node". Decoded text and names live in one byte
 * store addressed by sxml_ast_text. Both have a fixed capacity set at
 * initialization, which the front end computes from the concrete syntax
 * tree: every AST node comes from a distinct concrete syntax tree node, so
 * the number of tree-sitter nodes (plus one for index 0) bounds the node
 * count, and decoded text is never longer than its source, so the byte
 * count of the expression text bounds the text store. Children are linked
 * (first child, last child, next sibling), so a front end can build top
 * down or bottom up.
 *
 * Extension to XQuery 4.0 and XSLT 4.0 patterns (Phases 3 and 4)
 * --------------------------------------------------------------
 * grammars/xquery and grammars/xslt_pattern inherit from grammars/xpath
 * (D29), so their trees are XPath trees with more node types and fields:
 * - New kinds are appended to sxml_ast_kind and new fields to
 *   sxml_ast_field; existing values never change. XQuery adds 66 kinds
 *   (D111; the 67th new node type, allowing_empty, is a flag): modules and
 *   the prolog with its declarations, the FLWOR clauses where, while,
 *   order by, group by, count, trace and the windows, direct constructors,
 *   string constructors, switch, typeswitch, try/catch, validate,
 *   ordered/unordered and extension expressions. XSLT patterns add their
 *   19 `..._pattern` types the same way (D118), the field `pattern` and
 *   the flag OPEN.
 * - Four XPath node types gain fields in XQuery: `annotation` on
 *   inline_function_expression, any_function_type and typed_function_type
 *   (children of kind ANNOTATION), and `allowing_empty` on
 *   for_item_binding (flag ALLOWING_EMPTY). XQuery's computed element and
 *   attribute constructors also take a Name in field `name` (an
 *   unreserved name, xgc: unreserved-name), the namespace and PI
 *   constructors an NCNAME. Children are found by field, never by
 *   position, so code written for the XPath shape keeps working.
 * - XQuery FLWOR clauses are more children with field `clause` of the same
 *   FLWOR_EXPRESSION node, in source order.
 * - Direct constructors are DIRECT_* nodes whose leaves carry decoded
 *   text (see the kinds). Each decoded text is still no longer than its
 *   source, so the capacity rule holds.
 * - A query module is one AST whose root is MODULE; each imported library
 *   module is a separate AST. An XSLT stylesheet is an XML document read
 *   through the XML parser; each XPath expression, attribute value
 *   template part, sequence type and pattern in it is parsed to its own
 *   AST, with source spans that point into the stylesheet (D102).
 * - Entry points sxml_xquery_build_ast and sxml_xslt_pattern_build_ast take
 *   the request below and produce the same sxml_ast, through the parser
 *   part seam at the end of this header.
 *
 * Lowering XQuery 4.0: five examples (D55, D103; src/ir_internal.h)
 * -----------------------------------------------------------------
 * Section numbers are of XQuery 4.0 (W3C QT4 CG draft, 13 October 2026).
 * The IR is written in the text form of src/ir_internal.h; for kinds the
 * text writer does not print fully yet (catch clauses, constructor
 * content lists) the form shown is the one packets 3.6, 3.7 and 3.9 print.
 * `$in` is global 1. The AST is abbreviated to kinds and fields.
 *
 * Example 1: FLWOR with group by (4.13.8)
 *
 *     for $x in $in let $k := $x mod 2 group by $k return count($x)
 *
 *     FLWOR_EXPRESSION
 *       clause FOR_CLAUSE binding FOR_ITEM_BINDING (variable $x, sequence $in)
 *       clause LET_CLAUSE binding LET_VALUE_BINDING (variable $k,
 *                                                    value $x mod 2)
 *       clause GROUP_BY_CLAUSE specification GROUPING_SPECIFICATION (name k)
 *       return RETURN_CLAUSE (value count($x))
 *
 * The clauses before group by produce one tuple array per binding (one
 * member per live variable, as order by does in src/ir_internal.h); the
 * group-by native row (packet 3.8, internal, in no function table) takes
 * the tuple arrays, the members that are grouping keys and their
 * collations, and returns one array per group: the key members as atomic
 * items, every other member the concatenation of its values in the group
 * (4.13.8: non-grouping variables become sequences). A MAP unpacks them:
 *
 *     (let $1 (map $global:1 (focus $2 _ _)
 *               (let $3 (binary modulo $2 2)
 *                 (array-constructor square $2 $3)))
 *       (let $4 (static-call Q{}group-by#3 $1 2 "")
 *         (map $4 (focus $5 _ _)
 *           (let $6 (lookup $5 1)
 *             (static-call fn:count#1 $6)))))
 *
 * (2 is the key member, "" the codepoint collation; $k, member 2, is not
 * read after the group by, so it is not unpacked.)
 *
 * Example 2: a tumbling window (4.13.4.1)
 *
 *     for tumbling window $w in (1 to 6)
 *         start $s when true() end $e when $e - $s eq 2
 *     return sum($w)
 *
 *     FLWOR_EXPRESSION
 *       clause TUMBLING_WINDOW_CLAUSE (variable $w, sequence 1 to 6,
 *         start WINDOW_START_CONDITION (current s, condition true()),
 *         end WINDOW_END_CONDITION (current e, condition $e - $s eq 2))
 *       return RETURN_CLAUSE (value sum($w))
 *
 * The window native row (3.8) takes the input, the start and end
 * conditions as closures of the window variables (current, position,
 * previous, next, D55; an absent variable is passed and ignored) and the
 * kind (tumbling, sliding, `only end`), and returns one array per window
 * whose members are the window and its bound variables:
 *
 *     (function 1 main (parameters 0) (captures) (slots 9)
 *       (let $1 (binary range 1 6)
 *         (let $2 (function-definition 2)
 *           (let $3 (function-definition 3)
 *             (let $4 (static-call Q{}tumbling-window#3 $1 $2 $3)
 *               (map $4 (focus $5 _ _)
 *                 (let $6 (lookup $5 1)
 *                   (static-call fn:sum#1 $6))))))))
 *     (function 2 anonymous (parameters 4) (captures) (slots 4) true())
 *     (function 3 anonymous (parameters 8) (captures) (slots 10)
 *       (let $9 (binary subtract $5 $1)
 *         (let $10 (binary value-equal $9 2) $10)))
 *
 * (function 3's parameters 1 to 4 are the start variables $s, ..., 5 to 8
 * the end variables $e, ...; it lowers `$e - $s` with $e as slot 5.)
 *
 * Example 3: a direct constructor with namespaces (4.12.1)
 *
 *     <p:a xmlns:p="urn:p" xmlns="urn:d" b="{1 + 1}">x<c/></p:a>
 *
 *     DIRECT_ELEMENT_CONSTRUCTOR (name QNAME p:a, end_name QNAME p:a)
 *       attribute DIRECT_ATTRIBUTE (flag NAMESPACE_DECLARATION_ATTRIBUTE,
 *         name QNAME xmlns:p, value DIRECT_ATTRIBUTE_VALUE
 *                                (part ATTRIBUTE_VALUE_TEXT "urn:p"))
 *       attribute DIRECT_ATTRIBUTE (flag NAMESPACE_DECLARATION_ATTRIBUTE,
 *         name QNAME xmlns, value ... "urn:d")
 *       attribute DIRECT_ATTRIBUTE (name QNAME b, value DIRECT_ATTRIBUTE_VALUE
 *         (part ENCLOSED_EXPRESSION (expression 1 + 1)))
 *       content DIRECT_ELEMENT_TEXT "x"
 *       content DIRECT_ELEMENT_CONSTRUCTOR (name QNAME c)
 *
 * The namespace declaration attributes make no attribute node
 * (4.12.1.2); they become the element's namespace bindings p=urn:p and
 * ""=urn:d and extend the statically known namespaces and the default
 * element namespace of everything inside, so c is {urn:d}c. Names are
 * resolved while lowering, so they are constants:
 *
 *     (element-constructor Q{urn:p}p:a _ (namespaces p="urn:p" ""="urn:d")
 *       (let $1 (binary add 1 1)
 *         (let $2 (attribute-constructor b $1)
 *           (let $3 (text-constructor "x")
 *             (let $4 (element-constructor Q{urn:d}c _ (sequence))
 *               (sequence $2 $3 $4))))))
 *
 * The content expression yields the attribute, text and element in order;
 * ELEMENT_CONSTRUCTOR applies 4.12.1.3 "Content" to them (packet 3.5).
 *
 * Example 4: try/catch with finally (4.20)
 *
 *     try { 1 div 0 } catch err:FOAR0001 { $err:code } finally { () }
 *
 *     TRY_CATCH_EXPRESSION
 *       body ENCLOSED_EXPRESSION (expression 1 div 0)
 *       catch CATCH_CLAUSE (errors NAME_TEST_UNION (test err:FOAR0001),
 *                           body ENCLOSED_EXPRESSION $err:code)
 *       finally FINALLY_CLAUSE (body ENCLOSED_EXPRESSION ())
 *
 * The handler reads $err:code from the error details slot of its catch
 * clause (src/ir_internal.h, sxml_ir_catch_clause):
 *
 *     (try-catch
 *       (binary divide 1 0)
 *       (catch (err:FOAR0001) $1
 *         (lookup $1 "code"))
 *       (finally (sequence)))
 *
 * Example 5: a prolog function (5.18)
 *
 *     declare function local:twice($x as xs:integer) as xs:integer
 *       { $x * 2 };
 *     local:twice(21)
 *
 *     MODULE module MAIN_MODULE
 *       prolog PROLOG declaration FUNCTION_DECLARATION (name local:twice,
 *         parameter PARAMETER_WITH_DEFAULT (variable $x as xs:integer),
 *         return_type TYPE_DECLARATION xs:integer,
 *         body ENCLOSED_EXPRESSION $x * 2)
 *       body FUNCTION_CALL (name local:twice, arguments 21)
 *
 * The declared function is IR function 2 (D55: an IR function of the
 * unit, called by STATIC_CALL); its result is coerced to the declared
 * type:
 *
 *     (function 1 main (parameters 0) (captures) (slots 0)
 *       (static-call function:2#1 21))
 *     (function 2 Q{http://www.w3.org/2005/xquery-local-functions}twice
 *               (parameters 1) (captures) (slots 2)
 *       (let $2 (binary multiply $1 2)
 *         (coerce $2 xs:integer function-result)))
 *
 * Thread safety: an AST is used by one thread at a time; separate ASTs are
 * independent.
 */

/* ----------------------------------------------------------- Identifiers */

/* A node of an AST. 0 means "no node". */
typedef struct sxml_ast_node_index {
    uint32_t value;
} sxml_ast_node_index;

/* Bytes in the text store of an AST: byte_count bytes from byte_offset. */
typedef struct sxml_ast_text {
    uint32_t byte_offset;
    uint32_t byte_count;
} sxml_ast_text;

/*
 * Where a node is in the expression text: the bytes from the first to the
 * last byte of its concrete syntax tree node. line_number counts from 1
 * and column_number counts codepoints from 1 (glossary `source_location`).
 * The source name is once per AST.
 */
typedef struct sxml_ast_source_span {
    uint32_t byte_offset;
    uint32_t byte_count;
    uint32_t line_number;
    uint32_t column_number;
} sxml_ast_source_span;

/* ----------------------------------------------------------------- Kinds */

/*
 * The kind of an AST node: the grammar production it comes from. After each
 * kind, its fields and, where they apply, its detail, flags and payload
 * (see sxml_ast_node). "?" marks an optional field, "*" a field that may
 * repeat (zero or more), "+" one or more. Field types are kinds or groups
 * of kinds: Expression is any expression kind, Name is NCNAME,
 * PREFIXED_NAME or URI_QUALIFIED_NAME, ItemType is any item type kind or a
 * Name (a type name), Literal is any literal kind.
 *
 * XPath 4.0 Appendix A.1 names the productions; section numbers below are
 * of XPath 4.0 (W3C QT4 CG draft, 13 October 2026, D29).
 *
 * Values are only ever appended (XQuery and pattern kinds in Phases 3-4).
 */
typedef enum sxml_ast_kind {
    SXML_AST_KIND_NONE = 0,

    /* --- Top level (A.1) --- */
    /* body: Expression; declaration*: DEFAULT_ELEMENT_NAMESPACE_DECLARATION
       or NAMESPACE_DECLARATION, in source order. */
    SXML_AST_KIND_XPATH,
    /* uri: STRING_LITERAL. */
    SXML_AST_KIND_DEFAULT_ELEMENT_NAMESPACE_DECLARATION,
    /* prefix: NCNAME; uri: STRING_LITERAL. */
    SXML_AST_KIND_NAMESPACE_DECLARATION,

    /* --- Expressions (section 4) --- */
    /* item+ (two or more): Expression. The comma operator, 4.8.1. */
    SXML_AST_KIND_SEQUENCE_EXPRESSION,
    /* expression?: Expression. "()" has none. */
    SXML_AST_KIND_PARENTHESIZED_EXPRESSION,
    /* expression?: Expression. "{ }" has none (4.5.2: the empty
       sequence). */
    SXML_AST_KIND_ENCLOSED_EXPRESSION,
    /* No fields: ".", 4.3.4. */
    SXML_AST_KIND_CONTEXT_VALUE_REFERENCE,
    /* name: Name. 4.3.3. */
    SXML_AST_KIND_VARIABLE_REFERENCE,

    /* Literals (4.3.1, A.3.1). Payload text, decoded as described above. */
    SXML_AST_KIND_INTEGER_LITERAL,
    SXML_AST_KIND_HEX_INTEGER_LITERAL,
    SXML_AST_KIND_BINARY_INTEGER_LITERAL,
    SXML_AST_KIND_DECIMAL_LITERAL,
    SXML_AST_KIND_DOUBLE_LITERAL,
    SXML_AST_KIND_STRING_LITERAL,
    /* name: Name. "#name", 4.3.1.3. */
    SXML_AST_KIND_QNAME_LITERAL,
    /* value: a numeric literal kind. "-1" in a jnode() selector. */
    SXML_AST_KIND_NEGATIVE_NUMERIC_LITERAL,
    /* No fields. Flag TRUE for "true()", none for "false()". */
    SXML_AST_KIND_BOOLEAN_CONSTANT,
    /* part*: STRING_TEMPLATE_FIXED_PART or ENCLOSED_EXPRESSION. 4.9.3. */
    SXML_AST_KIND_STRING_TEMPLATE,
    /* Payload text. */
    SXML_AST_KIND_STRING_TEMPLATE_FIXED_PART,

    /* FLWOR subset of XPath (4.12, 4.13). */
    /* clause+: FOR_CLAUSE or LET_CLAUSE; return: RETURN_CLAUSE. */
    SXML_AST_KIND_FLWOR_EXPRESSION,
    /* binding+: FOR_ITEM_BINDING, FOR_MEMBER_BINDING or
       FOR_ENTRY_BINDING. */
    SXML_AST_KIND_FOR_CLAUSE,
    /* variable: VARIABLE_NAME_AND_TYPE; position?: POSITIONAL_VARIABLE;
       sequence: Expression. "for $x at $i in E". Flag ALLOWING_EMPTY for
       XQuery's "allowing empty" (4.13.2); score?: FT_SCORE_VARIABLE in
       XQuery (Full Text 3.0, 2.3.1, D214). */
    SXML_AST_KIND_FOR_ITEM_BINDING,
    /* As FOR_ITEM_BINDING: "for member $m in A". */
    SXML_AST_KIND_FOR_MEMBER_BINDING,
    /* key?: FOR_ENTRY_KEY_BINDING; value?: FOR_ENTRY_VALUE_BINDING (at
       least one); position?: POSITIONAL_VARIABLE; sequence: Expression. */
    SXML_AST_KIND_FOR_ENTRY_BINDING,
    /* variable: VARIABLE_NAME_AND_TYPE. */
    SXML_AST_KIND_FOR_ENTRY_KEY_BINDING,
    SXML_AST_KIND_FOR_ENTRY_VALUE_BINDING,
    /* name: Name. "at $i". */
    SXML_AST_KIND_POSITIONAL_VARIABLE,
    /* binding+: LET_VALUE_BINDING, LET_SEQUENCE_BINDING,
       LET_ARRAY_BINDING or LET_MAP_BINDING; in XQuery also
       LET_SCORE_BINDING (Full Text 3.0, D214). */
    SXML_AST_KIND_LET_CLAUSE,
    /* variable: VARIABLE_NAME_AND_TYPE; value: Expression. */
    SXML_AST_KIND_LET_VALUE_BINDING,
    /* variable+: VARIABLE_NAME_AND_TYPE; type?: TYPE_DECLARATION;
       value: Expression. "let $( $a, $b ) := E" and the array "$[...]" and
       map "${...}" destructuring forms. */
    SXML_AST_KIND_LET_SEQUENCE_BINDING,
    SXML_AST_KIND_LET_ARRAY_BINDING,
    SXML_AST_KIND_LET_MAP_BINDING,
    /* name: Name; type?: TYPE_DECLARATION. "$x as T". */
    SXML_AST_KIND_VARIABLE_NAME_AND_TYPE,
    /* value: Expression. */
    SXML_AST_KIND_RETURN_CLAUSE,

    /* binding+: QUANTIFIER_BINDING; condition: Expression. Detail
       operator SOME or EVERY. 4.15. */
    SXML_AST_KIND_QUANTIFIED_EXPRESSION,
    /* variable: VARIABLE_NAME_AND_TYPE; sequence: Expression. */
    SXML_AST_KIND_QUANTIFIER_BINDING,
    /* condition: Expression; then: Expression or, in the braced form "if
       (C) { E }", ENCLOSED_EXPRESSION; else?: Expression (absent in the
       braced form, meaning the empty sequence). 4.14. */
    SXML_AST_KIND_IF_EXPRESSION,

    /* Binary operators (A.5 precedence; the grammar is left-recursive).
       left: Expression; right: Expression; detail operator. */
    SXML_AST_KIND_OR_EXPRESSION,                   /* OR */
    SXML_AST_KIND_AND_EXPRESSION,                  /* AND */
    SXML_AST_KIND_COMPARISON_EXPRESSION,           /* value, general, node */
    SXML_AST_KIND_OTHERWISE_EXPRESSION,            /* OTHERWISE */
    SXML_AST_KIND_STRING_CONCATENATION_EXPRESSION, /* CONCATENATE */
    SXML_AST_KIND_RANGE_EXPRESSION,                /* TO */
    SXML_AST_KIND_ADDITIVE_EXPRESSION,             /* PLUS, MINUS */
    SXML_AST_KIND_MULTIPLICATIVE_EXPRESSION, /* MULTIPLY, DIVIDE, ... */
    SXML_AST_KIND_UNION_EXPRESSION,          /* UNION */
    SXML_AST_KIND_INTERSECT_EXCEPT_EXPRESSION, /* INTERSECT, EXCEPT */
    SXML_AST_KIND_BUT_WITH_EXPRESSION, /* no operator: "but with" */
    SXML_AST_KIND_PIPELINE_EXPRESSION, /* PIPELINE "->" */
    SXML_AST_KIND_SIMPLE_MAP_EXPRESSION,     /* SIMPLE_MAP "!" */
    SXML_AST_KIND_RELATIVE_PATH_EXPRESSION,  /* SLASH, DOUBLE_SLASH */

    /* value: Expression; type: SEQUENCE_TYPE. 4.16. */
    SXML_AST_KIND_INSTANCE_OF_EXPRESSION,
    SXML_AST_KIND_TREAT_EXPRESSION,
    /* value: Expression; type: CAST_TARGET. */
    SXML_AST_KIND_CASTABLE_EXPRESSION,
    SXML_AST_KIND_CAST_EXPRESSION,
    /* type: Name, CHOICE_ITEM_TYPE, ENUMERATION_TYPE or a map, array or
       record type. Flag OPTIONAL for a trailing "?". */
    SXML_AST_KIND_CAST_TARGET,

    /* value: Expression; target: FUNCTION_CALL (static arrow) or
       RESTRICTED_DYNAMIC_CALL (dynamic arrow). Detail operator
       SEQUENCE_ARROW "=>" or MAPPING_ARROW "=!>". 4.20. */
    SXML_AST_KIND_ARROW_EXPRESSION,
    /* function: VARIABLE_REFERENCE, PARENTHESIZED_EXPRESSION,
       NAMED_FUNCTION_REFERENCE, INLINE_FUNCTION_EXPRESSION,
       MAP_CONSTRUCTOR or an array constructor; arguments:
       POSITIONAL_ARGUMENT_LIST. */
    SXML_AST_KIND_RESTRICTED_DYNAMIC_CALL,
    /* operand: Expression. Detail operator PLUS or MINUS. Nested signs are
       nested nodes. */
    SXML_AST_KIND_UNARY_EXPRESSION,

    /* Paths (4.6). */
    /* path?: Expression. Detail operator SLASH ("/" alone has no path) or
       DOUBLE_SLASH. */
    SXML_AST_KIND_ABSOLUTE_PATH_EXPRESSION,
    /* test?: a node test (absent for ".."); predicate*: PREDICATE. Detail
       axis: the axis written, or SXML_AST_AXIS_NONE when none is (the
       default axis rule of 4.6.5 is applied by lowering). Flag
       ABBREVIATED for "@test" (axis ATTRIBUTE) and ".." (axis PARENT). A
       node test is a Name, WILDCARD, UNION_NODE_TEST, DYNAMIC_NODE_TEST,
       GNODE_TYPE, JNODE_TYPE or an XNode type kind. */
    SXML_AST_KIND_AXIS_STEP,
    /* test+: Name, WILDCARD or a type test. "(a | b)". */
    SXML_AST_KIND_UNION_NODE_TEST,
    /* expression: ENCLOSED_EXPRESSION. */
    SXML_AST_KIND_DYNAMIC_NODE_TEST,
    /* No fields. Detail name_test_kind: ANY_NAME "*", NAMESPACE_WILDCARD
       "p:*" (payload name.namespace_prefix) or "Q{uri}*" (flag
       BRACED_URI, payload name.namespace_uri), LOCAL_NAME_WILDCARD "*:n"
       (payload name.local_name). Also the "*" selector of jnode(). */
    SXML_AST_KIND_WILDCARD,

    /* Postfix expressions (4.4, 4.5, 4.7). */
    /* value: Expression; predicate: PREDICATE. */
    SXML_AST_KIND_FILTER_EXPRESSION,
    /* expression: Expression. */
    SXML_AST_KIND_PREDICATE,
    /* function: Expression; arguments: POSITIONAL_ARGUMENT_LIST. */
    SXML_AST_KIND_DYNAMIC_FUNCTION_CALL,
    /* value: Expression; lookup: LOOKUP. "E?key". */
    SXML_AST_KIND_LOOKUP_EXPRESSION,
    /* key: NCNAME, a literal, CONTEXT_VALUE_REFERENCE,
       VARIABLE_REFERENCE, PARENTHESIZED_EXPRESSION ("?(E)") or
       LOOKUP_WILDCARD ("?*"). A PREFIXED_NAME key is XPST0003 (D29) and
       never reaches the AST. */
    SXML_AST_KIND_LOOKUP,
    /* key: as LOOKUP. "?key" with the context value as the left operand. */
    SXML_AST_KIND_UNARY_LOOKUP,
    /* No fields: "*" as a key. */
    SXML_AST_KIND_LOOKUP_WILDCARD,
    /* value: Expression; name: NCNAME; arguments:
       POSITIONAL_ARGUMENT_LIST. "E =?> name(...)". */
    SXML_AST_KIND_METHOD_CALL,

    /* Function calls and function items (4.5). */
    /* name: Name (never a reserved function name, A.4); arguments:
       ARGUMENT_LIST. */
    SXML_AST_KIND_FUNCTION_CALL,
    /* argument*: Expression or ARGUMENT_PLACEHOLDER, then KEYWORD_ARGUMENT
       nodes, in source order. */
    SXML_AST_KIND_ARGUMENT_LIST,
    /* argument*: Expression or ARGUMENT_PLACEHOLDER. */
    SXML_AST_KIND_POSITIONAL_ARGUMENT_LIST,
    /* No fields: "?" as an argument (partial function application,
       4.5.2.5). */
    SXML_AST_KIND_ARGUMENT_PLACEHOLDER,
    /* name: Name; value: Expression or ARGUMENT_PLACEHOLDER. "n := E". */
    SXML_AST_KIND_KEYWORD_ARGUMENT,
    /* name: Name (an EQName, such as Q{uri}f); arity: INTEGER_LITERAL.
       "f#2". */
    SXML_AST_KIND_NAMED_FUNCTION_REFERENCE,
    /* annotation*: ANNOTATION (XQuery); parameter*:
       VARIABLE_NAME_AND_TYPE; return_type?: TYPE_DECLARATION; body:
       ENCLOSED_EXPRESSION. Flag HAS_SIGNATURE when "(...)" was
       written: without it, "fn { E }" is a focus function of one
       parameter that becomes the context value (4.5.6). "function" and
       "fn" are not distinguished. */
    SXML_AST_KIND_INLINE_FUNCTION_EXPRESSION,

    /* Maps and arrays (4.7). */
    /* entry*: MAP_CONSTRUCTOR_ENTRY. "map { }" and "{ }" are not
       distinguished. */
    SXML_AST_KIND_MAP_CONSTRUCTOR,
    /* key: Expression; value?: Expression. Without a value the key
       expression supplies maps whose entries are added. */
    SXML_AST_KIND_MAP_CONSTRUCTOR_ENTRY,
    /* member*: Expression. "[a, b]": each member one array member. */
    SXML_AST_KIND_SQUARE_ARRAY_CONSTRUCTOR,
    /* content: ENCLOSED_EXPRESSION. "array { E }": each item a member. */
    SXML_AST_KIND_CURLY_ARRAY_CONSTRUCTOR,

    /* Computed node constructors (4.10). Lowered in Phase 3. */
    /* content: ENCLOSED_EXPRESSION. */
    SXML_AST_KIND_COMPUTED_DOCUMENT_CONSTRUCTOR,
    SXML_AST_KIND_COMPUTED_TEXT_CONSTRUCTOR,
    SXML_AST_KIND_COMPUTED_COMMENT_CONSTRUCTOR,
    /* name?: QNAME_LITERAL (in XQuery also a Name) or name_expression?:
       Expression (one of them); content: ENCLOSED_EXPRESSION. */
    SXML_AST_KIND_COMPUTED_ELEMENT_CONSTRUCTOR,
    SXML_AST_KIND_COMPUTED_ATTRIBUTE_CONSTRUCTOR,
    /* name?: MARKED_NCNAME (in XQuery also an NCNAME) or
       name_expression?: Expression (one of them); content:
       ENCLOSED_EXPRESSION. */
    SXML_AST_KIND_COMPUTED_NAMESPACE_CONSTRUCTOR,
    SXML_AST_KIND_COMPUTED_PROCESSING_INSTRUCTION_CONSTRUCTOR,
    /* name: NCNAME. "#name". */
    SXML_AST_KIND_MARKED_NCNAME,

    /* --- Sequence types and item types (section 3) --- */
    /* type: SEQUENCE_TYPE. "as T". */
    SXML_AST_KIND_TYPE_DECLARATION,
    /* item_type?: ItemType. Detail occurrence: EXACTLY_ONE when no
       indicator is written, ZERO_OR_ONE "?", ZERO_OR_MORE "*", ONE_OR_MORE
       "+", and ZERO for "empty-sequence()", which has no item_type. */
    SXML_AST_KIND_SEQUENCE_TYPE,
    /* No fields: item(). */
    SXML_AST_KIND_ANY_ITEM_TYPE,
    /* No fields: node(). */
    SXML_AST_KIND_GNODE_TYPE,
    /* selector?: WILDCARD, JROOT_SELECTOR, NCNAME or a constant
       (STRING_LITERAL, a numeric literal, NEGATIVE_NUMERIC_LITERAL,
       QNAME_LITERAL, BOOLEAN_CONSTANT); type?: SEQUENCE_TYPE. */
    SXML_AST_KIND_JNODE_TYPE,
    /* No fields: "()" as a jnode() selector. */
    SXML_AST_KIND_JROOT_SELECTOR,
    /* content?: ELEMENT_NODE_TYPE, SCHEMA_ELEMENT_NODE_TYPE or
       NAME_TEST_UNION. */
    SXML_AST_KIND_DOCUMENT_NODE_TYPE,
    /* name?: NAME_TEST_UNION; type?: Name. Flag NILLABLE for "?" after
       the type. */
    SXML_AST_KIND_ELEMENT_NODE_TYPE,
    /* name?: NAME_TEST_UNION; type?: Name. */
    SXML_AST_KIND_ATTRIBUTE_NODE_TYPE,
    /* name: Name. */
    SXML_AST_KIND_SCHEMA_ELEMENT_NODE_TYPE,
    SXML_AST_KIND_SCHEMA_ATTRIBUTE_NODE_TYPE,
    /* name?: NCNAME or STRING_LITERAL. */
    SXML_AST_KIND_PROCESSING_INSTRUCTION_NODE_TYPE,
    /* No fields. */
    SXML_AST_KIND_COMMENT_NODE_TYPE,
    SXML_AST_KIND_TEXT_NODE_TYPE,
    SXML_AST_KIND_NAMESPACE_NODE_TYPE,
    SXML_AST_KIND_ANY_XNODE_TYPE,
    /* test+: Name or WILDCARD. */
    SXML_AST_KIND_NAME_TEST_UNION,
    /* No fields: map(*), array(*), record(*). */
    SXML_AST_KIND_ANY_MAP_TYPE,
    SXML_AST_KIND_ANY_ARRAY_TYPE,
    SXML_AST_KIND_ANY_RECORD_TYPE,
    /* key: ItemType; value: SEQUENCE_TYPE. map(K, V). */
    SXML_AST_KIND_TYPED_MAP_TYPE,
    /* member: SEQUENCE_TYPE. array(T), array(empty-sequence()). */
    SXML_AST_KIND_TYPED_ARRAY_TYPE,
    /* field*: FIELD_DECLARATION. */
    SXML_AST_KIND_TYPED_RECORD_TYPE,
    /* name: NCNAME or STRING_LITERAL; type?: SEQUENCE_TYPE. */
    SXML_AST_KIND_FIELD_DECLARATION,
    /* value+: STRING_LITERAL. enum("a", "b"). */
    SXML_AST_KIND_ENUMERATION_TYPE,
    /* annotation*: ANNOTATION (XQuery). function(*) or fn(*). */
    SXML_AST_KIND_ANY_FUNCTION_TYPE,
    /* annotation*: ANNOTATION (XQuery); parameter*:
       TYPED_FUNCTION_PARAMETER; return_type: SEQUENCE_TYPE. function(T) as
       U. */
    SXML_AST_KIND_TYPED_FUNCTION_TYPE,
    /* name?: Name; type: SEQUENCE_TYPE. "$name as T" or "T". */
    SXML_AST_KIND_TYPED_FUNCTION_PARAMETER,
    /* alternative+: ItemType. "(A | B)". */
    SXML_AST_KIND_CHOICE_ITEM_TYPE,

    /* --- Names (A.3.1). Payload name, as written. --- */
    /* local_name only (also a keyword used as a name). */
    SXML_AST_KIND_NCNAME,
    /* namespace_prefix and local_name: "p:n". */
    SXML_AST_KIND_PREFIXED_NAME,
    /* namespace_uri and local_name: "Q{uri}n". A "Q{uri}p:n" form also
       sets namespace_prefix. */
    SXML_AST_KIND_URI_QUALIFIED_NAME,

    /* ====== XQuery 4.0 (D111). Section numbers of XQuery 4.0 (QT4 CG
       draft, 13 October 2026); productions of its Appendix A.1. Built by
       sxml_xquery_build_ast (src/xquery_parse.c, packet 3.2). ====== */

    /* --- Modules and the prolog (section 5) --- */
    /* version?: VERSION_DECLARATION; module: MAIN_MODULE or
       LIBRARY_MODULE. The grammar gives the module no field; the front
       end puts it in field MODULE (sxml_ast_unnamed_child_entry). The root
       of every XQuery AST. */
    SXML_AST_KIND_MODULE,
    /* version?: STRING_LITERAL; encoding?: STRING_LITERAL (at least one).
       5.1. */
    SXML_AST_KIND_VERSION_DECLARATION,
    /* prolog?: PROLOG; body: Expression. Section 5, MainModule. */
    SXML_AST_KIND_MAIN_MODULE,
    /* declaration: MODULE_DECLARATION; prolog?: PROLOG. 5.2. */
    SXML_AST_KIND_LIBRARY_MODULE,
    /* prefix: NCNAME; uri: STRING_LITERAL. 5.2. */
    SXML_AST_KIND_MODULE_DECLARATION,
    /* declaration+: the declaration kinds below and NAMESPACE_DECLARATION,
       in source order (the grammar keeps setters and imports before the
       other declarations). */
    SXML_AST_KIND_PROLOG,
    /* uri: STRING_LITERAL. Detail keyword ELEMENT or FUNCTION; flag FIXED.
       5.14. */
    SXML_AST_KIND_DEFAULT_NAMESPACE_DECLARATION,
    /* No fields. Detail keyword PRESERVE or STRIP. 5.3. */
    SXML_AST_KIND_BOUNDARY_SPACE_DECLARATION,
    /* uri: STRING_LITERAL. 5.4. */
    SXML_AST_KIND_DEFAULT_COLLATION_DECLARATION,
    /* uri: STRING_LITERAL. 5.5. */
    SXML_AST_KIND_BASE_URI_DECLARATION,
    /* No fields. Detail keyword PRESERVE or STRIP. 5.6. */
    SXML_AST_KIND_CONSTRUCTION_DECLARATION,
    /* No fields. Detail keyword ORDERED or UNORDERED. 5.7. */
    SXML_AST_KIND_ORDERING_MODE_DECLARATION,
    /* No fields. Detail keyword GREATEST or LEAST. 5.8. */
    SXML_AST_KIND_EMPTY_ORDER_DECLARATION,
    /* No fields. Flag PRESERVE_NAMESPACES for "preserve" (absent for
       "no-preserve"), INHERIT_NAMESPACES for "inherit". 5.9. */
    SXML_AST_KIND_COPY_NAMESPACES_DECLARATION,
    /* name?: Name (absent for "declare default decimal-format");
       property*: DECIMAL_FORMAT_PROPERTY. 5.10. */
    SXML_AST_KIND_DECIMAL_FORMAT_DECLARATION,
    /* value: STRING_LITERAL. Detail decimal_format_property, the property
       name written. */
    SXML_AST_KIND_DECIMAL_FORMAT_PROPERTY,
    /* prefix?: SCHEMA_PREFIX; uri: STRING_LITERAL; location*:
       STRING_LITERAL. 5.11; lowering raises XQST0009 (D97: no schema
       awareness). */
    SXML_AST_KIND_SCHEMA_IMPORT,
    /* prefix?: NCNAME ("namespace p ="; absent for "default element
       namespace"). Flag FIXED. */
    SXML_AST_KIND_SCHEMA_PREFIX,
    /* prefix?: NCNAME; uri: STRING_LITERAL; location*: STRING_LITERAL.
       5.12. */
    SXML_AST_KIND_MODULE_IMPORT,
    /* type?: SEQUENCE_TYPE (detail keyword VALUE) or ItemType (keyword
       ITEM); value?: Expression; default?: Expression. Flag EXTERNAL; value
       only without it, default only with it. 5.17. */
    SXML_AST_KIND_CONTEXT_VALUE_DECLARATION,
    /* annotation*: ANNOTATION; variable: VARIABLE_NAME_AND_TYPE; value?:
       Expression; default?: Expression. Flag EXTERNAL as for the context
       value. 5.16. */
    SXML_AST_KIND_VARIABLE_DECLARATION,
    /* annotation*: ANNOTATION; name: Name; parameter*:
       PARAMETER_WITH_DEFAULT; return_type?: TYPE_DECLARATION; body?:
       ENCLOSED_EXPRESSION (absent with flag EXTERNAL). 5.18. */
    SXML_AST_KIND_FUNCTION_DECLARATION,
    /* variable: VARIABLE_NAME_AND_TYPE; default?: Expression. 5.18.3. */
    SXML_AST_KIND_PARAMETER_WITH_DEFAULT,
    /* annotation*: ANNOTATION; name: Name; type: ItemType. 5.19. */
    SXML_AST_KIND_ITEM_TYPE_DECLARATION,
    /* annotation*: ANNOTATION; name: Name; field*:
       EXTENDED_FIELD_DECLARATION. 5.20. */
    SXML_AST_KIND_NOMINATIVE_RECORD_TYPE_DECLARATION,
    /* name: NCNAME or STRING_LITERAL; type?: SEQUENCE_TYPE; default?:
       Expression. Flag STATIC for "%static". */
    SXML_AST_KIND_EXTENDED_FIELD_DECLARATION,
    /* name: Name; value: STRING_LITERAL. 5.21. */
    SXML_AST_KIND_OPTION_DECLARATION,
    /* name: Name; value*: Literal, NEGATIVE_NUMERIC_LITERAL, QNAME_LITERAL
       or BOOLEAN_CONSTANT. "%name(...)", 5.15; also on inline functions
       and function types. */
    SXML_AST_KIND_ANNOTATION,

    /* --- FLWOR clauses (4.13), children with field clause of
       FLWOR_EXPRESSION, besides FOR_CLAUSE and LET_CLAUSE --- */
    /* variable: VARIABLE_NAME_AND_TYPE; sequence: Expression; start?:
       WINDOW_START_CONDITION; end?: WINDOW_END_CONDITION. 4.13.4.1. */
    SXML_AST_KIND_TUMBLING_WINDOW_CLAUSE,
    /* As TUMBLING_WINDOW_CLAUSE with end required. 4.13.4.2. */
    SXML_AST_KIND_SLIDING_WINDOW_CLAUSE,
    /* current?: Name; position?: POSITIONAL_VARIABLE; previous?: Name;
       next?: Name; condition?: Expression ("when"). */
    SXML_AST_KIND_WINDOW_START_CONDITION,
    /* As WINDOW_START_CONDITION; flag ONLY for "only end". */
    SXML_AST_KIND_WINDOW_END_CONDITION,
    /* condition: Expression. 4.13.5. */
    SXML_AST_KIND_WHERE_CLAUSE,
    /* condition: Expression. 4.13.6. */
    SXML_AST_KIND_WHILE_CLAUSE,
    /* name: Name. "count $n", 4.13.7. */
    SXML_AST_KIND_COUNT_CLAUSE,
    /* specification+: GROUPING_SPECIFICATION. 4.13.8. */
    SXML_AST_KIND_GROUP_BY_CLAUSE,
    /* name: Name; type?: TYPE_DECLARATION; value?: Expression (":=");
       collation?: STRING_LITERAL. */
    SXML_AST_KIND_GROUPING_SPECIFICATION,
    /* specification+: ORDER_SPECIFICATION. Flag STABLE. 4.13.9. */
    SXML_AST_KIND_ORDER_BY_CLAUSE,
    /* key: Expression; collation?: STRING_LITERAL. Flag DESCENDING (absent:
       ascending), EMPTY_GREATEST or EMPTY_LEAST (neither: the prolog's
       default order for empty sequences). */
    SXML_AST_KIND_ORDER_SPECIFICATION,
    /* value: Expression. "trace E", 4.13.10. */
    SXML_AST_KIND_TRACE_CLAUSE,

    /* --- Other expressions --- */
    /* comparand?: Expression (absent in the 4.0 "switch ()" form);
       case+: SWITCH_CASE_CLAUSE; default: Expression. The braced form is
       not distinguished. 4.18. */
    SXML_AST_KIND_SWITCH_EXPRESSION,
    /* operand+: Expression (one per "case"); value: Expression. */
    SXML_AST_KIND_SWITCH_CASE_CLAUSE,
    /* operand: Expression; case+: TYPESWITCH_CASE_CLAUSE; default:
       TYPESWITCH_DEFAULT_CLAUSE. 4.21.2. */
    SXML_AST_KIND_TYPESWITCH_EXPRESSION,
    /* variable?: Name; type+: SEQUENCE_TYPE (a union "T1 | T2"); value:
       Expression. */
    SXML_AST_KIND_TYPESWITCH_CASE_CLAUSE,
    /* variable?: Name; value: Expression. */
    SXML_AST_KIND_TYPESWITCH_DEFAULT_CLAUSE,
    /* body: ENCLOSED_EXPRESSION; catch*: CATCH_CLAUSE; finally?:
       FINALLY_CLAUSE (at least one of them). 4.20. */
    SXML_AST_KIND_TRY_CATCH_EXPRESSION,
    /* errors: NAME_TEST_UNION; body: ENCLOSED_EXPRESSION. */
    SXML_AST_KIND_CATCH_CLAUSE,
    /* body: ENCLOSED_EXPRESSION. */
    SXML_AST_KIND_FINALLY_CLAUSE,
    /* type?: Name ("validate type T"); expression: Expression. Detail
       keyword LAX, STRICT or NONE. 4.25; lowering raises XQST0075 (D97). */
    SXML_AST_KIND_VALIDATE_EXPRESSION,
    /* pragma+: PRAGMA; expression?: Expression. 4.26. */
    SXML_AST_KIND_EXTENSION_EXPRESSION,
    /* name: Name; contents?: PRAGMA_CONTENTS. "(# name contents #)". */
    SXML_AST_KIND_PRAGMA,
    /* Payload text: the contents as written, without the whitespace that
       separates them from the name. */
    SXML_AST_KIND_PRAGMA_CONTENTS,
    /* content: ENCLOSED_EXPRESSION. 4.15. */
    SXML_AST_KIND_ORDERED_EXPRESSION,
    SXML_AST_KIND_UNORDERED_EXPRESSION,
    /* part*: STRING_CONSTRUCTOR_CHARACTERS or STRING_INTERPOLATION.
       "``[...]``", 4.9.3. */
    SXML_AST_KIND_STRING_CONSTRUCTOR,
    /* Payload text as written (string constructors have no escapes). */
    SXML_AST_KIND_STRING_CONSTRUCTOR_CHARACTERS,
    /* expression?: Expression. "`{E}`". */
    SXML_AST_KIND_STRING_INTERPOLATION,

    /* --- Direct constructors (4.12.1, 4.12.2) --- */
    /* name: QNAME; attribute*: DIRECT_ATTRIBUTE; content*:
       DIRECT_ELEMENT_TEXT, CDATA_SECTION, ENCLOSED_EXPRESSION or a
       direct constructor kind, in source order; end_name?: QNAME (absent
       for "<a/>"; the front end checks it equals name, XQST0118). */
    SXML_AST_KIND_DIRECT_ELEMENT_CONSTRUCTOR,
    /* name: QNAME; value: DIRECT_ATTRIBUTE_VALUE. Flag
       NAMESPACE_DECLARATION_ATTRIBUTE for "xmlns" and "xmlns:p" (4.12.1.2;
       its value must be all ATTRIBUTE_VALUE_TEXT, XQST0022). The front end
       raises XQST0040 for two attributes of one element with the same
       name as written; same expanded names are lowering's (XQST0040). */
    SXML_AST_KIND_DIRECT_ATTRIBUTE,
    /* part*: ATTRIBUTE_VALUE_TEXT or ENCLOSED_EXPRESSION. "" has none. */
    SXML_AST_KIND_DIRECT_ATTRIBUTE_VALUE,
    /* Payload text, decoded (4.12.1.1): the doubled delimiter, "{{" and
       "}}" unescaped, predefined entity and character references replaced
       (XQST0090 for a reference to a character XML does not allow), then
       each literal tab, line feed and carriage return a space (characters
       from references are kept). */
    SXML_AST_KIND_ATTRIBUTE_VALUE_TEXT,
    /* Payload text, decoded: "{{" and "}}" unescaped, references
       replaced (XQST0090). Flag BOUNDARY_WHITESPACE when it is boundary
       whitespace (4.12.1.4: only whitespace written as such, delimited by
       the start or end of the content, a direct constructor or an
       enclosed expression); lowering drops it unless boundary-space is
       preserve. */
    SXML_AST_KIND_DIRECT_ELEMENT_TEXT,
    /* Payload text: the characters between "<![CDATA[" and "]]>". */
    SXML_AST_KIND_CDATA_SECTION,
    /* Payload text: the characters between "<!--" and "-->". 4.12.2. */
    SXML_AST_KIND_DIRECT_COMMENT_CONSTRUCTOR,
    /* Payload processing_instruction: the target and the content after
       the whitespace that follows it (empty when none). 4.12.2; a target
       "xml" in any case is XPST0003. */
    SXML_AST_KIND_DIRECT_PROCESSING_INSTRUCTION_CONSTRUCTOR,
    /* Payload name: namespace_prefix and local_name as written in a tag
       or attribute ("p:n" or "n"). */
    SXML_AST_KIND_QNAME,

    /* ====== XSLT 4.0 patterns (D118). Section numbers of XSLT 4.0 (QT4
       CG draft, October 2026), section 6.3 and Appendix E; one kind per
       `..._pattern` node type of grammars/xslt_pattern (D29). Built by
       sxml_xslt_pattern_build_ast (src/xslt_pattern.c, packet 4.4).
       Predicates, node tests, names and constants inside them are the
       XPath kinds. ====== */
    /* pattern: a pattern kind. The root of every pattern AST. */
    SXML_AST_KIND_PATTERN,
    /* left, right: pattern kinds. Detail operator UNION. 6.3.2. */
    SXML_AST_KIND_UNION_PATTERN,
    /* left, right: pattern kinds. Detail operator INTERSECT or EXCEPT. */
    SXML_AST_KIND_INTERSECT_EXCEPT_PATTERN,
    /* variable: VARIABLE_REFERENCE; predicate*: PREDICATE; path?: a
       pattern kind. Detail operator SLASH or DOUBLE_SLASH before the
       path (NONE without one). "$v[1]//a". */
    SXML_AST_KIND_ROOTED_PATH_PATTERN,
    /* path?: a pattern kind. Detail operator SLASH ("/" alone has no
       path) or DOUBLE_SLASH. */
    SXML_AST_KIND_ABSOLUTE_PATH_PATTERN,
    /* left, right: pattern kinds. Detail operator SLASH or DOUBLE_SLASH. */
    SXML_AST_KIND_RELATIVE_PATH_PATTERN,
    /* value: FUNCTION_CALL_PATTERN or PARENTHESIZED_PATTERN; predicate+:
       PREDICATE. */
    SXML_AST_KIND_FILTER_PATTERN,
    /* name: Name; argument*: VARIABLE_REFERENCE or a constant
       (STRING_LITERAL, a numeric literal, NEGATIVE_NUMERIC_LITERAL,
       QNAME_LITERAL, BOOLEAN_CONSTANT). "id('a')". */
    SXML_AST_KIND_FUNCTION_CALL_PATTERN,
    /* pattern: a pattern kind. */
    SXML_AST_KIND_PARENTHESIZED_PATTERN,
    /* test: a node test; predicate*: PREDICATE. Detail axis, as for
       AXIS_STEP (only the forward axes of ForwardAxisP); flag ABBREVIATED
       for "@test". */
    SXML_AST_KIND_AXIS_STEP_PATTERN,
    /* item: ANY_ITEM_PATTERN, ATOMIC_PATTERN, MAP_PATTERN,
       RECORD_PATTERN, ARRAY_PATTERN or JNODE_PATTERN; predicate*:
       PREDICATE. 6.3.3. */
    SXML_AST_KIND_SIMPLE_PATTERN,
    /* No fields: ".". */
    SXML_AST_KIND_ANY_ITEM_PATTERN,
    /* type+: Name. "atomic(xs:int | xs:long)", 6.3.4. */
    SXML_AST_KIND_ATOMIC_PATTERN,
    /* entry*: MAP_ENTRY_PATTERN. Flag OPEN for a trailing "*" (also "{*}"
       alone). 6.3.6. */
    SXML_AST_KIND_MAP_PATTERN,
    /* key: a constant; value?: SEQUENCE_PATTERN. */
    SXML_AST_KIND_MAP_ENTRY_PATTERN,
    /* name: Name. 6.3.7. */
    SXML_AST_KIND_RECORD_PATTERN,
    /* member: SEQUENCE_PATTERN. 6.3.8. */
    SXML_AST_KIND_ARRAY_PATTERN,
    /* selector: WILDCARD or a constant; value: SEQUENCE_PATTERN. 6.3.9. */
    SXML_AST_KIND_JNODE_PATTERN,
    /* pattern?: a pattern kind. Detail occurrence: 0 when no indicator is
       written (exactly one), ZERO for "empty-sequence()", which has no
       pattern, ZERO_OR_ONE, ZERO_OR_MORE, ONE_OR_MORE. 6.3.5. */
    SXML_AST_KIND_SEQUENCE_PATTERN,

    /* ====== XQuery Update Facility 3.0 (D187; section numbers of XQUF
       3.0, W3C Recommendation 24 January 2017). Built by the XQuery front
       end from grammars/xquery. ====== */
    /* No fields. Detail keyword STRICT, LAX or SKIP. 5.2. */
    SXML_AST_KIND_REVALIDATION_DECLARATION,
    /* No fields: "declare updating function", the same as %updating
       (5.4); field annotation of VARIABLE_DECLARATION or
       FUNCTION_DECLARATION. */
    SXML_AST_KIND_COMPATIBILITY_ANNOTATION,
    /* source, target: expressions. Detail keyword FIRST or LAST ("as
       first into", "as last into"), INTO, AFTER or BEFORE. 5.4.1. */
    SXML_AST_KIND_INSERT_EXPRESSION,
    /* target: an expression. 5.4.2. */
    SXML_AST_KIND_DELETE_EXPRESSION,
    /* target, value: expressions. Flag VALUE_OF for "replace value of
       node". 5.4.3. */
    SXML_AST_KIND_REPLACE_EXPRESSION,
    /* target, name_expression: expressions. 5.4.4. */
    SXML_AST_KIND_RENAME_EXPRESSION,
    /* binding+: COPY_BINDING; modify, return: expressions. 5.4.5. */
    SXML_AST_KIND_COPY_MODIFY_EXPRESSION,
    /* name: Name; value: an expression. "$a := E" of a copy clause. */
    SXML_AST_KIND_COPY_BINDING,
    /* value: an expression; modify?: an expression. "E transform with
       { M }", 5.4.6. */
    SXML_AST_KIND_TRANSFORM_WITH_EXPRESSION,
    /* function: a primary expression; arguments:
       POSITIONAL_ARGUMENT_LIST. "invoke updating $f(1)", 5.3. */
    SXML_AST_KIND_UPDATING_FUNCTION_CALL,

    /* ====== XQuery and XPath Full Text 3.0 (D214; section numbers of
       Full Text 3.0, W3C Recommendation 24 November 2015). Built by the
       XQuery front end from grammars/xquery. A full-text selection kind
       is FT_SELECTION, FT_OR, FT_AND, FT_MILD_NOT, FT_UNARY_NOT,
       FT_PRIMARY_WITH_OPTIONS, FT_WORDS or FT_EXTENSION_SELECTION; a
       positional filter kind FT_ORDER, FT_WINDOW, FT_DISTANCE, FT_SCOPE or
       FT_CONTENT; a match option kind one of FT_LANGUAGE_OPTION to
       FT_EXTENSION_OPTION. ====== */
    /* option+: a match option kind. "declare ft-option", 2.1. */
    SXML_AST_KIND_FT_OPTION_DECLARATION,
    /* name: Name. "score $s", field score of FOR_ITEM_BINDING, 2.3.1. */
    SXML_AST_KIND_FT_SCORE_VARIABLE,
    /* name: Name; value: an expression. "let score $s := E", a binding
       of LET_CLAUSE, 2.3.2. */
    SXML_AST_KIND_LET_SCORE_BINDING,
    /* value: an expression; selection: FT_SELECTION; ignore?: an
       expression ("without content E", 3.7). "E contains text S", 2.2. */
    SXML_AST_KIND_FT_CONTAINS_EXPRESSION,
    /* operand: a full-text selection kind; filter*: a positional filter
       kind, in source order. 3.1; also a parenthesized selection. */
    SXML_AST_KIND_FT_SELECTION,
    /* left, right: full-text selection kinds. "ftor", "ftand" and
       "not in", 3.1.1 to 3.1.3. */
    SXML_AST_KIND_FT_OR,
    SXML_AST_KIND_FT_AND,
    SXML_AST_KIND_FT_MILD_NOT,
    /* operand: a full-text selection kind. "ftnot", 3.1.4. */
    SXML_AST_KIND_FT_UNARY_NOT,
    /* operand: a full-text selection kind; option*: a match option kind;
       weight?: an expression (at least one of them). 3.1.5, 3.4. */
    SXML_AST_KIND_FT_PRIMARY_WITH_OPTIONS,
    /* value: STRING_LITERAL or an expression ("{E}"); times?: FT_RANGE
       ("occurs ... times", 3.3). Detail keyword ANY, ALL or PHRASE (NONE
       when no FTAnyallOption is written: any); flag SEPARATE_WORDS for
       "any word" and "all words". 3.2. */
    SXML_AST_KIND_FT_WORDS,
    /* value?: "exactly"; minimum?: "at least" and "from"; maximum?: "at
       most" and "to" (one of the four forms). Expressions in FTRange,
       INTEGER_LITERAL nodes in FTLiteralRange (3.4.3). */
    SXML_AST_KIND_FT_RANGE,
    /* pragma+: PRAGMA; selection?: FT_SELECTION. 3.6. */
    SXML_AST_KIND_FT_EXTENSION_SELECTION,
    /* No fields: "ordered", 3.5.1. */
    SXML_AST_KIND_FT_ORDER,
    /* value: an expression. Detail keyword WORDS, SENTENCES or
       PARAGRAPHS. 3.5.2. */
    SXML_AST_KIND_FT_WINDOW,
    /* range: FT_RANGE. Detail keyword as FT_WINDOW. 3.5.3. */
    SXML_AST_KIND_FT_DISTANCE,
    /* No fields. Detail keyword SENTENCE or PARAGRAPH; flag DIFFERENT for
       "different" (absent: "same"). 3.5.4. */
    SXML_AST_KIND_FT_SCOPE,
    /* No fields. Detail keyword START, END or ENTIRE. 3.5.5. */
    SXML_AST_KIND_FT_CONTENT,
    /* language: STRING_LITERAL. 3.4.1. */
    SXML_AST_KIND_FT_LANGUAGE_OPTION,
    /* No fields. Flag NO for "no wildcards". 3.4.2. */
    SXML_AST_KIND_FT_WILDCARD_OPTION,
    /* thesaurus*: FT_THESAURUS_ID. Flag NO for "no thesaurus", DEFAULT
       when "default" is written. 3.4.3. */
    SXML_AST_KIND_FT_THESAURUS_OPTION,
    /* uri: STRING_LITERAL; relationship?: STRING_LITERAL; range?:
       FT_RANGE ("... levels"). */
    SXML_AST_KIND_FT_THESAURUS_ID,
    /* No fields. Flag NO for "no stemming". 3.4.4. */
    SXML_AST_KIND_FT_STEM_OPTION,
    /* No fields. Detail keyword INSENSITIVE, SENSITIVE, LOWERCASE or
       UPPERCASE. 3.4.5. */
    SXML_AST_KIND_FT_CASE_OPTION,
    /* No fields. Detail keyword INSENSITIVE or SENSITIVE. 3.4.6. */
    SXML_AST_KIND_FT_DIACRITICS_OPTION,
    /* words?: FT_STOP_WORDS; inclusion*: FT_STOP_WORDS_INCLUSION. Flag
       NO for "no stop words", DEFAULT for "stop words default". 3.4.7. */
    SXML_AST_KIND_FT_STOP_WORD_OPTION,
    /* uri?: STRING_LITERAL ("at URI") or word+: STRING_LITERAL. */
    SXML_AST_KIND_FT_STOP_WORDS,
    /* words: FT_STOP_WORDS. Detail operator UNION or EXCEPT. */
    SXML_AST_KIND_FT_STOP_WORDS_INCLUSION,
    /* name: Name; value: STRING_LITERAL. 3.4.8. */
    SXML_AST_KIND_FT_EXTENSION_OPTION,

    /* One more than the largest kind; grows with Phases 3 and 4. */
    SXML_AST_KIND_COUNT,
} sxml_ast_kind;

/*
 * The role of a child in its parent: a field name of the grammar
 * (grammars/xpath/grammar.js). Values are only ever appended.
 */
typedef enum sxml_ast_field {
    /* The root, which has no parent. */
    SXML_AST_FIELD_NONE = 0,
    SXML_AST_FIELD_ALTERNATIVE,
    SXML_AST_FIELD_ARGUMENT,
    SXML_AST_FIELD_ARGUMENTS,
    SXML_AST_FIELD_ARITY,
    SXML_AST_FIELD_BINDING,
    SXML_AST_FIELD_BODY,
    SXML_AST_FIELD_CLAUSE,
    SXML_AST_FIELD_CONDITION,
    SXML_AST_FIELD_CONTENT,
    SXML_AST_FIELD_DECLARATION,
    SXML_AST_FIELD_ELSE,
    SXML_AST_FIELD_ENTRY,
    SXML_AST_FIELD_EXPRESSION,
    /* A field_declaration of a record type ("field" in the grammar). */
    SXML_AST_FIELD_FIELD,
    SXML_AST_FIELD_FUNCTION,
    SXML_AST_FIELD_ITEM,
    SXML_AST_FIELD_ITEM_TYPE,
    SXML_AST_FIELD_KEY,
    SXML_AST_FIELD_LEFT,
    SXML_AST_FIELD_LOOKUP,
    SXML_AST_FIELD_MEMBER,
    SXML_AST_FIELD_NAME,
    SXML_AST_FIELD_NAME_EXPRESSION,
    SXML_AST_FIELD_OPERAND,
    SXML_AST_FIELD_PARAMETER,
    SXML_AST_FIELD_PART,
    SXML_AST_FIELD_PATH,
    SXML_AST_FIELD_POSITION,
    SXML_AST_FIELD_PREDICATE,
    SXML_AST_FIELD_PREFIX,
    SXML_AST_FIELD_RETURN,
    SXML_AST_FIELD_RETURN_TYPE,
    SXML_AST_FIELD_RIGHT,
    SXML_AST_FIELD_SELECTOR,
    SXML_AST_FIELD_SEQUENCE,
    SXML_AST_FIELD_TARGET,
    SXML_AST_FIELD_TEST,
    SXML_AST_FIELD_THEN,
    SXML_AST_FIELD_TYPE,
    SXML_AST_FIELD_URI,
    SXML_AST_FIELD_VALUE,
    SXML_AST_FIELD_VARIABLE,

    /* XQuery 4.0 (D111): the fields of grammars/xquery that hold named
       nodes and are not XPath's. */
    SXML_AST_FIELD_ANNOTATION,
    SXML_AST_FIELD_ATTRIBUTE,
    SXML_AST_FIELD_CASE,
    SXML_AST_FIELD_CATCH,
    SXML_AST_FIELD_COLLATION,
    SXML_AST_FIELD_COMPARAND,
    SXML_AST_FIELD_CONTENTS,
    SXML_AST_FIELD_CURRENT,
    SXML_AST_FIELD_DEFAULT,
    SXML_AST_FIELD_ENCODING,
    SXML_AST_FIELD_END,
    SXML_AST_FIELD_END_NAME,
    SXML_AST_FIELD_ERRORS,
    SXML_AST_FIELD_FINALLY,
    SXML_AST_FIELD_LOCATION,
    /* Not a grammar field: the child of MODULE, which has none. */
    SXML_AST_FIELD_MODULE,
    SXML_AST_FIELD_NEXT,
    SXML_AST_FIELD_PRAGMA,
    SXML_AST_FIELD_PREVIOUS,
    SXML_AST_FIELD_PROLOG,
    SXML_AST_FIELD_PROPERTY,
    SXML_AST_FIELD_SPECIFICATION,
    SXML_AST_FIELD_START,
    SXML_AST_FIELD_VERSION,

    /* XSLT 4.0 patterns (D118). */
    SXML_AST_FIELD_PATTERN,

    /* XQuery Update Facility 3.0 (D187). */
    SXML_AST_FIELD_MODIFY,
    SXML_AST_FIELD_SOURCE,

    /* XQuery and XPath Full Text 3.0 (D214). */
    SXML_AST_FIELD_FILTER,
    SXML_AST_FIELD_IGNORE,
    SXML_AST_FIELD_INCLUSION,
    SXML_AST_FIELD_LANGUAGE,
    SXML_AST_FIELD_MAXIMUM,
    SXML_AST_FIELD_MINIMUM,
    SXML_AST_FIELD_OPTION,
    SXML_AST_FIELD_RANGE,
    SXML_AST_FIELD_RELATIONSHIP,
    SXML_AST_FIELD_SCORE,
    SXML_AST_FIELD_SELECTION,
    SXML_AST_FIELD_THESAURUS,
    SXML_AST_FIELD_TIMES,
    SXML_AST_FIELD_WEIGHT,
    SXML_AST_FIELD_WORD,
    SXML_AST_FIELD_WORDS,

    /* One more than the largest field; grows with Phases 3 and 4. */
    SXML_AST_FIELD_COUNT,
} sxml_ast_field;

/*
 * The operator token of an operator node, quantified expression or
 * absolute path. Synonymous glyphs share one value.
 */
typedef enum sxml_ast_operator {
    SXML_AST_OPERATOR_NONE = 0,
    SXML_AST_OPERATOR_OR,
    SXML_AST_OPERATOR_AND,
    /* Value comparisons (4.11.1). */
    SXML_AST_OPERATOR_VALUE_EQUAL,         /* eq */
    SXML_AST_OPERATOR_VALUE_NOT_EQUAL,     /* ne */
    SXML_AST_OPERATOR_VALUE_LESS,          /* lt */
    SXML_AST_OPERATOR_VALUE_LESS_OR_EQUAL, /* le */
    SXML_AST_OPERATOR_VALUE_GREATER,       /* gt */
    SXML_AST_OPERATOR_VALUE_GREATER_OR_EQUAL, /* ge */
    /* General comparisons (4.11.2). */
    SXML_AST_OPERATOR_GENERAL_EQUAL,            /* = */
    SXML_AST_OPERATOR_GENERAL_NOT_EQUAL,        /* != */
    SXML_AST_OPERATOR_GENERAL_LESS,             /* < */
    SXML_AST_OPERATOR_GENERAL_LESS_OR_EQUAL,    /* <= */
    SXML_AST_OPERATOR_GENERAL_GREATER,          /* > */
    SXML_AST_OPERATOR_GENERAL_GREATER_OR_EQUAL, /* >= */
    /* Node comparisons (4.11.3). */
    SXML_AST_OPERATOR_IS,             /* is */
    SXML_AST_OPERATOR_IS_NOT,         /* is-not */
    SXML_AST_OPERATOR_PRECEDES,       /* << or precedes */
    SXML_AST_OPERATOR_FOLLOWS,        /* >> or follows */
    SXML_AST_OPERATOR_PRECEDES_OR_IS, /* precedes-or-is */
    SXML_AST_OPERATOR_FOLLOWS_OR_IS,  /* follows-or-is */
    SXML_AST_OPERATOR_OTHERWISE,
    SXML_AST_OPERATOR_CONCATENATE, /* || */
    SXML_AST_OPERATOR_TO,
    /* Arithmetic (4.9); PLUS and MINUS also as unary operators. */
    SXML_AST_OPERATOR_PLUS,
    SXML_AST_OPERATOR_MINUS,
    SXML_AST_OPERATOR_MULTIPLY,         /* * or × */
    SXML_AST_OPERATOR_DIVIDE,           /* div or ÷ */
    SXML_AST_OPERATOR_INTEGER_DIVIDE,   /* idiv */
    SXML_AST_OPERATOR_MODULO,           /* mod */
    /* Sequence operators (4.8.4). */
    SXML_AST_OPERATOR_UNION, /* union or | */
    SXML_AST_OPERATOR_INTERSECT,
    SXML_AST_OPERATOR_EXCEPT,
    SXML_AST_OPERATOR_SIMPLE_MAP,     /* ! */
    SXML_AST_OPERATOR_SLASH,          /* / */
    SXML_AST_OPERATOR_DOUBLE_SLASH,   /* // */
    SXML_AST_OPERATOR_PIPELINE,       /* -> */
    SXML_AST_OPERATOR_SEQUENCE_ARROW, /* => */
    SXML_AST_OPERATOR_MAPPING_ARROW,  /* =!> */
    SXML_AST_OPERATOR_SOME,
    SXML_AST_OPERATOR_EVERY,
} sxml_ast_operator;

/*
 * The axis written in an axis step (XPath 4.0, section 4.6.4), in the
 * order of the grammar's AXES list. A separate enumeration from sxml_axis
 * (sxml/tree.h) because it records what was written, including no axis.
 */
typedef enum sxml_ast_axis {
    SXML_AST_AXIS_NONE = 0,
    SXML_AST_AXIS_ANCESTOR,
    SXML_AST_AXIS_ANCESTOR_OR_SELF,
    SXML_AST_AXIS_ATTRIBUTE,
    SXML_AST_AXIS_CHILD,
    SXML_AST_AXIS_DESCENDANT,
    SXML_AST_AXIS_DESCENDANT_OR_SELF,
    SXML_AST_AXIS_FOLLOWING,
    SXML_AST_AXIS_FOLLOWING_OR_SELF,
    SXML_AST_AXIS_FOLLOWING_SIBLING,
    SXML_AST_AXIS_FOLLOWING_SIBLING_OR_SELF,
    SXML_AST_AXIS_ITEM,
    SXML_AST_AXIS_NAMESPACE,
    SXML_AST_AXIS_PARENT,
    SXML_AST_AXIS_PRECEDING,
    SXML_AST_AXIS_PRECEDING_OR_SELF,
    SXML_AST_AXIS_PRECEDING_SIBLING,
    SXML_AST_AXIS_PRECEDING_SIBLING_OR_SELF,
    SXML_AST_AXIS_SELF,
} sxml_ast_axis;

/*
 * The keyword an XQuery declaration or expression chooses where the
 * grammar offers several (its `mode` and `kind` fields, D111).
 */
typedef enum sxml_ast_keyword {
    SXML_AST_KEYWORD_NONE = 0,
    /* boundary-space and construction declarations. */
    SXML_AST_KEYWORD_PRESERVE,
    SXML_AST_KEYWORD_STRIP,
    /* ordering mode declaration. */
    SXML_AST_KEYWORD_ORDERED,
    SXML_AST_KEYWORD_UNORDERED,
    /* empty order declaration. */
    SXML_AST_KEYWORD_GREATEST,
    SXML_AST_KEYWORD_LEAST,
    /* default namespace declaration. */
    SXML_AST_KEYWORD_ELEMENT,
    SXML_AST_KEYWORD_FUNCTION,
    /* context value declaration: "context value" or "context item". */
    SXML_AST_KEYWORD_VALUE,
    SXML_AST_KEYWORD_ITEM,
    /* validate expression and revalidation declaration. */
    SXML_AST_KEYWORD_LAX,
    SXML_AST_KEYWORD_STRICT,
    /* XQuery Update Facility 3.0 (D187): revalidation declaration. */
    SXML_AST_KEYWORD_SKIP,
    /* insert expression: "as first into", "as last into", "into",
       "after", "before". */
    SXML_AST_KEYWORD_FIRST,
    SXML_AST_KEYWORD_LAST,
    SXML_AST_KEYWORD_INTO,
    SXML_AST_KEYWORD_AFTER,
    SXML_AST_KEYWORD_BEFORE,
    /* Full Text 3.0 (D214): FTAnyallOption (3.2). */
    SXML_AST_KEYWORD_ANY,
    SXML_AST_KEYWORD_ALL,
    SXML_AST_KEYWORD_PHRASE,
    /* FTUnit and FTBigUnit (3.5.2 to 3.5.4). */
    SXML_AST_KEYWORD_WORDS,
    SXML_AST_KEYWORD_SENTENCES,
    SXML_AST_KEYWORD_PARAGRAPHS,
    SXML_AST_KEYWORD_SENTENCE,
    SXML_AST_KEYWORD_PARAGRAPH,
    /* FTContent (3.5.5). */
    SXML_AST_KEYWORD_START,
    SXML_AST_KEYWORD_END,
    SXML_AST_KEYWORD_ENTIRE,
    /* FTCaseOption and FTDiacriticsOption (3.4.5, 3.4.6). */
    SXML_AST_KEYWORD_INSENSITIVE,
    SXML_AST_KEYWORD_SENSITIVE,
    SXML_AST_KEYWORD_LOWERCASE,
    SXML_AST_KEYWORD_UPPERCASE,
} sxml_ast_keyword;

/*
 * The token a node records beyond its children. Which member is meaningful
 * is given by the kind (see sxml_ast_kind); every other kind leaves it
 * zero (operator SXML_AST_OPERATOR_NONE).
 */
typedef union sxml_ast_detail {
    sxml_ast_operator operator;
    sxml_ast_axis axis;
    sxml_occurrence occurrence;
    sxml_name_test_kind name_test_kind;
    sxml_ast_keyword keyword;
    sxml_decimal_format_property_kind decimal_format_property;
} sxml_ast_detail;

/* Flags of a node (bits of sxml_ast_node.flags); see the kinds. */
typedef enum sxml_ast_flag {
    /* AXIS_STEP written as "@test" or "..". */
    SXML_AST_FLAG_ABBREVIATED = 1U << 0U,
    /* CAST_TARGET with a trailing "?". */
    SXML_AST_FLAG_OPTIONAL = 1U << 1U,
    /* ELEMENT_NODE_TYPE with "?" after its type name. */
    SXML_AST_FLAG_NILLABLE = 1U << 2U,
    /* WILDCARD written "Q{uri}*". */
    SXML_AST_FLAG_BRACED_URI = 1U << 3U,
    /* BOOLEAN_CONSTANT "true()". */
    SXML_AST_FLAG_TRUE = 1U << 4U,
    /* INLINE_FUNCTION_EXPRESSION with a parameter list. */
    SXML_AST_FLAG_HAS_SIGNATURE = 1U << 5U,
    /* XQuery (D111). FOR_ITEM_BINDING with "allowing empty". */
    SXML_AST_FLAG_ALLOWING_EMPTY = 1U << 6U,
    /* DEFAULT_NAMESPACE_DECLARATION or SCHEMA_PREFIX with "fixed". */
    SXML_AST_FLAG_FIXED = 1U << 7U,
    /* CONTEXT_VALUE_DECLARATION, VARIABLE_DECLARATION or
       FUNCTION_DECLARATION with "external". */
    SXML_AST_FLAG_EXTERNAL = 1U << 8U,
    /* ORDER_BY_CLAUSE with "stable". */
    SXML_AST_FLAG_STABLE = 1U << 9U,
    /* ORDER_SPECIFICATION with "descending". */
    SXML_AST_FLAG_DESCENDING = 1U << 10U,
    /* ORDER_SPECIFICATION with "empty greatest" or "empty least". */
    SXML_AST_FLAG_EMPTY_GREATEST = 1U << 11U,
    SXML_AST_FLAG_EMPTY_LEAST = 1U << 12U,
    /* WINDOW_END_CONDITION with "only". */
    SXML_AST_FLAG_ONLY = 1U << 13U,
    /* EXTENDED_FIELD_DECLARATION with "%static". */
    SXML_AST_FLAG_STATIC = 1U << 14U,
    /* COPY_NAMESPACES_DECLARATION with "preserve" and with "inherit". */
    SXML_AST_FLAG_PRESERVE_NAMESPACES = 1U << 15U,
    SXML_AST_FLAG_INHERIT_NAMESPACES = 1U << 16U,
    /* DIRECT_ELEMENT_TEXT that is boundary whitespace (4.12.1.4). */
    SXML_AST_FLAG_BOUNDARY_WHITESPACE = 1U << 17U,
    /* DIRECT_ATTRIBUTE named xmlns or xmlns:p (4.12.1.2). */
    SXML_AST_FLAG_NAMESPACE_DECLARATION_ATTRIBUTE = 1U << 18U,
    /* XSLT patterns (D118). MAP_PATTERN with "*" ("{*}", "{'a', *}"). */
    SXML_AST_FLAG_OPEN = 1U << 19U,
    /* XQuery Update Facility 3.0 (D187). REPLACE_EXPRESSION written
       "replace value of node". */
    SXML_AST_FLAG_VALUE_OF = 1U << 20U,
    /* Full Text 3.0 (D214). FT_WORDS with "any word" or "all words". */
    SXML_AST_FLAG_SEPARATE_WORDS = 1U << 21U,
    /* FT_SCOPE with "different". */
    SXML_AST_FLAG_DIFFERENT = 1U << 22U,
    /* FT_WILDCARD_OPTION, FT_THESAURUS_OPTION, FT_STEM_OPTION or
       FT_STOP_WORD_OPTION with "no". */
    SXML_AST_FLAG_NO = 1U << 23U,
    /* FT_THESAURUS_OPTION or FT_STOP_WORD_OPTION with "default". */
    SXML_AST_FLAG_DEFAULT = 1U << 24U,
} sxml_ast_flag;

/* A name exactly as written; parts that were not written are empty. */
typedef struct sxml_ast_name {
    sxml_ast_text namespace_prefix;
    sxml_ast_text local_name;
    /* The text between the braces of "Q{...}". */
    sxml_ast_text namespace_uri;
} sxml_ast_name;

/* The two parts of a direct processing instruction constructor. */
typedef struct sxml_ast_processing_instruction {
    sxml_ast_text target;
    sxml_ast_text content;
} sxml_ast_processing_instruction;

/*
 * Data a leaf carries: text for literal and fixed-part kinds and the
 * XQuery text kinds, name for the name kinds, WILDCARD and QNAME,
 * processing_instruction for DIRECT_PROCESSING_INSTRUCTION_CONSTRUCTOR.
 * Zero for every other kind.
 */
typedef union sxml_ast_payload {
    sxml_ast_text text;
    sxml_ast_name name;
    sxml_ast_processing_instruction processing_instruction;
} sxml_ast_payload;

/* One node. Read through sxml_ast_get_node; written only by the builder. */
typedef struct sxml_ast_node {
    sxml_ast_kind kind;
    /* The field this node has in its parent; NONE for the root. */
    sxml_ast_field field;
    sxml_ast_detail detail;
    /* Bits of sxml_ast_flag. */
    uint32_t flags;
    sxml_ast_node_index parent;
    sxml_ast_node_index first_child;
    sxml_ast_node_index last_child;
    sxml_ast_node_index next_sibling;
    sxml_ast_source_span source_span;
    sxml_ast_payload payload;
} sxml_ast_node;

/* ------------------------------------------------------------------- AST */

typedef struct sxml_ast_configuration {
    /* Most nodes the AST may hold, not counting the reserved index 0. At
       most UINT32_MAX - 1. */
    size_t node_capacity;
    /* Most bytes of decoded text and names. At most UINT32_MAX. */
    size_t text_byte_capacity;
    /* The name the caller gives the expression text (glossary
       `source_name`); borrowed, must outlive the AST. May be empty. */
    sxml_byte_view source_name;
} sxml_ast_configuration;

/*
 * An AST. The layout is visible so the accessors below can be inline; only
 * the functions below write it, except root, which the front end sets when
 * it has built the tree.
 */
typedef struct sxml_ast {
    /* node_capacity + 1 entries; entry 0 is the "no node" sentinel, all
       zero. */
    sxml_ast_node *nodes;
    /* Nodes in use, including entry 0. */
    uint32_t node_count;
    uint32_t node_capacity;
    uint8_t *text_bytes;
    uint32_t text_byte_count;
    uint32_t text_byte_capacity;
    sxml_byte_view source_name;
    /* XPATH for an expression, SEQUENCE_TYPE for a sequence type. */
    sxml_ast_node_index root;
    /* The allocator of sxml_ast_create; the null allocator after
       sxml_ast_initialize. */
    sxml_allocator allocator;
} sxml_ast;

/*
 * The builder functions below are defined in src/xpath_ast.c, written with
 * the XPath front end (packet 1.16). They follow the lifecycle conventions
 * of sxml/memory.h: on failure outputs are untouched (D20).
 */

/*
 * Writes the memory requirements of an AST with configuration's capacities
 * (header, nodes and text store in one block) to *requirements. Returns
 * SXML_STATUS_INVALID_ARGUMENT for a capacity above its stated maximum and
 * SXML_STATUS_ARITHMETIC_OVERFLOW when the size does not fit size_t.
 */
SXML_NODISCARD sxml_status sxml_ast_memory_requirements(
    const sxml_ast_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places an empty AST (only node 0, no root) in memory and writes its
 * address to *ast. memory must stay valid, unmoved, until
 * sxml_ast_finalize. Returns the failures of sxml_ast_memory_requirements
 * and sxml_memory_view_check_requirements.
 */
SXML_NODISCARD sxml_status sxml_ast_initialize(
    sxml_memory_view memory, const sxml_ast_configuration *configuration,
    sxml_ast **ast);

/* Does nothing beyond invalidating ast; accepts NULL. */
void sxml_ast_finalize(sxml_ast *ast);

/*
 * Allocates one block through allocator and initializes an AST in it.
 * allocator is stored and must outlive the AST.
 */
SXML_NODISCARD sxml_status sxml_ast_create(
    sxml_allocator allocator, const sxml_ast_configuration *configuration,
    sxml_ast **ast);

/* Finalizes an AST made by sxml_ast_create and frees it; accepts NULL. */
void sxml_ast_destroy(sxml_ast *ast);

/* A node to add; it has no parent and no children yet. */
typedef struct sxml_ast_node_request {
    sxml_ast_kind kind;
    sxml_ast_detail detail;
    uint32_t flags;
    sxml_ast_source_span source_span;
    sxml_ast_payload payload;
} sxml_ast_node_request;

/*
 * Adds a node and writes its index to *node. Returns
 * SXML_STATUS_CAPACITY_EXCEEDED when node_capacity nodes exist and
 * SXML_STATUS_INVALID_ARGUMENT for kind NONE or a payload text outside the
 * text store.
 */
SXML_NODISCARD sxml_status sxml_ast_add_node(
    sxml_ast *ast, const sxml_ast_node_request *request,
    sxml_ast_node_index *node);

/* Makes child the last child of parent, in the role field. */
typedef struct sxml_ast_child_request {
    sxml_ast_node_index parent;
    sxml_ast_field field;
    sxml_ast_node_index child;
} sxml_ast_child_request;

/*
 * Appends request.child to the children of request.parent. O(1). Returns
 * SXML_STATUS_INVALID_ARGUMENT when either index is 0 or not in use, when
 * the child already has a parent or is the parent, or for field NONE.
 */
SXML_NODISCARD sxml_status sxml_ast_append_child(
    sxml_ast *ast, sxml_ast_child_request request);

/*
 * Copies bytes to the end of the text store. *text is in/out: when its
 * byte_count is 0 on entry, the bytes start a new text; otherwise *text
 * must be the text that ends the store, and the bytes extend it (how a
 * decoder appends the pieces between escapes). Returns
 * SXML_STATUS_CAPACITY_EXCEEDED when the store is full and
 * SXML_STATUS_INVALID_ARGUMENT when *text is neither empty nor at the end.
 */
SXML_NODISCARD sxml_status sxml_ast_append_text(sxml_ast *ast,
                                                sxml_byte_view bytes,
                                                sxml_ast_text *text);

/* Flags to add to a node already in the AST. */
typedef struct sxml_ast_flags_request {
    sxml_ast_node_index node;
    /* Bits of sxml_ast_flag, ORed into the node's flags. */
    uint32_t flags;
} sxml_ast_flags_request;

/*
 * Adds request.flags to the node, for marks that depend on the node's
 * siblings (BOUNDARY_WHITESPACE, set by a parser part's finish_node).
 * Returns SXML_STATUS_INVALID_ARGUMENT when the index is 0 or not in use.
 */
SXML_NODISCARD sxml_status sxml_ast_add_flags(sxml_ast *ast,
                                              sxml_ast_flags_request request);

/*
 * Moves every node's source span from a place in the expression text to
 * the place in the document the text came from (an XSLT attribute, D169):
 * a span on the text's first line moves origin.column_number columns
 * right, a span on a later line origin.line_number - 1 lines down, and
 * every byte offset by origin.byte_offset. A span without a line (0)
 * stays. The rule is that of an error raised inside the text, so a
 * dynamic error points where a static one does.
 */
void sxml_ast_move_source_spans(sxml_ast *ast, sxml_ast_source_span origin);

/* The place of span, a span inside a text whose origin is as for
   sxml_ast_move_source_spans, in the document the text came from. */
sxml_ast_source_span sxml_ast_moved_source_span(sxml_ast_source_span span,
                                                sxml_ast_source_span origin);

/* ------------------------------------------------------------- Accessors */

/*
 * The node at index, borrowed from ast; node 0 (all zero) for index 0 or an
 * index not in use, so a chain of reads through absent children ends in
 * kind NONE instead of out of bounds.
 */
static inline const sxml_ast_node *sxml_ast_get_node(
    const sxml_ast *ast, sxml_ast_node_index index) {
    if (index.value >= ast->node_count) {
        return &ast->nodes[0];
    }
    return &ast->nodes[index.value];
}

/*
 * The bytes of text, borrowed from ast; empty when text is outside the
 * store.
 */
static inline sxml_byte_view sxml_ast_get_text(const sxml_ast *ast,
                                               sxml_ast_text text) {
    if (text.byte_offset > ast->text_byte_count ||
        text.byte_count > ast->text_byte_count - text.byte_offset) {
        return (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    }
    return (sxml_byte_view){.bytes = ast->text_bytes + text.byte_offset,
                            .byte_count = text.byte_count};
}

/* A child to look for: the first child of parent with field. */
typedef struct sxml_ast_child_query {
    sxml_ast_node_index parent;
    sxml_ast_field field;
} sxml_ast_child_query;

/*
 * The first child of query.parent whose field is query.field; index 0 when
 * there is none (an absent optional field).
 */
static inline sxml_ast_node_index sxml_ast_find_child(
    const sxml_ast *ast, sxml_ast_child_query query) {
    sxml_ast_node_index child = sxml_ast_get_node(ast, query.parent)->first_child;
    while (child.value != 0) {
        const sxml_ast_node *child_node = sxml_ast_get_node(ast, child);
        if (child_node->field == query.field) {
            return child;
        }
        child = child_node->next_sibling;
    }
    return child;
}

/*
 * The next sibling of node that has the same field as node; index 0 after
 * the last. With sxml_ast_find_child, walks a repeated field:
 *
 *     for (sxml_ast_node_index argument = sxml_ast_find_child(ast,
 *              (sxml_ast_child_query){.parent = list,
 *                                     .field = SXML_AST_FIELD_ARGUMENT});
 *          argument.value != 0;
 *          argument = sxml_ast_find_next_with_same_field(ast, argument)) {
 *         ...
 *     }
 */
static inline sxml_ast_node_index sxml_ast_find_next_with_same_field(
    const sxml_ast *ast, sxml_ast_node_index node) {
    const sxml_ast_node *start = sxml_ast_get_node(ast, node);
    sxml_ast_node_index sibling = start->next_sibling;
    while (sibling.value != 0) {
        const sxml_ast_node *sibling_node = sxml_ast_get_node(ast, sibling);
        if (sibling_node->field == start->field) {
            return sibling;
        }
        sibling = sibling_node->next_sibling;
    }
    return sibling;
}

/*
 * The full source location of node, for an error record or diagnostic:
 * the AST's source name with the node's line, column and byte offset.
 * Borrows the source name; all zero for index 0.
 */
static inline sxml_source_location sxml_ast_get_source_location(
    const sxml_ast *ast, sxml_ast_node_index node) {
    const sxml_ast_source_span span = sxml_ast_get_node(ast, node)->source_span;
    return (sxml_source_location){.source_name = ast->source_name,
                                  .line_number = span.line_number,
                                  .column_number = span.column_number,
                                  .byte_offset = span.byte_offset};
}

/* ------------------------------------------------------------ Front ends */

/* An expression or sequence type to parse. */
typedef struct sxml_ast_build_request {
    /* UTF-8; at most UINT32_MAX bytes. Borrowed for the call. */
    sxml_byte_view expression_text;
    /* Becomes the AST's source name and the error record's; borrowed,
       must outlive the AST. May be empty. */
    sxml_byte_view source_name;
    /* Memory for the AST, which is made with sxml_ast_create. The
       tree-sitter runtime allocates the concrete syntax tree with malloc
       during the call (D12). */
    sxml_allocator allocator;
    /* Deepest nesting of AST nodes accepted (glossary `expression_depth`),
       so later recursive walks are bounded; 0 means no bound. Exceeding it
       gives SXML_STATUS_LIMIT_EXCEEDED. */
    uint32_t maximum_expression_depth;
    /* Receives XPST0003 with the location of the first ERROR or MISSING
       node or failed check; may be NULL. */
    sxml_error_record *error_record;
    /* XQuery text read by XML 1.1's rules (glossary
       query_text_xml_version; D275); parser parts read it through
       sxml_ast_front_end_reads_xml_1_1. */
    bool reads_xml_1_1;
} sxml_ast_build_request;

/*
 * Parses request->expression_text with grammars/xpath and writes an AST,
 * owned (release with sxml_ast_destroy), whose root is an XPATH node, to
 * *ast. Defined in src/xpath_parse.c (packet 1.16).
 *
 * Returns SXML_STATUS_STATIC_ERROR (XPST0003) for text that is not an
 * XPath 4.0 expression, SXML_STATUS_INVALID_UTF8,
 * SXML_STATUS_LIMIT_EXCEEDED, and the failures of the allocator.
 */
SXML_NODISCARD sxml_status sxml_xpath_build_ast(
    const sxml_ast_build_request *request, sxml_ast **ast);

/*
 * As sxml_xpath_build_ast for text that is one SequenceType (XPath 4.0,
 * section 3.1), such as "fn(item()) as xs:boolean?"; the root is a
 * SEQUENCE_TYPE node. Used for the parameter and result types of the
 * native function table (src/function_library_internal.h). Defined in
 * src/xpath_parse.c (packet 1.16).
 */
SXML_NODISCARD sxml_status sxml_xpath_build_sequence_type_ast(
    const sxml_ast_build_request *request, sxml_ast **ast);

/*
 * Parses request->expression_text, an XQuery 4.0 module, with
 * grammars/xquery and writes an AST, owned (release with sxml_ast_destroy),
 * whose root is a MODULE node, to *ast. Defined in src/xquery_parse.c
 * (packet 3.2) on top of sxml_ast_build_with_parser_part.
 *
 * Returns SXML_STATUS_STATIC_ERROR for text that is not an XQuery 4.0
 * module (XPST0003, or the XQuery code a check of the front end raises:
 * XQST0118, XQST0040, XQST0090, ...), SXML_STATUS_INVALID_UTF8,
 * SXML_STATUS_LIMIT_EXCEEDED, and the failures of the allocator.
 */
SXML_NODISCARD sxml_status sxml_xquery_build_ast(
    const sxml_ast_build_request *request, sxml_ast **ast);

/*
 * Parses request->expression_text, an XSLT 4.0 pattern (XSLT 4.0, section
 * 6.3), with grammars/xslt_pattern and writes an AST, owned (release with
 * sxml_ast_destroy), whose root is a PATTERN node, to *ast. Defined in
 * src/xslt_pattern.c (packet 4.4, D118) on top of
 * sxml_ast_build_with_parser_part; src/xslt_pattern_internal.h turns
 * patterns into match expressions.
 *
 * Returns SXML_STATUS_STATIC_ERROR for text that is not a pattern
 * (XTSE0340, 6.3.1), SXML_STATUS_INVALID_UTF8, SXML_STATUS_LIMIT_EXCEEDED,
 * and the failures of the allocator.
 */
SXML_NODISCARD sxml_status sxml_xslt_pattern_build_ast(
    const sxml_ast_build_request *request, sxml_ast **ast);

/* ---------------------------------------------------------- Parser parts */

/*
 * The parser part seam (D111). The XPath front end in src/xpath_parse.c is
 * one walker for every grammar that inherits from grammars/xpath: it maps
 * node types and fields through its own tables and those of a parser
 * part, absorbs tokens into detail and flags, decodes payloads, makes the
 * A.3.2 and lookup key checks, bounds the depth and fills the error
 * record, all without recursion (D59, D71). A parser part is what one
 * derived grammar adds (XQuery: packet 3.2, src/xquery_parse.c; XSLT
 * patterns: Phase 4): the grammar, the node types and fields that are not
 * XPath's, and three hooks the walker calls, in the way lowering parts add
 * AST kinds to the lowerer (D103). Unlike lowering parts, parser parts are
 * not registered by glob: each entry point (sxml_xquery_build_ast, ...)
 * passes its own part, because the grammar is chosen by the entry point.
 *
 * How the walker uses a part, in source order for each concrete syntax
 * tree node:
 * 1. The node's AST kind is the XPath table's, else the part's
 *    node_types; NONE (no AST node, the node's tokens are absorbed by its
 *    parent) for anonymous nodes and for named nodes in neither table.
 * 2. The node's field is the XPath table's, else the part's fields; a
 *    named child without a field takes the part's unnamed_children entry
 *    for its parent's kind; otherwise it is XPST0003.
 * 3. Describing the node: describe_payload is called first and may fill
 *    the payload (and detail and flags); when it does not, the XPath
 *    decoders run (literals, names, wildcards; QNAME like a name). Then
 *    for every child that makes no AST node, apply_token is called first;
 *    when it does not claim the token, the XPath rules apply.
 * 4. The node is added and appended to its parent. finish_node is called
 *    when the node is complete: at once for a node without concrete
 *    syntax tree children, else after its last child is complete. The
 *    AST may then be read from the node down and from the node to the
 *    root, and flags added with sxml_ast_add_flags.
 * A hook may be NULL. A hook that fails stops the walk with its status;
 * a syntax or static error is raised with sxml_ast_front_end_raise, so the
 * walker fills the error record.
 *
 * A part may name an extension: the part of a language extension that
 * several grammars share (Full Text's, src/full_text_parse.c, for
 * grammars/xquery and grammars/xpath_full_text; D235). Steps 1 and 2 look
 * in the extension's tables after the part's, and step 3 offers a token
 * to the extension's apply_token after the part's; the extension's other
 * members are unused, and it has no extension of its own.
 */

/* A grammar's language; declared by the generated parser of each grammar. */
struct TSLanguage; // NOLINT(readability-identifier-naming)

/* The state of one call of the front end; opaque to parts. */
typedef struct sxml_ast_front_end sxml_ast_front_end;

/* A node type of a part's grammar and the AST kind it becomes. */
typedef struct sxml_ast_node_type_entry {
    sxml_byte_view node_type;
    sxml_ast_kind kind;
} sxml_ast_node_type_entry;

/* Entries sorted by node_type in byte order (as strcmp orders them). */
typedef struct sxml_ast_node_type_view {
    const sxml_ast_node_type_entry *entries;
    size_t entry_count;
} sxml_ast_node_type_view;

/* A field name of a part's grammar and the AST field it becomes. */
typedef struct sxml_ast_field_entry {
    sxml_byte_view field_name;
    sxml_ast_field field;
} sxml_ast_field_entry;

/* Entries sorted by field_name in byte order. */
typedef struct sxml_ast_field_view {
    const sxml_ast_field_entry *entries;
    size_t entry_count;
} sxml_ast_field_view;

/* The field a named child without a grammar field takes in a parent of
   parent_kind (MODULE's module, D111). */
typedef struct sxml_ast_unnamed_child_entry {
    sxml_ast_kind parent_kind;
    sxml_ast_field field;
} sxml_ast_unnamed_child_entry;

typedef struct sxml_ast_unnamed_child_view {
    const sxml_ast_unnamed_child_entry *entries;
    size_t entry_count;
} sxml_ast_unnamed_child_view;

/* A child that makes no AST node, as its parent is described. */
typedef struct sxml_ast_parser_token {
    /* The field the token has in its parent; empty when it has none. */
    sxml_byte_view field_name;
    /* The token's source text (for an absorbed named node such as
       allowing_empty, all of its text). */
    sxml_byte_view written;
} sxml_ast_parser_token;

/* A node about to be added, as described to describe_payload. */
typedef struct sxml_ast_parser_node {
    sxml_ast_front_end *front_end;
    /* The node's source text, borrowed from the expression text. Its
       offset in the text is request->source_span.byte_offset. */
    sxml_byte_view written;
} sxml_ast_parser_node;

/*
 * A part's hooks (C_GUIDELINES: at most three parameters).
 * - apply_token: adds what token says to request->detail and
 *   request->flags (request->kind is the parent's kind); returns true when
 *   it did, so the XPath rules are skipped.
 * - describe_payload: may store the payload of request through
 *   sxml_ast_append_text on sxml_ast_front_end_get_ast(node.front_end),
 *   and adjust detail and flags; sets outcome->is_described when it did,
 *   so the XPath decoders are skipped. Called for every node of a kind.
 * - finish_node: checks or completes node (see above).
 */
typedef bool (*sxml_ast_apply_token_function)(sxml_ast_parser_token token,
                                              sxml_ast_node_request *request);
/* What describe_payload did. */
typedef struct sxml_ast_payload_outcome {
    /* Set when the hook described the node. */
    bool is_described;
} sxml_ast_payload_outcome;

typedef sxml_status (*sxml_ast_describe_payload_function)(
    sxml_ast_parser_node node, sxml_ast_node_request *request,
    sxml_ast_payload_outcome *outcome);
typedef sxml_status (*sxml_ast_finish_node_function)(
    sxml_ast_front_end *front_end, sxml_ast_node_index node);

typedef struct sxml_ast_parser_part {
    /* The generated parser's language function (tree_sitter_xquery). */
    const struct TSLanguage *(*language)(void);
    /* The node type the start rule makes ("module"); any other root is
       XPST0003. */
    sxml_byte_view root_node_type;
    sxml_ast_node_type_view node_types;
    sxml_ast_field_view fields;
    sxml_ast_unnamed_child_view unnamed_children;
    /* Descriptions for XPST0003 from an ERROR node and from a MISSING
       node; empty means the XPath front end's. */
    sxml_byte_view syntax_error_description;
    sxml_byte_view missing_token_description;
    sxml_ast_apply_token_function apply_token;
    sxml_ast_describe_payload_function describe_payload;
    sxml_ast_finish_node_function finish_node;
    /* The extension whose tables and apply_token come after this part's
       (see above); NULL for none. */
    const struct sxml_ast_parser_part *extension;
} sxml_ast_parser_part;

/*
 * As sxml_xpath_build_ast with part's grammar, tables and hooks; the root
 * is the AST node of part->root_node_type. Defined in src/xpath_parse.c.
 * Returns the failures of sxml_xpath_build_ast, SXML_STATUS_UNSUPPORTED_
 * FEATURE when part's language does not load, and those of the hooks.
 */
SXML_NODISCARD sxml_status sxml_ast_build_with_parser_part(
    const sxml_ast_build_request *request, const sxml_ast_parser_part *part,
    sxml_ast **ast);

/* The AST being built, for hooks; valid during the call only. */
sxml_ast *sxml_ast_front_end_get_ast(sxml_ast_front_end *front_end);

/* The request's expression text, for hooks (spans are offsets into it). */
sxml_byte_view
sxml_ast_front_end_get_expression_text(const sxml_ast_front_end *front_end);

/* The request's reads_xml_1_1, for hooks. */
bool sxml_ast_front_end_reads_xml_1_1(const sxml_ast_front_end *front_end);

/* A static error a hook raises. */
typedef struct sxml_ast_static_error {
    /* The local name of an err: code, such as "XQST0118"; static. */
    sxml_byte_view error_code;
    /* A static description (D21). */
    sxml_byte_view description;
    /* Where, as an offset into the expression text. */
    uint32_t byte_offset;
} sxml_ast_static_error;

/*
 * Records error for the error record of the request (the err namespace,
 * the location computed from byte_offset) and returns
 * SXML_STATUS_STATIC_ERROR, for the hook to return; the walk stops
 * there.
 */
SXML_NODISCARD sxml_status sxml_ast_front_end_raise(
    sxml_ast_front_end *front_end, sxml_ast_static_error error);
