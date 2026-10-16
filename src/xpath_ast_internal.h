#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/types.h"
#include "sxml/view.h"

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
 *   sxml_ast_field; existing values never change. XQuery adds 66 node types
 *   (prolog and its declarations, the FLWOR clauses where, order by, group
 *   by, count, window, while and trace, direct and computed constructors,
 *   string constructors, switch, typeswitch, try/catch, validate,
 *   extension expressions); XSLT patterns add 20 `..._pattern` types.
 * - Four XPath node types gain fields in XQuery: `annotation` on
 *   inline_function_expression, any_function_type and typed_function_type
 *   (children of a new kind ANNOTATION), and `allowing_empty` on
 *   for_item_binding (a new flag). Children are found by field, never by
 *   position, so code written for the XPath shape keeps working.
 * - XQuery FLWOR clauses are more children with field `clause` of the same
 *   flwor_expression node.
 * - Direct constructors become element, attribute and text nodes whose text
 *   payload is the decoded content (entity and character references
 *   replaced, boundary whitespace handled by the front end); the end tag
 *   name check (XQST0118) is made while building. Each decoded text is
 *   still no longer than its source, so the capacity rule holds.
 * - A query module is one AST whose root is `module`; each imported
 *   library module is a separate AST. An XSLT stylesheet is an XML
 *   document read through the XML parser; each XPath expression, attribute
 *   value template part, sequence type and pattern in it is parsed to its
 *   own AST, with source spans that point into the stylesheet.
 * - Entry points sxml_xquery_build_ast and sxml_xslt_pattern_build_ast take
 *   the request below and produce the same sxml_ast.
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
       sequence: Expression. "for $x at $i in E". */
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
       LET_ARRAY_BINDING or LET_MAP_BINDING. */
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
    /* parameter*: VARIABLE_NAME_AND_TYPE; return_type?: TYPE_DECLARATION;
       body: ENCLOSED_EXPRESSION. Flag HAS_SIGNATURE when "(...)" was
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
    /* name?: QNAME_LITERAL or name_expression?: Expression (one of them);
       content: ENCLOSED_EXPRESSION. */
    SXML_AST_KIND_COMPUTED_ELEMENT_CONSTRUCTOR,
    SXML_AST_KIND_COMPUTED_ATTRIBUTE_CONSTRUCTOR,
    /* name?: MARKED_NCNAME or name_expression?: Expression (one of them);
       content: ENCLOSED_EXPRESSION. */
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
    /* No fields: function(*) or fn(*). */
    SXML_AST_KIND_ANY_FUNCTION_TYPE,
    /* parameter*: TYPED_FUNCTION_PARAMETER; return_type: SEQUENCE_TYPE.
       function(T) as U. */
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
 * The token a node records beyond its children. Which member is meaningful
 * is given by the kind (see sxml_ast_kind); every other kind leaves it
 * zero (operator SXML_AST_OPERATOR_NONE).
 */
typedef union sxml_ast_detail {
    sxml_ast_operator operator;
    sxml_ast_axis axis;
    sxml_occurrence occurrence;
    sxml_name_test_kind name_test_kind;
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
} sxml_ast_flag;

/* A name exactly as written; parts that were not written are empty. */
typedef struct sxml_ast_name {
    sxml_ast_text namespace_prefix;
    sxml_ast_text local_name;
    /* The text between the braces of "Q{...}". */
    sxml_ast_text namespace_uri;
} sxml_ast_name;

/*
 * Data a leaf carries: text for literal and fixed-part kinds, name for the
 * name kinds and WILDCARD. Zero for every other kind.
 */
typedef union sxml_ast_payload {
    sxml_ast_text text;
    sxml_ast_name name;
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
